# USBPV agent capture

A command-line USB packet capture tool for CH56x-based USBPV sniffers on
Windows and Linux. It writes Wireshark/TShark-readable pcapng, bus-event JSONL,
and machine-readable capture status for embedded USB debugging and automation.

Capture uses native WinUSB on Windows and libusb on Linux. Select the captured
bus speed explicitly: low (1.5 Mb/s), full (12 Mb/s), or high (480 Mb/s).
The sniffer's monitor connection to the computer can use SuperSpeed; this does
not enable capture of SuperSpeed bus traffic. SOF and NAK capture are disabled
by default.

Only CH56x sniffers are supported; older FTDI sniffers are unsupported.

## Quick start

### Linux

Install a C++17 compiler, CMake, pkg-config, and libusb development headers
(libusb 1.0.21 or newer). On Debian/Ubuntu:

```sh
sudo apt-get install build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j
ctest --test-dir build-linux --output-on-failure
mkdir -p captures
./build-linux/usbpv_capture list
./build-linux/usbpv_capture capture --speed high --duration 20 \
  --output captures/run.pcapng
```

The capture user needs read/write access to the sniffer's USB device node.
See [Linux USB permissions](docs/guides/agent-capture.md#linux-usb-permissions)
if listing or opening the device fails.

### Windows

Use a Visual Studio developer PowerShell or a configured MinGW environment.
The sniffer's capture interface must use the WinUSB driver. With Visual Studio:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
New-Item -ItemType Directory -Force captures | Out-Null
.\build\Release\usbpv_capture.exe list
.\build\Release\usbpv_capture.exe capture --speed high --duration 20 --output captures\run.pcapng
```

For a MinGW single-configuration build, configure with
`-G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release` and use
`build/usbpv_capture.exe` as the executable path.

### Capture results

Choose a new output basename for each run; output directories must exist.
One connected sniffer is selected automatically. Use `--serial SN` when
multiple sniffers are connected.

| File | Contents |
| --- | --- |
| `run.pcapng` | Captured USB packets for Wireshark/TShark |
| `run.pcapng.events.jsonl` | Reset, suspend, and overflow events |
| `run.pcapng.summary.json` | Completion status, stop reason, and counters |

Check both the process exit code and the final summary's `complete` field.
Detected capture loss, transport errors, and output failures return nonzero.
The [capture guide](docs/guides/agent-capture.md) covers ready/stop files,
filters, limits, and NAK capture. Run `usbpv_capture help` for all CLI options.

## Performance and burst handling

Both platforms batch up to 256 packets with a 1 ms publication target and use
a separate 32 MiB pcapng output pool to absorb short disk stalls. Linux also
groups USB completions over a 250-microsecond target interval during active
SuperSpeed monitor transfers; Windows retains its own transport policy.

On the tested USB disk, the matching write/read workload used about 23% of one
CPU core on Linux, within the saved Windows range of 20-28%. NAK-heavy Linux
captures also passed injected reader stalls of 5, 10, and 20 ms without loss.
These measurements cover the tested workload; the reported 8 MB sniffer FIFO
has not been independently measured and does not guarantee lossless capture
under sustained overload. See the [Linux load and burst report](docs/reports/2026-09-27/linux-native.md)
for methodology and limits, and the [buffer reference](docs/reference/capture-data-flow.md)
for capacities and ownership.

## Deployment and repository

CMake installation contains the capture executable only. Windows uses WinUSB;
Linux requires the system libusb runtime and USB permissions. Both builds
extract and embed the volatile FPGA image from the bundled SDK as data.
No vendor capture DLL/SO is loaded or deployed.

```text
src/           Capture implementation and native transports
include/       Vendor packet and API definitions used by native capture
tests/         Automated tests and explicit hardware test runner
docs/          Operating guides, references, and dated investigation reports
tools/         Build and diagnostic utilities
examples/      Archived vendor SDK examples
vendor/        Archived SDK binaries and build-time FPGA image source
captures/      Local captures (ignored by Git)
diagnostics/   Evidence index and ignored dated diagnostic archives
```

Start with the [documentation index](docs/README.md) for further detail.
The [TODO](TODO.md) tracks remaining validation, and the
[diagnostic tools guide](tools/diagnostics/README.md) explains how to reproduce
storage-load and CPU measurements.

The binary SDK components are vendor-provided; no upstream license file was
included in the supplied SDK, so verify redistribution terms before publishing
this repository.
