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

#include <algorithm>
#include <cstring>
#include <mutex>
#include "../core.h"

// Timing from melonDS: a frame starts every 1/15 second with the camera interrupt, and its scanlines arrive later,
// 3173 cycles apart, ending just before the next one
static const uint32_t FRAME_CYCLES = 2234248;
static const uint32_t LINE_CYCLES = 3173;
static const uint32_t TRANSFER_START = FRAME_CYCLES - LINE_CYCLES * DsiCamera::HEIGHT;

std::atomic<bool> DsiCamera::streaming{false};

// The latest picture from the frontend, and whether one ever came; until then, the cameras see color bars
static std::mutex inputMutex;
static uint32_t inputFrame[DsiCamera::WIDTH * DsiCamera::HEIGHT / 2];
static bool inputReceived = false;

void DsiCamera::sendFrame(const uint32_t *frame) {
    // Keep a copy of a picture for the next frame a sensor starts
    std::lock_guard<std::mutex> guard(inputMutex);
    memcpy(inputFrame, frame, sizeof(inputFrame));
    inputReceived = true;
}

void DsiCamera::Sensor::reset() {
    // Hold a sensor in reset with its registers at their defaults, in preview mode
    *this = Sensor();
    mcuVars[0x2104] = 3;
}

void DsiCamera::saveState(FILE *file) {
    // Write DSi state data to the file
    if (!core->dsiMode) return;
    fwrite(sensors, sizeof(sensors), 1, file);
    fwrite(&selected, sizeof(selected), 1, file);
    fwrite(&mcnt, sizeof(mcnt), 1, file);
    fwrite(&cnt, sizeof(cnt), 1, file);
    fwrite(&sofs, sizeof(sofs), 1, file);
    fwrite(&eofs, sizeof(eofs), 1, file);
    fwrite(&transferring, sizeof(transferring), 1, file);
    fwrite(buffers, sizeof(buffers), 1, file);
    fwrite(readPos, sizeof(readPos), 1, file);
    fwrite(writePos, sizeof(writePos), 1, file);
    fwrite(&curBuffer, sizeof(curBuffer), 1, file);
    fwrite(&bufferLines, sizeof(bufferLines), 1, file);
    fwrite(&sending, sizeof(sending), 1, file);
    fwrite(&frameWidth, sizeof(frameWidth), 1, file);
    fwrite(&frameHeight, sizeof(frameHeight), 1, file);
    fwrite(&readMode, sizeof(readMode), 1, file);
    fwrite(&line, sizeof(line), 1, file);
    fwrite(&sensorY, sizeof(sensorY), 1, file);
    fwrite(&frameY, sizeof(frameY), 1, file);
    fwrite(&barLine, sizeof(barLine), 1, file);
}

void DsiCamera::loadState(FILE *file) {
    // Read DSi state data from the file; the picture being sent isn't kept, so the rest of the frame comes from the
    // latest one
    if (!core->dsiMode) return;
    fread(sensors, sizeof(sensors), 1, file);
    fread(&selected, sizeof(selected), 1, file);
    fread(&mcnt, sizeof(mcnt), 1, file);
    fread(&cnt, sizeof(cnt), 1, file);
    fread(&sofs, sizeof(sofs), 1, file);
    fread(&eofs, sizeof(eofs), 1, file);
    fread(&transferring, sizeof(transferring), 1, file);
    fread(buffers, sizeof(buffers), 1, file);
    fread(readPos, sizeof(readPos), 1, file);
    fread(writePos, sizeof(writePos), 1, file);
    fread(&curBuffer, sizeof(curBuffer), 1, file);
    fread(&bufferLines, sizeof(bufferLines), 1, file);
    fread(&sending, sizeof(sending), 1, file);
    fread(&frameWidth, sizeof(frameWidth), 1, file);
    fread(&frameHeight, sizeof(frameHeight), 1, file);
    fread(&readMode, sizeof(readMode), 1, file);
    fread(&line, sizeof(line), 1, file);
    fread(&sensorY, sizeof(sensorY), 1, file);
    fread(&frameY, sizeof(frameY), 1, file);
    fread(&barLine, sizeof(barLine), 1, file);
    if (sending >= 0) {
        std::lock_guard<std::mutex> guard(inputMutex);
        if (inputReceived) memcpy(frame, inputFrame, sizeof(frame));
    }
    updateStreaming();
}

void DsiCamera::start() {
    // Start the sensors in reset, and the frames, which run whether a camera is on or not
    for (int i = 0; i < 2; i++)
        sensors[i].reset();
    updateStreaming();
    core->schedule(CAM_FRAME, FRAME_CYCLES);
}

void DsiCamera::updateStreaming() {
    // Let the frontend know whether pictures are wanted
    streaming.store(sensors[0].active() || sensors[1].active());
}

void DsiCamera::frameStart() {
    // Start a frame from the first sensor sending pictures; with both on, a real DSi would mix their data
    core->schedule(CAM_FRAME, FRAME_CYCLES);
    sending = sensors[0].active() ? 0 : (sensors[1].active() ? 1 : -1);
    if (sending < 0) return;

    // Take the sensor's size and read mode for its current mode, preview or capture
    Sensor &sensor = sensors[sending];
    bool capture = (sensor.mcuVars[0x2104] == 7);
    bool preview = (sensor.mcuVars[0x2104] == 3);
    frameWidth = (capture || preview) ? sensor.mcuVar16(capture ? 0x2707 : 0x2703) : 0;
    frameHeight = (capture || preview) ? sensor.mcuVar16(capture ? 0x2709 : 0x2705) : 0;
    readMode = (capture || preview) ? sensor.mcuVar16(capture ? 0x272D : 0x2717) : 0;
    sensorY = frameY = 0;

    // Take the latest picture, or draw color bars with a line moving down them if there isn't one
    bool received;
    {
        std::lock_guard<std::mutex> guard(inputMutex);
        if ((received = inputReceived)) memcpy(frame, inputFrame, sizeof(frame));
    }
    if (!received) {
        // White, yellow, cyan, green, magenta, red, blue and black, as BT.601 YUV
        static const uint32_t bars[8] = { 0x80EB80EB, 0x92D210D2, 0x10AAA6AA, 0x22913691, 0xDE6ACA6A, 0xF0515A51,
            0x6E29F029, 0x80108010 };
        barLine = (barLine + 4) % HEIGHT;
        for (int y = 0; y < HEIGHT; y++)
            for (int x = 0; x < WIDTH / 2; x++)
                frame[y * WIDTH / 2 + x] = (y >= barLine && y < barLine + 8) ? 0x80EB80EB : bars[x * 8 / (WIDTH / 2)];
    }

    // Send the camera interrupt and the frame's first scanline later
    if (cnt & BIT(11)) core->interpreter[0].sendInterrupt(25);
    line = 0;
    core->schedule(CAM_LINE, TRANSFER_START);
}

int DsiCamera::sensorLine(uint32_t *out, int &lines) {
    // Scale the next scanline from the sensor's full picture, in words of two pixels, and count the sensor's lines
    // until the one after it
    lines = 0;
    if (frameY >= frameHeight || sensorY >= HEIGHT) return 0;
    if (frameWidth > WIDTH || frameHeight > HEIGHT || frameWidth < 2 || frameHeight < 2 || (frameWidth & 1)) return 0;
    int words = frameWidth >> 1;
    int y = (readMode & BIT(1)) ? (HEIGHT - 1 - sensorY) : sensorY;
    for (int x = 0; x < words; x++) {
        // Without read mode bit 0, the picture is mirrored, which swaps each word's two pixels too
        if (readMode & BIT(0)) {
            out[x] = frame[y * WIDTH / 2 + x * WIDTH / frameWidth];
        }
        else {
            uint32_t value = frame[y * WIDTH / 2 + (WIDTH / 2 - 1) - x * WIDTH / frameWidth];
            out[x] = (value & 0xFF00FF00) | ((value >> 16) & 0xFF) | ((value & 0xFF) << 16);
        }
    }
    int last = frameY;
    do {
        sensorY++;
        frameY = sensorY * frameHeight / HEIGHT;
        lines++;
    }
    while (frameY == last && sensorY < HEIGHT);
    return words;
}

void DsiCamera::transferLine() {
    // Stop after an overrun, and only start a frame's transfer if it's enabled
    if (cnt & BIT(4)) {
        transferring = false;
        return;
    }
    if (line == 0) {
        if (~cnt & BIT(15)) return;
        bufferLines = 0;
        transferring = true;
    }

    // Take the next scanline from the sensor
    uint32_t data[512];
    int lines;
    int words = sensorLine(data, lines);
    int first = 0, count = words;
    bool lastLine = false;

    // Keep only the cropped part of the picture, if trimming is on
    bool skip = false;
    if (cnt & BIT(14)) {
        int y0 = (sofs >> 16) & 0x1FF, y1 = (eofs >> 16) & 0x1FF;
        if (line < y0 || line > y1) {
            lastLine = (line == y1 + 1);
            skip = true;
        }
        first = int((sofs >> 1) & 0x1FF);
        count = std::max(0, std::min(words, int((eofs >> 1) & 0x1FF) + 1) - first);
    }

    if (!skip) {
        // Add the scanline to the filling buffer, converted to RGB555 if asked
        uint32_t *dst = &buffers[curBuffer][writePos[curBuffer]];
        count = std::min(count, 512 - writePos[curBuffer]);
        for (int i = 0; i < count; i++) {
            uint32_t value = data[first + i];
            if (cnt & BIT(13)) {
                int u = int((value >> 8) & 0xFF) - 128, v = int(value >> 24) - 128;
                uint32_t rgb[2];
                for (int p = 0; p < 2; p++) {
                    int y = (value >> (p * 16)) & 0xFF;
                    int r = std::clamp(y + ((v * 91881) >> 16), 0, 255);
                    int g = std::clamp(y - ((v * 46793) >> 16) - ((u * 22544) >> 16), 0, 255);
                    int b = std::clamp(y + ((u * 116129) >> 16), 0, 255);
                    rgb[p] = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | BIT(15);
                }
                value = rgb[0] | (rgb[1] << 16);
            }
            dst[i] = value;
        }
        writePos[curBuffer] += count;

        // Hand the buffer to NDMA every few scanlines, as many as CAM_CNT asks for
        if (bufferLines >= (cnt & 0xF)) {
            bufferLines = 0;
            swapBuffers();
        }
        else {
            bufferLines++;
        }
    }

    // At the end of the frame or the cropped part, hand over a partly filled buffer too
    bool done = (frameY >= frameHeight);
    if ((done || lastLine) && bufferLines > 0) {
        bufferLines = 0;
        swapBuffers();
    }
    if (done) {
        transferring = false;
        return;
    }
    line++;
    core->schedule(CAM_LINE, std::max(1, lines) * LINE_CYCLES);
}

void DsiCamera::swapBuffers() {
    // Give the filled buffer to NDMA, or flag an overrun if the other one wasn't emptied yet
    int other = curBuffer ^ 1;
    if (readPos[other] < writePos[other]) {
        cnt |= BIT(4);
        transferring = false;
        return;
    }
    readPos[curBuffer] = 0;
    writePos[other] = 0;
    curBuffer = other;
    core->ndma[0].trigger(0xB);
}

uint32_t DsiCamera::readData() {
    // Read two pixels from the buffer given to NDMA; past its end, the last word is read again
    int buf = curBuffer ^ 1;
    if (readPos[buf] < writePos[buf]) return buffers[buf][readPos[buf]++];
    return buffers[buf][readPos[buf] ? (readPos[buf] - 1) : 0];
}

void DsiCamera::writeMcnt(uint16_t mask, uint16_t value) {
    // Write to CAM_MCNT, where clearing bit 5 holds the sensors in reset and setting it releases them; it can't change
    // while transferring
    if ((cnt & BIT(15)) || transferring) return;
    uint16_t old = mcnt;
    mcnt = (mcnt & ~mask) | (value & mask & 0xFF7F);
    if ((old & BIT(5)) && !(mcnt & BIT(5))) {
        for (int i = 0; i < 2; i++)
            sensors[i].reset();
        updateStreaming();
    }
    else if (!(old & BIT(5)) && (mcnt & BIT(5))) {
        for (int i = 0; i < 2; i++)
            sensors[i].resetHeld = false;
    }
}

void DsiCamera::writeCnt(uint16_t mask, uint16_t value) {
    // Write to CAM_CNT; while transferring, only the enable, IRQ and flush bits can change
    bool busy = (cnt & BIT(15)) || transferring;
    uint16_t writable = (busy ? 0x8F20 : 0xEF2F) & mask;
    cnt = (cnt & ~writable) | (value & writable & ~BIT(5));

    // Bit 5 clears an overrun and empties the buffers
    if (value & mask & BIT(5)) {
        cnt &= ~BIT(4);
        memset(buffers, 0, sizeof(buffers));
        memset(readPos, 0, sizeof(readPos));
        memset(writePos, 0, sizeof(writePos));
        curBuffer = 0;
    }
}

void DsiCamera::writeSofs(uint32_t mask, uint32_t value) {
    // Write to CAM_SOFS, the crop window's start
    if ((cnt & BIT(15)) || transferring) return;
    mask &= 0x01FF03FE;
    sofs = (sofs & ~mask) | (value & mask);
}

void DsiCamera::writeEofs(uint32_t mask, uint32_t value) {
    // Write to CAM_EOFS, the crop window's end
    if ((cnt & BIT(15)) || transferring) return;
    mask &= 0x01FF03FE;
    eofs = (eofs & ~mask) | (value & mask);
}

bool DsiCamera::i2cSelect(uint8_t device) {
    // Address a sensor at the start of an I2C transfer; both answer, even in reset
    selected = (device == 0x78) ? 0 : ((device == 0x7A) ? 1 : -1);
    if (selected < 0) return false;
    sensors[selected].dataPos = 0;
    return true;
}

uint8_t DsiCamera::i2cRead(bool last) {
    // Read a 16-bit register a byte at a time, high byte first, moving to the next register after the low one
    if (selected < 0 || sensors[selected].resetHeld) return 0xFF;
    Sensor &sensor = sensors[selected];
    uint8_t value;
    if (sensor.dataPos & 0x1) {
        value = sensor.regData;
        sensor.regAddr += 2;
    }
    else {
        sensor.regData = sensor.readReg(sensor.regAddr);
        value = sensor.regData >> 8;
    }
    sensor.dataPos = last ? 0 : (sensor.dataPos + 1);
    return value;
}

void DsiCamera::i2cWrite(uint8_t value, bool last) {
    // Take a 16-bit register address, then write 16-bit values to it and the registers after it, high byte first
    if (selected < 0 || sensors[selected].resetHeld) return;
    Sensor &sensor = sensors[selected];
    if (sensor.dataPos < 2) {
        sensor.regAddr = sensor.dataPos ? (sensor.regAddr | value) : (value << 8);
    }
    else if (sensor.dataPos & 0x1) {
        sensor.regData |= value;
        sensor.writeReg(sensor.regAddr, sensor.regData);
        sensor.regAddr += 2;
        updateStreaming();
    }
    else {
        sensor.regData = value << 8;
    }
    sensor.dataPos = last ? 0 : (sensor.dataPos + 1);
}

uint16_t DsiCamera::Sensor::readReg(uint16_t addr) const {
    // Read a sensor register, or MCU variables through the window at 0x0990, as bytes unless bit 15 of the address
    // register is set
    switch (addr) {
        case 0x0000: return 0x2280; // Chip ID
        case 0x0010: return pllDiv;
        case 0x0012: return pllPDiv;
        case 0x0014: return pllCnt;
        case 0x0016: return clocksCnt;
        case 0x0018: return standbyCnt;
        case 0x001A: return miscCnt;
        case 0x098C: return mcuAddr;
        case 0x301A: return (~standbyCnt & 0x4000) >> 12;

    default:
        if (addr >= 0x0990 && addr < 0x09A0) {
            uint16_t var = (mcuAddr & 0x7FFF) + (addr - 0x0990);
            uint16_t value = mcuVars[var & 0x7FFF];
            if (!(mcuAddr & BIT(15))) value |= mcuVars[(var + 1) & 0x7FFF] << 8;
            return value;
        }
        LOG_INFO("Unknown camera register read: 0x%X\n", addr);
        return 0;
    }
}

void DsiCamera::Sensor::writeReg(uint16_t addr, uint16_t value) {
    // Write a sensor register, or MCU variables through the window at 0x0990
    switch (addr) {
        case 0x0010: pllDiv = value & 0x3FFF; return;
        case 0x0012: pllPDiv = value & 0xBFFF; return;
        case 0x0014: pllCnt = (value & 0x7FFF) | ((value & 0x2) << 14); return; // Locks right away
        case 0x0016: clocksCnt = value; return;
        case 0x0018: standbyCnt = (value & 0x3F) | ((value & 0x1) << 14); return; // Enters or leaves standby right away
        case 0x001A: miscCnt = value & 0x0B7B; return;
        case 0x098C: mcuAddr = value; return;

    default:
        if (addr >= 0x0990 && addr < 0x09A0) {
            uint16_t var = (mcuAddr & 0x7FFF) + (addr - 0x0990);
            for (int i = 0; i < ((mcuAddr & BIT(15)) ? 1 : 2); i++, var++) {
                uint8_t byte = value >> (i * 8);
                switch (var & 0x7FFF) {
                case 0x2103: // SEQ_CMD, which switches to capture with 2 and preview with 1, and reads back as done
                    if (byte == 2) mcuVars[0x2104] = 7;
                    else if (byte == 1) mcuVars[0x2104] = 3;
                    mcuVars[0x2103] = 0;
                    break;
                case 0x2104: // SEQ_STATE, read-only
                    break;
                default:
                    mcuVars[var & 0x7FFF] = byte;
                    break;
                }
            }
            return;
        }
        LOG_INFO("Unknown camera register write: 0x%X = 0x%X\n", addr, value);
        return;
    }
}
