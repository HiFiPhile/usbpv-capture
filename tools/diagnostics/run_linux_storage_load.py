"""Opt-in Linux capture/CPU measurement with read-only, uncached USB disk load."""
import argparse
import json
import mmap
import os
from pathlib import Path
import stat
import subprocess
import time


def cpu_seconds(pid):
    # comm can contain spaces and parentheses. Fields after it begin with state.
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def open_disk(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECT | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if not stat.S_ISBLK(info.st_mode):
            raise RuntimeError('load target must be a USB block device')
        sysfs = Path(f'/sys/dev/block/{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}').resolve()
        for parent in (sysfs, *sysfs.parents):
            if (parent / 'idVendor').exists():
                identity = tuple((parent / field).read_text().strip()
                                 for field in ('idVendor', 'idProduct', 'serial'))
                if identity != ('0011', '7788', 'C4D197AC'):
                    raise RuntimeError(f'USB disk identity changed: {identity}')
                return fd
        raise RuntimeError('load target is not on USB')
    except BaseException:
        os.close(fd)
        raise


def main():
    root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe', type=Path, default=root / 'build-linux/usbpv_capture')
    ap.add_argument('--device', default='/dev/disk/by-id/usb-Generic_Flash_Disk_C4D197AC-0:0')
    ap.add_argument('--seconds', type=float, default=15)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    if not 1 <= args.seconds <= 60:
        ap.error('--seconds must be between 1 and 60')
    fd = open_disk(args.device)
    try:
        # O_DIRECT with page-aligned memory prevents page-cache hits from posing
        # as USB traffic. This tool never opens the disk for writing.
        with mmap.mmap(-1, 1024 * 1024) as buffer:
            args.output.mkdir(parents=True, exist_ok=False)
            run = args.output.resolve()
            ready, stop = run / 'ready.json', run / 'stop'
            command = [str(args.exe.resolve()), 'capture', '--speed', 'high',
                       '--include-nak', '--include-sof', '--duration', str(args.seconds + 15),
                       '--output', str(run / 'traffic.pcapng'),
                       '--ready-file', str(ready), '--stop-file', str(stop)]
            (run / 'command.json').write_text(json.dumps(command))
            with (run / 'capture.log').open('w') as log:
                capture = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 8
                    while not ready.exists() and capture.poll() is None and time.monotonic() < deadline:
                        time.sleep(0.02)
                    if not ready.exists():
                        raise RuntimeError('capture did not start: ' + (run / 'capture.log').read_text())
                    begin_cpu = cpu_seconds(capture.pid)
                    begin = time.monotonic()
                    offset = total = ops = 0
                    random = 1234567
                    samples = []
                    next_sample = begin + 1
                    while time.monotonic() - begin < args.seconds:
                        small = int(time.monotonic() - begin) // 2 % 2
                        size = 4096 if small else len(buffer)
                        random = (random * 1664525 + 1013904223) & 0xffffffff
                        offset = ((random % (64 * 1024 * 1024 // 4096)) * 4096 if small
                                  else (offset + size) % (64 * 1024 * 1024))
                        if offset + size > 64 * 1024 * 1024:
                            offset = 0
                        view = memoryview(buffer)[:size]
                        try:
                            n = os.preadv(fd, [view], offset)
                        finally:
                            view.release()
                        if n != size:
                            raise RuntimeError('short uncached disk read')
                        total += n
                        ops += 1
                        now = time.monotonic()
                        if now >= next_sample:
                            samples.append({'seconds': now - begin, 'cpu_seconds': cpu_seconds(capture.pid) - begin_cpu})
                            next_sample = now + 1
                        if capture.poll() is not None:
                            raise RuntimeError('capture stopped during load')
                    elapsed = time.monotonic() - begin
                    cpu = cpu_seconds(capture.pid) - begin_cpu
                    result = {'read_bytes': total, 'read_operations': ops, 'seconds': elapsed,
                              'mib_per_second': total / elapsed / 1024**2,
                              'capture_cpu_seconds': cpu, 'capture_cpu_percent_one_core': 100 * cpu / elapsed,
                              'samples': samples, 'read_only': True, 'direct_io': True}
                    (run / 'load.json').write_text(json.dumps(result, indent=2))
                    print(json.dumps(result), flush=True)
                finally:
                    stop.touch()
                    try:
                        capture.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        capture.kill()
                        capture.wait()
                        raise
                summary = json.loads((run / 'traffic.pcapng.summary.json').read_text())
                print(json.dumps(summary), flush=True)
                if capture.returncode or not summary['complete']:
                    raise RuntimeError('capture reported failure under load')
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
