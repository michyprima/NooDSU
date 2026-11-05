#!/usr/bin/env python3
# Puts a flashcart's DLDI driver into a ROM, for flashcarts that don't do that themselves, like devkitPro's dlditool
# Usage: dldi.py <ROM> <driver.dldi>
import struct, sys

MAGIC = bytes.fromhex('EDA58DBF') + b' Chishm\0'
START, END, GLUE_START, GLUE_END, GOT_START, GOT_END, BSS_START, BSS_END = range(0x40, 0x60, 4)
FUNCTIONS = range(0x68, 0x80, 4)
FIX_ALL, FIX_GLUE, FIX_GOT, FIX_BSS = 1, 2, 4, 8

rom = bytearray(open(sys.argv[1], 'rb').read())
driver = bytearray(open(sys.argv[2], 'rb').read())
at = rom.find(MAGIC)
if at < 0: sys.exit('The ROM has no place for a driver')
if driver[:12] != MAGIC: sys.exit("That isn't a DLDI driver")

allocated = rom[at + 0xF]
if driver[0xD] > allocated: sys.exit(f'The driver needs {1 << driver[0xD]} bytes, and the ROM has room for {1 << allocated}')
if rom.find(MAGIC, at + 1) >= 0: sys.exit('The ROM has more than one place for a driver')

word = lambda data, offset: struct.unpack_from('<I', data, offset)[0]
stubStart, driverStart = word(rom, at + START), word(driver, START)
size = 1 << driver[0xD]
move = (stubStart - driverStart) & 0xFFFFFFFF
inside = lambda value: driverStart <= value < driverStart + size
print(f"{driver[0x10:0x40].split(bytes(1))[0].decode('latin1')}: {len(driver)} bytes at {stubStart:08X}, room for {1 << allocated}, flags {driver[0xE]:02X}")

def fix(data, offset):
    value = word(data, offset)
    if inside(value): struct.pack_into('<I', data, offset, (value + move) & 0xFFFFFFFF)

# Sections to fix are found before their pointers move
sections = []
flags = driver[0xE]
if flags & FIX_ALL: sections.append((0x80, len(driver) & ~3))
if flags & FIX_GLUE: sections.append((word(driver, GLUE_START) - driverStart, word(driver, GLUE_END) - driverStart))
if flags & FIX_GOT: sections.append((word(driver, GOT_START) - driverStart, word(driver, GOT_END) - driverStart))
bss = (word(driver, BSS_START) - driverStart, word(driver, BSS_END) - driverStart) if flags & FIX_BSS else None

for offset in [START, END, GLUE_START, GLUE_END, GOT_START, GOT_END, BSS_START, BSS_END, *FUNCTIONS]:
    fix(driver, offset)
done = set()
for start, end in sections:
    for offset in range(start, min(end, len(driver)) & ~3, 4):
        if offset not in done:
            done.add(offset)
            fix(driver, offset)
driver[0xF] = allocated
rom[at:at + len(driver)] = driver
if bss:
    rom[at + bss[0]:at + bss[1]] = bytes(bss[1] - bss[0])
open(sys.argv[1], 'wb').write(rom)
