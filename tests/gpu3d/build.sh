#!/bin/bash
# Builds the 3D ROM from a dump of a frame's polygons
# Usage: build.sh <ROM to take the logo from> [polygon dump, or the existing scene.h is kept]
set -e
here=$(cd "$(dirname "$0")"; pwd); logo=$1; dump=$2
tools=${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi
libgba=${DEVKITPRO:-/opt/devkitpro}/libgba
out=$here/build; mkdir -p "$out"
[ -n "$dump" ] && python3 -I "$here/gen_scene.py" "$dump" "$here/scene.h"
common="-O2 -marm -nostdlib -nostartfiles -ffreestanding -fno-builtin -Wl,--no-warn-rwx-segments -T$here/../rom/link.ld"
"$tools-gcc" $common -mcpu=arm946e-s -DARM9 -DFILE_NAME='"fat:/noodsu-3d.bin"' -DFILE_LABEL='"NOODSU-3D.BIN"' -Wl,--defsym=BASE=0x02000000 -Wl,--defsym=STACKS=0x023F0000 \
    -I"$libgba/include" -L"$libgba/lib" -I"$here/../rom" "$here/start.S" "$here/../rom/save.c" "$here/arm9.c" \
    -Wl,--start-group -lfat -lgba -lsysbase -lc -lgcc -Wl,--end-group -o "$out/arm9.elf"
"$tools-gcc" $common -mcpu=arm7tdmi -Wl,--defsym=BASE=0x02180000 -Wl,--defsym=STACKS=0x0380E000 \
    "$here/start.S" "$here/arm7.c" -lgcc -o "$out/arm7.elf"
"$tools-objcopy" -O binary "$out/arm9.elf" "$out/arm9.bin"
"$tools-objcopy" -O binary "$out/arm7.elf" "$out/arm7.bin"
python3 -I "$here/../rom/make_rom.py" "$out/arm9.bin" "$out/arm7.bin" "$logo" "$out/noodsu-3d.nds"
echo "Built $out/noodsu-3d.nds, ARM9 $(wc -c < "$out/arm9.bin") bytes"
