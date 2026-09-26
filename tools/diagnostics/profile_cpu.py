"""Windows process/thread CPU accounting; bounded authorized USB storage load."""
import ctypes as c
from ctypes import wintypes as w
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

k = c.WinDLL('kernel32', use_last_error=True)
class FT(c.Structure):
    _fields_ = [('lo', w.DWORD), ('hi', w.DWORD)]
class TE(c.Structure):
    _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('tid', w.DWORD),
                ('pid', w.DWORD), ('base', w.LONG), ('delta', w.LONG), ('flags', w.DWORD)]
def api(name, args, result):
    f = getattr(k, name); f.argtypes = args; f.restype = result
    return f
open_process = api('OpenProcess', [w.DWORD, w.BOOL, w.DWORD], w.HANDLE)
open_thread = api('OpenThread', [w.DWORD, w.BOOL, w.DWORD], w.HANDLE)
close = api('CloseHandle', [w.HANDLE], w.BOOL)
snapshot = api('CreateToolhelp32Snapshot', [w.DWORD, w.DWORD], w.HANDLE)
first = api('Thread32First', [w.HANDLE, c.POINTER(TE)], w.BOOL)
next_thread = api('Thread32Next', [w.HANDLE, c.POINTER(TE)], w.BOOL)
ptime = api('GetProcessTimes', [w.HANDLE] + [c.POINTER(FT)] * 4, w.BOOL)
ttime = api('GetThreadTimes', [w.HANDLE] + [c.POINTER(FT)] * 4, w.BOOL)
name = api('GetThreadDescription', [w.HANDLE, c.POINTER(c.c_void_p)], w.LONG)
free = api('LocalFree', [c.c_void_p], c.c_void_p)
def seconds(ft): return ((ft.hi << 32) | ft.lo) / 1e7
def times(handle, fn):
    created, exited, kernel, user = FT(), FT(), FT(), FT()
    if not fn(handle, c.byref(created), c.byref(exited), c.byref(kernel), c.byref(user)):
        raise c.WinError(c.get_last_error())
    return dict(kernel=seconds(kernel), user=seconds(user))
def thread_name(handle):
    p = c.c_void_p()
    if name(handle, c.byref(p)) < 0: return 'unlabelled'
    try: return c.wstring_at(p) if p.value else 'unlabelled'
    finally:
        if p.value: free(p)

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description="Profile capture CPU during the verified E: USB storage workload (creates and deletes one new 64 MiB file).")
parser.add_argument('--exe', type=Path, default=root/'build/usbpv_capture.exe')
parser.add_argument('--load-exe', type=Path, default=root/'build-diagnostics-tools/usb_storage_load.exe')
parser.add_argument('--output-root', type=Path, default=root/'diagnostics/artifacts'/time.strftime('%Y-%m-%d')/'bottleneck')
args = parser.parse_args()
exe = args.exe.resolve()
load_exe = args.load_exe.resolve()
if not exe.is_file() or not load_exe.is_file():
    parser.error('build the capture executable and diagnostic workload helper first')
args.output_root.mkdir(parents=True, exist_ok=True)
run = args.output_root / ('cpu-' + time.strftime('%Y%m%d-%H%M%S'))
run.mkdir()
(args.output_root/'latest-cpu.txt').write_text(str(run.resolve()))
ready, stop = run/'ready.json', run/'stop'
temp = Path('E:/')/('.usbpv-load-'+uuid.uuid4().hex+'.tmp')
command = [str(exe), 'capture', '--speed', 'high', '--duration', '90', '--include-nak',
           '--include-sof', '--output', str(run/'traffic.pcapng'),
           '--ready-file', str(ready), '--stop-file', str(stop)]
(run/'command.json').write_text(json.dumps(command))
handles = {}
def sample(proc, handle):
    snap = snapshot(4, 0)
    if snap == c.c_void_p(-1).value: raise c.WinError(c.get_last_error())
    try:
        entry = TE(); entry.size = c.sizeof(entry)
        ok = first(snap, c.byref(entry))
        while ok:
            if entry.pid == proc.pid and entry.tid not in handles:
                h = open_thread(0x0800, False, entry.tid)
                if h: handles[entry.tid] = h
            ok = next_thread(snap, c.byref(entry))
    finally: close(snap)
    return dict(t=time.perf_counter(), process=times(handle, ptime),
                threads={str(tid): dict(name=thread_name(h), **times(h, ttime))
                         for tid, h in handles.items()})

samples = []
flags = subprocess.CREATE_NO_WINDOW
with (run/'stdout.log').open('w') as out, (run/'stderr.log').open('w') as err:
    capture = subprocess.Popen(command, stdout=out, stderr=err, creationflags=flags)
    handle = open_process(0x1000, False, capture.pid)
    if not handle: raise c.WinError(c.get_last_error())
    try:
        deadline = time.monotonic() + 8
        while not ready.exists() and capture.poll() is None and time.monotonic() < deadline:
            time.sleep(.02)
        if not ready.exists(): raise RuntimeError('capture startup failed')
        print('CPU profile running:', run, flush=True)
        def collect_until(deadline):
            while time.perf_counter() < deadline:
                samples.append(sample(capture, handle)); time.sleep(.25)
        idle_start = sample(capture, handle)
        collect_until(time.perf_counter() + 2)
        load_start = sample(capture, handle)
        with (run/'load.stdout.log').open('w') as lo, (run/'load.stderr.log').open('w') as le:
            load = subprocess.Popen([str(load_exe), str(temp), '15'],
                                    stdout=lo, stderr=le, creationflags=flags)
            deadline = time.monotonic() + 50
            while load.poll() is None:
                if time.monotonic() > deadline:
                    load.terminate(); load.wait(); raise RuntimeError('load timed out')
                samples.append(sample(capture, handle)); time.sleep(.25)
        load_end = sample(capture, handle)
        if load.returncode: raise RuntimeError((run/'load.stderr.log').read_text())
        collect_until(time.perf_counter() + 2)
        idle_end = sample(capture, handle)
    finally:
        stop.touch(); capture.wait(timeout=15)
    final = sample(capture, handle)
    (run/'samples.json').write_text(json.dumps(dict(logical_processors=os.cpu_count(),
        idle_start=idle_start, load_start=load_start, load_end=load_end, idle_end=idle_end,
        final=final, samples=samples), indent=2))
    close(handle)
    for h in handles.values(): close(h)
if temp.exists(): raise RuntimeError('temporary file not removed')
def delta(a, b):
    wall = b['t'] - a['t']
    def one(x, y):
        user = y['user'] - x.get('user', 0); kernel = y['kernel'] - x.get('kernel', 0)
        return dict(user_seconds=user, kernel_seconds=kernel, cpu_seconds=user+kernel,
                    one_core_percent=100*(user+kernel)/wall)
    result = dict(wall_seconds=wall, process=one(a['process'], b['process']), threads={})
    result['process']['machine_percent'] = result['process']['one_core_percent']/os.cpu_count()
    for tid, t in b['threads'].items():
        result['threads'][tid] = dict(name=t['name'], **one(a['threads'].get(tid, {}), t))
    return result
profile = dict(logical_processors=os.cpu_count(), idle_before=delta(idle_start, load_start),
               load=delta(load_start, load_end), idle_after=delta(load_end, idle_end),
               process_lifetime=final['process'])
windows = [delta(samples[i-4], samples[i])['process']['one_core_percent']
           for i in range(4, len(samples))
           if samples[i-4]['t'] >= load_start['t'] and samples[i]['t'] <= load_end['t']]
profile['load']['peak_approximately_one_second_core_percent'] = max(windows)
(run/'cpu.json').write_text(json.dumps(profile, indent=2))
summary = json.loads((run/'traffic.pcapng.summary.json').read_text())
print(json.dumps(profile['load'], indent=2), flush=True)
print(json.dumps(summary), flush=True)
print('Temporary file removed. CPU profile:', run, flush=True)
if capture.returncode: raise RuntimeError('capture incomplete')
