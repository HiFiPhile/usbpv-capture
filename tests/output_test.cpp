#include "usbpv_output.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* what) {
  if (!ok) { std::cerr << what << '\n'; std::exit(1); }
}
void append32(std::vector<char>& bytes, std::uint32_t n) {
  for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>(n >> shift));
}
void encoding() {
  // Independent reference encoding matching the previous field-by-field writer.
  // Cover all padding cases, maximum packet size, and the timestamp's high word.
  for (std::size_t length = 0; length <= 2048; ++length) {
    std::vector<char> payload(length);
    for (std::size_t i = 0; i < length; ++i) payload[i] = static_cast<char>(i * 17 + length);
    const std::uint32_t seconds = 1780000000, nanos = 999999983;
    const std::uint64_t stamp = seconds * 1000000000ULL + nanos;
    const auto total = static_cast<std::uint32_t>(32 + (length + 3) / 4 * 4);
    std::vector<char> expected;
    append32(expected, 6); append32(expected, total); append32(expected, 0);
    append32(expected, static_cast<std::uint32_t>(stamp >> 32)); append32(expected, static_cast<std::uint32_t>(stamp));
    append32(expected, static_cast<std::uint32_t>(length)); append32(expected, static_cast<std::uint32_t>(length));
    expected.insert(expected.end(), payload.begin(), payload.end());
    while (expected.size() % 4) expected.push_back(0);
    append32(expected, total);
    std::vector<char> actual(total + 2, 'X');
    require(usbpv::packet_block_size(length) == total, "encoded block size");
    usbpv::encode_packet_block(actual.data() + 1, seconds, nanos, payload.data(), length);
    require(actual.front() == 'X' && actual.back() == 'X', "encoder exceeded reservation");
    require(std::equal(expected.begin(), expected.end(), actual.begin() + 1), "pcapng bytes changed");
  }
}
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, released = false;
  void block() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true; cv.notify_all();
    cv.wait(lock, [&] { return released; });
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    require(cv.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }), "sink never started");
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true; cv.notify_all();
  }
};
void stalled_output() {
  Gate gate;
  usbpv::BufferedOutput out;
  std::vector<char> received;
  const auto block = usbpv::BufferedOutput::block_bytes;
  received.reserve(block * 16 + 37);
  bool first = true;
  unsigned flushes = 0;
  require(out.start([&](const char* bytes, std::size_t size) {
    if (first) { first = false; gate.block(); }
    received.insert(received.end(), bytes, bytes + size);
    return true;
  }, [&] { ++flushes; return true; }), "start stalled output");
  auto* p = out.reserve(block); require(p != nullptr, "first block"); std::memset(p, 0, block);
  require(out.flush(), "publish first block");
  gate.wait();
  // Sink cannot release any block while the producer fills another 15 MiB.
  // This proves continued producer progress and stable in-flight ownership.
  for (unsigned i = 1; i < 16; ++i) {
    p = out.reserve(block); require(p != nullptr, "producer blocked by stalled sink");
    std::memset(p, static_cast<int>(i), block);
  }
  p = out.reserve(37); require(p != nullptr, "partial tail"); std::memset(p, 99, 37);
  require(out.flush(), "asynchronous flush during stall");
  require(out.peak_pending_bytes() >= 16 * block, "pending byte accounting");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  gate.release();
  require(out.finish(), "drain after 150 ms controlled stall");
  require(out.finish(), "finish is idempotent");
  require(received.size() == 16 * block + 37, "missing final partial block");
  for (unsigned i = 0; i < 16; ++i)
    require(std::all_of(received.begin() + i * block, received.begin() + (i + 1) * block,
                        [i](char value) { return value == static_cast<char>(i); }), "in-flight block overwritten or reordered");
  require(std::all_of(received.end() - 37, received.end(), [](char value) { return value == 99; }), "tail changed");
  require(flushes > 0, "sink was not flushed");
}
void exhaustion() {
  Gate gate;
  usbpv::BufferedOutput out(128, 2);
  std::vector<char> received;
  bool first = true;
  require(out.start([&](const char* data, std::size_t size) {
    if (first) { first = false; gate.block(); }
    received.insert(received.end(), data, data + size); return true;
  }, [] { return true; }), "start small pool");
  std::memset(out.reserve(128), 1, 128); require(out.flush(), "first small block"); gate.wait();
  std::memset(out.reserve(128), 2, 128); require(out.flush(), "second small block");
  require(out.reserve(1) == nullptr && out.failed(), "full pool must fail without waiting");
  require(out.error().find("exhausted") != std::string::npos, "missing exhaustion diagnostic");
  require(out.peak_pending_bytes() == 256 && out.capacity_bytes() == 256, "pool exceeded bound");
  gate.release();
  require(!out.finish(), "exhaustion must remain a failure");
  require(received.size() == 256 && received[0] == 1 && received[128] == 2, "queued prefix lost or overwritten");
}
void wrap_and_flush() {
  usbpv::BufferedOutput out(64, 3);
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<char> received;
  unsigned flushes = 0;
  require(out.start([&](const char* p, std::size_t n) { received.insert(received.end(), p, p + n); return true; },
      [&] { std::lock_guard<std::mutex> lock(mutex); ++flushes; cv.notify_all(); return true; }), "start wrap test");
  for (unsigned i = 0; i < 100; ++i) {
    std::unique_lock<std::mutex> lock(mutex);
    const auto before = flushes;
    char* p = out.reserve(37); require(p != nullptr, "reserve after pool reuse");
    std::memset(p, static_cast<int>(i), 37);
    require(out.flush(), "ordered async flush");
    require(cv.wait_for(lock, std::chrono::seconds(3), [&] { return flushes > before; }), "flush completion missing");
  }
  require(out.finish(), "wrap finish");
  require(received.size() == 3700, "wrap output size");
  for (std::size_t i = 0; i < received.size(); ++i) require(received[i] == static_cast<char>(i / 37), "wrap order/data");
}
void failures() {
  for (unsigned kind = 0; kind < 3; ++kind) {
    usbpv::BufferedOutput out(64, 2);
    require(out.start([&](const char*, std::size_t) {
      if (kind == 2) throw std::runtime_error("injected failure");
      return kind != 0;
    }, [&] { return kind != 1; }), "start failing sink");
    auto* p = out.reserve(10); require(p != nullptr, "failure reservation"); std::memset(p, 0, 10);
    require(!out.finish() && out.failed() && !out.error().empty(), "write/flush/exception must fail capture");
    require(out.reserve(1) == nullptr, "failed sink accepted output");
  }
  bool rejected = false;
  try { usbpv::BufferedOutput invalid(0, 1); } catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "invalid pool dimensions");
}
}  // namespace
int main() {
  encoding(); stalled_output(); exhaustion(); wrap_and_flush(); failures();
  std::cout << "output tests passed (150 ms stall, bounded exhaustion, exact pcapng encoding)\n";
}
