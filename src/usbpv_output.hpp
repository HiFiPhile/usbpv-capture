#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace usbpv {

// One producer encodes records; one worker owns the blocking sink. A published
// buffer remains owned by the worker until its write returns. Exhaustion fails
// explicitly instead of blocking the capture queue or allocating more memory.
class BufferedOutput {
 public:
  using Write = std::function<bool(const char*, std::size_t)>;
  using Flush = std::function<bool()>;
  static constexpr std::size_t block_bytes = 1024 * 1024;
  static constexpr std::size_t block_count = 32;

  explicit BufferedOutput(std::size_t bytes = block_bytes,
                          std::size_t count = block_count);
  ~BufferedOutput();
  BufferedOutput(const BufferedOutput&) = delete;
  BufferedOutput& operator=(const BufferedOutput&) = delete;

  bool start(Write write, Flush flush);
  // The pointer is producer-owned until the next reserve/flush/finish call.
  // A record must fit one block; reservations never split a record.
  char* reserve(std::size_t bytes);
  bool flush();   // Asynchronous, ordered after all records submitted so far.
  bool finish();  // Publish the tail, drain, flush, and join before releasing storage.
  bool failed() const { return failed_.load(std::memory_order_acquire); }
  std::string error() const;
  std::uint64_t peak_pending_bytes() const;
  std::size_t capacity_bytes() const { return storage_.size(); }

 private:
  bool publish();
  void fail(const std::string& message);
  void run() noexcept;
  const std::size_t bytes_, count_;
  std::vector<char> storage_;
  std::vector<std::size_t> sizes_;
  Write write_;
  Flush flush_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::atomic<bool> failed_{false};
  std::string error_;
  std::size_t producer_ = 0, used_ = 0;
  bool owns_active_ = false, finished_ = false;
  std::size_t consumer_ = 0, pending_ = 0;
  std::uint64_t published_ = 0, completed_ = 0, flush_target_ = 0, flushed_ = 0;
  std::uint64_t pending_bytes_ = 0, peak_bytes_ = 0;
  bool stopping_ = false;
};

// Encode one speed-specific raw-USB Enhanced Packet Block, without options.
// Caller supplies 32 + round_up(length, 4) bytes.
std::size_t packet_block_size(std::size_t length);
void encode_packet_block(char* output, std::uint32_t seconds,
                         std::uint32_t nanoseconds, const void* payload,
                         std::size_t length);

}  // namespace usbpv
