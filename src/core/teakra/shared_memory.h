#pragma once
#include <array>
#include <cstdio>
#include "common_types.h"

namespace Teakra {
struct SharedMemory {
    // NooDS U: memory is reached through the host's 32KB pages, 8 of program memory then 8 of data memory, holding
    // little-endian words; a page that isn't mapped reads as 0
    u16 ReadWord(u32 word_address) const {
        const u8* page = pages ? pages[(word_address >> 14) & 0xF] : nullptr;
        if (!page)
            return 0;
        const u8* data = &page[(word_address << 1) & 0x7FFE];
        return (u16)(data[0] | (data[1] << 8));
    }
    void WriteWord(u32 word_address, u16 value) {
        u8* page = pages ? pages[(word_address >> 14) & 0xF] : nullptr;
        if (!page)
            return;
        u8* data = &page[(word_address << 1) & 0x7FFE];
        data[0] = (u8)value;
        data[1] = (u8)(value >> 8);
    }

    void SetPages(u8** new_pages) {
        pages = new_pages;
    }

    u8** pages = nullptr;
};
} // namespace Teakra
