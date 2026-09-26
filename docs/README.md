# Documentation

All command examples and artifact paths are relative to the repository root
unless stated otherwise.

## Operating guides

- [Capture workflow](guides/agent-capture.md): building, device selection,
  packet filters, ready/stop coordination, output buffering, and loss reporting.
- [Legacy Windows runtime isolation](guides/windows-runtime-isolation.md):
  reproduce the isolated vendor DLL dependencies when using the legacy backend.

## Reference

- [Native CH56x protocol](reference/native-protocol.md): WinUSB transport,
  register commands, stream framing, timestamps, crash diagnosis, and validation.
- [Vendor API](reference/vendor-api.md): original vendor API documentation.

## Investigation reports — 2026-09-26

Read these in order for the measured progression from the storage-load issue
to the current implementation. Results describe the tested hardware/workload;
they are not maximum-throughput guarantees.

| Report | Finding |
| --- | --- |
| [USB storage stress test](reports/2026-09-26/usb-storage-load.md) | Dense unfiltered NAK traffic exposed application queue loss. |
| [Output bottleneck](reports/2026-09-26/capture-bottleneck.md) | A 61 ms synchronous write stalled queue draining; bounded asynchronous output handled injected 151–161 ms stalls. |
| [CPU profile](reports/2026-09-26/cpu-profile.md) | Reading, parsing, and queue insertion dominated CPU after output buffering. |
| [CPU optimization](reports/2026-09-26/cpu-optimization.md) | Native callback batching reduced measured CPU by 23–33% in the recorded comparisons. |

See the [diagnostic evidence index](../diagnostics/README.md) for local captures,
dumps, logs, and their relocation manifest. The [diagnostic tools guide](../tools/diagnostics/README.md)
explains how to build the helpers and run new storage/CPU measurements.
