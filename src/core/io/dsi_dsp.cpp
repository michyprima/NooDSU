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

#include <stdexcept>

#include "../core.h"
#include "../settings.h"
#include "../teakra/teakra.h"
#include "dsi_dsp_hle.h"

DsiDsp::~DsiDsp() {
    // Free the processor if it was ever used, and the high-level program
    delete teakra;
    delete ucode;
}

void DsiDsp::saveState(FILE *file) {
    // Write DSi state data to the file
    if (!core->dsiMode) return;
    fwrite(readFifo, 2, sizeof(readFifo) / 2, file);
    fwrite(&readHead, sizeof(readHead), 1, file);
    fwrite(&readCount, sizeof(readCount), 1, file);
    fwrite(&readLength, sizeof(readLength), 1, file);
    fwrite(&lastCycles, sizeof(lastCycles), 1, file);
    fwrite(&rst, sizeof(rst), 1, file);
    fwrite(&padr, sizeof(padr), 1, file);
    fwrite(&pcfg, sizeof(pcfg), 1, file);
    fwrite(&psts, sizeof(psts), 1, file);
    fwrite(&psem, sizeof(psem), 1, file);
    fwrite(&pmask, sizeof(pmask), 1, file);
    fwrite(cmd, 2, sizeof(cmd) / 2, file);
    fwrite(&updatePending, sizeof(updatePending), 1, file);

    // Write which high-level program is running, if any, and its state
    uint32_t id = ucode ? ucode->getId() : -1;
    fwrite(&id, sizeof(id), 1, file);
    if (ucode) ucode->saveState(file);

    // Write the processor's state if it was ever used
    bool used = teakra;
    fwrite(&used, sizeof(used), 1, file);
    if (!used) return;
    melonDS::Savestate state(file, true);
    teakra->DoSavestate(&state);
}

void DsiDsp::loadState(FILE *file) {
    // Read DSi state data from the file
    if (!core->dsiMode) return;
    fread(readFifo, 2, sizeof(readFifo) / 2, file);
    fread(&readHead, sizeof(readHead), 1, file);
    fread(&readCount, sizeof(readCount), 1, file);
    fread(&readLength, sizeof(readLength), 1, file);
    fread(&lastCycles, sizeof(lastCycles), 1, file);
    fread(&rst, sizeof(rst), 1, file);
    fread(&padr, sizeof(padr), 1, file);
    fread(&pcfg, sizeof(pcfg), 1, file);
    fread(&psts, sizeof(psts), 1, file);
    fread(&psem, sizeof(psem), 1, file);
    fread(&pmask, sizeof(pmask), 1, file);
    fread(cmd, 2, sizeof(cmd) / 2, file);
    fread(&updatePending, sizeof(updatePending), 1, file);

    // Read which high-level program was running, and its state
    uint32_t id = -1;
    fread(&id, sizeof(id), 1, file);
    stopUcode();
    if (id != (uint32_t)-1) {
        ucode = DspUcode::create(core, this, pages, id);
        ucode->loadState(file);
    }

    // Read the processor's state if it was in use, or put it back as it starts
    bool used = false;
    fread(&used, sizeof(used), 1, file);
    if (used) {
        create();
        melonDS::Savestate state(file, false);
        teakra->DoSavestate(&state);
    }
    else if (teakra) {
        teakra->Reset();
    }

    // Sound waiting to be mixed isn't kept
    sampleHead = sampleCount = 0;
    samplePhase = 0;
    pagesDirty = true;
}

bool DsiDsp::accessible() {
    // The registers only work while the ARM9 has access to the DSP
    return core->memory.readScfgExt(0) & BIT(18);
}

bool DsiDsp::running() {
    // The DSP runs while it has a clock and neither of its resets is held
    return (teakra || ucode) && (core->memory.readScfgClk() & BIT(1)) && (rst & BIT(0)) && !(pcfg & BIT(0));
}

uint32_t DsiDsp::frameCycles() {
    // Get the cycles in an I2S frame, at 32.73kHz or 47.61kHz
    return (core->dsiMic.readSndExCnt() & BIT(13)) ? 704 : 1024;
}

void DsiDsp::create() {
    // Make the processor the first time it's needed, since most software never uses it
    if (teakra) return;
    teakra = new Teakra::Teakra();
    teakra->SetSharedMemoryPages(pages);

    // Replies and semaphore bits from the DSP can interrupt the ARM9
    for (int i = 0; i < 3; i++)
        teakra->SetRecvDataHandler(i, [this, i]() { replyIrq(i); });
    teakra->SetSemaphoreHandler([this]() { semaphoreIrq(); });

    // The DSP can reach the ARM9's memory by itself, which happens at once here
    Teakra::AHBMCallback ahbm;
    ahbm.read8 = [this](uint32_t address) { return core->memory.read<uint8_t>(0, address); };
    ahbm.write8 = [this](uint32_t address, uint8_t value) { core->memory.write<uint8_t>(0, address, value); };
    ahbm.read16 = [this](uint32_t address) { return core->memory.read<uint16_t>(0, address); };
    ahbm.write16 = [this](uint32_t address, uint16_t value) { core->memory.write<uint16_t>(0, address, value); };
    ahbm.read32 = [this](uint32_t address) { return core->memory.read<uint32_t>(0, address); };
    ahbm.write32 = [this](uint32_t address, uint32_t value) { core->memory.write<uint32_t>(0, address, value); };
    teakra->SetAHBMCallback(ahbm);
}

void DsiDsp::replyIrq(int i) {
    // Interrupt the ARM9 for a reply from the DSP if it's enabled
    if (pcfg & BIT(9 + i)) core->interpreter[0].sendInterrupt(24);
}

void DsiDsp::semaphoreIrq() {
    // Interrupt the ARM9 for semaphore bits the DSP set
    psts |= BIT(9);
    core->interpreter[0].sendInterrupt(24);
}

void DsiDsp::startUcode() {
    // Do the loaded program in high level if it's one of Nintendo's, otherwise Teakra runs it
    stopUcode();
    if (!Settings::dspHle) return;
    updatePages();
    ucode = DspUcode::create(core, this, pages);
    if (ucode) ucode->start();
}

void DsiDsp::stopUcode() {
    // Drop the high-level program, going back to Teakra
    delete ucode;
    ucode = nullptr;
}

void DsiDsp::reset() {
    // Put the registers and the processor back as they start
    padr = pcfg = psts = psem = 0;
    pmask = 0xFF;
    cmd[0] = cmd[1] = cmd[2] = 0;
    readHead = readCount = 0;
    readLength = 0;
    sampleHead = sampleCount = 0;
    samplePhase = 0;
    stopUcode();
    if (teakra) teakra->Reset();
}

void DsiDsp::updatePages() {
    // Find the new WRAM blocks given to the DSP; when two share a slot, the lower one is seen
    for (int i = 0; i < 16; i++)
        pages[i] = nullptr;
    for (int i = 7; i >= 0; i--) {
        uint8_t slotB = core->memory.readMbk23(i), slotC = core->memory.readMbk45(i);
        if ((slotB & 0x82) == 0x82) pages[(slotB >> 2) & 0x7] = core->memory.getNwram(1) + (i << 15);
        if ((slotC & 0x82) == 0x82) pages[8 + ((slotC >> 2) & 0x7)] = core->memory.getNwram(2) + (i << 15);
    }
    pagesDirty = false;
    if (teakra) teakra->Wake();
}

void DsiDsp::catchUp() {
    // Run the DSP up to the current time, at 4 of its cycles to one of the system's
    uint32_t cycles = core->globalCycles - lastCycles;
    lastCycles = core->globalCycles;

    // A high-level program has nothing to run, but needs the memory it sees
    if (ucode) {
        if (pagesDirty) updatePages();
        return;
    }
    if (!cycles || !running()) return;
    if (pagesDirty) updatePages();

    // Teakra gives up on a few things no known program does; carry on rather than stop everything
    try {
        teakra->Run(cycles * 4);
    }
    catch (const std::runtime_error &error) {
        LOG_CRIT("The DSP ran something that isn't emulated: %s\n", error.what());
    }
}

void DsiDsp::requestUpdate() {
    // Schedule the next update if the DSP is running and has none coming
    if (updatePending || !running()) return;
    core->schedule(DSP_UPDATE, frameCycles());
    updatePending = true;
}

void DsiDsp::update() {
    // Bring the DSP up to date once every I2S frame, so its interrupts are never late by more than that
    updatePending = false;
    catchUp();
    if (!running()) return;

    // Exchange a frame of sound with the DSP, giving it the microphone and keeping its output for the mixer
    if (core->dsiMic.readSndExCnt() & BIT(15)) {
        int16_t output[2], input = core->spi.readMic(core->globalCycles);
        if (ucode)
            ucode->sampleClock(output, input);
        else
            teakra->SampleClock(output, input);
        if (sampleCount == 64) {
            sampleHead = (sampleHead + 1) & 63;
            sampleCount--;
        }
        int16_t *sample = samples[(sampleHead + sampleCount++) & 63];
        sample[0] = output[0];
        sample[1] = output[1];
    }
    requestUpdate();
}

void DsiDsp::finishHle() {
    // Finish a high-level program's command when it's due
    catchUp();
    if (ucode) ucode->finish();
}

void DsiDsp::resetCycles() {
    // Adjust the last run time for the global reset
    catchUp();
    lastCycles -= core->globalCycles;
    if (ucode) ucode->resetCycles();
}

void DsiDsp::clockChanged() {
    // Start updates again if the DSP just got its clock back, without running it for the time it was stopped
    lastCycles = core->globalCycles;
    requestUpdate();
}

bool DsiDsp::takeSample(int16_t *sample) {
    // Get the DSP's sound for the mixer's next sample, moving through it faster when the DSP's rate is higher
    sample[0] = sample[1] = 0;
    if (!sampleCount) return false;
    uint32_t frame = frameCycles();
    for (samplePhase += 1024; samplePhase >= frame && sampleCount; samplePhase -= frame) {
        sample[0] = samples[sampleHead][0];
        sample[1] = samples[sampleHead][1];
        sampleHead = (sampleHead + 1) & 63;
        sampleCount--;
    }
    if (samplePhase >= frame) samplePhase %= frame;
    return true;
}

uint32_t DsiDsp::transferAddress() {
    // Get the address PDATA reaches, with its upper bits from the DSP's first DMA channel, then move it on if set to
    uint32_t address = padr;
    if (((pcfg >> 12) & 0x7) != 1 && !ucode)
        address |= teakra->DMAChan0GetDstHigh() << 16;
    if (pcfg & BIT(1)) padr++;
    return address;
}

uint16_t DsiDsp::transferRead() {
    // Read a word from the DSP's side for PDATA, from the selected kind of memory; a high-level program only has memory
    if (ucode) {
        switch ((pcfg >> 12) & 0x7) {
            case 0: return ucode->readData(transferAddress());
            case 5: return ucode->readProgram(transferAddress());
        }
        transferAddress();
        return 0;
    }
    if (!teakra) return 0;
    switch ((pcfg >> 12) & 0x7) {
        case 0: return teakra->DataReadA32(transferAddress()); // Data
        case 1: return teakra->MMIORead(transferAddress() & 0x7FF); // Registers
        case 5: return teakra->ProgramRead(transferAddress()); // Program

        case 7: { // The ARM9's memory, if the DSP has set its first channel up to read it
            uint32_t address = transferAddress();
            if (teakra->AHBMGetDmaChannel(0) != 0 || teakra->AHBMGetDirection(0) != 0) return 0;
            switch (teakra->AHBMGetUnitSize(0)) {
                case 0: return core->memory.read<uint8_t>(0, address);
                case 1: return teakra->AHBMRead16(address);
                case 2: return teakra->AHBMRead32(address);
            }
            return 0;
        }
    }
    return 0;
}

void DsiDsp::fetch() {
    // Fetch the next word of a read transfer into the FIFO
    if (!readLength) return;
    readFifo[(readHead + readCount++) & 15] = transferRead();
    if (readLength > 0) readLength--;
}

uint16_t DsiDsp::readPdata() {
    // Read a word from the FIFO, then fill it again with what's left of the transfer
    if (!accessible()) return 0;
    catchUp();
    uint16_t value = 0;
    if (readCount) {
        value = readFifo[readHead];
        readHead = (readHead + 1) & 15;
        readCount--;
    }
    while (readLength && readCount < 16)
        fetch();
    if (readCount) core->interpreter[0].sendInterrupt(24);
    return value;
}

uint16_t DsiDsp::readPsts() {
    // Read from the DSP_PSTS register; writes are instant, so the write FIFO is always empty
    if (!accessible()) return 0;
    catchUp();
    uint16_t value = (psts & BIT(9)) | BIT(8);
    if (readCount == 16) value |= BIT(5);
    if (readCount) value |= BIT(6) | BIT(0);
    for (int i = 0; i < 3; i++) {
        if (ucode ? ucode->replyReady(i) : (teakra && teakra->RecvDataIsReady(i))) value |= BIT(10 + i);
        if (ucode ? !ucode->cmdEmpty(i) : (teakra && !teakra->SendDataIsEmpty(i))) value |= BIT(13 + i);
    }
    return value;
}

uint16_t DsiDsp::readSem() {
    // Read the semaphore bits the DSP has set
    if (!accessible() || !(teakra || ucode)) return 0;
    catchUp();
    return ucode ? ucode->getSemaphore() : teakra->GetSemaphore();
}

uint16_t DsiDsp::readRep(int i) {
    // Read one of the DSP's replies, which lets it send the next
    if (!accessible() || !(teakra || ucode)) return 0;
    catchUp();
    return ucode ? ucode->readReply(i) : teakra->RecvData(i);
}

void DsiDsp::writeRst(uint16_t mask, uint16_t value) {
    // Write to the SCFG_RST register, where any write resets the DSP and bit 0 then lets it run
    if (!(mask & 0xFF)) return;
    rst = value & BIT(0);
    if (rst) create();
    reset();
    lastCycles = core->globalCycles;
    requestUpdate();
}

void DsiDsp::writePdata(uint16_t mask, uint16_t value) {
    // Write a word to the DSP's side, to the selected kind of memory
    if (!accessible() || mask != 0xFFFF) return;
    catchUp();
    if (ucode) {
        switch ((pcfg >> 12) & 0x7) {
            case 0: ucode->writeData(transferAddress(), value); break;
            case 5: ucode->writeProgram(transferAddress(), value); break;
            default: transferAddress(); break;
        }
    }
    else if (teakra) {
        switch ((pcfg >> 12) & 0x7) {
            case 0: teakra->DataWriteA32(transferAddress(), value); break; // Data
            case 1: teakra->MMIOWrite(transferAddress() & 0x7FF, value); break; // Registers
            case 5: teakra->ProgramWrite(transferAddress(), value); break; // Program

            case 7: { // The ARM9's memory, if the DSP has set its first channel up to write it
                uint32_t address = transferAddress();
                if (teakra->AHBMGetDmaChannel(0) != 0 || teakra->AHBMGetDirection(0) != 1) break;
                switch (teakra->AHBMGetUnitSize(0)) {
                    case 0: core->memory.write<uint8_t>(0, address, value); break;
                    case 1: teakra->AHBMWrite16(address, value); break;
                    case 2: teakra->AHBMWrite32(address, value); break;
                }
                break;
            }

            default: return;
        }
    }
    else if (pcfg & BIT(1)) {
        padr++;
    }

    // The write FIFO is empty again straight away
    core->interpreter[0].sendInterrupt(24);
}

void DsiDsp::writePadr(uint16_t mask, uint16_t value) {
    // Write to the DSP_PADR register, the lower address for PDATA
    if (!accessible()) return;
    padr = (padr & ~mask) | (value & mask);
}

void DsiDsp::writePcfg(uint16_t mask, uint16_t value) {
    // Write to the DSP_PCFG register
    if (!accessible()) return;
    catchUp();
    uint16_t old = pcfg;
    pcfg = (pcfg & ~mask) | (value & mask);

    // Bit 0 holds the processor in reset, which puts it back as it starts; letting it go starts the loaded program,
    // which may be done in high level
    if (!(old & BIT(0)) && (pcfg & BIT(0))) {
        stopUcode();
        if (teakra) teakra->Reset();
    }
    else if ((old & BIT(0)) && !(pcfg & BIT(0)) && (rst & BIT(0))) {
        startUcode();
    }
    requestUpdate();

    // Bit 4 starts a read transfer of the set length, filling the FIFO, and clearing it drops what was fetched
    readHead = readCount = 0;
    readLength = 0;
    if (!(pcfg & BIT(4))) return;
    static const int8_t lengths[] = { 1, 8, 16, -1 };
    readLength = lengths[(pcfg >> 2) & 0x3];
    while (readLength && readCount < 16)
        fetch();
    core->interpreter[0].sendInterrupt(24);
}

void DsiDsp::writePsem(uint16_t mask, uint16_t value) {
    // Write to the DSP_PSEM register, setting semaphore bits for the DSP
    if (!accessible()) return;
    catchUp();
    psem = (psem & ~mask) | (value & mask);
    if (ucode) ucode->setSemaphore(psem);
    else if (teakra) teakra->SetSemaphore(psem);
}

void DsiDsp::writePmask(uint16_t mask, uint16_t value) {
    // Write to the DSP_PMASK register, choosing which of the DSP's semaphore bits don't interrupt the ARM9
    if (!accessible()) return;
    catchUp();
    pmask = (pmask & ~mask) | (value & mask);
    if (ucode) ucode->maskSemaphore(pmask);
    else if (teakra) teakra->MaskSemaphore(pmask);
}

void DsiDsp::writePclear(uint16_t mask, uint16_t value) {
    // Write to the DSP_PCLEAR register, clearing semaphore bits the DSP set
    if (!accessible()) return;
    catchUp();
    if (ucode) ucode->clearSemaphore(value & mask);
    else if (teakra) teakra->ClearSemaphore(value & mask);
    if (ucode ? !ucode->getSemaphore() : (!teakra || !teakra->GetSemaphore()))
        psts &= ~BIT(9);
}

void DsiDsp::writeCmd(int i, uint16_t mask, uint16_t value) {
    // Write one of the commands for the DSP
    if (!accessible() || mask != 0xFFFF) return;
    catchUp();
    cmd[i] = value;
    if (ucode) ucode->writeCmd(i, value);
    else if (teakra) teakra->SendData(i, value);
}
