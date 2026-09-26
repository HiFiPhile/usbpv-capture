#pragma once

#include "usbpv_lib.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace usbpv {

std::array<std::uint8_t, 4> register_command(std::uint8_t address,
                                          std::uint8_t value, bool read = false);
std::array<std::uint8_t, 8> filter_registers(
    const std::array<std::uint8_t, 8>& pairs, bool accept);

// One owner feeds the stream and invokes the callback. No shared packet list,
// unbounded allocation, or assumptions about USB transfer/record boundaries.
class ProtocolParser {
 public:
  ProtocolParser(void* context, pfn_packet_handler callback)
      : context_(context), callback_(callback) {}
  bool feed(const std::uint8_t* data, std::size_t size, std::uint64_t host_ns);
  bool started() const { return started_; }
  bool stopped() const { return stopped_; }
  const std::string& error() const { return error_; }

 private:
  bool fail(const std::string& error);
  void emit(std::uint32_t ticks, const void* data, std::size_t size,
            long status, std::uint64_t host_ns);
  void* context_;
  pfn_packet_handler callback_;
  std::array<std::uint8_t, 1056> record_{};
  std::size_t used_ = 0;
  std::size_t needed_ = 1;
  std::uint32_t marker_ = 0;
  std::size_t startup_bytes_ = 0;
  bool started_ = false;
  bool stopped_ = false;
  bool have_time_ = false;
  std::uint32_t last_ticks_ = 0;
  std::uint64_t extended_ticks_ = 0;
  std::uint64_t epoch_ns_ = 0;
  std::uint64_t last_host_ns_ = 0;
  std::uint64_t last_ns_ = 0;
  std::string error_;
};
}  // namespace usbpv
