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

void DsiMic::saveState(FILE *file) {
    // Write DSi state data to the file
    if (!core->dsiMode) return;
    fwrite(fifo, 2, sizeof(fifo) / 2, file);
    fwrite(&fifoHead, sizeof(fifoHead), 1, file);
    fwrite(&fifoCount, sizeof(fifoCount), 1, file);
    fwrite(&overrun, sizeof(overrun), 1, file);
    fwrite(&irqSent, sizeof(irqSent), 1, file);
    fwrite(&nextSample, sizeof(nextSample), 1, file);
    fwrite(&nextRight, sizeof(nextRight), 1, file);
    fwrite(&lastSample, sizeof(lastSample), 1, file);
    fwrite(&lastData, sizeof(lastData), 1, file);
    fwrite(&drqHigh, sizeof(drqHigh), 1, file);
    fwrite(&drqEdge, sizeof(drqEdge), 1, file);
    fwrite(&micCnt, sizeof(micCnt), 1, file);
    fwrite(&sndExCnt, sizeof(sndExCnt), 1, file);
}

void DsiMic::loadState(FILE *file) {
    // Read DSi state data from the file
    if (!core->dsiMode) return;
    fread(fifo, 2, sizeof(fifo) / 2, file);
    fread(&fifoHead, sizeof(fifoHead), 1, file);
    fread(&fifoCount, sizeof(fifoCount), 1, file);
    fread(&overrun, sizeof(overrun), 1, file);
    fread(&irqSent, sizeof(irqSent), 1, file);
    fread(&nextSample, sizeof(nextSample), 1, file);
    fread(&nextRight, sizeof(nextRight), 1, file);
    fread(&lastSample, sizeof(lastSample), 1, file);
    fread(&lastData, sizeof(lastData), 1, file);
    fread(&drqHigh, sizeof(drqHigh), 1, file);
    fread(&drqEdge, sizeof(drqEdge), 1, file);
    fread(&micCnt, sizeof(micCnt), 1, file);
    fread(&sndExCnt, sizeof(sndExCnt), 1, file);

    // The scheduler doesn't keep events, so check the FIFO again soon
    updatePending = false;
    requestUpdate(core->globalCycles + 1);
}

bool DsiMic::running() {
    // Samples arrive while both enable bits are set, with a data format, until the FIFO overruns
    return (micCnt & BIT(15)) && (sndExCnt & BIT(15)) && (micCnt & 0x3) != 0x3 && !overrun;
}

uint32_t DsiMic::frameCycles() {
    // Get the cycles in an I2S frame, at 32.73kHz or 47.61kHz
    return (sndExCnt & BIT(13)) ? 704 : 1024;
}

uint32_t DsiMic::periodCycles() {
    // Get the cycles between the frames that are sampled, with the rate divided by 1 to 4
    return frameCycles() * (((micCnt >> 2) & 0x3) + 1);
}

bool DsiMic::takes(bool right) {
    // Format 0 takes both channels' slots, 1 only the right one's and 2 only the left one's
    uint8_t format = micCnt & 0x3;
    return format == 0 || format == (right ? 1 : 2);
}

void DsiMic::nextSlot(uint32_t &time, bool &right) {
    // Move to the next slot the format takes; a frame's right slot comes half a base frame after its left one
    do {
        time += right ? periodCycles() - frameCycles() / 2 : frameCycles() / 2;
        right = !right;
    } while (!takes(right));
}

void DsiMic::start() {
    // The sample clock runs freely, so the first slot is the next one after now, starting from a left one in stereo
    uint32_t period = periodCycles();
    nextRight = (micCnt & 0x3) == 1;
    nextSample = (core->globalCycles / period) * period + (nextRight ? frameCycles() / 2 : 0);
    if (int32_t(nextSample - core->globalCycles) <= 0) nextSample += period;
}

void DsiMic::push(int16_t value) {
    // Add a sample to the FIFO, stopping with an overrun if it's full
    if (fifoCount == 32) {
        overrun = true;
        return;
    }
    fifo[(fifoHead + fifoCount++) & 31] = value;

    // NDMA is asked for a block when the FIFO becomes half full, and not again until it has been empty
    if (fifoCount == 16 && !drqHigh)
        drqHigh = drqEdge = true;
}

void DsiMic::catchUp() {
    // Add the samples that arrived since the last check
    if (!running()) return;
    while (int32_t(core->globalCycles - nextSample) >= 0 && !overrun) {
        // In stereo, the right slot repeats the left one's sample
        if (!nextRight || (micCnt & 0x3) != 0)
            lastSample = core->spi.readMic(nextSample);
        push(lastSample);
        nextSlot(nextSample, nextRight);
    }
}

uint32_t DsiMic::thresholdTime() {
    // Get when the FIFO next reaches half full, full or an overrun, or 0 if it won't
    if (!running()) return 0;
    uint32_t time = nextSample;
    bool right = nextRight;
    uint8_t target = (fifoCount < 16) ? 16 : (fifoCount < 32) ? 32 : 33;
    for (uint8_t count = fifoCount + 1; count < target; count++)
        nextSlot(time, right);
    return time;
}

void DsiMic::requestUpdate(uint32_t time) {
    // Schedule an update unless one is already due by then; extra events just check again
    int32_t delay = time - core->globalCycles;
    if (updatePending && int32_t(time - updateAt) >= 0) return;
    core->schedule(MIC_UPDATE, delay > 0 ? delay : 1);
    updatePending = true;
    updateAt = time;
}

void DsiMic::update() {
    // Bring the FIFO up to date
    if (updatePending && int32_t(core->globalCycles - updateAt) >= 0)
        updatePending = false;
    catchUp();

    // Start NDMA when the FIFO has become half full
    if (drqEdge) {
        core->ndma[1].setDrq(0xC);
        drqEdge = false;
    }

    // Send an interrupt when the FIFO reaches half full, or in mode 2 when it overruns
    uint8_t mode = (micCnt >> 13) & 0x3;
    if (mode && ((mode == 2) ? overrun : (fifoCount / 2 >= 8))) {
        if (!irqSent) core->interpreter[1].sendInterrupt(32 + 14);
        irqSent = true;
    }
    else if (mode != 2) {
        irqSent = false;
    }

    // Check again when the FIFO reaches the next level
    if (uint32_t time = thresholdTime())
        requestUpdate(time);
}

void DsiMic::resetCycles() {
    // Adjust the sample and update times for the global reset
    nextSample -= core->globalCycles;
    updateAt -= core->globalCycles;
}

uint16_t DsiMic::readMicCnt() {
    // Read from the MIC_CNT register, with the FIFO's status in bits 8-11
    catchUp();
    uint8_t words = fifoCount / 2;
    return micCnt | ((words == 0) << 8) | ((words >= 8) << 9) | ((words == 16) << 10) | (overrun << 11);
}

uint32_t DsiMic::readMicData() {
    // Read a word from the FIFO, with the older sample in the low half; when it's empty, the last word read comes again,
    // even after the FIFO is cleared
    catchUp();
    if (fifoCount < 2) return lastData;
    uint32_t value = lastData = uint16_t(fifo[fifoHead]) | (uint16_t(fifo[(fifoHead + 1) & 31]) << 16);
    fifoHead = (fifoHead + 2) & 31;
    fifoCount -= 2;

    // Drop the NDMA request once the FIFO is empty, and allow another half full interrupt below that level
    uint8_t words = fifoCount / 2;
    if (words == 0) {
        drqHigh = drqEdge = false;
        core->ndma[1].clearDrq(0xC);
    }
    if (words < 8 && ((micCnt >> 13) & 0x3) != 2) irqSent = false;
    if (uint32_t time = thresholdTime())
        requestUpdate(time);
    return value;
}

void DsiMic::writeMicCnt(uint16_t mask, uint16_t value) {
    // Write to the MIC_CNT register, after adding the samples from before the change
    catchUp();
    bool wasRunning = running();
    bool wasEnabled = micCnt & BIT(15);
    micCnt = (micCnt & ~(mask & 0xE00F)) | (value & mask & 0xE00F);

    // Clear the FIFO and its overrun, which only works if the microphone was disabled
    if ((value & mask & BIT(12)) && !wasEnabled) {
        fifoHead = fifoCount = 0;
        overrun = irqSent = drqHigh = drqEdge = false;
        core->ndma[1].clearDrq(0xC);
    }

    // Start sampling if the microphone just turned on, and check the FIFO with the new settings
    if (!wasRunning && running())
        start();
    requestUpdate(core->globalCycles + 1);
}

void DsiMic::writeSndExCnt(uint16_t mask, uint16_t value) {
    // Write to the SNDEXCNT register, where the I2S frequency can only change while bit 15 is clear
    catchUp();
    bool wasRunning = running();
    mask &= (sndExCnt & BIT(15)) ? 0xC00F : 0xE00F;
    sndExCnt = (sndExCnt & ~mask) | (value & mask);

    // Start sampling if the microphone just turned on
    if (!wasRunning && running())
        start();
    requestUpdate(core->globalCycles + 1);
}
