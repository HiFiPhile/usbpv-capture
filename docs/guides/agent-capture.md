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

Linux:

```sh
sudo apt-get install build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The native backend talks directly to CH56x sniffers through WinUSB on Windows
and libusb on Linux. Both implement FPGA setup, register commands, stream
framing, filters, and timestamps without loading the vendor capture runtime.
Ready/summary JSON identifies them as `native-winusb` and `native-libusb`.

Pass an absolute `--library` path to opt into the legacy vendor engine (for
example, for an older FTDI sniffer). The Windows DLL retains its known
capture-queue race. See [native protocol details](../reference/native-protocol.md).
CMake defaults `USBPV_COPY_VENDOR_RUNTIME=OFF` on both platforms; existing
build directories retain their cached setting.

### Linux USB permissions

Install the system libusb runtime (`libusb-1.0-0` on Debian/Ubuntu). The capture
user needs read/write access to the sniffer's `/dev/bus/usb` node. For desktop
sessions using systemd-logind, an administrator can install this rule in
`/etc/udev/rules.d/70-usbpv.rules`, reload udev rules, and reconnect the sniffer:

```udev
SUBSYSTEM=="usb", ATTR{idVendor}=="16c0", ATTR{idProduct}=="05dc", ATTR{manufacturer}=="tusb.org", TAG+="uaccess"
```

For headless operation, grant access to a dedicated group using local udev
policy. The backend claims the USB interface exclusively and does not detach
kernel drivers or change the active USB configuration. A busy or inaccessible
sniffer returns a JSON error.

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

Packet processing encodes pcapng into a fixed pool of 32 one-MiB buffers.
See the [data-flow diagram](../reference/capture-data-flow.md) for all buffer
sizes and thread boundaries.
A separate worker writes those buffers to disk; bus-event JSONL has its own
256 KiB pool and worker. Periodic flush requests are asynchronous and ordered.
Shutdown drains pending output and checks final flush/close errors before
reporting success. Sustained slow storage can exhaust either pool; this is an
explicit output failure, with a nonzero exit and `complete: false`.

Ready JSON includes `output_buffer_bytes` (33,554,432 for pcapng). Final summary
JSON includes `output_pending_peak_bytes`, the maximum published pcapng bytes
waiting for or inside a file write; it excludes the partially filled producer
buffer. These buffers absorb temporary stalls but cannot guarantee lossless
capture through arbitrarily long I/O delays; shutdown still waits for
outstanding file I/O.

The native reader stages up to 256 validated packets (about 516 KiB) before
inserting them into the bounded packet queue with one lock operation. A
partial batch can span USB transfers, with a 1 ms publication target. A full
batch publishes immediately; idle waits, stop, and reader errors flush the
tail. OS scheduling can delay publication beyond this target. Total
callback count and host-side activity time update at each transfer; successful
packet counters update when publishing. Captured USB timestamps are unchanged.
The legacy callback path continues to publish each packet immediately. See
the [CPU follow-up measurements](../reports/2026-09-27/cpu-optimization.md).

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
