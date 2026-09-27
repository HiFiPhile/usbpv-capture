#include "usbpv_native.hpp"
#include "usbpv_protocol.hpp"
#include "usbpv_device_scan.hpp"
#include "usbpv_fpga.hpp"
#include <libusb.h>
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
constexpr unsigned char input_pipe = 0x81;
constexpr unsigned char output_pipe = 0x01;
// Linux usbfs measurements favor one 16 KiB URB per read, with enough queued
// reads to retain a 512 KiB reserve. Windows keeps its independent 8 x 64 KiB ring.
constexpr std::size_t capture_read_bytes = 16384;
constexpr std::size_t capture_read_count = 32;
static_assert(capture_read_bytes * capture_read_count == 512 * 1024);
thread_local std::string last_error;
thread_local std::string device_list;

void check(int rc, const char* action) {
  if (rc < 0) throw std::runtime_error(std::string(action) + ": " + libusb_error_name(rc));
}
std::uint64_t wall_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
struct UsbContext {
  libusb_context* value = nullptr;
  UsbContext() { check(libusb_init(&value), "initialize libusb"); }
  ~UsbContext() { libusb_exit(value); }
};
struct UsbList {
  libusb_device** value = nullptr;
  explicit UsbList(libusb_context* context) {
    const auto n = libusb_get_device_list(context, &value);
    if (n < 0) check(static_cast<int>(n), "enumerate USB devices");
  }
  ~UsbList() { libusb_free_device_list(value, 1); }
};
std::string device_path(libusb_device* device) {
  return std::to_string(libusb_get_bus_number(device)) + ":" +
         std::to_string(libusb_get_device_address(device));
}
bool matches(const libusb_device_descriptor& d) {
  return d.idVendor == 0x16c0 && d.idProduct == 0x05dc && (d.bcdDevice & 0xff00) == 0x1700;
}
std::vector<std::string> device_paths() {
  UsbContext context;
  UsbList devices(context.value);
  std::vector<std::string> paths;
  for (auto** p = devices.value; *p; ++p) {
    libusb_device_descriptor d{};
    if (libusb_get_device_descriptor(*p, &d) == 0 && matches(d))
      paths.push_back(device_path(*p));
  }
  return paths;
}

struct Device {
  // Each capture has a private event loop. No other thread dispatches callbacks.
  UsbContext context;
  libusb_device_handle* usb = nullptr;
  int claimed_interface = -1;
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
    if (claimed_interface >= 0) libusb_release_interface(usb, claimed_interface);
    if (usb) libusb_close(usb);
  }
  void fail(const std::string& message) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (error.empty()) error = message;
    }
    broken.store(true, std::memory_order_release);
    ready.notify_all();
  }
  std::string usb_string(unsigned char index) {
    std::array<unsigned char, 256> bytes{};
    const int n = libusb_get_string_descriptor(usb, index, 0x409, bytes.data(), bytes.size());
    check(n, "read USB identity string");
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
    UsbList devices(context.value);
    for (auto** p = devices.value; *p; ++p) {
      if (device_path(*p) != path) continue;
      libusb_device_descriptor d{};
      check(libusb_get_device_descriptor(*p, &d), "read USB descriptor");
      if (!matches(d)) throw std::runtime_error("native capture requires a CH56x USBPV sniffer");
      check(libusb_open(*p, &usb), "open sniffer (check USB device permissions)");
      libusb_config_descriptor* raw = nullptr;
      check(libusb_get_active_config_descriptor(*p, &raw), "read USB configuration");
      std::unique_ptr<libusb_config_descriptor, decltype(&libusb_free_config_descriptor)>
          config(raw, libusb_free_config_descriptor);
      for (unsigned i = 0; i < config->bNumInterfaces; ++i) {
        const auto& iface = config->interface[i];
        for (int j = 0; j < iface.num_altsetting; ++j) {
          const auto& alt = iface.altsetting[j];
          bool in = false, out = false;
          for (unsigned k = 0; k < alt.bNumEndpoints; ++k) {
            const auto& ep = alt.endpoint[k];
            if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep.bEndpointAddress == input_pipe) {
              in = true;
              monitor_speed = ep.wMaxPacketSize > 512 ? 1 : 0;
            }
            if (ep.bEndpointAddress == output_pipe) out = true;
          }
          if (!in || !out) continue;
          // Kernel interface claiming supplies exclusive ownership across processes.
          // Do not detach drivers or change the device's active configuration.
          check(libusb_claim_interface(usb, alt.bInterfaceNumber), "claim sniffer interface");
          claimed_interface = alt.bInterfaceNumber;
          if (alt.bAlternateSetting)
            check(libusb_set_interface_alt_setting(usb, claimed_interface, alt.bAlternateSetting),
                  "select sniffer interface");
          if (usb_string(d.iManufacturer) != "tusb.org")
            throw std::runtime_error("native capture requires a tusb.org CH56x USBPV sniffer");
          serial = usb_string(d.iSerialNumber);
          return;
        }
      }
      throw std::runtime_error("CH56x bulk endpoints missing");
    }
    throw std::runtime_error("sniffer disconnected during enumeration");
  }
  void control(unsigned char request, unsigned short index, unsigned char* data,
               unsigned short size) {
    const int n = libusb_control_transfer(usb, size ? 0xc0 : 0x40, request, 0,
                                          index, data, size, 1000);
    check(n, "CH56x control transfer");
    if (n != size) throw std::runtime_error("short CH56x control transfer");
  }
  void write(const unsigned char* data, int size) {
    int n = 0;
    check(libusb_bulk_transfer(usb, output_pipe, const_cast<unsigned char*>(data),
                              size, &n, 1000), "CH56x bulk write");
    if (n != size) throw std::runtime_error("short CH56x bulk write");
  }
  unsigned char reg(unsigned char address, unsigned char value, bool read = false) {
    const auto cmd = register_command(address, value, read);
    write(cmd.data(), static_cast<int>(cmd.size()));
    std::array<unsigned char, 4> response{};
    int received = 0;
    while (received < static_cast<int>(response.size())) {
      int n = 0;
      check(libusb_bulk_transfer(usb, input_pipe, response.data() + received,
                                static_cast<int>(response.size()) - received, &n, 1000),
            "read register response");
      if (!n) throw std::runtime_error("empty register response");
      received += n;
    }
    if (response[0] != 0x55 || response[1] != cmd[1] ||
        static_cast<unsigned char>(response[0] + response[1] + response[2]) != response[3] ||
        (!read && response[2] != value))
      throw std::runtime_error("invalid register response at " + std::to_string(address));
    return response[2];
  }
  void configure(const unsigned char* options) {
    // Same volatile FPGA image and initialization as the supplied SDK. No
    // application/bootloader flash programming commands are used here.
    std::array<unsigned char, 4> version{};
    control(0x75, 1, version.data(), 4);
    control(0x73, 0, nullptr, 0);
    // The configuration command resets the device FIFO. Drain any old host
    // data before loading the image; a timeout is the expected empty condition.
    std::array<unsigned char, 65536> stale{};
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    for (;;) {
      int n = 0;
      const int rc = libusb_bulk_transfer(usb, input_pipe, stale.data(),
                                         stale.size(), &n, 10);
      if (rc == LIBUSB_ERROR_TIMEOUT) break;
      check(rc, "drain old capture bytes");
      if (Clock::now() >= deadline) throw std::runtime_error("capture FIFO did not drain");
    }
    for (std::size_t offset = 0; offset < sizeof(kCh56xFpga); offset += 4096) {
      write(kCh56xFpga + offset, static_cast<int>(
          std::min<std::size_t>(4096, sizeof(kCh56xFpga) - offset)));
    }
    bool configured = false;
    for (int i = 0; i < 100; ++i) {
      unsigned char status[2]{};
      control(0x75, 0, status, 2);
      if ((status[0] & 0x0f) == 3) { configured = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!configured) throw std::runtime_error("FPGA configuration did not complete");
    control(0x74, 0, nullptr, 0);
    reg(0x46, 0x10); reg(0x47, 0x17); reg(0x48, 0x11); reg(0x4a, 0x28);
    const unsigned char revision = reg(0x64, 0, true);
    reg(1, revision & 1 ? 0x50 : 0xa0);
    reg(0, 0x30); reg(0x49, 0x40);
    reg(8, 0x0c | options[0]);
    reg(0x1f, static_cast<unsigned char>(~options[1]));
    std::array<unsigned char, 8> pairs{};
    std::copy_n(options + 3, pairs.size(), pairs.begin());
    const auto filters = filter_registers(pairs, options[2] != 0);
    for (std::size_t i = 0; i < filters.size(); ++i)
      reg(static_cast<unsigned char>(0x20 + i), filters[i]);
  }

  void capture(void* callback_context, pfn_packet_handler callback, BatchEnd batch_end) noexcept {
    struct Read {
      unsigned char* bytes = nullptr;
      std::unique_ptr<unsigned char[]> fallback;
      bool dma = false;
      libusb_transfer* transfer = nullptr;
      bool pending = false;
      bool complete = false;
    };
    // Heap storage avoids placing a large transport ring on the reader stack.
    std::unique_ptr<std::array<Read, capture_read_count>> storage;
    bool command_started = false, stop_sent = false, pending_batch = false;
    try {
      storage = std::make_unique<std::array<Read, capture_read_count>>();
      auto submit = [](Read& read) {
        read.complete = false;
        check(libusb_submit_transfer(read.transfer), "submit capture read");
        read.pending = true;
      };
      for (auto& read : *storage) {
        read.transfer = libusb_alloc_transfer(0);
        if (!read.transfer) throw std::bad_alloc();
        // Linux usbfs can map DMA buffers directly, avoiding its completion
        // copy. Allocation is optional; ordinary buffers work on older hosts.
        read.bytes = libusb_dev_mem_alloc(usb, capture_read_bytes);
        read.dma = read.bytes != nullptr;
        if (!read.bytes) {
          read.fallback = std::make_unique<unsigned char[]>(capture_read_bytes);
          read.bytes = read.fallback.get();
        }
        libusb_fill_bulk_transfer(read.transfer, usb, input_pipe, read.bytes,
            capture_read_bytes, [](libusb_transfer* transfer) {
              auto& r = *static_cast<Read*>(transfer->user_data);
              r.pending = false;
              r.complete = true;
            }, &read, 0);
        submit(read);
      }
      ProtocolParser parser(callback_context, callback);
      const auto start_cmd = register_command(1, 1);
      command_started = true;
      write(start_cmd.data(), 4);
      const auto start_deadline = Clock::now() + std::chrono::seconds(2);
      auto stop_deadline = Clock::time_point::max();
      std::size_t index = 0;
      bool ready_reported = false;
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
        auto& read = (*storage)[index];
        if (!read.complete) {
          // One event dispatch can reap many Linux URBs. Parse them in submission
          // order, regardless of callback order, without a poll per completion.
          // SuperSpeed short packets can cause frequent Linux wakeups.
          // Let the queued reads accumulate briefly before reaping
          // them together. Keep startup/stop handshakes and USB 2.0 unchanged.
          // This adds a 250 us service target, below the 1 ms callback target;
          // scheduler delays can exceed either target. Buffers stay submitted.
          if (monitor_speed == 1 && parser.started() && !stop_sent)
            std::this_thread::sleep_for(std::chrono::microseconds(250));
          timeval timeout{0, pending_batch ? 1000 : 20000};
          const int rc = libusb_handle_events_timeout(context.value, &timeout);
          if (rc != LIBUSB_ERROR_INTERRUPTED) check(rc, "wait for capture read");
          if (batch_service && pending_batch)
            pending_batch = batch_service(callback_context, !read.complete);
          continue;
        }
        if (read.transfer->status != LIBUSB_TRANSFER_COMPLETED)
          throw std::runtime_error("capture transfer failed (libusb status " +
                                   std::to_string(read.transfer->status) + ")");
        const bool parsed = parser.feed(read.bytes, read.transfer->actual_length, wall_ns());
        if (batch_end) batch_end(callback_context);
        if (batch_service) pending_batch = batch_service(callback_context, !parsed || parser.stopped());
        if (!parsed) throw std::runtime_error(parser.error());
        if (!ready_reported && parser.started()) {
          std::lock_guard<std::mutex> lock(mutex);
          started = ready_reported = true;
          ready.notify_all();
        }
        if (parser.stopped()) {
          if (!stop_sent) throw std::runtime_error("device stopped capture unexpectedly");
          break;
        }
        submit(read);
        index = (index + 1) % storage->size();
      }
    } catch (const std::exception& ex) { fail(ex.what()); }
      catch (...) { fail("unexpected native capture exception"); }
    if (batch_end) {
      try { batch_end(callback_context); }
      catch (const std::exception& ex) { fail(ex.what()); }
      catch (...) { fail("unexpected native batch flush exception"); }
    }
    if (batch_service) {
      try { batch_service(callback_context, true); }
      catch (const std::exception& ex) { fail(ex.what()); }
      catch (...) { fail("unexpected native batch service exception"); }
    }
    if (command_started && !stop_sent) {
      try { const auto cmd = register_command(1, 0); write(cmd.data(), 4); }
      catch (const std::exception& ex) { fail(ex.what()); }
    }
    if (storage) {
      for (auto& read : *storage) if (read.pending) libusb_cancel_transfer(read.transfer);
      // Cancellation does not return ownership. Dispatch all callbacks before
      // freeing any transfer, including partially submitted startup failures.
      while (std::any_of(storage->begin(), storage->end(), [](const Read& r) { return r.pending; })) {
        timeval timeout{0, 20000};
        const int rc = libusb_handle_events_timeout(context.value, &timeout);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
          fail(std::string("reap cancelled capture reads: ") + libusb_error_name(rc));
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      for (auto& read : *storage) {
        if (read.transfer) libusb_free_transfer(read.transfer);
        if (read.dma) libusb_dev_mem_free(usb, read.bytes, capture_read_bytes);
      }
    }
  }
};
}  // namespace

const char* UPV_CALL list_devices() {
  device_list.clear();
  last_error.clear();
  try {
    device_list = detail::list_serials<Device>(device_paths());
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
    auto device = detail::find_device<Device>(device_paths(), serial);
    // Once selected, configuration/startup errors must reach the caller;
    // they are not failures probing an unrelated interface.
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
