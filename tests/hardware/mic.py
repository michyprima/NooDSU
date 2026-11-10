#!/usr/bin/env python3
# Explains the noodsu-mictest.bin the DSi microphone test saves, or finds its block in a RAM dump from tests/jit/game.sh; with
# a second file, such as NooDS U's, it shows both side by side and marks the tests where they differ. Sample data differs
# between any two runs, and NooDS U's harness has no microphone input, so the samples are shown but not compared
# Usage: mic.py <noodsu-mictest.bin or RAM dump> [another to compare]
import re, struct, sys

h = open(__file__.replace('mic.py', 'mic.h')).read()
D = {k: int(v, 0) for k, v in re.findall(r'#define (\w+) (-?(?:0x)?[0-9A-Fa-f]+)\b', h)}
TESTS = re.findall(r'X\((\w+)\)', h[h.index('#define TESTS'):h.index('enum {')])
STATUSES = {1: 'done', 2: "the ARM7 didn't answer", 3: 'not in DSi mode'}
TOTAL = D['M_RESULTS'] + len(TESTS) * D['RESULT_WORDS']
SPECIAL = {D['BUSY']: 'busy', D['TIMEOUT']: 'timeout'}

def load(path):
    data = open(path, 'rb').read()
    # The title on screen spells the same bytes, so only a word-aligned match counts
    at = data.find(struct.pack('<2I', D['MIC_MAGIC0'], D['MIC_MAGIC1']))
    while at >= 0 and at % 4: at = data.find(struct.pack('<2I', D['MIC_MAGIC0'], D['MIC_MAGIC1']), at + 1)
    if at < 0: sys.exit(f"{path} doesn't have the test's results")
    return struct.unpack_from('<%dI' % TOTAL, data, at)

def results(w, test):
    at = D['M_RESULTS'] + test * D['RESULT_WORDS']
    return w[at:at + D['RESULT_WORDS']]

def st(s):
    names = [n for b, n in ((1, 'E'), (2, 'H'), (4, 'F'), (8, 'O'), (16, 'I')) if s & b]
    return ''.join(names) or '-'

class T:
    # An event: its time relative to the first event is compared within the poll loop's latency, since the sample
    # clock runs freely and the first event's own time depends on where it was
    def __init__(self, t, t0, what): self.t, self.rel, self.what = t, t - t0, what
    def __eq__(self, o): return isinstance(o, T) and self.what == o.what and abs(self.rel - o.rel) <= 800
    def __str__(self): return '%d(+%.2f):%s' % (self.t, self.rel / 1024, self.what)

def timeline(r):
    # Each event as time:status, with the time also in samples of 1024 cycles, then the reads afterwards
    if r[0] in SPECIAL: return [SPECIAL[r[0]]], [], []
    evs = r[2:2 + min(r[0], 12)]
    ev = ['before ' + st(r[1])] + [T(e >> 5, evs[0] >> 5, st(e & 31)) for e in evs]
    shape = ['after 16 reads ' + st(r[19]), 'then ' + st(r[22]), 'MIC_CNT %04X' % r[23]]
    shown = ['equal halves %d/16' % r[18], 'first words ' + ' '.join('%08X' % v for v in r[14:18]),
             'reads 17-18 %08X %08X' % (r[20], r[21])]
    return ev, shape, shown

def ndma(r):
    if r[0] in SPECIAL: return [SPECIAL[r[0]]], []
    # The poll loop can count words while NDMA is partway through a block, so only the order of the statuses and the words
    # at the end are compared, with the events shown
    evs = r[1:1 + min(r[0], 15)]
    ev = [str(T(e >> 11, evs[0] >> 11, '%dw:%s' % ((e >> 5) & 63, st(e & 31)))) for e in evs]
    order = [st(e & 31) for i, e in enumerate(evs) if i == 0 or (e & 31) != (evs[i - 1] & 31)]
    words = (evs[-1] >> 5) & 63 if evs else 0
    return (['statuses ' + ' '.join(order), 'words %d' % words, 'CNT %08X' % r[16], 'end ' + st(r[17])],
            ev + (['NDMA on at %d' % r[18]] if r[18] else []))

def explain(name, r):
    # A list of lines that are compared between files, and a list only shown
    if name == 'REGS':
        # What SNDEXCNT starts as depends on what launched the test
        return (['MIC_CNT %04X' % r[1], 'MIC_CNT as a word %08X' % r[2],
                 'MIC_CNT after 6F0F %04X, 0 %04X, 7FFF %04X' % (r[4], r[5], r[6]),
                 'SNDEXCNT after 7FFF %04X, FFFF %04X, DFFF %04X, 5FFF %04X, 0 %04X' % r[8:13]],
                ['SNDEXCNT %04X, as a word %08X, without bit 15 %04X, put back %04X' % (r[0], r[3], r[7], r[13]),
                 'codec 0:51 %02X 0:52 %02X 1:2E %02X 1:2F %02X, DSi mode %d' % r[16:21]])
    if name.startswith('TL_'):
        ev, shape, shown = timeline(r)
        return ev + shape, shown
    if name == 'IRQ_EDGE':
        if r[0] in SPECIAL: return [SPECIAL[r[0]]], []
        return ['IF2 at half %d, again after ack %d, after reading 4 %d, half again %d, status %s' % (r[0], r[1], r[2], r[3], st(r[4]))], []
    if name == 'READ_SIZES':
        return (['full then off ' + st(r[0]), 'reads to empty: 16-bit@604 %d, 16-bit@606 %d, 8-bit %d, 32-bit %d' % r[1:5],
                 'cleared FIFO reads the last word again: %s, then %s' % (r[9] == r[8] == r[10], st(r[11]))],
                ['word %08X, half@604 %04X, half@606 %04X, word %08X' % r[5:9], 'cleared FIFO reads %08X %08X' % (r[9], r[10])])
    if name == 'KEEP_CLEAR':
        if r[0] in SPECIAL: return [SPECIAL[r[0]]], []
        return (['half then off %s, on again %s, overrun %s, off/on %s, clear while on %s, clear+off %s, clear while off %s, '
                 'MIC_CNT %04X' % (tuple(st(v) for v in r[0:7]) + (r[7],))], [])
    if name == 'START_DELAY':
        free = max(r[0:12]) - min(r[0:12]) > 300
        return (['sample clock ' + ('runs freely' if free else 'starts when turned on')],
                ['first sample after ' + ' '.join(str(v) for v in r[0:12])])
    if name.startswith('NDMA'):
        return ndma(r)
    if name in ('MUTED', 'UNMUTED'):
        return ([], ['equal halves %d/16, flat %d/16' % (r[8], r[9]), 'words ' + ' '.join('%08X' % v for v in r[0:8])])
    return [' '.join('%X' % v for v in r)], []

a = load(sys.argv[1])
b = load(sys.argv[2]) if len(sys.argv) > 2 else None
print(f"status: {STATUSES.get(a[D['M_STATUS']], a[D['M_STATUS']])}, SCFG_EXT7 {a[D['M_SCFG7']]:08X}")
if b: print(f"other:  {STATUSES.get(b[D['M_STATUS']], b[D['M_STATUS']])}, SCFG_EXT7 {b[D['M_SCFG7']]:08X}")
same = 0
for test, name in enumerate(TESTS):
    ca, sa = explain(name, results(a, test))
    if not b:
        print(f"{name}\n    " + '\n    '.join(map(str, ca + sa)))
        continue
    cb, sb = explain(name, results(b, test))
    if ca == cb: same += 1
    print(f"{name}{'' if ca == cb else '   DIFFERS'}")
    print('  1: ' + '\n     '.join(map(str, ca + sa)))
    print('  2: ' + '\n     '.join(map(str, cb + sb)))
if b: print(f"{same} of {len(TESTS)} match, by the compared parts")
