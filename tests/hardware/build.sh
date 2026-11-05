#!/bin/bash
# Builds the test ROM. With a results file from running it in NooDS U, the ROM shows what differs from that
# Usage: build.sh <ROM to take the logo from> [results file]
set -e
here=$(cd "$(dirname "$0")"; pwd); logo=$1; results=$2
tools=${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi
out=$here/build; mkdir -p "$out"
common="-Os -marm -nostdlib -nostartfiles -ffreestanding -fno-builtin -Wl,--no-warn-rwx-segments -T$here/link.ld"
"$tools-gcc" $common -mcpu=arm946e-s -DARM9 -Wl,--defsym=BASE=0x02000000 -Wl,--defsym=IRQ_STACK=0x023F0000 \
    -Wl,--defsym=SVC_STACK=0x023F1000 -Wl,--defsym=SYS_STACK=0x023FA000 \
    "$here/start.S" "$here/tests.S" "$here/common.c" "$here/arm9.c" -lgcc -o "$out/arm9.elf"
"$tools-gcc" $common -mcpu=arm7tdmi -Wa,-march=armv5te -DARM7 -Wl,--defsym=BASE=0x037F8000 -Wl,--defsym=IRQ_STACK=0x0380F000 \
    -Wl,--defsym=SVC_STACK=0x0380EC00 -Wl,--defsym=SYS_STACK=0x0380E800 \
    "$here/start.S" "$here/tests.S" "$here/common.c" "$here/arm7.c" -lgcc -o "$out/arm7.elf"
"$tools-objcopy" -O binary "$out/arm9.elf" "$out/arm9.bin"
"$tools-objcopy" -O binary "$out/arm7.elf" "$out/arm7.bin"
if [ -n "$results" ]; then
    offset=$(( $("$tools-nm" "$out/arm9.elf" | awk '$3 == "expected" { print "0x" $1 }') - 0x02000000 ))
    dd if="$results" of="$out/arm9.bin" bs=1 seek=$offset conv=notrunc 2>/dev/null
fi
python3 -I "$here/make_rom.py" "$out/arm9.bin" "$out/arm7.bin" "$logo" "$out/noodsu-test.nds"
echo "Built $out/noodsu-test.nds, ARM9 $(wc -c < "$out/arm9.bin") bytes, ARM7 $(wc -c < "$out/arm7.bin") bytes"
