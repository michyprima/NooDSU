#!/bin/bash
# Builds the test ROM with libnds. With a results file from running it in NooDS U, the ROM shows what differs from that
# Usage: build.sh [results file]
set -e
here=$(cd "$(dirname "$0")"; pwd); results=$1
dkp=${DEVKITPRO:-/opt/devkitpro}; tools=${DEVKITARM:-$dkp/devkitARM}/bin/arm-none-eabi
out=$here/build; mkdir -p "$out"

# NooDS U's results go in a table of the same size either way, so the code is in the same place with or without them
python3 -I - "$out/expected.c" "$results" <<'EOF'
import struct, sys
words = [0x4E4F4E45] + [0] * (2 * 110 * 12 - 1)
if sys.argv[2]:
    data = open(sys.argv[2], 'rb').read()
    words = list(struct.unpack('<%dI' % (2 * 110 * 12), data[:2 * 110 * 12 * 4]))
with open(sys.argv[1], 'w') as f:
    f.write('volatile const unsigned expected[2][110][12] = {\n')
    f.write(',\n'.join(', '.join('0x%08X' % w for w in words[i:i + 12]) for i in range(0, len(words), 12)))
    f.write('\n};\n')
EOF

common="-g -Os -marm -D__NDS__ -I$dkp/calico/include -I$dkp/libnds/include -L$dkp/calico/lib -L$dkp/libnds/lib"
"$tools-gcc" $common -march=armv5te -mtune=arm946e-s -DARM9 -specs="$dkp/calico/share/ds9.specs" \
    "$here/tests.S" "$here/common.c" "$here/screen.c" "$here/arm9.c" "$out/expected.c" -lfat -lnds9 -lcalico_ds9 -o "$out/arm9.elf"
"$tools-gcc" $common -march=armv4t -mtune=arm7tdmi -Wa,-march=armv5te -DARM7 -specs="$dkp/calico/share/ds7.specs" \
    "$here/tests.S" "$here/common.c" "$here/arm7.c" -lnds7 -lcalico_ds7 -o "$out/arm7.elf"
"$dkp/tools/bin/ndstool" -c "$out/noodsu-test.nds" -7 "$out/arm7.elf" -9 "$out/arm9.elf" \
    -b "$dkp/calico/share/nds-icon.bmp" "NooDS U test;Instructions on a real DS;" > /dev/null
echo "Built $out/noodsu-test.nds"

# The shared WRAM test, a ROM of its own
"$tools-gcc" $common -march=armv5te -mtune=arm946e-s -DARM9 -specs="$dkp/calico/share/ds9.specs" \
    "$here/screen.c" "$here/wram9.c" -lfat -lnds9 -lcalico_ds9 -o "$out/wram9.elf"
"$tools-gcc" $common -march=armv4t -mtune=arm7tdmi -DARM7 -specs="$dkp/calico/share/ds7.specs" \
    "$here/wram7.S" "$here/wram7.c" -lnds7 -lcalico_ds7 -o "$out/wram7.elf"
"$dkp/tools/bin/ndstool" -c "$out/noodsu-wram.nds" -7 "$out/wram7.elf" -9 "$out/wram9.elf" \
    -b "$dkp/calico/share/nds-icon.bmp" "NooDS U WRAM test;Shared WRAM on a real DSi;" > /dev/null
echo "Built $out/noodsu-wram.nds"

# The NDMA test, a ROM of its own for a DSi
"$tools-gcc" $common -march=armv5te -mtune=arm946e-s -DARM9 -specs="$dkp/calico/share/ds9.specs" \
    "$here/screen.c" "$here/ndma_tests.c" "$here/ndma9.c" -lfat -lnds9 -lcalico_ds9 -o "$out/ndma9.elf"
"$tools-gcc" $common -march=armv4t -mtune=arm7tdmi -DARM7 -specs="$dkp/calico/share/ds7.specs" \
    "$here/ndma_tests.c" "$here/ndma7.c" -lnds7 -lcalico_ds7 -o "$out/ndma7.elf"
"$dkp/tools/bin/ndstool" -c "$out/noodsu-ndma.nds" -7 "$out/ndma7.elf" -9 "$out/ndma9.elf" \
    -b "$dkp/calico/share/nds-icon.bmp" "NooDS U NDMA test;NDMA on a real DSi;" > /dev/null
echo "Built $out/noodsu-ndma.nds"

# The 3D timing test, a ROM of its own for a DS or a DSi in either mode
"$tools-gcc" $common -march=armv5te -mtune=arm946e-s -DARM9 -specs="$dkp/calico/share/ds9.specs" \
    "$here/screen.c" "$here/rdlines9.c" -lfat -lnds9 -lcalico_ds9 -o "$out/rdlines9.elf"
"$tools-gcc" $common -march=armv4t -mtune=arm7tdmi -DARM7 -specs="$dkp/calico/share/ds7.specs" \
    "$here/rdlines7.c" -lnds7 -lcalico_ds7 -o "$out/rdlines7.elf"
"$dkp/tools/bin/ndstool" -c "$out/noodsu-rdlines.nds" -7 "$out/rdlines7.elf" -9 "$out/rdlines9.elf" \
    -b "$dkp/calico/share/nds-icon.bmp" "NooDS U 3D timing test;3D render timing on a real DS;" > /dev/null
echo "Built $out/noodsu-rdlines.nds"
