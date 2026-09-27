"""Run the Windows-equivalent verified 64 MiB USB write / mixed-read workload."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import uuid


def sample(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    hz = os.sysconf('SC_CLK_TCK')
    return {'time': time.monotonic(), 'user': int(fields[11]) / hz, 'kernel': int(fields[12]) / hz}


def main():
    root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe', type=Path, default=root / 'build-linux/usbpv_capture')
    ap.add_argument('--load-exe', type=Path, default=root / 'build-linux-tools/usb_storage_load_linux')
    ap.add_argument('--directory', type=Path, required=True, help='directory on the verified USB disk')
    ap.add_argument('--read-seconds', type=float, default=15)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    if not 1 <= args.read_seconds <= 30:
        ap.error('--read-seconds must be between 1 and 30')
    if not args.exe.is_file() or not args.load_exe.is_file():
        ap.error('build the capture executable and Linux workload helper first')
    if not args.directory.is_dir():
        ap.error('--directory must already exist')
    args.output.mkdir(parents=True, exist_ok=False)
    run = args.output.resolve()
    ready, stop = run / 'ready.json', run / 'stop'
    temporary = args.directory.resolve() / ('.usbpv-load-' + uuid.uuid4().hex + '.tmp')
    command = [str(args.exe.resolve()), 'capture', '--speed', 'high', '--duration', '90',
               '--include-nak', '--include-sof', '--output', str(run / 'traffic.pcapng'),
               '--ready-file', str(ready), '--stop-file', str(stop)]
    load_command = [str(args.load_exe.resolve()), str(temporary), str(args.read_seconds)]
    (run / 'command.json').write_text(json.dumps({'capture': command, 'load': load_command}))
    load = None
    samples = []
    with (run / 'capture.log').open('w') as log:
        capture = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 8
            while not ready.exists() and capture.poll() is None and time.monotonic() < deadline:
                time.sleep(.02)
            if not ready.exists():
                raise RuntimeError('capture failed to start: ' + (run / 'capture.log').read_text())
            time.sleep(2)  # Match the Windows profiler's initial idle interval.
            begin = sample(capture.pid)
            with (run / 'load.stdout.log').open('w') as out, (run / 'load.stderr.log').open('w') as err:
                load = subprocess.Popen(load_command, stdout=out, stderr=err)
                deadline = time.monotonic() + 50
                while load.poll() is None:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('USB workload timed out')
                    if capture.poll() is not None:
                        raise RuntimeError('capture stopped during USB workload')
                    samples.append(sample(capture.pid))
                    time.sleep(.25)
            end = sample(capture.pid)
            (run / 'samples.json').write_text(json.dumps({'begin': begin, 'end': end, 'samples': samples}, indent=2))
            if load.returncode:
                raise RuntimeError('USB workload failed: ' + (run / 'load.stderr.log').read_text())
            phases = [json.loads(line) for line in (run / 'load.stdout.log').read_text().splitlines()]
            result = next(p for p in phases if p['phase'] == 'complete')
            if not result['verified'] or result['write_bytes'] != 64 * 1024**2:
                raise RuntimeError('incomplete workload verification')
            elapsed = end['time'] - begin['time']
            user, kernel = end['user'] - begin['user'], end['kernel'] - begin['kernel']
            result.update({'measurement_seconds': elapsed, 'capture_user_seconds': user,
                           'capture_kernel_seconds': kernel, 'capture_cpu_seconds': user + kernel,
                           'capture_cpu_percent_one_core': 100 * (user + kernel) / elapsed,
                           'write_mib_per_second': result['write_bytes'] / result['write_seconds'] / 1024**2,
                           'read_mib_per_second': result['read_bytes'] / result['read_seconds'] / 1024**2,
                           'direct_io': True, 'synchronous_writes': True,
                           'temporary_file_removed': not temporary.exists()})
            (run / 'load.json').write_text(json.dumps(result, indent=2))
            print(json.dumps(result), flush=True)
            time.sleep(2)  # Match the Windows profiler's final idle interval.
        finally:
            if load is not None and load.poll() is None:
                load.kill()
                load.wait()
            stop.touch()
            try:
                capture.wait(timeout=10)
            except subprocess.TimeoutExpired:
                capture.kill()
                capture.wait()
                raise
        if temporary.exists():
            raise RuntimeError('temporary file was not removed: ' + str(temporary))
        summary = json.loads((run / 'traffic.pcapng.summary.json').read_text())
        print(json.dumps(summary), flush=True)
        if capture.returncode or not summary['complete']:
            raise RuntimeError('capture reported failure under load')


if __name__ == '__main__':
    main()
