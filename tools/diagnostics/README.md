# Diagnostic tools

These are explicit lab commands, separate from the normal capture build and
CTest. For comparisons between operating systems, use the Windows helper below
and the [matching Linux write/read workload](#linux-workload-matching-windows).
The [Linux read-only runner](#linux-read-only-usb-storage-load) uses a different
workload and should not be used for a direct CPU comparison.

The Windows workload helper is restricted to the previously verified flash
drive on **E:** (physical disk 1, USB storage serial bytes `03 43`, corresponding
to USB PnP serial `C4D197AC`). It refuses a different identity. Each workload
creates one new, uniquely named 64 MiB file, performs verified uncached reads
for 15 seconds, and deletes that file on close. Existing drive files are not
changed. Volume identity queries may require an elevated shell.

## Windows helpers

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

## Linux read-only USB storage load

Build the Linux capture and analyzer with CMake. This explicit lab runner opens
only the verified USB disk (`0011:7788`, serial `C4D197AC`) read-only with
`O_DIRECT`. It alternates 1 MiB sequential and 4 KiB random reads within the
first 64 MiB. It never writes to the disk or changes its mount options. The
capture records the returned USB payloads, so keep its artifacts private.

```sh
cmake -S tools/diagnostics -B build-linux-tools -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-tools -j
python tools/diagnostics/run_linux_storage_load.py --exe build-linux/usbpv_capture \
  --seconds 15 --output diagnostics/artifacts/linux-load-01
build-linux-tools/analyze_load diagnostics/artifacts/linux-load-01/traffic.pcapng
```

The user must already have access to the sniffer and read access to the disk.
The runner verifies the opened device's USB identity through sysfs, saves
read throughput and capture process CPU samples from `/proc`, and fails if
the final capture reports loss. 100% CPU means one logical processor. Output
must be a new directory. Avoid builds and capture analysis during measurement.
This read-only Linux workload differs from the Windows write/read workload.

## Linux workload matching Windows

`usb_storage_load_linux` performs the same application I/O pattern as the
Windows helper: a new 64 MiB file, 1 MiB sequential synchronous writes using
pattern `(i * 131 + 17) & 255`, then 15 seconds alternating two-second phases
of 1 MiB sequential and 4 KiB random reads. It uses the same PRNG, offsets,
and byte-by-byte read verification. `O_DIRECT | O_SYNC` supplies uncached,
synchronous Linux I/O; underlying OS/filesystem command sequences can differ.

The helper verifies that the opened destination directory is on USB disk
`0011:7788`, serial `C4D197AC`, before creating a file with `O_EXCL`. It unlinks
only that newly created file while its handle is open, so closing the handle
reclaims it on success, errors, or process termination. Existing files are
never opened for writing. The Linux disk must be mounted writable on the host.
A restricted sandbox may expose a read-only view of an otherwise writable mount.

```sh
cmake -S tools/diagnostics -B build-linux-tools -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-tools -j
python tools/diagnostics/run_linux_storage_write_load.py \
  --exe build-linux/usbpv_capture \
  --load-exe build-linux-tools/usb_storage_load_linux \
  --directory /path/to/verified-usb-mount \
  --output diagnostics/artifacts/linux-write-read-01
build-linux-tools/analyze_load diagnostics/artifacts/linux-write-read-01/traffic.pcapng
```

Replace `/path/to/verified-usb-mount` with the actual mount directory of the
verified disk; the helper refuses other USB identities. Build the main Linux
capture executable first using the [quick start](../../README.md#linux).

The runner matches the Windows profiler's capture flags, default queue,
initial/final two-second idle intervals, and 250 ms CPU sampling. It measures
capture process user plus kernel CPU over the complete write/read helper
lifetime; 100% means one logical processor. It saves workload verification,
write/read throughput, CPU samples, and capture completeness counters. The
output directory must be new. The helper is an explicit hardware tool and
is never invoked by CTest or during builds.
