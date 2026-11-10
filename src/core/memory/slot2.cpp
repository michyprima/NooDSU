/*
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

#include "../core.h"

// How far the solar sensor's light level moves its sample, for sunlight levels 0 to 10 (from melonDS)
static const uint8_t solarLux[] = { 0, 5, 11, 18, 27, 42, 62, 84, 109, 139, 183 };

void Slot2::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&device, sizeof(device), 1, file);
    fwrite(&gpioData, sizeof(gpioData), 1, file);
    fwrite(&gpioDirection, sizeof(gpioDirection), 1, file);
    fwrite(&gpioControl, sizeof(gpioControl), 1, file);
    fwrite(&solarCounter, sizeof(solarCounter), 1, file);
    fwrite(&solarSample, sizeof(solarSample), 1, file);
    fwrite(&solarEdge, sizeof(solarEdge), 1, file);
    fwrite(&rumbleState, sizeof(rumbleState), 1, file);
    fwrite(&ramUnlocked, sizeof(ramUnlocked), 1, file);
    fwrite(&paddle, sizeof(paddle), 1, file);
    if (ram) fwrite(ram, 1, 0x800000, file);
}

void Slot2::loadState(FILE *file) {
    // Read state data from the file, skipping the expansion RAM if the state was made with another accessory
    int saved = SLOT2_NONE;
    fread(&saved, sizeof(saved), 1, file);
    fread(&gpioData, sizeof(gpioData), 1, file);
    fread(&gpioDirection, sizeof(gpioDirection), 1, file);
    fread(&gpioControl, sizeof(gpioControl), 1, file);
    fread(&solarCounter, sizeof(solarCounter), 1, file);
    fread(&solarSample, sizeof(solarSample), 1, file);
    fread(&solarEdge, sizeof(solarEdge), 1, file);
    fread(&rumbleState, sizeof(rumbleState), 1, file);
    fread(&ramUnlocked, sizeof(ramUnlocked), 1, file);
    fread(&paddle, sizeof(paddle), 1, file);
    if (ram && saved == device) fread(ram, 1, 0x800000, file);

    // Map the expansion RAM as it was
    core->memory.updateMap9(0x8000000, 0xA000000);
    core->memory.updateMap7(0x8000000, 0xA000000);
}

void Slot2::init() {
    // There's no GBA slot on a DSi
    if (core->dsiMode) return;

    if (core->cartridgeGba.getRomSize() > 0) {
        // A GBA ROM fills the slot; check its game code for a solar sensor or a rumble motor on the GPIO pins
        // Boktai 1, 2 and 3 (and a sample of 1) have the solar sensor; WarioWare: Twisted and Drill Dozer have rumble
        uint8_t *code = core->cartridgeGba.getRom(0x80000AC);
        static const char *solarCodes[] = { "U3I", "U32", "U33", "A3I" };
        static const char *rumbleCodes[] = { "RZW", "V49" };
        for (const char *prefix : solarCodes)
            if (code && !memcmp(code, prefix, 3)) solar = true;
        for (const char *prefix : rumbleCodes)
            if (code && !memcmp(code, prefix, 3)) gbaRumble = true;
    }
    else if (!core->gbaMode) {
        // Put the chosen accessory in the empty slot
        device = Settings::slot2Device % SLOT2_MAX;
        if (device == SLOT2_EXPANSION) {
            // The RAM starts out filled with 0xFF (as melonDS and DeSmuME have it)
            ram = new uint8_t[0x800000];
            memset(ram, 0xFF, 0x800000);
        }
        else if (device == SLOT2_SOLAR) {
            // Make up the header of a Boktai cart, which is all games look at besides the sensor
            // The logo is copied from the DS ROM, which has the same one; the game code is Boktai's (USA)
            solar = true;
            if (const uint8_t *logo = core->cartridgeNds.getLogo())
                memcpy(&fakeRom[0x04], logo, 0x9C);
            else
                memset(&fakeRom[0x04], 0xFF, 0x9C);
            memcpy(&fakeRom[0xA0], "BOKTAI", 6);
            memcpy(&fakeRom[0xAC], "U3IE", 4);
            memcpy(&fakeRom[0xB0], "A4", 2);
            fakeRom[0xB2] = 0x96;
            uint8_t check = 0;
            for (int i = 0xA0; i < 0xBD; i++)
                check -= fakeRom[i];
            fakeRom[0xBD] = check - 0x19;
        }
    }

    // Map the slot again now that it's known what's in it
    core->memory.updateMap9(0x8000000, 0xA000000);
    core->memory.updateMap7(0x8000000, 0xA000000);
}

bool Slot2::takeRumble() {
    // Report if anything rumbled since the last call: the Rumble Pak if it flipped, a GBA cart if its motor is on
    return rumbleFlips.exchange(0) || motorOn.load();
}

bool Slot2::owns(bool arm7) {
    // Check if a CPU has the GBA slot, which EXMEMCNT gives to one of them
    return arm7 == bool(core->memory.readExMemCnt() & BIT(7));
}

uint16_t Slot2::openBus(bool arm7, uint32_t address) {
    // Read what floats on the bus with nothing driving it: the halfword address, unless the ROM access time
    // leaves garbage on it (10 cycles) or lets it go high (18 cycles), according to GBATEK
    uint16_t timing = (arm7 ? core->memory.readExMemStat() : core->memory.readExMemCnt()) >> 2;
    switch (timing & 0x3) {
        case 0: return (address >> 1) | 0xFE08;
        case 3: return 0xFFFF;
        default: return address >> 1;
    }
}

uint8_t *Slot2::getRom(uint32_t address) {
    // Get the ROM to map for a page, leaving the GPIO registers' page to the slow path
    if ((solar || gbaRumble) && address < 0x8001000) return nullptr;
    return core->cartridgeGba.getRom(address);
}

uint8_t *Slot2::getRam(uint32_t address) {
    // Get the expansion RAM to map for a page, while it's unlocked
    if (!ram || !ramUnlocked || (address & 0xFF800000) != 0x9000000) return nullptr;
    return &ram[address & 0x7FFFFF];
}

uint16_t Slot2::read16(bool arm7, uint32_t address) {
    // Read the GPIO registers if the game made them readable
    uint32_t offset = address & 0x1FFFFFF;
    if ((solar || gbaRumble) && (gpioControl & BIT(0)) && offset >= 0xC4 && offset < 0xCA) {
        switch (offset) {
            case 0xC4: return (gpioData & gpioDirection) | readGpioPins(gpioDirection);
            case 0xC6: return gpioDirection;
            case 0xC8: return gpioControl;
        }
    }

    // Read the GBA ROM if there is one, or the made-up Boktai header
    if (uint8_t *data = core->cartridgeGba.getRom(address))
        return readLsb<uint16_t>(data);
    if (solar && offset < sizeof(fakeRom))
        return readLsb<uint16_t>(&fakeRom[offset]);

    switch (device) {
    case SLOT2_RUMBLE:
        // The Rumble Pak pulls address line 1 low, which is how games find it (GBATEK)
        return openBus(arm7, address) & 0xFFFD;

    case SLOT2_EXPANSION:
        // Read the RAM, or the ID values Opera's browser checks (from melonDS)
        if (offset >= 0x1000000) {
            if (offset < 0x1800000 && ramUnlocked)
                return readLsb<uint16_t>(&ram[offset & 0x7FFFFF]);
            return 0xFFFF;
        }
        switch (offset) {
            case 0xB2: case 0x240002: return 0x0000;
            case 0xB4: return 0x2400;
            case 0xB6: return 0x2424;
            case 0xBE: case 0x1FFFE: return 0x7FFF;
            case 0x240000: return ramUnlocked;
            default: return 0xFFFF;
        }

    case SLOT2_GUITAR:
        // The Guitar Grip shows up as 0xF9FF everywhere in ROM; its buttons are in SRAM (from DeSmuME and melonDS)
        return 0xF9FF;

    case SLOT2_PIANO: {
        // The keyboard's keys are at the end of ROM, low when pressed; B and high C skip two bits (from DeSmuME)
        if (address != 0x9FFFFFE) return 0xE7FF;
        uint16_t keys = pianoKeys.load();
        return 0xE7FF & ~((keys & 0x7FF) | ((keys & 0x1800) << 2));
    }

    case SLOT2_PADDLE:
        // The paddle shows up as 0xEFFF everywhere in ROM; its position is in SRAM (from DeSmuME)
        return 0xEFFF;

    default:
        return openBus(arm7, address);
    }
}

void Slot2::write16(uint32_t address, uint16_t value) {
    // Write the GPIO registers, which drive the pins set as outputs
    uint32_t offset = address & 0x1FFFFFF;
    if ((solar || gbaRumble) && offset >= 0xC4 && offset < 0xCA) {
        switch (offset) {
        case 0xC4:
            gpioData = value & 0xF;
            writeGpioPins(gpioData & gpioDirection, gpioDirection);
            return;
        case 0xC6:
            gpioDirection = value & 0xF;
            return;
        case 0xC8:
            gpioControl = value & 0x1;
            return;
        }
    }

    switch (device) {
    case SLOT2_RUMBLE:
        // The Rumble Pak latches address line 1 on writes, and its actuator moves each time that bit changes (GBATEK)
        if ((value ^ rumbleState) & BIT(1))
            rumbleFlips.store(rumbleFlips.load() + 1);
        rumbleState = value;
        return;

    case SLOT2_EXPANSION:
        // Lock or unlock the RAM, mapping it in or out
        if (offset == 0x240000) {
            if (bool(value & BIT(0)) == ramUnlocked) return;
            ramUnlocked = value & BIT(0);
            core->memory.updateMap9(0x9000000, 0x9800000);
            core->memory.updateMap7(0x9000000, 0x9800000);
        }
        return;

    case SLOT2_PADDLE:
        // Writing anything recalibrates the paddle (DeSmuME)
        paddle = 0;
        paddleTurn.exchange(0);
        return;
    }
}

template <typename T> T Slot2::romRead(bool arm7, uint32_t address) {
    // Read from the GBA slot's ROM region, which is 0 for the CPU that doesn't have the slot
    // The bus is 16-bit, so bytes come from a halfword and words are two of them
    if (core->dsiMode) return (T)0xFFFFFFFF;
    if (!owns(arm7)) return 0;
    if (sizeof(T) == 4)
        return read16(arm7, address) | (read16(arm7, address + 2) << 16);
    uint16_t value = read16(arm7, address & ~1);
    return (sizeof(T) == 1) ? (value >> ((address & 1) * 8)) : value;
}

template <typename T> void Slot2::romWrite(bool arm7, uint32_t address, T value) {
    // Write to the GBA slot's ROM region, if this CPU has the slot
    if (core->dsiMode || !owns(arm7)) return;

    // Write the expansion RAM if it got here, which happens when compiled code in it keeps its page unmapped
    if (uint8_t *data = getRam(address)) {
        core->jitWriteCode(data, sizeof(T));
        writeLsb<T>(data, value);
        return;
    }

    // The bus is 16-bit; byte writes are ignored, though they recalibrate the paddle (GBATEK, DeSmuME)
    if (sizeof(T) == 1) {
        if (device == SLOT2_PADDLE) write16(address, value);
        return;
    }
    write16(address, value);
    if (sizeof(T) == 4)
        write16(address + 2, value >> 16);
}

uint8_t Slot2::read8Sram(uint32_t address) {
    // Read the GBA ROM's save memory if there is one, which repeats every 64KB
    if (core->cartridgeGba.getRomSize() > 0)
        return core->cartridgeGba.sramRead(0xE000000 | (address & 0xFFFF));

    switch (device) {
    case SLOT2_GUITAR: {
        // The Guitar Grip's buttons are low when pressed: green, red, yellow and blue in bits 6 to 3
        uint8_t keys = guitarKeys.load();
        return ~(((keys & BIT(0)) << 6) | ((keys & BIT(1)) << 4) | ((keys & BIT(2)) << 2) | (keys & BIT(3)));
    }

    case SLOT2_PADDLE:
        // The paddle's position is 12 bits, which increase turning clockwise
        paddle = (paddle + paddleTurn.exchange(0)) & 0xFFF;
        switch (address & 0xFFFF) {
            case 0: return paddle;
            case 1: return paddle >> 8;
            default: return 0;
        }

    default:
        // Nothing drives the bus, so it reads high (GBATEK)
        return 0xFF;
    }
}

template <typename T> T Slot2::sramRead(bool arm7, uint32_t address) {
    // Read from the GBA slot's SRAM region, which is 0 for the CPU that doesn't have the slot
    // The bus is 8-bit, so wider reads get the byte repeated (DeSmuME found this with the paddle)
    if (core->dsiMode) return 0xFF;
    if (!owns(arm7)) return 0;
    return (T)(read8Sram(address) * 0x01010101);
}

template <typename T> void Slot2::sramWrite(bool arm7, uint32_t address, T value) {
    // Write to the GBA slot's SRAM region, if this CPU has the slot
    if (core->dsiMode || !owns(arm7)) return;
    if (core->cartridgeGba.getRomSize() > 0) {
        core->cartridgeGba.sramWrite(0xE000000 | (address & 0xFFFF), value);
    }
    else if (device == SLOT2_PADDLE) {
        // Writing anything recalibrates the paddle (DeSmuME)
        paddle = 0;
        paddleTurn.exchange(0);
    }
}

void Slot2::writeGpioPins(uint8_t pins, uint8_t direction) {
    // Drive the motor of GBA carts with rumble from pin 3, while it's an output
    if (gbaRumble && (direction & BIT(3)))
        motorOn.store(pins & BIT(3));

    // Drive the solar sensor's counter, unless pin 2 selects the RTC (from melonDS)
    // Pin 1 resets the counter and takes a new light sample; pin 0 counts up on its rising edges
    if (!solar || (pins & BIT(2))) return;
    if (pins & BIT(1)) {
        solarCounter = 0;
        solarSample = 0xFF - (0x16 + solarLux[std::min(10, std::max(0, Settings::sunlight))]);
    }
    if ((pins & BIT(0)) && solarEdge)
        solarCounter++;
    solarEdge = !(pins & BIT(0));
}

uint8_t Slot2::readGpioPins(uint8_t direction) {
    // The solar sensor raises pin 3 once the counter reaches the light sample
    return (solar && !(direction & BIT(3)) && solarCounter >= solarSample) ? BIT(3) : 0;
}

// Instantiate the access functions for each size
template uint8_t Slot2::romRead(bool arm7, uint32_t address);
template uint16_t Slot2::romRead(bool arm7, uint32_t address);
template uint32_t Slot2::romRead(bool arm7, uint32_t address);
template void Slot2::romWrite(bool arm7, uint32_t address, uint8_t value);
template void Slot2::romWrite(bool arm7, uint32_t address, uint16_t value);
template void Slot2::romWrite(bool arm7, uint32_t address, uint32_t value);
template uint8_t Slot2::sramRead(bool arm7, uint32_t address);
template uint16_t Slot2::sramRead(bool arm7, uint32_t address);
template uint32_t Slot2::sramRead(bool arm7, uint32_t address);
template void Slot2::sramWrite(bool arm7, uint32_t address, uint8_t value);
template void Slot2::sramWrite(bool arm7, uint32_t address, uint16_t value);
template void Slot2::sramWrite(bool arm7, uint32_t address, uint32_t value);
