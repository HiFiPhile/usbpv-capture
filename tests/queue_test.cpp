#include "usbpv_queue.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
void require(bool ok, const char* message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
void boundaries() {
  usbpv::PacketQueue queue(3);
  std::vector<usbpv::PacketSlot> batch;
  const unsigned char value = 37;
  for (unsigned i = 0; i < 3; ++i) require(queue.push(i, 9, &value, 1, 2), "fill queue");
  require(!queue.push(4, 9, &value, 1, 2), "capacity bound");
  require(queue.pop_batch(batch, 2) == 2 && batch[0].seconds == 0 && batch[1].seconds == 1,
          "partial dequeue order");
  require(queue.push(3, 10, nullptr, 0, 3) && queue.push(4, 10, &value, 1, 3), "wrap queue");
  queue.stop();
  require(!queue.drained() && !queue.push(5, 9, &value, 1, 2), "stop rejects new packets");
  require(queue.pop_batch(batch, 8) == 3, "stop drains tail");
  for (unsigned i = 0; i < 3; ++i) require(batch[i].seconds == i + 2, "wrapped tail order");
  require(batch[1].length == 0 && batch[2].data[0] == value, "event/payload after reuse");
  require(queue.drained() && queue.pop_batch(batch, 8) == 0, "empty stopped queue");
}
void batched_boundaries() {
  usbpv::PacketQueue queue(3);
  std::array<usbpv::PacketSlot, 5> input{};
  for (unsigned i = 0; i < input.size(); ++i) {
    input[i].seconds = i; input[i].length = 1; input[i].data[0] = static_cast<unsigned char>(i + 50);
  }
  require(queue.push_batch(input.data(), 5) == 3, "batch must accept only available prefix");
  require(queue.push_batch(input.data(), 1) == 0, "full batch queue");
  std::vector<usbpv::PacketSlot> got;
  require(queue.pop_batch(got, 2) == 2, "batch partial pop");
  require(queue.push_batch(input.data() + 3, 2) == 2, "batch wraps");
  queue.stop();
  require(queue.push_batch(input.data(), 2) == 0, "batch after stop");
  require(queue.pop_batch(got, 5) == 3, "batch tail after stop");
  for (unsigned i = 0; i < 3; ++i)
    require(got[i].seconds == i + 2 && got[i].data[0] == i + 52, "batched order/payload");
}
void contention(unsigned producers, bool mixed_batches = false) {
  constexpr unsigned packets = 100000;
  usbpv::PacketQueue queue(257); // Exercise non-power-of-two wrapping too.
  std::atomic<bool> start{false};
  std::thread consumer([&] {
    std::vector<usbpv::PacketSlot> batch;
    std::vector<unsigned> next(producers);
    while (true) {
      const auto count = queue.pop_batch(batch, 113);
      for (std::size_t i = 0; i < count; ++i) {
        const auto& p = batch[i];
        require(p.status >= 0 && static_cast<unsigned>(p.status) < producers, "producer ID corrupted");
        require(p.seconds == next[p.status]++, "packet lost, duplicated, or reordered");
        require(p.nanoseconds == p.seconds * 17, "timestamp corrupted");
        const std::size_t lengths[] = {0, 1, 3, 64, 512, 2048};
        require(p.length == lengths[p.seconds % 6], "length corrupted");
        for (std::size_t j = 0; j < p.length; ++j)
          require(p.data[j] == static_cast<unsigned char>(p.seconds + p.status + j), "payload overwritten");
      }
      if (queue.drained()) break;
    }
    for (auto n : next) require(n == packets, "missing final packets");
  });
  std::vector<std::thread> threads;
  for (unsigned id = 0; id < producers; ++id) threads.emplace_back([&, id] {
    std::array<unsigned char, 2048> bytes;
    const std::size_t lengths[] = {0, 1, 3, 64, 512, 2048};
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    if (mixed_batches && id == 0) {
      std::array<usbpv::PacketSlot, 17> pending{};
      for (unsigned base = 0; base < packets; base += 17) {
        const auto count = std::min(17U, packets - base);
        for (unsigned j = 0; j < count; ++j) {
          auto& p = pending[j]; p.seconds = base + j; p.nanoseconds = p.seconds * 17;
          p.status = static_cast<int>(id); p.length = static_cast<std::uint16_t>(lengths[p.seconds % 6]);
          for (std::size_t k = 0; k < p.length; ++k) p.data[k] = static_cast<unsigned char>(p.seconds + id + k);
        }
        std::size_t sent = 0;
        while (sent < count) {
          const auto accepted = queue.push_batch(pending.data() + sent, count - sent);
          sent += accepted;
          if (!accepted) std::this_thread::yield();
        }
      }
      return;
    }
    for (unsigned i = 0; i < packets; ++i) {
      const auto length = lengths[i % 6];
      for (std::size_t j = 0; j < length; ++j) bytes[j] = static_cast<unsigned char>(i + id + j);
      while (!queue.push(i, i * 17, bytes.data(), length, static_cast<int>(id))) std::this_thread::yield();
    }
  });
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();
  queue.stop(); consumer.join();
}
void sleeping_consumer() {
  usbpv::PacketQueue queue(2);
  std::vector<usbpv::PacketSlot> batch;
  const auto start = std::chrono::steady_clock::now();
  require(queue.pop_batch(batch, 2) == 0, "empty wait returned packets");
  require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(50), "wait spins");
  for (unsigned i = 0; i < 20; ++i) {
    std::thread consumer([&] {
      std::vector<usbpv::PacketSlot> got;
      require(queue.pop_batch(got, 1) == 1 && got[0].seconds == i, "sleeping consumer missed packet");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    require(queue.push(i, 0, nullptr, 0, 0), "wake packet");
    consumer.join();
  }
  std::thread consumer([&] {
    std::vector<usbpv::PacketSlot> got;
    require(queue.pop_batch(got, 1) == 0 && queue.drained(), "stop wake");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  queue.stop(); consumer.join();
}
}  // namespace
int main() {
  boundaries(); batched_boundaries(); contention(1); contention(2); contention(2, true); sleeping_consumer();
  std::cout << "queue tests passed (500000 packets, batched/individual contention, wrap, stop, wake)\n";
}
