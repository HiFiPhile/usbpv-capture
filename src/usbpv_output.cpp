#include "usbpv_output.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace usbpv {
namespace {
std::size_t storage_size(std::size_t bytes, std::size_t count) {
  if (!bytes || !count || count > std::numeric_limits<std::size_t>::max() / bytes)
    throw std::invalid_argument("invalid output buffer capacity");
  return bytes * count;
}
void le32(char* p, std::uint32_t n) {
  p[0] = static_cast<char>(n);
  p[1] = static_cast<char>(n >> 8);
  p[2] = static_cast<char>(n >> 16);
  p[3] = static_cast<char>(n >> 24);
}
}  // namespace

std::size_t packet_block_size(std::size_t length) { return 32 + ((length + 3) & ~std::size_t(3)); }
void encode_packet_block(char* out, std::uint32_t seconds,
                         std::uint32_t nanoseconds, const void* payload,
                         std::size_t length) {
  const auto total = static_cast<std::uint32_t>(packet_block_size(length));
  const std::uint64_t stamp = seconds * 1000000000ULL + nanoseconds;
  le32(out, 6); le32(out + 4, total); le32(out + 8, 0);
  le32(out + 12, static_cast<std::uint32_t>(stamp >> 32));
  le32(out + 16, static_cast<std::uint32_t>(stamp));
  le32(out + 20, static_cast<std::uint32_t>(length));
  le32(out + 24, static_cast<std::uint32_t>(length));
  if (length) std::memcpy(out + 28, payload, length);
  std::memset(out + 28 + length, 0, total - 32 - length);
  le32(out + total - 4, total);
}

BufferedOutput::BufferedOutput(std::size_t bytes, std::size_t count)
    : bytes_(bytes), count_(count), storage_(storage_size(bytes, count)), sizes_(count) {}
BufferedOutput::~BufferedOutput() { finish(); }

void BufferedOutput::fail(const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_.empty()) error_ = message;
  }
  failed_.store(true, std::memory_order_release);
  ready_.notify_all();
}
std::string BufferedOutput::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}
std::uint64_t BufferedOutput::peak_pending_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peak_bytes_;
}
bool BufferedOutput::start(Write write, Flush flush) {
  if (worker_.joinable() || finished_ || !write || !flush) return false;
  write_ = std::move(write); flush_ = std::move(flush);
  try { worker_ = std::thread(&BufferedOutput::run, this); }
  catch (const std::exception& ex) { fail(ex.what()); return false; }
  return true;
}
char* BufferedOutput::reserve(std::size_t bytes) {
  if (failed() || finished_ || !worker_.joinable()) return nullptr;
  if (bytes > bytes_) { fail("record exceeds output block size"); return nullptr; }
  if (used_ + bytes > bytes_ && !publish()) return nullptr;
  if (!owns_active_) {
    bool full;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      full = pending_ == count_;
      if (!full) owns_active_ = true;
    }
    if (full) { fail("output buffer exhausted while disk output was stalled"); return nullptr; }
  }
  char* result = storage_.data() + producer_ * bytes_ + used_;
  used_ += bytes;
  return result;
}
bool BufferedOutput::publish() {
  if (!used_) return !failed();
  bool notify;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed()) return false;
    sizes_[producer_] = used_;
    notify = pending_ == 0;
    ++pending_; ++published_;
    pending_bytes_ += used_;
    peak_bytes_ = std::max(peak_bytes_, pending_bytes_);
    producer_ = (producer_ + 1) % count_;
    used_ = 0; owns_active_ = false;
  }
  if (notify) ready_.notify_one();
  return true;
}
bool BufferedOutput::flush() {
  if (failed() || finished_ || !worker_.joinable() || !publish()) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    flush_target_ = published_;
  }
  ready_.notify_one();
  return !failed();
}
bool BufferedOutput::finish() {
  if (finished_) return !failed();
  if (worker_.joinable()) {
    if (!failed()) publish();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_one();
    worker_.join();
  }
  finished_ = true;
  return !failed();
}
void BufferedOutput::run() noexcept {
  try {
    while (true) {
      std::size_t index = 0, bytes = 0;
      std::uint64_t flush_to = 0;
      bool have_block = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] {
          return pending_ || stopping_ ||
                 (flush_target_ > flushed_ && completed_ >= flush_target_);
        });
        if (pending_) {
          index = consumer_; bytes = sizes_[index]; have_block = true;
        } else if (stopping_) break;
        else flush_to = flush_target_;
      }
      if (have_block) {
        if (!write_(storage_.data() + index * bytes_, bytes)) {
          fail("output file write failed"); return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pending_bytes_ -= bytes;
        --pending_; ++completed_;
        consumer_ = (consumer_ + 1) % count_;
        if (flush_target_ > flushed_ && completed_ >= flush_target_) flush_to = flush_target_;
      }
      if (flush_to) {
        if (!flush_()) { fail("output file flush failed"); return; }
        std::lock_guard<std::mutex> lock(mutex_);
        flushed_ = flush_to;
      }
    }
    if (!flush_()) fail("final output file flush failed");
  } catch (const std::exception& ex) { fail(std::string("output worker: ") + ex.what()); }
  catch (...) { fail("unexpected output worker exception"); }
}
}  // namespace usbpv
