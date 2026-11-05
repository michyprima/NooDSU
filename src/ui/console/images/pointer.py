# Draws the right stick pointer from signed distance shapes, with the other designs it was chosen from
#   python3 pointer.py sheet.png         a preview sheet of every design, numbered
#   python3 pointer.py 3 pointer.bmp     one design as a BMP like the other UI images (3 is the one in use)
# It prints where the hotspot is as a share of the image; ConsoleUI::updatePointer offsets the hand by that
import math, struct, zlib, sys

def capsule(px, py, ax, ay, bx, by, r):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy) - r

def box(px, py, cx, cy, hx, hy, r):
    qx, qy = abs(px - cx) - hx + r, abs(py - cy) - hy + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r

def cone(px, py, ax, ay, ra, bx, by, rb):
    # A capsule whose radius goes from ra to rb
    n = 24
    return min(capsule(px, py, ax + (bx - ax) * i / n, ay + (by - ay) * i / n, ax + (bx - ax) * (i + 1) / n,
        ay + (by - ay) * (i + 1) / n, ra + (rb - ra) * (i + 0.5) / n) for i in range(n))

def poly(px, py, pts):
    d = float('inf'); inside = False
    for i in range(len(pts)):
        (ax, ay), (bx, by) = pts[i], pts[i - 1]
        d = min(d, capsule(px, py, ax, ay, bx, by, 0))
        if (ay > py) != (by > py) and px < (bx - ax) * (py - ay) / (by - ay) + ax: inside = not inside
    return -d if inside else d

def smin(a, b, k):
    h = max(0.0, min(1.0, 0.5 + 0.5 * (b - a) / k))
    return b * (1 - h) + a * h - k * h * (1 - h)

# The pointing hand, a right hand seen from the back with the index finger up and the thumb out to the left
def hand(x, y):
    index = capsule(x, y, 26, 9, 26, 36, 6.2)
    middle = box(x, y, 37.5, 35, 5.6, 7, 5.4)
    ring = box(x, y, 47.6, 37.5, 5.2, 6.5, 5)
    pinky = box(x, y, 56.2, 41, 4.6, 6, 4.5)
    palm = box(x, y, 40, 49, 19, 11, 10)
    thumb = capsule(x, y, 24, 50, 12, 37, 6)
    d = smin(smin(smin(smin(index, middle, 1.2), ring, 1.2), pinky, 1.2), palm, 3)
    return smin(d, thumb, 4)
def handLines(x, y):
    return min(capsule(x, y, 32.1, 29, 32.1, 40, 0), capsule(x, y, 42.7, 32, 42.7, 42.5, 0),
        capsule(x, y, 52.2, 36, 52.2, 44.5, 0))

WHITE = lambda x, y: (lambda t: (int(255 - 30 * t), int(255 - 27 * t), int(255 - 20 * t)))(max(0, min(1, (y - 10) / 55)))
DARK = (28, 28, 34)
CYAN = (0, 150, 205)

# Each design paints layers in order, each a shape with a fill, an outline and inner lines
# Placement: units per pixel, the hotspot in design units, where it goes in the image, and a tilt in degrees
def handDesign(outline, width, tilt):
    return dict(layers=[(hand, WHITE, outline, width, handLines)], hot=(26, 2.8), at=(24 if tilt else 44, 6), tilt=tilt,
        scale=0.56 if tilt else 0.53)

def glove(x, y):
    return hand(x, y)
def cuff(x, y):
    return box(x, y, 40, 63, 17.5, 5.5, 4.5)
def gloveLines(x, y):
    return handLines(x, y)

def stylusTip(x, y): return cone(x, y, 5, 5, 1.0, 15, 15, 4.6)
def stylusBody(x, y): return capsule(x, y, 15, 15, 52, 52, 5.2)
def stylusCap(x, y): return capsule(x, y, 50, 50, 57, 57, 5.6)
def stylusLines(x, y): return capsule(x, y, 18, 15.5, 47, 44.5, 0) - 0.2

ARROW = [(8, 4), (8, 54), (20, 43), (28.5, 61), (37, 57), (28.5, 40), (44, 40)]
def arrow(x, y): return poly(x, y, ARROW)

GRAY = lambda x, y: (200, 204, 212)
STEEL = lambda x, y: (90, 96, 108)
designs = [
    ("Pointing hand", handDesign(DARK, 2.6, 0)),
    ("Tilted hand", handDesign(DARK, 2.6, -22)),
    ("Tilted, blue outline", handDesign(CYAN, 3.0, -22)),
    ("Glove with cuff", dict(layers=[(glove, WHITE, DARK, 3.2, gloveLines), (cuff, WHITE, DARK, 3.2, None)],
        hot=(26, 2.8), at=(30, 6), tilt=-15, scale=0.6)),
    ("Stylus", dict(layers=[(stylusBody, WHITE, DARK, 2.6, stylusLines), (stylusCap, GRAY, DARK, 2.6, None),
        (stylusTip, STEEL, DARK, 2.0, None)], hot=(5, 5), at=(6, 6), tilt=0, scale=0.5)),
    ("Arrow", dict(layers=[(arrow, WHITE, DARK, 2.6, None)], hot=(8, 4), at=(8, 6), tilt=0, scale=0.55)),
]

SIZE, SS = 128, 3
def render(d):
    c, s = math.cos(math.radians(d['tilt'])), math.sin(math.radians(d['tilt']))
    def toDesign(ix, iy):
        # Image pixels to design units, turning around the hotspot
        ux, uy = (ix - d['at'][0]) * d['scale'], (iy - d['at'][1]) * d['scale']
        return d['hot'][0] + ux * c + uy * s, d['hot'][1] - ux * s + uy * c
    def sample(x, y):
        color, alpha = None, 0.0
        for shape, fill, edge, width, lines in d['layers']:
            v = shape(x, y)
            if v < 0:
                color, alpha = fill(x, y), 1.0
                if lines and lines(x, y) < width * 0.4: color = edge
            elif v < width:
                color, alpha = edge, 1.0
        if alpha == 0.0:
            if min(shape(x - 1.6, y - 2.4) for shape, _, _, w, _ in d['layers']) < 2.6:
                return (0, 0, 0), 0.3
        return color or (0, 0, 0), alpha
    px = []
    for iy in range(SIZE):
        row = []
        for ix in range(SIZE):
            r = g = b = a = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    col, al = sample(*toDesign(ix + (sx + .5) / SS, iy + (sy + .5) / SS))
                    r += col[0] * al; g += col[1] * al; b += col[2] * al; a += al
            if a: r, g, b = r / a, g / a, b / a
            row.append((r, g, b, a / SS / SS))
        px.append(row)
    return px

def writeBmp(path, px):
    data = b''.join(bytes((int(p[2] + .5), int(p[1] + .5), int(p[0] + .5), int(p[3] * 255 + .5)))
        for row in reversed(px) for p in row)
    n = len(px)
    open(path, 'wb').write(struct.pack('<2sIHHI', b'BM', 0x46 + len(data), 0, 0, 0x46)
        + struct.pack('<IiiHHIIiiII', 0x38, n, n, 1, 32, 3, len(data), 0x2E23, 0x2E23, 0, 0)
        + struct.pack('<IIII', 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000) + data)

def writePng(path, rows):
    raw = b''.join(b'\0' + bytes(min(255, max(0, int(c + .5))) for p in r for c in p) for r in rows)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', len(rows[0]), len(rows), 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

DIGITS = ["111 010 111 111 101 111 111", "101 110 001 001 101 100 100", "101 010 111 111 111 111 111",
    "101 010 100 001 001 001 101", "111 111 111 111 001 111 111"]
def digit(n, x, y): return DIGITS[y][(n % 7) * 4 + x] == '1'

if __name__ == '__main__':
    if len(sys.argv) > 2:
        # Write one design's BMP and print its hotspot as a share of the image
        d = designs[int(sys.argv[1]) - 1][1]
        writeBmp(sys.argv[2], render(d))
        print('hotspot %.3f %.3f' % (d['at'][0] / SIZE, d['at'][1] / SIZE))
        sys.exit()
    images = [render(d) for _, d in designs]
    # Each column: the number, the image enlarged over a checkerboard, then game size on light, mid and dark
    CELL, BIG = 300, 256
    W, H = CELL * len(images), 70 + BIG + 20 + 3 * 90
    sheet = [[(250, 250, 250)] * W for _ in range(H)]
    for n, img in enumerate(images):
        ox = n * CELL + (CELL - BIG) // 2
        for y in range(5):
            for x in range(3):
                if digit(n + 1, x, y):
                    for yy in range(10):
                        for xx in range(10):
                            sheet[12 + y * 10 + yy][n * CELL + CELL // 2 - 15 + x * 10 + xx] = (40, 40, 40)
        for y in range(BIG):
            for x in range(BIG):
                p = img[y * SIZE // BIG][x * SIZE // BIG]
                bg = 150 if ((x // 16 + y // 16) & 1) else 190
                sheet[70 + y][ox + x] = tuple(p[i] * p[3] + bg * (1 - p[3]) for i in range(3))
        for k, bg in enumerate([(235, 230, 215), (70, 140, 90), (25, 30, 45)]):
            y0 = 70 + BIG + 20 + k * 90
            for y in range(80):
                for x in range(BIG):
                    sheet[y0 + y][ox + x] = bg
            # Average 2x2 blocks for the size it's drawn at on the GamePad
            for y in range(SIZE // 2):
                for x in range(SIZE // 2):
                    ps = [img[2 * y + j][2 * x + i] for j in (0, 1) for i in (0, 1)]
                    a = sum(p[3] for p in ps) / 4
                    col = [sum(p[c] * p[3] for p in ps) / 4 for c in range(3)]
                    sheet[y0 + 8 + y][ox + 96 + x] = tuple(col[c] + bg[c] * (1 - a) for c in range(3))
    writePng(sys.argv[1], sheet)
