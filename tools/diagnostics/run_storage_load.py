import argparse
import json
from pathlib import Path
import subprocess
import time
import uuid

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description="Capture the verified E: USB storage workload (creates and deletes one new 64 MiB file).")
parser.add_argument('--exe', type=Path, default=root/'build/usbpv_capture.exe')
parser.add_argument('--load-exe', type=Path, default=root/'build-diagnostics-tools/usb_storage_load.exe')
parser.add_argument('--output-root', type=Path, default=root/'diagnostics/artifacts'/time.strftime('%Y-%m-%d')/'native')
parser.add_argument('--queue-capacity', type=int, default=16384)
args = parser.parse_args()
exe = args.exe.resolve()
load_exe = args.load_exe.resolve()
if not exe.is_file() or not load_exe.is_file():
    parser.error('build the capture executable and diagnostic workload helper first')
args.output_root.mkdir(parents=True, exist_ok=True)
run = args.output_root / ('storage-load-' + time.strftime('%Y%m%d-%H%M%S'))
run.mkdir()
(args.output_root/'latest-storage-load.txt').write_text(str(run.resolve()))
ready, stop = run / 'ready.json', run / 'stop'
temp = Path('E:/') / ('.usbpv-load-' + uuid.uuid4().hex + '.tmp')
cap_cmd = [str(exe), 'capture', '--speed', 'high',
           '--duration', '60', '--include-nak', '--include-sof',
           '--output', str(run / 'traffic.pcapng'), '--ready-file', str(ready), '--stop-file', str(stop)]
cap_cmd += ['--queue-capacity', str(args.queue_capacity)]
(run / 'command.json').write_text(json.dumps(cap_cmd))
flags = subprocess.CREATE_NO_WINDOW
with (run / 'capture.stdout.log').open('w') as out, (run / 'capture.stderr.log').open('w') as err:
    capture = subprocess.Popen(cap_cmd, stdout=out, stderr=err, creationflags=flags)
    try:
        deadline = time.monotonic() + 8
        while not ready.exists() and capture.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        if not ready.exists():
            raise RuntimeError('capture failed to start: ' + (run / 'capture.stdout.log').read_text())
        print('ready', ready.read_text(), flush=True)
        load_cmd = [str(load_exe), str(temp), '15']
        load = subprocess.run(load_cmd, capture_output=True, text=True, timeout=50, creationflags=flags)
        (run / 'load.stdout.log').write_text(load.stdout)
        (run / 'load.stderr.log').write_text(load.stderr)
        print(load.stdout, load.stderr, flush=True)
        if load.returncode:
            raise RuntimeError(f'load exit {load.returncode}')
    finally:
        stop.touch()
        capture.wait(timeout=10)
    print('capture exit', capture.returncode, flush=True)
    print((run / 'capture.stdout.log').read_text(), flush=True)
    if temp.exists():
        raise RuntimeError('temporary load file was not automatically deleted: ' + str(temp))
    print('temporary file removed; results:', run, flush=True)
    if capture.returncode:
        raise RuntimeError('capture reported failure under load')
