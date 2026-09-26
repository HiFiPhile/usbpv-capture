# USBPV agent capture TODO

- [x] Require an explicit USB capture speed; never select the buggy auto mode.
- [x] Disable SOF and NAK capture by default, with explicit opt-in switches.
- [x] Flush stale callbacks for a bounded interval before declaring capture ready.
- [x] Keep the vendor callback allocation-free with a bounded, preallocated queue.
- [x] Move file I/O to a dedicated writer thread and report all capture loss.
- [x] Write raw USB packets as nanosecond-resolution pcapng for Wireshark/TShark.
- [x] Preserve reset, suspend, and overflow events in a JSONL sidecar.
- [x] Emit stable JSON status/summary records for automated agents.
- [x] Support duration, packet-count, idle-timeout, Ctrl+C, and stop-file exits.
- [x] Expose the device's address/endpoint filters to reduce traffic at source.
- [x] Document safe NAK use and recovery from a jammed sniffer.
- [x] Replace the crashing Windows CH56x vendor capture engine with native WinUSB.
- [x] Test native framing, chunk boundaries, padding, malformed lengths, and timer wrap.
- [ ] Port the native transport to Linux and support older FTDI sniffers.
- [ ] Validate sustained high-speed throughput and queue sizing on physical hardware.
- [ ] Add capture fixtures from real enumeration, bulk, interrupt, and periodic transfers.
