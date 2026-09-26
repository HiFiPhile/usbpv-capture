# USBPV agent capture

`usbpv_capture` is a bounded, automation-oriented USBPV capture program.
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

On Windows, the default backend talks directly to CH56x sniffers through
WinUSB. It implements FPGA setup, register commands, stream framing, filters,
and timestamps without loading the vendor DLL. Existing CLI invocations keep
working; ready/summary JSON identifies this backend as `native-winusb`.

Pass an absolute `--library` path only to opt into the legacy vendor engine
(for example, for an older FTDI sniffer). That path retains the vendor's
known capture-queue race. See [native protocol details](native-protocol.md).
Linux continues to discover and use the bundled vendor library. CMake copies
vendor runtimes by default on Linux, but not on Windows; the copy option is
`USBPV_COPY_VENDOR_RUNTIME`.

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
overflow events, malformed native stream records, USB transport failures,
and file write failures make the command return nonzero. Native shutdown
waits for the stop response and reaps pending reads before releasing buffers.

Run `usbpv_capture help` for all options. Address filters accept USB addresses
0..127, endpoints 0..15, and `*` as a wildcard. Up to four `--accept` or four
`--drop` pairs can be sent to the hardware.
