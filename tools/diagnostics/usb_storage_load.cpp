#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <algorithm>

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void check(bool ok, const char* action) {
  if (!ok) throw std::runtime_error(std::string(action) + ": Windows error " + std::to_string(GetLastError()));
}
double now() {
  LARGE_INTEGER ticks, frequency;
  QueryPerformanceCounter(&ticks); QueryPerformanceFrequency(&frequency);
  return double(ticks.QuadPart) / frequency.QuadPart;
}
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string path(argv[1]);
  if (path.rfind("E:\\.usbpv-load-", 0) != 0 || path.find("..") != std::string::npos) return 2;
  const double seconds = std::stod(argv[2]);
  if (seconds < 1 || seconds > 30) return 2;
  try {
    // Verify E: still resolves to the USB disk identified before this test.
    Handle volume;
    volume.value = CreateFileA("\\\\.\\E:", 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    check(volume.value != INVALID_HANDLE_VALUE, "open volume identity");
    STORAGE_DEVICE_NUMBER number{}; DWORD n = 0;
    check(DeviceIoControl(volume.value, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                          &number, sizeof(number), &n, nullptr), "get storage device number");
    if (number.DeviceNumber != 1) throw std::runtime_error("E: no longer maps to physical disk 1");
    STORAGE_PROPERTY_QUERY query{}; query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    alignas(STORAGE_DEVICE_DESCRIPTOR) unsigned char identity[4096]{};
    check(DeviceIoControl(volume.value, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                          identity, sizeof(identity), &n, nullptr), "get storage identity");
    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(identity);
    if (n < sizeof(*desc) || desc->BusType != BusTypeUsb)
      throw std::runtime_error("E: is not a USB drive");
    const char* serial = desc->SerialNumberOffset && desc->SerialNumberOffset < n
                             ? reinterpret_cast<char*>(identity + desc->SerialNumberOffset) : "";
    // This device exposes PnP USB serial C4D197AC but its storage descriptor
    // serial is the two bytes 03 43, independently confirmed via Win32_DiskDrive.
    if (std::string(serial) != std::string("\x03" "C"))
      throw std::runtime_error("USB storage descriptor identity changed");
    CloseHandle(volume.value); volume.value = INVALID_HANDLE_VALUE;

    constexpr DWORD chunk = 1024 * 1024;
    constexpr std::uint64_t file_size = 64ULL * 1024 * 1024;
    auto* buffer = static_cast<unsigned char*>(VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(buffer != nullptr, "allocate aligned buffer");
    struct Memory { void* p; ~Memory() { VirtualFree(p, 0, MEM_RELEASE); } } memory{buffer};
    for (DWORD i = 0; i < chunk; ++i) buffer[i] = static_cast<unsigned char>(i * 131 + 17);
    Handle file;
    file.value = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_NO_BUFFERING |
                              FILE_FLAG_WRITE_THROUGH | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    check(file.value != INVALID_HANDLE_VALUE, "create unique temporary load file");
    const auto begin = now();
    std::uint64_t writes = 0, reads = 0, ops = 0;
    for (; writes < file_size; writes += n) {
      check(WriteFile(file.value, buffer, chunk, &n, nullptr), "uncached write");
      if (n != chunk) throw std::runtime_error("short write");
    }
    const double write_seconds = now() - begin;
    std::printf("{\"phase\":\"write\",\"bytes\":%llu,\"seconds\":%.6f}\n",
                (unsigned long long)writes, write_seconds); std::fflush(stdout);
    const auto read_begin = now();
    std::uint64_t offset = 0;
    std::uint32_t random = 1234567;
    while (now() - read_begin < seconds) {
      const bool small = (static_cast<int>(now() - read_begin) / 2) % 2;
      const DWORD length = small ? 4096 : chunk;
      random = random * 1664525U + 1013904223U;
      offset = small ? (random % (file_size / 4096)) * 4096 : (offset + chunk) % file_size;
      if (offset + length > file_size) offset = 0;
      LARGE_INTEGER position{}; position.QuadPart = offset;
      check(SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN), "seek load file");
      check(ReadFile(file.value, buffer, length, &n, nullptr), "uncached read");
      if (n != length) throw std::runtime_error("short read");
      for (DWORD i = 0; i < n; ++i)
        if (buffer[i] != static_cast<unsigned char>(i * 131 + 17))
          throw std::runtime_error("test data verification failed");
      reads += n; ++ops;
    }
    const double read_seconds = now() - read_begin;
    // Handle destruction deletes only the new file, including on failures.
    std::printf("{\"phase\":\"complete\",\"write_bytes\":%llu,\"write_seconds\":%.6f,"
                "\"read_bytes\":%llu,\"read_seconds\":%.6f,\"read_operations\":%llu,\"verified\":true}\n",
                (unsigned long long)writes, write_seconds, (unsigned long long)reads,
                read_seconds, (unsigned long long)ops);
    return 0;
  } catch (const std::exception& ex) { std::fprintf(stderr, "%s\n", ex.what()); return 1; }
}
