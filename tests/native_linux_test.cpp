// Exercise the real Linux backend with deterministic libusb completions/faults.
// No hardware is opened. Assertions stay enabled in Release builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "usbpv_native.hpp"
#include <libusb.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

namespace {
enum class Fault { None, Submit, Completion, Malformed, NoStart, NoStop };
Fault fault;
int submit_count, fail_submit, allocations, claims, callbacks, forced_flushes;
bool streaming;
bool dma_enabled = true;
unsigned short monitor_packet_size = 512;
int dma_allocations;
std::deque<libusb_transfer*> pending;
std::vector<unsigned char> stream;
std::array<unsigned char, 4> response;
libusb_device* devices[] = {reinterpret_cast<libusb_device*>(1), nullptr};
libusb_endpoint_descriptor endpoints[2]{};
libusb_interface_descriptor alt{};
libusb_interface iface{};
libusb_config_descriptor config{};
void reset(Fault f = Fault::None, int at = 0) {
  assert(pending.empty() && allocations == 0 && claims == 0 && dma_allocations == 0);
  fault = f; fail_submit = at; submit_count = callbacks = forced_flushes = 0;
  streaming = false; stream.clear();
}
long UPV_CB packet(void*, unsigned long, unsigned long, const void*, unsigned long, long) {
  ++callbacks;
  return 0;
}
bool service(void*, bool force) {
  if (force) ++forced_flushes;
  return false;
}
}

extern "C" {
int LIBUSB_CALL libusb_init(libusb_context** c) { *c = reinterpret_cast<libusb_context*>(1); return 0; }
void LIBUSB_CALL libusb_exit(libusb_context*) {}
ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context*, libusb_device*** list) { *list = devices; return 1; }
void LIBUSB_CALL libusb_free_device_list(libusb_device**, int) {}
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device*) { return 1; }
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device*) { return 2; }
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device*, libusb_device_descriptor* d) {
  *d = {}; d->idVendor = 0x16c0; d->idProduct = 0x05dc; d->bcdDevice = 0x1701;
  d->iManufacturer = 1; d->iSerialNumber = 2; return 0;
}
int LIBUSB_CALL libusb_open(libusb_device*, libusb_device_handle** handle) {
  *handle = reinterpret_cast<libusb_device_handle*>(1); return 0;
}
void LIBUSB_CALL libusb_close(libusb_device_handle*) { assert(pending.empty()); }
int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device*, libusb_config_descriptor** out) {
  endpoints[0].bEndpointAddress = 0x81; endpoints[1].bEndpointAddress = 1;
  for (auto& ep : endpoints) { ep.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK; ep.wMaxPacketSize = monitor_packet_size; }
  alt.bNumEndpoints = 2; alt.endpoint = endpoints;
  iface.num_altsetting = 1; iface.altsetting = &alt;
  config.bNumInterfaces = 1; config.interface = &iface; *out = &config; return 0;
}
void LIBUSB_CALL libusb_free_config_descriptor(libusb_config_descriptor*) {}
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle*, int) {
  if (claims) return LIBUSB_ERROR_BUSY;
  ++claims; return 0;
}
int LIBUSB_CALL libusb_release_interface(libusb_device_handle*, int) { --claims; return 0; }
int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle*, int, int) { return 0; }
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle*, uint8_t, uint8_t request,
                                       uint16_t value, uint16_t, unsigned char* data,
                                       uint16_t size, unsigned int) {
  if (request == LIBUSB_REQUEST_GET_DESCRIPTOR) {
    const char* text = (value & 255) == 1 ? "tusb.org" : "TEST";
    const auto n = std::strlen(text);
    assert(size >= 2 + 2 * n);
    data[0] = static_cast<unsigned char>(2 + 2 * n); data[1] = LIBUSB_DT_STRING;
    for (std::size_t i = 0; i < n; ++i) { data[2 + i * 2] = text[i]; data[3 + i * 2] = 0; }
    return data[0];
  }
  if (size) { std::memset(data, 0, size); data[0] = 3; }
  return size;
}
int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle*, unsigned char endpoint,
                                     unsigned char* data, int length, int* n, unsigned int) {
  *n = length;
  if (endpoint == 0x81) {
    if (length > 4) { *n = 0; return LIBUSB_ERROR_TIMEOUT; }
    std::memcpy(data, response.data(), length); return 0;
  }
  if (length == 4 && data[0] == 0x55) {
    std::copy_n(data, 4, response.begin());
    if (data[1] == 1 && data[2] == 1) {
      streaming = true;
      if (fault != Fault::NoStart) stream.assign(data, data + 4);
      if (fault == Fault::Malformed) stream.push_back(0xee);
      else if (fault != Fault::NoStart) {
        const unsigned char record[] = {0x60, 1, 0, 0, 1, 0, 0x5a, 0};
        stream.insert(stream.end(), std::begin(record), std::end(record));
      }
    } else if (data[1] == 1 && data[2] == 0 && fault != Fault::NoStop) {
      stream.insert(stream.end(), data, data + 4);
    }
  }
  return 0;
}
libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int) {
  ++allocations;
  return static_cast<libusb_transfer*>(std::calloc(1, sizeof(libusb_transfer)));
}
void LIBUSB_CALL libusb_free_transfer(libusb_transfer* t) {
  assert(std::find(pending.begin(), pending.end(), t) == pending.end());
  --allocations; std::free(t);
}
unsigned char* LIBUSB_CALL libusb_dev_mem_alloc(libusb_device_handle*, size_t size) {
  if (!dma_enabled) return nullptr;
  ++dma_allocations; return static_cast<unsigned char*>(std::malloc(size));
}
int LIBUSB_CALL libusb_dev_mem_free(libusb_device_handle*, unsigned char* data, size_t) {
  assert(pending.empty()); --dma_allocations; std::free(data); return 0;
}
int LIBUSB_CALL libusb_submit_transfer(libusb_transfer* t) {
  if (++submit_count == fail_submit) return LIBUSB_ERROR_IO;
  t->status = LIBUSB_TRANSFER_COMPLETED; pending.push_back(t); return 0;
}
int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer* t) { t->status = LIBUSB_TRANSFER_CANCELLED; return 0; }
int LIBUSB_CALL libusb_handle_events_timeout(libusb_context*, timeval*) {
  std::vector<libusb_transfer*> completed;
  for (int i = 0; i < 2 && !pending.empty(); ++i) {
    auto* t = pending.front();
    bool complete = t->status == LIBUSB_TRANSFER_CANCELLED;
    t->actual_length = 0;
    if (!complete && streaming && fault == Fault::Completion) {
      t->status = LIBUSB_TRANSFER_NO_DEVICE; complete = true;
    }
    if (!complete && !stream.empty()) {
      // Split every record, including start/stop markers, across reads.
      t->actual_length = std::min<int>(3, stream.size());
      std::copy_n(stream.begin(), t->actual_length, t->buffer);
      stream.erase(stream.begin(), stream.begin() + t->actual_length); complete = true;
    }
    if (complete) { pending.pop_front(); completed.push_back(t); }
    else break;
  }
  // Completion callbacks can be dispatched in a different order; the stream
  // still belongs to the order in which the reads were submitted.
  for (auto p = completed.rbegin(); p != completed.rend(); ++p) (*p)->callback(*p);
  if (!completed.empty()) return 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return 0;
}
const char* LIBUSB_CALL libusb_error_name(int) { return "injected libusb error"; }
}

int main() {
  const char options[] = {'T','E','S','T',0, 0,0,0, 0,0,0,0,0,0,0,0};
  using namespace usbpv::native;
  reset();
  dma_enabled = false;
  assert(std::strcmp(list_devices(), "TEST") == 0);
  auto* handle = open_device_batched(options, sizeof(options), nullptr, packet, nullptr, service);
  assert(handle && !failed(handle) && get_monitor_speed(handle) == 0);
  assert(close_device(handle) == 0);
  assert(callbacks == 1 && forced_flushes > 0);
  dma_enabled = true;
  monitor_packet_size = 1024;
  reset();
  handle = open_device(options, sizeof(options), nullptr, packet);
  assert(handle && get_monitor_speed(handle) == 1 && close_device(handle) == 0);
  for (int at : {1, 5, 32, 33}) {
    reset(Fault::Submit, at);
    handle = open_device(options, sizeof(options), nullptr, packet);
    if (handle) assert(close_device(handle) == -1);
    else assert(get_last_error() == -1);
  }
  for (auto f : {Fault::Completion, Fault::Malformed, Fault::NoStart}) {
    reset(f);
    handle = open_device(options, sizeof(options), nullptr, packet);
    assert(!handle && get_last_error() == -1);
  }
  reset(Fault::NoStop);
  handle = open_device(options, sizeof(options), nullptr, packet);
  assert(handle && close_device(handle) == -1);
  reset();
  assert(!open_device(options, sizeof(options) - 1, nullptr, packet));
  assert(!open_device(options, sizeof(options), nullptr, nullptr));
  assert(pending.empty() && allocations == 0 && claims == 0 && dma_allocations == 0);
}
