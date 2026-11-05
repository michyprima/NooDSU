#!/usr/bin/env python3
# Shows where the JIT's results differ from the interpreter's, and what each instruction costs in real time
# Usage: report.py <romtest.txt>
import sys

runs = []
for line in open(sys.argv[1], errors='replace'):
    line = line.rstrip('\n')
    if line.startswith('RUN '):
        runs.append({'name': line[4:], 'cases': [], 'times': {}, 'stopped': ''})
    elif line.startswith('STOPPED '):
        runs[-1]['stopped'] = line[8:]
    elif line.startswith('C '):
        cpu, value, name = line[2:].split(' ', 2)
        runs[-1]['cases'].append((cpu, name, value))
    elif line.startswith('T '):
        cpu, us, guest, count, name = line[2:].split(' ', 4)
        runs[-1]['times'][cpu, name] = (int(us), int(guest), int(count))

base = runs[0]
print(f"{len(base['cases'])} cases in '{base['name']}'")
for run in runs:
    if run['stopped']: print(f"{run['name']}: {run['stopped']}")
for run in runs[1:]:
    wrong = [(a, b) for a, b in zip(base['cases'], run['cases']) if a != b]
    missing = len(base['cases']) - len(run['cases'])
    print(f"\n{run['name']}: {len(wrong)} differ" + (f", {missing} didn't run" if missing else ''))
    for a, b in wrong[:60]:
        print(f'  {a[0]} {a[1]}: {b[2]}, interpreter {a[2]}')

# Times per instruction, without what the loop itself takes
for run in runs[1:]:
    print(f"\nNanoseconds per instruction, '{base['name']}' against '{run['name']}', the JIT's worst first")
    print(f"{'CPU':5s}{'interpreter':>12s}{'JIT':>9s}{'JIT/interp':>11s}  {'guest cycles':>12s} {'differ':>7s}  instruction")
    rows = []
    for key, (us, guest, count) in base['times'].items():
        if key not in run['times']: continue
        cpu, name = key
        empty = 'T empty loop' if name.startswith('T ') else 'empty loop'
        emptyA = base['times'].get((cpu, empty), (0,))[0]
        emptyB = run['times'].get((cpu, empty), (0,))[0]
        a = max(us - (0 if name == empty else emptyA), 0) * 1000 / count
        b = max(run['times'][key][0] - (0 if name == empty else emptyB), 0) * 1000 / count
        rows.append((b / a if a else 0, cpu, a, b, guest / count, (run['times'][key][1] - guest) / count, name))
    for ratio, cpu, a, b, cycles, drift, name in sorted(rows, reverse=True):
        print(f'{cpu:5s}{a:12.1f}{b:9.1f}{ratio:11.2f}  {cycles:12.1f} {drift:7.2f}  {name}')
