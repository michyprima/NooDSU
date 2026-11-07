#!/usr/bin/env python3
# Explains the noodsu-wramtest.bin the shared WRAM test saves, or finds its block in a RAM dump from tests/jit/game.sh;
# with a second file, such as NooDS U's, it says where the two differ
# Usage: wram.py <noodsu-wramtest.bin or RAM dump> [another to compare]
import re, struct, sys

h = open(__file__.replace('wram.py', 'wram.h')).read()
D = {k: int(v, 0) for k, v in re.findall(r'#define (\w+) (-?(?:0x)?[0-9A-Fa-f]+)\b', h)}
TOTAL = D['W_PHASES'] + D['PHASES'] * D['PHASE_WORDS']
STATUSES = {1: 'done', 2: "the ARM7 didn't answer", 3: "shared WRAM wasn't all the ARM7's", 4: 'new WRAM was in the way'}

def load(path):
    data = open(path, 'rb').read()
    at = data.find(struct.pack('<2I', D['WRAM_MAGIC0'], D['WRAM_MAGIC1']))
    if at < 0 or at % 4: sys.exit(f"{path} doesn't have the test's results")
    return struct.unpack_from('<%dI' % TOTAL, data, at)

def blocks(words, value_at, mark, phase):
    # What each block showed: the block an ARM9 marker of this phase was written to, W if it could only be written now,
    # or - if it couldn't be written
    out = ''
    for n in range(8):
        value, back = words[value_at + n], words[value_at + 8 + n]
        if value & 0xFFFF0000 == 0xB9000000 | phase << 16 and value & 0xFF < 8: out += str(value & 7)
        else: out += 'W' if back == mark | n else '-'
    return out

def describe(w):
    lines = [f"status: {STATUSES.get(w[D['W_STATUS']], w[D['W_STATUS']])}, at ARM9 step {w[D['W_STEP']]}, after ARM7 command "
        f"{w[D['W_DONE7']] >> 4}"]
    i9, i7 = D['W_INIT9'], D['W_INIT7']
    lines.append('ARM9 at the start: WRAMCNT %X, SCFG_EXT9 %08X, MBK6-8 %08X %08X %08X' % w[i9:i9 + 5])
    lines.append(f"shared WRAM at the start: {w[D['W_USED']]} words not 0")
    lines.append('ARM7 at the start: MBK9 %08X, SCFG_EXT7 %08X, WRAMSTAT %X, MBK6-8 %08X %08X %08X' % w[i7:i7 + 6])
    for i in range(D['PHASES']):
        p = D['W_PHASES'] + i * D['PHASE_WORDS']
        lock, cnt = struct.unpack('<iI', struct.pack('<2I', w[p], w[p + 1]))
        mark = 0xB9000000 | i << 16
        lines.append(f"phase {i}: " + (f"MBK9 top byte {lock:02X} at WRAMCNT {cnt}, read back {w[p + D['P_MBK9']]:08X}"
            if lock >= 0 else f"MBK9 as the system set it, at WRAMCNT {cnt}") + f", then WRAMCNT {3 - cnt}")
        lines.append('    ARM9 before: ' + blocks(w, p + D['P_LOOK9_BEFORE'], mark | 0x100, i) +
            '  after: ' + blocks(w, p + D['P_LOOK9_AFTER'], mark | 0x200, i) +
            '  after ARM7: ' + blocks(w, p + D['P_LOOK9_LAST'], mark | 0x400, i))
        lines.append('    ARM7 after:  ' + blocks(w, p + D['P_LOOK7'], 0xC7000000 | i << 16 | 0x300, i))
    return lines

a = describe(load(sys.argv[1]))
if len(sys.argv) < 3:
    print('\n'.join(a))
else:
    b = describe(load(sys.argv[2]))
    for x, y in zip(a, b):
        print(x if x == y else f'{x}\n  vs {y.strip()}')
