# USBPV agent capture

An automation-oriented command-line capture program for USBPV USB sniffers.
It is intended for embedded USB debugging workflows, including test runs
against projects such as TinyUSB.

The program requires an explicit low/full/high capture speed, disables SOF and
NAK traffic by default, moves disk I/O out of the capture callback, and writes
Wireshark/TShark-readable pcapng plus machine-readable JSON status files.

On Windows, CH56x sniffers use a native WinUSB protocol implementation by
default. This removes the vendor DLL's capture-queue race that caused access
violations during ISO captures. No vendor DLL is loaded for native capture.
See [protocol and crash diagnosis](docs/native-protocol.md).

## Repository layout

```text
include/       Public vendor API header
src/           Agent capture implementation
docs/          Operation guide and vendor documentation
examples/      Original C++, Python, C#, Qt, and Visual Studio samples
tools/         Runtime preparation utilities
vendor/        Windows x86/x64 and Linux x64 vendor runtimes
captures/      Local captures (ignored by Git)
```

## Build

Windows with Visual Studio or MinGW:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Linux x86-64:

```sh
sudo apt-get install build-essential cmake libudev1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows builds embed the sniffer's existing volatile FPGA image as data from
the bundled SDK; they need only Windows' WinUSB runtime when deployed.
`--library PATH` explicitly opts into the legacy DLL backend, including its
known capture race. Older FTDI sniffers require that legacy backend.

Linux still uses the vendor backend; its capture engine has not been replaced.
CMake copies the runtime by default on Linux (`USBPV_COPY_VENDOR_RUNTIME=ON`)
and defaults to `OFF` on Windows. The Linux vendor
library is x86-64 and depends on `libudev.so.1`, `libstdc++.so.6`,
`libgcc_s.so.1`, and `libc.so.6`.

Run parser regression tests with `ctest --test-dir build --output-on-failure`
(add `-C Release` for Visual Studio builds).

## Capture

```sh
./build/usbpv_capture list
./build/usbpv_capture capture --speed high --duration 20 \
  --output captures/run.pcapng
```

On multi-configuration Windows builds the executable may be under
`build/Release`. See [the agent workflow guide](docs/agent-capture.md) for
ready/stop-file coordination, filters, output schema, and NAK safety.

The [TODO](TODO.md) tracks remaining hardware soak tests and capture fixtures.
The binary SDK components are vendor-provided; no upstream license file was
included in the supplied SDK, so verify redistribution terms before publishing
this repository.
