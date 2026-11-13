#!/usr/bin/env python3
"""playtest-sample.py <pid> <out.jsonl> [interval_s]: low-rate, read-only system samples for a
playtest session (tools/playtest.sh starts it at nice 19 and stops it at exit).

Every interval (default 2 s) one JSON line:
  t          CLOCK_MONOTONIC seconds (the emulator's frames.csv uses the same clock, from its start)
  cores      per CPU: busy % since the last sample, current MHz
  temps      hwmon temperatures (CPU package/cores, GPU if exposed), deg C
  mem        system memory: available MB, swap used MB, pages swapped in / out since boot (/proc/vmstat)
  temp_limits (first sample only) the CPU package's critical temperature (where it throttles), deg C
  emu        emulator RSS and VmSwap (MB), thread count, its named threads (KytyGpuCmd, KytyGpuPrep*,
             KytyGpuRec*, ...) with the CPU each ran on last and its CPU % since the last sample
  hot_cores  the CPUs those named threads ran on, plus their SMT siblings
  intruders  the top non-emulator processes by CPU % on hot_cores since the last sample
  gpu        nvidia-smi: VRAM used/total MB, GPU %, temperature, graphics clock (every 5th sample)
Nothing is changed on the system; no root is needed.
"""
import json
import os
import subprocess
import sys
import time

CLK = os.sysconf('SC_CLK_TCK')
NAMED = ('KytyGpuCmd', 'KytyGpuPrep', 'KytyGpuRec', 'KytyGpuDecode', 'KytyGpuPresent', 'DrawThread')


def read(path):
    try:
        with open(path, 'r', errors='replace') as f:
            return f.read()
    except OSError:
        return None


def cpu_times():
    out = {}
    text = read('/proc/stat') or ''
    for line in text.splitlines():
        if line.startswith('cpu') and line[3:4].isdigit():
            parts = line.split()
            vals = list(map(int, parts[1:]))
            idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
            out[int(parts[0][3:])] = (sum(vals), idle)
    return out


def freqs():
    out = {}
    base = '/sys/devices/system/cpu'
    for name in os.listdir(base):
        if name.startswith('cpu') and name[3:].isdigit():
            v = read(f'{base}/{name}/cpufreq/scaling_cur_freq')
            if v:
                out[int(name[3:])] = int(v) // 1000
    return out


def siblings(cpu):
    v = read(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list') or ''
    out = set()
    for part in v.strip().split(','):
        if '-' in part:
            a, b = part.split('-')
            out.update(range(int(a), int(b) + 1))
        elif part:
            out.add(int(part))
    return out


def temps():
    out = {}
    base = '/sys/class/hwmon'
    try:
        mons = os.listdir(base)
    except OSError:
        return out
    for mon in mons:
        name = (read(f'{base}/{mon}/name') or mon).strip()
        if name not in ('coretemp', 'k10temp', 'nvme', 'amdgpu', 'nouveau'):
            continue
        for f in os.listdir(f'{base}/{mon}'):
            if f.startswith('temp') and f.endswith('_input'):
                label = (read(f'{base}/{mon}/{f[:-6]}_label') or f[:-6]).strip()
                if name == 'coretemp' and not (label.startswith('Package') or label.startswith('Core')):
                    continue
                v = read(f'{base}/{mon}/{f}')
                if v:
                    out[f'{name}:{label}'] = int(v) / 1000.0
    # Keep it short: the package and the hottest core.
    pkg = {k: v for k, v in out.items() if 'Package' in k or not k.startswith('coretemp')}
    cores = [v for k, v in out.items() if k.startswith('coretemp:Core')]
    if cores:
        pkg['coretemp:hottest core'] = max(cores)
    return pkg


def memory():
    out = {}
    info = {}
    for line in (read('/proc/meminfo') or '').splitlines():
        k, _, v = line.partition(':')
        if v.strip():
            info[k] = int(v.split()[0])
    if 'MemAvailable' in info:
        out['avail_mb'] = info['MemAvailable'] // 1024
    if 'SwapTotal' in info and 'SwapFree' in info:
        out['swap_used_mb'] = (info['SwapTotal'] - info['SwapFree']) // 1024
    for line in (read('/proc/vmstat') or '').splitlines():
        k, _, v = line.partition(' ')
        if k in ('pswpin', 'pswpout'):
            out[k] = int(v)
    return out


def temp_limits():
    # The package sensor's critical temperature: Intel CPUs throttle there (TjMax).
    out = {}
    base = '/sys/class/hwmon'
    try:
        mons = os.listdir(base)
    except OSError:
        return out
    for mon in mons:
        if (read(f'{base}/{mon}/name') or '').strip() != 'coretemp':
            continue
        for f in os.listdir(f'{base}/{mon}'):
            if f.startswith('temp') and f.endswith('_crit'):
                label = (read(f'{base}/{mon}/{f[:-5]}_label') or '').strip()
                v = read(f'{base}/{mon}/{f}')
                if label.startswith('Package') and v:
                    out[f'coretemp:{label}'] = int(v) / 1000.0
    return out


def proc_stat(path):
    text = read(path)
    if not text:
        return None
    rp = text.rfind(')')
    comm = text[text.find('(') + 1:rp]
    f = text[rp + 2:].split()
    # fields after ')' : state(0) ... utime(11) stime(12) ... processor(36)
    return comm, int(f[11]) + int(f[12]), int(f[36])


def gpu():
    try:
        r = subprocess.run(['nvidia-smi', '--query-gpu=memory.used,memory.total,utilization.gpu,temperature.gpu,clocks.gr',
                            '--format=csv,noheader,nounits'], capture_output=True, text=True, timeout=5)
        # Some drivers print the temperature with decimals ("57.82").
        used, total, util, temp, clock = [float(x) for x in r.stdout.strip().split('\n')[0].split(',')]
        return {'vram_used_mb': int(used), 'vram_total_mb': int(total), 'util': int(util), 'temp': round(temp, 1),
                'clock_mhz': int(clock)}
    except Exception:
        return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pid = int(sys.argv[1])
    out_path = sys.argv[2]
    interval = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
    prev_cpu = cpu_times()
    prev_threads = {}
    prev_procs = {}
    n = 0
    with open(out_path, 'a') as out:
        while os.path.exists(f'/proc/{pid}'):
            time.sleep(interval)
            if not os.path.exists(f'/proc/{pid}'):
                break
            n += 1
            now = time.monotonic()
            cur_cpu = cpu_times()
            fr = freqs()
            cores = {}
            for c, (tot, idle) in cur_cpu.items():
                if c in prev_cpu:
                    dt = tot - prev_cpu[c][0]
                    di = idle - prev_cpu[c][1]
                    cores[c] = {'busy': round(100.0 * (dt - di) / dt, 1) if dt > 0 else 0.0, 'mhz': fr.get(c)}
            prev_cpu = cur_cpu
            # Emulator threads.
            threads = []
            hot = set()
            cur_threads = {}
            try:
                tids = os.listdir(f'/proc/{pid}/task')
            except OSError:
                break
            for tid in tids:
                st = proc_stat(f'/proc/{pid}/task/{tid}/stat')
                if not st:
                    continue
                comm, ticks, cpu = st
                cur_threads[tid] = ticks
                if comm.startswith(NAMED):
                    pct = 100.0 * (ticks - prev_threads.get(tid, ticks)) / CLK / interval
                    threads.append({'name': comm, 'tid': int(tid), 'cpu': cpu, 'pct': round(pct, 1)})
                    if not comm.startswith('DrawThread'):
                        hot |= siblings(cpu) or {cpu}
            prev_threads = cur_threads
            status = read(f'/proc/{pid}/status') or ''
            mem = {}
            for line in status.splitlines():
                k, _, v = line.partition(':')
                if k in ('VmRSS', 'VmSwap', 'Threads'):
                    mem[k] = int(v.split()[0])
            # Other processes on the hot cores.
            cur_procs = {}
            intruders = []
            for p in os.listdir('/proc'):
                if not p.isdigit() or int(p) == pid or int(p) == os.getpid():
                    continue
                st = proc_stat(f'/proc/{p}/stat')
                if not st:
                    continue
                comm, ticks, cpu = st
                cur_procs[p] = ticks
                if p in prev_procs and cpu in hot:
                    pct = 100.0 * (ticks - prev_procs[p]) / CLK / interval
                    if pct >= 0.5:
                        intruders.append({'pid': int(p), 'comm': comm, 'cpu': cpu, 'pct': round(pct, 1)})
            prev_procs = cur_procs
            intruders.sort(key=lambda x: -x['pct'])
            rec = {
                't': round(now, 3), 'cores': cores, 'temps': temps(), 'mem': memory(),
                'emu': {'rss_mb': round(mem.get('VmRSS', 0) / 1024, 1), 'swap_mb': round(mem.get('VmSwap', 0) / 1024, 1),
                        'threads': mem.get('Threads'), 'named': sorted(threads, key=lambda x: x['name'])},
                'hot_cores': sorted(hot), 'intruders': intruders[:5],
            }
            if n == 1:
                rec['temp_limits'] = temp_limits()
            if n % 5 == 1:
                g = gpu()
                if g:
                    rec['gpu'] = g
            out.write(json.dumps(rec, separators=(',', ':')) + '\n')
            out.flush()


if __name__ == '__main__':
    main()
