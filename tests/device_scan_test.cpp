#include "usbpv_device_scan.hpp"
#include <cstdlib>
#include <iostream>

namespace {
void require(bool ok, const char* message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
struct FakeDevice {
  inline static int alive = 0;
  inline static std::vector<std::string> probes;
  std::string serial;
  FakeDevice() { ++alive; }
  ~FakeDevice() { --alive; }
  void connect(const std::string& path) {
    probes.push_back(path);
    if (path == "busy") throw std::runtime_error("open sniffer (Windows error 32)");
    if (path == "broken") throw std::runtime_error("invalid USB identity string");
    serial = path;
  }
};
template <typename Action>
void fails(Action action, const char* expected) {
  try { action(); require(false, "expected inaccessible/missing device error"); }
  catch (const std::runtime_error& ex) {
    require(std::string(ex.what()).find(expected) != std::string::npos, "lost probe diagnostic");
  }
  require(FakeDevice::alive == 0, "failed probe leaked device ownership");
}
}  // namespace

int main() {
  using usbpv::native::detail::list_serials;
  using usbpv::native::detail::find_device;
  for (const std::vector<std::string>& paths : {
           std::vector<std::string>{"busy", "B", "broken", "C"},
           std::vector<std::string>{"B", "busy", "C", "broken"}}) {
    require(list_serials<FakeDevice>(paths) == "B,C", "failed probe blocked other serials");
    require(FakeDevice::alive == 0, "enumeration retained an exclusive handle");
    auto device = find_device<FakeDevice>(paths, "C");
    require(device->serial == "C" && FakeDevice::alive == 1, "explicit serial selection/ownership");
  }
  require(FakeDevice::alive == 0, "selected device was not released");
  FakeDevice::probes.clear();
  {
    auto device = find_device<FakeDevice>({"B", "broken"}, "B");
    require(FakeDevice::probes == std::vector<std::string>{"B"}, "probed after matching serial");
  }
  require(list_serials<FakeDevice>({}).empty(), "empty enumeration should succeed");
  fails([] { list_serials<FakeDevice>({"busy", "broken"}); }, "Windows error 32");
  fails([] { find_device<FakeDevice>({"busy", "B"}, "A"); }, "Windows error 32");
  fails([] { find_device<FakeDevice>({"B"}, "A"); }, "not connected");
  fails([] { find_device<FakeDevice>({}, "A"); }, "not connected");
  std::cout << "device scan tests passed\n";
}
