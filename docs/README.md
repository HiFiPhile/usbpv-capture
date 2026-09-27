# Documentation

All command examples and artifact paths are relative to the repository root
unless stated otherwise. Start with the [project quick start](../README.md#quick-start)
for native CH56x capture on Windows or Linux.

## Operating guides

- [Capture workflow](guides/agent-capture.md): building, device selection,
  packet filters, ready/stop coordination, output buffering, and loss reporting.
- [Diagnostic tools](../tools/diagnostics/README.md): reproduce storage-load,
  CPU, and capture-integrity measurements. For cross-platform load comparisons,
  use the Linux workload matching Windows.

## Reference

- [Capture data flow and buffers](reference/capture-data-flow.md): thread
  boundaries, buffer capacities, payload copies, and output ownership.
- [Native CH56x protocol](reference/native-protocol.md): WinUSB/libusb transports,
  register commands, stream framing, timestamps, crash diagnosis, and validation.
- [Remaining validation](../TODO.md): completed work and outstanding hardware coverage.

## Current performance evidence - 2026-09-27

- [Native Linux backend and tuning](reports/2026-09-27/linux-native.md):
  SuperSpeed monitor transport, matched Windows/Linux USB disk write/read load,
  CPU comparisons, NAK-heavy burst recovery, and fault tests.
- [CPU optimization across USB transfers](reports/2026-09-27/cpu-optimization.md):
  bounded callback coalescing, direct record decoding, and transport experiments.

Results apply to the tested hardware and workload. They are not
maximum-throughput or lossless-capture guarantees.

## Earlier investigations - 2026-09-26

These reports describe the progression from storage-load failures to buffered
output and lower CPU usage. Commands, backend options, and validation counts
in dated reports describe the implementation at that time; use the current
operating guide for today's CLI.

| Report | Finding |
| --- | --- |
| [USB storage stress test](reports/2026-09-26/usb-storage-load.md) | Dense unfiltered NAK traffic exposed application queue loss. |
| [Output bottleneck](reports/2026-09-26/capture-bottleneck.md) | A 61 ms synchronous write stalled queue draining; bounded asynchronous output handled injected 151-161 ms stalls. |
| [CPU profile](reports/2026-09-26/cpu-profile.md) | Reading, parsing, and queue insertion dominated CPU after output buffering. |
| [CPU optimization](reports/2026-09-26/cpu-optimization.md) | Native callback batching reduced measured CPU by 23-33% in the recorded comparisons. |

## Archived vendor SDK

These documents cover SDK provenance and the original examples:

- [Vendor API](reference/vendor-api.md): original SDK API documentation.
- [Windows runtime isolation](guides/windows-runtime-isolation.md): historical
  vendor DLL dependency preparation.
- [SDK binaries](../vendor/README.md) and [examples](../examples/README.md).

See the [diagnostic evidence index](../diagnostics/README.md) for local captures,
dumps, logs, and their relocation manifest. The [diagnostic tools guide](../tools/diagnostics/README.md)
explains how to build the helpers and run new storage/CPU measurements.
