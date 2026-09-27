# USBPV agent capture

An automation-oriented command-line capture program for USBPV USB sniffers.
It is intended for embedded USB debugging workflows, including test runs
against projects such as TinyUSB.

The program requires an explicit low/full/high capture speed, disables SOF and
NAK traffic by default, moves disk I/O out of the capture callback, and writes
Wireshark/TShark-readable pcapng plus machine-readable JSON status files.

CH56x sniffers use native capture by default: WinUSB on Windows and libusb
on Linux. This removes the vendor DLL's capture-queue race that caused access
violations during ISO captures. No vendor DLL is loaded for native capture.
See [protocol and crash diagnosis](docs/reference/native-protocol.md).

## Repository layout

```text
include/       Public vendor API header
src/           Agent capture implementation
docs/          Indexed guides, reference, and dated investigation reports
examples/      Original C++, Python, C#, Qt, and Visual Studio samples
tools/         Runtime preparation utilities
vendor/        Windows x86/x64 and Linux x64 vendor runtimes
captures/      Local captures (ignored by Git)
diagnostics/   Evidence index and ignored dated diagnostic archives
```

## Build

Start with the [documentation index](docs/README.md) for operating guides,
protocol reference, and investigation reports.

Windows with Visual Studio or MinGW:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Linux x86-64:

```sh
sudo apt-get install build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows and Linux builds embed the sniffer's existing volatile FPGA image as data from
the bundled SDK; deployed builds use WinUSB or the system libusb runtime.
`--library PATH` explicitly opts into the legacy DLL backend, including its
known capture race. Older FTDI sniffers require that legacy backend.

Linux ready/summary JSON reports `native-libusb`. The USB device node must be
readable and writable by the capture user; a desktop session may already grant
access. See the [Linux permissions guide](docs/guides/agent-capture.md#linux-usb-permissions).
CMake defaults `USBPV_COPY_VENDOR_RUNTIME=OFF` on both platforms. Explicit
`--library PATH` still selects the legacy runtime; the bundled Linux library
is x86-64 and also needs `libudev.so.1`.

Run parser, output, queue, and callback tests with `ctest --test-dir build --output-on-failure`
(add `-C Release` for Visual Studio builds).

## Capture

```sh
./build/usbpv_capture list
./build/usbpv_capture capture --speed high --duration 20 \
  --output captures/run.pcapng
```

On multi-configuration Windows builds the executable may be under
`build/Release`. See [the agent workflow guide](docs/guides/agent-capture.md) for
ready/stop-file coordination, filters, output schema, and NAK safety.

Pcapng output uses a fixed 32 MiB buffer pool and a separate file writer so
short disk stalls do not block packet processing. Buffer exhaustion or output
errors fail the capture explicitly. See the [bottleneck investigation and
validation](docs/reports/2026-09-26/capture-bottleneck.md).

The native reader also batches queue insertion and packet counters in groups
of up to 256 packets across USB transfers, with a 1 ms publication target,
reducing synchronization and clock-read overhead. See the latest
[CPU optimization measurements](docs/reports/2026-09-27/cpu-optimization.md).

The [TODO](TODO.md) tracks remaining hardware soak tests and capture fixtures.
See the [diagnostic evidence index](diagnostics/README.md) and
[reusable profiling tools](tools/diagnostics/README.md) for saved investigations
and new workload measurements.
The binary SDK components are vendor-provided; no upstream license file was
included in the supplied SDK, so verify redistribution terms before publishing
this repository.
