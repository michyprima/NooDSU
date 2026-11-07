#!/usr/bin/env python3
# Lists where a DS differed from NooDS U, from the noodsu-hwtest.bin the ROM saves to the flashcart's card
# Usage: results.py <noodsu-hwtest.bin> [all]
import os, re, struct, sys

MAX_TESTS, WORDS = 110, 12
here = os.path.dirname(os.path.abspath(__file__))
names = re.findall(r'T\(\w+, "([^"]*)"\)', open(os.path.join(here, 'list.h')).read())
data = open(sys.argv[1], 'rb').read()
words = struct.unpack('<%dI' % (len(data) // 4), data)
if words[0] != 0x54535748: sys.exit("That isn't a file from the hardware test ROM")
ran, normal, boots, restarted, stuck = words[1:6]
size = 2 * MAX_TESTS * WORDS
results, expected = words[6:6 + size], words[6 + size:6 + size * 2]
print(f'{ran} of {len(names)} tests ran ({ran - normal} after START), {boots} boot{"s" if boots != 1 else ""}' +
    (f', restarted in {names[normal + restarted - 1]}' if restarted else '') +
    (f', ARM7 stuck at {stuck} {names[stuck - 1]}' if stuck else ''))
for cpu in range(2):
    differ = 0
    for test in range(ran):
        at = (cpu * MAX_TESTS + test) * WORDS
        got, want = results[at:at + WORDS], expected[at:at + WORDS]
        if got == want and 'all' not in sys.argv[2:]: continue
        differ += got != want
        print(f'ARM{9 - cpu * 2} {test + 1} {names[test]}' + (' differs' if got != want else ''))
        for w in range(WORDS):
            if got[w] != want[w]: print(f'    word {w}: DS {got[w]:08X}, NooDS U {want[w]:08X}')
            elif 'all' in sys.argv[2:] and got[w]: print(f'    word {w}: {got[w]:08X}')
    print(f'ARM{9 - cpu * 2}: {differ} of {ran} differ')
