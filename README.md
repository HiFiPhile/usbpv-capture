# USBPV agent capture

An automation-oriented command-line capture program for USBPV USB sniffers.
It is intended for embedded USB debugging workflows, including test runs
against projects such as TinyUSB.

The program requires an explicit low/full/high capture speed, disables SOF and
NAK traffic by default, moves disk I/O out of the vendor callback, and writes
Wireshark/TShark-readable pcapng plus machine-readable JSON status files.

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

By default CMake copies the matching bundled vendor runtime beside the
executable. Set `-DUSBPV_COPY_VENDOR_RUNTIME=OFF` to build without copying it,
then use `--library PATH` or retain the repository layout. The Linux vendor
library is x86-64 and depends on `libudev.so.1`, `libstdc++.so.6`,
`libgcc_s.so.1`, and `libc.so.6`.

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
