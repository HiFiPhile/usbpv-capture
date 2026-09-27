#pragma once
#include "usbpv_lib.h"
#include "usbpv_queue.hpp"
#include <atomic>

namespace usbpv {
struct Counters {
  std::atomic<std::uint64_t> callbacks{0};
  std::atomic<std::uint64_t> flushed{0};
  std::atomic<std::uint64_t> data_packets{0};
  std::atomic<std::uint64_t> data_bytes{0};
  std::atomic<std::uint64_t> bus_events{0};
  std::atomic<std::uint64_t> device_overflows{0};
  std::atomic<std::uint64_t> queue_dropped{0};
  std::atomic<std::uint64_t> invalid_dropped{0};
  std::atomic<std::uint64_t> speed_mismatches{0};
  std::atomic<std::uint64_t> output_pending_peak_bytes{0};
  std::atomic<std::int64_t> last_callback_ns{0};
};

inline std::int64_t steady_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct CaptureContext {
  explicit CaptureContext(std::size_t capacity, UPV_CaptureSpeed selected_speed)
      : queue(capacity), expected_packet_speed(selected_speed == UPV_Cap_Speed_High ? UPV_SPD_HIGH
                                      : selected_speed == UPV_Cap_Speed_Full ? UPV_SPD_FULL
                                                                            : UPV_SPD_LOW), pending(256) {}
  PacketQueue queue;
  Counters counters;
  const int expected_packet_speed;
  std::atomic<bool> accepting{false};
  // The native reader is the sole callback owner.
  std::uint64_t pending_callbacks = 0;
  std::int64_t batch_deadline_ns = 0;
  std::vector<PacketSlot> pending;
  std::size_t pending_count = 0;
};

inline void publish_callback_activity(CaptureContext& context, std::int64_t now_ns) {
  if (context.pending_callbacks) {
    context.counters.callbacks.fetch_add(context.pending_callbacks, std::memory_order_relaxed);
    context.counters.last_callback_ns.store(now_ns, std::memory_order_relaxed);
    context.pending_callbacks = 0;
  }
}

inline void flush_callback_batch(void* opaque) {
  auto& context = *static_cast<CaptureContext*>(opaque);
  if (context.pending_callbacks) publish_callback_activity(context, steady_now_ns());
  context.batch_deadline_ns = 0;
  if (!context.pending_count) return;
  const auto accepted = context.queue.push_batch(context.pending.data(), context.pending_count);
  std::uint64_t packets = 0, bytes = 0, events = 0, overflows = 0;
  for (std::size_t i = 0; i < accepted; ++i) {
    const auto& packet = context.pending[i];
    const int type = GetPacketType(packet.status);
    if (type == UPV_DATA_PACKET) { ++packets; bytes += packet.length; }
    else { ++events; if (type == UPV_OVERFLOW) ++overflows; }
  }
  if (packets) context.counters.data_packets.fetch_add(packets, std::memory_order_relaxed);
  if (bytes) context.counters.data_bytes.fetch_add(bytes, std::memory_order_relaxed);
  if (events) context.counters.bus_events.fetch_add(events, std::memory_order_relaxed);
  if (overflows) context.counters.device_overflows.fetch_add(overflows, std::memory_order_relaxed);
  if (accepted < context.pending_count)
    context.counters.queue_dropped.fetch_add(context.pending_count - accepted, std::memory_order_relaxed);
  context.pending_count = 0;
}

inline bool service_callback_batch_at(CaptureContext& context, bool force, std::int64_t now_ns) {
  // Activity tracking remains per transfer even when publication is deferred.
  // Flushes without callbacks must not extend the host idle timeout.
  publish_callback_activity(context, now_ns);
  if (!context.pending_count) { context.batch_deadline_ns = 0; return false; }
  if (force || (context.batch_deadline_ns && now_ns >= context.batch_deadline_ns)) {
    flush_callback_batch(&context);
    return false;
  }
  if (!context.batch_deadline_ns) context.batch_deadline_ns = now_ns + 1000000;
  return true;
}

inline bool service_callback_batch(void* opaque, bool force) {
  auto& context = *static_cast<CaptureContext*>(opaque);
  if (!context.pending_callbacks && !context.pending_count) return false;
  return service_callback_batch_at(context, force, steady_now_ns());
}

inline long UPV_CB packet_callback(void* opaque, unsigned long seconds,
                            unsigned long nanoseconds, const void* data,
                            unsigned long length, long status) {
  auto* context = static_cast<CaptureContext*>(opaque);
  ++context->pending_callbacks;
  if (!context->accepting.load(std::memory_order_acquire)) {
    context->counters.flushed.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const int type = GetPacketType(status);
  if (type == UPV_DATA_PACKET) {
    if ((!data && length != 0) || length == 0 || length > kMaxPacketBytes ||
        (status & 0xf00)) {
      context->counters.invalid_dropped.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
    if (GetPacketSpeed(status) != context->expected_packet_speed) {
      context->counters.speed_mismatches.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
  } else if (length > kMaxPacketBytes) {
    context->counters.invalid_dropped.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  auto& packet = context->pending[context->pending_count++];
  packet.seconds = static_cast<std::uint32_t>(seconds);
  packet.nanoseconds = static_cast<std::uint32_t>(nanoseconds);
  packet.status = static_cast<std::int32_t>(status);
  packet.length = static_cast<std::uint16_t>(length);
  if (length) std::memcpy(packet.data.data(), data, length);
  if (context->pending_count == context->pending.size()) flush_callback_batch(context);
  return 0;
}

}  // namespace usbpv
