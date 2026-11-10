#!/usr/bin/env python3
# Explains the noodsu-fifotest.bin the display FIFO test saves, or finds its block in a RAM dump from tests/jit/game.sh;
# with a second file, such as NooDS U's, it lists only the scenes where the two differ
# Usage: fifo.py <noodsu-fifotest.bin or RAM dump> [another to compare] [--images folder]
import os, re, struct, sys

h = open(__file__.replace('fifo.py', 'fifo.h')).read()
D = {k: int(v, 0) for k, v in re.findall(r'#define (\w+) (-?(?:0x)?[0-9A-Fa-f]+)\b', h)}
D['CAPTURE_WORDS'] = 256 * 192 // 2
SCENES = [(m[0], int(m[1]), int(m[2])) for m in
          re.findall(r'X\((\w+), (\d), (\d)\)', h[h.index('#define SCENES'):h.index('enum {')])]
RESULTS = D['R_RESULTS']
CAPTURES = RESULTS + len(SCENES) * D['RESULT_WORDS']
TOTAL = CAPTURES + D['CAPTURES'] * D['CAPTURE_WORDS']
NAMES = ['ran', 'cnt191', 'cnt193', 'cntEnd', 'capcnt', 'total', 'first', 'last', 'low', 'high', 'firstCount', 'min',
         'max', 'lines', 'frames']

args = sys.argv[1:]
images = args[args.index('--images') + 1] if '--images' in args else None
if images: del args[args.index('--images'):args.index('--images') + 2]

def load(path):
    data = open(path, 'rb').read()
    # The magic words are also in the ROM's code, so take the copy whose status and scene make sense
    magic = struct.pack('<2I', D['FIFO_MAGIC0'], D['FIFO_MAGIC1'])
    at = data.find(magic)
    while at >= 0:
        if at % 4 == 0 and at + TOTAL * 4 <= len(data):
            w = struct.unpack_from('<%dI' % TOTAL, data, at)
            if w[D['R_STATUS']] in (0, D['STATUS_DONE']) and w[D['R_STEP']] < len(SCENES): return w
        at = data.find(magic, at + 1)
    sys.exit(f"{path} doesn't have the test's results")

def results(w, s):
    return list(w[RESULTS + s * D['RESULT_WORDS']:RESULTS + s * D['RESULT_WORDS'] + len(NAMES)])

def slot(s):
    return sum(1 for i in range(s) if SCENES[i][1])

def pixels(w, s):
    at = CAPTURES + slot(s) * D['CAPTURE_WORDS']
    return struct.unpack_from('<%dH' % (256 * 192), struct.pack('<%dI' % D['CAPTURE_WORDS'], *w[at:at + D['CAPTURE_WORDS']]))

def describe(r):
    if not r[0]: return 'skipped'
    out = f"CNT {r[1]:08X} at line 191, {r[2]:08X} at 193, {r[3]:08X} after; DISPCAPCNT {r[4]:08X}"
    if r[5]:
        out += (f" | {r[5]} transfers over {r[14]} frame(s), lines {r[6]}..{r[7]} (lowest {r[8]}, highest {r[9]}), "
                f"{r[13]} line runs, {r[10]} on the first, then {r[11]}-{r[12]} a line")
    else:
        out += " | no transfers recorded"
    return out

def bitmapPixel(i):
    return ((i >> 8) << 8 | (i & 255)) & 0xFFFF

def capture(px):
    # How a capture relates to the bitmap: pixels equal to it, and whether each line is one 16-pixel pattern repeated
    same = sum(1 for i in range(256 * 192) if px[i] == bitmapPixel(i))
    same15 = sum(1 for i in range(256 * 192) if px[i] & 0x7FFF == bitmapPixel(i) & 0x7FFF)
    ring = px[:16]
    repeated = sum(1 for i in range(256 * 192) if px[i] == ring[i & 15])
    lines = []
    for y in (0, 1, 100, 191):
        lines.append(f"    line {y:3d}: " + ' '.join(f"{p:04X}" for p in px[y * 256:y * 256 + 16]) + ' ...')
    return (f"  {same} pixels are the bitmap's ({same15} ignoring bit 15), {repeated} repeat line 0's first 16\n" +
            '\n'.join(lines))

def ppm(path, px):
    with open(path, 'wb') as f:
        f.write(b'P6 256 192 255\n')
        for p in px:
            f.write(bytes(((p & 31) * 255 // 31, ((p >> 5) & 31) * 255 // 31, ((p >> 10) & 31) * 255 // 31)))

a = load(args[0])
b = load(args[1]) if len(args) > 1 else None
print(f"status: {'done' if a[D['R_STATUS']] == D['STATUS_DONE'] else 'stopped at scene %d' % a[D['R_STEP']]}, "
      f"SCFG_EXT {a[D['R_SCFG9']]:08X} ({'DSi mode' if a[D['R_SCFG9']] else 'DS mode'}), "
      f"reads of DISP_MMEM_FIFO {a[D['R_READ32']]:08X} and {a[D['R_READ16']]:04X}")
if b and (a[D['R_READ32']], a[D['R_READ16']]) != (b[D['R_READ32']], b[D['R_READ16']]):
    print(f"  the other read {b[D['R_READ32']]:08X} and {b[D['R_READ16']]:04X}")
same = 0
for s, (name, cap, dsi) in enumerate(SCENES):
    ra, pa = results(a, s), pixels(a, s) if cap else None
    if not b:
        print(f"{name:11s} {describe(ra)}")
        if cap and ra[0]: print(capture(pa))
    else:
        rb, pb = results(b, s), pixels(b, s) if cap else None
        diff = sum(1 for i in range(256 * 192) if pa[i] != pb[i]) if cap else 0
        if ra == rb and not diff:
            same += 1
            continue
        print(f"{name:11s} {describe(ra)}\n{'':11s} {describe(rb)}")
        if diff:
            print(f"  {diff} captured pixels differ\n  this one:\n{capture(pa)}\n  other:\n{capture(pb)}")
    if images and cap and ra[0]:
        os.makedirs(images, exist_ok=True)
        ppm(os.path.join(images, f"{name}.ppm"), pa)
if b: print(f"{same} of {len(SCENES)} match")
sys.exit(1 if b and same != len(SCENES) else 0)
