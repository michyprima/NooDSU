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

#include <algorithm>

#include "../core.h"
#include "../faad2/neaacdec.h"
#include "dsi_dsp_hle.h"

static uint32_t crc32(const uint8_t *data, uint32_t size, uint32_t crc) {
    // Add data to a CRC32, the zlib kind; no data counts as zeros
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t value = i;
            for (int j = 0; j < 8; j++)
                value = (value >> 1) ^ ((value & 1) ? 0xEDB88320 : 0);
            table[i] = value;
        }
    }
    crc = ~crc;
    for (uint32_t i = 0; i < size; i++)
        crc = (crc >> 8) ^ table[(crc ^ (data ? data[i] : 0)) & 0xFF];
    return ~crc;
}

DspUcode *DspUcode::create(Core *core, DsiDsp *dsp, uint8_t **pages) {
    // Hash the program memory as the DSP sees it, and make the matching program if it's one of Nintendo's
    uint32_t crc = 0;
    for (int i = 0; i < 8; i++)
        crc = crc32(pages[i], 0x8000, crc);

    static const struct { uint32_t crc; uint8_t kind; int8_t version; } known[] = {
        { 0x7867C94B, AAC, -1 }, // DSi Sound
        { 0x0CAFEF48, AAC, 0x00 }, { 0xEF5174AA, AAC, 0x01 }, { 0x1D320185, AAC, 0x20 }, { 0xAE11D2FB, AAC, 0x40 },
        { 0xFAA1B612, G711, 0x00 }, { 0x7EEE19FE, G711, 0x10 }, { 0x6056C6FF, G711, 0x20 },
        { 0x2C281DAE, G711, 0x30 }, { 0x2A1D7F94, G711, 0x40 }, { 0x4EBEB519, G711, 0x50 },
        { 0xCD2A8B1B, GRAPHICS, 0x00 }, { 0x7323B75B, GRAPHICS, 0x10 }, { 0xBD4B63B6, GRAPHICS, 0x11 },
        { 0x448BB6A2, GRAPHICS, 0x20 }, { 0x63CAEC33, GRAPHICS, 0x30 }, { 0x1451EB84, GRAPHICS, 0x40 },
        { 0x2C974FC8, GRAPHICS, 0x50 }
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (known[i].crc != crc) continue;
        LOG_INFO("DSP program %08X is done in high level\n", crc);
        return create(core, dsp, pages, (known[i].kind << 16) | (uint16_t)known[i].version);
    }
    LOG_INFO("DSP program %08X isn't known, so it's run by Teakra\n", crc);
    return nullptr;
}

DspUcode *DspUcode::create(Core *core, DsiDsp *dsp, uint8_t **pages, uint32_t id) {
    // Make a program from what getId gave
    int version = (int16_t)id;
    switch (id >> 16) {
        case AAC: return new AacUcode(core, dsp, pages, version);
        case G711: return new G711Ucode(core, dsp, pages, version);
        case GRAPHICS: return new GraphicsUcode(core, dsp, pages, version);
    }
    return nullptr;
}

void DspUcode::saveState(FILE *file) {
    // Write the state shared by all programs to the file
    fwrite(&exited, sizeof(exited), 1, file);
    fwrite(cmd, 2, 3, file);
    fwrite(cmdWritten, sizeof(bool), 3, file);
    fwrite(reply, 2, 3, file);
    fwrite(replyWritten, sizeof(bool), 3, file);
    fwrite(&replyAction, sizeof(replyAction), 1, file);
    fwrite(&replyParam, sizeof(replyParam), 1, file);
    fwrite(&semIn, sizeof(semIn), 1, file);
    fwrite(&semOut, sizeof(semOut), 1, file);
    fwrite(&semMask, sizeof(semMask), 1, file);
    fwrite(&playing, sizeof(playing), 1, file);
    fwrite(&playHalved, sizeof(playHalved), 1, file);
    fwrite(&playAddress, sizeof(playAddress), 1, file);
    fwrite(&playLength, sizeof(playLength), 1, file);
    fwrite(playFifo, 2, 8, file);
    fwrite(&playHead, sizeof(playHead), 1, file);
    fwrite(&playCount, sizeof(playCount), 1, file);
    fwrite(&micSampling, sizeof(micSampling), 1, file);
    fwrite(micFifo, 2, 8, file);
    fwrite(&micCount, sizeof(micCount), 1, file);
    fwrite(&finishPending, sizeof(finishPending), 1, file);
    fwrite(&finishCycles, sizeof(finishCycles), 1, file);
    fwrite(&finishParam, sizeof(finishParam), 1, file);
}

void DspUcode::loadState(FILE *file) {
    // Read the state shared by all programs from the file
    fread(&exited, sizeof(exited), 1, file);
    fread(cmd, 2, 3, file);
    fread(cmdWritten, sizeof(bool), 3, file);
    fread(reply, 2, 3, file);
    fread(replyWritten, sizeof(bool), 3, file);
    fread(&replyAction, sizeof(replyAction), 1, file);
    fread(&replyParam, sizeof(replyParam), 1, file);
    fread(&semIn, sizeof(semIn), 1, file);
    fread(&semOut, sizeof(semOut), 1, file);
    fread(&semMask, sizeof(semMask), 1, file);
    fread(&playing, sizeof(playing), 1, file);
    fread(&playHalved, sizeof(playHalved), 1, file);
    fread(&playAddress, sizeof(playAddress), 1, file);
    fread(&playLength, sizeof(playLength), 1, file);
    fread(playFifo, 2, 8, file);
    fread(&playHead, sizeof(playHead), 1, file);
    fread(&playCount, sizeof(playCount), 1, file);
    fread(&micSampling, sizeof(micSampling), 1, file);
    fread(micFifo, 2, 8, file);
    fread(&micCount, sizeof(micCount), 1, file);
    fread(&finishPending, sizeof(finishPending), 1, file);
    fread(&finishCycles, sizeof(finishCycles), 1, file);
    fread(&finishParam, sizeof(finishParam), 1, file);
}

uint16_t DspUcode::readData(uint32_t address) {
    // Read a word of data memory, which is new WRAM C, stored little-endian
    uint8_t *page = pages[8 + ((address >> 14) & 0x7)];
    return page ? readLsb<uint16_t>(&page[(address << 1) & 0x7FFF]) : 0;
}

void DspUcode::writeData(uint32_t address, uint16_t value) {
    // Write a word of data memory
    uint8_t *page = pages[8 + ((address >> 14) & 0x7)];
    if (page) writeLsb<uint16_t>(&page[(address << 1) & 0x7FFF], value);
}

uint16_t DspUcode::readProgram(uint32_t address) {
    // Read a word of program memory, which is new WRAM B
    uint8_t *page = pages[(address >> 14) & 0x7];
    return page ? readLsb<uint16_t>(&page[(address << 1) & 0x7FFF]) : 0;
}

void DspUcode::writeProgram(uint32_t address, uint16_t value) {
    // Write a word of program memory
    uint8_t *page = pages[(address >> 14) & 0x7];
    if (page) writeLsb<uint16_t>(&page[(address << 1) & 0x7FFF], value);
}

void DspUcode::start() {
    // Set up the 16 pipes the ARM9 and the program talk through, each with a 512-byte buffer
    for (int i = 0; i < 16; i++) {
        uint32_t pipe = PIPE_MONITOR + i * 5;
        writeData(pipe + 0, PIPE_BUFFERS + 0x100 * i); // Buffer address
        writeData(pipe + 1, 0x200); // Length in bytes
        writeData(pipe + 2, 0); // Read pointer
        writeData(pipe + 3, 0); // Write pointer
        writeData(pipe + 4, i); // Pipe index
    }

    // Say the program is up, then where the pipes are once those replies are read
    sendReply(0, 1);
    sendReply(1, 1);
    sendReply(2, 1);
    setReplyAction(1, 0);
}

void DspUcode::scheduleFinish(uint32_t cycles, uint16_t param) {
    // Finish the running command after the time it would take the real program
    finishPending = true;
    finishCycles = core->globalCycles + cycles;
    finishParam = param;
    core->schedule(DSP_HLE_FINISH, cycles);
}

void DspUcode::finish() {
    // Finish the running command if it's due; events from a program that was replaced come early or not at all
    if (!finishPending || (int32_t)(core->globalCycles - finishCycles) < 0) return;
    finishPending = false;
    finishCmd(finishParam);
}

void DspUcode::resetCycles() {
    // Adjust the finish time for the global reset
    finishCycles -= core->globalCycles;
}

uint16_t DspUcode::readReply(int i) {
    // Read a reply, then do what was waiting on it being read
    if (!replyWritten[i]) return 0;
    replyWritten[i] = false;
    uint16_t value = reply[i];
    if (i == 2 && replyAction) {
        replyRead();
        replyAction = 0;
    }
    return value;
}

void DspUcode::writeCmd(int i, uint16_t value) {
    // Take a command word, unless one is already waiting
    if (exited) return;
    if (cmdWritten[i]) {
        LOG_WARN("DSP command %d written while one was waiting\n", i);
        return;
    }
    cmd[i] = value;
    cmdWritten[i] = true;
    if (i != 2) return;

    // CMD2 stops the program, or says the ARM9 put a sound or microphone command in pipe 5
    if (value == 0x8000) {
        sendReply(2, 0x8000);
        exited = true;
    }
    else if (value == 5) {
        startAudioCmd();
    }
    cmdWritten[2] = false;
}

void DspUcode::sendReply(int i, uint16_t value) {
    // Send a reply to the ARM9, which can interrupt it
    if (replyWritten[i]) {
        LOG_WARN("DSP reply %d sent while one was waiting\n", i);
        return;
    }
    reply[i] = value;
    replyWritten[i] = true;
    dsp->replyIrq(i);
}

void DspUcode::setReplyAction(uint8_t action, uint32_t param) {
    // Do something once REP2 is read, or now if it already was
    replyAction = action;
    replyParam = param;
    if (replyWritten[2]) return;
    replyRead();
    replyAction = 0;
}

void DspUcode::replyRead() {
    // Do what was waiting on REP2 being read
    switch (replyAction) {
        case 1: // Finish starting up by sending where the pipes are
            sendReply(2, PIPE_MONITOR);
            setSemaphoreOut(0x8000);
            return;

        case 2: { // Say a sound finished playing
            uint16_t response[4] = { 0x0000, 0x1200, (uint16_t)(replyParam >> 16), (uint16_t)replyParam };
            writePipe(4, response, 4);
            return;
        }

        case 3: { // Answer a microphone command with where its buffer is
            uint16_t response[4] = { (uint16_t)(replyParam >> 16), (uint16_t)replyParam, 0, MIC_BUFFER };
            writePipe(4, response, 4);
            return;
        }
    }
}

void DspUcode::setSemaphoreOut(uint16_t value) {
    // Set semaphore bits for the ARM9, interrupting it if any aren't masked
    semOut |= value;
    if (semOut & ~semMask)
        dsp->semaphoreIrq();
}

uint32_t DspUcode::pipeLength(int pipe) {
    // Get how many words are waiting in a pipe; the top bit of each pointer flips every time it wraps
    uint32_t address = PIPE_MONITOR + pipe * 5;
    uint16_t read = readData(address + 2), write = readData(address + 3);
    uint32_t length = (write & 0x7FFF) - (read & 0x7FFF);
    if ((read ^ write) & 0x8000) length += readData(address + 1);
    return length >> 1;
}

uint32_t DspUcode::readPipe(int pipe, uint16_t *data, uint32_t length) {
    // Take words from a pipe, then tell the ARM9 which pipe moved
    uint32_t address = PIPE_MONITOR + pipe * 5;
    uint16_t buffer = readData(address), size = readData(address + 1) >> 1;
    uint16_t read = (readData(address + 2) & 0x7FFF) >> 1, phase = readData(address + 2) & 0x8000;
    uint16_t write = (readData(address + 3) & 0x7FFF) >> 1;
    uint32_t count = 0;
    while (count < length) {
        data[count++] = readData(buffer + read++);
        if (read >= size) {
            read = 0;
            phase ^= 0x8000;
        }
        if (read == write) break;
    }
    writeData(address + 2, (read << 1) | phase);
    sendReply(2, readData(address + 4));
    setSemaphoreOut(0x8000);
    return count;
}

uint32_t DspUcode::writePipe(int pipe, const uint16_t *data, uint32_t length) {
    // Put words in a pipe, then tell the ARM9 which pipe moved
    uint32_t address = PIPE_MONITOR + pipe * 5;
    uint16_t buffer = readData(address), size = readData(address + 1) >> 1;
    uint16_t read = (readData(address + 2) & 0x7FFF) >> 1;
    uint16_t write = (readData(address + 3) & 0x7FFF) >> 1, phase = readData(address + 3) & 0x8000;
    uint32_t count = 0;
    while (count < length) {
        writeData(buffer + write++, data[count++]);
        if (write >= size) {
            write = 0;
            phase ^= 0x8000;
        }
        if (write == read) {
            LOG_WARN("DSP pipe %d is full\n", pipe);
            break;
        }
    }
    writeData(address + 3, (write << 1) | phase);
    sendReply(2, readData(address + 4));
    setSemaphoreOut(0x8000);
    return count;
}

void DspUcode::readArm9(uint16_t *data, uint32_t address, uint32_t size) {
    // Read bytes of the ARM9's memory as words, a word at a time where it's aligned
    if (address & 2) {
        *data++ = core->memory.read<uint16_t>(0, address);
        address += 2;
        size -= 2;
    }
    for (; size >= 4; size -= 4, address += 4) {
        uint32_t value = core->memory.read<uint32_t>(0, address);
        *data++ = value;
        *data++ = value >> 16;
    }
    if (size) *data = core->memory.read<uint16_t>(0, address);
}

void DspUcode::writeArm9(const uint16_t *data, uint32_t address, uint32_t size) {
    // Write words to the ARM9's memory, a word at a time where it's aligned
    if (address & 2) {
        core->memory.write<uint16_t>(0, address, *data++);
        address += 2;
        size -= 2;
    }
    for (; size >= 4; size -= 4, address += 4, data += 2)
        core->memory.write<uint32_t>(0, address, data[0] | (data[1] << 16));
    if (size) core->memory.write<uint16_t>(0, address, *data);
}

void DspUcode::startAudioCmd() {
    // Run a sound or microphone command once all 8 words of it are in pipe 5
    if (pipeLength(5) < 8) return;
    uint16_t params[8];
    readPipe(5, params, 8);
    uint32_t command = (params[0] << 16) | params[1];
    uint32_t address = (params[2] << 16) | params[3];
    uint32_t length = (params[4] << 16) | params[5];
    uint8_t type = (command >> 12) & 0xF, action = (command >> 8) & 0xF;

    if (type == 1 && action == 1) { // Play a sound
        playHalved = command & BIT(1);
        playAddress = address;
        playLength = length;
        playing = true;
        if (!playCount) advancePlay();
    }
    else if (type == 2) {
        if (action == 1) { // Start sampling the microphone
            micSampling = true;
            micCount = 0;
        }
        else if (action == 2) { // Stop sampling the microphone
            micSampling = false;
        }

        // Starting or stopping clears the microphone buffer: its address, length and write position, then samples
        if (action == 1 || action == 2) {
            writeData(MIC_BUFFER + 0, MIC_BUFFER + 3);
            writeData(MIC_BUFFER + 1, 0x1000);
            writeData(MIC_BUFFER + 2, 0);
            for (int i = 0; i < 0x1000; i++)
                writeData(MIC_BUFFER + 3 + i, 0);
        }

        // Tell the ARM9 where the buffer is
        setReplyAction(3, command);
    }
}

void DspUcode::advancePlay() {
    // Fetch the next sound samples from the ARM9's memory
    playHead = playCount = 0;
    while (playCount < 8) {
        int16_t sample = core->memory.read<uint16_t>(0, playAddress);
        playAddress += 2;

        // Halving isn't in the DSi Sound program
        if (playHalved && version != -1)
            sample = (sample + (sample >> 15)) >> 1;
        playFifo[playCount++] = sample;

        // Say when the sound is done
        if (!--playLength) {
            playing = false;
            setReplyAction(2, playLength);
            break;
        }
    }
}

void DspUcode::advanceMic() {
    // Move microphone samples into the circular buffer in data memory
    uint16_t length = readData(MIC_BUFFER + 1);
    uint16_t position = readData(MIC_BUFFER + 2);
    for (int i = 0; i < micCount; i++) {
        writeData(MIC_BUFFER + 3 + (position & 0x3FFF), micFifo[i]);
        if (++position >= length) position = 0;
    }
    micCount = 0;
    writeData(MIC_BUFFER + 2, position);
}

void DspUcode::sampleClock(int16_t *output, int16_t input) {
    // Take a microphone sample, moving them to the buffer 8 at a time
    if (micSampling && micCount < 8) {
        micFifo[micCount++] = input;
        if (micCount == 8) advanceMic();
    }

    // Play the next sound sample on both sides, fetching more when they run out
    if (!playCount && playing) advancePlay();
    if (!playCount) {
        output[0] = output[1] = 0;
        return;
    }
    output[0] = output[1] = playFifo[playHead++];
    playCount--;
}

AacUcode::~AacUcode() {
    // Free the decoder if it was made
    if (decoder) NeAACDecClose(decoder);
}

void AacUcode::saveState(FILE *file) {
    // Write the AAC program's state to the file; the decoder starts again on the next frame
    DspUcode::saveState(file);
    fwrite(&cmdState, sizeof(cmdState), 1, file);
    fwrite(&paramCount, sizeof(paramCount), 1, file);
    fwrite(params, 2, 10, file);
}

void AacUcode::loadState(FILE *file) {
    // Read the AAC program's state from the file
    DspUcode::loadState(file);
    fread(&cmdState, sizeof(cmdState), 1, file);
    fread(&paramCount, sizeof(paramCount), 1, file);
    fread(params, 2, 10, file);
    lastFrequency = lastChannels = -1;
}

void AacUcode::writeCmd(int i, uint16_t value) {
    // CMD1 carries decode commands and their parameters
    DspUcode::writeCmd(i, value);
    if (i == 1) receiveCmd();
    else if (i == 2) cmdWritten[2] = false;
}

void AacUcode::receiveCmd() {
    // Take a word of a command: 1 starts one, then 10 parameters run it; words wait while a frame is decoding
    uint16_t value = cmd[1];
    if (cmdState == 0) {
        if (value == 1) {
            cmdState = 1;
            paramCount = 0;
        }
    }
    else if (cmdState == 1) {
        params[paramCount++] = value;
        if (paramCount == 10) {
            cmdState = 2;
            decodeFrame();
        }
    }
    else {
        return;
    }
    cmdWritten[1] = false;
}

bool AacUcode::configure(uint32_t frequency, int channels) {
    // Start the decoder again for a sample rate and channel count, giving it the MPEG-4 config for AAC LC with them
    if (decoder) NeAACDecClose(decoder);
    decoder = NeAACDecOpen();
    if (!decoder) return false;
    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(decoder);
    config->defObjectType = LC;
    config->outputFormat = FAAD_FMT_16BIT;
    if (!NeAACDecSetConfiguration(decoder, config)) return false;

    static const uint32_t rates[] = { 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };
    uint8_t rate = 3;
    for (int i = 0; i < 9; i++) {
        if (frequency != rates[i]) continue;
        rate = 3 + i;
        break;
    }
    uint8_t asc[5] = { (uint8_t)(0x10 | (rate >> 1)), (uint8_t)((rate << 7) | ((channels & 0xF) << 3)), 0x56, 0xE5, 0x00 };
    unsigned long outRate;
    unsigned char outChannels;
    return NeAACDecInit2(decoder, asc, sizeof(asc), &outRate, &outChannels) == 0;
}

void AacUcode::decodeFrame() {
    // Check the parameters, ending with code 1 if they're wrong
    uint16_t size = params[0];
    uint32_t frequency = (params[1] << 16) | params[2];
    uint16_t channels = params[3];
    uint32_t frame = (params[4] << 16) | params[5];
    uint32_t left = (params[6] << 16) | params[7];
    uint32_t right = (params[8] << 16) | params[9];
    if (!size || size > 1700 || !frequency || frequency > 48000 || (channels != 1 && channels != 2) || !frame ||
            !left || (channels == 2 && !right)) {
        scheduleFinish(256, 1);
        return;
    }

    // Read the frame; DSi Sound sends a frame of zeros before the first, which the real program fails on harmlessly
    uint8_t input[1700];
    for (int i = 0; i < size; i++)
        input[i] = core->memory.read<uint8_t>(0, frame + i);
    if (!input[0] && !input[1] && !input[2] && !input[3]) {
        scheduleFinish(512, 2);
        return;
    }

    // Set the decoder up when the format changes, ending with code 2 if it can't be or the frame can't be decoded
    if (frequency != lastFrequency || channels != lastChannels) {
        if (!configure(frequency, channels)) {
            LOG_WARN("Couldn't set the AAC decoder up for %u Hz, %d channels\n", frequency, channels);
            lastFrequency = lastChannels = -1;
            scheduleFinish(512, 2);
            return;
        }
        lastFrequency = frequency;
        lastChannels = channels;
    }
    int16_t output[1024 * 2];
    void *buffer = output;
    NeAACDecFrameInfo info;
    NeAACDecDecode2(decoder, &info, input, size, &buffer, sizeof(output));
    if (info.error || info.bytesconsumed != size || !info.channels) {
        LOG_WARN("Couldn't decode an AAC frame: %s\n", NeAACDecGetErrorMessage(info.error));
        lastFrequency = lastChannels = -1;
        scheduleFinish(512, 2);
        return;
    }

    // Write the 1024 samples to the ARM9's memory, each side to its own buffer
    for (int i = 0; i < 1024; i++) {
        int16_t *sample = &output[i * info.channels];
        core->memory.write<uint16_t>(0, left + i * 2, sample[0]);
        if (channels == 2) core->memory.write<uint16_t>(0, right + i * 2, sample[info.channels - 1]);
    }

    // Finish in about the time the real program takes, measured on hardware by melonDS
    scheduleFinish(channels == 1 ? 60000 : 115000, 0);
}

void AacUcode::finishCmd(uint16_t param) {
    // Reply with the result, then take the next command word if one came in meanwhile
    cmdState = 0;
    paramCount = 0;
    sendReply(0, param);
    if (cmdWritten[1]) receiveCmd();
}

void G711Ucode::saveState(FILE *file) {
    // Write the G.711 program's state to the file
    DspUcode::saveState(file);
    fwrite(&cmdState, sizeof(cmdState), 1, file);
    fwrite(params, 2, 8, file);
}

void G711Ucode::loadState(FILE *file) {
    // Read the G.711 program's state from the file
    DspUcode::loadState(file);
    fread(&cmdState, sizeof(cmdState), 1, file);
    fread(params, 2, 8, file);
}

void G711Ucode::writeCmd(int i, uint16_t value) {
    // Commands come through pipe 7, and CMD2 says when it was written
    DspUcode::writeCmd(i, value);
    if (i != 2) return;
    if (value == 7) tryStartCmd();
    cmdWritten[2] = false;
}

void G711Ucode::tryStartCmd() {
    // Start a command once its 8 words are in pipe 7, taking a rough guess of the real program's time
    if (cmdState != 0 || pipeLength(7) < 8) return;
    readPipe(7, params, 8);
    uint32_t command = (params[0] << 16) | params[1];
    uint32_t length = (params[6] << 16) | params[7];
    uint8_t type = command & 0xFF;
    uint32_t cycles = (type != 1 && type != 2) ? 1000 : length * (((command >> 8) & 0xF) == 1 ? 31 : 14);
    cmdState = 1;
    scheduleFinish(200 + cycles, 0);
}

void G711Ucode::finishCmd(uint16_t param) {
    // Encode or decode, A-law for type 1 and u-law for type 2
    uint32_t command = (params[0] << 16) | params[1];
    uint8_t type = command & 0xFF;
    if (((command >> 8) & 0xF) == 1) {
        if (type == 1) encodeALaw();
        else if (type == 2) encodeULaw();
    }
    else {
        if (type == 1) decodeALaw();
        else if (type == 2) decodeULaw();
    }

    // Reply with the length done, even for a type that does nothing, then start the next command
    uint16_t response[2] = { params[6], params[7] };
    writePipe(6, response, 2);
    cmdState = 0;
    tryStartCmd();
}

void G711Ucode::encodeALaw() {
    // Encode 16-bit samples to A-law bytes the way the program does
    static const int16_t segments[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF };
    uint32_t src = (params[2] << 16) | params[3];
    uint32_t dst = (params[4] << 16) | params[5];
    uint32_t length = (params[6] << 16) | params[7];
    for (uint32_t i = 0; i < length; i++, src += 2, dst++) {
        int16_t value = (int16_t)core->memory.read<uint16_t>(0, src) >> 3, mask;
        if (value > 0) {
            mask = 0xD5;
        }
        else {
            value ^= 0xFFFF;
            mask = 0x55;
        }
        int seg = 0;
        while (seg < 8 && value > segments[seg]) seg++;
        if (seg < 8) {
            int16_t top = seg << 4;
            value = top | ((value >> (seg ? seg : 1)) & 0xF);
        }
        else {
            value = 0x7F;
        }
        core->memory.write<uint8_t>(0, dst, value ^ mask);
    }
}

void G711Ucode::encodeULaw() {
    // Encode 16-bit samples to u-law bytes the way the program does
    static const int16_t segments[8] = { 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF };
    uint32_t src = (params[2] << 16) | params[3];
    uint32_t dst = (params[4] << 16) | params[5];
    uint32_t length = (params[6] << 16) | params[7];
    for (uint32_t i = 0; i < length; i++, src += 2, dst++) {
        int16_t value = (int16_t)core->memory.read<uint16_t>(0, src) >> 2, mask;
        if (value > 0) {
            mask = 0xFF;
        }
        else {
            value = (value ^ 0xFFFF) + 1;
            mask = 0x7F;
        }
        if (value > 0x1FDF) value = 0x1FDF;
        value += 0x21;
        int seg = 0;
        while (seg < 8 && value > segments[seg]) seg++;
        if (seg < 8)
            value = (seg << 4) | ((value >> (seg + 1)) & 0xF);
        else
            value = 0x7F;
        core->memory.write<uint8_t>(0, dst, value ^ mask);
    }
}

void G711Ucode::decodeALaw() {
    // Decode A-law bytes to 16-bit samples
    uint32_t src = (params[2] << 16) | params[3];
    uint32_t dst = (params[4] << 16) | params[5];
    uint32_t length = (params[6] << 16) | params[7];
    for (uint32_t i = 0; i < length; i++, src++, dst += 2) {
        int8_t value = core->memory.read<uint8_t>(0, src) ^ 0x55;
        int16_t sample = ((value & 0xF) << 4) + 8;
        int shift = (value >> 4) & 0x7;
        if (shift) sample = (sample + 0x100) << (shift - 1);
        if (value & 0x80) sample = -sample;
        core->memory.write<uint16_t>(0, dst, sample);
    }
}

void G711Ucode::decodeULaw() {
    // Decode u-law bytes to 16-bit samples
    uint32_t src = (params[2] << 16) | params[3];
    uint32_t dst = (params[4] << 16) | params[5];
    uint32_t length = (params[6] << 16) | params[7];
    for (uint32_t i = 0; i < length; i++, src++, dst += 2) {
        int8_t value = ~core->memory.read<uint8_t>(0, src);
        int16_t sample = ((value & 0xF) << 3) + 0x84;
        sample = 0x84 - (sample << ((value >> 4) & 0x7));
        if (value & 0x80) sample = -sample;
        core->memory.write<uint16_t>(0, dst, sample);
    }
}

void GraphicsUcode::saveState(FILE *file) {
    // Write the graphics program's state to the file
    DspUcode::saveState(file);
    fwrite(&cmdState, sizeof(cmdState), 1, file);
    fwrite(&cmdIndex, sizeof(cmdIndex), 1, file);
    fwrite(params, 2, 14, file);
}

void GraphicsUcode::loadState(FILE *file) {
    // Read the graphics program's state from the file
    DspUcode::loadState(file);
    fread(&cmdState, sizeof(cmdState), 1, file);
    fread(&cmdIndex, sizeof(cmdIndex), 1, file);
    fread(params, 2, 14, file);
}

void GraphicsUcode::writeCmd(int i, uint16_t value) {
    // CMD0 picks the command, its parameters come through pipe 7, and CMD2 says when that was written
    DspUcode::writeCmd(i, value);
    if (i == 0) {
        tryStartCmd();
    }
    else if (i == 2) {
        if (value == 7) tryStartCmd();
        cmdWritten[2] = false;
    }
}

void GraphicsUcode::tryStartCmd() {
    // Take a command from CMD0
    if (cmdState == 0) {
        if (!cmdWritten[0]) return;
        cmdState = 1;
        cmdIndex = cmd[0];
        cmdWritten[0] = false;
    }
    if (cmdState != 1) return;

    // Start it once all its parameters are in pipe 7, guessing the real program's time; bicubic scaling is slow
    uint32_t length = pipeLength(7), cycles;
    switch (cmdIndex) {
        case 1: { // Scaling
            if (length < 14) return;
            readPipe(7, params, 14);
            uint32_t width = params[11], height = params[12];
            if (params[4] == 10) {
                // A third of the size, which fails unless the size divides by 3
                if (width % 3 || height % 3) {
                    sendReply(1, 0);
                    cmdState = 0;
                    return;
                }
                cycles = 30 * width * height;
            }
            else {
                uint32_t pixels = (width * params[7] / 1000) * (height * params[8] / 1000);
                cycles = 4 * width * height;
                switch (params[4]) {
                    case 2: cycles += 58 * pixels; break; // Bilinear
                    case 3: cycles += 605 * pixels; break; // Bicubic
                    default: cycles += 26 * pixels; break; // Nearest
                }
            }
            break;
        }

        case 2: // YUV to RGB
            if (length < 6) return;
            readPipe(7, params, 6);
            cycles = 24 * (((params[1] << 16) | params[0]) >> 1);
            break;

        default: // Unknown
            sendReply(1, 0);
            cmdState = 0;
            return;
    }
    cmdState = 2;
    scheduleFinish(200 + cycles, 0);
}

void GraphicsUcode::finishCmd(uint16_t param) {
    // Run the command, then reply and start the next one
    if (cmdIndex == 1) {
        switch (params[4]) {
            case 2: scaleBilinear(); break;
            case 3: scaleBicubic(); break;
            case 10: scaleThird(); break;
            default: scaleNearest(); break;
        }
    }
    else if (cmdIndex == 2) {
        yuvToRgb();
    }
    sendReply(1, 1);
    cmdState = 0;
    tryStartCmd();
}

void GraphicsUcode::scaleNearest() {
    // Scale a rectangle of a 15-bit picture by thousandths, taking the nearest pixel
    uint32_t srcAddr = (params[1] << 16) | params[0];
    uint32_t dstAddr = (params[3] << 16) | params[2];
    uint16_t srcWidth = params[5], rectX = params[9], rectY = params[10], rectWidth = params[11], rectHeight = params[12];
    uint32_t dstWidth = rectWidth * params[7] / 1000, dstHeight = rectHeight * params[8] / 1000;
    if (!dstWidth || !dstHeight || rectWidth < 2 || rectHeight < 2 || rectWidth > 16384 || dstWidth > 16384) return;
    uint32_t xStep = ((rectWidth - 2) << 10) / std::max(1U, dstWidth - 1);
    uint32_t yStep = ((rectHeight - 2) << 10) / std::max(1U, dstHeight - 1);
    src.resize(rectWidth);
    dst.resize(dstWidth);

    srcAddr += (rectY * srcWidth + rectX) << 1;
    readArm9(&src[0], srcAddr, rectWidth << 1);
    for (uint32_t y = 0, sy = 0x3FF; y < dstHeight; y++) {
        for (uint32_t x = 0, sx = 0x3FF; x < dstWidth; x++, sx += xStep)
            dst[x] = src[sx >> 10];
        writeArm9(&dst[0], dstAddr, dstWidth << 1);
        dstAddr += dstWidth << 1;
        uint32_t next = sy + yStep;
        if ((next >> 10) != (sy >> 10))
            readArm9(&src[0], srcAddr + (((next >> 10) * srcWidth) << 1), rectWidth << 1);
        sy = next;
    }
}

void GraphicsUcode::scaleBilinear() {
    // Scale a rectangle of a 15-bit picture by thousandths, blending the 4 nearest pixels
    uint32_t srcAddr = (params[1] << 16) | params[0];
    uint32_t dstAddr = (params[3] << 16) | params[2];
    uint16_t srcWidth = params[5], rectX = params[9], rectY = params[10], rectWidth = params[11], rectHeight = params[12];
    uint32_t dstWidth = rectWidth * params[7] / 1000, dstHeight = rectHeight * params[8] / 1000;
    if (!dstWidth || !dstHeight || rectWidth < 2 || rectHeight < 2 || rectWidth > 8192 || dstWidth > 8192) return;
    uint32_t xStep = ((rectWidth - 2) << 10) / std::max(1U, dstWidth - 1);
    uint32_t yStep = ((rectHeight - 2) << 10) / std::max(1U, dstHeight - 1);
    src.resize(rectWidth * 2 + 1);
    dst.resize(dstWidth);

    // Keep the line and the one below it
    srcAddr += (rectY * srcWidth + rectX) << 1;
    readArm9(&src[0], srcAddr, rectWidth << 1);
    readArm9(&src[rectWidth], srcAddr + (srcWidth << 1), rectWidth << 1);
    for (uint32_t y = 0, sy = 0x200; y < dstHeight; y++) {
        for (uint32_t x = 0, sx = 0x200; x < dstWidth; x++, sx += xStep) {
            uint16_t p[4] = { src[sx >> 10], src[(sx >> 10) + 1], src[rectWidth + (sx >> 10)],
                src[rectWidth + (sx >> 10) + 1] };
            uint32_t fx0 = sx & 0x3FF, fx1 = 0x400 - fx0, fy0 = sy & 0x3FF, fy1 = 0x400 - fy0;
            uint16_t color = BIT(15);
            for (int c = 0; c < 15; c += 5) {
                uint32_t v[4];
                for (int i = 0; i < 4; i++)
                    v[i] = (p[i] >> c) & 0x1F;
                uint32_t blend = (((v[0] * fx1 + v[1] * fx0) * fy1) + ((v[2] * fx1 + v[3] * fx0) * fy0)) >> 20;
                color |= (blend & 0x1F) << c;
            }
            dst[x] = color;
        }
        writeArm9(&dst[0], dstAddr, dstWidth << 1);
        dstAddr += dstWidth << 1;
        uint32_t next = sy + yStep;
        if ((next >> 10) != (sy >> 10)) {
            readArm9(&src[0], srcAddr + (((next >> 10) * srcWidth) << 1), rectWidth << 1);
            readArm9(&src[rectWidth], srcAddr + ((((next >> 10) + 1) * srcWidth) << 1), rectWidth << 1);
        }
        sy = next;
    }
}

static int32_t bicubicWeight(int32_t x) {
    // Get the weight of a pixel x/1024 away for bicubic convolution with a = -1, with 16 fractional bits
    if (x <= 0x400) { // x^3 - 2x^2 + 1
        int32_t square = (x * x) >> 2;
        int32_t cube = (square * x) >> 12;
        return cube - 2 * (square >> 2) + 0x10000;
    }
    if (x <= 0x800) { // -x^3 + 5x^2 - 8x + 4
        int32_t cube = (uint32_t)((x * x) >> 2) * (uint32_t)x >> 12;
        return -cube + ((5 * x * x) >> 4) - 8 * x * 64 + 0x40000;
    }
    return 0;
}

void GraphicsUcode::scaleBicubic() {
    // Scale a rectangle of a 15-bit picture by thousandths, from the 16 nearest pixels
    uint32_t srcAddr = (params[1] << 16) | params[0];
    uint32_t dstAddr = (params[3] << 16) | params[2];
    uint16_t srcWidth = params[5], rectX = params[9], rectY = params[10], rectWidth = params[11], rectHeight = params[12];
    uint32_t dstWidth = rectWidth * params[7] / 1000, dstHeight = rectHeight * params[8] / 1000;
    if (!dstWidth || !dstHeight || rectWidth < 4 || rectHeight < 4 || rectWidth > 4096 || dstWidth > 4096) return;
    uint32_t xStep = ((rectWidth - 4) << 10) / std::max(1U, dstWidth - 1);
    uint32_t yStep = ((rectHeight - 4) << 10) / std::max(1U, dstHeight - 1);
    src.resize(rectWidth * 4 + 3);
    dst.resize(dstWidth);

    srcAddr += (rectY * srcWidth + rectX) << 1;
    for (int i = 0; i < 4; i++)
        readArm9(&src[rectWidth * i], srcAddr + ((srcWidth * i) << 1), rectWidth << 1);
    for (uint32_t y = 0, sy = 0x200; y < dstHeight; y++) {
        for (uint32_t x = 0, sx = 0x200; x < dstWidth; x++, sx += xStep) {
            uint32_t fx = sx & 0x3FF, fy = sy & 0x3FF;
            int32_t wx[4] = { bicubicWeight(0x400 + fx), bicubicWeight(fx), bicubicWeight(0x400 - fx),
                bicubicWeight(0x800 - fx) };
            int32_t wy[4] = { bicubicWeight(0x400 + fy), bicubicWeight(fy), bicubicWeight(0x400 - fy),
                bicubicWeight(0x800 - fy) };
            int64_t total[3] = {};
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    uint16_t pixel = src[rectWidth * i + (sx >> 10) + j];
                    int32_t weight = ((wx[j] >> 1) * (wy[i] >> 1)) >> 6;
                    for (int c = 0; c < 3; c++)
                        total[c] += (int32_t)((pixel >> (c * 5)) & 0x1F) * weight;
                }
            }

            // Round and clamp each colour
            uint16_t color = BIT(15);
            for (int c = 0; c < 3; c++)
                color |= std::clamp((int32_t)((total[c] + 0x800000) >> 24), 0, 31) << (c * 5);
            dst[x] = color;
        }
        writeArm9(&dst[0], dstAddr, dstWidth << 1);
        dstAddr += dstWidth << 1;
        uint32_t next = sy + yStep;
        if ((next >> 10) != (sy >> 10)) {
            for (int i = 0; i < 4; i++)
                readArm9(&src[rectWidth * i], srcAddr + ((((next >> 10) + i) * srcWidth) << 1), rectWidth << 1);
        }
        sy = next;
    }
}

void GraphicsUcode::scaleThird() {
    // Scale a rectangle of a 15-bit picture to a third, averaging the 8 outer pixels of each 3x3 block
    uint32_t srcAddr = (params[1] << 16) | params[0];
    uint32_t dstAddr = (params[3] << 16) | params[2];
    uint16_t srcWidth = params[5], rectX = params[9], rectY = params[10], rectWidth = params[11], rectHeight = params[12];
    uint32_t dstWidth = rectWidth / 3, dstHeight = rectHeight / 3;
    if (!dstWidth || rectWidth > 16384) return;
    src.resize(rectWidth * 3);
    dst.resize(dstWidth);

    srcAddr += (rectY * srcWidth + rectX) << 1;
    for (uint32_t y = 0; y < dstHeight; y++) {
        for (int i = 0; i < 3; i++)
            readArm9(&src[rectWidth * i], srcAddr + (((y * 3 + i) * srcWidth) << 1), rectWidth << 1);
        for (uint32_t x = 0; x < dstWidth; x++) {
            const uint16_t *top = &src[x * 3], *mid = top + rectWidth, *bot = mid + rectWidth;
            uint16_t p[8] = { top[0], top[1], top[2], mid[0], mid[2], bot[0], bot[1], bot[2] };
            uint32_t r = 0, g = 0, b = 0;
            for (int i = 0; i < 8; i++) {
                r += p[i] & 0x1F;
                g += (p[i] >> 5) & 0x1F;
                b += (p[i] >> 10) & 0x1F;
            }
            dst[x] = BIT(15) | (r >> 3) | ((g << 2) & 0x3E0) | ((b << 7) & 0x7C00);
        }
        writeArm9(&dst[0], dstAddr, dstWidth << 1);
        dstAddr += dstWidth << 1;
    }
}

void GraphicsUcode::yuvToRgb() {
    // Convert YUV422 pixel pairs to 15-bit colour, with the program's shift-based maths
    uint32_t length = (params[1] << 16) | params[0];
    uint32_t srcAddr = (params[3] << 16) | params[2];
    uint32_t dstAddr = (params[5] << 16) | params[4];
    for (uint32_t i = 0; i < length; i += 4, srcAddr += 4, dstAddr += 4) {
        uint32_t value = core->memory.read<uint32_t>(0, srcAddr);
        int32_t y[2] = { (int32_t)(value & 0xFF), (int32_t)((value >> 16) & 0xFF) };
        int32_t u = (int32_t)((value >> 8) & 0xFF) - 128, v = (int32_t)(value >> 24) - 128;
        int32_t r = (v * 359) >> 8, g = -(u * 352 + v * 731) >> 10, b = (u * 1815) >> 10;
        uint32_t colors = 0;
        for (int j = 0; j < 2; j++) {
            uint32_t color = BIT(15) | (std::clamp(y[j] + r, 0, 255) >> 3) | ((std::clamp(y[j] + g, 0, 255) >> 3) << 5) |
                ((std::clamp(y[j] + b, 0, 255) >> 3) << 10);
            colors |= color << (j * 16);
        }
        core->memory.write<uint32_t>(0, dstAddr, colors);
    }
}
