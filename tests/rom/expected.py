#!/usr/bin/env python3
# Turns a run's results into the table the ROM compares with on its own screen; the run needs detail turned on, for the S lines
# Usage: expected.py <results> <cases.h> <output>
import struct, sys

MAX_CASES, MAX_TIMED, SETS = 4000, 160, 24

# Which CPUs have each case and timed unit, in order, from the generated tables
tables = {'Case': [], 'Timed': []}
guard = table = None
for line in open(sys.argv[2]):
    if line.startswith('static const struct Case'): table = 'Case'
    elif line.startswith('static const struct Timed'): table = 'Timed'
    elif line.startswith('};'): table = None
    elif table:
        if line.startswith('#ifdef'): guard = line.split()[1]
        elif line.startswith('{ case_') or line.startswith('{ time_'): tables[table].append(guard)
        elif line.startswith('#endif'): guard = None

results = {'ARM9': [], 'ARM7': []}
sets = {'ARM9': [], 'ARM7': []}
cycles = {'ARM9': [], 'ARM7': []}
for line in open(sys.argv[1]):
    if line.startswith('RUN ') and results['ARM9']: break # Only the first run, the interpreter's
    parts = line.split(' ')
    if parts[0] == 'C': results[parts[1]].append(int(parts[2], 16))
    elif parts[0] == 'S': sets[parts[1]].append([int(x, 16) & 0xFFFF for x in parts[2:]])
    elif parts[0] == 'T': cycles[parts[1]].append(int(parts[3]))

def spread(values, guards, name, empty):
    # One entry for each row of the table, with nothing for what this CPU doesn't have
    values = iter(values)
    return [next(values) if guard in (None, name) else empty for guard in guards]

blob = b''
for name in ('ARM9', 'ARM7'):
    words = spread(results[name], tables['Case'], name, 0)
    blob += struct.pack('<%dI' % MAX_CASES, *(words + [0] * (MAX_CASES - len(words))))
for name in ('ARM9', 'ARM7'):
    rows = spread(sets[name], tables['Case'], name, [0] * SETS)
    flat = [x for row in rows for x in row]
    blob += struct.pack('<%dH' % (MAX_CASES * SETS), *(flat + [0] * (MAX_CASES * SETS - len(flat))))
for name in ('ARM9', 'ARM7'):
    words = spread(cycles[name], tables['Timed'], name, 0)
    blob += struct.pack('<%dI' % MAX_TIMED, *(words + [0] * (MAX_TIMED - len(words))))
open(sys.argv[3], 'wb').write(blob)
