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

#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>

class Core;

// The DSi's Atheros Wi-Fi chip, behind the SDIO controller; the values follow melonDS's DSi_NWifi
// It takes the firmware the system uploads and reports being ready, which the DSi menu needs to start any title, but
// it doesn't connect to anything yet
class DsiWifi {
public:
    DsiWifi(Core *core): core(core) {}
    void saveState(FILE *file);
    void loadState(FILE *file);
    void reset();

    bool readIrq() { return irq; }
    uint8_t ioDirect(uint32_t param);
    void startTransfer(uint32_t param);
    void readData(uint8_t *data, uint32_t size);
    void writeData(const uint8_t *data, uint32_t size);

private:
    Core *core;

    // Mailboxes 0-3 are from the host and 4-7 to it; events wait in 8 until there's room in 4
    std::deque<uint8_t> mailbox[9];
    uint32_t transferParam = 0;
    uint32_t transferAddr = 0;

    uint8_t f0IrqEnable = 0;
    uint8_t f0IrqStatus = 0;
    uint8_t f1IrqEnable[4] = {}; // Mailboxes, CPU, error, counter
    uint8_t f1IrqStatus[4] = {};
    bool irq = false;

    uint32_t windowData = 0;
    uint32_t windowReadAddr = 0;
    uint32_t windowWriteAddr = 0;

    uint8_t cis0[0x100] = {};
    uint8_t cis1[0x100] = {};
    uint8_t eeprom[0x400] = {};
    uint32_t romId = 0;
    uint32_t chipId = 0;
    uint32_t hostIntAddr = 0;
    uint8_t bootPhase = 0; // BMI, HTC, then WMI
    bool firmwareLoaded = false; // Kept across resets, like the chip's own state

    void updateIrq();
    void updateF1Irq();
    uint8_t readF0(uint32_t addr);
    void writeF0(uint32_t addr, uint8_t value);
    uint8_t readF1(uint32_t addr);
    void writeF1(uint32_t addr, uint8_t value);
    uint8_t readMailbox(int i, bool last);
    void writeMailbox(int i, uint8_t value, bool last);

    uint16_t read16(int i);
    uint32_t read32(int i);
    void write16(int i, uint16_t value);
    void write32(int i, uint32_t value);
    void drain(int i);
    void drainEvents();
    uint32_t readWindow(uint32_t addr);

    void handleCommand();
    void bmiCommand();
    void htcCommand();
    void wmiCommand();
    void sendEvent(uint8_t endpoint, uint16_t id, const uint8_t *data, uint32_t size);
    void sendAck(uint8_t endpoint);
};
