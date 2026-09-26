# Windows diagnostic tools

These are explicit lab commands, separate from the normal capture build and
CTest. The workload helper is restricted to the previously verified flash
drive on **E:** (physical disk 1, USB storage serial bytes `03 43`, corresponding
to USB PnP serial `C4D197AC`). It refuses a different identity. Each workload
creates one new, uniquely named 64 MiB file, performs verified uncached reads
for 15 seconds, and deletes that file on close. Existing drive files are not
changed. Volume identity queries may require an elevated shell.

## Build helpers

From the repository root, using the same compiler as the main build:

```powershell
cmake -S tools/diagnostics -B build-diagnostics-tools -DCMAKE_BUILD_TYPE=Release
cmake --build build-diagnostics-tools --config Release
```

`analyze_load.exe` validates the high-speed raw-USB pcapng captures produced by
this project and reports packet/PID counts, USB CRC failures, and backwards
timestamps. `usb_storage_load.exe` is the identity-checked workload helper.
Neither helper runs automatically during a build or test.

## Capture and CPU measurements

The capture executable defaults to `build/usbpv_capture.exe`. Use `--exe` to
select another binary. Both runners enable NAK and SOF capture with the default
16,384-packet queue; storage capture also supports `--queue-capacity`.

```powershell
python tools/diagnostics/run_storage_load.py --exe build/usbpv_capture.exe
python tools/diagnostics/profile_cpu.py --exe build/usbpv_capture.exe
```

CPU profiling samples process and thread CPU time externally, includes brief
capture intervals before/after the workload, and saves `samples.json` and
`cpu.json`. A CPU percentage of 100 means one fully occupied logical processor.
Each runner has `--help`, `--load-exe`, and `--output-root`. With Visual Studio,
pass `--load-exe build-diagnostics-tools/Release/usb_storage_load.exe` and the
appropriate `--exe .../Release/usbpv_capture.exe` path.

New evidence goes to `diagnostics/artifacts/YYYY-MM-DD/native/` for storage
capture or `.../bottleneck/` for CPU profiling, with a timestamped run folder.
To validate a saved capture:

```powershell
build-diagnostics-tools/analyze_load.exe PATH/traffic.pcapng
```

## Optional labelled CPU build (MinGW)

`make_cpu_profile.py` generates source copies adding one thread-name call per
thread. It does not change production sources or add per-packet measurements.
Build the main application first so `build/usbpv_fpga.hpp` exists, then run:

```powershell
python tools/diagnostics/make_cpu_profile.py
g++ -std=c++17 -O3 -DNDEBUG -static -Iinclude -Isrc -Ibuild -Ibuild-diagnostics-tools/labelled `
  build-diagnostics-tools/labelled/cpu_usbpv_capture.cpp `
  build-diagnostics-tools/labelled/cpu_usbpv_native.cpp `
  src/usbpv_protocol.cpp build-diagnostics-tools/labelled/cpu_usbpv_output.cpp `
  -lwinusb -lsetupapi -o build-diagnostics-tools/labelled/cpu_capture.exe
python tools/diagnostics/profile_cpu.py --exe build-diagnostics-tools/labelled/cpu_capture.exe
```

Historical experiments, including debugger reproducers and superseded source
rewriting scripts, remain intact in the [dated archive](../../diagnostics/README.md).
