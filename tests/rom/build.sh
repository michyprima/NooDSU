#!/bin/bash
# Builds the instruction ROM. With a results file from NooDS U's interpreter, the ROM shows on its own screen what differs
# Usage: build.sh <ROM to take the logo from> [expected.bin]
set -e
here=$(cd "$(dirname "$0")"; pwd); logo=$1; results=$2
tools=${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi
libgba=${DEVKITPRO:-/opt/devkitpro}/libgba # Its libfat works anywhere with a DLDI driver
out=$here/build; mkdir -p "$out"
python3 -I "$here/gen.py" "$out/cases.S" "$out/cases.h"
regions9='-DDATA_MAIN=0x02300000 -DDATA_SLOW=0x05000000 -DDATA_DTCM=0x027E1000 -DDATA_ITCM=0x00002000 -DDATA_WRAM=0'
regions7='-DDATA_MAIN=0x02340000 -DDATA_SLOW=0 -DDATA_DTCM=0 -DDATA_ITCM=0 -DDATA_WRAM=0x03808000'
common="-O2 -marm -nostdlib -nostartfiles -ffreestanding -fno-builtin -Wa,--no-warn -Wl,--no-warn-rwx-segments -T$here/link.ld -I$out -I$here"
"$tools-gcc" $common -mcpu=arm946e-s -DARM9 $regions9 -Wl,--defsym=BASE=0x02000000 -Wl,--defsym=STACKS=0x023F0000 \
    -I"$libgba/include" -L"$libgba/lib" "$here/start.S" "$out/cases.S" "$here/common.c" "$here/save.c" "$here/arm9.c" \
    -Wl,--start-group -lfat -lgba -lsysbase -lc -lgcc -Wl,--end-group -o "$out/arm9.elf"
"$tools-gcc" $common -mcpu=arm7tdmi -Wa,-march=armv5te -DARM7 $regions7 -Wl,--defsym=BASE=0x02180000 -Wl,--defsym=STACKS=0x0380E000 \
    "$here/start.S" "$out/cases.S" "$here/common.c" "$here/arm7.c" -lgcc -o "$out/arm7.elf"
for cpu in arm9 arm7; do
    "$tools-objdump" -d "$out/$cpu.elf" > "$out/$cpu.txt"
    python3 -I "$here/check.py" "$out/$cpu.txt"
done
"$tools-objcopy" -O binary "$out/arm9.elf" "$out/arm9.bin"
"$tools-objcopy" -O binary "$out/arm7.elf" "$out/arm7.bin"
if [ -n "$results" ]; then
    offset=$(( $("$tools-nm" "$out/arm9.elf" | awk '$3 == "expected" { print "0x" $1 }') - 0x02000000 ))
    dd if="$results" of="$out/arm9.bin" bs=1 seek=$offset conv=notrunc 2>/dev/null
fi
python3 -I "$here/make_rom.py" "$out/arm9.bin" "$out/arm7.bin" "$logo" "$out/noodsu-instructions.nds"
echo "Built $out/noodsu-instructions.nds, ARM9 $(wc -c < "$out/arm9.bin") bytes, ARM7 $(wc -c < "$out/arm7.bin") bytes"
