#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace usbpv::native::detail {
// A failed probe belongs to one interface, not the entire enumeration. Keep
// ownership local so even partially connected devices release their handles.
template <typename Device>
std::unique_ptr<Device> probe_device(const std::string& path, std::string& first_error) {
  auto device = std::make_unique<Device>();
  try {
    device->connect(path);
  } catch (const std::runtime_error& ex) {
    if (first_error.empty()) first_error = ex.what();
    return nullptr;
  }
  return device;
}

template <typename Device>
std::string list_serials(const std::vector<std::string>& paths) {
  std::string result, first_error;
  for (const auto& path : paths) {
    auto device = probe_device<Device>(path, first_error);
    if (!device) continue;
    if (!result.empty()) result += ',';
    result += device->serial;
  }
  if (result.empty() && !first_error.empty())
    throw std::runtime_error("no accessible CH56x sniffer: " + first_error);
  return result;
}

template <typename Device>
std::unique_ptr<Device> find_device(const std::vector<std::string>& paths,
                                    const std::string& serial) {
  std::string first_error;
  for (const auto& path : paths) {
    auto device = probe_device<Device>(path, first_error);
    if (device && device->serial == serial) return device;
  }
  if (!first_error.empty())
    throw std::runtime_error("requested CH56x sniffer was not found among accessible devices; "
                             "a device probe failed: " + first_error);
  throw std::runtime_error("requested CH56x sniffer is not connected");
}
}  // namespace usbpv::native::detail
