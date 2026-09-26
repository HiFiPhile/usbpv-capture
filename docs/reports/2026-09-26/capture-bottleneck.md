# Capture bottleneck: synchronous file-output stalls

The immediate cause of the reproduced packet loss is a long synchronous
file-write call on the same thread that drains the packet queue. The writer
has ample average throughput, but cannot drain packets during that call.

This diagnosis profiles the implementation committed as `f66e54b`. Production
source and `build/usbpv_capture.exe` were unchanged during diagnosis; the
subsequent fix and its validation are recorded below. Instrumented binaries,
profiling generators, and captures are local artifacts under `diagnostics/artifacts/2026-09-26/bottleneck/`.

## Reproduced failure and timing correlation

Run `diagnostics/artifacts/2026-09-26/bottleneck/io-20260926-225928` used the original workload: write a
new 64 MiB file on the verified USB drive, then perform 15 seconds of uncached
mixed reads. Capture enabled all packet types (`0xFF`), used the default
16,384-packet queue, and ran over the SuperSpeed monitor connection.

The diagnostic build wraps the `write` function called by MinGW libstdc++'s
`std::__basic_file<char>::xsputn`. It measures each call's elapsed time and
thread CPU cycles. Queue-drop timestamps use the same monotonic clock.

| Observation | Measurement |
| --- | ---: |
| Slow file write begins, relative to queue construction | 30,915.2447 ms |
| Bytes requested by that call | 4,092 |
| Call duration | 60.9565 ms |
| CPU cycles consumed across that call | 146,907 |
| First queue drop | Approximately 30,930.3 ms |
| Time to fill the queue after write begins | Approximately 15.1 ms |
| Last sampled queue drop | Approximately 30,975.7 ms |
| File write returns | 30,976.2012 ms |
| Queue high-water mark | 16,384 (full) |
| Total dropped packets | 49,637 |

All recorded drop samples fall inside this one file-write interval. The first
100 drops and every thousandth subsequent drop were timestamped, rather than
adding a clock call for every packet. The 49,637 total is the normal capture
loss counter. The writer-loop measurement independently reports a 61.0016 ms
batch duration covering the same call.

There were no device overflows, parser-invalid packets, speed mismatches, or
crashes. The largest reader parsing/callback interval was 0.984 ms, queue-pop
critical section 0.531 ms, and queue-lock acquisition delay 0.436 ms.
Periodic explicit flushes peaked at 0.0285 ms. The long stall occurred while
writing packet records, not in the once-per-second `flush()` call.

The slow call used very few CPU cycles for its 61 ms elapsed time. This points
to waiting/descheduling within the file-write path rather than spending that
interval encoding pcapng. Filesystem, cache manager, storage/filter driver,
and scheduler attribution would require a further OS trace; this experiment
does not identify a particular driver, SSD, or antivirus component.

## Controlled comparisons

All live variants use the same parser and packet queue. Diagnostic `null`
serializes pcapng into Windows `NUL`; `discard` drains the queue without
serializing packets. These variants are diagnostic sinks, not valid captures.
Runs were sequential; no large-file analysis or compilation overlapped them.

| Variant | Read phase | Packets | Writer CPU | Longest writer batch | Queue peak | Drops |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Real file | 5 s | 20,237,136 | 8.05 s | 5.99 ms | 6,426 | 0 |
| NUL, same serialization | 5 s | 20,131,649 | 5.56 s | 0.72 ms | 1,171 | 0 |
| Queue drain only | 5 s | 20,141,317 | 4.05 s | 0.06 ms | 920 | 0 |
| Real file | 15 s | 29,165,977 | 12.16 s | 19.77 ms | 14,584 | 0 |
| NUL, same serialization | 15 s | 27,879,819 | 9.09 s | 0.85 ms | 1,044 | 0 |
| Real file, individual write timing | 15 s | 27,567,305 | 11.50 s | 61.00 ms | 16,384 | 49,637 |

The failure is intermittent. Several lossless runs therefore do not establish
that the queue can absorb the worst output stalls. Profiling also changes
timing, so these runs establish the mechanism rather than its natural frequency.

## Throughput and secondary costs

A separate single-thread benchmark replays exactly the same 100,000 saved
packet records 50 times. Input preparation is outside the measurement. It
calls the production `PcapngWriter` and compares it with a diagnostic encoder
that packs records into 512 KiB output chunks.

| Five-million-packet benchmark | Original writer | Packed output experiment |
| --- | ---: | ---: |
| Windows NUL sink | 12.08 million packets/s | 79.44 million packets/s |
| File on D: | 7.80 million packets/s | 37.73 million packets/s |

These measure isolated output processing, not end-to-end USB limits. They show
that the observed live average of about 0.9 million packets/s is not a steady
writer-throughput ceiling. Chunking offers substantial CPU/I/O-call reduction,
but its isolated benchmark does not prove lossless live capture.

The profiled failure made 591,620 low-level file-write calls for 1,227,245,696
bytes: only about 2,074 bytes per call on average. `write_packet` performs
eight four-byte stream writes plus payload and padding writes per packet,
with the standard stream buffer determining the smaller underlying I/Os.

Two additional costs reduce headroom:

- `PacketQueue::pop_batch` copies a complete 2,064-byte `PacketSlot`, even
  for one-byte NAKs. In the first 20.2-million-packet run this moves about
  41.8 GB of slot data; the pop critical sections totaled 2.07 seconds.
- `push` calls `notify_one()` for every packet. In that same run, 19.62 million
  of 20.24 million notifications occurred when the queue was already nonempty;
  actual consumer batches averaged about 33 packets. This is measurable
  overhead, but it was not the 61 ms delay that caused the reproduced loss.

Relevant production code: `src/usbpv_capture.cpp`, `PacketQueue::push`,
`PacketQueue::pop_batch`, `PcapngWriter::write_packet`, and `writer_thread`.

## Fix direction

Batch encoded pcapng records into substantial byte buffers and decouple their
consumption from blocking file writes, using a bounded pool of pending output
buffers with correct completion ownership. Size that byte reserve for measured
stall duration and peak encoded-byte rate, and keep explicit loss reporting
when exhausted. Compact packet copies and notify only when needed to reduce
CPU/memory overhead. A larger packet queue alone provides a finite additional
stall allowance and cannot guarantee losslessness.

The next validation should repeat the original live load, preserve byte-for-byte
pcapng correctness, and include a controlled output stall exceeding 61 ms to
test the buffering guarantee. The following implementation was validated
against these requirements.

## Implemented fix and validation

Follow-up [CPU profiling](cpu-profile.md) measured the updated
production executable at 39.0% of one logical core under this workload
(2.44% of the 16-thread host). The reader/callback path dominates CPU after
the output fix; the labelled diagnostic file writer used 1.6% of one core.

`src/usbpv_output.cpp` implements a fixed pool of 32 one-MiB buffers. The
packet-processing thread encodes each complete pcapng record directly into
its active buffer, then publishes filled buffers to a dedicated file-I/O
worker. Ownership returns only after the write completes. Bus-event JSONL
uses a separate 256 KiB pool and worker. Flush requests are ordered and
asynchronous; shutdown drains pending output, joins both workers, and checks
flush/close errors. Exhaustion is an explicit capture failure rather than an
unbounded allocation or a blocked packet consumer.

Queue batches now copy metadata and actual payload length, and `push` wakes
the consumer only when the queue changes from empty to nonempty. The default
16,384-packet queue is unchanged. Ready JSON exposes `output_buffer_bytes`;
the final summary reports `output_pending_peak_bytes` for published/in-flight
pcapng buffers, excluding the partially filled producer buffer.

Both live runs below repeated the original 64 MiB write plus 15-second mixed
uncached read workload, with all packet types enabled (`0xFF`) and the default
packet queue. Compilation and large-file analysis did not overlap these runs.

| Measurement | Production build | Diagnostic build with injected write stalls |
| --- | ---: | ---: |
| Capture duration | 31.499 s | 31.580 s |
| Captured packets | 27,680,128 | 27,808,424 |
| Captured NAKs | 12,949,781 | 13,017,034 |
| Queue drops / hardware overflows | 0 / 0 | 0 / 0 |
| Published output peak | 1,048,576 bytes | 7,339,764 bytes |
| Injected file-write delays | None | 160.949, 153.276, 151.448 ms |
| Complete / exit status | true / 0 | true / 0 |

The diagnostic binary links an isolated `write` wrapper that sleeps before
large pcapng writes numbered 100, 500, and 900. It otherwise builds the same
production sources. The production executable contains no injected delays.
These stalls exceed the original 60.9565 ms failure by more than 2.4 times;
the output backlog remained below 7.0 MiB of the 32 MiB pool. This demonstrates
that the consumer continues during disk stalls, without increasing the packet
queue. It does not guarantee losslessness during arbitrarily long stalls or
prove a new maximum USB throughput. Shutdown still waits for file I/O.

An independent reader checked all 55,488,552 saved packets: zero invalid PIDs,
CRC5/CRC16 errors, or backwards timestamps; file record counts match both
summaries. Both workload data checks passed and temporary drive files were
automatically removed.

MinGW and MSVC Release builds pass both CTest suites. The output suite checks
every payload length from 0 through 2,048 against the previous pcapng encoding,
holds a sink blocked for 150 ms while the producer queues over 16 MiB, verifies
exact ordering and tail draining, tests pool reuse, and checks bounded
exhaustion plus write/flush/exception failures.

The hardware regression completed ten captures covering restarts, idle exit,
stop-file exit, exclusive access, and recovery after an output-open failure.
TShark checked the sustained capture without CRC/PID or timestamp errors.
The packet-count stop was then checked separately with SOFs enabled: it
stopped for `max_packets`, drained its partial output buffer, and returned
success with 208 packets. The regression now enables SOFs and asserts that
exit reason so an idle storage device cannot silently substitute a duration
exit for the packet-count test.

Local evidence:

- `diagnostics/artifacts/2026-09-26/native/storage-load-20260926-231543`: production workload, summary,
  and full packet analysis.
- `diagnostics/artifacts/2026-09-26/native/storage-load-20260926-231702`: injected-stall workload,
  measured delay log, summary, and full packet analysis.
- `diagnostics/artifacts/2026-09-26/bottleneck/stall_io.cpp` and `stall_capture.exe`: isolated diagnostic
  wrapper and binary.
- `tests/output_test.cpp`: portable deterministic output regression tests.
- `diagnostics/artifacts/2026-09-26/native/buffered-validation`: hardware regression artifacts and the
  separate `packet-stop-verified` capture.

## Local evidence

- `diagnostics/artifacts/2026-09-26/bottleneck/io-20260926-225928/profile.json`: file-write intervals,
  thread CPU cycles, writer timings, queue peak, and drop timestamps.
- `diagnostics/artifacts/2026-09-26/bottleneck/profile-20260926-225606/profile.json` and
  `null-20260926-225639/profile.json`: full-length file/NUL comparison.
- `diagnostics/artifacts/2026-09-26/bottleneck/profile-20260926-225432`, `null-20260926-225455`, and
  `discard-20260926-225517`: shorter comparison runs.
- `diagnostics/artifacts/2026-09-26/bottleneck/make_profile.py`, `profile_io.cpp`, `run_profile.py`, and
  `bench_writer.cpp`: diagnostic instrumentation and benchmark sources.

All USB workload data checks passed, and all temporary drive files were deleted.
