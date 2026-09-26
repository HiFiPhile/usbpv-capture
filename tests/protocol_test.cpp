#include "usbpv_protocol.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void require(bool ok, const char* what) {
  if (!ok) { std::cerr << what << '\n'; std::exit(1); }
}
struct Packet {
  std::uint64_t ns;
  long status;
  std::vector<std::uint8_t> bytes;
};
long UPV_CB receive(void* context, unsigned long seconds, unsigned long nanos,
                    const void* bytes, unsigned long size, long status) {
  auto& packets = *static_cast<std::vector<Packet>*>(context);
  Packet packet{seconds * 1000000000ULL + nanos, status, {}};
  if (size) {
    const auto* begin = static_cast<const std::uint8_t*>(bytes);
    packet.bytes.assign(begin, begin + size);
  }
  packets.push_back(packet);
  return 0;
}
void append_packet(std::vector<std::uint8_t>& wire, std::uint8_t tag,
                   std::uint32_t ticks, std::size_t size) {
  const auto begin = wire.size();
  wire.insert(wire.end(), {tag, static_cast<std::uint8_t>(ticks),
      static_cast<std::uint8_t>(ticks >> 8), static_cast<std::uint8_t>(ticks >> 16),
      static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8)});
  for (std::size_t i = 0; i < size; ++i) wire.push_back(static_cast<std::uint8_t>(i));
  // Deliberately use nonzero padding: it is opaque, not a delimiter.
  while ((wire.size() - begin) % 4) wire.push_back(0xa5);
}
void framing() {
  std::vector<std::uint8_t> wire{0x12, 0x55, 0x55, 1, 1, 0x57};
  const std::vector<std::size_t> sizes{1, 2, 3, 4, 27, 1027, 1050};
  for (std::size_t i = 0; i < sizes.size(); ++i)
    append_packet(wire, 0x60, static_cast<std::uint32_t>(100 + i * 7500), sizes[i]);
  wire.insert(wire.end(), {0x1f, 0x10, 0xb0, 0, 0x21, 0x20, 0xb0, 0,
                           0x32, 0x30, 0xb0, 0, 0x40, 0x40, 0xb0, 0,
                           0xff, 7, 0xa5, 0xa5, 0x55, 1, 0, 0x56, 0xde, 0xad});
  // Every possible single split, and fixed-size chunks down to one byte.
  auto verify = [&](const std::vector<Packet>& packets, const usbpv::ProtocolParser& parser) {
    require(parser.started() && parser.stopped(), "start/stop markers");
    require(packets.size() == sizes.size() + 5, "packet/event count");
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      require(packets[i].bytes.size() == sizes[i] && packets[i].status == UPV_SPD_HIGH,
              "packet size/status");
      for (std::size_t j = 0; j < sizes[i]; ++j)
        require(packets[i].bytes[j] == static_cast<std::uint8_t>(j), "payload/padding split");
      require(packets[i].ns == 1000000000ULL + i * 125000, "60MHz packet timestamp");
    }
    for (std::size_t i = 0; i < 4; ++i)
      require(packets[sizes.size() + i].status == static_cast<long>((i + 1) << 4), "bus event type");
    require(packets.back().status == 0xf0 && packets.back().bytes[0] == 7, "overflow event");
  };
  for (std::size_t split = 0; split <= wire.size(); ++split) {
    std::vector<Packet> packets;
    usbpv::ProtocolParser parser(&packets, receive);
    require(parser.feed(wire.data(), split, 1000000000ULL), "first split");
    require(parser.feed(wire.data() + split, wire.size() - split, 1000000000ULL), "second split");
    verify(packets, parser);
  }
  for (std::size_t chunk = 1; chunk <= 65; ++chunk) {
    std::vector<Packet> packets;
    usbpv::ProtocolParser parser(&packets, receive);
    for (std::size_t pos = 0; pos < wire.size(); pos += chunk)
      require(parser.feed(wire.data() + pos, std::min(chunk, wire.size() - pos),
                          1000000000ULL), "chunked framing");
    verify(packets, parser);
  }
}
void timestamps_and_flags() {
  std::vector<Packet> packets;
  usbpv::ProtocolParser parser(&packets, receive);
  std::vector<std::uint8_t> wire{0x55, 1, 1, 0x57};
  append_packet(wire, 0x61, 0xfffffe, 3);
  append_packet(wire, 0x62, 1, 3);
  append_packet(wire, 0x6c, 7501, 3);
  require(parser.feed(wire.data(), wire.size(), 999999990ULL), "wrap stream");
  require(packets[1].ns - packets[0].ns == 50, "24bit wrap across second");
  require(packets[2].ns - packets[1].ns == 125000, "timestamp after wrap");
  require(packets[0].status == UPV_SPD_FULL && packets[1].status == UPV_SPD_LOW &&
          packets[2].status == (UPV_SPD_HIGH | 0x300), "speeds and hardware error flags");
  wire.clear();
  append_packet(wire, 0x60, 10, 1);
  require(parser.feed(wire.data(), wire.size(), 3000000000ULL), "idle gap stream");
  require(packets.back().ns == 3000000000ULL, "idle host reanchor");
}
void malformed() {
  const std::vector<std::vector<std::uint8_t>> bad{
      {0x60, 0, 0, 0, 0, 0}, {0x60, 0, 0, 0, 0x1b, 4},
      {0x60, 0, 0, 0, 0xff, 0xff}, {0x90}, {0x55, 1, 0, 0}, {0x55, 2, 0, 0x57}};
  for (const auto& suffix : bad) {
    std::vector<Packet> packets;
    usbpv::ProtocolParser parser(&packets, receive);
    std::vector<std::uint8_t> wire{0x55, 1, 1, 0x57};
    wire.insert(wire.end(), suffix.begin(), suffix.end());
    require(!parser.feed(wire.data(), wire.size(), 0), "malformed stream accepted");
    require(!parser.error().empty() && packets.empty(), "malformed diagnostics");
    require(!parser.feed(wire.data(), 0, 0), "error is sticky");
  }
  std::vector<Packet> packets;
  usbpv::ProtocolParser parser(&packets, receive);
  std::vector<std::uint8_t> garbage(1048577, 0);
  require(!parser.feed(garbage.data(), garbage.size(), 0), "bounded start search");
}
void commands() {
  require(usbpv::register_command(1, 1) == std::array<std::uint8_t, 4>{0x55, 1, 1, 0x57}, "start command");
  require(usbpv::register_command(0x64, 99, true) ==
          std::array<std::uint8_t, 4>{0x55, 0xe4, 0, 0x39}, "read command checksum wrap");
  std::array<std::uint8_t, 8> pairs{255,255,255,255,255,255,255,255};
  require(usbpv::filter_registers(pairs, true) == std::array<std::uint8_t, 8>{}, "accept all");
  require(usbpv::filter_registers(pairs, false) ==
          std::array<std::uint8_t, 8>{0,32,0,0,0,0,0,0}, "drop wildcard");
  pairs = {1,1,255,2,127,255,255,255};
  require(usbpv::filter_registers(pairs, true) ==
          std::array<std::uint8_t, 8>{0x81,0xe1,0,0xe2,0xff,0x60,0,0x20}, "accept filters");
  require(usbpv::filter_registers(pairs, false) ==
          std::array<std::uint8_t, 8>{0x81,0xc1,0,0xc2,0xff,0x40,0,0}, "drop filters");
}
}  // namespace
int main() {
  framing(); timestamps_and_flags(); malformed(); commands();
  std::cout << "protocol tests passed\n";
}
