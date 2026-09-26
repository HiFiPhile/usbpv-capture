# Diagnostic evidence

Reports and reusable tools are tracked in Git. Large captures, dumps, generated
source snapshots, logs, and diagnostic binaries stay in the ignored
`diagnostics/artifacts/` directory.

## 2026-09-26 archive

| Evidence | Location | Previous location |
| --- | --- | --- |
| Native capture, storage-load, and hardware regression runs | [native](artifacts/2026-09-26/native) | Diagnostic files from `build-native/` |
| CPU, output-stall, and writer benchmarks | [bottleneck](artifacts/2026-09-26/bottleneck) | `build-bottleneck/` |
| Vendor DLL crash reproductions, dumps, debugger helpers, and disassembly | [vendor-crash](artifacts/2026-09-26/vendor-crash) | `build-diagnostics/` |

The archive contains 752 relocated files, totaling 20,416,470,263 bytes.
[relocation-manifest.json](artifacts/2026-09-26/relocation-manifest.json) maps
every original path to its new path and records byte size and UTC modification
time. All files were checked against those fields after relocation. Original
logs, JSON, and source snapshots are unchanged, including historical absolute
paths and commands. The manifest resolves those old locations.

Archived scripts describe the experiments as they ran; some target older
source layouts and are not current entry points. Use the maintained
[diagnostic tools](../tools/diagnostics/README.md) for new measurements.
Active CMake build/cache/install files remain in their original build trees.
General captures under `captures/` were left in place.

## 2026-09-27 archive

- [CPU profiles and transport experiments](artifacts/2026-09-27/bottleneck):
  baseline comparisons, callback coalescing, RAW_IO and transfer-size trials,
  captures with independent packet validation, and isolated parser benchmarks.
- [Native hardware regression runs](artifacts/2026-09-27/native):
  MinGW/MSVC restart, idle/stop, error recovery, and TShark checks.

## Reports

- [Native protocol and vendor crash diagnosis](../docs/reference/native-protocol.md)
- [Original USB storage stress test](../docs/reports/2026-09-26/usb-storage-load.md)
- [Output bottleneck and buffering validation](../docs/reports/2026-09-26/capture-bottleneck.md)
- [CPU profile before reader batching](../docs/reports/2026-09-26/cpu-profile.md)
- [CPU optimization and comparison](../docs/reports/2026-09-26/cpu-optimization.md)
- [CPU follow-up: batching across USB transfers](../docs/reports/2026-09-27/cpu-optimization.md)

New workload tools write under `artifacts/YYYY-MM-DD/native/` or
`artifacts/YYYY-MM-DD/bottleneck/` by default. These files remain local and are
not included in commits.
