#!/usr/bin/env python3
# Compares what the ROM saved on a real DS with a run in NooDS U, and writes each image for looking at
# Usage: compare.py <noodsu-3d.bin from a DS> <the same from NooDS U> <folder for images>
import os, struct, sys

def load(path):
    data = open(path, 'rb').read()
    magic, scenes = struct.unpack_from('<II', data)
    if magic != 0x53443352: sys.exit(f"{path} isn't from this ROM")
    return scenes, struct.unpack_from('<%dH' % (scenes * 256 * 192), data, 8)

def write(path, pixels):
    with open(path, 'wb') as out:
        out.write(b'P6 256 192 255\n')
        for c in pixels:
            out.write(bytes(((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)))

names = ['as in the game', 'anti-aliasing off', 'only anti-aliasing', 'black over white']
scenes, real = load(sys.argv[1])
_, ours = load(sys.argv[2])
os.makedirs(sys.argv[3], exist_ok=True)
for s in range(scenes):
    a = real[s * 256 * 192:(s + 1) * 256 * 192]
    b = ours[s * 256 * 192:(s + 1) * 256 * 192]
    write(f'{sys.argv[3]}/{s + 1}-ds.ppm', a)
    write(f'{sys.argv[3]}/{s + 1}-noods.ppm', b)
    rows = {}
    for i in range(256 * 192):
        if a[i] != b[i]: rows.setdefault(i // 256, []).append(i % 256)
    print(f'{s + 1} {names[s]}: {sum(map(len, rows.values()))} pixels differ on {len(rows)} rows')
    for y, xs in sorted(rows.items(), key=lambda r: -len(r[1]))[:12]:
        x = xs[len(xs) // 2]
        print(f'   y {y}: {len(xs)} pixels, x {xs[0]}-{xs[-1]}, at {x} DS {a[y * 256 + x]:04X} NooDS {b[y * 256 + x]:04X}')
