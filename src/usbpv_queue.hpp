#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace usbpv {
constexpr std::size_t kMaxPacketBytes = 2048;

struct PacketSlot {
  std::uint32_t seconds = 0;
  std::uint32_t nanoseconds = 0;
  std::int32_t status = 0;
  std::uint16_t length = 0;
  std::array<std::uint8_t, kMaxPacketBytes> data{};
};

class PacketQueue {
 public:
  explicit PacketQueue(std::size_t capacity) : slots_(capacity) {}

  bool push(std::uint32_t seconds, std::uint32_t nanoseconds,
            const void* data, std::size_t length, std::int32_t status) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopped_ || count_ == slots_.size()) return false;
    const bool notify = count_ == 0;
    PacketSlot& slot = slots_[head_];
    slot.seconds = seconds;
    slot.nanoseconds = nanoseconds;
    slot.status = status;
    slot.length = static_cast<std::uint16_t>(length);
    if (length) std::memcpy(slot.data.data(), data, length);
    if (++head_ == slots_.size()) head_ = 0;
    ++count_;
    lock.unlock();
    if (notify) cv_.notify_one();
    return true;
  }

  // Packets already have validated payload lengths. Accept a prefix,
  // preserving the same bounded/drop-new behavior as push.
  std::size_t push_batch(const PacketSlot* packets, std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopped_) return 0;
    const bool notify = count_ == 0;
    const auto amount = std::min(count, slots_.size() - count_);
    for (std::size_t i = 0; i < amount; ++i) {
      const auto& source = packets[i];
      auto& target = slots_[head_];
      target.seconds = source.seconds;
      target.nanoseconds = source.nanoseconds;
      target.status = source.status;
      target.length = source.length;
      if (source.length) std::memcpy(target.data.data(), source.data.data(), source.length);
      if (++head_ == slots_.size()) head_ = 0;
    }
    count_ += amount;
    lock.unlock();
    if (notify && amount) cv_.notify_one();
    return amount;
  }

  std::size_t pop_batch(std::vector<PacketSlot>& batch, std::size_t maximum) {
    // Allocate/initialize the reusable batch once, outside the queue lock.
    if (batch.size() < maximum) batch.resize(maximum);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(100),
                 [&] { return count_ != 0 || stopped_; });
    const std::size_t amount = std::min(count_, maximum);
    for (std::size_t i = 0; i < amount; ++i) {
      const PacketSlot& source = slots_[tail_];
      PacketSlot& target = batch[i];
      target.seconds = source.seconds;
      target.nanoseconds = source.nanoseconds;
      target.status = source.status;
      target.length = source.length;
      if (source.length) std::memcpy(target.data.data(), source.data.data(), source.length);
      if (++tail_ == slots_.size()) tail_ = 0;
    }
    count_ -= amount;
    return amount;
  }

  void stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cv_.notify_all();
  }

  bool drained() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_ && count_ == 0;
  }

 private:
  std::vector<PacketSlot> slots_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  std::size_t count_ = 0;
  bool stopped_ = false;
};

}  // namespace usbpv
