#!/usr/bin/env python3
# Shows what a real DS does to the flags with multiplies, from the survey in its saved results, and tries rules for the carry
# Usage: multiply.py <saved results> [output.csv]
import collections, struct, sys

OPS = ['MUL', 'MLA', 'UMULL', 'UMLAL', 'SMULL', 'SMLAL', 'THUMB MUL']
GRID = [0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0xFFFF, 0x10000, 0xFFFFFF, 0x1000000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFF00, 0xFFFF0000,
    0xFF000000, 0xAAAAAAAA, 0x55555555, 0x12345678, 0x89ABCDEF, 0xFFFFFF80, 0xC0000000]
RANDOM = 1024
PAIRS = len(GRID) ** 2 + RANDOM
M = 0xFFFFFFFF

def operands():
    pairs = [(rm, rs, 0, 0) for rm in GRID for rs in GRID]
    state = 0x13579BDF
    for _ in range(RANDOM):
        value = []
        for _ in range(4):
            state ^= (state << 13) & M
            state ^= state >> 17
            state ^= (state << 5) & M
            value.append(state)
        pairs.append((value[0], value[1] >> ((value[3] >> 8) & 0x18), value[2], value[3]))
    return pairs

data = open(sys.argv[1], 'rb').read()
at = data.find(b'NMUL')
if at < 0: sys.exit('No survey in that file')
size = struct.unpack_from('<I', data, at + 4)[0]
if size != len(OPS) * PAIRS * 2: sys.exit(f"The survey has {size} bytes, and this script expects {len(OPS) * PAIRS * 2}")
pairs = operands()
tables = [data[at + 8:at + 8 + size], data[at + 8 + size:at + 8 + size * 2]]

rows = []
for cpu, table in enumerate(tables):
    print(f"\nARM{7 if cpu else 9}: tests, carry kept, carry clear afterwards, carry set afterwards, whether the carry going in ever matters")
    for op, name in enumerate(OPS):
        kept = cleared = setAfter = matters = 0
        for pair in range(PAIRS):
            base = (op * PAIRS + pair) * 2
            zero, one = table[base], table[base + 1]
            rows.append((cpu, name, *pairs[pair], (zero >> 1) & 1, (one >> 1) & 1, zero >> 2, one >> 2))
            for carry, flags in ((0, zero), (1, one)):
                got = (flags >> 1) & 1
                kept += got == carry
                cleared += got == 0
                setAfter += got == 1
            matters += ((zero >> 1) & 1) != ((one >> 1) & 1)
        print(f'  {name:10s}{PAIRS * 2:7d}{kept:7d}{cleared:7d}{setAfter:7d}   differs for {matters} of {PAIRS} pairs')

# The rules found so far for the ARM7's carry, which only depends on the operands
def signed(value):
    return value - (1 << 32) if value >> 31 else value

def cycles(multiplier, sign):
    # 8 bits of the multiplier a cycle, stopping when the rest is only sign, which is only zeros if it's unsigned
    value = signed(multiplier) if sign else multiplier
    for count, bits in enumerate((8, 16, 24)):
        if (value >> bits) == 0 or (sign and (value >> bits) == -1): return count + 1
    return 4

def carrySave(multiplicand, multiplier, accumulate, sign):
    # Booth steps of 2 bits, using carry-save adders, with the carry from the last one; the low bit makes negative addends
    # invert the upper bits, and can't reach the carry itself
    multiplicand |= 1
    booth = -(multiplier & 1)
    carry = (multiplicand * booth) & M
    total = (carry + accumulate) & M
    saved = accumulate
    bits = 1
    target = signed(multiplier) if sign else multiplier
    while True:
        for _ in range(4):
            bits += 2
            if bits <= 32:
                following = multiplier & ((1 << bits) - 1)
                if following >> (bits - 1): following -= 1 << bits
            else:
                following = target
            addend = (multiplicand * (following - booth)) & M
            booth = following
            saved ^= carry ^ addend
            total = (total + addend) & M
            carry = (total - saved) & M
        if booth == target or bits >= 33: return carry >> 31

def rule(name, rm, rs, low):
    # None where there's no rule yet: full-length UMULL, UMLAL and SMLAL
    sign = name not in ('UMULL', 'UMLAL')
    accumulate = low if name in ('MLA', 'UMLAL', 'SMLAL') else 0
    if cycles(rs, sign) < 4: return carrySave(rm, rs, accumulate, sign)
    top, next = rs >> 31, (rs >> 30) & 1
    if name in ('MUL', 'MLA', 'THUMB MUL'): return int(top == 1 and next == 0)
    if name == 'SMULL': return int(top != next and top != rm >> 31)
    return None

print('\nHow well the rules explain the ARM7: right, wrong, and without a rule')
for name in OPS:
    right = wrong = none = 0
    for row in rows:
        if row[0] != 1 or row[1] != name: continue
        expected = rule(name, row[2], row[3], row[4])
        if expected is None: none += 1
        elif expected == row[6]: right += 1
        else: wrong += 1
    print(f'  {name:10s}{right:6d}{wrong:6d}{none:6d}')

if len(sys.argv) > 2:
    with open(sys.argv[2], 'w') as out:
        out.write('cpu,op,rm,rs,low,high,carry0,carry1,nz0,nz1\n')
        for cpu, name, rm, rs, low, high, c0, c1, nz0, nz1 in rows:
            out.write(f'{7 if cpu else 9},{name},{rm:08X},{rs:08X},{low:08X},{high:08X},{c0},{c1},{nz0},{nz1}\n')
