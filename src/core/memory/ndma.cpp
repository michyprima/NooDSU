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

void Ndma::saveState(FILE *file) {
    // Write DSi state data to the file
    if (!core->dsiMode) return;
    fwrite(srcAddrs, 4, sizeof(srcAddrs) / 4, file);
    fwrite(dstAddrs, 4, sizeof(dstAddrs) / 4, file);
    fwrite(totalLeft, 4, sizeof(totalLeft) / 4, file);
    fwrite(blockLeft, 4, sizeof(blockLeft) / 4, file);
    fwrite(&drqMask, sizeof(drqMask), 1, file);
    fwrite(&runMask, sizeof(runMask), 1, file);
    fwrite(&ndmaGcnt, sizeof(ndmaGcnt), 1, file);
    fwrite(ndmaSad, 4, sizeof(ndmaSad) / 4, file);
    fwrite(ndmaDad, 4, sizeof(ndmaDad) / 4, file);
    fwrite(ndmaTcnt, 4, sizeof(ndmaTcnt) / 4, file);
    fwrite(ndmaWcnt, 4, sizeof(ndmaWcnt) / 4, file);
    fwrite(ndmaBcnt, 4, sizeof(ndmaBcnt) / 4, file);
    fwrite(ndmaFdata, 4, sizeof(ndmaFdata) / 4, file);
    fwrite(ndmaCnt, 4, sizeof(ndmaCnt) / 4, file);
}

void Ndma::loadState(FILE *file) {
    // Read DSi state data from the file
    if (!core->dsiMode) return;
    fread(srcAddrs, 4, sizeof(srcAddrs) / 4, file);
    fread(dstAddrs, 4, sizeof(dstAddrs) / 4, file);
    fread(totalLeft, 4, sizeof(totalLeft) / 4, file);
    fread(blockLeft, 4, sizeof(blockLeft) / 4, file);
    fread(&drqMask, sizeof(drqMask), 1, file);
    fread(&runMask, sizeof(runMask), 1, file);
    fread(&ndmaGcnt, sizeof(ndmaGcnt), 1, file);
    fread(ndmaSad, 4, sizeof(ndmaSad) / 4, file);
    fread(ndmaDad, 4, sizeof(ndmaDad) / 4, file);
    fread(ndmaTcnt, 4, sizeof(ndmaTcnt) / 4, file);
    fread(ndmaWcnt, 4, sizeof(ndmaWcnt) / 4, file);
    fread(ndmaBcnt, 4, sizeof(ndmaBcnt) / 4, file);
    fread(ndmaFdata, 4, sizeof(ndmaFdata) / 4, file);
    fread(ndmaCnt, 4, sizeof(ndmaCnt) / 4, file);
}

bool Ndma::shouldTransfer(int i, uint8_t type) {
    // Check if a channel is enabled and using the triggered DRQ type
    if (~ndmaCnt[i] & BIT(31)) return false;
    return type == ((ndmaCnt[i] >> 24) & 0x1F);
}

void Ndma::trigger(uint8_t type) {
    // Schedule transfers on channels waiting for an event
    bool scheduled = runMask;
    for (int i = 0; i < 4; i++)
        if (shouldTransfer(i, type)) runMask |= BIT(i);
    if (!scheduled && runMask)
        core->schedule(SchedTask(NDMA9_UPDATE + arm7), 1);
}

void Ndma::setDrq(uint8_t type) {
    // Set a DRQ type's active bit and start channels using it
    drqMask |= BIT(type);
    trigger(type);
}

void Ndma::clearDrq(uint8_t type) {
    // Clear a DRQ type's active bit
    drqMask &= ~BIT(type);
}

void Ndma::update() {
    // Perform scheduled transfers and acknowledge them, except for ones that should go again
    uint8_t again = 0;
    for (int i = 0; runMask >> i; i++) {
        if (~runMask & BIT(i)) continue;
        if (transferBlock(i)) again |= BIT(i);
    }
    runMask = again;
    if (runMask) core->schedule(SchedTask(NDMA9_UPDATE + arm7), 1);
}

bool Ndma::cartArmed() {
    // Check if a channel is waiting for words from the DS cart
    for (int i = 0; i < 4; i++) {
        if ((ndmaCnt[i] & BIT(31)) && ((ndmaCnt[i] >> 24) & 0x1F) == 0x4)
            return true;
    }
    return false;
}

void Ndma::transferNow(uint8_t type) {
    // Transfer a block right away on channels using a DRQ type, for events that need the words before the emulator
    // carries on, like the display FIFO
    for (int i = 0; i < 4; i++)
        if (shouldTransfer(i, type)) transferBlock(i);
}

bool Ndma::transferBlock(int i) {
    // Set the destination address step; on a real DSi, the reserved setting acts as fixed
    int dstStep;
    switch ((ndmaCnt[i] >> 10) & 0x3) {
        case 0: dstStep = 4; break; // Increment
        case 1: dstStep = -4; break; // Decrement
        default: dstStep = 0; break; // Fixed
    }

    // Set the source address step or handle special cases
    int srcStep;
    switch ((ndmaCnt[i] >> 13) & 0x3) {
        case 0: srcStep = 4; break; // Increment
        case 1: srcStep = -4; break; // Decrement
        case 2: srcStep = 0; break; // Fixed

    default: // Fill
        srcAddrs[i] = 0x4004118 + i * 0x1C;
        srcStep = 0;
        break;
    }

    // Start a new block unless a GXFIFO transfer is partway through one
    if (!blockLeft[i])
        blockLeft[i] = ndmaWcnt[i] ? ndmaWcnt[i] : 0x1000000;

    // Limit the words to what's left of the total, and in GXFIFO mode only send 112 words at a time
    bool gxFifo = !arm7 && ((ndmaCnt[i] >> 24) & 0x1F) == 0xA;
    bool untilTotal = !(ndmaCnt[i] & (BIT(28) | BIT(29)));
    uint32_t count = blockLeft[i];
    if (gxFifo && count > 112) count = 112;
    if (untilTotal && count > totalLeft[i]) count = totalLeft[i];
    blockLeft[i] -= count;
    if (untilTotal) totalLeft[i] -= count;

    // Transfer the words and adjust source and destination addresses
    while (count--) {
        uint32_t value = core->memory.read<uint32_t>(arm7, srcAddrs[i], false);
        core->memory.write<uint32_t>(arm7, dstAddrs[i], value, false);
        srcAddrs[i] += srcStep;
        dstAddrs[i] += dstStep;
    }

    // Wait for the FIFO to have room for more if the block isn't done
    bool finished = untilTotal && !totalLeft[i];
    if (blockLeft[i] && !finished)
        return core->gpu3D.fifoHalfEmpty();
    blockLeft[i] = 0;

    // Trigger an interrupt at the end of a block, or the end of the total if repeating until then
    if ((ndmaCnt[i] & BIT(30)) && (!untilTotal || finished))
        core->interpreter[arm7].sendInterrupt(28 + i);

    // End the transfer if it doesn't repeat or has reached the total
    if ((ndmaCnt[i] & BIT(28)) || finished) {
        LOG_INFO("ARM%d NDMA channel %d finished transferring\n", arm7 ? 7 : 9, i);
        ndmaCnt[i] &= ~BIT(31);
    }

    // Reload destination and source addresses if enabled
    if (ndmaCnt[i] & BIT(12)) dstAddrs[i] = ndmaDad[i];
    if (ndmaCnt[i] & BIT(15)) srcAddrs[i] = ndmaSad[i];

    // In GXFIFO mode, go again right away if the FIFO is still half empty
    return gxFifo && (ndmaCnt[i] & BIT(31)) && core->gpu3D.fifoHalfEmpty();
}

void Ndma::writeGcnt(uint32_t mask, uint32_t value) {
    // Write to the NDMAGCNT register; arbitration has no effect since transfers finish at once
    mask &= 0x800F0000;
    ndmaGcnt = (ndmaGcnt & ~mask) | (value & mask);
}

void Ndma::writeSad(int i, uint32_t mask, uint32_t value) {
    // Write to one of the NDMAxSAD registers, which are read-only while the channel is enabled
    if (ndmaCnt[i] & BIT(31)) return;
    mask &= 0xFFFFFFFC;
    ndmaSad[i] = (ndmaSad[i] & ~mask) | (value & mask);
}

void Ndma::writeDad(int i, uint32_t mask, uint32_t value) {
    // Write to one of the NDMAxDAD registers, which are read-only while the channel is enabled
    if (ndmaCnt[i] & BIT(31)) return;
    mask &= 0xFFFFFFFC;
    ndmaDad[i] = (ndmaDad[i] & ~mask) | (value & mask);
}

void Ndma::writeTcnt(int i, uint32_t mask, uint32_t value) {
    // Write to one of the NDMAxTCNT registers, which are read-only while the channel is enabled
    if (ndmaCnt[i] & BIT(31)) return;
    mask &= 0xFFFFFFF;
    ndmaTcnt[i] = (ndmaTcnt[i] & ~mask) | (value & mask);
}

void Ndma::writeWcnt(int i, uint32_t mask, uint32_t value) {
    // Write to one of the NDMAxWCNT registers, which are read-only while the channel is enabled
    if (ndmaCnt[i] & BIT(31)) return;
    mask &= 0xFFFFFF;
    ndmaWcnt[i] = (ndmaWcnt[i] & ~mask) | (value & mask);
}

void Ndma::writeBcnt(int i, uint32_t mask, uint32_t value) {
    // Write to one of the NDMAxBCNT registers, which are read-only while the channel is enabled
    // Intervals have no effect since transfers finish at once
    if (ndmaCnt[i] & BIT(31)) return;
    mask &= 0x3FFFF;
    ndmaBcnt[i] = (ndmaBcnt[i] & ~mask) | (value & mask);
}

void Ndma::writeFdata(int i, uint32_t mask, uint32_t value) {
    // Write to one of the NDMAxFDATA registers, which are read-only while the channel is enabled
    if (ndmaCnt[i] & BIT(31)) return;
    ndmaFdata[i] = (ndmaFdata[i] & ~mask) | (value & mask);
}

void Ndma::writeCnt(int i, uint32_t mask, uint32_t value) {
    // Write to one of the NDMAxCNT registers and check if a transfer started
    // While the channel is enabled, nothing else can be changed until it's disabled
    uint32_t old = ndmaCnt[i];
    mask &= (old & BIT(31)) ? BIT(31) : 0xFF0FFC00;
    ndmaCnt[i] = (ndmaCnt[i] & ~mask) | (value & mask);

    // A channel waiting for words from the DS cart relies on them being scheduled
    if (!arm7 && cartArmed()) core->cartridgeNds.flushWord();
    if (!(~old & ndmaCnt[i] & BIT(31))) return;

    // Reload internal values and trigger immediate transfers right away
    srcAddrs[i] = ndmaSad[i];
    dstAddrs[i] = ndmaDad[i];
    totalLeft[i] = ndmaTcnt[i] ? ndmaTcnt[i] : 0x10000000;
    blockLeft[i] = 0;
    uint8_t mode = (ndmaCnt[i] >> 24) & 0x1F;
    if (mode >= 0x10) setDrq(mode);

    // In GXFIFO mode, also start right away if the FIFO is already half empty, like DMA
    if (!arm7 && mode == 0xA && core->gpu3D.fifoHalfEmpty()) trigger(mode);

    // Log started channel information and catch modes that have nothing to trigger them
    bool noSource = arm7 ? (mode == 0x5 || mode == 0x7 || (mode >= 0xD && mode < 0x10)) :
        (mode == 0x5 || (mode >= 0xC && mode < 0x10));
    if (noSource)
        LOG_CRIT("ARM%d NDMA channel %d started in unimplemented mode: 0x%X\n", arm7 ? 7 : 9, i, mode);
    else
        LOG_INFO("ARM%d NDMA channel %d starting in mode 0x%X, transferring from 0x%X to 0x%X with size 0x%X in "
            "blocks of 0x%X\n", arm7 ? 7 : 9, i, mode, ndmaSad[i], ndmaDad[i], ndmaTcnt[i] << 2, ndmaWcnt[i] << 2);
}
