// Explicit lab workload matching usb_storage_load.cpp's Windows I/O pattern.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
struct Handle {
  int value = -1;
  ~Handle() { if (value >= 0) close(value); }
};
void check(bool ok, const char* action) {
  if (!ok) throw std::runtime_error(std::string(action) + ": " + std::strerror(errno));
}
double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string attribute(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string value;
  if (!std::getline(input, value)) throw std::runtime_error("cannot read USB identity: " + path.string());
  return value;
}
void verify_usb_directory(int fd) {
  struct stat info{};
  check(fstat(fd, &info) == 0, "stat workload directory");
  auto device = std::filesystem::canonical("/sys/dev/block/" +
      std::to_string(major(info.st_dev)) + ":" + std::to_string(minor(info.st_dev)));
  while (device != device.root_path()) {
    if (std::filesystem::exists(device / "idVendor")) {
      if (attribute(device / "idVendor") != "0011" ||
          attribute(device / "idProduct") != "7788" ||
          attribute(device / "serial") != "C4D197AC")
        throw std::runtime_error("workload directory is not on the verified USB disk");
      return;
    }
    device = device.parent_path();
  }
  throw std::runtime_error("workload directory is not on USB storage");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    const std::filesystem::path path(argv[1]);
    const std::string name = path.filename().string();
    if (!path.is_absolute() || name.rfind(".usbpv-load-", 0) != 0)
      throw std::runtime_error("use an absolute path with a new .usbpv-load- filename");
    std::size_t parsed = 0;
    const double seconds = std::stod(argv[2], &parsed);
    if (parsed != std::strlen(argv[2]) || !std::isfinite(seconds) || seconds < 1 || seconds > 30)
      throw std::runtime_error("read duration must be between 1 and 30 seconds");
    Handle directory;
    directory.value = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(directory.value >= 0, "open workload directory");
    verify_usb_directory(directory.value);

    constexpr std::size_t chunk = 1024 * 1024;
    constexpr std::uint64_t file_size = 64ULL * 1024 * 1024;
    void* allocation = nullptr;
    if (posix_memalign(&allocation, 4096, chunk)) throw std::bad_alloc();
    std::unique_ptr<unsigned char, decltype(&std::free)> memory(
        static_cast<unsigned char*>(allocation), std::free);
    auto* buffer = memory.get();
    for (std::size_t i = 0; i < chunk; ++i) buffer[i] = static_cast<unsigned char>(i * 131 + 17);

    Handle file;
    file.value = openat(directory.value, name.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_DIRECT | O_SYNC, 0600);
    check(file.value >= 0, "create unique direct-I/O temporary load file");
    // POSIX delete-on-close: remove only this newly created directory entry.
    // Its data stays allocated until the open handle closes, even on failure.
    check(unlinkat(directory.value, name.c_str(), 0) == 0, "unlink new temporary file");
    const auto begin = now();
    std::uint64_t writes = 0, reads = 0, ops = 0;
    while (writes < file_size) {
      const auto n = write(file.value, buffer, chunk);
      check(n >= 0, "uncached synchronous write");
      if (n != static_cast<ssize_t>(chunk)) throw std::runtime_error("short write");
      writes += static_cast<std::uint64_t>(n);
    }
    const double write_seconds = now() - begin;
    std::printf("{\"phase\":\"write\",\"bytes\":%llu,\"seconds\":%.6f}\n",
                static_cast<unsigned long long>(writes), write_seconds);
    std::fflush(stdout);
    const auto read_begin = now();
    std::uint64_t offset = 0;
    std::uint32_t random = 1234567;
    while (now() - read_begin < seconds) {
      const bool small = (static_cast<int>(now() - read_begin) / 2) % 2;
      const std::size_t length = small ? 4096 : chunk;
      random = random * 1664525U + 1013904223U;
      offset = small ? (random % (file_size / 4096)) * 4096 : (offset + chunk) % file_size;
      if (offset + length > file_size) offset = 0;
      check(lseek(file.value, static_cast<off_t>(offset), SEEK_SET) >= 0, "seek load file");
      const auto n = read(file.value, buffer, length);
      check(n >= 0, "uncached read");
      if (n != static_cast<ssize_t>(length)) throw std::runtime_error("short read");
      for (std::size_t i = 0; i < length; ++i)
        if (buffer[i] != static_cast<unsigned char>(i * 131 + 17))
          throw std::runtime_error("test data verification failed");
      reads += static_cast<std::uint64_t>(n); ++ops;
    }
    const double read_seconds = now() - read_begin;
    std::printf("{\"phase\":\"complete\",\"write_bytes\":%llu,\"write_seconds\":%.6f,"
                "\"read_bytes\":%llu,\"read_seconds\":%.6f,\"read_operations\":%llu,\"verified\":true}\n",
                static_cast<unsigned long long>(writes), write_seconds,
                static_cast<unsigned long long>(reads), read_seconds,
                static_cast<unsigned long long>(ops));
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
