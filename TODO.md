# USBPV agent capture TODO

Current scope is native CH56x capture on Windows and Linux. Older FTDI sniffers
are unsupported. See the [Linux validation report](docs/reports/2026-09-27/linux-native.md)
for measured load coverage and remaining hardware limits.

- [x] Require an explicit USB capture speed; never select the buggy auto mode.
- [x] Disable SOF and NAK capture by default, with explicit opt-in switches.
- [x] Flush stale callbacks for a bounded interval before declaring capture ready.
- [x] Keep the capture callback allocation-free with a bounded, preallocated queue.
- [x] Move file I/O to a dedicated writer thread and report all capture loss.
- [x] Write raw USB packets as nanosecond-resolution pcapng for Wireshark/TShark.
- [x] Preserve reset, suspend, and overflow events in a JSONL sidecar.
- [x] Emit stable JSON status/summary records for automated agents.
- [x] Support duration, packet-count, idle-timeout, Ctrl+C, and stop-file exits.
- [x] Expose the device's address/endpoint filters to reduce traffic at source.
- [x] Document safe NAK use and recovery from a jammed sniffer.
- [x] Implement native WinUSB capture for CH56x sniffers on Windows.
- [x] Test native framing, chunk boundaries, padding, malformed lengths, and timer wrap.
- [x] Port the native CH56x transport to Linux with libusb and fault-injection tests.
- [x] Validate high-speed USB disk load over a SuperSpeed monitor link on Windows and Linux.
- [x] Compare Linux CPU using the Windows write/read workload and test NAK-heavy burst recovery.
- [ ] Extend sustained-load and queue-sizing coverage to other hosts, controllers, and traffic patterns.
- [ ] Validate physical low/full-speed capture, cable-removal faults, and older CH56x revisions.
- [ ] Add capture fixtures from real enumeration, bulk, interrupt, and periodic transfers.
