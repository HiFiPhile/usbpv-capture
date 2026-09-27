#include "usbpv_capture_queue.hpp"
#include <cstdlib>
#include <iostream>

namespace {
void require(bool ok, const char* message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
void native_batches() {
  usbpv::CaptureContext context(300, UPV_Cap_Speed_High);
  const unsigned char packet[] = {0x5a};
  auto data = [&](unsigned seq) { usbpv::packet_callback(&context, 10, seq, packet, 1, UPV_SPD_HIGH); };
  data(0); // Startup flushing must not enter the queue.
  usbpv::flush_callback_batch(&context);
  require(context.counters.flushed == 1 && context.counters.data_packets == 0, "startup flush");
  context.accepting = true;
  for (unsigned i = 0; i < 255; ++i) data(i);
  require(context.counters.data_packets == 0 && context.pending_count == 255, "early publish");
  data(255);
  require(context.counters.data_packets == 256 && context.pending_count == 0, "full batch publish");
  data(256);
  usbpv::packet_callback(&context, 10, 257, nullptr, 0, (UPV_OVERFLOW << 4) | UPV_SPD_HIGH);
  // Stop acceptance between the last callback and the transfer-end hook.
  context.accepting = false;
  data(258);
  usbpv::flush_callback_batch(&context);
  usbpv::flush_callback_batch(&context); // Empty flush cannot double-count.
  require(context.counters.data_packets == 257 && context.counters.data_bytes == 257,
          "partial batch packet/byte counters");
  require(context.counters.bus_events == 1 && context.counters.device_overflows == 1, "event counters");
  require(context.counters.flushed == 2 && context.counters.callbacks == 260, "callback accounting");
  require(context.counters.last_callback_ns > 0, "idle timestamp was not updated");
  context.queue.stop();
  std::vector<usbpv::PacketSlot> output;
  require(context.queue.pop_batch(output, 300) == 258, "partial batch lost on stop");
  for (unsigned i = 0; i < 257; ++i)
    require(output[i].seconds == 10 && output[i].nanoseconds == i && output[i].length == 1 &&
            output[i].data[0] == packet[0], "batch record changed");
  require(output[257].length == 0 && GetPacketType(output[257].status) == UPV_OVERFLOW, "event order");
}
void errors_and_full_queue() {
  usbpv::CaptureContext context(3, UPV_Cap_Speed_High);
  context.accepting = true;
  const unsigned char packet[] = {0x5a};
  usbpv::packet_callback(&context, 0, 0, nullptr, 1, UPV_SPD_HIGH);
  usbpv::packet_callback(&context, 0, 0, packet, 0, UPV_SPD_HIGH);
  usbpv::packet_callback(&context, 0, 0, packet, 2049, UPV_SPD_HIGH);
  usbpv::packet_callback(&context, 0, 0, packet, 1, UPV_SPD_HIGH | 0x100);
  usbpv::packet_callback(&context, 0, 0, packet, 1, UPV_SPD_FULL);
  for (unsigned i = 0; i < 7; ++i) usbpv::packet_callback(&context, 0, i, packet, 1, UPV_SPD_HIGH);
  usbpv::flush_callback_batch(&context);
  require(context.counters.invalid_dropped == 4 && context.counters.speed_mismatches == 1, "invalid counters");
  require(context.counters.queue_dropped == 4 && context.counters.data_packets == 3 &&
          context.counters.data_bytes == 3 && context.counters.callbacks == 12, "bounded batch drop counters");
  std::vector<usbpv::PacketSlot> output;
  require(context.queue.pop_batch(output, 8) == 3 && output[0].nanoseconds == 0 && output[2].nanoseconds == 2,
          "full queue must preserve accepted prefix");
}
void callbacks_without_packets() {
  usbpv::CaptureContext context(3, UPV_Cap_Speed_High);
  for (unsigned i = 0; i < 1000; ++i)
    usbpv::packet_callback(&context, 0, 0, nullptr, 0, UPV_SPD_HIGH);
  require(context.counters.callbacks == 0 && context.pending_callbacks == 1000, "native count was not batched");
  usbpv::flush_callback_batch(&context);
  require(context.counters.callbacks == 1000 && context.counters.flushed == 1000 &&
          context.pending_callbacks == 0 && context.pending_count == 0, "empty startup batch count lost");
  context.accepting = true;
  usbpv::packet_callback(&context, 0, 0, nullptr, 0, UPV_SPD_HIGH);
  usbpv::flush_callback_batch(&context);
  usbpv::flush_callback_batch(&context);
  require(context.counters.callbacks == 1001 && context.counters.invalid_dropped == 1 &&
          context.counters.data_packets == 0, "invalid-only batch count lost or duplicated");
}
void timed_publication() {
  usbpv::CaptureContext context(1024, UPV_Cap_Speed_High);
  context.accepting = true;
  const unsigned char packet[] = {0x5a};
  auto data = [&](unsigned sequence) {
    usbpv::packet_callback(&context, 0, sequence, packet, 1, UPV_SPD_HIGH);
  };
  data(1);
  require(usbpv::service_callback_batch_at(context, false, 10000000), "first partial batch not pending");
  require(context.counters.callbacks == 1 && context.counters.last_callback_ns == 10000000 &&
          context.counters.data_packets == 0 && context.batch_deadline_ns == 11000000, "deferred activity/deadline");
  data(2);
  require(usbpv::service_callback_batch_at(context, false, 10999999), "batch published before deadline");
  require(context.batch_deadline_ns == 11000000 && context.counters.callbacks == 2,
          "active traffic must not postpone deadline");
  require(!usbpv::service_callback_batch_at(context, false, 11000000), "deadline did not publish");
  require(context.counters.data_packets == 2 && context.batch_deadline_ns == 0 &&
          context.counters.last_callback_ns == 10999999, "timer publication changed activity time");
  data(3);
  require(usbpv::service_callback_batch_at(context, false, 12000000), "new partial batch");
  context.accepting = false;
  require(!usbpv::service_callback_batch_at(context, true, 12000001), "forced stop/idle publication");
  require(context.counters.data_packets == 3 && context.counters.callbacks == 3 &&
          context.counters.last_callback_ns == 12000000, "forced publication accounting");
  require(!usbpv::service_callback_batch_at(context, true, 13000000), "empty forced publication");
  context.accepting = true;
  data(4);
  require(usbpv::service_callback_batch_at(context, false, 14000000), "full batch initial tail");
  for (unsigned i = 5; i < 260; ++i) data(i);
  require(context.pending_count == 0 && context.batch_deadline_ns == 0 &&
          context.counters.data_packets == 259, "full batch must publish before timer");
  context.queue.stop();
  std::vector<usbpv::PacketSlot> output;
  require(context.queue.pop_batch(output, 1024) == 259, "timed publication lost records");
  for (unsigned i = 0; i < 259; ++i) require(output[i].nanoseconds == i + 1, "timed publication reordered records");
}
}  // namespace
int main() {
  native_batches(); errors_and_full_queue(); callbacks_without_packets(); timed_publication();
  std::cout << "callback tests passed (partial batches, stop, counters, errors)\n";
}
