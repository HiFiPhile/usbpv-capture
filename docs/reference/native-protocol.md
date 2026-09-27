# Native CH56x capture

Capture uses direct WinUSB access on Windows and libusb on Linux. The CLI,
pcapng writer, event sidecar, and bounded writer queue form the application
interface.
Only CH56x sniffers are supported; older FTDI-based sniffers are unsupported.

## Crash diagnosis

The bundled x64 `usbpv_lib.dll` uses an unsynchronized `std::list` between its
USB receive and parsing threads. Its semaphore counts available buffers but
does not serialize append/removal. If the consumer removes the last node while
the producer is appending, the producer writes through a freed tail pointer.
The consumer subsequently pops the list sentinel as a data node. Its count
field becomes buffer address `1`.

Controlled scheduling on actual ISO traffic reproduced both failures:

- Read address `1` at DLL RVA `0x28A43` (matching the application-error dialog).
- Write address `0xFFFFFFFFFFFFFFF9` at DLL RVA `0x28A94` when recycling that
  invalid buffer.

The original, unmodified vendor DLL/runtime bundle reproduced the same race;
disabling SOF did not fix it. Ten unforced debugger runs passed, so the
controlled reproduction establishes the mechanism, not natural crash frequency.
Local full dumps, debugger source, disassembly, and the detailed report are
under `diagnostics/artifacts/2026-09-26/vendor-crash/` (ignored because dumps contain process memory).

## Windows transport and error handling

One native reader thread owns an eight-entry ring of 64 KiB WinUSB reads (512 KiB
total transport reserve) and the stream parser. Callbacks copy complete packets into the existing locked,
bounded writer queue. There is no vendor producer/consumer list or vendor
capture thread in this path. The device handle is opened exclusively.
Enumeration and serial selection skip individual interfaces that cannot be
opened or identified, so a busy sniffer does not block another available one.
If every probe fails, enumeration reports the first probe error; configuration
and startup failures on the selected sniffer still fail capture.

Start waits for the device's start marker. Stop waits for the stop marker
with a two-second deadline, then cancels and reaps every remaining overlapped
read before freeing buffers or handles. Cancellation alone does not release
buffer ownership; see Microsoft's [CancelIoEx contract](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex).
Register and output-pipe transfers have one-second timeouts. Idle stream reads
remain pending and are polled every 20 ms so idle buses do not lose partial
transfers to periodic read timeouts. While a partial callback batch is pending,
the reader uses a 1 ms wait and publishes that batch on timeout. Complete
records are decoded directly from read buffers; split records use the parser's
fragment buffer. Callback payloads are copied before those read buffers are reused.

Malformed records/checksums, transport failures, missing start/stop markers,
hardware error flags, device overflow, and writer-queue loss cannot produce
a successful final summary. Parser failures are sticky; the parser does not
silently search USB payloads for a plausible new boundary.

## Linux transport

The Linux backend uses a private libusb context and one capture thread. It
claims the bulk interface exclusively, checks the same manufacturer, revision,
serial, and endpoints as Windows, and runs the same FPGA/register sequence.
It never loads the vendor capture engine.

Linux queues 32 asynchronous 16 KiB reads (512 KiB total). Optional
`libusb_dev_mem_alloc` buffers avoid the usbfs completion copy; allocation
failure falls back to ordinary buffers. Completions only mark reads ready.
The reader parses them in submission order and requeues them after callbacks
have copied their payloads. One event dispatch can complete several reads.
During an active SuperSpeed capture, the reader waits 250 microseconds before
an event dispatch when the next read is not yet ready. This groups short USB
completions and reduces Linux kernel wakeup overhead; submitted reads continue
receiving during that interval. USB 2.0 and start/stop handshakes skip this wait.
The interval is a service target, not a hard timing guarantee under scheduling
load. Windows retains eight 64 KiB WinUSB reads and its existing policies.

Stream reads have no transfer timeout. Event waits are bounded to 20 ms,
shortening to 1 ms when a callback batch is pending. Start and stop markers
retain two-second deadlines. All submitted reads are cancelled and their
callbacks reaped before freeing transfers, DMA memory, or the USB handle,
including partial startup failures. This follows libusb's
[asynchronous transfer ownership contract](https://libusb.sourceforge.io/api-1.0/group__libusb__asyncio.html).

The `native_linux` CTest suite injects libusb failures without hardware. It
covers partial submissions, disconnect completions, malformed streams,
missing start/stop markers, split records, reordered completion callbacks,
DMA and fallback buffers, and cleanup ownership. See the
[Linux load measurements](../reports/2026-09-27/linux-native.md) for hardware
validation and the limits of the tuning results.

## Recovered wire format

The protocol was recovered from the bundled SDK and verified against USBPV
serial `R9GF6P0` (CH56x, USB device revision `0x1701`). The symbol-bearing
Linux SDK functions `cha_pv_transaction`, `ov_load_bit_data`,
`USBPV_private::open`, `upv_open_device`, and `USBPV_private::on_byte` provide
the register, initialization, filter, and stream definitions.

Identification: VID/PID `16c0:05dc`, manufacturer `tusb.org`, revision high byte
`0x17`, WinUSB interface GUID `{1D4B2365-4749-48EA-B38A-7C6FDDDD7E26}`.
Bulk OUT is `0x01`; bulk IN is `0x81`.

Register commands and replies are four bytes: `55 command value checksum`.
`command` is the address for a write or `address | 80` for a read. Read requests
set value to zero; the checksum is the sum of the first three bytes modulo 256.
Both read and write responses are checked for address and checksum, and write
responses must echo the value.

| Operation | Encoding |
| --- | --- |
| Read FPGA status | Control `C0 75`, value/index 0, two bytes |
| Read application version | Control `C0 75`, value 0, index 1, four bytes |
| Begin volatile FPGA configuration | Control `40 73`, value/index 0, no data |
| Configure FPGA | Bulk OUT bitstream, then poll status low nibble for 3 |
| Start FIFO application | Control `40 74`, value/index 0, no data |
| Capture start / stop | `55 01 01 57` / `55 01 00 56`, echoed on IN |
| Capture speed | Register `08 = 0C | speed` (high 0, full 1, low 2) |
| Packet filter | Register `1F = ~requested_flags` |
| Address/endpoint filters | Registers `20..27`, four pairs |

Initial register writes: `46=10`, `47=17`, `48=11`, `4A=28`; read `64`, then
write `01=50` if its low bit is set, otherwise `01=A0`; write `00=30`, `49=40`.
All numbers in this section are hexadecimal unless stated otherwise.

In a filter pair, address/endpoint bit 7 enables the corresponding comparison;
endpoint bit 6 enables the pair. Endpoint bit 5 selects accept versus drop
for the table. Wildcards leave comparison bits clear. See `filter_registers`
and its golden command tests for the empty-table special cases.

| Stream record | Layout |
| --- | --- |
| Data | `6S`, 24-bit LE ticks, 16-bit LE length, payload, padding |
| Reset/suspend begin/end | High tag nibble `1/2/3/4`, 24-bit LE ticks |
| Overflow | `FF`, one status byte, two padding bytes |
| Stop | `55 01 00 56` |

Data length uses the low 14 bits, bounded to 1..1050 decimal bytes. The entire
data record, including its six-byte header, is padded to a four-byte boundary.
Padding is opaque and often nonzero. The low two tag bits encode high/full/low/
unknown speed as 0/1/2/3; bits 2..3 are hardware error flags. Payload contains
the original USB PID, bytes, and CRC. Transfers may end anywhere in a record.

The 24-bit clock runs at 60 MHz. Wraps are extended before conversion to
nanoseconds. After a host-observed idle gap longer than one counter period
(about 280 ms), absolute timing is re-anchored to host time because the number
of unobserved wraps is ambiguous. Absolute timestamps include transport latency;
continuous-stream intervals come from the device clock.
Overflow records carry no ticks: their event timestamp repeats the last timed
record (or uses host time before the first timed record). They never advance
the device-clock accumulator or refresh its idle-gap anchor.

## FPGA image provenance

The native backend reuses the SDK's existing FPGA image, not its executable
capture engine. `tools/embed-fpga.cmake` verifies the complete source library's
SHA-256 before extracting `firm_ng` at ELF file offset `0x29260`, length
`0xABBF5` (703,477 bytes). The generated header is a build artifact. This image
is loaded into volatile FPGA configuration on each open, as in the vendor SDK;
application/bootloader flash programming is not implemented.

- Source: `vendor/linux-x64/libusbpv_lib.so`
- Source SHA-256: `e0a7ecb5b6748fb13dce9acb035ddb3bd45cdbdf07336911f965b3d3d0967a69`
- Image SHA-256: `04faecd4c1dc95a60b2bc0df32096a0c610a4e84f5d4287382578072dbf1db2a`

The existing vendor redistribution caveat still applies to this image.
No vendor DLL/SO needs to accompany a deployed native executable. Linux uses
the system libusb runtime.

## Validation

`ctest --test-dir build --output-on-failure` exercises all single split points
and 1..65-byte chunks of mixed records, maximum payloads, every padding length,
timer/second wrap, idle re-anchoring, bus events, error flags, malformed lengths,
unknown tags, checksums, and filter encodings. Assertions remain active in
Release builds.

`tests/hardware_native.py` is an explicit hardware test, not part of CTest.
With active high-speed traffic, it performs sustained SOF captures, rapid
restarts with and without SOF, filtered idle shutdown, and packet-count shutdown.
It checks summaries and independently decodes sustained captures through
TShark for CRC/PID errors and backwards timestamps. Use a new output directory:

```powershell
python tests/hardware_native.py --exe build/usbpv_capture.exe --output build/soak
```

Hardware results on 2026-09-26, serial `R9GF6P0`, ongoing RT1064 high-speed
ISO audio traffic:

| Check | Result |
| --- | --- |
| Five 20-second SOF captures, 50 rapid restarts, idle and packet-count exits | 57 successful captures, 2,994,182 packets |
| Final integration suite including output-open failure recovery, exclusive access, and stop-file exit on an idle bus | 10 successful captures, 85,181 packets; expected errors returned cleanly |
| Independent TShark decode of all six sustained suite captures | No CRC/PID errors or backwards timestamps |
| Final sustained capture ISO cadence | 24,238 SOFs, 124,983..125,000 ns spacing; all 24,236 consecutive OUT7 intervals advance exactly one microframe |
| Five-second capture under the external debugger | 196,736 packets; exit 0; module log contains no vendor capture DLL/runtime |
| MinGW GCC and MSVC x64 Release builds | Both built without warnings and passed parser CTest |
| Native install | Executable only in `bin`; no vendor DLL deployment needed |

All successful hardware summaries reported zero device overflows, writer-queue
drops, invalid packets, and speed mismatches. Local captures/results are in
`diagnostics/artifacts/2026-09-26/native/soak`, `diagnostics/artifacts/2026-09-26/native/final-validation`, and
`diagnostics/artifacts/2026-09-26/native/final-debug.*`. The diagnostic build in `build/usbpv_capture.exe`
uses `native-winusb` by default, including when old vendor DLLs remain beside it.

Physical low/full-speed capture, cable-removal faults, and older CH56x
revisions require further hardware coverage. SuperSpeed monitor load has since
been tested on both platforms; see the [Linux write/read and burst results](../reports/2026-09-27/linux-native.md). Passing
an ISO soak is not a maximum-throughput guarantee.

Subsequent [USB storage stress testing with NAKs enabled](../reports/2026-09-26/usb-storage-load.md)
over a SuperSpeed monitor connection sustained about 0.88 million recorded
packets/s without crashes, but reported application queue drops with both
16,384- and 65,536-packet queues. Follow-up profiling traced these losses to
synchronous file-write stalls. The [buffered-output fix and validation](../reports/2026-09-26/capture-bottleneck.md#implemented-fix-and-validation)
repeated that load without drops, including three injected 151..161 ms output
stalls. Consult the capture's completeness counters for every run.
