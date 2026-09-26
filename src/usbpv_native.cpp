#define NOMINMAX
#include "usbpv_native.hpp"
#include "usbpv_protocol.hpp"
#include "usbpv_fpga.hpp"
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace usbpv::native {
namespace {
using Clock = std::chrono::steady_clock;
constexpr GUID interface_guid = {0x1d4b2365, 0x4749, 0x48ea,
    {0xb3, 0x8a, 0x7c, 0x6f, 0xdd, 0xdd, 0x7e, 0x26}};
constexpr UCHAR input_pipe = 0x81;
constexpr UCHAR output_pipe = 0x01;
constexpr std::size_t capture_read_bytes = 65536;
constexpr std::size_t capture_read_count = 8;
static_assert(capture_read_bytes * capture_read_count == 512 * 1024);
thread_local std::string last_error;
thread_local std::string device_list;

std::string windows_error(const char* action) {
  return std::string(action) + " (Windows error " + std::to_string(GetLastError()) + ")";
}
void check(BOOL ok, const char* action) {
  if (!ok) throw std::runtime_error(windows_error(action));
}
std::uint64_t wall_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::vector<std::string> device_paths() {
  const HDEVINFO set = SetupDiGetClassDevsA(&interface_guid, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) throw std::runtime_error(windows_error("enumerate WinUSB"));
  struct Guard { HDEVINFO set; ~Guard() { SetupDiDestroyDeviceInfoList(set); } } guard{set};
  std::vector<std::string> result;
  for (DWORD i = 0;; ++i) {
    SP_DEVICE_INTERFACE_DATA item{};
    item.cbSize = sizeof(item);
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &interface_guid, i, &item)) {
      if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
      throw std::runtime_error(windows_error("enumerate USB interface"));
    }
    DWORD bytes = 0;
    SetupDiGetDeviceInterfaceDetailA(set, &item, nullptr, 0, &bytes, nullptr);
    if (!bytes) throw std::runtime_error(windows_error("size USB interface path"));
    std::vector<unsigned char> buffer(bytes);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buffer.data());
    detail->cbSize = sizeof(*detail);
    check(SetupDiGetDeviceInterfaceDetailA(set, &item, detail, bytes, nullptr, nullptr),
          "read USB interface path");
    result.emplace_back(detail->DevicePath);
  }
  return result;
}

struct Device {
  HANDLE file = INVALID_HANDLE_VALUE;
  WINUSB_INTERFACE_HANDLE usb = nullptr;
  int monitor_speed = 0;
  std::string serial;
  std::thread worker;
  BatchService batch_service = nullptr;
  std::atomic<bool> stop{false};
  std::atomic<bool> broken{false};
  std::mutex mutex;
  std::condition_variable ready;
  bool started = false;
  std::string error;

  ~Device() {
    stop.store(true);
    if (worker.joinable()) worker.join();
    if (usb) WinUsb_Free(usb);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  }
  void fail(const std::string& message) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (error.empty()) error = message;
    }
    broken.store(true, std::memory_order_release);
    ready.notify_all();
  }
  std::string usb_string(UCHAR index) {
    std::array<UCHAR, 256> bytes{};
    ULONG n = 0;
    check(WinUsb_GetDescriptor(usb, USB_STRING_DESCRIPTOR_TYPE, index, 0x409,
                              bytes.data(), static_cast<ULONG>(bytes.size()), &n),
          "read USB identity string");
    if (n < 2 || bytes[0] > n || (bytes[0] & 1))
      throw std::runtime_error("invalid USB identity string");
    std::string value;
    for (unsigned i = 2; i < bytes[0]; i += 2) {
      if (bytes[i + 1] || bytes[i] < 0x20 || bytes[i] > 0x7e)
        throw std::runtime_error("non-ASCII USB identity string");
      value += static_cast<char>(bytes[i]);
    }
    return value;
  }
  void connect(const std::string& path) {
    // Exclusive access also prevents two independent native captures from
    // interleaving register commands and consuming each other's responses.
    file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error(windows_error("open sniffer"));
    check(WinUsb_Initialize(file, &usb), "initialize WinUSB");
    USB_DEVICE_DESCRIPTOR desc{};
    ULONG n = 0;
    check(WinUsb_GetDescriptor(usb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
              reinterpret_cast<PUCHAR>(&desc), sizeof(desc), &n), "read USB descriptor");
    if (n != sizeof(desc) || desc.idVendor != 0x16c0 || desc.idProduct != 0x05dc ||
        (desc.bcdDevice & 0xff00) != 0x1700 || usb_string(desc.iManufacturer) != "tusb.org")
      throw std::runtime_error("native capture requires a tusb.org CH56x USBPV sniffer");
    serial = usb_string(desc.iSerialNumber);
    USB_INTERFACE_DESCRIPTOR iface{};
    check(WinUsb_QueryInterfaceSettings(usb, 0, &iface), "query USB interface");
    bool in = false, out = false;
    for (UCHAR i = 0; i < iface.bNumEndpoints; ++i) {
      WINUSB_PIPE_INFORMATION pipe{};
      check(WinUsb_QueryPipe(usb, 0, i, &pipe), "query USB pipe");
      if (pipe.PipeType != UsbdPipeTypeBulk) continue;
      if (pipe.PipeId == input_pipe) {
        in = true;
        monitor_speed = pipe.MaximumPacketSize > 512 ? 1 : 0;
      }
      if (pipe.PipeId == output_pipe) out = true;
    }
    if (!in || !out) throw std::runtime_error("CH56x bulk endpoints missing");
    ULONG timeout = 1000;
    check(WinUsb_SetPipePolicy(usb, output_pipe, PIPE_TRANSFER_TIMEOUT,
                              sizeof(timeout), &timeout), "set command timeout");
    check(WinUsb_SetPipePolicy(usb, input_pipe, PIPE_TRANSFER_TIMEOUT,
                              sizeof(timeout), &timeout), "set register timeout");
  }
  void control(UCHAR request, USHORT index, UCHAR* data, USHORT size) {
    WINUSB_SETUP_PACKET setup{};
    setup.RequestType = size ? 0xc0 : 0x40;
    setup.Request = request;
    setup.Index = index;
    setup.Length = size;
    ULONG transferred = 0;
    check(WinUsb_ControlTransfer(usb, setup, data, size, &transferred, nullptr),
          "CH56x control transfer");
    if (transferred != size) throw std::runtime_error("short CH56x control transfer");
  }
  void write(const UCHAR* data, ULONG size) {
    ULONG n = 0;
    check(WinUsb_WritePipe(usb, output_pipe, const_cast<PUCHAR>(data), size, &n, nullptr),
          "CH56x bulk write");
    if (n != size) throw std::runtime_error("short CH56x bulk write");
  }
  UCHAR reg(UCHAR address, UCHAR value, bool read = false) {
    const auto cmd = register_command(address, value, read);
    write(cmd.data(), static_cast<ULONG>(cmd.size()));
    std::array<UCHAR, 4> response{};
    ULONG received = 0;
    while (received < response.size()) {
      ULONG n = 0;
      check(WinUsb_ReadPipe(usb, input_pipe, response.data() + received,
                            static_cast<ULONG>(response.size()) - received, &n, nullptr),
            "read register response");
      if (!n) throw std::runtime_error("empty register response");
      received += n;
    }
    if (response[0] != 0x55 || response[1] != cmd[1] ||
        static_cast<UCHAR>(response[0] + response[1] + response[2]) != response[3] ||
        (!read && response[2] != value))
      throw std::runtime_error("invalid register response at " + std::to_string(address));
    return response[2];
  }
  void configure(const unsigned char* options) {
    // Same volatile FPGA image and initialization as the supplied SDK. No
    // application/bootloader flash programming commands are used here.
    std::array<UCHAR, 4> version{};
    control(0x75, 1, version.data(), 4);
    control(0x73, 0, nullptr, 0);
    check(WinUsb_FlushPipe(usb, input_pipe), "flush old capture bytes");
    for (std::size_t offset = 0; offset < sizeof(kCh56xFpga); offset += 4096) {
      write(kCh56xFpga + offset, static_cast<ULONG>(
          std::min<std::size_t>(4096, sizeof(kCh56xFpga) - offset)));
    }
    bool configured = false;
    for (int i = 0; i < 100; ++i) {
      UCHAR status[2]{};
      control(0x75, 0, status, 2);
      if ((status[0] & 0x0f) == 3) { configured = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!configured) throw std::runtime_error("FPGA configuration did not complete");
    control(0x74, 0, nullptr, 0);
    reg(0x46, 0x10); reg(0x47, 0x17); reg(0x48, 0x11); reg(0x4a, 0x28);
    const UCHAR revision = reg(0x64, 0, true);
    reg(1, revision & 1 ? 0x50 : 0xa0);
    reg(0, 0x30); reg(0x49, 0x40);
    reg(8, 0x0c | options[0]);
    reg(0x1f, static_cast<UCHAR>(~options[1]));
    std::array<UCHAR, 8> pairs{};
    std::copy_n(options + 3, pairs.size(), pairs.begin());
    const auto filters = filter_registers(pairs, options[2] != 0);
    for (std::size_t i = 0; i < filters.size(); ++i)
      reg(static_cast<UCHAR>(0x20 + i), filters[i]);
  }

  void capture(void* context, pfn_packet_handler callback, BatchEnd batch_end) noexcept {
    // Stable addresses for OVERLAPPED and buffers until every completion has
    // been collected. This thread alone owns all transfers and the parser.
    struct Read {
      std::array<UCHAR, capture_read_bytes> bytes{};
      OVERLAPPED ov{};
      bool pending = false;
    };
    std::array<Read, capture_read_count> reads{};
    bool command_started = false;
    bool stop_sent = false;
    bool ready_reported = false;
    bool pending_batch = false;
    try {
      ULONG timeout = 0; // Pending reads survive idle buses; we poll events.
      check(WinUsb_SetPipePolicy(usb, input_pipe, PIPE_TRANSFER_TIMEOUT,
                                sizeof(timeout), &timeout), "set stream policy");
      auto submit = [&](Read& read) {
        ResetEvent(read.ov.hEvent);
        const BOOL ok = WinUsb_ReadPipe(usb, input_pipe, read.bytes.data(),
            static_cast<ULONG>(read.bytes.size()), nullptr, &read.ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING)
          throw std::runtime_error(windows_error("submit capture read"));
        read.pending = true;
      };
      for (auto& read : reads) {
        read.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!read.ov.hEvent) throw std::runtime_error(windows_error("create read event"));
        submit(read);
      }
      ProtocolParser parser(context, callback);
      const auto start_cmd = register_command(1, 1);
      command_started = true;
      write(start_cmd.data(), 4);
      const auto start_deadline = Clock::now() + std::chrono::seconds(2);
      auto stop_deadline = Clock::time_point::max();
      std::size_t index = 0;
      while (true) {
        if (stop.load(std::memory_order_acquire) && !stop_sent) {
          const auto cmd = register_command(1, 0);
          write(cmd.data(), 4);
          stop_sent = true;
          stop_deadline = Clock::now() + std::chrono::seconds(2);
        }
        if (!parser.started() && Clock::now() > start_deadline)
          throw std::runtime_error("timed out waiting for capture start marker");
        if (stop_sent && Clock::now() > stop_deadline)
          throw std::runtime_error("timed out waiting for capture stop marker");
        auto& read = reads[index];
        const DWORD result = WaitForSingleObject(read.ov.hEvent, pending_batch ? 1 : 20);
        if (result == WAIT_TIMEOUT) {
          if (batch_service && pending_batch) pending_batch = batch_service(context, true);
          continue;
        }
        if (result != WAIT_OBJECT_0) throw std::runtime_error(windows_error("wait for capture read"));
        ULONG n = 0;
        const BOOL ok = WinUsb_GetOverlappedResult(usb, &read.ov, &n, FALSE);
        read.pending = false;
        check(ok, "complete capture read");
        const bool parsed = parser.feed(read.bytes.data(), n, wall_ns());
        if (batch_end) batch_end(context);
        if (batch_service) pending_batch = batch_service(context, !parsed || parser.stopped());
        if (!parsed)
          throw std::runtime_error(parser.error());
        if (!ready_reported && parser.started()) {
          std::lock_guard<std::mutex> lock(mutex);
          started = true;
          ready_reported = true;
          ready.notify_all();
        }
        if (parser.stopped()) {
          if (!stop_sent) throw std::runtime_error("device stopped capture unexpectedly");
          break;
        }
        submit(read);
        index = (index + 1) % reads.size();
      }
    } catch (const std::exception& ex) {
      fail(ex.what());
    } catch (...) {
      fail("unexpected native capture exception");
    }
    // Also publish a partial callback batch if parsing/setup threw. The normal
    // per-transfer flush makes this a no-op on a clean stop.
    if (batch_end) {
      try { batch_end(context); }
      catch (const std::exception& ex) { fail(ex.what()); }
      catch (...) { fail("unexpected native batch flush exception"); }
    }
    if (batch_service) {
      try { batch_service(context, true); }
      catch (const std::exception& ex) { fail(ex.what()); }
      catch (...) { fail("unexpected native batch service exception"); }
    }
    if (command_started && !stop_sent) {
      try { const auto cmd = register_command(1, 0); write(cmd.data(), 4); }
      catch (const std::exception& ex) { fail(ex.what()); }
    }
    // Cancellation is a request, not completion. Always reap before freeing
    // buffers/events/WinUSB, including partially submitted startup failures.
    for (auto& read : reads) if (read.pending) CancelIoEx(file, &read.ov);
    for (auto& read : reads) {
      if (read.pending) {
        ULONG n = 0;
        WinUsb_GetOverlappedResult(usb, &read.ov, &n, TRUE);
      }
      if (read.ov.hEvent) CloseHandle(read.ov.hEvent);
    }
  }
};
}  // namespace

const char* UPV_CALL list_devices() {
  device_list.clear();
  last_error.clear();
  try {
    for (const auto& path : device_paths()) {
      Device device;
      device.connect(path);
      if (!device_list.empty()) device_list += ',';
      device_list += device.serial;
    }
  } catch (const std::exception& ex) {
    device_list.clear();
    last_error = ex.what();
    return nullptr;
  }
  return device_list.c_str();
}

UPV_HANDLE UPV_CALL open_device(const char* options, int size, void* context,
                                pfn_packet_handler callback) {
  return open_device_batched(options, size, context, callback, nullptr);
}

UPV_HANDLE open_device_batched(const char* options, int size, void* context,
                                pfn_packet_handler callback, BatchEnd batch_end,
                                BatchService batch_service) {
  last_error.clear();
  try {
    if (!options || size <= 0 || !callback) throw std::runtime_error("invalid native capture options");
    const auto* end = static_cast<const char*>(std::memchr(options, 0, size));
    if (!end || options + size - end != 12 || static_cast<unsigned char>(end[1]) > 2)
      throw std::runtime_error("native capture needs explicit speed and four filter pairs");
    const std::string serial(options, end);
    for (const auto& path : device_paths()) {
      auto device = std::make_unique<Device>();
      device->connect(path);
      if (device->serial != serial) continue;
      device->configure(reinterpret_cast<const unsigned char*>(end + 1));
      device->batch_service = batch_service;
      device->worker = std::thread(&Device::capture, device.get(), context, callback, batch_end);
      std::unique_lock<std::mutex> lock(device->mutex);
      const bool ready = device->ready.wait_for(lock, std::chrono::seconds(4), [&] {
        return device->started || device->broken.load(std::memory_order_acquire);
      });
      const std::string error = device->error;
      lock.unlock();
      if (!ready || !error.empty()) throw std::runtime_error(
          error.empty() ? "native capture startup timed out" : error);
      return device.release();
    }
    throw std::runtime_error("requested CH56x sniffer is not connected");
  } catch (const std::exception& ex) {
    last_error = ex.what();
    return nullptr;
  }
}

int UPV_CALL close_device(UPV_HANDLE handle) {
  std::unique_ptr<Device> device(static_cast<Device*>(handle));
  device->stop.store(true, std::memory_order_release);
  if (device->worker.joinable()) device->worker.join();
  last_error = device->error;
  return last_error.empty() ? 0 : -1;
}
int UPV_CALL get_monitor_speed(UPV_HANDLE handle) {
  return static_cast<Device*>(handle)->monitor_speed;
}
int UPV_CALL get_last_error() { return last_error.empty() ? 0 : -1; }
const char* UPV_CALL get_error_string(int) { return last_error.c_str(); }
bool failed(UPV_HANDLE handle) {
  return static_cast<Device*>(handle)->broken.load(std::memory_order_acquire);
}
}  // namespace usbpv::native
