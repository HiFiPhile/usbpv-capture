#include "usbpv_lib.h"
#include "usbpv_output.hpp"
#include "usbpv_capture_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "usbpv_native.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sys/stat.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kMaxPacketBytes = usbpv::kMaxPacketBytes;
constexpr std::size_t kDefaultQueueCapacity = 16384;
constexpr std::uint8_t kDefaultFlags = static_cast<std::uint8_t>(
    UPV_FLAG_ALL & ~(UPV_FLAG_SOF | UPV_FLAG_NAK));
constexpr std::uint16_t kLinkTypeUsbLow = 293;
constexpr std::uint16_t kLinkTypeUsbFull = 294;
constexpr std::uint16_t kLinkTypeUsbHigh = 295;

std::atomic<bool> g_interrupted{false};

void signal_handler(int) { g_interrupted.store(true, std::memory_order_relaxed); }

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
      type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
    g_interrupted.store(true, std::memory_order_relaxed);
    return TRUE;
  }
  return FALSE;
}
#endif

std::string json_escape(const std::string& input) {
  std::ostringstream out;
  for (unsigned char ch : input) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<unsigned>(ch) << std::dec;
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  return out.str();
}

bool path_exists(const std::string& path) {
  if (path.empty()) return false;
#ifdef _WIN32
  struct _stat64 info {};
  return _stat64(path.c_str(), &info) == 0;
#else
  struct stat info {};
  return stat(path.c_str(), &info) == 0;
#endif
}

bool write_text_file(const std::string& path, const std::string& text,
                     std::string& error) {
  std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!file) {
    error = "cannot create " + path;
    return false;
  }
  file << text << '\n';
  file.flush();
  if (!file) {
    error = "cannot write " + path;
    return false;
  }
  return true;
}

bool parse_u64(const std::string& text, std::uint64_t& value) {
  if (text.empty() || text[0] == '-') return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (errno || !end || *end != '\0') return false;
  value = static_cast<std::uint64_t>(parsed);
  return true;
}

bool parse_double_nonnegative(const std::string& text, double& value) {
  if (text.empty() || text[0] == '-') return false;
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(text.c_str(), &end);
  if (errno || !end || *end != '\0' || !std::isfinite(parsed) || parsed < 0.0)
    return false;
  value = parsed;
  return true;
}

std::vector<std::string> split_devices(const char* devices) {
  std::vector<std::string> result;
  if (!devices) return result;
  std::string all(devices);
  std::size_t begin = 0;
  while (begin <= all.size()) {
    const std::size_t comma = all.find(',', begin);
    std::string item = all.substr(begin, comma == std::string::npos
                                           ? std::string::npos
                                           : comma - begin);
    if (!item.empty()) result.push_back(item);
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  return result;
}

#ifndef _WIN32
std::string executable_directory(const char* argv0) {
  std::array<char, 4096> buffer{};
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  std::string path = length > 0 ? std::string(buffer.data(), static_cast<std::size_t>(length))
                                : std::string(argv0 ? argv0 : "");
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string default_library_path(const char* argv0) {
  const std::string filename = "libusbpv_lib.so";
  const std::string separator = "/";
  const std::string runtime_directory = "vendor/linux-x64";
  const std::string executable_dir = executable_directory(argv0);
  std::vector<std::string> candidates;
  candidates.push_back(executable_dir + separator + filename);
  candidates.push_back(executable_dir + separator + runtime_directory + separator + filename);
  candidates.push_back(executable_dir + separator + ".." + separator + runtime_directory +
                       separator + filename);
  std::array<char, 4096> current{};
  if (getcwd(current.data(), current.size())) {
    const std::string current_dir(current.data());
    candidates.push_back(current_dir + separator + filename);
    candidates.push_back(current_dir + separator + runtime_directory + separator + filename);
  }
  for (const std::string& candidate : candidates) {
    if (path_exists(candidate)) return candidate;
  }
  return candidates.front();
}
#endif

class CaptureApi {
 public:
  ~CaptureApi() { unload(); }
  CaptureApi(const CaptureApi&) = delete;
  CaptureApi& operator=(const CaptureApi&) = delete;
  CaptureApi() = default;

  bool load(const std::string& path, std::string& error) {
#ifdef _WIN32
    if (path.empty()) {
      list_devices = usbpv::native::list_devices;
      open_device = usbpv::native::open_device;
      close_device = usbpv::native::close_device;
      get_last_error = usbpv::native::get_last_error;
      get_error_string = usbpv::native::get_error_string;
      get_monitor_speed = usbpv::native::get_monitor_speed;
      return true;
    }
    module_ = LoadLibraryExA(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
      error = "cannot load " + path + " (Windows error " +
              std::to_string(GetLastError()) + ")";
      return false;
    }
#else
    module_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module_) {
      const char* detail = dlerror();
      error = "cannot load " + path + ": " + (detail ? detail : "unknown error");
      return false;
    }
#endif
    list_devices = symbol<pfnt_upv_list_devices>("upv_list_devices");
    open_device = symbol<pfnt_upv_open_device>("upv_open_device");
    close_device = symbol<pfnt_upv_close_device>("upv_close_device");
    get_last_error = symbol<pfnt_upv_get_last_error>("upv_get_last_error");
    get_error_string = symbol<pfnt_upv_get_error_string>("upv_get_error_string");
    get_monitor_speed = symbol<pfnt_upv_get_monitor_speed>("upv_get_monitor_speed");
    if (!list_devices || !open_device || !close_device || !get_last_error ||
        !get_error_string || !get_monitor_speed) {
      error = "USBPV library is missing one or more required exports";
      unload();
      return false;
    }
    return true;
  }

  std::string last_error() const {
    if (!get_last_error || !get_error_string) return "unknown USBPV error";
    const int code = get_last_error();
    const char* message = get_error_string(code);
    return std::to_string(code) + ": " + (message ? message : "unknown USBPV error");
  }

  bool failed(UPV_HANDLE device) const {
#ifdef _WIN32
    return !module_ && usbpv::native::failed(device);
#else
    (void)device;
    return false;
#endif
  }

  pfnt_upv_list_devices list_devices = nullptr;
  pfnt_upv_open_device open_device = nullptr;
  pfnt_upv_close_device close_device = nullptr;
  pfnt_upv_get_last_error get_last_error = nullptr;
  pfnt_upv_get_error_string get_error_string = nullptr;
  pfnt_upv_get_monitor_speed get_monitor_speed = nullptr;

 private:
  template <typename T>
  T symbol(const char* name) {
#ifdef _WIN32
    FARPROC raw = GetProcAddress(module_, name);
#else
    void* raw = dlsym(module_, name);
#endif
    static_assert(sizeof(T) == sizeof(raw), "unsupported function pointer representation");
    T result = nullptr;
    std::memcpy(&result, &raw, sizeof(result));
    return result;
  }
  void unload() {
    if (!module_) return;
#ifdef _WIN32
    FreeLibrary(module_);
#else
    dlclose(module_);
#endif
    module_ = nullptr;
  }
#ifdef _WIN32
  HMODULE module_ = nullptr;
#else
  void* module_ = nullptr;
#endif
};

struct EndpointFilter {
  std::uint8_t address = UPV_NO_ADDR;
  std::uint8_t endpoint = UPV_NO_EP;
};

enum class FilterMode { Accept, Drop };

struct Config {
  std::string serial;
  std::string output;
  std::string events;
  std::string summary;
  std::string ready_file;
  std::string stop_file;
  std::string library;
  std::string speed_name;
  UPV_CaptureSpeed speed = UPV_Cap_Speed_High;
  std::uint8_t flags = kDefaultFlags;
  FilterMode filter_mode = FilterMode::Accept;
  std::vector<EndpointFilter> filters;
  std::size_t queue_capacity = kDefaultQueueCapacity;
  std::uint64_t flush_ms = 250;
  double duration_seconds = 30.0;
  double idle_timeout_seconds = 0.0;
  std::uint64_t max_packets = 0;
};

bool parse_endpoint_filter(const std::string& text, EndpointFilter& filter,
                           std::string& error) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos || text.find(':', colon + 1) != std::string::npos) {
    error = "filter must be ADDR:EP (use * as a wildcard): " + text;
    return false;
  }
  const std::string address = text.substr(0, colon);
  const std::string endpoint = text.substr(colon + 1);
  std::uint64_t number = 0;
  if (address == "*") {
    filter.address = UPV_NO_ADDR;
  } else if (!parse_u64(address, number) || number > 127) {
    error = "USB address must be 0..127 or *: " + address;
    return false;
  } else {
    filter.address = static_cast<std::uint8_t>(number);
  }
  if (endpoint == "*") {
    filter.endpoint = UPV_NO_EP;
  } else if (!parse_u64(endpoint, number) || number > 15) {
    error = "USB endpoint must be 0..15 or *: " + endpoint;
    return false;
  } else {
    filter.endpoint = static_cast<std::uint8_t>(number);
  }
  return true;
}

void print_help() {
  std::cout <<
      "USBPV agent capture\n\n"
      "Usage:\n"
      "  usbpv_capture list [--library PATH]\n"
      "  usbpv_capture capture --speed high|full|low --output FILE [options]\n\n"
      "Capture options:\n"
      "  --serial SN          Select device; one connected device is auto-selected\n"
      "  --duration SEC       Stop after SEC (default 30; 0 disables)\n"
      "  --max-packets N      Stop after N queued data packets (0 disables)\n"
      "  --idle-timeout SEC   Stop after no callbacks for SEC (0 disables)\n"
      "  --flush-ms MS        Discard stale startup callbacks (default 250)\n"
      "  --queue-capacity N   Preallocated packet slots (default 16384)\n"
      "  --include-sof        Enable high-volume SOF packets\n"
      "  --include-nak        Enable dangerous, very high-volume NAK packets\n"
      "  --accept ADDR:EP     Hardware accept filter; repeat up to four times\n"
      "  --drop ADDR:EP       Hardware drop filter; repeat up to four times\n"
      "  --ready-file PATH    Create after flushing, immediately before capture\n"
      "  --stop-file PATH     Stop when this path appears\n"
      "  --library PATH       Opt into legacy usbpv_lib DLL/SO backend\n\n"
      "Outputs FILE, FILE.events.jsonl, and FILE.summary.json must not exist.\n"
      "All status lines on stdout are JSON. Auto speed is intentionally unsupported.\n";
}

bool take_value(int argc, char** argv, int& index, std::string& value,
                std::string& error) {
  if (index + 1 >= argc) {
    error = std::string("missing value for ") + argv[index];
    return false;
  }
  value = argv[++index];
  return true;
}

bool parse_capture_args(int argc, char** argv, Config& config, std::string& error) {
  bool speed_set = false;
  bool accept_set = false;
  bool drop_set = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    std::string value;
    if (arg == "--speed") {
      if (!take_value(argc, argv, i, value, error)) return false;
      speed_set = true;
      config.speed_name = value;
      if (value == "high") config.speed = UPV_Cap_Speed_High;
      else if (value == "full") config.speed = UPV_Cap_Speed_Full;
      else if (value == "low") config.speed = UPV_Cap_Speed_Low;
      else {
        error = "--speed must be high, full, or low; auto is unsupported";
        return false;
      }
    } else if (arg == "--output") {
      if (!take_value(argc, argv, i, config.output, error)) return false;
    } else if (arg == "--serial") {
      if (!take_value(argc, argv, i, config.serial, error)) return false;
    } else if (arg == "--library") {
      if (!take_value(argc, argv, i, config.library, error)) return false;
    } else if (arg == "--ready-file") {
      if (!take_value(argc, argv, i, config.ready_file, error)) return false;
    } else if (arg == "--stop-file") {
      if (!take_value(argc, argv, i, config.stop_file, error)) return false;
    } else if (arg == "--duration" || arg == "--idle-timeout") {
      if (!take_value(argc, argv, i, value, error)) return false;
      double parsed = 0.0;
      if (!parse_double_nonnegative(value, parsed)) {
        error = arg + " requires a finite nonnegative number";
        return false;
      }
      if (arg == "--duration") config.duration_seconds = parsed;
      else config.idle_timeout_seconds = parsed;
    } else if (arg == "--flush-ms" || arg == "--max-packets" ||
               arg == "--queue-capacity") {
      if (!take_value(argc, argv, i, value, error)) return false;
      std::uint64_t parsed = 0;
      if (!parse_u64(value, parsed)) {
        error = arg + " requires a nonnegative integer";
        return false;
      }
      if (arg == "--flush-ms") config.flush_ms = parsed;
      else if (arg == "--max-packets") config.max_packets = parsed;
      else {
        if (parsed < 256 || parsed > 65536) {
          error = "--queue-capacity must be 256..65536";
          return false;
        }
        config.queue_capacity = static_cast<std::size_t>(parsed);
      }
    } else if (arg == "--include-sof") {
      config.flags = static_cast<std::uint8_t>(config.flags | UPV_FLAG_SOF);
    } else if (arg == "--include-nak") {
      config.flags = static_cast<std::uint8_t>(config.flags | UPV_FLAG_NAK);
    } else if (arg == "--accept" || arg == "--drop") {
      if (!take_value(argc, argv, i, value, error)) return false;
      if (arg == "--accept") accept_set = true;
      else drop_set = true;
      if (accept_set && drop_set) {
        error = "--accept and --drop cannot be combined";
        return false;
      }
      EndpointFilter filter;
      if (!parse_endpoint_filter(value, filter, error)) return false;
      config.filters.push_back(filter);
      config.filter_mode = accept_set ? FilterMode::Accept : FilterMode::Drop;
      if (config.filters.size() > 4) {
        error = "the USBPV hardware accepts at most four address/endpoint filters";
        return false;
      }
    } else {
      error = "unknown capture option: " + arg;
      return false;
    }
  }
  if (!speed_set) {
    error = "--speed high|full|low is required (auto speed is intentionally unavailable)";
    return false;
  }
  if (config.output.empty()) {
    error = "--output FILE is required";
    return false;
  }
  config.events = config.output + ".events.jsonl";
  config.summary = config.output + ".summary.json";
  if (config.duration_seconds == 0.0 && config.max_packets == 0 &&
      config.idle_timeout_seconds == 0.0 && config.stop_file.empty()) {
    std::cerr << "warning: capture is unbounded; stop it with Ctrl+C\n";
  }
  if (config.flags & UPV_FLAG_NAK) {
    std::cerr << "warning: NAK capture can overwhelm or jam the sniffer; use a short duration and hardware filter\n";
  }
  return true;
}

using usbpv::PacketSlot;
using usbpv::PacketQueue;

using usbpv::Counters;
using usbpv::CaptureContext;
using usbpv::steady_now_ns;
using usbpv::flush_callback_batch;
using usbpv::service_callback_batch;
using usbpv::packet_callback;

void put_u16(std::ostream& out, std::uint16_t value) {
  const std::array<char, 2> bytes{{static_cast<char>(value & 0xff),
                                   static_cast<char>((value >> 8) & 0xff)}};
  out.write(bytes.data(), bytes.size());
}

void put_u32(std::ostream& out, std::uint32_t value) {
  const std::array<char, 4> bytes{{static_cast<char>(value & 0xff),
                                   static_cast<char>((value >> 8) & 0xff),
                                   static_cast<char>((value >> 16) & 0xff),
                                   static_cast<char>((value >> 24) & 0xff)}};
  out.write(bytes.data(), bytes.size());
}

void put_u64(std::ostream& out, std::uint64_t value) {
  put_u32(out, static_cast<std::uint32_t>(value & 0xffffffffULL));
  put_u32(out, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t padded_length(std::uint32_t length) { return (length + 3U) & ~3U; }

void put_option(std::ostream& out, std::uint16_t type, const void* data,
                std::uint16_t length) {
  put_u16(out, type);
  put_u16(out, length);
  if (length) out.write(static_cast<const char*>(data), length);
  const std::uint32_t padding = padded_length(length) - length;
  static const char zeros[4] = {};
  if (padding) out.write(zeros, padding);
}

void put_string_option(std::ostream& out, std::uint16_t type,
                       const std::string& value) {
  put_option(out, type, value.data(), static_cast<std::uint16_t>(value.size()));
}

class PcapngWriter {
 public:
  bool open(const std::string& path, std::uint16_t link_type,
            const std::string& description, std::string& error) {
    file_.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file_) {
      error = "cannot create " + path;
      return false;
    }
    write_section_header();
    write_interface(link_type, description);
    file_.flush();
    if (!file_) {
      error = "cannot write pcapng headers to " + path;
      return false;
    }
    if (!output_.start(
            [this](const char* bytes, std::size_t size) {
              file_.write(bytes, static_cast<std::streamsize>(size));
              return static_cast<bool>(file_);
            },
            [this] { file_.flush(); return static_cast<bool>(file_); })) {
      error = output_.error();
      return false;
    }
    return true;
  }

  bool write_packet(const PacketSlot& packet) {
    char* record = output_.reserve(usbpv::packet_block_size(packet.length));
    if (!record) return false;
    usbpv::encode_packet_block(record, packet.seconds, packet.nanoseconds,
                               packet.data.data(), packet.length);
    return true;
  }

  bool flush() { return output_.flush(); }
  bool failed() const { return output_.failed(); }
  std::string error() const { return output_.error(); }
  std::uint64_t peak_pending_bytes() const { return output_.peak_pending_bytes(); }
  bool finish() {
    const bool ok = output_.finish();
    if (file_.is_open()) file_.close();
    return ok && static_cast<bool>(file_);
  }

 private:
  void write_section_header() {
    const std::string app = "usbpv_capture 1.0";
    const std::uint32_t option_bytes = 4U + padded_length(static_cast<std::uint32_t>(app.size())) + 4U;
    const std::uint32_t total = 28U + option_bytes;
    put_u32(file_, 0x0A0D0D0AU);
    put_u32(file_, total);
    put_u32(file_, 0x1A2B3C4DU);
    put_u16(file_, 1);
    put_u16(file_, 0);
    put_u64(file_, std::numeric_limits<std::uint64_t>::max());
    put_string_option(file_, 4, app);
    put_u16(file_, 0);
    put_u16(file_, 0);
    put_u32(file_, total);
  }

  void write_interface(std::uint16_t link_type, const std::string& description) {
    const std::string name = "USBPV";
    const std::uint8_t resolution = 9;
    const std::uint32_t options =
        4U + padded_length(static_cast<std::uint32_t>(name.size())) +
        4U + padded_length(static_cast<std::uint32_t>(description.size())) +
        4U + padded_length(1U) + 4U;
    const std::uint32_t total = 20U + options;
    put_u32(file_, 0x00000001U);
    put_u32(file_, total);
    put_u16(file_, link_type);
    put_u16(file_, 0);
    put_u32(file_, static_cast<std::uint32_t>(kMaxPacketBytes));
    put_string_option(file_, 2, name);
    put_string_option(file_, 3, description);
    put_option(file_, 9, &resolution, 1);
    put_u16(file_, 0);
    put_u16(file_, 0);
    put_u32(file_, total);
  }

  std::ofstream file_;
  // Destroy/join output before destroying the stream captured by its callbacks.
  usbpv::BufferedOutput output_;
};

const char* event_name(int type) {
  switch (type) {
    case UPV_RESET_BEGIN: return "reset_begin";
    case UPV_RESET_END: return "reset_end";
    case UPV_SUSPEND_BEGIN: return "suspend_begin";
    case UPV_SUSPEND_END: return "suspend_end";
    case UPV_OVERFLOW: return "overflow";
    default: return "unknown";
  }
}

const char* packet_speed_name(int speed) {
  switch (speed) {
    case UPV_SPD_LOW: return "low";
    case UPV_SPD_FULL: return "full";
    case UPV_SPD_HIGH: return "high";
    default: return "unknown";
  }
}

struct WriterState {
  std::atomic<bool> failed{false};
  std::mutex error_mutex;
  std::string error;
};

void set_writer_error(WriterState& state, const std::string& error) {
  {
    std::lock_guard<std::mutex> lock(state.error_mutex);
    if (state.error.empty()) state.error = error;
  }
  state.failed.store(true, std::memory_order_release);
}

void writer_thread(CaptureContext& context, PcapngWriter& pcap,
                   std::ofstream& events, WriterState& state) {
  usbpv::BufferedOutput event_output(64 * 1024, 4);
  if (!event_output.start(
          [&](const char* bytes, std::size_t size) {
            events.write(bytes, static_cast<std::streamsize>(size));
            return static_cast<bool>(events);
          }, [&] { events.flush(); return static_cast<bool>(events); })) {
    set_writer_error(state, "event output: " + event_output.error());
    pcap.finish();
    return;
  }
  std::vector<PacketSlot> batch(256);
  auto last_flush = std::chrono::steady_clock::now();
  while (true) {
    if (pcap.failed() || event_output.failed()) {
      set_writer_error(state, pcap.failed() ? "pcapng: " + pcap.error()
                                           : "events: " + event_output.error());
      break;
    }
    const std::size_t amount = context.queue.pop_batch(batch, batch.size());
    for (std::size_t i = 0; i < amount; ++i) {
      const PacketSlot& packet = batch[i];
      const int type = GetPacketType(packet.status);
      if (type == UPV_DATA_PACKET) {
        if (!pcap.write_packet(packet)) {
          set_writer_error(state, "pcapng: " + pcap.error());
          break;
        }
      } else {
        std::ostringstream line;
        line << "{\"event\":\"bus_event\",\"type\":\""
               << event_name(type) << "\",\"type_id\":" << type
               << ",\"seconds\":" << packet.seconds
               << ",\"nanoseconds\":" << packet.nanoseconds
               << ",\"speed\":\"" << packet_speed_name(GetPacketSpeed(packet.status))
               << "\"}\n";
        const std::string text = line.str();
        char* output = event_output.reserve(text.size());
        if (!output) {
          set_writer_error(state, "events: " + event_output.error());
          break;
        }
        std::memcpy(output, text.data(), text.size());
      }
    }
    if (state.failed.load(std::memory_order_acquire)) break;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_flush >= std::chrono::seconds(1)) {
      if (!pcap.flush()) {
        set_writer_error(state, "pcapng: " + pcap.error());
        break;
      }
      if (!event_output.flush()) {
        set_writer_error(state, "events: " + event_output.error());
        break;
      }
      last_flush = now;
    }
    if (context.queue.drained()) break;
  }
  if (!pcap.finish()) set_writer_error(state, "final pcapng output failed: " + pcap.error());
  if (!event_output.finish()) set_writer_error(state, "final event output failed: " + event_output.error());
  events.close();
  if (!events) set_writer_error(state, "event file close failed");
  context.counters.output_pending_peak_bytes.store(pcap.peak_pending_bytes(), std::memory_order_relaxed);
}

std::uint16_t link_type_for_speed(UPV_CaptureSpeed speed) {
  if (speed == UPV_Cap_Speed_Low) return kLinkTypeUsbLow;
  if (speed == UPV_Cap_Speed_Full) return kLinkTypeUsbFull;
  return kLinkTypeUsbHigh;
}

std::vector<char> make_open_options(const Config& config) {
  std::vector<char> options(config.serial.begin(), config.serial.end());
  options.push_back('\0');
  options.push_back(static_cast<char>(config.speed));
  options.push_back(static_cast<char>(config.flags));
  options.push_back(static_cast<char>(config.filter_mode == FilterMode::Accept
                                          ? UPV_FILTER_ACCEPT
                                          : UPV_FILTER_DROP));
  for (const EndpointFilter& filter : config.filters) {
    options.push_back(static_cast<char>(filter.address));
    options.push_back(static_cast<char>(filter.endpoint));
  }
  while (options.size() < config.serial.size() + 1 + 3 + 8) {
    options.push_back(static_cast<char>(UPV_NO_ADDR));
    options.push_back(static_cast<char>(UPV_NO_EP));
  }
  return options;
}

std::string hex_flags(std::uint8_t flags) {
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
      << static_cast<unsigned>(flags);
  return out.str();
}

std::string make_status_json(const char* event, const Config& config,
                             const CaptureContext& context,
                             const std::string& reason, bool complete,
                             double elapsed_seconds) {
  const Counters& c = context.counters;
  std::ostringstream out;
  out << "{\"event\":\"" << event << "\",\"complete\":"
      << (complete ? "true" : "false") << ",\"reason\":\""
      << json_escape(reason) << "\",\"backend\":\""
      << (config.library.empty() ? "native-winusb" : "vendor-library")
      << "\",\"serial\":\"" << json_escape(config.serial)
      << "\",\"speed\":\"" << config.speed_name << "\",\"filter_mask\":\""
      << hex_flags(config.flags) << "\",\"output\":\"" << json_escape(config.output)
      << "\",\"events_file\":\"" << json_escape(config.events)
      << "\",\"summary_file\":\"" << json_escape(config.summary)
      << "\",\"elapsed_seconds\":" << std::fixed << std::setprecision(6)
      << elapsed_seconds << ",\"callbacks\":" << c.callbacks.load()
      << ",\"flushed_callbacks\":" << c.flushed.load()
      << ",\"packets\":" << c.data_packets.load()
      << ",\"bytes\":" << c.data_bytes.load()
      << ",\"bus_events\":" << c.bus_events.load()
      << ",\"device_overflows\":" << c.device_overflows.load()
      << ",\"queue_dropped\":" << c.queue_dropped.load()
      << ",\"invalid_dropped\":" << c.invalid_dropped.load()
      << ",\"speed_mismatches\":" << c.speed_mismatches.load()
      << ",\"output_pending_peak_bytes\":" << c.output_pending_peak_bytes.load() << "}";
  return out.str();
}

int emit_error(const std::string& code, const std::string& message, int exit_code) {
  std::cout << "{\"event\":\"error\",\"code\":\"" << json_escape(code)
            << "\",\"message\":\"" << json_escape(message) << "\"}" << std::endl;
  return exit_code;
}

bool select_device(CaptureApi& api, Config& config, std::string& error) {
  const char* listed = api.list_devices();
  if (!listed) { error = api.last_error(); return false; }
  const std::vector<std::string> devices = split_devices(listed);
  if (devices.empty()) {
    error = "no USBPV device connected; after a NAK-related jam, power-cycle the sniffer";
    return false;
  }
  if (!config.serial.empty()) {
    if (std::find(devices.begin(), devices.end(), config.serial) == devices.end()) {
      error = "requested serial is not connected: " + config.serial;
      return false;
    }
    return true;
  }
  if (devices.size() != 1) {
    error = "multiple USBPV devices are connected; specify --serial";
    return false;
  }
  config.serial = devices.front();
  return true;
}

int run_list(int argc, char** argv) {
  std::string library;
  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]) == "--library" && i + 1 < argc) library = argv[++i];
    else return emit_error("usage", "list accepts only --library PATH", 2);
  }
#ifndef _WIN32
  if (library.empty()) {
    library = default_library_path(argv[0]);
  }
#endif
  CaptureApi api;
  std::string error;
  if (!api.load(library, error)) return emit_error("library_load", error, 3);
  const char* listed = api.list_devices();
  if (!listed) return emit_error("device_list", api.last_error(), 4);
  const auto devices = split_devices(listed);
  std::cout << "{\"event\":\"devices\",\"count\":" << devices.size()
            << ",\"devices\":[";
  for (std::size_t i = 0; i < devices.size(); ++i) {
    if (i) std::cout << ',';
    std::cout << '"' << json_escape(devices[i]) << '"';
  }
  std::cout << "]}" << std::endl;
  return 0;
}

int run_capture(int argc, char** argv) {
  Config config;
  std::string error;
  if (!parse_capture_args(argc, argv, config, error)) return emit_error("usage", error, 2);
  const std::array<std::string, 5> outputs{{config.output, config.events, config.summary,
                                           config.ready_file, config.stop_file}};
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    if (outputs[i].empty()) continue;
    for (std::size_t j = i + 1; j < outputs.size(); ++j) {
      if (!outputs[j].empty() && outputs[i] == outputs[j]) {
        return emit_error("path_conflict", "output and control paths must be distinct: " +
                                              outputs[i], 2);
      }
    }
  }
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    if (!outputs[i].empty() && path_exists(outputs[i])) {
      const std::string kind = i == 4 ? "stop/control path already exists: "
                                      : "refusing to overwrite existing path: ";
      return emit_error("path_exists", kind + outputs[i], 2);
    }
  }
#ifndef _WIN32
  if (config.library.empty()) {
    config.library = default_library_path(argv[0]);
  }
#endif
  CaptureApi api;
  if (!api.load(config.library, error)) return emit_error("library_load", error, 3);
  if (!select_device(api, config, error)) return emit_error("device_selection", error, 4);

  CaptureContext context(config.queue_capacity, config.speed);
  const std::vector<char> options = make_open_options(config);
  UPV_HANDLE device;
#ifdef _WIN32
  if (config.library.empty()) {
    context.native_batching = true;
    device = usbpv::native::open_device_batched(options.data(), static_cast<int>(options.size()),
                                               &context, packet_callback, nullptr, service_callback_batch);
  } else
#endif
  device = api.open_device(options.data(), static_cast<int>(options.size()),
                                      &context, packet_callback);
  if (!device) {
    return emit_error("device_open", api.last_error() +
                                         "; power-cycle the sniffer if stale NAK traffic jammed it",
                      5);
  }

  const int monitor_speed = api.get_monitor_speed(device);
  if (monitor_speed < 0) {
    std::cerr << "warning: could not query USBPV monitor port speed\n";
  }
  if (config.flush_ms) std::this_thread::sleep_for(std::chrono::milliseconds(config.flush_ms));
  if (api.failed(device)) {
    api.close_device(device);
    return emit_error("device_read", api.last_error(), 5);
  }

  PcapngWriter pcap;
  const std::string description = "USBPV serial " + config.serial + ", explicit " +
                                  config.speed_name + " speed, flags " + hex_flags(config.flags);
  if (!pcap.open(config.output, link_type_for_speed(config.speed), description, error)) {
    api.close_device(device);
    return emit_error("output_open", error, 6);
  }
  std::ofstream events(config.events, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!events) {
    api.close_device(device);
    return emit_error("output_open", "cannot create " + config.events, 6);
  }

  WriterState writer_state;
  std::thread writer(writer_thread, std::ref(context), std::ref(pcap),
                     std::ref(events), std::ref(writer_state));
  context.counters.last_callback_ns.store(steady_now_ns(), std::memory_order_relaxed);
  const auto capture_start = std::chrono::steady_clock::now();
  context.accepting.store(true, std::memory_order_release);

  std::ostringstream ready;
  ready << "{\"event\":\"ready\",\"backend\":\""
        << (config.library.empty() ? "native-winusb" : "vendor-library")
        << "\",\"serial\":\"" << json_escape(config.serial)
        << "\",\"speed\":\"" << config.speed_name << "\",\"filter_mask\":\""
        << hex_flags(config.flags) << "\",\"output\":\"" << json_escape(config.output)
        << "\",\"events_file\":\"" << json_escape(config.events)
        << "\",\"summary_file\":\"" << json_escape(config.summary)
        << "\",\"monitor_port\":\""
        << (monitor_speed < 0 ? "unknown" : monitor_speed == 0 ? "high" : "super")
        << "\",\"queue_capacity\":" << config.queue_capacity
        << ",\"output_buffer_bytes\":" << usbpv::BufferedOutput::block_bytes * usbpv::BufferedOutput::block_count
        << ",\"flushed_callbacks\":" << context.counters.flushed.load() << "}";
  const std::string ready_json = ready.str();
  if (!config.ready_file.empty() && !write_text_file(config.ready_file, ready_json, error)) {
    context.accepting.store(false, std::memory_order_release);
    api.close_device(device);
    context.queue.stop();
    writer.join();
    return emit_error("ready_file", error, 6);
  }
  std::cout << ready_json << std::endl;

  std::string reason = "interrupted";
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - capture_start).count();
    if (g_interrupted.load(std::memory_order_relaxed)) {
      reason = "interrupted";
      break;
    }
    if (writer_state.failed.load(std::memory_order_acquire)) {
      reason = "writer_error";
      break;
    }
    if (api.failed(device)) {
      reason = "device_read_error";
      break;
    }
    if (config.duration_seconds > 0.0 && elapsed >= config.duration_seconds) {
      reason = "duration";
      break;
    }
    if (config.max_packets > 0 &&
        context.counters.data_packets.load(std::memory_order_relaxed) >= config.max_packets) {
      reason = "max_packets";
      break;
    }
    if (config.idle_timeout_seconds > 0.0) {
      const std::int64_t idle_ns = steady_now_ns() -
          context.counters.last_callback_ns.load(std::memory_order_relaxed);
      if (idle_ns >= static_cast<std::int64_t>(config.idle_timeout_seconds * 1e9)) {
        reason = "idle_timeout";
        break;
      }
    }
    if (!config.stop_file.empty() && path_exists(config.stop_file)) {
      reason = "stop_file";
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  context.accepting.store(false, std::memory_order_release);
  const int close_result = api.close_device(device);
  context.queue.stop();
  writer.join();
  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - capture_start)
                             .count();

  bool complete = close_result == 0 && !writer_state.failed.load() &&
                  context.counters.queue_dropped.load() == 0 &&
                  context.counters.invalid_dropped.load() == 0 &&
                  context.counters.device_overflows.load() == 0 &&
                  context.counters.speed_mismatches.load() == 0;
  if (close_result != 0) reason = "device_close_error: " + api.last_error();
  if (writer_state.failed.load()) {
    std::lock_guard<std::mutex> lock(writer_state.error_mutex);
    reason = writer_state.error.empty() ? "writer_error" : writer_state.error;
  } else if (!complete && close_result == 0) {
    reason += "_with_loss";
  }
  std::string summary = make_status_json("complete", config, context, reason,
                                         complete, elapsed);
  if (!write_text_file(config.summary, summary, error)) {
    std::cerr << error << '\n';
    complete = false;
    reason = "summary_write_error: " + error;
    summary = make_status_json("complete", config, context, reason, false, elapsed);
  }
  std::cout << summary << std::endl;
  return complete ? 0 : 7;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
#ifdef _WIN32
  SetConsoleCtrlHandler(console_handler, TRUE);
#endif
  if (argc < 2 || std::string(argv[1]) == "help" ||
      std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
    print_help();
    return argc < 2 ? 2 : 0;
  }
  const std::string command = argv[1];
  if (command == "list") return run_list(argc, argv);
  if (command == "capture") return run_capture(argc, argv);
  return emit_error("usage", "unknown command: " + command, 2);
}
