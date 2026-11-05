#!/usr/bin/env python3
# Compares what a real DS saved, in noodsu-results.bin or a .sav file, with what NooDS U's interpreter does, word by word, to show what the differences have in common
# Usage: results.py <saved results> <details from the Mac, from a run with RomTest::detail set to 2> <cases.h>
import collections, re, struct, sys

WORDS = ['r%d' % i for i in range(13)] + ['lr', 'cpsr', 'spsr'] + ['mem+%02X' % (0x40 + i * 4) for i in range(16)]

names = []
active = False
for line in open(sys.argv[3]):
    if line.startswith('static const char *const caseNames'): active = True
    elif line.startswith('};'): active = False
    elif active: names.append(line.strip().rstrip(',').strip('"'))

expected = {}
timed = {}
for line in open(sys.argv[2], errors='replace'):
    if line[:2] == 'D ':
        parts = line.split()
        expected[int(parts[1]), int(parts[2]), int(parts[3])] = [int(x, 16) for x in parts[4:]]

data = open(sys.argv[1], 'rb').read()
start = data.index(b'NRES')
data = data[start:]
compared, count, skipped, stored = struct.unpack_from('<4I', data, 4)
print(f'{compared - count} of {compared} match, {skipped} skipped, details saved for {stored} of {count} differences')

at = 20
perWord = collections.Counter()
pairs = collections.defaultdict(collections.Counter)
patterns = collections.Counter()
examples = {}
rows = []
for i in range(stored):
    if at + 8 + 32 * 4 > len(data): break
    head = struct.unpack_from('<2I', data, at)
    words = struct.unpack_from('<32I', data, at + 8)
    at += 8 + 32 * 4
    cpu, index, chosen = head[0] & 0xFF, head[0] >> 8, head[1]
    want = expected.get((cpu, index, chosen))
    name = names[index] if index < len(names) else str(index)
    if not want:
        rows.append((cpu, name, chosen, 'no value from the Mac'))
        continue
    different = [(WORDS[k], words[k], want[k]) for k in range(32) if words[k] != want[k]]
    key = (cpu, tuple(d[0] for d in different))
    patterns[key] += 1
    examples.setdefault(key, name)
    for word, got, wanted in different:
        perWord[cpu, word] += 1
        pairs[cpu, word][got, wanted] += 1
    rows.append((cpu, name, chosen, ' '.join(f'{w}={g:08X}/{e:08X}' for w, g, e in different) or 'same as the Mac when run again'))

print('\nWords that differ most, by CPU (9 or 7), with the commonest values as DS/NooDS U:')
for (cpu, word), n in perWord.most_common(16):
    top = ', '.join(f'{g:08X}/{e:08X} x{c}' for (g, e), c in pairs[cpu, word].most_common(3))
    print(f'  {"7" if cpu else "9"} {word:8s}{n:6d}  {top}')
print('\nCombinations of words:')
for (cpu, words), n in patterns.most_common(24):
    print(f'  {"7" if cpu else "9"} {n:5d}  {" ".join(words) or "(none)":40s} e.g. {examples[cpu, words]}')
print('\nEvery difference:')
for cpu, name, chosen, text in rows:
    print(f'  {"7" if cpu else "9"} {name} [inputs {chosen // 4} flags {chosen % 4}] {text}')
