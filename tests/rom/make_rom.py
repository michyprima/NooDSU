#!/usr/bin/env python3
# Builds a DS ROM from the two CPUs' binaries, with a header a flashcart or emulator accepts
# Usage: make_rom.py <arm9.bin> <arm7.bin> <ROM to take the logo from> <output>
import struct, sys

def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

arm9 = open(sys.argv[1], 'rb').read()
arm7 = open(sys.argv[2], 'rb').read()
logo = open(sys.argv[3], 'rb').read()[0xC0:0x15C]
arm9 += b'\0' * (-len(arm9) % 0x200)
arm7 += b'\0' * (-len(arm7) % 0x200)
ARM9_OFFSET = 0x200
arm7_offset = ARM9_OFFSET + len(arm9)
total = arm7_offset + len(arm7)
capacity = 0
while (0x20000 << capacity) < total:
    capacity += 1

header = bytearray(0x200)
header[0x00:0x0C] = b'NOODS U ROM\0'
header[0x0C:0x10] = b'####' # Flashcarts patch their DLDI driver into homebrew, which they tell by this and the ARM9's offset
header[0x10:0x12] = b'00'
header[0x14] = capacity
struct.pack_into('<IIII', header, 0x20, ARM9_OFFSET, 0x02000000, 0x02000000, len(arm9))
struct.pack_into('<IIII', header, 0x30, arm7_offset, 0x02180000, 0x02180000, len(arm7))
struct.pack_into('<I', header, 0x60, 0x00586000)
struct.pack_into('<I', header, 0x64, 0x001808F8)
struct.pack_into('<I', header, 0x80, total)
struct.pack_into('<I', header, 0x84, 0x200)
header[0xC0:0x15C] = logo
struct.pack_into('<H', header, 0x15C, crc16(logo))
struct.pack_into('<H', header, 0x15E, crc16(header[:0x15E]))
rom = bytes(header) + b'\0' * (ARM9_OFFSET - 0x200) + arm9 + arm7
rom += b'\xFF' * (-len(rom) % 0x20000)
open(sys.argv[4], 'wb').write(rom)
