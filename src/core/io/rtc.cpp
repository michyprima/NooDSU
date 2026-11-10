/*
    Copyright 2019-2026 Hydr8gon
    Copyright 2026 michyprima

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

#include <ctime>
#include "../core.h"

std::time_t Rtc::fixedTime = 0;

void Rtc::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&csCur, sizeof(csCur), 1, file);
    fwrite(&sckCur, sizeof(sckCur), 1, file);
    fwrite(&sioCur, sizeof(sioCur), 1, file);
    fwrite(&writeCount, sizeof(writeCount), 1, file);
    fwrite(&command, sizeof(command), 1, file);
    fwrite(&control, sizeof(control), 1, file);
    fwrite(dateTime, 1, sizeof(dateTime), file);
    fwrite(&rtc, sizeof(rtc), 1, file);
    fwrite(&gpDirection, sizeof(gpDirection), 1, file);
    fwrite(&gpControl, sizeof(gpControl), 1, file);
    fwrite(&status2, sizeof(status2), 1, file);
    fwrite(&adjust, sizeof(adjust), 1, file);
    fwrite(&freeReg, sizeof(freeReg), 1, file);
    fwrite(&dateOffset, sizeof(dateOffset), 1, file);
    fwrite(&dayShift, sizeof(dayShift), 1, file);
    fwrite(&ticking, sizeof(ticking), 1, file);
    fwrite(&subTick, sizeof(subTick), 1, file);
    fwrite(&lastMinute, sizeof(lastMinute), 1, file);
    fwrite(&irqFlag, sizeof(irqFlag), 1, file);
    fwrite(&rcnt, sizeof(rcnt), 1, file);
    fwrite(alarm, 1, sizeof(alarm), file);
    fwrite(alarmDate, 1, sizeof(alarmDate), file);
    fwrite(fout, 1, sizeof(fout), file);
}

void Rtc::loadState(FILE *file) {
    // Read state data from the file
    fread(&csCur, sizeof(csCur), 1, file);
    fread(&sckCur, sizeof(sckCur), 1, file);
    fread(&sioCur, sizeof(sioCur), 1, file);
    fread(&writeCount, sizeof(writeCount), 1, file);
    fread(&command, sizeof(command), 1, file);
    fread(&control, sizeof(control), 1, file);
    fread(dateTime, 1, sizeof(dateTime), file);
    fread(&rtc, sizeof(rtc), 1, file);
    fread(&gpDirection, sizeof(gpDirection), 1, file);
    fread(&gpControl, sizeof(gpControl), 1, file);
    fread(&status2, sizeof(status2), 1, file);
    fread(&adjust, sizeof(adjust), 1, file);
    fread(&freeReg, sizeof(freeReg), 1, file);
    fread(&dateOffset, sizeof(dateOffset), 1, file);
    fread(&dayShift, sizeof(dayShift), 1, file);
    fread(&ticking, sizeof(ticking), 1, file);
    fread(&subTick, sizeof(subTick), 1, file);
    fread(&lastMinute, sizeof(lastMinute), 1, file);
    fread(&irqFlag, sizeof(irqFlag), 1, file);
    fread(&rcnt, sizeof(rcnt), 1, file);
    fread(alarm, 1, sizeof(alarm), file);
    fread(alarmDate, 1, sizeof(alarmDate), file);
    fread(fout, 1, sizeof(fout), file);
}

void Rtc::updateRtc(bool cs, bool sck, bool sio) {
    if (cs) {
        // Transfer a bit to the RTC when SCK changes from low to high
        if (!sckCur && sck) {
            if (writeCount < 8) {
                // Write the first 8 bits to the command register
                command |= sio << (7 - writeCount);

                // Once the command is written, reverse the bit order if necessary
                // DSi extended commands start with 0x7 instead, apart from one that's really a reversed normal command
                bool extended = core->dsiMode && (command & 0xF0) == 0x70 && (command & 0xFE) != 0x76;
                if (writeCount == 7 && (command & 0xF0) != 0x60 && !extended) {
                    uint8_t value = 0;
                    for (int i = 0; i < 8; i++)
                        value |= ((command >> i) & BIT(0)) << (7 - i);
                    command = value;
                }
            }
            else if (command & BIT(0)) {
                // Read a bit from an RTC register
                sio = readRegister((command >> 1) & 0x7);
            }
            else {
                // Write a bit to an RTC register
                writeRegister((command >> 1) & 0x7, sio);
            }

            writeCount++;
        }
    }
    else {
        // Reset the transfer when CS is low
        writeCount = 0;
        command = 0;
    }

    // Update the CS/SCK/SIO bits
    csCur = cs;
    sckCur = sck;
    sioCur = sio;
}

void Rtc::updateDateTime(uint8_t *out) {
    // Get the local time, moved by any time that was set
    std::time_t t = (fixedTime ? fixedTime : std::time(nullptr)) + dateOffset;
    std::tm *time = std::localtime(&t);
    bool pm = time->tm_hour >= 12;
    time->tm_year %= 100; // The DS only counts years 2000-2099
    time->tm_mon++; // The DS starts month values at 1, not 0

    // Convert to 12-hour format if enabled
    if (!(control & BIT(core->gbaMode ? 6 : 1)))
        time->tm_hour %= 12;

    // Save to the date and time registers in BCD format
    out[0] = ((time->tm_year / 10) << 4) | (time->tm_year % 10);
    out[1] = ((time->tm_mon / 10) << 4) | (time->tm_mon % 10);
    out[2] = ((time->tm_mday / 10) << 4) | (time->tm_mday % 10);
    out[3] = (time->tm_wday + dayShift) % 7;
    out[4] = ((time->tm_hour / 10) << 4) | (time->tm_hour % 10);
    out[5] = ((time->tm_min / 10) << 4) | (time->tm_min % 10);
    out[6] = ((time->tm_sec / 10) << 4) | (time->tm_sec % 10);

    // Set the AM/PM bit
    if (pm)
        out[4] |= BIT(6 << core->gbaMode);
}

void Rtc::applyDateTime() {
    // Turn the registers into a time, and keep how far it is from the real clock
    auto bcd = [](uint8_t value) { return (value >> 4) * 10 + (value & 0xF); };
    std::tm time = {};
    time.tm_year = bcd(dateTime[0]) + 100;
    time.tm_mon = bcd(dateTime[1] & 0x1F) - 1;
    time.tm_mday = bcd(dateTime[2] & 0x3F);
    time.tm_hour = bcd(dateTime[4] & 0x3F) + ((~control & BIT(1)) && (dateTime[4] & BIT(6)) ? 12 : 0);
    time.tm_min = bcd(dateTime[5] & 0x7F);
    time.tm_sec = bcd(dateTime[6] & 0x7F);
    time.tm_isdst = -1;
    dateOffset = (int64_t)std::mktime(&time) - (fixedTime ? fixedTime : std::time(nullptr));
    dayShift = ((dateTime[3] & 0x7) + 7 - time.tm_wday) % 7;
}

void Rtc::updateTicking() {
    // Start counting time for interrupts when any are enabled; ticks stop by themselves once none are
    if (ticking || !(status2 & 0x4F)) return;
    ticking = true;
    uint8_t clock[7];
    updateDateTime(clock);
    lastMinute = clock[5];
    core->schedule(RTC_TICK, TICK_CYCLES);
}

void Rtc::tick() {
    // Stop if interrupts were disabled, or keep ticking 32 times per second
    if (!(status2 & 0x4F)) {
        ticking = false;
        return;
    }
    core->schedule(RTC_TICK, TICK_CYCLES);
    subTick = (subTick + 1) & 0x1F;

    // Interrupts follow the clock games read, which is the real time and not the emulated time
    uint8_t clock[7];
    updateDateTime(clock);
    if (clock[5] != lastMinute) {
        lastMinute = clock[5];
        updateIrq(0, clock);
    }
    updateIrq(1, clock);
}

void Rtc::setIrq(uint8_t bits) {
    // Set interrupt flags, sending an ARM7 interrupt if the RTC's interrupt line goes high and RCNT connects it
    bool wasHigh = irqFlag;
    irqFlag |= bits;
    control |= bits;
    if (!wasHigh && (rcnt & 0xC100) == 0x8100)
        core->interpreter[1].sendInterrupt(7);
}

bool Rtc::alarmMatches(int i, const uint8_t *clock) {
    // Compare each enabled part of an alarm with the clock
    if ((alarm[i][0] & BIT(7)) && (alarm[i][0] & 0x07) != clock[3]) return false;
    if ((alarm[i][1] & BIT(7)) && (alarm[i][1] & 0x7F) != clock[4]) return false;
    if ((alarm[i][2] & BIT(7)) && (alarm[i][2] & 0x7F) != clock[5]) return false;

    // The DSi's RTC can also compare the date
    if (core->dsiMode) {
        if ((alarmDate[i][1] & BIT(6)) && alarmDate[i][0] != clock[0]) return false;
        if ((alarmDate[i][1] & BIT(7)) && (alarmDate[i][1] & 0x1F) != clock[1]) return false;
        if ((alarmDate[i][2] & BIT(7)) && (alarmDate[i][2] & 0x3F) != clock[2]) return false;
    }
    return true;
}

void Rtc::updateIrq(int type, const uint8_t *clock) {
    // Update the interrupts for a new minute (type 0), a tick (1) or a write to status 2 (2), based on melonDS
    // Ticks are 32 times per second, so the 32kHz output only pulses that often
    switch (status2 & 0x0F) {
    case 0x0: // None
        if (type == 2) irqFlag &= ~0x10;
        break;

    case 0x1: case 0x5: // Selected frequency steady
        if (type != 0) {
            uint16_t mask = ((alarm[0][2] & 0x01) ? 0x4000 : 0) | ((alarm[0][2] & 0x02) ? 0x2000 : 0) |
                ((alarm[0][2] & 0x04) ? 0x1000 : 0) | ((alarm[0][2] & 0x08) ? 0x0800 : 0) | ((alarm[0][2] & 0x10) ? 0x0400 : 0);
            if (mask && ((subTick << 10) & mask) != mask) setIrq(0x10);
            else irqFlag &= ~0x10;
        }
        break;

    case 0x2: case 0x6: // Per-minute edge
        if (type == 0) setIrq(0x10);
        break;

    case 0x3: // Per-minute steady, for 30 seconds
        if (type == 0) setIrq(0x10);
        else if (type == 1 && clock[6] >= 0x30) irqFlag &= ~0x10;
        break;

    case 0x7: // Per-minute steady, for 256 RTC cycles, which is less than a tick
        if (type == 0) setIrq(0x10);
        else if (type == 1) irqFlag &= ~0x10;
        break;

    case 0x4: // Alarm 1
        if (type == 0) {
            if (alarmMatches(0, clock)) setIrq(0x10);
            else irqFlag &= ~0x10;
        }
        break;

    default: // 32kHz output
        if (type == 1) {
            setIrq(0x10);
            irqFlag &= ~0x10;
        }
        break;
    }

    // Alarm 2 is the only thing that uses the second interrupt
    if (status2 & BIT(6)) {
        if (type == 0) {
            if (alarmMatches(1, clock)) setIrq(0x20);
            else irqFlag &= ~0x20;
        }
    }
    else if (type == 2) {
        irqFlag &= ~0x20;
    }
}

void Rtc::reset() {
    // Reset the RTC registers
    updateRtc(0, 0, 0);
    control = 0;
    rtc = 0;
    gpDirection = 0;
    gpControl = 0;
    status2 = 0;
    memset(alarm, 0, sizeof(alarm));
    memset(alarmDate, 0, sizeof(alarmDate));
    adjust = 0;
    freeReg = 0;
    memset(fout, 0, sizeof(fout));
    dateOffset = 0;
    dayShift = 0;
    irqFlag = 0;
}

bool Rtc::readRegister(uint8_t index) {
    if (core->gbaMode) {
        // Read a bit from a GBA RTC register
        switch (index) {
        case 0: // Reset
            reset();
            return 0;

        case 1: // Control
            return (control >> (writeCount & 7)) & BIT(0);

        case 2: // Date and time
            if (writeCount == 8) updateDateTime(dateTime);
            return (dateTime[(writeCount / 8) - 1] >> (writeCount % 8)) & BIT(0);

        case 3: // Time
            if (writeCount == 8) updateDateTime(dateTime);
            return (dateTime[(writeCount / 8) - 5] >> (writeCount % 8)) & BIT(0);

        default:
            LOG_WARN("Read from unknown GBA RTC register: %d\n", index);
            return 0;
        }
    }

    // Read a bit from an NDS RTC register, or one past the end of a register as 0
    int byte = (writeCount / 8) - 1;
    auto bit = [&](uint8_t reg) -> bool { return (reg >> (writeCount & 7)) & BIT(0); };

    if (core->dsiMode && (command & BIT(4))) {
        // Read from a DSi extended register
        switch (index) {
        case 0: { // Up counter, which counts minutes
            int64_t minutes = ((fixedTime ? fixedTime : std::time(nullptr)) + dateOffset) / 60;
            return byte < 3 && bit(minutes >> (16 - byte * 8));
        }
        case 1: return byte == 0 && bit(fout[0]); // FOUT 1
        case 2: return byte == 0 && bit(fout[1]); // FOUT 2
        case 4: case 5: return byte < 3 && bit(alarmDate[index - 4][byte]); // Alarm dates

        default:
            LOG_WARN("Read from unknown DSi RTC register: %d\n", index);
            return 0;
        }
    }

    switch (index) {
    case 0: { // Status 1
        bool value = bit(control);
        if ((writeCount & 7) == 7) control &= 0x0F; // Reading clears the flags
        return value;
    }

    case 1: // Status 2
        return byte == 0 && bit(status2);

    case 2: // Date and time
        if (writeCount == 8) updateDateTime(dateTime);
        return byte < 7 && bit(dateTime[byte]);

    case 3: // Time
        if (writeCount == 8) updateDateTime(dateTime);
        return byte < 3 && bit(dateTime[byte + 4]);

    case 4: // Alarm 1 when status 2 selects it, otherwise only the frequency duty
        if (status2 & BIT(2)) return byte < 3 && bit(alarm[0][byte]);
        return byte == 0 && bit(alarm[0][2]);

    case 5: // Alarm 2
        return byte < 3 && bit(alarm[1][byte]);

    case 6: // Clock adjustment
        return byte == 0 && bit(adjust);

    case 7: // Free register
        return byte == 0 && bit(freeReg);
    }
    return 0;
}

void Rtc::writeRegister(uint8_t index, bool value) {
    if (core->gbaMode) {
        // Read a bit from a GBA RTC register
        switch (index) {
        case 1: // Control
            if (BIT(writeCount & 7) & 0x6A) // R/W bits
                control = (control & ~BIT(writeCount & 7)) | (value << (writeCount & 7));
            return;

        default:
            LOG_WARN("Write to unknown GBA RTC register: %d\n", index);
            return;
        }
    }

    // Write a bit to an NDS RTC register
    int byte = (writeCount / 8) - 1;
    bool lastBit = (writeCount & 7) == 7;
    auto set = [&](uint8_t &reg) { reg = (reg & ~BIT(writeCount & 7)) | (value << (writeCount & 7)); };

    if (core->dsiMode && (command & BIT(4))) {
        // Write to a DSi extended register; the up counter can only be read
        switch (index) {
        case 1: if (byte == 0) set(fout[0]); return; // FOUT 1
        case 2: if (byte == 0) set(fout[1]); return; // FOUT 2
        case 4: case 5: if (byte < 3) set(alarmDate[index - 4][byte]); return; // Alarm dates

        default:
            LOG_WARN("Write to unknown DSi RTC register: %d\n", index);
            return;
        }
    }

    switch (index) {
    case 0: // Status 1
        if ((BIT(writeCount & 7) & 0x01) && value) // Reset bit
            reset();
        else if (BIT(writeCount & 7) & 0x0E) // R/W bits
            set(control);
        return;

    case 1: // Status 2
        if (byte != 0) return;
        set(status2);
        if (lastBit) {
            updateTicking();
            updateIrq(2, dateTime);
        }
        return;

    case 2: case 3: { // Date and time, or only the time
        // Start from the current values, so a shorter write keeps the rest
        if (writeCount == 8) updateDateTime(dateTime);
        int first = (index == 2) ? 0 : 4;
        if (byte + first >= 7) return;
        set(dateTime[byte + first]);
        if (lastBit && byte + first == 6) applyDateTime();
        return;
    }

    case 4: // Alarm 1 when status 2 selects it, otherwise only the frequency duty
        if (status2 & BIT(2)) { if (byte < 3) set(alarm[0][byte]); }
        else if (byte == 0) set(alarm[0][2]);
        return;

    case 5: // Alarm 2
        if (byte < 3) set(alarm[1][byte]);
        return;

    case 6: // Clock adjustment
        if (byte == 0) set(adjust);
        return;

    case 7: // Free register
        if (byte == 0) set(freeReg);
        return;
    }
}

void Rtc::writeRtc(uint8_t value) {
    // Write to the RTC register
    rtc = value & ~0x07;

    // Change the CS/SCK/SIO bits if writable and update the RTC
    bool cs = (rtc & BIT(6)) ? (value & BIT(2)) : csCur;
    bool sck = (rtc & BIT(5)) ? !(value & BIT(1)) : sckCur;
    bool sio = (rtc & BIT(4)) ? (value & BIT(0)) : sioCur;
    updateRtc(cs, sck, sio);
}

void Rtc::writeGpData(uint16_t mask, uint16_t value) {
    if (mask & 0xFF) {
        // Change the CS/SCK/SIO bits if writable and update the RTC
        bool cs = (gpDirection & BIT(2)) ? (value & BIT(2)) : csCur;
        bool sio = (gpDirection & BIT(1)) ? (value & BIT(1)) : sioCur;
        bool sck = (gpDirection & BIT(0)) ? (value & BIT(0)) : sckCur;
        updateRtc(cs, sck, sio);

        // Drive a solar sensor or rumble motor on the same pins
        core->slot2.writeGpioPins(value & gpDirection, gpDirection);
    }
}

void Rtc::writeGpDirection(uint16_t mask, uint16_t value) {
    // Write to the GP_DIRECTION register
    mask &= 0x000F;
    gpDirection = (gpDirection & ~mask) | (value & mask);
}

void Rtc::writeGpControl(uint16_t mask, uint16_t value) {
    // Only allow enabling register reads if an RTC was detected
    if (!gpRtc)
        return;

    // Write to the GP_CONTROL register
    mask &= 0x0001;
    gpControl = (gpControl & ~mask) | (value & mask);

    // Update the memory map to reflect the read status of the GP registers
    core->memory.updateMap7(0x8000000, 0x8001000);
}

uint8_t Rtc::readRtc() {
    // Get the CS/SCK/SIO bits if readable and read from the RTC register
    bool cs = csCur;
    bool sck = (rtc & BIT(5)) ? 0 : sckCur;
    bool sio = (rtc & BIT(4)) ? 0 : sioCur;
    return rtc | (cs << 2) | (sck << 1) | (sio << 0);
}

uint16_t Rtc::readGpData() {
    // Get the CS/SCK/SIO bits if readable and read from the GP_DATA register
    bool cs = (gpDirection & BIT(2)) ? 0 : csCur;
    bool sio = (gpDirection & BIT(1)) ? 0 : sioCur;
    bool sck = (gpDirection & BIT(0)) ? 0 : sckCur;
    return (cs << 2) | (sio << 1) | (sck << 0) | core->slot2.readGpioPins(gpDirection);
}
