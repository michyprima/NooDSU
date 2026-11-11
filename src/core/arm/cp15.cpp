/*
    Copyright 2019-2026 Hydr8gon

    This file is part of NooDS U.

    NooDS U is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS U is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS U. If not, see <https://www.gnu.org/licenses/>.
*/

#include "../core.h"

void Cp15::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&ctrlReg, sizeof(ctrlReg), 1, file);
    fwrite(&dtcmReg, sizeof(dtcmReg), 1, file);
    fwrite(&itcmReg, sizeof(itcmReg), 1, file);
    fwrite(&procId, sizeof(procId), 1, file);
    fwrite(codeTags, sizeof(codeTags), 1, file);
    fwrite(&codeTurn, sizeof(codeTurn), 1, file);
}

void Cp15::loadState(FILE *file) {
    // Read state data from the file
    uint32_t ctrl, dtcm, itcm;
    fread(&ctrl, sizeof(ctrl), 1, file);
    fread(&dtcm, sizeof(dtcm), 1, file);
    fread(&itcm, sizeof(itcm), 1, file);
    fread(&procId, sizeof(procId), 1, file);
    fread(codeTags, sizeof(codeTags), 1, file);
    fread(&codeTurn, sizeof(codeTurn), 1, file);

    // Mark the lines the code cache holds
    memset(codeLines, 0, sizeof(codeLines));
    for (int i = 0; i < CODE_SETS * CODE_WAYS; i++)
        if (uint32_t tag = codeTags[i / CODE_WAYS][i % CODE_WAYS]) codeLines[(tag - 1) & (sizeof(codeLines) - 1)] = 1;

    // Set registers along with values based on them
    write(1, 0, 0, ctrl);
    write(9, 1, 0, dtcm);
    write(9, 1, 1, itcm);
}

uint32_t Cp15::read(uint8_t cn, uint8_t cm, uint8_t cp) {
    // Read a value from a CP15 register
    switch ((cn << 16) | (cm << 8) | (cp << 0)) {
        case 0x000000: return 0x41059461; // Main ID
        case 0x000001: return 0x0F0D2112; // Cache type
        case 0x010000: return ctrlReg; // Control
        case 0x090100: return dtcmReg; // Data TCM base/size
        case 0x090101: return itcmReg; // Instruction TCM size
        case 0x0D0001: case 0x0D0101: return procId; // Trace process ID

    default:
        LOG_WARN("Unknown CP15 register read: C%d,C%d,%d\n", cn, cm, cp);
        return 0;
    }
}

void Cp15::write(uint8_t cn, uint8_t cm, uint8_t cp, uint32_t value) {
    // Write a value to a CP15 register
    uint32_t oldAddr, oldSize;
    switch ((cn << 16) | (cm << 8) | (cp << 0)) {
    case 0x010000: // Control
        // Set writable control bits and update their state values
        ctrlReg = (ctrlReg & ~0xFF085) | (value & 0xFF085);
        exceptionAddr = (ctrlReg & BIT(13)) ? 0xFFFF0000 : 0x00000000;
        dtcmCanRead = (ctrlReg & BIT(16)) && !(ctrlReg & BIT(17));
        dtcmCanWrite = (ctrlReg & BIT(16));
        itcmCanRead = (ctrlReg & BIT(18)) && !(ctrlReg & BIT(19));
        itcmCanWrite = (ctrlReg & BIT(18));

        // Update the memory map at the current TCM locations
        core->memory.updateMap9(dtcmAddr, dtcmAddr + dtcmSize, true);
        core->memory.updateMap9(0x00000000, itcmSize, true);
        return;

    case 0x090100: // Data TCM base/size
        // Set the DTCM register and update its address and size
        dtcmReg = value;
        oldAddr = dtcmAddr;
        oldSize = dtcmSize;
        dtcmAddr = dtcmReg & 0xFFFFF000;
        dtcmSize = std::max(0x1000, 0x200 << ((dtcmReg >> 1) & 0x1F)); // Min 4KB

        // Update the memory map at the old and new DTCM areas
        core->memory.updateMap9(oldAddr, oldAddr + oldSize, true);
        core->memory.updateMap9(dtcmAddr, dtcmAddr + dtcmSize, true);
        return;

    case 0x070004: case 0x070802: // Wait for interrupt
        // Halt the CPU
        core->interpreter[0].halt(0);
        return;

    case 0x090101: // Instruction TCM size
        // Set the ITCM register and update its size
        itcmReg = value;
        oldSize = itcmSize;
        itcmSize = std::max(0x1000, 0x200 << ((itcmReg >> 1) & 0x1F)); // Min 4KB

        // Update the memory map at the old and new ITCM areas
        core->memory.updateMap9(0x00000000, std::max(oldSize, itcmSize), true);
        return;

    case 0x070500: // Invalidate the code cache
        codeFlush();
        return;

    case 0x070501: // Invalidate a line of the code cache
        // Forget the line an address is in, if the code cache holds it
        for (int i = 0; i < CODE_WAYS; i++) {
            uint32_t at = ((value >> 5) & (CODE_SETS - 1)) * CODE_WAYS + i;
            if ((value >> 24) == 0x02 && codeTags[at / CODE_WAYS][i] == ((value & 0xFFFFFF) >> 5) + 1) codeForget(at);
        }
        return;

    case 0x0D0001: case 0x0D0101: // Trace process ID
        // Set the trace process ID register
        procId = value;
        return;

    default:
        LOG_WARN("Unknown CP15 register write: C%d,C%d,%d\n", cn, cm, cp);
        return;
    }
}

void Cp15::codeForget(uint32_t at) {
    // Empty one of the code cache's places
    uint32_t &tag = codeTags[at / CODE_WAYS][at % CODE_WAYS];
    if (tag) codeLines[tag - 1] = 0;
    tag = 0;
}

void Cp15::codeFlush() {
    // Forget every line the code cache holds
    for (int i = 0; i < CODE_SETS * CODE_WAYS; i++)
        codeForget(i);
}

int Cp15::codeMiss(uint32_t address) {
    // Fetch a line of main RAM into the code cache, in place of whatever its set's next place held, and return how long
    // that takes
    uint32_t line = (address & 0xFFFFFF) >> 5;
    uint32_t at = (line & (CODE_SETS - 1)) * CODE_WAYS + (codeTurn++ & (CODE_WAYS - 1));
    codeForget(at);
    codeTags[at / CODE_WAYS][at % CODE_WAYS] = line + 1;
    codeLines[line] = 1;
    return MISS_TICKS;
}

int Cp15::codeMissCall(Cp15 *cp15, uint32_t address) {
    // Entry point for generated code
    return cp15->codeMiss(address);
}
