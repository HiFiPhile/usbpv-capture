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

For MinGW, configure with `-G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release`
and use `build/usbpv_capture.exe` as the executable path.

Linux (libusb 1.0.21 or newer):

```sh
sudo apt-get install build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j
```

The native backend talks directly to CH56x sniffers through WinUSB on Windows
and libusb on Linux. Both implement FPGA setup, register commands, stream
framing, filters, and timestamps without loading the vendor capture runtime.
Ready/summary JSON identifies them as `native-winusb` and `native-libusb`.

Capture supports only CH56x sniffers; older FTDI sniffers are unsupported.
Installation contains only the capture executable.
See [native protocol details](../reference/native-protocol.md).

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

Run from the repository root. The Windows examples use a Visual Studio build;
with MinGW, use `build/usbpv_capture.exe` without the `Release` directory.
The Windows capture interface must use the WinUSB driver.

List sniffers and capture high-speed traffic for 20 seconds on Linux:

```sh
mkdir -p captures
./build-linux/usbpv_capture list
./build-linux/usbpv_capture capture --speed high --duration 20 --output captures/run.pcapng
```

Or on Windows:

```powershell
New-Item -ItemType Directory -Force captures | Out-Null
.\build\Release\usbpv_capture.exe list
.\build\Release\usbpv_capture.exe capture --speed high --duration 20 --output captures\run.pcapng
```

One available sniffer is selected automatically. With multiple devices, pass
`--serial SN` using a serial returned by `list`. Choose `--speed low`, `full`,
or `high` for the captured bus. The SuperSpeed monitor link is a separate
connection to the capture computer; it does not change the captured bus speed.

Output directories must already exist. Use a fresh basename for each capture;
the pcapng, events, summary, ready, and stop paths must be distinct and must
not already exist.

Coordinate an external test with files on Linux:

```sh
./build-linux/usbpv_capture capture --speed high --duration 0 \
  --ready-file capture.ready --stop-file capture.stop --output captures/coordinated.pcapng
```

Or on Windows:

```powershell
.\build\Release\usbpv_capture.exe capture --speed high --duration 0 `
  --ready-file capture.ready --stop-file capture.stop --output captures\coordinated.pcapng
```

Wait until `capture.ready` exists before starting the device test. Create
`capture.stop` when the test finishes. Both control paths must not already
exist. Create the stop file with `touch capture.stop` on Linux or
`New-Item -ItemType File capture.stop` in PowerShell. Wait for the capture
process to exit and inspect its summary before using the capture.
The program also accepts Ctrl+C.

Status lines on stdout are JSON objects (`help` prints plain text). For an
output path `FILE`, the final summary is also written to `FILE.summary.json`;
reset/suspend/overflow events go to `FILE.events.jsonl`. Diagnostics go to stderr.
A successful capture exits zero and reports `complete: true`; detected loss
or errors produce a nonzero exit. Failures before capture starts may emit only
an error object and no summary file. Treat `ready` as permission to begin the
test workload, not proof that the eventual capture completed successfully.

The default duration is 30 seconds. `--duration 0` disables that limit;
`--max-packets N`, `--idle-timeout SEC`, a stop file, or Ctrl+C can still stop
capture. Packet-count stopping is checked asynchronously, so the final count
can exceed the requested threshold. Idle timeout measures callback activity,
including bus events, rather than only accepted data packets.

## Buffering and platform tuning

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
See the [CPU follow-up measurements](../reports/2026-09-27/cpu-optimization.md).

Linux additionally waits for a 250-microsecond target interval before handling
USB completions when the next read is incomplete during active SuperSpeed
monitor capture. USB 2.0 monitor links and start/stop handshakes skip this wait.
Windows retains its existing WinUSB policy. See the
[matched write/read workload and burst tests](../reports/2026-09-27/linux-native.md)
for CPU comparisons and the limits of the measured burst margin. The reported
8 MB device FIFO is separate from host buffers; its capacity alone cannot
guarantee lossless capture during a NAK storm.

## Packet filters and NAK capture

The default packet mask is `0xEB`: ACK, ISO, STALL, PING, incomplete, and
error packets are enabled; SOF and NAK are disabled. Use `--include-sof` when
frame timing is relevant. Use `--include-nak` only for a short, tightly
filtered capture, for example:

```powershell
.\build\Release\usbpv_capture.exe capture --speed high --duration 2 --include-nak `
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
0..127, endpoints 0..15, and `*` as a wildcard. Quote wildcard filters in the
shell, for example `--accept "5:*"`. Up to four `--accept` or four `--drop`
pairs can be sent to the hardware; the two modes cannot be combined.
