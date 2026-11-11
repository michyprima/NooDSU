#!/usr/bin/env python3
# Explains the noodsu-speedtest.bin the speed test saves, or finds its block in a RAM dump from tests/jit/game.sh;
# with a second file, such as NooDS U's, it says where the two differ
# Usage: speed.py <noodsu-speedtest.bin or RAM dump> [another to compare]
import re, struct, sys

h = open(__file__.replace('speed.py', 'speed.h')).read()
D = {k: int(v, 0) for k, v in re.findall(r'#define (\w+) (-?(?:0x)?[0-9A-Fa-f]+)\b', h)}
R, LOOPS = D['REPEATS'], D['LOOPS']
TICK = 1e9 / 33513982 # Nanoseconds
STATUSES = {1: 'done', 2: "the ARM7 didn't answer"}

def load(path):
    data = open(path, 'rb').read()
    at = data.find(struct.pack('<2I', D['SPED_MAGIC0'], D['SPED_MAGIC1']))
    if at < 0 or at % 4: sys.exit(f"{path} doesn't have the test's results")
    return struct.unpack_from('<%dI' % D['S_TOTAL'], data, at)

def times(w, at):
    # The least of a measurement's times, and all of them when they aren't the same
    t = w[at:at + R]
    if t[0] == D['NOT_RUN']: return None, 'not run'
    return min(t), str(min(t)) + ('' if min(t) == max(t) else ' (' + ' '.join(map(str, t)) + ')')

def describe(w):
    # The first results saved didn't say how many lines they ran
    LINES, DLINES = w[D['S_LINES']] or 128, w[D['S_LINES'] + 1] or 64
    lines = [f"status: {STATUSES.get(w[D['S_STATUS']], w[D['S_STATUS']])}, at ARM9 step {w[D['S_STEP']]}"]
    lines.append(f"{'DSi' if w[D['S_DSI']] else 'DS'} mode, ARM9 SCFG_CLK %04X SCFG_EXT %08X, ARM7 SCFG_CLK %04X SCFG_EXT %08X" %
        (w[D['S_SCFG9']], w[D['S_SCFG9'] + 1], w[D['S_SCFG7']], w[D['S_SCFG7'] + 1]))
    c = w[D['S_CP15']:D['S_CP15'] + 12]
    lines.append('ARM9 control %08X (code cache %s, data cache %s), cached regions: code %02X data %02X, write buffer %02X' %
        (c[0], 'on' if c[0] & 0x1000 else 'OFF', 'on' if c[0] & 4 else 'OFF', c[2], c[1], c[3]))
    for i, region in enumerate(c[4:]):
        if region & 1:
            base, size = region & 0xFFFFF000, 2 << ((region >> 1) & 0x1F)
            where = [n for n, a in (('code lines', w[D['S_ADDR9']]), ('data lines', w[D['S_ADDR9'] + 1])) if base <= a < base + size]
            lines.append(f'    region {i}: {base:08X}-{base + size - 1:08X}' + (', holds the ' + ' and '.join(where) if where else ''))
    lines.append('ARM9 code lines at %08X, data lines at %08X; ARM7 loops at %s' % (w[D['S_ADDR9']], w[D['S_ADDR9'] + 1],
        ' '.join('%08X' % a if a != D['NOT_RUN'] else '-' for a in w[D['S_ADDR7']:D['S_ADDR7'] + D['PLACES']])))

    def m9(which): return times(w, D['S_RESULTS9'] + D[which] * R)
    empty, text = m9('M9_EMPTY')
    lines.append(f'ARM9, in ticks of {TICK:.2f}ns:')
    lines.append(f'    empty call: {text}')
    loop, text = m9('M9_LOOP')
    per = (loop - empty) / LOOPS
    lines.append(f'    loop of {LOOPS}: {text}, {per:.3f} ticks a turn' + (f', {4 / (per * TICK) * 1000:.1f}MHz at 4 cycles a turn' if per > 0 else ''))
    bios, text = m9('M9_BIOS')
    lines.append(f'    BIOS wait of {LOOPS}: {text}, {(bios - empty) / LOOPS:.3f} ticks a turn')
    for name, key, count, what in (('2 instructions a line', 'HOP', LINES, 'code'), ('8 instructions a line', 'SEQ', LINES, 'code'),
            ('a read a line', 'READ', DLINES, 'data'), ('a write a line', 'WRITE', DLINES, 'data')):
        hot, hot_text = m9(f'M9_{key}_HOT')
        cold, cold_text = m9(f'M9_{key}_COLD')
        line = f'    {count} lines of {what}, {name}: in the cache {hot_text}, not in it {cold_text}, {(cold - hot) / count * TICK:.0f}ns more a line'
        if what == 'code':
            off, off_text = m9(f'M9_{key}_OFF')
            line += f'; cache off {off_text}, {(off - empty) / count * TICK:.0f}ns a line'
        lines.append(line)

    lines.append('ARM7, in ticks, which are its cycles:')
    for place, name in enumerate(('its WRAM', 'new WRAM', 'main RAM')):
        at = D['S_RESULTS7'] + place * D['KINDS'] * R
        empty, text = times(w, at)
        if empty is None:
            lines.append(f'    {name}: not run')
            continue
        parts = [f'empty call {text}']
        for kind, label in ((1, 'loop'), (2, 'with a read'), (3, 'with a write')):
            t, text = times(w, at + kind * R)
            parts.append(f'{label} {text}, {(t - empty) / LOOPS:.3f} a turn')
        lines.append(f'    {name}: ' + '; '.join(parts))
    bios, text = times(w, D['S_RESULTS7'] + D['M7_BIOS'] * R)
    empty = times(w, D['S_RESULTS7'])[0]
    lines.append(f'    BIOS wait of {LOOPS}: {text}, {(bios - empty) / LOOPS:.3f} a turn')
    return lines

a = describe(load(sys.argv[1]))
if len(sys.argv) < 3:
    print('\n'.join(a))
else:
    b = describe(load(sys.argv[2]))
    for x, y in zip(a, b):
        print(x if x == y else f'{x}\n  vs {y.strip()}')
