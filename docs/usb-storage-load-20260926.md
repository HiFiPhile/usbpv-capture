# USB storage load with NAK capture enabled

Generated actual uncached USB storage traffic on 2026-09-26 using the connected
8 GB Generic Flash Disk (`0011:7788`, PnP serial `C4D197AC`, Windows volume
`E:`). The sniffer was `R9GF6P0`, using the native WinUSB backend over a
SuperSpeed monitor connection, capturing a high-speed USB bus.

Each run created a new 64 MiB temporary file with `CREATE_NEW`, unbuffered I/O,
write-through, and delete-on-close. After writing it, the workload alternated
1 MiB sequential and 4 KiB random uncached reads for 15 seconds and checked
the returned bytes against the test pattern. Both files were automatically
deleted, and no pre-existing drive files were changed.

Capture used `--include-nak --include-sof` (mask `0xFF`), with no address or
endpoint filters. The workload stopped capture through its stop file.

| Measurement | Default queue: 16,384 packets | Larger queue: 65,536 packets |
| --- | ---: | ---: |
| Capture duration | 30.47 s | 31.60 s |
| Recorded packets | 26,572,857 | 27,820,064 |
| Average recorded packet rate | 872,164/s | 880,490/s |
| Recorded NAK packets | 12,404,286 | 13,024,428 |
| Application queue drops | 2,179 (0.0082%) | 4,858 (0.0175%) |
| Device overflows / invalid packets | 0 / 0 | 0 / 0 |
| Write throughput | 4.24 MiB/s | 4.24 MiB/s |
| Mixed read throughput | 10.26 MiB/s | 10.24 MiB/s |
| Test-file data verification | Passed | Passed |
| Capture result | Incomplete, exit 7 | Incomplete, exit 7 |

Neither run crashed or jammed the sniffer. Both correctly reported
`stop_file_with_loss`. A subsequent normal capture completed successfully.
The lost packets were reported at the application's reader-to-writer queue,
not as hardware overflow. This demonstrates a capture capacity limitation
under dense NAK traffic; it does not reintroduce the vendor linked-list crash.

The larger-queue run also lost packets. These were not isolated performance
benchmarks: analysis of the first capture overlapped part of the second run.
The results do not establish that a larger queue is slower, or identify whether
CPU scheduling, formatting, or disk stalls dominate the writer backlog.

A separate pcapng reader counted PIDs and checked USB data/token CRCs and
timestamp monotonicity across both saved captures: zero CRC/PID errors and
zero backwards timestamps in recorded packets. TShark independently checked
the first million packets of the first run without CRC/PID errors. Those
checks cannot recover or validate packets lost before writing.

Local artifacts (ignored by Git because captures total several gigabytes):

- `build-native/storage-load-20260926-224132/`: default-queue capture, workload
  output, summary, full packet analysis, and TShark sample check.
- `build-native/storage-load-20260926-224353/`: larger-queue capture, workload
  output, summary, full packet analysis, and recovery capture.
- `build-native/storage-load-comparison.json`: numerical comparison.
- `build-native/usb_storage_load.cpp` and `run_storage_load.py`: bounded workload
  and capture coordination; the helper verifies the volume/device identity
  before creating its uniquely named test file.
- `build-native/analyze_load.cpp`: independent saved-packet checker.
