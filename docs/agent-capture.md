# USBPV agent capture

`usbpv_capture` is a bounded, automation-oriented front end for `usbpv_lib`.
It writes USB link-layer packets to pcapng and writes bus events plus a final
summary to JSON files. Wireshark and TShark understand the speed-specific raw
USB link types used in the pcapng file.

## Build

Windows, from a Visual Studio developer shell or a MinGW environment:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Linux x86-64:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

CMake copies the matching bundled vendor runtime beside the executable. The
program also searches the repository's `vendor/windows-x64`,
`vendor/windows-x86`, or `vendor/linux-x64` directory. Pass an absolute path
with `--library` to override discovery.

## Agent workflow

List sniffers:

```powershell
.\usbpv_capture.exe list
```

Capture high-speed traffic for 20 seconds:

```powershell
.\usbpv_capture.exe capture --speed high --duration 20 --output captures\run.pcapng
```

Coordinate an external test with files:

```powershell
.\usbpv_capture.exe capture --speed high --duration 0 `
  --ready-file capture.ready --stop-file capture.stop --output captures\run.pcapng
```

Wait until `capture.ready` exists before starting the device test. Create
`capture.stop` when the test finishes. Both control paths must not already
exist. The program also accepts Ctrl+C.

Every stdout line is one JSON object. The final object is also written to
`captures/run.pcapng.summary.json`; reset/suspend/overflow events are written
to `captures/run.pcapng.events.jsonl`. Diagnostics go to stderr.

The default packet mask is `0xEB`: ACK, ISO, STALL, PING, incomplete, and
error packets are enabled; SOF and NAK are disabled. Use `--include-sof` when
frame timing is relevant. Use `--include-nak` only for a short, tightly
filtered capture, for example:

```powershell
.\usbpv_capture.exe capture --speed high --duration 2 --include-nak `
  --accept 5:2 --output nak-debug.pcapng
```

NAKs can overwhelm the sniffer. The program drains callbacks for 250 ms by
default before declaring itself ready; change this with `--flush-ms`. If a
session still opens with corrupt packets, disconnect/reconnect or power-cycle
the sniffer before retrying. Queue loss, oversized/corrupt callbacks, device
overflow events, and file write failures make the command return nonzero.

Run `usbpv_capture help` for all options. Address filters accept USB addresses
0..127, endpoints 0..15, and `*` as a wildcard. Up to four `--accept` or four
`--drop` pairs can be sent to the hardware.
