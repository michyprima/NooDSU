#!/usr/bin/env python3
# Fails the build if THUMB code has BX PC at an address that isn't word aligned, which jumps to a misaligned ARM address
# that a real CPU may not come back from
# Usage: check.py <objdump -d output>
import re, sys
bad = []
for line in open(sys.argv[1]):
    m = re.match(r'\s*([0-9a-f]+):\s+4778\s', line)
    if m and int(m.group(1), 16) % 4: bad.append(line.strip())
if bad:
    print('BX PC not word aligned:\n' + '\n'.join(bad))
    sys.exit(1)
