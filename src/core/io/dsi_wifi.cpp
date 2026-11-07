/*
    Copyright 2026 michyprima
    Based on melonDS's DSi_NWifi, Copyright 2016-2026 melonDS team

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
#include "wifi_ap.h"

// The card information structures of the two SDIO functions; the rest of each is zero
static const uint8_t cisData0[] = {
    0x01, 0x03, 0xD9, 0x01, 0xFF, 0x20, 0x04, 0x71, 0x02, 0x00, 0x02, 0x21, 0x02, 0x0C, 0x00, 0x22, 0x04, 0x00, 0x00,
    0x08, 0x32, 0x1A, 0x05, 0x01, 0x01, 0x00, 0x02, 0x07, 0x1B, 0x08, 0xC1, 0x41, 0x30, 0x30, 0xFF, 0xFF, 0x32, 0x00,
    0x14, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static const uint8_t cisData1[] = {
    0x20, 0x04, 0x71, 0x02, 0x00, 0x02, 0x21, 0x02, 0x0C, 0x00, 0x22, 0x2A, 0x01, 0x01, 0x11, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0A, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x80, 0x01, 0x06,
    0x81, 0x01, 0x07, 0x82, 0x01, 0xDF, 0xFF, 0x01
};

// How much mailbox 4 holds; events wait in mailbox 8 for room, and packets aren't received while it's nearly full
static const uint32_t MAILBOX_SIZE = 0x600;
static const uint32_t EVENT_SIZE = 0x8000;

void DsiWifi::saveState(FILE *file) {
    // Write DSi state data to the file
    if (!core->dsiMode) return;
    for (int i = 0; i < 9; i++)
        SaveStates::writeFifo(mailbox[i], file);
    fwrite(&transferParam, sizeof(transferParam), 1, file);
    fwrite(&transferAddr, sizeof(transferAddr), 1, file);
    fwrite(&f0IrqEnable, sizeof(f0IrqEnable), 1, file);
    fwrite(&f0IrqStatus, sizeof(f0IrqStatus), 1, file);
    fwrite(f1IrqEnable, sizeof(f1IrqEnable), 1, file);
    fwrite(f1IrqStatus, sizeof(f1IrqStatus), 1, file);
    fwrite(&irq, sizeof(irq), 1, file);
    fwrite(&windowData, sizeof(windowData), 1, file);
    fwrite(&windowReadAddr, sizeof(windowReadAddr), 1, file);
    fwrite(&windowWriteAddr, sizeof(windowWriteAddr), 1, file);
    fwrite(&bootPhase, sizeof(bootPhase), 1, file);
    fwrite(&firmwareLoaded, sizeof(firmwareLoaded), 1, file);
    fwrite(&scanMs, sizeof(scanMs), 1, file);
    fwrite(&reportAp, sizeof(reportAp), 1, file);
    fwrite(&connected, sizeof(connected), 1, file);
}

void DsiWifi::loadState(FILE *file) {
    // Read DSi state data from the file
    if (!core->dsiMode) return;
    for (int i = 0; i < 9; i++)
        SaveStates::readFifo(mailbox[i], file);
    fread(&transferParam, sizeof(transferParam), 1, file);
    fread(&transferAddr, sizeof(transferAddr), 1, file);
    fread(&f0IrqEnable, sizeof(f0IrqEnable), 1, file);
    fread(&f0IrqStatus, sizeof(f0IrqStatus), 1, file);
    fread(f1IrqEnable, sizeof(f1IrqEnable), 1, file);
    fread(f1IrqStatus, sizeof(f1IrqStatus), 1, file);
    fread(&irq, sizeof(irq), 1, file);
    fread(&windowData, sizeof(windowData), 1, file);
    fread(&windowReadAddr, sizeof(windowReadAddr), 1, file);
    fread(&windowWriteAddr, sizeof(windowWriteAddr), 1, file);
    fread(&bootPhase, sizeof(bootPhase), 1, file);
    fread(&firmwareLoaded, sizeof(firmwareLoaded), 1, file);
    fread(&scanMs, sizeof(scanMs), 1, file);
    fread(&reportAp, sizeof(reportAp), 1, file);
    fread(&connected, sizeof(connected), 1, file);
}

void DsiWifi::reset() {
    // Start with empty mailboxes and nothing pending
    for (int i = 0; i < 9; i++)
        mailbox[i].clear();
    transferParam = transferAddr = 0;
    f0IrqEnable = f0IrqStatus = 0;
    memset(f1IrqEnable, 0, sizeof(f1IrqEnable));
    memset(f1IrqStatus, 0, sizeof(f1IrqStatus));
    irq = false;
    windowData = windowReadAddr = windowWriteAddr = 0;
    bootPhase = 0;
    scanMs = 0;
    reportAp = true;
    connected = false;

    // Identify the chip from the firmware's Wi-Fi board type, and get the MAC address
    size_t size;
    const uint8_t *firmware = core->spi.getFirmware(size);
    uint8_t board = (firmware && size > 0x1FD) ? firmware[0x1FD] : 1;
    switch (board) {
    case 2: // W024, AR6013
        romId = 0x23000024;
        chipId = 0x0D000000;
        hostIntAddr = 0x00520000;
        break;

    case 3: // W028, AR6014 in a 3DS
        romId = 0x2300006F;
        chipId = 0x0D000001;
        hostIntAddr = 0x00520000;
        break;

    default: // W015, AR6002
        romId = 0x20000188;
        chipId = 0x02000001;
        hostIntAddr = 0x00500400;
        break;
    }
    memset(cis0, 0, sizeof(cis0));
    memset(cis1, 0, sizeof(cis1));
    memcpy(cis0, cisData0, sizeof(cisData0));
    memcpy(cis1, cisData1, sizeof(cisData1));
    cis0[9] = cis1[4] = (chipId >= 0x0D000000);

    // Build the EEPROM contents the system reads, with the MAC address and a checksum
    memset(eeprom, 0, sizeof(eeprom));
    eeprom[0x1] = 0x03; // Size 0x300
    eeprom[0x8] = 0x48; // Country code
    eeprom[0x9] = 0x83;
    for (int i = 0; i < 6; i++)
        eeprom[0xA + i] = (firmware && size > 0x3B) ? firmware[0x36 + i] : 0;
    eeprom[0x13] = 0x60;
    memset(&eeprom[0x3C], 0xFF, 0x70);
    memset(&eeprom[0x140], 0xFF, 0x8);
    uint16_t check = 0xFFFF;
    for (int i = 0; i < 0x300; i += 2)
        check ^= U8TO16(eeprom, i);
    eeprom[0x4] = check;
    eeprom[0x5] = check >> 8;
}

void DsiWifi::updateIrq() {
    // Raise the card interrupt for function 1 when enabled, and let the controller know
    f0IrqStatus = (f1IrqStatus[0] & f1IrqEnable[0]) ? BIT(1) : 0;
    irq = (f0IrqEnable & BIT(0)) && (f0IrqStatus & f0IrqEnable);
    core->sdio.updateCardIrq();
}

void DsiWifi::updateF1Irq() {
    // Summarize function 1's interrupt sources: data in mailboxes 4-7, then the counter, CPU and error ones
    uint8_t status = 0;
    for (int i = 0; i < 4; i++)
        if (!mailbox[4 + i].empty()) status |= BIT(i);
    if (f1IrqStatus[3] & f1IrqEnable[3]) status |= BIT(4);
    if (f1IrqStatus[1] & f1IrqEnable[1]) status |= BIT(6);
    if (f1IrqStatus[2] & f1IrqEnable[2]) status |= BIT(7);
    f1IrqStatus[0] = status;
    updateIrq();
}

uint8_t DsiWifi::ioDirect(uint32_t param) {
    // Read or write a byte for CMD52, reading it back after writing if asked
    uint32_t func = (param >> 28) & 0x7;
    uint32_t addr = (param >> 9) & 0x1FFFF;
    if (param & BIT(31)) {
        uint8_t value = param;
        func ? writeF1(addr, value) : writeF0(addr, value);
        if (~param & BIT(27)) return value;
    }
    return func ? readF1(addr) : readF0(addr);
}

void DsiWifi::startTransfer(uint32_t param) {
    // Remember a CMD53 transfer's function and address for the data that follows
    transferParam = param;
    transferAddr = (param >> 9) & 0x1FFFF;
}

void DsiWifi::readData(uint8_t *data, uint32_t size) {
    // Read bytes for a CMD53 transfer, stepping the address if asked
    bool func = (transferParam >> 28) & 0x7;
    for (uint32_t i = 0; i < size; i++) {
        data[i] = func ? readF1(transferAddr) : readF0(transferAddr);
        if (transferParam & BIT(26)) transferAddr = (transferAddr + 1) & 0x1FFFF;
    }
}

void DsiWifi::writeData(const uint8_t *data, uint32_t size) {
    // Write bytes for a CMD53 transfer, stepping the address if asked
    bool func = (transferParam >> 28) & 0x7;
    for (uint32_t i = 0; i < size; i++) {
        func ? writeF1(transferAddr, data[i]) : writeF0(transferAddr, data[i]);
        if (transferParam & BIT(26)) transferAddr = (transferAddr + 1) & 0x1FFFF;
    }
}

uint8_t DsiWifi::readF0(uint32_t addr) {
    // Read from function 0, the SDIO card's common registers and card information
    switch (addr) {
        case 0x00000: return 0x11; // SDIO revision
        case 0x00002: return 0x02; // I/O enable
        case 0x00003: return 0x02; // I/O ready
        case 0x00004: return f0IrqEnable;
        case 0x00005: return f0IrqStatus;
        case 0x00008: return 0x17; // Card capability
        case 0x0000A: return 0x10; // Card information pointer
        case 0x00012: return 0x03; // Power control
        case 0x0010A: return 0x11; // Function 1 card information pointer
    }
    if (addr >= 0x1000 && addr < 0x1100) return cis0[addr & 0xFF];
    if (addr >= 0x1100 && addr < 0x1200) return cis1[addr & 0xFF];
    return 0;
}

void DsiWifi::writeF0(uint32_t addr, uint8_t value) {
    // Write to function 0, of which only the interrupt enable matters
    if (addr == 0x00004) {
        f0IrqEnable = value;
        updateIrq();
        return;
    }
    LOG_WARN("Unknown DSi Wi-Fi function 0 write: 0x%X = 0x%X\n", addr, value);
}

uint8_t DsiWifi::readMailbox(int i, bool last) {
    // Read a byte from a mailbox, passing on waiting events after the last byte of a read from mailbox 4
    uint8_t value = 0;
    if (!mailbox[i].empty()) {
        value = mailbox[i].front();
        mailbox[i].pop_front();
    }
    if (i == 4 && last) drainEvents();
    updateF1Irq();
    return value;
}

void DsiWifi::writeMailbox(int i, uint8_t value, bool last) {
    // Write a byte to a mailbox, handling a command after the last byte written to mailbox 0
    mailbox[i].push_back(value);
    if (i == 0 && last) handleCommand();
    updateF1Irq();
}

uint8_t DsiWifi::readF1(uint32_t addr) {
    // Read from function 1: mailboxes, with registers in between and the last bytes of each area marking the end
    if (addr < 0x400) return readMailbox(4 + (addr >> 8), addr == 0xFF);
    if (addr >= 0x800 && addr < 0x2800) {
        int i = (addr < 0x1000) ? 0 : ((addr - 0x800) >> 11);
        return readMailbox(4 + i, addr == 0xFFF);
    }
    if (addr >= 0x2800) return readMailbox(4, addr == 0x3FFF);

    switch (addr) {
    case 0x400: case 0x401: case 0x402: case 0x403:
        return f1IrqStatus[addr & 0x3];

    case 0x405: {
        // Which mailboxes have a word to read
        uint8_t value = 0;
        for (int i = 0; i < 4; i++)
            if (mailbox[4 + i].size() >= 4) value |= BIT(i);
        return value;
    }

    case 0x408: case 0x409: case 0x40A: case 0x40B:
        // Look ahead at mailbox 4 without reading it
        return (mailbox[4].size() > (addr & 0x3)) ? mailbox[4][addr & 0x3] : 0;

    case 0x418: case 0x419: case 0x41A: case 0x41B:
        return f1IrqEnable[addr & 0x3];

    case 0x440:
        // Reading the first counter decrements it, which here just clears its interrupt
        f1IrqStatus[3] &= ~BIT(0);
        updateF1Irq();
        return 0;

    case 0x450: return 1;
    case 0x474: case 0x475: case 0x476: case 0x477:
        return windowData >> ((addr & 0x3) * 8);
    }
    return 0;
}

void DsiWifi::writeF1(uint32_t addr, uint8_t value) {
    // Write to function 1, the same layout as reading but with mailboxes 0-3
    if (addr < 0x400) return writeMailbox(addr >> 8, value, addr == 0xFF);
    if (addr >= 0x800 && addr < 0x2800) {
        int i = (addr < 0x1000) ? 0 : ((addr - 0x800) >> 11);
        return writeMailbox(i, value, addr == 0xFFF);
    }
    if (addr >= 0x2800) return writeMailbox(0, value, addr == 0x3FFF);

    switch (addr) {
    case 0x418: case 0x419: case 0x41A: case 0x41B:
        f1IrqEnable[addr & 0x3] = value;
        updateF1Irq();
        return;

    case 0x440:
        f1IrqStatus[3] &= ~BIT(0);
        updateF1Irq();
        return;

    case 0x474: case 0x475: case 0x476: case 0x477: {
        uint32_t shift = (addr & 0x3) * 8;
        windowData = (windowData & ~(0xFF << shift)) | (value << shift);
        return;
    }

    case 0x479: case 0x47A: case 0x47B: {
        // The low byte of the address goes last and starts the access
        uint32_t shift = (addr & 0x3) * 8;
        windowWriteAddr = (windowWriteAddr & ~(0xFF << shift)) | (value << shift);
        return;
    }

    case 0x47D: case 0x47E: case 0x47F: {
        uint32_t shift = (addr & 0x3) * 8;
        windowReadAddr = (windowReadAddr & ~(0xFF << shift)) | (value << shift);
        return;
    }

    case 0x478:
        // Writes to the chip's own memory and registers do nothing
        windowWriteAddr = (windowWriteAddr & ~0xFF) | value;
        return;

    case 0x47C:
        windowReadAddr = (windowReadAddr & ~0xFF) | value;
        windowData = readWindow(windowReadAddr);
        return;
    }
    LOG_WARN("Unknown DSi Wi-Fi function 1 write: 0x%X = 0x%X\n", addr, value);
}

uint16_t DsiWifi::read16(int i) {
    uint16_t value = readMailbox(i, false);
    return value | (readMailbox(i, false) << 8);
}

uint32_t DsiWifi::read32(int i) {
    uint32_t value = read16(i);
    return value | (read16(i) << 16);
}

void DsiWifi::write16(int i, uint16_t value) {
    mailbox[i].push_back(value);
    mailbox[i].push_back(value >> 8);
}

void DsiWifi::write32(int i, uint32_t value) {
    write16(i, value);
    write16(i, value >> 16);
}

void DsiWifi::drain(int i) {
    // Drop whatever is left of a command
    mailbox[i].clear();
}

void DsiWifi::drainEvents() {
    // Move whole events from mailbox 8 to mailbox 4 while they fit, each padded to 0x80 bytes
    while (mailbox[8].size() >= 6) {
        uint32_t size = (mailbox[8][2] | (mailbox[8][3] << 8)) + 6;
        uint32_t padded = (size + 0x7F) & ~0x7F;
        if (mailbox[4].size() + padded > MAILBOX_SIZE) break;
        for (uint32_t i = 0; i < size; i++) {
            mailbox[4].push_back(mailbox[8].front());
            mailbox[8].pop_front();
        }
        mailbox[4].insert(mailbox[4].end(), padded - size, 0);
    }
    updateF1Irq();
}

uint32_t DsiWifi::readWindow(uint32_t addr) {
    // Read from the chip's memory or registers through the window
    if ((addr & 0xFFFF00) == hostIntAddr) {
        // The host interest area, which says where the EEPROM data is and whether the firmware was loaded
        switch (addr & 0xFF) {
            case 0x54: return 0x1FFC00;
            case 0x58: return firmwareLoaded;
        }
        return 0;
    }
    if ((addr & 0x1FFC00) == 0x1FFC00) return U8TO32(eeprom, addr & 0x3FC);
    switch (addr) {
        case 0x40EC: return chipId;
        case 0x40C0: return 2; // Reset cause
    }
    return 0;
}

void DsiWifi::handleCommand() {
    // Handle a command written to mailbox 0 in the protocol of the current boot phase
    switch (bootPhase) {
        case 0: return bmiCommand();
        case 1: return htcCommand();
        default: return wmiCommand();
    }
}

void DsiWifi::bmiCommand() {
    // Handle a boot loader command, which uploads and starts the firmware; nothing runs it here
    switch (uint32_t cmd = read32(0)) {
    case 0x01: { // BMI_DONE
        static const uint8_t ready[] = { 0x0A, 0x00, 0x08, 0x06, 0x16, 0x00 };
        firmwareLoaded = true;
        sendEvent(0, 0x0001, ready, sizeof(ready));
        bootPhase = 1;
        return;
    }

    case 0x03: { // BMI_WRITE_MEMORY
        read32(0);
        uint32_t size = read32(0);
        for (uint32_t i = 0; i < size; i++) readMailbox(0, false);
        return;
    }

    case 0x04: // BMI_EXECUTE
        read32(0);
        read32(0);
        write32(4, 0);
        updateF1Irq();
        return;

    case 0x06: // BMI_READ_SOC_REGISTER
        write32(4, readWindow(read32(0)));
        updateF1Irq();
        return;

    case 0x07: // BMI_WRITE_SOC_REGISTER
        read32(0);
        read32(0);
        return;

    case 0x08: // BMI_GET_TARGET_ID
        write32(4, 0xFFFFFFFF);
        write32(4, 0x0000000C);
        write32(4, romId);
        write32(4, 0x00000002);
        updateF1Irq();
        return;

    case 0x0D: // BMI_LZ_STREAM_START
        read32(0);
        return;

    case 0x0E: { // BMI_LZ_DATA
        uint32_t size = read32(0);
        for (uint32_t i = 0; i < size; i++) readMailbox(0, false);
        return;
    }

    default:
        LOG_WARN("Unknown DSi Wi-Fi BMI command: 0x%X\n", cmd);
        drain(0);
        return;
    }
}

void DsiWifi::htcCommand() {
    // Handle a host target communication command, which sets up the services before WMI
    read16(0);
    uint16_t size = read16(0);
    read16(0);
    switch (uint16_t cmd = read16(0)) {
    case 0x0002: { // Service connect
        // Reply with the service, its endpoint, and the largest message size, 0x602 for the first service or 0x600
        uint16_t service = read16(0);
        uint8_t reply[8] = { uint8_t(service), uint8_t(service >> 8), 0, uint8_t((service & 0xFF) + 1),
            uint8_t((service == 0x0100) ? 0x02 : 0x00), 0x06, 0, 0 };
        sendEvent(0, 0x0003, reply, sizeof(reply));
        break;
    }

    case 0x0004: { // Setup complete
        // Report being ready with the MAC address, then the regulatory domain from the EEPROM
        uint8_t ready[12] = {};
        memcpy(ready, &eeprom[0xA], 6);
        ready[6] = 0x02;
        ready[8] = 0x6C;
        ready[11] = 0x23;
        sendEvent(1, 0x1001, ready, sizeof(ready));
        uint32_t domain = 0x80000000 | (U8TO16(eeprom, 0x8) & 0xFFF);
        uint8_t region[4] = { uint8_t(domain), uint8_t(domain >> 8), uint8_t(domain >> 16), uint8_t(domain >> 24) };
        sendEvent(1, 0x1006, region, sizeof(region));
        bootPhase = 2;
        break;
    }

    default:
        LOG_WARN("Unknown DSi Wi-Fi HTC command: 0x%X, size 0x%X\n", cmd, size);
        break;
    }
    drain(0);
}

void DsiWifi::wmiCommand() {
    // Handle a wireless module interface command, or a packet on one of the data endpoints
    uint16_t header = read16(0);
    uint16_t size = read16(0);
    read16(0);
    uint8_t endpoint = header;
    if (endpoint > 1) {
        sendPacket(size);
    }
    else {
        switch (uint16_t cmd = read16(0)) {
        case 0x0001: // Connect
            connect();
            break;

        case 0x0003: { // Disconnect
            uint8_t reply[11] = { 0x03 };
            memcpy(&reply[2], WifiAp::mac, 6);
            reply[8] = 3; // Reason: requested
            connected = false;
            sendEvent(1, 0x1003, reply, sizeof(reply));
            break;
        }

        case 0x0007: { // Start scan, which finds the access point if there is one
            read32(0); // Force foreground
            read32(0); // Legacy
            uint32_t time = read32(0);
            if (!core->wifi.getAp()) {
                uint8_t status[4] = {};
                sendEvent(1, 0x100A, status, sizeof(status));
                break;
            }
            if (reportAp) sendBssInfo();
            scanMs = std::max<uint32_t>(time, 1);
            break;
        }

        case 0x000A: { // Set probed SSID, which limits scans to one network unless the flags are clear
            readMailbox(0, false);
            uint8_t flags = readMailbox(0, false);
            uint8_t len = readMailbox(0, false);
            char ssid[33] = {};
            for (int i = 0; i < len && i < 32; i++)
                ssid[i] = readMailbox(0, false);
            reportAp = !flags || !strcmp(ssid, WifiAp::ssid);
            break;
        }

        case 0x000E: { // Get channel list: 1 to 11
            uint8_t reply[2 + 11 * 2 + 2] = { 0, 11 };
            for (int i = 0; i < 11; i++) {
                reply[2 + i * 2] = uint8_t(2412 + i * 5);
                reply[3 + i * 2] = uint8_t((2412 + i * 5) >> 8);
            }
            sendEvent(1, 0x000E, reply, sizeof(reply));
            break;
        }

        case 0x002E: { // Extension, of which only the heartbeat needs an answer
            uint32_t ext = read32(0);
            if (ext == 0x2008) {
                uint32_t cookie = read32(0), source = read32(0);
                uint8_t reply[12] = { 0x07, 0x30 };
                for (int i = 0; i < 4; i++) {
                    reply[4 + i] = cookie >> (i * 8);
                    reply[8 + i] = source >> (i * 8);
                }
                sendEvent(1, 0x1010, reply, sizeof(reply));
            }
            break;
        }

        case 0x0004: case 0x0005: case 0x0008: case 0x0009: case 0x000D: case 0x0011: case 0x0012:
        case 0x0017: case 0x0022: case 0x003D: case 0x0041: case 0x0047: case 0x0048: case 0x0049: case 0xF000:
            // Settings that need no answer
            break;

        default:
            LOG_WARN("Unknown DSi Wi-Fi WMI command: 0x%X\n", cmd);
            break;
        }
    }
    if (header & BIT(8)) sendAck(endpoint);
    drain(0);
}

void DsiWifi::connect() {
    // Join the access point if it's the one asked for, as an open network, or report that there's no such network
    uint8_t type = readMailbox(0, false);
    uint8_t auth11 = readMailbox(0, false);
    uint8_t auth = readMailbox(0, false);
    uint8_t pairCrypto = readMailbox(0, false);
    readMailbox(0, false);
    uint8_t groupCrypto = readMailbox(0, false);
    readMailbox(0, false);
    uint8_t ssidLen = readMailbox(0, false);
    char ssid[33] = {};
    for (int i = 0; i < 32; i++)
        ssid[i] = readMailbox(0, false);
    ssid[std::min<uint8_t>(ssidLen, 32)] = 0;
    read16(0); // Channel
    uint8_t bssid[6];
    for (int i = 0; i < 6; i++)
        bssid[i] = readMailbox(0, false);

    static const uint8_t zero[6] = {};
    bool found = core->wifi.getAp() && (!memcmp(bssid, WifiAp::mac, 6) ||
        (!memcmp(bssid, zero, 6) && !strcmp(ssid, WifiAp::ssid)));
    if (!found || type != 1 || auth11 != 1 || auth != 1 || pairCrypto != 1 || groupCrypto != 1) {
        LOG_WARN("DSi Wi-Fi can't connect to %s: type %d, auth %d/%d, crypto %d/%d\n",
            ssid, type, auth11, auth, pairCrypto, groupCrypto);
        uint8_t reply[11] = {};
        memcpy(&reply[2], bssid, 6);
        reply[8] = 1; // Reason: no network
        connected = false;
        sendEvent(1, 0x1003, reply, sizeof(reply));
        return;
    }

    // Report the channel's frequency, the access point, the listen and beacon intervals, and an infrastructure network;
    // the information element sizes are the ones melonDS uses
    LOG_INFO("DSi Wi-Fi connected to %s\n", ssid);
    uint16_t freq = 2407 + WifiAp::channel * 5;
    uint8_t reply[20] = { uint8_t(freq), uint8_t(freq >> 8) };
    memcpy(&reply[2], WifiAp::mac, 6);
    reply[8] = reply[10] = 128;
    reply[12] = 0x01;
    reply[16] = 0x16;
    reply[17] = 0x2F;
    reply[18] = 0x16;
    connected = true;
    sendEvent(1, 0x1002, reply, sizeof(reply));
}

void DsiWifi::sendPacket(uint16_t size) {
    // Pass a data packet to the network as an Ethernet frame; it has the chip's header, then 802.3 with LLC/SNAP
    WifiAp *ap = core->wifi.getAp();
    if (!connected || !ap || size < 24) return;
    uint16_t header = read16(0);
    if (header & 0x0300) return; // Sync and other special frames, which need nothing done
    std::vector<uint8_t> frame(14);
    for (int i = 0; i < 12; i++)
        frame[i] = readMailbox(0, false);
    uint16_t len = readMailbox(0, false) << 8;
    len |= readMailbox(0, false);
    if (len < 8 || len > size - 16) {
        LOG_WARN("Bad DSi Wi-Fi packet length: 0x%X\n", len);
        return;
    }
    static const uint8_t snap[] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
    for (int i = 0; i < 6; i++) {
        if (readMailbox(0, false) != snap[i]) return;
    }
    frame[12] = readMailbox(0, false);
    frame[13] = readMailbox(0, false);
    for (int i = 0; i < len - 8; i++)
        frame.push_back(readMailbox(0, false));
    ap->sendEthernet(frame.data(), frame.size());
}

void DsiWifi::receivePackets() {
    // Pass packets from the network for this chip to the data endpoint, while there's room for them
    WifiAp *ap = core->wifi.getAp();
    std::vector<uint8_t> frame;
    for (int n = 0; n < 4 && mailbox[8].size() + 0x800 <= EVENT_SIZE; n++) {
        if (!ap->receiveEthernet(frame)) break;
        if (frame.size() < 14 || frame.size() > 1514) continue; // Larger frames wouldn't fit in mailbox 4
        if (!(frame[0] & 0x01) && memcmp(&frame[0], &eeprom[0xA], 6)) continue;
        if (!memcmp(&frame[6], &eeprom[0xA], 6)) continue;

        // Send it with the chip's header, the addresses, the big-endian length, and an LLC/SNAP header
        uint32_t len = frame.size() - 14;
        mailbox[8].push_back(2); // Endpoint
        mailbox[8].push_back(0);
        write16(8, 16 + 8 + len);
        write16(8, 0);
        write16(8, 0x0080);
        mailbox[8].insert(mailbox[8].end(), &frame[0], &frame[12]);
        mailbox[8].push_back((len + 8) >> 8);
        mailbox[8].push_back(len + 8);
        static const uint8_t snap[] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
        mailbox[8].insert(mailbox[8].end(), snap, snap + sizeof(snap));
        mailbox[8].insert(mailbox[8].end(), &frame[12], frame.data() + frame.size());
        drainEvents();
    }
}

void DsiWifi::sendBssInfo() {
    // Report the access point as found by a scan, with the frequency, a beacon, the signal, the access point's address
    // and an empty information element mask, then the body of its beacon
    uint16_t freq = 2407 + WifiAp::channel * 5;
    const uint8_t ssidLen = sizeof(WifiAp::ssid) - 1;
    uint8_t info[] = {
        uint8_t(freq), uint8_t(freq >> 8), 0x01, 0x1B, 0xBC, 0xFF,
        WifiAp::mac[0], WifiAp::mac[1], WifiAp::mac[2], WifiAp::mac[3], WifiAp::mac[4], WifiAp::mac[5],
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Timestamp
        0x80, 0x00, // Beacon interval
        0x21, 0x00, // Capabilities
        0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, // Rates
        0x03, 0x01, WifiAp::channel, // Channel
        0x05, 0x04, 0x00, 0x00, 0x00, 0x00, // Traffic indication map
        0x00, ssidLen // SSID, which follows
    };

    // Unlike other events this has no trailer, and the control bytes are 0xFF as the real chip sends them
    mailbox[8].push_back(1); // Endpoint
    mailbox[8].push_back(0);
    write16(8, sizeof(info) + ssidLen + 2);
    mailbox[8].push_back(0xFF);
    mailbox[8].push_back(0xFF);
    write16(8, 0x1004);
    mailbox[8].insert(mailbox[8].end(), info, info + sizeof(info));
    mailbox[8].insert(mailbox[8].end(), WifiAp::ssid, WifiAp::ssid + ssidLen);
    drainEvents();
}

void DsiWifi::countMs() {
    // Finish a scan once its time is up, and receive packets while connected
    if (scanMs && --scanMs == 0) {
        uint8_t status[4] = {};
        sendEvent(1, 0x100A, status, sizeof(status));
    }
    if (connected && core->wifi.getAp())
        receivePackets();
}

void DsiWifi::sendEvent(uint8_t endpoint, uint16_t id, const uint8_t *data, uint32_t size) {
    // Queue an event with its header and a lookahead trailer, then pass it on if there's room
    mailbox[8].push_back(endpoint);
    mailbox[8].push_back(0x02); // Has a trailer
    write16(8, size + 2 + 8);
    mailbox[8].push_back(8); // Trailer size
    mailbox[8].push_back(0);
    write16(8, id);
    mailbox[8].insert(mailbox[8].end(), data, data + size);
    static const uint8_t trailer[] = { 0x02, 0x06, 0, 0, 0, 0, 0, 0 };
    mailbox[8].insert(mailbox[8].end(), trailer, trailer + sizeof(trailer));
    drainEvents();
}

void DsiWifi::sendAck(uint8_t endpoint) {
    // Return a credit for an endpoint, with a lookahead trailer
    static const uint8_t header[] = { 0, 0x02, 0x0C, 0, 0x0C, 0 };
    mailbox[8].insert(mailbox[8].end(), header, header + sizeof(header));
    uint8_t credit[] = { 0x01, 0x02, endpoint, 0x01, 0x02, 0x06, 0, 0, 0, 0, 0, 0 };
    mailbox[8].insert(mailbox[8].end(), credit, credit + sizeof(credit));
    drainEvents();
}
