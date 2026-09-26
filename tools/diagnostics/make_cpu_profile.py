from pathlib import Path

root = Path(__file__).resolve().parents[2]
out = root / 'build-diagnostics-tools/labelled'
out.mkdir(parents=True, exist_ok=True)
(out / 'cpu_tag.hpp').write_text('''#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
inline void cpu_tag(const wchar_t* name) {
  using SetName = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  auto symbol = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
  SetName set = nullptr;
  static_assert(sizeof(set) == sizeof(symbol));
  std::memcpy(&set, &symbol, sizeof(set));
  if (set) set(GetCurrentThread(), name);
}
''')
replacements = {
    'usbpv_capture.cpp': [
        ('int main(int argc, char** argv) {', 'int main(int argc, char** argv) {\n  cpu_tag(L"control");'),
        ('std::ofstream& events, WriterState& state) {', 'std::ofstream& events, WriterState& state) {\n  cpu_tag(L"packet-encoder");'),
    ],
    'usbpv_native.cpp': [
        ('void capture(void* context, pfn_packet_handler callback, BatchEnd batch_end) noexcept {', 'void capture(void* context, pfn_packet_handler callback, BatchEnd batch_end) noexcept {\n    cpu_tag(L"usb-reader-parser");'),
    ],
    'usbpv_output.cpp': [
        ('void BufferedOutput::run() noexcept {', 'void BufferedOutput::run() noexcept {\n  cpu_tag(bytes_ == 64 * 1024 ? L"event-file-writer" : L"pcap-file-writer");'),
    ],
}
for filename, edits in replacements.items():
    source = (root / 'src' / filename).read_text()
    if source.startswith('#define NOMINMAX\n'):
        source = source[len('#define NOMINMAX\n'):]
    for before, after in edits:
        assert source.count(before) == 1, before
        source = source.replace(before, after)
    (out / ('cpu_' + filename)).write_text('#include "cpu_tag.hpp"\n' + source)
print('Diagnostic copies add one thread-name call per thread; no hot-path instrumentation.')
