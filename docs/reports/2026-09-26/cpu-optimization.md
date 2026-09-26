# Native reader CPU optimization

The native reader now publishes packets in batches of up to 256, reducing
queue locking, consumer wakeups, atomic counter updates, and host-clock
reads. Two workload comparisons showed approximately **23% and 33% lower
process CPU usage**, with no packet loss in either optimized run.

This is an algorithmic improvement that keeps the existing CPU compatibility.
It does not enable AVX-only code, host-specific instruction sets, busy polling,
or additional reader threads.

## Implementation

- `src/usbpv_capture_queue.hpp` owns callback validation, counters, and a
  preallocated 256-slot staging buffer. The native callback stages validated
  packets; publishing counts only the prefix accepted by the queue and records
  any rejected suffix as queue loss. Successful-packet counters are aggregated
  per publication, and host activity time is sampled per batch/transfer.
- `src/usbpv_queue.hpp` owns the bounded queue and adds `push_batch` under one
  existing mutex acquisition. It copies metadata and actual payload bytes.
  The queue still supports concurrent producers, and the legacy callback path
  still publishes individual packets.
- `src/usbpv_native.cpp` adds an optional transfer-end hook. It flushes partial
  batches after each parsed USB transfer, including parser-error returns, and
  on reader exit. Short captures, idle waits, stop, and error paths therefore
  do not strand partial batches. The original native callback API remains
  available without the hook.

The staging buffer adds 528,384 bytes (516 KiB) with the tested Windows slot
layout. The default 16,384-packet queue and 32 MiB output pool are unchanged.
USB packet timestamps and encoded pcapng bytes are unchanged. Host activity
and accepted-packet counters update at publication boundaries; packet-count
stops retain their existing polling/batching overshoot behavior.

## Measurements

The workload and external CPU sampler are described in the
[earlier CPU profile](cpu-profile.md). Each run writes a new 64 MiB
temporary file to the verified E: USB drive, then performs 15 seconds of
uncached mixed reads with data checks. NAK and SOF filtering are disabled
(`0xFF`), the monitor connection is SuperSpeed, and pcapng is written to D:.
Compilation and large-file analysis did not overlap the measurements.

Both binaries use the normal MinGW Release settings (`-O3 -DNDEBUG`), without
per-packet profiling instrumentation. The saved baseline executable predates
the queue changes. The final pair runs that baseline immediately before the
final optimized executable.

**100% CPU means one fully occupied logical processor.** The host has 16
logical processors; divide by 16 for a share of whole-machine CPU capacity.

| Measurement | Earlier baseline | First batched run | Repeated baseline | Final batched run |
| --- | ---: | ---: | ---: | ---: |
| Load measurement window | 31.863 s | 31.609 s | 31.658 s | 31.775 s |
| Process CPU time | 12.422 s | 9.516 s | 17.828 s | 12.063 s |
| Average CPU, one-core basis | 38.99% | 30.10% | 56.31% | 37.96% |
| Average CPU, whole-machine basis | 2.44% | 1.88% | 3.52% | 2.37% |
| Captured packets | 27,875,545 | 27,622,429 | 27,758,887 | 27,794,948 |
| Captured NAKs | 13,024,998 | 12,898,576 | 12,968,352 | 12,989,116 |
| Queue drops / hardware overflows | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

The first comparison reduced average CPU by 22.8%; the adjacent final pair
reduced it by 32.6%. Baseline CPU varied substantially between measurements.
These are observed workload comparisons, not a guaranteed speedup or a new
maximum USB throughput measurement. CPU frequency, scheduling, and background
conditions were not fixed. The two optimized captures contain 55,417,377
packets in total; packet rates and workload volumes are comparable but the
streams are not identical replays.

A direct SRW-lock experiment was also measured and discarded: it used 51.1%
of one core in its run and increased consumer CPU relative to the initial
baseline. The retained implementation uses batching with the original mutex
and condition-variable synchronization.

## Correctness and regression checks

- MinGW and MSVC Release builds pass all four CTest suites.
- Queue tests cover 500,000 packets with single/concurrent producers, mixed
  individual and batched insertion, full queues, accepted-prefix semantics,
  wrapping, sleeping consumers, stop notification, and final draining.
- Callback tests cover startup flushing, a full 256-packet batch, a partial
  transfer tail, acceptance stopping before final publication, event ordering,
  packet/byte/event/overflow counters, invalid data, speed mismatch, queue loss,
  repeated empty flushes, and immediate legacy callbacks.
- Independent pcapng validation of both optimized runs and the repeated
  baseline found no invalid PIDs, CRC5/CRC16 failures, or backwards timestamps.
  Saved packet counts match the summaries.
- Hardware regression passed ten captures and 25,058 packets, including
  rapid restarts, idle timeout, packet-count stop, idle stop-file exit, exclusive
  access, and recovery after an output-open failure. TShark checked the sustained
  capture without CRC/PID or timestamp errors.
- All workload data checks passed; each temporary drive file was automatically
  deleted.

## Local evidence

- `diagnostics/artifacts/2026-09-26/bottleneck/cpu-20260926-232601`: earlier baseline.
- `diagnostics/artifacts/2026-09-26/bottleneck/cpu-20260926-233917`: first batched run.
- `diagnostics/artifacts/2026-09-26/bottleneck/cpu-20260926-234209`: repeated saved baseline.
- `diagnostics/artifacts/2026-09-26/bottleneck/cpu-20260926-234246`: final batched run.
- `diagnostics/artifacts/2026-09-26/bottleneck/cpu-20260926-233524`: discarded SRW experiment.
- `diagnostics/artifacts/2026-09-26/bottleneck/before-cpu-opt.exe`: saved baseline executable.
- `diagnostics/artifacts/2026-09-26/native/batched-validation`: hardware regression artifacts.

Each CPU run includes `cpu.json`, `samples.json`, workload logs, capture and
summary. The validated runs also include `analysis.json`. Raw captures and diagnostic binaries remain in the ignored dated archive.
