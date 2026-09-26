#include "usbpv_protocol.hpp"
#include <algorithm>
#include <cstring>

namespace usbpv {
std::array<std::uint8_t, 4> register_command(std::uint8_t address,
                                          std::uint8_t value, bool read) {
  const auto command = static_cast<std::uint8_t>(address | (read ? 0x80 : 0));
  if (read) value = 0;
  return {{0x55, command, value, static_cast<std::uint8_t>(0x55 + command + value)}};
}

std::array<std::uint8_t, 8> filter_registers(
    const std::array<std::uint8_t, 8>& pairs, bool accept) {
  std::array<std::uint8_t, 8> result{};
  bool enabled = false;
  for (std::size_t i = 0; i < pairs.size(); i += 2) {
    if (pairs[i] <= 127) result[i] = 0x80 | pairs[i];
    if (pairs[i + 1] <= 15) result[i + 1] = 0x80 | pairs[i + 1];
    if (result[i] || result[i + 1]) {
      result[i + 1] |= 0x40;
      enabled = true;
    }
  }
  if (enabled && accept) {
    for (std::size_t i = 1; i < result.size(); i += 2) result[i] |= 0x20;
  } else if (!enabled && !accept) {
    result[1] = 0x20;
  }
  return result;
}

bool ProtocolParser::fail(const std::string& error) {
  error_ = error;
  return false;
}

void ProtocolParser::emit(std::uint32_t ticks, const void* data, std::size_t size,
                          long status, std::uint64_t host_ns) {
  // The wire clock is 24 bits at 60 MHz. After an idle gap longer than a
  // counter period the number of wraps is unknowable; re-anchor to host time,
  // as the vendor does. Within continuous traffic all timing is device based.
  if (!have_time_ || (host_ns > last_host_ns_ &&
                     host_ns - last_host_ns_ > 279620267ULL)) {
    epoch_ns_ = std::max(host_ns, last_ns_);
    extended_ticks_ = 0;
    have_time_ = true;
  } else {
    extended_ticks_ += (ticks - last_ticks_) & 0xffffffU;
  }
  last_ticks_ = ticks;
  last_host_ns_ = host_ns;
  last_ns_ = epoch_ns_ + (extended_ticks_ * 50 + 2) / 3;
  callback_(context_, static_cast<unsigned long>(last_ns_ / 1000000000ULL),
            static_cast<unsigned long>(last_ns_ % 1000000000ULL), data,
            static_cast<unsigned long>(size), status);
}

void ProtocolParser::emit_data(const std::uint8_t* record, std::size_t length,
                              std::uint64_t host_ns) {
  const auto tag = record[0];
  const auto ticks = record[1] | (record[2] << 8) | (record[3] << 16);
  const long speed[] = {UPV_SPD_HIGH, UPV_SPD_FULL, UPV_SPD_LOW, UPV_SPD_Unknown};
  emit(ticks, record + 6, length, speed[tag & 3] | ((tag << 6) & 0x300), host_ns);
}

bool ProtocolParser::feed(const std::uint8_t* data, std::size_t size,
                          std::uint64_t host_ns) {
  if (!error_.empty()) return false;
  while (size && !stopped_) {
    if (!started_) {
      marker_ = (marker_ << 8) | *data++;
      --size;
      if (++startup_bytes_ > 1048576) return fail("capture start marker missing");
      if (marker_ == 0x55010157) started_ = true;
      continue;
    }
    // Most data records fit in the current USB transfer. Decode those directly
    // without the fragment buffer's three incremental copy operations.
    // The callback consumes/copies payload before feed returns. Split records,
    // commands, and bus events keep using the existing incremental decoder.
    if (!used_ && size >= 6 && (data[0] & 0xf0) == 0x60) {
      const std::size_t length = (data[4] | (data[5] << 8)) & 0x3fff;
      if (!length || length > 1050) return fail("invalid stream packet length");
      const std::size_t total = (6 + length + 3) & ~std::size_t(3);
      if (size >= total) {
        emit_data(data, length, host_ns);
        data += total;
        size -= total;
        continue;
      }
    }
    const std::size_t count = std::min(size, needed_ - used_);
    std::memcpy(record_.data() + used_, data, count);
    used_ += count;
    data += count;
    size -= count;
    if (used_ != needed_) continue;
    const auto tag = record_[0];
    if (used_ == 1) {
      if ((tag & 0xf0) == 0x60) needed_ = 6;
      else if (tag == 0x55 || tag == 0xff || (tag & 0xf0) == 0x10 ||
               (tag & 0xf0) == 0x20 || (tag & 0xf0) == 0x30 ||
               (tag & 0xf0) == 0x40) needed_ = 4;
      else return fail("unknown stream tag " + std::to_string(tag));
      continue;
    }
    if ((tag & 0xf0) == 0x60) {
      const std::size_t length = (record_[4] | (record_[5] << 8)) & 0x3fff;
      if (used_ == 6) {
        if (!length || length > 1050) return fail("invalid stream packet length");
        needed_ = (6 + length + 3) & ~std::size_t(3);
        continue;
      }
      emit_data(record_.data(), length, host_ns);
    } else if (tag == 0x55) {
      if (static_cast<std::uint8_t>(tag + record_[1] + record_[2]) != record_[3])
        return fail("invalid stream command checksum");
      if (record_[1] != 1 || record_[2] != 0)
        return fail("unexpected stream command");
      stopped_ = true;
    } else if (tag == 0xff) {
      // Overflow records have no device ticks. Reuse the last packet/event
      // time (host time before the first timed record), without changing any
      // clock state, including the host-gap anchor used for idle recovery.
      const auto ns = have_time_ ? last_ns_ : host_ns;
      callback_(context_, static_cast<unsigned long>(ns / 1000000000ULL),
                static_cast<unsigned long>(ns % 1000000000ULL), record_.data() + 1, 1, 0xf0);
    } else {
      const auto ticks = record_[1] | (record_[2] << 8) | (record_[3] << 16);
      emit(ticks, nullptr, 0, tag & 0xf0, host_ns);
    }
    used_ = 0;
    needed_ = 1;
  }
  return true;
}
}  // namespace usbpv
