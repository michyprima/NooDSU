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

#include <cstring>
#include "../core.h"
#include "../file_lock.h"

SdMmc::~SdMmc() {
    // Close the NAND and SD files
    std::lock_guard<FileLock> guard(fileLock());
    fclose(nand);
    fclose(sd);
}

void SdMmc::saveState(FILE *file) {
    // Write DSi state data to the file
    if (!core->dsiMode) return;
    fwrite(&cardStatus, sizeof(cardStatus), 1, file);
    fwrite(&opCond, sizeof(opCond), 1, file);
    fwrite(&blockLen, sizeof(blockLen), 1, file);
    fwrite(&curAddress, sizeof(curAddress), 1, file);
    fwrite(&curBlock, sizeof(curBlock), 1, file);
    fwrite(&busWidth, sizeof(busWidth), 1, file);
    fwrite(&detecting, sizeof(detecting), 1, file);
    fwrite(&detectAt, sizeof(detectAt), 1, file);
    fwrite(&sdCmd, sizeof(sdCmd), 1, file);
    fwrite(&sdPortSelect, sizeof(sdPortSelect), 1, file);
    fwrite(&sdCmdParam, sizeof(sdCmdParam), 1, file);
    fwrite(&sdData16Blkcnt, sizeof(sdData16Blkcnt), 1, file);
    fwrite(sdResponse, 4, sizeof(sdResponse) / 4, file);
    fwrite(&sdIrqStatus, sizeof(sdIrqStatus), 1, file);
    fwrite(&sdIrqMask, sizeof(sdIrqMask), 1, file);
    fwrite(&sdData16Blklen, sizeof(sdData16Blklen), 1, file);
    fwrite(&sdCardOption, sizeof(sdCardOption), 1, file);
    fwrite(&sdErrDetail, sizeof(sdErrDetail), 1, file);
    fwrite(&sdData16Fifo, sizeof(sdData16Fifo), 1, file);
    fwrite(&sdDataCtl, sizeof(sdDataCtl), 1, file);
    fwrite(&sdData32Irq, sizeof(sdData32Irq), 1, file);
    fwrite(&sdData32Blklen, sizeof(sdData32Blklen), 1, file);
    fwrite(&sdData32Fifo, sizeof(sdData32Fifo), 1, file);
    fwrite(&sdCardIrqCtl, sizeof(sdCardIrqCtl), 1, file);
    fwrite(&sdCardIrqStatus, sizeof(sdCardIrqStatus), 1, file);
    fwrite(&sdCardIrqMask, sizeof(sdCardIrqMask), 1, file);
    fwrite(&sdSoftReset, sizeof(sdSoftReset), 1, file);
    SaveStates::writeFifo(dataFifo16, file);
    SaveStates::writeFifo(dataFifo32, file);
}

void SdMmc::loadState(FILE *file) {
    // Read DSi state data from the file
    if (!core->dsiMode) return;
    fread(&cardStatus, sizeof(cardStatus), 1, file);
    fread(&opCond, sizeof(opCond), 1, file);
    fread(&blockLen, sizeof(blockLen), 1, file);
    fread(&curAddress, sizeof(curAddress), 1, file);
    fread(&curBlock, sizeof(curBlock), 1, file);
    fread(&busWidth, sizeof(busWidth), 1, file);
    fread(&detecting, sizeof(detecting), 1, file);
    fread(&detectAt, sizeof(detectAt), 1, file);
    fread(&sdCmd, sizeof(sdCmd), 1, file);
    fread(&sdPortSelect, sizeof(sdPortSelect), 1, file);
    fread(&sdCmdParam, sizeof(sdCmdParam), 1, file);
    fread(&sdData16Blkcnt, sizeof(sdData16Blkcnt), 1, file);
    fread(sdResponse, 4, sizeof(sdResponse) / 4, file);
    fread(&sdIrqStatus, sizeof(sdIrqStatus), 1, file);
    fread(&sdIrqMask, sizeof(sdIrqMask), 1, file);
    fread(&sdData16Blklen, sizeof(sdData16Blklen), 1, file);
    fread(&sdCardOption, sizeof(sdCardOption), 1, file);
    fread(&sdErrDetail, sizeof(sdErrDetail), 1, file);
    fread(&sdData16Fifo, sizeof(sdData16Fifo), 1, file);
    fread(&sdDataCtl, sizeof(sdDataCtl), 1, file);
    fread(&sdData32Irq, sizeof(sdData32Irq), 1, file);
    fread(&sdData32Blklen, sizeof(sdData32Blklen), 1, file);
    fread(&sdData32Fifo, sizeof(sdData32Fifo), 1, file);
    fread(&sdCardIrqCtl, sizeof(sdCardIrqCtl), 1, file);
    fread(&sdCardIrqStatus, sizeof(sdCardIrqStatus), 1, file);
    fread(&sdCardIrqMask, sizeof(sdCardIrqMask), 1, file);
    fread(&sdSoftReset, sizeof(sdSoftReset), 1, file);
    SaveStates::readFifo(dataFifo16, file);
    SaveStates::readFifo(dataFifo32, file);
}

uint32_t *SdMmc::init() {
    // Check the SD's capacity so cards over 2GB can be handled differently
    std::lock_guard<FileLock> guard(fileLock());
#ifdef JIT_SELFTEST
    // The self-test runs two cores at once, and the Wii U can't open a file twice if one of them can write to it
    const char *mode = "rb";
#else
    const char *mode = "rb+";
#endif

    // The files are read through caches in large chunks, which the standard library's buffering would only split up
    if (sd = fopen(Settings::sdImagePath.c_str(), mode)) {
        setvbuf(sd, nullptr, _IONBF, 0);
        fseeko(sd, 0, SEEK_END);
        sdSize = ftello(sd);
        sdhc = (sdSize > 0x40000000);
        LOG_INFO("SD card is being treated as %s-capacity\n", sdhc ? "high" : "standard");

        // Request the card inserted interrupt, as if the card was detected at power on
        sdIrqStatus |= BIT(4);
    }
    else {
        // Report the SD slot as empty, without the card detect and write-enable bits
        sdIrqStatus &= ~0xA0;
    }

    // Try to open a NAND dump and load IDs from its nocash footer
    if (!(nand = fopen(Settings::dsiNandPath.c_str(), mode))) return nullptr;
    setvbuf(nand, nullptr, _IONBF, 0);
    fseeko(nand, 0, SEEK_END);
    nandSize = ftello(nand);
    uint8_t footer[0x18];
    fseek(nand, -0x30, SEEK_END);
    fread(footer, sizeof(uint8_t), sizeof(footer), nand);
    for (int i = 0; i < 4; i++)
        mmcCid[i] = U8TO32(footer, i * 4);
    for (int i = 0; i < 2; i++)
        consoleId[i] = U8TO32(footer, 0x10 + i * 4);
    return mmcCid;
}

void SdMmc::scheduleRead() {
    // Read the next block of data a little later
    core->schedule(sdio ? SDIO_READ_BLOCK : SDMMC_READ_BLOCK, 1);
}

void SdMmc::scheduleWrite() {
    // Write the next block of data a little later
    core->schedule(sdio ? SDIO_WRITE_BLOCK : SDMMC_WRITE_BLOCK, 1);
}

void SdMmc::startDetect() {
    // Detect the card once its detect time has passed with SD_CARD_OPTION's current setting, which can change on the way
    int n = sdCardOption & 0xF;
    detectAt = core->globalCycles + ((n == 0xF) ? 0x100 : (0x400 << n));
    core->schedule(SDMMC_CARD_DETECT, detectAt - core->globalCycles);
}

void SdMmc::detectCard() {
    // Request the card inserted interrupt, which the system waits for after resetting the controller before it uses the
    // card; ignore events left from an earlier detect time
    if (!detecting || int32_t(core->globalCycles - detectAt) < 0) return;
    detecting = false;
    sendInterrupt(4);
}

void SdMmc::sendInterrupt(int bit) {
    // Set the interrupt's request bit
    sdIrqStatus |= BIT(bit);

    // Send an interrupt to the ARM9 if the new bit is unmasked
    if (~sdIrqMask & BIT(bit))
        core->interpreter[1].sendInterrupt(irqBit());
}

void SdMmc::startWrite() {
    // Change to write state
    cardStatus = (cardStatus & ~0x1E00) | (0x6 << 9);

    // Write a block right away if the FIFO is full, which NDMA started before the command can do; otherwise trigger a
    // DRQ for the first block, and a FIFO empty interrupt
    if (sdDataCtl & BIT(1)) { // 32-bit
        if ((dataFifo32.size() << 2) >= sdData32Blklen)
            return scheduleWrite();
        core->ndma[1].setDrq(drqType());
        if (dataFifo32.empty() && (sdData32Irq & BIT(12)))
            core->interpreter[1].sendInterrupt(irqBit());
    }
    else { // 16-bit
        if ((dataFifo16.size() << 1) >= sdData16Blklen)
            return scheduleWrite();
        core->ndma[1].setDrq(drqType());
        if (dataFifo16.empty())
            sendInterrupt(25);
    }
}

uint32_t SdMmc::popFifo() {
    // Handle 32-bit FIFO mode
    if (sdDataCtl & BIT(1)) {
        // Pop a value from the 32-bit FIFO and check if empty
        if (dataFifo32.empty()) return 0;
        uint32_t value = dataFifo32.front();
        dataFifo32.pop_front();
        sdData32Irq &= ~BIT(8); // Not full
        if (!dataFifo32.empty()) return value;

        // Trigger a 32-bit FIFO empty interrupt if enabled and finish a block
        sdData32Irq &= ~BIT(9); // Empty
        if (sdData32Irq & BIT(12))
            core->interpreter[1].sendInterrupt(irqBit());
        curBlock--;
        return value;
    }

    // Pop two values from the 16-bit FIFO and check if empty
    if (dataFifo16.empty()) return 0;
    uint32_t value = dataFifo16.front();
    dataFifo16.pop_front();
    if (!dataFifo16.empty()) {
        value |= dataFifo16.front() << 16;
        dataFifo16.pop_front();
    }
    if (!dataFifo16.empty()) return value;

    // Trigger a 16-bit FIFO empty interrupt and finish a block
    sendInterrupt(25);
    curBlock--;
    return value;
}

void SdMmc::pushFifo(uint32_t value) {
    // Handle 32-bit FIFO mode
    if (sdDataCtl & BIT(1)) {
        // Push a value to the 32-bit FIFO and check if full
        uint32_t size = (dataFifo32.size() << 2);
        if (size >= 0x200) return;
        dataFifo32.push_back(value);
        sdData32Irq |= BIT(9); // Not empty
        if (size + 4 < sdData32Blklen) return;

        // Trigger a 32-bit FIFO full interrupt if enabled and finish a block
        sdData32Irq |= BIT(8);
        if (sdData32Irq & BIT(11))
            core->interpreter[1].sendInterrupt(irqBit());
        curBlock--;
        return;
    }

    // Push two values to the 16-bit FIFO and check if full
    uint32_t size = (dataFifo16.size() << 1);
    if (size >= 0x200) return;
    dataFifo16.push_back(value);
    dataFifo16.push_back(value >> 16);
    if (size + 4 < sdData16Blklen) return;

    // Trigger a 16-bit FIFO full interrupt and finish a block
    sendInterrupt(24);
    curBlock--;
}

void SdMmc::pushResponse(uint32_t value) {
    // Push a value to the response registers, shifting old data over
    sdResponse[3] = sdResponse[2];
    sdResponse[2] = sdResponse[1];
    sdResponse[1] = sdResponse[0];
    sdResponse[0] = value;
}

void SdMmc::pushBytes(const uint8_t *data, int size) {
    // Push a register sent over the data lines to a FIFO, its first byte lowest like block data
    curBlock = 1;
    for (int i = 0; i < size; i += 4)
        pushFifo(U8TO32(data, i));
}

void SdMmc::readBlock() {
    // Check for high capacity and hardcode the block length if so
    bool hc = (~sdPortSelect & sdhc);
    uint32_t len = (hc ? 0x80 : blockLen);

    // Read a block of data from the SD or MMC if present, in 512- or 1-byte units, or from the Wi-Fi chip in blocks as
    // long as the block length
    uint8_t data[0x200];
    if (sdio) {
        uint32_t size = sdData16Blklen ? sdData16Blklen : 0x200;
        memset(data, 0, sizeof(data));
        core->dsiWifi.readData(data, size);
        len = (size + 3) >> 2;
    }
    else if (FILE *src = (sdPortSelect & BIT(0)) ? nand : sd) {
        FileCache &cache = (sdPortSelect & BIT(0)) ? nandCache : sdCache;
        cache.read(src, hc ? (int64_t(curAddress) << 9) : curAddress, data, len << 2);
    }
    else {
        memset(data, 0, sizeof(data));
    }

    // Push the data to a FIFO
    LOG_INFO("Reading %s block from 0x%X with size 0x%X\n",
        name(), curAddress, len << 2);
    for (int i = 0; i < len; i++) pushFifo(U8TO32(data, i * 4));

    // Change to read or idle state based on blocks left and trigger a DRQ
    cardStatus = (cardStatus & ~0x1E00) | ((curBlock ? 0x5 : 0x4) << 9);
    curAddress += hc ? 1 : (len << 2);
    core->ndma[1].setDrq(drqType());
}

void SdMmc::writeBlock() {
    // Check for high capacity and hardcode the block length if so
    bool hc = (~sdPortSelect & sdhc);
    uint32_t len = (hc ? 0x80 : blockLen);
    if (sdio) len = ((sdData16Blklen ? sdData16Blklen : 0x200) + 3) >> 2;

    // Pop a block of data from a FIFO, keeping its bytes in order on any host
    uint8_t data[0x200];
    LOG_INFO("Writing %s block to 0x%X with size 0x%X\n",
        name(), curAddress, len << 2);
    for (int i = 0; i < len; i++) {
        uint32_t value = popFifo();
        for (int j = 0; j < 4; j++)
            data[i * 4 + j] = value >> (j * 8);
    }

    // Write the data to the SD or MMC if present, in 512- or 1-byte units, or to the Wi-Fi chip
    if (sdio) {
        core->dsiWifi.writeData(data, sdData16Blklen ? sdData16Blklen : 0x200);
    }
    else if (FILE *dst = (sdPortSelect & BIT(0)) ? nand : sd) {
        FileCache &cache = (sdPortSelect & BIT(0)) ? nandCache : sdCache;
        cache.write(dst, hc ? (int64_t(curAddress) << 9) : curAddress, data, len << 2);
    }

    // Change state and trigger a DRQ or end interrupt based on blocks left
    cardStatus = (cardStatus & ~0x1E00) | ((curBlock ? 0x6 : 0x4) << 9);
    curAddress += hc ? 1 : (len << 2);
    if (!curBlock) return sendInterrupt(2);
    core->ndma[1].setDrq(drqType());
}

void SdMmc::runCommand() {
    // Commands for the Wi-Fi chip are its own
    if (sdio) return runSdioCommand();

    // Nothing answers in an empty SD slot, so the command times out
    if (noCard()) return sendInterrupt(22);

    // Ensure the command is being run in normal mode
    if (cardStatus & BIT(5)) {
        LOG_WARN("Tried to run %s CMD in app mode\n", name());
        cardStatus &= ~BIT(5);
        return;
    }

    // Execute a normal SD/MMC command
    switch (uint8_t cmd = sdCmd & 0x3F) {
        case 2: return getCid(); // ALL_GET_CID (stub)
        case 8: return setIfCond(); // SET_IF_COND
        case 9: return getCsd(); // GET_CSD
        case 10: return getCid(); // GET_CID
        case 13: return getStatus(); // GET_STATUS
        case 16: return setBlocklen(); // SET_BLOCKLEN
        case 17: return readSingleBlock(); // READ_SINGLE_BLOCK
        case 18: return readMultiBlock(); // READ_MULTIPLE_BLOCK
        case 24: return writeSingleBlock(); // WRITE_SINGLE_BLOCK
        case 25: return writeMultiBlock(); // WRITE_MULTIPLE_BLOCK
        case 55: return appCmd(); // APP_CMD

    case 1: // SEND_OP_COND
    case 3: // SET_RELATIVE_ADDR
        // Stub commands that want response bit 31 to be set
        LOG_WARN("Stubbed %s command: CMD%d\n", name(), cmd);
        return pushResponse(0x80000000);

    default:
        // Assume response R1 for unknown commands
        LOG_WARN("Unknown %s command: CMD%d\n", name(), cmd);
        return pushResponse(cardStatus);
    }
}

void SdMmc::runAppCommand() {
    // Nothing answers in an empty SD slot, so the command times out
    if (noCard()) return sendInterrupt(22);

    // Ensure the command is being run in app mode
    if (~cardStatus & BIT(5)) {
        LOG_WARN("Tried to run %s ACMD in normal mode\n", name());
        return;
    }

    // Return to normal mode for the next command
    cardStatus &= ~BIT(5);

    // Execute an app SD/MMC command
    switch (uint8_t cmd = sdCmd & 0x3F) {
        case 6: return setBusWidth(); // SET_BUS_WIDTH
        case 13: return sdStatus(); // SD_STATUS
        case 41: return sdSendOpCond(); // SD_SEND_OP_COND
        case 51: return getScr(); // GET_SCR

    default:
        // Assume response R1 for unknown commands
        LOG_WARN("Unknown %s command: ACMD%d\n", name(), cmd);
        return pushResponse(cardStatus);
    }
}

void SdMmc::runSdioCommand() {
    // Execute an SDIO command for the Wi-Fi chip, with the response of a single function card
    switch (uint8_t cmd = sdCmd & 0x3F) {
        case 3: return pushResponse(0); // SEND_RELATIVE_ADDR
        case 5: return pushResponse(0x80FFFF00); // IO_SEND_OP_COND
        case 7: return pushResponse(0); // SELECT_CARD
        case 52: return pushResponse(core->dsiWifi.ioDirect(sdCmdParam) | 0x1000); // IO_RW_DIRECT
        case 53: return ioExtended(); // IO_RW_EXTENDED

    default:
        LOG_WARN("Unknown SDIO command: CMD%d\n", cmd);
        return pushResponse(0);
    }
}

void SdMmc::ioExtended() {
    // Start a transfer of bytes or blocks to or from the Wi-Fi chip, which goes through the FIFOs like SD blocks
    core->dsiWifi.startTransfer(sdCmdParam);
    curBlock = (sdCmdParam & BIT(27)) ? sdData16Blkcnt : 1;
    pushResponse(0x1000);
    (sdCmdParam & BIT(31)) ? startWrite() : scheduleRead();
}

void SdMmc::setIfCond() {
    // Respond with the same voltage and check pattern that was input
    pushResponse(sdCmdParam & 0xFFF);
}

void SdMmc::getCsd() {
    // Build the 128-bit CSD register, which gives the capacity, block sizes and speeds, in the response registers' layout:
    // the CRC byte is left off, so bits 127-8 fill bits 119-0 (as the CID from the NAND footer is stored)
    uint8_t csd[16] = {};
    auto set = [&](int hi, int lo, uint32_t value) {
        for (int i = lo; i <= hi; i++) {
            uint8_t &byte = csd[(i - 8) >> 3];
            byte = (byte & ~BIT((i - 8) & 0x7)) | (((value >> (i - lo)) & 0x1) << ((i - 8) & 0x7));
        }
    };

    if (sdPortSelect & BIT(0)) {
        // Copy the CSD of a real DSi's eMMC (as in melonDS), whose 240MB is 1920 units of 128KB, sizing it to the NAND
        static const uint8_t mmc[16] = { 0x40, 0x40, 0x96, 0xE9, 0x7F, 0xDB, 0xF6, 0xDF,
            0x01, 0x59, 0x0F, 0x2A, 0x01, 0x26, 0x90, 0x00 };
        memcpy(csd, mmc, sizeof(csd));
        set(73, 62, std::min<int64_t>(0x1000, std::max<int64_t>(1, nandSize >> 17)) - 1); // C_SIZE
    }
    else if (sdhc) {
        // Describe an SDHC card with CSD version 2.0, sized in units of 512KB
        set(127, 126, 1); // CSD_STRUCTURE
        set(119, 112, 0x0E); // TAAC
        set(103, 96, 0x32); // TRAN_SPEED: 25MHz
        set(95, 84, 0x5B5); // CCC
        set(83, 80, 9); // READ_BL_LEN: 512 bytes
        set(69, 48, std::min<int64_t>(0x400000, sdSize >> 19) - 1); // C_SIZE
        set(46, 46, 1); // ERASE_BLK_EN
        set(45, 39, 0x7F); // SECTOR_SIZE
        set(28, 26, 2); // R2W_FACTOR
        set(25, 22, 9); // WRITE_BL_LEN: 512 bytes
    }
    else {
        // Describe a standard-capacity card with CSD version 1.0, sized in units of 256KB, up to 1GB
        set(119, 112, 0x26); // TAAC
        set(103, 96, 0x32); // TRAN_SPEED: 25MHz
        set(95, 84, 0x5B5); // CCC
        set(83, 80, 9); // READ_BL_LEN: 512 bytes
        set(79, 79, 1); // READ_BL_PARTIAL
        set(73, 62, std::min<int64_t>(0x1000, std::max<int64_t>(1, sdSize >> 18)) - 1); // C_SIZE
        set(61, 50, 0xFF6); // VDD_R_CURR_MIN/MAX, VDD_W_CURR_MIN/MAX
        set(49, 47, 7); // C_SIZE_MULT: 512
        set(46, 46, 1); // ERASE_BLK_EN
        set(45, 39, 0x7F); // SECTOR_SIZE
        set(28, 26, 2); // R2W_FACTOR
        set(25, 22, 9); // WRITE_BL_LEN: 512 bytes
    }

    // Send the register as a long response, the highest word last
    for (int i = 3; i >= 0; i--)
        pushResponse(U8TO32(csd, i * 4));
}

void SdMmc::getCid() {
    // Get the card ID value as a response
    for (int i = 0; i < 4; i++)
        pushResponse(mmcCid[3 - i]);
}

void SdMmc::getStatus() {
    // Get the status register without doing anything else
    pushResponse(cardStatus);
}

void SdMmc::setBlocklen() {
    // Set the word length for multi-block reads and writes
    blockLen = std::min(0x200U, sdCmdParam + 3) >> 2;
    pushResponse(cardStatus);
}

void SdMmc::readSingleBlock() {
    // Read a single block of data
    curAddress = sdCmdParam;
    curBlock = 1;
    pushResponse(cardStatus);
    scheduleRead();
}

void SdMmc::readMultiBlock() {
    // Read the first of multiple blocks of data
    curAddress = sdCmdParam;
    curBlock = sdData16Blkcnt;
    pushResponse(cardStatus);
    scheduleRead();
}

void SdMmc::writeSingleBlock() {
    // Prepare to write a single block of data
    curAddress = sdCmdParam;
    curBlock = 1;
    pushResponse(cardStatus);
    startWrite();
}

void SdMmc::writeMultiBlock() {
    // Prepare to write multiple blocks of data
    curAddress = sdCmdParam;
    curBlock = sdData16Blkcnt;
    pushResponse(cardStatus);
    startWrite();
}

void SdMmc::appCmd() {
    // Switch to app command mode
    cardStatus |= BIT(5);
    pushResponse(cardStatus);
}

void SdMmc::setBusWidth() {
    // Remember the bus width, which only shows in the SD status register
    busWidth = sdCmdParam & 0x3;
    pushResponse(cardStatus);
}

void SdMmc::sdStatus() {
    // Send the 512-bit SD status register: the bus width, speed class 4 and a 4MB allocation unit
    uint8_t status[64] = {};
    status[0] = busWidth << 6; // DAT_BUS_WIDTH
    status[8] = 0x02; // SPEED_CLASS
    status[10] = 0x90; // AU_SIZE
    pushResponse(cardStatus);
    pushBytes(status, sizeof(status));
}

void SdMmc::sdSendOpCond() {
    // Set the SD voltage window and return it along with high-capacity status
    opCond = (opCond & ~0xFFFFFF) | (sdCmdParam & 0xFFFFFF);
    pushResponse(opCond | ((~sdPortSelect & sdhc) << 30));
}

void SdMmc::getScr() {
    // Send the 64-bit SD configuration register: SD version 2.00, 1- and 4-bit buses, and the security version that goes
    // with the card's capacity
    uint8_t scr[8] = { 0x02, uint8_t((~sdPortSelect & sdhc) ? 0x35 : 0x25) };
    pushResponse(cardStatus);
    pushBytes(scr, sizeof(scr));
}

uint16_t SdMmc::readData16Fifo() {
    // Pop a value from the 16-bit FIFO and check if empty
    if (dataFifo16.empty()) return sdData16Fifo;
    sdData16Fifo = dataFifo16.front();
    dataFifo16.pop_front();
    if (!dataFifo16.empty()) return sdData16Fifo;

    // Trigger a 16-bit FIFO empty interrupt and read another block or finish with a data end interrupt
    sendInterrupt(25);
    core->ndma[1].clearDrq(drqType());
    (cardStatus & 0x1E00) == (0x5 << 9) ? scheduleRead() : sendInterrupt(2);
    return sdData16Fifo;
}

uint32_t SdMmc::readData32Fifo() {
    // Pop a value from the 32-bit read FIFO and check if empty
    if (dataFifo32.empty()) return sdData32Fifo;
    sdData32Fifo = dataFifo32.front();
    dataFifo32.pop_front();
    sdData32Irq &= ~BIT(8); // Not full
    if (!dataFifo32.empty()) return sdData32Fifo;

    // Trigger a 32-bit FIFO empty interrupt if enabled
    sdData32Irq &= ~BIT(9);
    if (sdData32Irq & BIT(12))
        core->interpreter[1].sendInterrupt(irqBit());
    core->ndma[1].clearDrq(drqType());

    // Read another block or finish with a data end interrupt
    (cardStatus & 0x1E00) == (0x5 << 9) ? scheduleRead() : sendInterrupt(2);
    return sdData32Fifo;
}

void SdMmc::writeCmd(uint16_t mask, uint16_t value) {
    // Write to the SD_CMD register
    // TODO: handle response type and other bits
    sdCmd = (sdCmd & ~mask) | (value & mask);

    // Run the written command and send an interrupt for completion, unless there's no card to answer
    if (!noCard()) sendInterrupt(0);
    switch (uint8_t type = (sdCmd >> 6) & 0x3) {
        case 0: return runCommand();
        case 1: return runAppCommand();

    default:
        LOG_WARN("Unknown %s command type: %d\n", name(), type);
        return;
    }
}

void SdMmc::writePortSelect(uint16_t mask, uint16_t value) {
    // Write to the SD_PORT_SELECT register
    mask &= 0xF;
    sdPortSelect = (sdPortSelect & ~mask) | (value & mask);
}

void SdMmc::writeCmdParam(uint32_t mask, uint32_t value) {
    // Write to the SD_CMD_PARAM parameter
    sdCmdParam = (sdCmdParam & ~mask) | (value & mask);
}

void SdMmc::writeData16Blkcnt(uint16_t mask, uint16_t value) {
    // Write to the SD_DATA16_BLKCNT block count
    sdData16Blkcnt = (sdData16Blkcnt & ~mask) | (value & mask);
}

void SdMmc::writeIrqStatus(uint32_t mask, uint32_t value) {
    // Acknowledge bits in the SD_IRQ_STATUS register
    mask &= ~0xA0; // Always set
    sdIrqStatus = (sdIrqStatus & ~mask) | (sdIrqStatus & value & mask);
}

void SdMmc::writeIrqMask(uint32_t mask, uint32_t value) {
    // Write to the SD_IRQ_MASK register
    mask &= 0x8B7F031D;
    uint32_t old = sdIrqMask;
    sdIrqMask = (sdIrqMask & ~mask) | (value & mask);

    // Trigger an ARM9 interrupt if a requested bit was newly unmasked
    if (sdIrqStatus & old & ~sdIrqMask)
        core->interpreter[1].sendInterrupt(irqBit());
}

void SdMmc::writeData16Blklen(uint16_t mask, uint16_t value) {
    // Write to the SD_DATA16_BLKLEN block length, clipping past 0x200
    mask &= 0x3FF;
    sdData16Blklen = std::min(0x200, (sdData16Blklen & ~mask) | (value & mask));
}

void SdMmc::writeCardOption(uint16_t mask, uint16_t value) {
    // Write to the SD_CARD_OPTION register, restarting a card detect in progress with the new detect time
    mask &= 0xC0FF;
    sdCardOption = (sdCardOption & ~mask) | (value & mask);
    if (detecting) startDetect();
}

void SdMmc::writeData16Fifo(uint16_t mask, uint16_t value) {
    // Push a value to the 16-bit FIFO and check if full
    uint32_t size = (dataFifo16.size() << 1);
    if (size >= 0x200) return;
    dataFifo16.push_back(value & mask);
    if (size + 2 < sdData16Blklen) return;

    // Trigger a 16-bit FIFO full interrupt and write a block if in write mode
    sendInterrupt(24);
    core->ndma[1].clearDrq(drqType());
    if ((cardStatus & 0x1E00) == (0x6 << 9))
        scheduleWrite();
}

void SdMmc::writeDataCtl(uint16_t mask, uint16_t value) {
    // Write to the SD_DATA_CTL register
    mask &= 0x22;
    sdDataCtl = (sdDataCtl & ~mask) | (value & mask);
}

void SdMmc::writeData32Irq(uint16_t mask, uint16_t value) {
    // Empty the 32-bit FIFO if requested
    if (value & mask & BIT(10)) {
        sdData32Irq &= ~(BIT(8) | BIT(9)); // Not full, empty
        dataFifo32 = {};
    }

    // Write to the SD_DATA32_IRQ register
    mask &= 0x1802;
    sdData32Irq = (sdData32Irq & ~mask) | (value & mask);
}

void SdMmc::writeData32Blklen(uint16_t mask, uint16_t value) {
    // Write to the SD_DATA32_BLKLEN block length
    mask &= 0x3FF;
    sdData32Blklen = (sdData32Blklen & ~mask) | (value & mask);
}

void SdMmc::writeData32Fifo(uint32_t mask, uint32_t value) {
    // Push a value to the 32-bit FIFO and check if full
    uint32_t size = (dataFifo32.size() << 2);
    if (size >= 0x200) return;
    dataFifo32.push_back(value & mask);
    sdData32Irq |= BIT(9); // Not empty
    if (size + 4 < sdData32Blklen) return;

    // Trigger a 32-bit FIFO full interrupt if enabled
    sdData32Irq |= BIT(8);
    if (sdData32Irq & BIT(11))
        core->interpreter[1].sendInterrupt(irqBit());
    core->ndma[1].clearDrq(drqType());

    // Write a block if in write mode
    if ((cardStatus & 0x1E00) == (0x6 << 9))
        scheduleWrite();
}

void SdMmc::writeCardIrqCtl(uint16_t mask, uint16_t value) {
    // Write to the SD_CARD_IRQ_CTL register, which enables taking interrupts from the card
    mask &= 0x0305;
    sdCardIrqCtl = (sdCardIrqCtl & ~mask) | (value & mask);
    updateCardIrq();
}

void SdMmc::writeCardIrqStatus(uint16_t mask, uint16_t value) {
    // Acknowledge bits in the SD_CARD_IRQ_STATUS register
    sdCardIrqStatus &= (value | ~mask);
}

void SdMmc::writeCardIrqMask(uint16_t mask, uint16_t value) {
    // Write to the SD_CARD_IRQ_MASK register, and interrupt if a pending bit was newly unmasked
    mask &= 0xC007;
    uint16_t old = sdCardIrqStatus & ~sdCardIrqMask;
    sdCardIrqMask = (sdCardIrqMask & ~mask) | (value & mask);
    if (!old && (sdCardIrqStatus & ~sdCardIrqMask)) {
        core->interpreter[1].sendInterrupt(irqBit());
        core->interpreter[1].sendInterrupt(irqBit() + 1);
    }
}

void SdMmc::updateCardIrq() {
    // Follow the Wi-Fi chip's interrupt line while card interrupts are enabled, interrupting on a new unmasked one
    if (!sdio || (~sdCardIrqCtl & BIT(0))) return;
    uint16_t old = sdCardIrqStatus & ~sdCardIrqMask;
    sdCardIrqStatus = (sdCardIrqStatus & ~BIT(0)) | core->dsiWifi.readIrq();
    if (!old && (sdCardIrqStatus & ~sdCardIrqMask)) {
        core->interpreter[1].sendInterrupt(irqBit());
        core->interpreter[1].sendInterrupt(irqBit() + 1);
    }
}

void SdMmc::writeSoftReset(uint16_t mask, uint16_t value) {
    // Write to the SD_SOFT_RESET register, where clearing bit 0 resets the card; the system does this to start the
    // Wi-Fi chip over, like after using the DS Wi-Fi hardware
    mask &= 0x0001;
    bool reset = (sdSoftReset & ~value & mask & BIT(0));
    bool release = (~sdSoftReset & value & mask & BIT(0));
    sdSoftReset = (sdSoftReset & ~mask) | (value & mask);
    if (reset && sdio) {
        core->dsiWifi.reset();
        updateCardIrq();
    }

    // Reset acknowledges all interrupts and resets the detect time
    if (!sdio && reset) {
        sdIrqStatus &= 0xA0;
        sdCardOption = 0x40EE;
        detecting = false;
    }

    // Detect the SD card again when coming out of reset, after its detect time
    if (!sdio && sd) {
        if (release) {
            detecting = true;
            startDetect();
        }
    }
}
