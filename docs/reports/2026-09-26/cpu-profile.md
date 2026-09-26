# CPU usage after buffered-output fix

These are the measurements before reader batching. The subsequent
[CPU optimization report](cpu-optimization.md) records the change
and its before/after validation.

The unmodified Release executable averaged **38.99% of one logical CPU**
during the USB storage workload: **2.44% of the 16-logical-CPU machine**.
The USB reader/parser/callback thread accounts for approximately three
quarters of capture CPU. File output is a small contributor after buffering.
These measurements do not show CPU saturation at this workload.

## Workload and method

- Host: AMD Ryzen AI 7 H 350, 16 logical processors, Windows.
- Sniffer: `R9GF6P0`, native WinUSB backend, SuperSpeed monitor connection;
  captured bus speed explicitly high.
- Capture: all packet types enabled (`--include-nak --include-sof`, mask
  `0xFF`), default 16,384-packet queue, 32 MiB pcapng output pool, output on D:.
- Load: a new automatically deleted 64 MiB file on the verified USB drive
  E:, followed by 15 seconds of uncached sequential/random reads with data
  verification. Existing drive files were unchanged.
- External Python sampler: Windows `GetProcessTimes` and `GetThreadTimes`,
  polled about four times per second. Thread handles remain open through
  shutdown. CPU time is user plus kernel execution time; blocked time does
  not count as CPU execution.
- CPU measurement covers workload launch through exit, about 31.6..31.9 s.
  Captures also include approximately two seconds before and after the load;
  packet counts below cover those complete captures.

The labelled diagnostic executable builds copies of the current sources with
the production Release optimization flags (`-O3 -DNDEBUG`). Its only source
instrumentation is one `SetThreadDescription` call at each thread's entry.
There are no per-packet or per-batch profiling calls. A second run samples
the existing, unmodified `build/usbpv_capture.exe` to confirm process totals.
Compilation and large-file analysis did not overlap either workload.

Percentages below use **100% = one fully occupied logical CPU**. Divide by 16
for the process's share of total logical-CPU capacity. This is CPU-time
accounting, not a frequency-adjusted utilization metric, and excludes CPU
charged to other processes or separate system/driver work.

## Results

| Measurement | Labelled Release diagnostic | Unmodified Release executable |
| --- | ---: | ---: |
| Load measurement window | 31.627 s | 31.863 s |
| Process CPU time | 12.250 s | 12.422 s |
| Average CPU, one-core basis | 38.73% | 38.99% |
| Average CPU, whole-machine basis | 2.42% | 2.44% |
| User-mode CPU, one-core basis | 13.09% | 12.70% |
| Kernel-mode CPU, one-core basis | 25.64% | 26.28% |
| Peak sampled CPU, one-core basis | 56.17% | 57.81% |
| Peak measurement window | 1.113 s | 1.108 s |
| Captured packets | 27,706,104 | 27,875,545 |
| Captured NAKs | 12,939,497 | 13,024,998 |
| Queue drops / device overflows | 0 / 0 | 0 / 0 |
| Output pending peak | 1 MiB | 1 MiB |

The thread breakdown comes from the labelled diagnostic run, avoiding
guessing thread roles in the unmodified binary:

| Thread role | User CPU seconds | Kernel CPU seconds | Average CPU, one-core basis | Share of process CPU |
| --- | ---: | ---: | ---: | ---: |
| USB reader, protocol parser, callback and queue insertion | 2.953 | 6.156 | 28.80% | 74.36% |
| Queue draining and packet encoding | 1.125 | 1.469 | 8.20% | 21.17% |
| Pcapng file writer | 0.016 | 0.484 | 1.58% | 4.08% |
| Main control thread | 0.047 | 0 | 0.15% | 0.38% |
| Event file writer and other threads | 0 measured | 0 measured | 0 measured | 0 measured |

There were no bus events in these runs, so the event writer's result does not
characterize an event-heavy capture. Small CPU durations are quantized by
Windows accounting; zero measured CPU does not imply literally no work.

## Interpretation and validation

The next CPU investigation should focus on the reader/callback/queue path:
`Device::capture` in `src/usbpv_native.cpp`, `ProtocolParser::feed` in
`src/usbpv_protocol.cpp`, and the capture callback / `PacketQueue::push` in
`src/usbpv_capture.cpp`. About two thirds of total capture CPU was charged
to kernel mode. The reader combines WinUSB completions, parsing, callback
counters, and queue synchronization, so thread accounting alone cannot
separate those costs or attribute the kernel time to a particular function.
That finer attribution would require CPU stack sampling and scheduling traces.

The buffered file writer is no longer a major CPU consumer. This does not
remove the possibility of future long output stalls: the finite buffer pool
still determines how long capture can continue without file-write progress.

Both workloads verified their read data and automatically deleted their
temporary files. Both captures exited successfully with zero loss counters.
An independent pcapng reader checked all 55,581,649 packets across the two
runs: record counts match the capture summaries, with no invalid PIDs,
CRC5/CRC16 failures, or backwards timestamps. Profiling made no production
source or executable changes.

## Local artifacts

- `diagnostics/artifacts/2026-09-26/bottleneck/cpu-20260926-232524/`: labelled Release run.
- `diagnostics/artifacts/2026-09-26/bottleneck/cpu-20260926-232601/`: unmodified Release run.
- Each run contains `samples.json`, `cpu.json`, command and workload logs,
  the capture and its summary, and `analysis.json`.
- `diagnostics/artifacts/2026-09-26/bottleneck/make_cpu_profile.py`: creates diagnostic source copies
  and the one-time thread-name helper.
- `diagnostics/artifacts/2026-09-26/bottleneck/profile_cpu.py`: external CPU sampler and load coordinator;
  takes an optional executable path, defaulting to the diagnostic binary.
- `diagnostics/artifacts/2026-09-26/native/usb_storage_load.cpp`: identity-checked temporary-file workload.

Historical scripts, binaries, and raw artifacts remain in the ignored dated archive.
Use [maintained diagnostic tools](../../../tools/diagnostics/README.md) for new runs. The production binary used here is `build/usbpv_capture.exe`.
