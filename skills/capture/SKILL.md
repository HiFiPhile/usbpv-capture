---
name: capture
description: Capture and inspect USB bus traffic with the usbpv_capture CLI and a CH56x USBPV sniffer on Windows or Linux. Use for USB packet captures, coordinated device tests, and capture-loss diagnosis; not for screenshots or general network capture.
---

# USBPV capture

Use the native `usbpv_capture` CLI to produce pcapng packets, bus-event JSONL,
and a verified completion summary. This skill operates the capture tool; it
does not authorize unrelated device workloads, disk writes, or driver changes.

## Locate the tool

The local project is `E:\code\usbpv-capture`. Prefer the user's specified
checkout or executable if different. In a checkout, read
`docs/guides/agent-capture.md` for build, permission, and detailed operating
instructions when needed. The installed skill does not bundle the executable.

Look for `build/Release/usbpv_capture.exe` (Visual Studio),
`build/usbpv_capture.exe` (MinGW), `build-linux/usbpv_capture` (Linux), or
`usbpv_capture` on PATH. Run its `help` command to verify supported options.
If a build is needed, use the checkout's documented CMake instructions and
available toolchain. Windows requires WinUSB on the capture interface;
Linux requires libusb and read/write permission on the sniffer's USB node.
Only CH56x sniffers are supported, not older FTDI models.

## Plan and run a capture

1. Run `usbpv_capture list`. One available sniffer is selected automatically;
   with multiple devices, use a serial returned by `list` and pass `--serial`.
   Ask for the intended device if the task does not identify it.
2. Select captured bus speed explicitly with `--speed low`, `full`, or `high`.
   Infer it only from known target details; ask if unknown. The SuperSpeed
   monitor connection does not determine the captured bus speed and cannot
   capture SuperSpeed bus traffic.
3. Create the output directory and choose a fresh basename, typically under
   the checkout's ignored `captures/` directory. The pcapng, events, summary,
   ready, and stop paths must all be distinct and must not already exist.
   Preserve previous captures instead of deleting files to reuse a name.
4. Use a bounded duration suitable for the task (the CLI default is 30 seconds).
   Start capture before the authorized workload, using readiness coordination
   below when timing matters.
5. Wait for process exit and validate the result before interpreting packets.

Example from the Windows repository root, after confirming high-speed traffic
and choosing an unused run name:

```powershell
New-Item -ItemType Directory -Force captures | Out-Null
& .\build\Release\usbpv_capture.exe list
& .\build\Release\usbpv_capture.exe capture --speed high --duration 20 --output captures\run-001.pcapng
$captureExit = $LASTEXITCODE
$captureSummary = Get-Content captures\run-001.pcapng.summary.json -Raw | ConvertFrom-Json
```

On Linux, the equivalent capture command is:

```sh
./build-linux/usbpv_capture capture --speed high --duration 20 --output captures/run-001.pcapng
```

## Coordinate a device test

Launch capture asynchronously with fresh `--ready-file` and `--stop-file`
paths. Prefer a finite duration as a backstop; use `--duration 0` only when
the workflow supplies a reliable stop and cleanup path. When using
PowerShell `Start-Process`, use `-WindowStyle Hidden` and retain the process
handle. Keep stdout/stderr available for diagnosis.

Wait for the ready file before starting the workload. While waiting, monitor
process exit and impose a startup deadline; an early exit can have only a
JSON error and no summary. If readiness times out, stop the capture and
diagnose it instead of running the workload without capture.

After the workload, including on workload failure, create the stop file
(`New-Item -ItemType File <stop-path>` or `touch <stop-path>`), wait for graceful
exit, and inspect the summary. Ctrl+C is also supported. Allow output to drain;
a forcibly terminated process cannot establish a complete capture. Readiness
only indicates that the workload may begin, not that capture will succeed.

## Filters and fidelity

- SOF and NAK are disabled by default. Enable SOF when frame timing matters.
  Enable NAK only for a short, tightly filtered investigation: NAK traffic can
  overwhelm the sniffer.
- Hardware filters accept addresses 0..127, endpoints 0..15, and `*` wildcards.
  Quote wildcards, for example `--accept "5:*"`. Use up to four `--accept`
  pairs or four `--drop` pairs; the modes cannot be mixed. Avoid filters that
  omit enumeration or address changes needed for the requested diagnosis.
- `--max-packets` is an asynchronous threshold and can overshoot.
  `--idle-timeout` measures callback activity, including bus events.
- The default startup flush is 250 ms (`--flush-ms`). If startup remains
  corrupt, report that reconnection or a power cycle may be needed before
  another attempt. Do not repeatedly retry the same failed setup.

## Validate and report

For output `FILE`, inspect `FILE.summary.json` and `FILE.events.jsonl` along
with the process exit status and diagnostics. Stdout consists of JSON status
objects, except plain-text help; stderr contains diagnostics.

Require exit code zero and final `complete: true` to call a capture complete.
Nonzero exit, missing summary, or `complete: false` means incomplete or failed
capture. Queue loss, corrupt/oversized callbacks, device overflow, malformed
stream records, transport errors, and output errors invalidate completeness.
Preserve partial artifacts and explain their limits; a readable pcapng or a
ready file alone is insufficient evidence of success.

Use Wireshark/TShark if packet analysis is requested and available. Summarize
the observed traffic and distinguish capture loss from target-device faults.
Report artifact paths, selected speed/serial, duration, completion status,
summary reason and relevant counters, and findings relevant to the user's
question. A complete capture with no relevant packets does not prove that
the intended workload occurred or that the selected speed/filter was correct.
