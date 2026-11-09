#!/usr/bin/env python3
# Explains the noodsu-ndmatest.bin the NDMA test saves, or finds its block in a RAM dump from tests/jit/game.sh; with a
# second file, such as NooDS U's, it lists only the tests where the two differ
# Usage: ndma.py <noodsu-ndmatest.bin or RAM dump> [another to compare]
import re, struct, sys

h = open(__file__.replace('ndma.py', 'ndma.h')).read()
D = {k: int(v, 0) for k, v in re.findall(r'#define (\w+) (-?(?:0x)?[0-9A-Fa-f]+)\b', h)}
TESTS = re.findall(r'X\((\w+)\)', h[h.index('#define TESTS'):h.index('enum {')])
STATUSES = {1: 'done', 2: "the ARM7 didn't answer", 3: 'not in DSi mode'}
TOTAL = D['N_RESULTS'] + 2 * len(TESTS) * D['RESULT_WORDS']

def load(path):
    data = open(path, 'rb').read()
    at = data.find(struct.pack('<2I', D['NDMA_MAGIC0'], D['NDMA_MAGIC1']))
    if at < 0 or at % 4: sys.exit(f"{path} doesn't have the test's results")
    return struct.unpack_from('<%dI' % TOTAL, data, at)

def results(w, cpu, test):
    at = D['N_RESULTS'] + (cpu * len(TESTS) + test) * D['RESULT_WORDS']
    return w[at:at + D['RESULT_WORDS']]

def show(r):
    return ' '.join({D['SKIPPED']: 'skipped', D['BUSY']: 'busy'}.get(v, '%X' % v) for v in r)

a = load(sys.argv[1])
b = load(sys.argv[2]) if len(sys.argv) > 2 else None
print(f"status: {STATUSES.get(a[D['N_STATUS']], a[D['N_STATUS']])}, SCFG_EXT {a[D['N_SCFG9']]:08X} {a[D['N_SCFG7']]:08X}")
if b: print(f"other:  {STATUSES.get(b[D['N_STATUS']], b[D['N_STATUS']])}, SCFG_EXT {b[D['N_SCFG9']]:08X} {b[D['N_SCFG7']]:08X}")
same = 0
for cpu in range(2):
    for test, name in enumerate(TESTS):
        r = results(a, cpu, test)
        label = f"ARM{7 if cpu else 9} {name:13s}"
        if not b:
            print(f"{label} {show(r)}")
        elif r == results(b, cpu, test):
            same += 1
        else:
            print(f"{label} {show(r)}\n{'':18s} {show(results(b, cpu, test))}")
if b: print(f"{same} of {2 * len(TESTS)} match")
