"""Opt-in hardware regression: active HS traffic must already be running.

python tests/hardware_native.py --exe build/usbpv_capture.exe --output build/soak
Never runs the vendor capture library or changes the USB device under test.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--restarts", type=int, default=50)
    ap.add_argument("--tshark", default="C:/Program Files/Wireshark/tshark.exe" if os.name == "nt" else "tshark")
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    results = []

    def capture(name, *options):
        path = args.output / (name + ".pcapng")
        command = [str(args.exe.resolve()), "capture", "--speed", "high",
                   "--output", str(path), *map(str, options)]
        proc = subprocess.run(command, capture_output=True, text=True, timeout=60,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        (args.output / (name + ".log")).write_text(proc.stdout + proc.stderr)
        if proc.returncode:
            raise RuntimeError(f"{name}: exit {proc.returncode}: {proc.stdout} {proc.stderr}")
        summary = json.loads(Path(str(path) + ".summary.json").read_text())
        if not summary["complete"]:
            raise RuntimeError(f"{name}: incomplete {summary}")
        results.append(summary)
        print(name, summary["packets"], summary["reason"], flush=True)
        return path, summary

    for i in range(args.runs):
        capture(f"soak-{i:02}", "--duration", args.seconds, "--include-sof")
    for i in range(args.restarts):
        capture(f"restart-{i:03}", "--duration", "0.03", "--flush-ms", "0",
                *(["--include-sof"] if i % 2 else []))
    _, summary = capture("idle", "--duration", 3, "--idle-timeout", "0.1",
                         "--accept", "127:15")
    if summary["reason"] != "idle_timeout" or summary["packets"]:
        raise RuntimeError(f"idle/filter failed: {summary}")
    _, summary = capture("packet-stop", "--duration", 3, "--max-packets", 100,
                         "--include-sof")
    if summary["reason"] != "max_packets" or summary["packets"] < 100:
        raise RuntimeError(f"packet-count stop failed: {summary}")
    # File-open failure happens after capture startup; the next capture must
    # still work, proving that this early-exit path closed the reader.
    command = [str(args.exe.resolve()), "capture", "--speed", "high", "--duration", "1",
               "--output", str(args.output / "missing-directory" / "failed.pcapng")]
    failure = subprocess.run(command, capture_output=True, text=True, timeout=10,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if failure.returncode != 6 or '"code":"output_open"' not in failure.stdout:
        raise RuntimeError(f"unexpected output-open failure: {failure.stdout} {failure.stderr}")

    # A stop file must close even a reader waiting on an idle bus. While it is
    # active, a competing capture must fail instead of sharing the protocol.
    stop = args.output / "idle.stop"
    ready = args.output / "idle.ready"
    path = args.output / "file-stop.pcapng"
    command = [str(args.exe.resolve()), "capture", "--speed", "high", "--duration", "3",
               "--accept", "127:15", "--output", str(path),
               "--stop-file", str(stop), "--ready-file", str(ready)]
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    # communicate() is bounded; the CLI itself has a three-second fallback.
    import time
    deadline = time.monotonic() + 2
    while not ready.exists() and proc.poll() is None and time.monotonic() < deadline:
        time.sleep(0.01)
    try:
        if not ready.exists():
            raise RuntimeError("stop-file test never became ready")
        busy = subprocess.run([str(args.exe.resolve()), "list"], capture_output=True,
                              text=True, timeout=5,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if busy.returncode != 4:
            raise RuntimeError(f"exclusive ownership failed: {busy.stdout}")
    finally:
        stop.touch()
        stdout, stderr = proc.communicate(timeout=10)
        (args.output / "file-stop.log").write_text(stdout + stderr)
    summary = json.loads(Path(str(path) + ".summary.json").read_text())
    if proc.returncode or not summary["complete"] or summary["reason"] != "stop_file":
        raise RuntimeError(f"stop-file close failed: {stdout} {stderr}")
    results.append(summary)
    capture("post-failure-recovery", "--duration", "0.2")
    # Analyze every sustained run independently with Wireshark's USB decoder.
    for i in range(args.runs):
        path = args.output / f"soak-{i:02}.pcapng"
        cmd = [args.tshark, "-r", str(path), "-T", "fields", "-e", "frame.time_epoch",
               "-e", "usbll.pid", "-e", "usbll.crc5.wrong", "-e", "usbll.crc16.wrong",
               "-e", "usbll.invalid_pid"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        errors = regressions = packets = 0
        last = None
        pids = collections.Counter()
        for line in proc.stdout:
            fields = line.rstrip("\n").split("\t")
            fields += [""] * (5 - len(fields))
            stamp = int(fields[0].replace(".", ""))
            if last is not None and stamp < last:
                regressions += 1
            last = stamp
            packets += 1
            pids[fields[1]] += 1
            errors += any(fields[2:])
        if proc.wait() or errors or regressions or not packets:
            raise RuntimeError(f"{path}: packets={packets}, CRC/PID errors={errors}, time regressions={regressions}")
        print(path.name, "CRC/PID/timestamps passed", dict(pids), flush=True)
    (args.output / "results.json").write_text(json.dumps(results, indent=2))
    print("PASS", len(results), "captures,", sum(r["packets"] for r in results), "packets", flush=True)


if __name__ == "__main__":
    main()
