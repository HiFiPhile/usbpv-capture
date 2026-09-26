# CPU follow-up: batching across USB transfers

The reader now combines partial callback batches across USB transfers instead
of waking packet processing at every transfer boundary. An adjacent comparison
used **27.7% of one logical CPU versus 38.8% for baseline**, a 28.6% reduction.
An earlier retained run used 20.1%, against earlier baseline runs of 29.9–31.6%.
Saved packets passed independent
CRC, PID, timestamp-order, and packet-count checks with zero capture drops.

This extends the [previous batching work](../2026-09-26/cpu-optimization.md).
All results below use the connected CH56x sniffer `R9GF6P0`, a SuperSpeed
monitor link, high-speed captured traffic, and all packets enabled (`0xFF`,
including NAK and SOF). Each CPU run writes a unique, automatically deleted
64 MiB file on the verified E: USB drive and performs 15 seconds of mixed
uncached reads with data checks. No existing drive files are modified.

## Retained changes

- Publish full 256-packet batches immediately. Partial batches span transfers
  with a 1 ms deadline that subsequent traffic cannot extend. When the reader
  waits with a pending batch, it uses a 1 ms timeout and flushes on timeout.
  Stop, parser failure, and reader exit also flush pending packets. Windows
  scheduling can exceed the target; this is not a hard latency guarantee.
- Publish total callback count and host activity per transfer, including
  callbacks discarded during startup. Accepted packet counters still update
  at queue publication. Empty timer flushes do not refresh activity time.
- Decode complete data records directly from the USB buffer, avoiding the
  fragment buffer's incremental copies. Split records retain the existing
  decoder; callbacks copy payloads before the USB buffer can be reused.
- Replace runtime queue-index division with increment-and-wrap, preserving
  arbitrary queue capacities. Avoid repeatedly acquiring the startup mutex
  after the reader has reported ready.

The transport remains eight 64 KiB reads (512 KiB total), with the original
WinUSB policy. Queue capacity, the 516 KiB callback staging buffer, and the
32 MiB output pool are unchanged. The code requires no newer CPU instruction
set. The legacy callback path still publishes immediately. USB timestamps
are unchanged; packet-count stops retain polling/batching overshoot.

## Locating the cost

An instrumented baseline (`000119`) spent 22.82% of one core in the reader,
7.24% in packet processing, and 1.34% in file output. It received 776,365 USB
completions for 514,847,200 bytes: about 663 bytes per completion, with 99.94%
at most 4 KiB. Only 3.18% were already complete at the sampled readiness
check. These small transfers prevented most 256-packet batches from filling
when every transfer forced publication.

The uninstrumented parser/counter/queue cleanup alone measured 31.64%,
against 31.62% immediately before it. Its isolated parser benchmark improved
by about 2.7 times, but that did not produce a measurable whole-process gain.
The benchmark alternates six before/after pairs in one process, uses the same
synthetic NAK-heavy stream and 512-byte feed chunks, and verifies matching
counts/checksums for 30 million packets per implementation. Only this isolated
benchmark pins its thread to one logical processor.

Coalescing transfers reduced both reader and packet-processing cost. In the
first retained run (`002157`), the reader used 16.52%, packet processing
2.02%, and file output 1.38% of one core, inferred from thread creation order
and the earlier labelled profile. Kernel time was 4.72 of 6.38 process CPU
seconds, so USB completion and synchronization work remain the main area
for further investigation. This is not a kernel stack attribution.

## Final workload comparison

The saved baseline is committed `e01c8ae`; the optimized binary uses the same
MinGW Release settings. Runs `002633` and `002710` are consecutive baseline
and optimized runs. `002157` is the first retained optimized run.

| Measurement | First optimized (`002157`) | Repeated baseline (`002633`) | Repeated optimized (`002710`) |
| --- | ---: | ---: | ---: |
| Measurement window | 31.693 s | 31.523 s | 31.831 s |
| Process CPU time | 6.375 s | 12.219 s | 8.813 s |
| CPU, one-core basis | 20.11% | 38.76% | 27.69% |
| CPU, whole-machine basis | 1.26% | 2.42% | 1.73% |
| Captured packets | 27,645,070 | 27,593,693 | 27,827,021 |
| Captured NAKs | 12,911,961 | 12,887,677 | 13,005,162 |
| Queue drops / device overflows | 0 / 0 | 0 / 0 | 0 / 0 |

Both optimized captures total 55,472,091 packets. Independent analysis also
checked the repeated baseline: summary counts match pcapng records, with zero
invalid PIDs, CRC5/CRC16 errors, and backwards timestamps. The reduction
repeats despite substantial absolute CPU variation; it is a workload result,
not a guaranteed speedup or maximum USB throughput measurement.

## Regression validation

- MinGW and MSVC Release builds pass all four CTest suites (protocol, callback,
  bounded queue, and output).
- Protocol tests cover all valid lengths 1..1050 with all 16 speed/error tag
  combinations, opaque padding, unaligned input, split records, and malformed
  records split at every boundary.
- Callback tests cover fixed deadlines under continued traffic, full batches,
  forced partial publication, stop acceptance, empty/idempotent flushes,
  activity accounting, queue loss, and immediate legacy callbacks.
- After reconnecting the sniffer, MinGW passed 55 hardware captures (50 rapid
  restarts), totaling 32,338 packets. MSVC passed 10 captures (five restarts),
  totaling 24,867 packets. Both suites covered idle timeout, packet-count stop,
  output-open failure recovery, exclusive access, and stop-file shutdown on
  an idle bus. TShark found no CRC/PID errors or backwards timestamps in either
  sustained capture. These restart tests used a mostly idle captured bus;
  the separate workload comparisons above provide heavy-traffic coverage.
- Before that reconnect, the first suite completed its 3-second capture and
  11 rapid restarts, then restart `011` returned a stop-marker timeout. The
  baseline also could not open that device state (register response, Windows
  error 31). The cause remains unestablished; reconnecting restored operation.
  Both failures returned explicitly, without a crash or false success.

The failed runs are preserved in
`diagnostics/artifacts/2026-09-27/native/coalesced-validation/` and
`baseline-restart-validation/`.
Passing reruns are in `coalesced-reconnected/` and
`coalesced-msvc-validation/` under the same native archive.

## Transport experiments

All entries use the same workload; single-run differences are not reliable
speedup estimates. Local run names have prefix `cpu-20260927-`.

| Run | Experiment | CPU, one-core basis | Decision |
| --- | --- | ---: | --- |
| `000325` | WinUSB RAW_IO | 114.95% | Discarded: 3.20 million completions, about four times baseline. |
| `001056` | Eight 4 KiB reads | 26.37% | Discarded: reserve shrank to 32 KiB. |
| `001230` | 32 reads of 16 KiB | 29.60% | No convincing gain. |
| `001355` | 128 reads of 4 KiB | 28.60% | Repeat did not confirm improvement. |
| `001550` | 128 aligned 4 KiB reads | 28.76% | No additional gain. |
| `001724` / `001801` | Repeat 128×4 KiB / committed baseline | 30.26% / 29.90% | Restore original eight 64 KiB reads. |

RAW_IO bypasses WinUSB's normal queuing/error handling, as described in
[Microsoft's pipe policy documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-functions-for-pipe-policy-modification).
It was tested only in a diagnostic executable and is not enabled in production.

## Evidence and reproduction

Raw profiles, pcapng captures, summaries, workload logs, and independent
analysis live under
`diagnostics/artifacts/2026-09-27/bottleneck/cpu-20260927-HHMMSS/`.
Experimental source snapshots, executables, and the parser benchmark are
archived in the adjacent `experiments/` directory. Historical generators
target the source revision used in that experiment; they are not maintained
entry points. Use [the diagnostic tools](../../../tools/diagnostics/README.md)
for new runs.

CPU comes from external Windows process/thread CPU-time sampling over the
workload window, excluding the initial/final idle windows. Production binaries
contain no per-packet profiler. Builds and large-file analysis do not overlap
CPU measurements. **100% means one logical processor**; divide by 16 for this
host's whole-machine percentage. CPU frequency, scheduling, and background
conditions are not fixed, and live captures are not identical replays.
