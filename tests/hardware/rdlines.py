#!/usr/bin/env python3
# Explains the noodsu-rdlinestest.bin the 3D timing test saves, or finds its block in a RAM dump from tests/jit/game.sh;
# with a second file, such as NooDS U's, it lists only the scenes where the two differ. With --images <folder> it also
# writes each capture as a PPM, and with --lines it says, for each run of lines in a capture, which quad shows and how far
# Usage: rdlines.py <noodsu-rdlinestest.bin or RAM dump> [another to compare] [--images folder] [--lines]
import os, re, struct, sys

h = open(__file__.replace('rdlines.py', 'rdlines.h')).read()
D = {k: int(v, 0) for k, v in re.findall(r'#define (\w+) (-?(?:0x)?[0-9A-Fa-f]+)\b', h)}
D['CAPTURE_WORDS'] = 256 * 192 // 2
SCENES = [(m[0], *map(int, m[1:])) for m in
          re.findall(r'X\((\w+), (\d+), (\d+), (\d+), (\d+), (\d)\)', h[h.index('#define SCENES'):h.index('enum {')])]
RESULTS = D['R_RESULTS']
CAPTURES = RESULTS + len(SCENES) * D['RESULT_WORDS']
TOTAL = CAPTURES + D['CAPTURES'] * D['CAPTURE_WORDS']
NO = D['NO_EVENT']

args = sys.argv[1:]
images = args[args.index('--images') + 1] if '--images' in args else None
if images: del args[args.index('--images'):args.index('--images') + 2]
lines = '--lines' in args
args = [a for a in args if a != '--lines']

def load(path):
    data = open(path, 'rb').read()
    # The magic words are also in the ROM's code, so take the copy whose status and scene make sense
    magic = struct.pack('<2I', D['RDLN_MAGIC0'], D['RDLN_MAGIC1'])
    at = data.find(magic)
    while at >= 0:
        if at % 4 == 0 and at + TOTAL * 4 <= len(data):
            w = struct.unpack_from('<%dI' % TOTAL, data, at)
            if w[D['R_STATUS']] in (0, D['STATUS_DONE']) and w[D['R_STEP']] < len(SCENES): return w
        at = data.find(magic, at + 1)
    sys.exit(f"{path} doesn't have the test's results")

def at192(v):
    # RDLINES_COUNT changes as line 192 starts, which the ROM can see with line 191 still read, so count both as 192
    return v + (1 << 24) + (1 << 8) if v != NO and (v >> 8) & 0xFFFF == 191 else v

def results(w, s):
    r = list(w[RESULTS + s * D['RESULT_WORDS']:RESULTS + (s + 1) * D['RESULT_WORDS']])
    r[D['W_CHANGES']:D['W_CHANGES'] + 4] = map(at192, r[D['W_CHANGES']:D['W_CHANGES'] + 4])
    return r

def describe(r):
    changes = ' '.join(f"f{v >> 24}:{(v >> 8) & 0xFFFF}={v & 0xFF}" for v in r[D['W_CHANGES']:D['W_CHANGES'] + 4] if v != NO)
    flags = ' '.join(f"f{v >> 16}:{v & 0xFFFF}" for v in r[D['W_FLAGS']:D['W_FLAGS'] + 4] if v != NO)
    return (f"rdlines {r[D['W_STEADY']]:2d} (was {r[D['W_BEFORE']]}), polygons {r[D['W_RAMCOUNT']] & 0xFFF}, "
            f"DISP3DCNT {r[D['W_DISP3DCNT']]:04X} | changes {changes or '-'} | bit 12 {flags or '-'}")

def pixels(w, c):
    at = CAPTURES + c * D['CAPTURE_WORDS']
    return struct.unpack_from('<%dH' % (256 * 192), struct.pack('<%dI' % D['CAPTURE_WORDS'], *w[at:at + D['CAPTURE_WORDS']]))

def quad(p):
    # The quad a pixel came from, from its colour (index + 1 in red and green), or 0 for nothing
    return (p & 31 | ((p >> 5) & 31) << 5) if p & 0x8000 else 0

def lineRuns(px):
    # For each line: the quads seen from left to right, as (quad, first x, last x)
    out = []
    for y in range(192):
        runs = []
        for x in range(256):
            q = quad(px[y * 256 + x])
            if runs and runs[-1][0] == q: runs[-1][2] = x
            else: runs.append([q, x, x])
        out.append(tuple(map(tuple, runs)))
    return out

def showCapture(name, px):
    runs = lineRuns(px)
    start = 0
    for y in range(1, 193):
        if y == 192 or runs[y] != runs[start]:
            span = f"{start}" if y - 1 == start else f"{start}-{y - 1}"
            print(f"    lines {span:8s} " + ', '.join(f"quad {q} x{a}-{b}" if q else f"none x{a}-{b}" for q, a, b in runs[start]))
            start = y

a = load(args[0])
b = load(args[1]) if len(args) > 1 else None
print(f"status: {'done' if a[D['R_STATUS']] == D['STATUS_DONE'] else 'stopped at scene %d' % a[D['R_STEP']]}, "
      f"SCFG_EXT {a[D['R_SCFG9']]:08X} ({'DSi mode' if a[D['R_SCFG9']] & 0x10000 else 'DS mode on a DSi' if a[D['R_SCFG9']] else 'DS mode'})")
same = 0
for s, (name, n, w, y0, y1, cap) in enumerate(SCENES):
    label = f"{name:8s} {n:4d}x{w:<3d} lines {y0}-{y1 - 1}"
    if not b:
        print(f"{label:30s} {describe(results(a, s))}")
    elif results(a, s) == results(b, s):
        same += 1
    else:
        print(f"{label:30s} {describe(results(a, s))}\n{'':30s} {describe(results(b, s))}")
if b: print(f"{same} of {len(SCENES)} match")
differ = b and same != len(SCENES)

capped = [s for s in SCENES if s[5]]
for c, scene in enumerate(capped[:D['CAPTURES']]):
    pa = pixels(a, c)
    if b:
        pb = pixels(b, c)
        diff = sum(1 for i in range(256 * 192) if pa[i] != pb[i])
        print(f"capture {scene[0]}: {diff} pixels differ")
        if diff and lines:
            print("  this one:"); showCapture(scene[0], pa)
            print("  other:"); showCapture(scene[0], pb)
    elif lines:
        print(f"capture {scene[0]}:")
        showCapture(scene[0], pa)
    if images:
        os.makedirs(images, exist_ok=True)
        with open(os.path.join(images, f"{scene[0]}.ppm"), 'wb') as f:
            f.write(b'P6 256 192 255\n')
            for p in pa:
                r, g, bl = p & 31, (p >> 5) & 31, (p >> 10) & 31
                f.write(bytes((r * 255 // 31, g * 255 // 31, bl * 255 // 31) if p & 0x8000 else (0, 0, 0)))
sys.exit(1 if differ else 0)
