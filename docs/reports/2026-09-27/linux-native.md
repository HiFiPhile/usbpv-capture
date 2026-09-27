# Native Linux backend and USB disk tuning

Linux uses `native-libusb` for CH56x sniffers. It implements FPGA
loading, register setup, exclusive interface ownership, asynchronous reads,
start/stop handshakes, cancellation/reaping, and error propagation. It reuses
the protocol parser and bounded callback/output pipeline. See the
[capture guide](../../guides/agent-capture.md) for build and usage instructions.

The Linux transport uses **32 reads of 16 KiB**, retaining the same 512 KiB
reserve as Windows. Optional libusb DMA allocations avoid the Linux usbfs
completion copy, with ordinary buffers as a fallback. Windows keeps its
existing eight 64 KiB WinUSB reads. Linux SuperSpeed capture also groups
completions over a 250-microsecond target interval, as measured below.
No CPU affinity, scheduler priority,
kernel parameters, or global filesystem settings are changed.

## Measured workload

Tested on 2026-09-27 with sniffer `R9GF6P0`, revision `0x1701`, libusb 1.0.30,
and USB flash disk `0011:7788`, serial `C4D197AC`. The sniffer monitor link was
**high-speed USB 2.0**, unlike the SuperSpeed link in the Windows reports.
The captured bus was also high speed.

The initial test used read-only I/O. The sandbox showed the mounted filesystem
as read-only, which was initially mistaken for the host's mount state. A later
check outside the sandbox confirmed that the host mount was writable; no
remount was needed. The initial Linux runner opens the verified block device with `O_RDONLY | O_DIRECT` and alternates 1 MiB sequential reads
and 4 KiB random reads in two-second phases over its first 64 MiB, for 15 seconds.
It does not create files, write disk sectors, or change mount options. This
is a different workload from the Windows write/read test.

All captures enable NAK and SOF, use the default 16,384-packet queue and 32 MiB
output pool, and write captures to the workspace's NVMe volume. CPU comes from
external `/proc/PID/stat` samples over the load interval. **100% means one
logical processor.** No compilation or capture analysis overlaps measurement.

| Run | Capture CPU, one core | Disk read MiB/s | Captured packets |
| --- | ---: | ---: | ---: |
| `baseline-8x64k` | 54.07% | 10.34 | 11,739,237 |
| `experiment-32x16k` | 43.87% | 10.64 | 11,656,610 |
| `experiment-dma` | 38.93% | 10.27 | 11,758,496 |
| `repeat-baseline` | 54.20% | 10.34 | 11,713,497 |
| `repeat-dma` | 39.86% | 10.50 | 11,692,364 |

The initial baseline used eight 64 KiB ordinary libusb buffers. The 32 x 16 KiB
experiment changed only the ring geometry. The DMA experiment added optional
DMA-backed buffers. The final repeated baseline/optimized pair reduced CPU
from 54.20% to 39.86%, about **26.4%**, with comparable disk throughput. The
first pair reduced CPU from 54.07% to 38.93%. These are live workload results,
not identical stream replays or a maximum-throughput guarantee.

Across all five runs, **58,560,204 packets** were recorded with zero queue
loss, device overflows, invalid packets, or speed mismatches. The independent
analyzer checked every recorded packet: no PID/CRC errors or backwards
timestamps, and pcapng packet counts matched the summaries. Peak pending
pcapng output was 1 MiB in every run.

## Initial SuperSpeed monitor validation

A follow-up on 2026-09-27 tested the same sniffer with a negotiated **5 Gb/s
SuperSpeed monitor link**. `lsusb -t`, the device descriptors (1024-byte bulk
endpoint packets), and every load capture's `monitor_port: "super"` ready
record confirmed the connection. The captured disk bus remained high-speed
USB 2.0. This tests SuperSpeed monitor transport, not 5 Gb/s captured traffic
or maximum monitor throughput.

The same read-only disk workload, capture flags, queue sizes, and NVMe output
were used. No source changes were needed. On kernel `7.2.8-1-cachyos` with
libusb 1.0.30, results were:

| Run | Load duration | Capture CPU, one core | Disk read MiB/s | Captured packets |
| --- | ---: | ---: | ---: | ---: |
| `superspeed-optimized-01` | 15 s | 62.40% | 10.34 | 11,729,231 |
| `superspeed-baseline-01` | 15 s | 72.40% | 10.62 | 11,670,220 |
| `superspeed-optimized-02` | 30 s | 61.66% | 10.43 | 23,423,043 |

All **46,822,494 packets** across the three runs passed the independent
PID/CRC/timestamp analyzer, and record counts matched capture summaries.
Every run completed with zero queue drops, device overflows, invalid packets,
or speed mismatches. Peak pending pcapng output stayed below 1.02 MiB.

The Linux tuning remains beneficial on this SuperSpeed workload: approximately
62% of one core versus 72% with the original eight 64 KiB non-DMA reads.
Absolute CPU usage is higher than the earlier USB 2.0 monitor results. The
cause was not profiled in this follow-up, and one baseline run does not establish
a precise speedup. These were the initial settings; the completion-grouping follow-up below
addresses this CPU gap.

The SuperSpeed hardware regression suite also passed **56 captures**, including
50 rapid restarts, two sustained captures, idle timeout, packet-count stop,
output-open failure recovery, exclusive access, and idle stop-file shutdown.
All 56 ready records reported a SuperSpeed monitor port. The suite recorded
56,358 packets; TShark found no CRC/PID errors or backwards timestamps in
either sustained capture.

Evidence is saved in the `superspeed-*` directories/files under
`diagnostics/artifacts/2026-09-27/linux/`. `superspeed-results.json` combines
CPU samples, summaries, full-capture analysis, kernel version, and SHA-256
hashes of both executables. USB topology and descriptors are saved alongside it.

## SuperSpeed CPU gap: diagnosis and completion grouping

The initial 62% Linux CPU result deserved further investigation. Workload
differences do not explain it away: recalculating the last approximately
14 seconds of the saved Windows read phases gives 20.1% and 27.5% of one
logical processor, close to their whole-workload measurements. Both platforms
measure process user plus kernel CPU time with 100% representing one core.

A Linux profile (`cpu-investigation-01`) found 64.1% total CPU: 14.1% user and
49.9% kernel. The USB reader alone used 12.8% user plus 47.9% kernel and had
402,431 voluntary context switches in approximately 15 seconds. Output and
packet processing accounted for only a small fraction of CPU. These are
external `/proc` thread counters; the `perf` profile sampled user stacks,
not kernel stacks.

The retained change waits **250 microseconds before dispatching USB events**
when the next submitted read is not yet ready, only during active SuperSpeed
capture. This lets completions accumulate and reduces repeated poll/wakeup
work. Reads remain submitted; completed reads are still parsed in submission
order. USB 2.0, startup, and stop-marker handling bypass the added wait.
No per-packet profiler, thread-priority change, or system-wide tuning is added.

A 100-microsecond diagnostic interval reduced CPU to 34.5%; 250 microseconds
reduced it to 23.0%, with unchanged disk throughput and no capture loss.
The production version uses the latter interval only for active SuperSpeed
capture. Its cost is a quarter-millisecond processing-delay target; scheduler
delays can exceed it. The existing callback publication target is 1 ms.

A second profile (`cpu-investigation-02`) measured 23.3% process CPU: 6.2% user
and 17.2% kernel. Reader voluntary context switches fell to **39,313**, about
90% fewer; reader CPU fell to 4.5% user plus 15.2% kernel. This strongly
supports frequent USB completion wakeups as the main cause of the initial
gap. It does not identify every kernel function involved.

Final unprofiled production runs measured **22.87%** and **22.73%** of one
core (`cpu-coalesced-final-01/02`), down from 61.7-62.4% before completion
grouping: roughly **63% less CPU**. Disk read throughput remained 10.44 and
10.36 MiB/s. The two runs recorded **23,413,659 packets**, all passing the
independent PID/CRC/timestamp analyzer, with zero drops, overflows, invalid
packets, or speed mismatches. Counts matched both summaries. The diagnostic
and profiled captures also passed full packet analysis.

All six CTest suites pass after the change, including fake SuperSpeed endpoints,
and the native transport fault tests pass ASan/UBSan/LeakSanitizer. The updated
hardware suite passed **56 captures** (56,336 packets), including 50 rapid
restarts, idle timeout, packet-count stop, output failure recovery, exclusive
access, and stop-file shutdown. Every ready record confirmed SuperSpeed.
TShark checked both sustained captures without CRC/PID or timestamp errors.
Evidence is in `cpu-coalesced-hardware/`.

Profile evidence is in `cpu-investigation-01/`, `cpu-investigation-02/`, and
`cpu-thread-comparison.json` under the same Linux artifact directory.
Diagnostic source variants, binaries, and profiling scripts are retained in
`experiments/`. The measured reduction is specific to this host and workload;
other traffic patterns still require throughput and loss validation.

## Matching the Windows write/read workload

The host USB mount was verified writable outside the sandbox on 2026-09-27.
The earlier claim that the drive itself was read-only was incorrect: it was
the sandbox's view. Two subsequent runs used the same application workload
as Windows on the same verified `C4D197AC` USB disk and SuperSpeed monitor link:

- Create one new 64 MiB temporary file, using 1 MiB sequential writes and the
  identical `(i * 131 + 17) & 255` byte pattern.
- Use uncached synchronous I/O (`O_DIRECT | O_SYNC` on Linux).
- Perform 15 seconds of alternating two-second phases of 1 MiB sequential
  and 4 KiB random reads, with the same PRNG, offsets, and byte verification.
- Automatically remove only the new file. No existing file is modified.
- Enable NAK/SOF, retain default capture buffers, and measure process user plus
  kernel CPU across the helper lifetime, with two-second idle capture intervals
  before and after, matching the Windows profiler.

| Run | Capture CPU, one core | Write MiB/s | Read MiB/s | Captured packets |
| --- | ---: | ---: | ---: | ---: |
| Saved Windows optimized `002157` | 20.11% | 4.31 | 10.38 | 27,645,070 |
| Saved Windows optimized `002710` | 27.69% | 4.25 | 10.32 | 27,827,021 |
| Linux `windows-workload-01` | 23.30% | 3.27 | 10.53 | 32,228,808 |
| Linux `windows-workload-02` | 23.34% | 3.58 | 10.67 | 29,094,405 |

Both Linux runs verified all returned read data and removed their temporary
files. All **61,323,213 captured packets** passed the independent PID/CRC and
timestamp checks, and counts matched the summaries. Queue drops, device
overflows, invalid packets, and speed mismatches were zero. Peak pending
pcapng output was 1 MiB in both runs.

Linux capture CPU is now in the range of the saved Windows measurements for
the matching workload. Linux writes were slower in these runs: 19.59 and
17.87 seconds for 64 MiB, versus about 15 seconds on Windows. Therefore load
and capture durations, and total packet counts, differ. The application I/O
pattern and verification match, but filesystem/driver flushing and USB command
sequences can differ between operating systems. These are live measurements,
not a controlled cross-OS speed ranking.

The helper and runner are `tools/diagnostics/usb_storage_load_linux.cpp` and
`tools/diagnostics/run_linux_storage_write_load.py`; see
[reproduction instructions](../../../tools/diagnostics/README.md#linux-workload-matching-windows).
Evidence is in `windows-workload-01/02` and
`windows-workload-comparison.json` under the Linux artifact directory. The
helper rejects invalid names, durations, and directories on other devices
before creating a file. It creates with `O_EXCL` and unlinks that new file
while open, giving cleanup on process exit. Linux I/O and unlink semantics
are documented in [open(2)](https://man7.org/linux/man-pages/man2/open.2.html)
and [unlink(2)](https://man7.org/linux/man-pages/man2/unlink.2.html).

## Burst margin and the reported 8 MB device FIFO

The user reports an 8 MB device FIFO. This capacity has not been independently
measured here. A 250-microsecond pause represents only 5 KB at an encoded
capture rate of 20 MB/s, or 25 KB at 100 MB/s. Using 8,000,000 bytes for the
FIFO, an initially empty FIFO would hold 400 ms or 80 ms respectively with
no draining. These rates are examples, not an asserted maximum sniffer rate.
Available headroom, rather than total capacity, determines the actual margin.
Raw USB payload throughput and pcapng file throughput are not FIFO byte rates.

A sliding-window analysis of the two matching Windows-workload captures
reconstructed native data record sizes as `align4(6 + payload_length)` and
used device-derived capture timestamps. The largest observed windows were:

| Window | Native record bytes | Packets | NAK packets |
| --- | ---: | ---: | ---: |
| 250 microseconds | 10,208 | 275 | 137 |
| 1 millisecond | 31,608 | 1,097 | 545 |
| 10 milliseconds | 277,336 | 10,946 | 5,433 |

Each column is its independent maximum across both captures; the maxima need
not occur in the same window. The largest 250-microsecond encoded burst is
about 0.13% of the reported FIFO capacity. This is a reconstructed stream
size, not a direct measurement of FIFO occupancy or its internal storage format.

An additional diagnostic executable retained the production 250-microsecond
coalescing and deliberately stalled only the reader during the write phase
of the matching USB workload. Requested stalls of **5, 10, and 20 ms** measured
**5.053, 10.055, and 20.053 ms**. These let input accumulate and exercise
backlog recovery; they are not a synthetic maximum-rate NAK generator.

The resulting capture completed with **31,780,694 packets**, including
**14,994,535 NAKs**, and zero device overflows, application queue drops, invalid
packets, or speed mismatches. Full independent analysis found no PID/CRC errors
or backwards timestamps, and packet counts matched the summary. All disk read
verification passed and the temporary file was removed. Peak pending output
was about 2 MiB. The ordinary 250-microsecond sleeps reached at most 521
microseconds in this run, excluding the deliberately injected stalls.

Transport diagnostics counted 2,447,918 completed reads and 549,815,416 bytes,
including startup bytes, with 22 full 16 KiB reads. These short completions
matter: 32 queued 16 KiB reads are nominally 512 KiB, but a short completion
retires an entire read slot. Do not treat the host ring as guaranteed 512 KiB
of useful buffering for short-packet traffic. Libusb dispatch can also reap
only part of the available completions; callback counts do not measure FIFO
occupancy.

This supports retaining the tweak for the tested NAK-heavy bursts and backlog
recovery: the largest injected stall was 80 times the requested coalescing
interval. It is not a guarantee for a maximum-rate NAK storm. Long scheduling
stalls, an already occupied FIFO, sustained arrival rates above drain capacity,
and exhaustion of the separate packet queue or output pool can still lose
capture data. For a sustained overload, FIFO fill time is approximately
`free_fifo_bytes / (input_bytes_per_second - drained_bytes_per_second)`.

The production binary is unchanged by this test. Evidence is in
`burst-stalls-01/` (`burst-validation.json`, capture, logs, and full analysis).
The earlier runs also have `burst-windows.json`. Diagnostic source and the
injected-stall executable are saved in `experiments/` under the Linux archive.

## Validation and reproduction

Linux CMake Release and Make builds succeed. All six CTest suites pass,
including the new mocked native transport tests. The transport suite also
passes AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer. It
checks both DMA and allocation-fallback paths, split records, reversed
completion callback order, submission failures at several ring positions,
disconnect completions, malformed input, marker timeouts, and cancellation
ownership. It links no real libusb implementation and opens no hardware.

The final hardware suite passed **56 captures**, including two sustained
captures, 50 rapid restarts, filtered idle timeout, packet-count stop,
output-open failure recovery, exclusive interface access, and stop-file
shutdown on an idle bus. It recorded 56,361 packets. TShark independently
checked both sustained captures with no CRC/PID or timestamp errors. This
suite used mostly idle traffic; the five disk-load runs provide load coverage.
An earlier smoke suite also passed 10 captures before tuning.

A staged CMake install contains the executable and public header, without a
vendor runtime. Build dependencies are CMake, pkg-config, and libusb development
headers (libusb >= 1.0.21). Deployment needs the system libusb runtime and USB
permissions. CI installs the Linux dependency and runs CTest on both platforms.
Windows was not rebuilt on this Linux host.

See [diagnostic tools](../../../tools/diagnostics/README.md#linux-read-only-usb-storage-load)
for commands. Evidence, JSON CPU samples, workload logs, captures, and independent
analysis are under `diagnostics/artifacts/2026-09-27/linux/`. The `experiments/`
subdirectory preserves the transport source variants and measurement binaries.

Physical low/full-speed captures and physical cable removal remain untested. Injected disconnect errors verify
cleanup but do not replace physical unplug testing. CPU gains may differ on
other hosts, controllers, kernel versions, or traffic patterns.
