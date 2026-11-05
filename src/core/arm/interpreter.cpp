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

#include "../core.h"
#include "../settings.h"
#include "../emu_profile.h"

Interpreter::Interpreter(Core *core, bool arm7): core(core), arm7(arm7) {
    // Initialize the registers for user mode
    for (int i = 0; i < 32; i++)
        registers[i] = &registersUsr[i & 0xF];
}

void Interpreter::saveState(FILE *file) {
    // Write state data to the file, with the pipeline filled in case the JIT left it empty
    if (pipelineStale) refillPipeline();
    fwrite(pipeline, 4, sizeof(pipeline) / 4, file);
    fwrite(registersUsr, 4, sizeof(registersUsr) / 4, file);
    fwrite(registersFiq, 4, sizeof(registersFiq) / 4, file);
    fwrite(registersSvc, 4, sizeof(registersSvc) / 4, file);
    fwrite(registersAbt, 4, sizeof(registersAbt) / 4, file);
    fwrite(registersIrq, 4, sizeof(registersIrq) / 4, file);
    fwrite(registersUnd, 4, sizeof(registersUnd) / 4, file);
    fwrite(&cpsr, sizeof(cpsr), 1, file);
    fwrite(&spsrFiq, sizeof(spsrFiq), 1, file);
    fwrite(&spsrSvc, sizeof(spsrSvc), 1, file);
    fwrite(&spsrAbt, sizeof(spsrAbt), 1, file);
    fwrite(&spsrIrq, sizeof(spsrIrq), 1, file);
    fwrite(&spsrUnd, sizeof(spsrUnd), 1, file);
    fwrite(&cycles, sizeof(cycles), 1, file);
    fwrite(&halted, sizeof(halted), 1, file);
    fwrite(&dsiCycle, sizeof(dsiCycle), 1, file);
    fwrite(&ime, sizeof(ime), 1, file);
    fwrite(&ie, sizeof(ie), 1, file);
    fwrite(&irf, sizeof(irf), 1, file);
    fwrite(&postFlg, sizeof(postFlg), 1, file);
}

void Interpreter::loadState(FILE *file) {
    // Read state data from the file
    fread(pipeline, 4, sizeof(pipeline) / 4, file);
    fread(registersUsr, 4, sizeof(registersUsr) / 4, file);
    fread(registersFiq, 4, sizeof(registersFiq) / 4, file);
    fread(registersSvc, 4, sizeof(registersSvc) / 4, file);
    fread(registersAbt, 4, sizeof(registersAbt) / 4, file);
    fread(registersIrq, 4, sizeof(registersIrq) / 4, file);
    fread(registersUnd, 4, sizeof(registersUnd) / 4, file);
    fread(&cpsr, sizeof(cpsr), 1, file);
    fread(&spsrFiq, sizeof(spsrFiq), 1, file);
    fread(&spsrSvc, sizeof(spsrSvc), 1, file);
    fread(&spsrAbt, sizeof(spsrAbt), 1, file);
    fread(&spsrIrq, sizeof(spsrIrq), 1, file);
    fread(&spsrUnd, sizeof(spsrUnd), 1, file);
    fread(&cycles, sizeof(cycles), 1, file);
    fread(&halted, sizeof(halted), 1, file);
    fread(&dsiCycle, sizeof(dsiCycle), 1, file);
    fread(&ime, sizeof(ime), 1, file);
    fread(&ie, sizeof(ie), 1, file);
    fread(&irf, sizeof(irf), 1, file);
    fread(&postFlg, sizeof(postFlg), 1, file);

    // Update mapped registers
    swapRegisters(cpsr);
    pcData = nullptr;
}

void Interpreter::init() {
    // Prepare to boot the BIOS
    setCpsr(0x000000D3); // Supervisor, interrupts off
    registersUsr[15] = arm7 ? 0x00000000 : 0xFFFF0000;
    flushPipeline();

    // Reset the I/O registers
    ime = 0;
    ie = irf = 0;
    postFlg = 0;
}

void Interpreter::directBoot() {
    // Prepare to directly boot a ROM
    setCpsr(0x000000DF); // System, interrupts off
    registersUsr[15] = core->gbaMode ? 0x8000000 : entryAddr;
    registersUsr[14] = registersUsr[15];
    registersUsr[12] = registersUsr[15];
    registersUsr[13] = arm7 ? (core->gbaMode ? 0x3007F00 : 0x380FD80) : 0x3002F7C;
    registersIrq[0] = arm7 ? (core->gbaMode ? 0x3007FA0 : 0x380FF80) : 0x3003F80;
    registersSvc[0] = arm7 ? (core->gbaMode ? 0x3007FE0 : 0x380FFC0) : 0x3003FC0;
    flushPipeline();
}

void Interpreter::resetCycles() {
    // Adjust CPU cycles for a global cycle reset
    cycles -= std::min(core->globalCycles, cycles);
}

void Interpreter::runCoreNone(Core &core) {
    // Run the core with no active CPUs
    while (core.running.exchange(true)) {
        // Jump to the next task and run all that are scheduled now
        EP_START(tasks);
        core.globalCycles = core.events[0].cycles;
        while (core.events[0].cycles <= core.globalCycles) {
            EP_TASK(core);
            core.events.popFront();
        }
        EP_ADD(EP_TASKS, tasks);
    }
}

template void Interpreter::runCoreSingle<false, 0>(Core &core);
template void Interpreter::runCoreSingle<true, 0>(Core &core);
template void Interpreter::runCoreSingle<true, 1>(Core &core);
template <bool _arm7, int shift> void Interpreter::runCoreSingle(Core &core) {
    // Run the core with one active CPU
    Interpreter &arm = core.interpreter[_arm7];
    static const uint32_t noLimit = 0xFFFFFFFF;
    while (core.running.exchange(true)) {
        // Run a CPU until the next scheduled task
        EP_START(cpu);
        core.globalCycles = std::max(core.globalCycles, arm.cycles);

        // The JIT leaves the pipeline empty if the ARM7 entered GBA mode while running compiled code
        if (arm.pipelineStale) arm.refillPipeline();

        while (core.events[0].cycles > arm.cycles) {
            // Run compiled code if possible, falling back to the interpreter; the ARM7 only has it in NDS mode
            // Blocks leave the global cycles at the start of their last instruction, so catch up after
            Jit &jit = _arm7 ? core.jit7 : core.jit;
            if ((!_arm7 || shift) && jit.enabled) {
                if (jit.run(&noLimit)) {
                    core.globalCycles = arm.cycles;
                    continue;
                }
                if (arm.pipelineStale) arm.refillPipeline();
            }
            arm.cycles = (core.globalCycles += arm.runOpcode() << shift);
            if (!_arm7) EP_INC(EC_INTERP9);
        }
        EP_ADD(_arm7 ? EP_ARM7 : EP_ARM9, cpu);

        // Jump to the next task and run all that are scheduled now
        EP_START(tasks);
        core.globalCycles = core.events[0].cycles;
        while (core.events[0].cycles <= core.globalCycles) {
            EP_TASK(core);
            core.events.popFront();
        }
        EP_ADD(EP_TASKS, tasks);
    }
}

void Interpreter::runCoreNds(Core &core) {
    // Run the core with both CPUs active in NDS mode, switching between them every instruction unless timing
    // is allowed to be less exact
    if (Settings::fastTiming)
        return runCoreNdsFast(core);
    Interpreter &arm9 = core.interpreter[0];
    Interpreter &arm7 = core.interpreter[1];
    while (core.running.exchange(true)) {
        // Run the ARM9 and half-speed ARM7 until the next scheduled task
        EP_START(cpu);
        while (core.events[0].cycles > core.globalCycles) {
            // Run compiled ARM9 code if possible, which runs the ARM7 in between and leaves the state as it
            // would be at the top of this loop, or fall back to the interpreter
            if (core.globalCycles >= arm9.cycles) {
                if (core.jit.enabled && core.jit.run(&arm7.cycles)) continue;
                if (arm9.pipelineStale) arm9.refillPipeline();
                arm9.cycles = core.globalCycles + arm9.runOpcode();
                EP_INC(EC_INTERP9);
            }
            if (core.globalCycles >= arm7.cycles) {
                EP_START(a7);
                stepArm7(core);
                EP_ADD(EP_BOTH_ARM7, a7);
            }
            core.globalCycles = std::min<uint32_t>(arm9.cycles, arm7.cycles);
        }
        EP_ADD(EP_BOTH, cpu);

        // Jump to the next task and run all that are scheduled now
        EP_START(tasks);
        core.globalCycles = core.events[0].cycles;
        while (core.events[0].cycles <= core.globalCycles) {
            EP_TASK(core);
            core.events.popFront();
        }
        EP_ADD(EP_TASKS, tasks);
    }
}

void Interpreter::runCoreNdsFast(Core &core) {
    // Run the core with both CPUs active in NDS mode, letting the ARM9 run ahead of the ARM7 by up to a slice
    // The ARM9 still stops at scheduled tasks, so only the ARM7 is behind, and it catches up from its own time
    // This switches between the CPUs much less often, at the cost of timing between them being less exact
    Interpreter &arm9 = core.interpreter[0];
    Interpreter &arm7 = core.interpreter[1];

    // A CPU that was halted keeps the cycles from when it stopped, so bring it up to the current time; otherwise both
    // are already there, since the ARM7 catches up before any task runs
    arm9.cycles = std::max(arm9.cycles, core.globalCycles);
    arm7.cycles = std::max(arm7.cycles, core.globalCycles);

    while (core.running.exchange(true)) {
        // Run the ARM9 ahead and let the ARM7 catch up, until the next scheduled task
        EP_START(cpu);
        while (core.events[0].cycles > core.globalCycles) {
            if (core.globalCycles >= arm9.cycles) {
                // Run compiled ARM9 code if possible, which lets the ARM7 catch up in between and leaves the state as
                // it would be at the top of this loop, or fall back to the interpreter
                core.sliceLimit = arm7.cycles + Core::FAST_SLICE;
                if (core.jit.enabled && core.jit.run(&core.sliceLimit)) continue;
                if (arm9.pipelineStale) arm9.refillPipeline();
                uint32_t next = core.globalCycles + arm9.runOpcode();
                EP_INC(EC_INTERP9);
                arm9.cycles = next;
                if (next <= core.sliceLimit && next < core.events[0].cycles) {
                    core.globalCycles = next;
                    continue;
                }
            }
            EP_START(a7);
            runArm7Behind(core);
            EP_ADD(EP_BOTH_ARM7, a7);
        }
        EP_ADD(EP_BOTH, cpu);

        // Jump to the next task and run all that are scheduled now
        EP_START(tasks);
        core.globalCycles = core.events[0].cycles;
        while (core.events[0].cycles <= core.globalCycles) {
            EP_TASK(core);
            core.events.popFront();
        }
        EP_ADD(EP_TASKS, tasks);
    }
}

bool Interpreter::runArm7Behind(Core &core) {
    // Run the ARM7 from its own time until it reaches the ARM9, or a task is due first
    // Returns whether the ARM9 can continue, with the state otherwise matching the top of the run loop
    Interpreter &arm9 = core.interpreter[0];
    Interpreter &arm7 = core.interpreter[1];
    while (true) {
        core.globalCycles = std::min<uint32_t>(arm9.cycles, arm7.cycles);
        if (core.events[0].cycles <= core.globalCycles) return false;
        if (core.globalCycles >= arm9.cycles) return true;
        stepArm7(core);
    }
}

void Interpreter::stepArm7(Core &core) {
    // Run the ARM7 from the global cycles, which the NDS run loops do while it's behind the ARM9
    // Compiled code keeps going while the ARM7 stays behind and no task is due, which is what the loops would do
    // one instruction at a time; it stops just before the ARM9's cycles, since the ARM9 goes first when they're equal
    // With exact timing, the CPUs take turns every few instructions, which is too often for compiled code to help
    Interpreter &arm7 = core.interpreter[1];
    if (core.jit7.enabled && Settings::fastTiming) {
        core.arm7Limit = core.interpreter[0].cycles - 1;
        if (core.jit7.run(&core.arm7Limit)) return;
    }

    // Compiled code might have run while the ARM7 was alone, leaving the pipeline for the interpreter to fill
    if (arm7.pipelineStale) arm7.refillPipeline();
    arm7.cycles = core.globalCycles + (arm7.runOpcode() << 1);
}

bool Interpreter::runArm7UntilArm9(Core &core) {
    // Continue the NDS run loop after the JIT ran an ARM9 instruction, with the global cycles still at its start
    // The ARM7 runs as it would in the loop, until either the ARM9 is due again, or a task is due first
    // When a task is due, the state matches the top of the loop, so it can carry on from there
    Interpreter &arm9 = core.interpreter[0];
    Interpreter &arm7 = core.interpreter[1];
    if (core.globalCycles >= arm7.cycles)
        stepArm7(core);
    while (true) {
        core.globalCycles = std::min<uint32_t>(arm9.cycles, arm7.cycles);
        if (core.events[0].cycles <= core.globalCycles) return false;
        if (core.globalCycles >= arm9.cycles) return true;
        stepArm7(core);
    }
}

void Interpreter::runCoreDsi(Core &core) {
    // Run the core in DSi mode
    Interpreter &arm9 = core.interpreter[0];
    Interpreter &arm7 = core.interpreter[1];
    while (core.running.exchange(true)) {
        // Run both CPUs until the next scheduled task
        while (core.events[0].cycles > core.globalCycles) {
            // Run the ARM9 twice as fast as usual
            if (core.globalCycles >= arm9.cycles) {
                int cycles = arm9.runOpcode() + arm9.dsiCycle;
                arm9.cycles = core.globalCycles + (cycles >> 1);
                arm9.dsiCycle = (cycles & 0x1);
            }

            // Run the ARM7 at half speed and advance to the next opcode cycle
            if (core.globalCycles >= arm7.cycles)
                arm7.cycles = core.globalCycles + (arm7.runOpcode() << 1);
            core.globalCycles = std::min<uint32_t>(arm9.cycles, arm7.cycles);
        }

        // Jump to the next task and run all that are scheduled now
        core.globalCycles = core.events[0].cycles;
        while (core.events[0].cycles <= core.globalCycles) {
            EP_TASK(core);
            core.events.popFront();
        }
    }
}

FORCE_INLINE int Interpreter::runOpcode() {
    // Push the next opcode through the pipeline
    uint32_t opcode = pipeline[0];
    pipeline[0] = pipeline[1];

    // Execute an instruction
    if (cpsr & BIT(5)) { // THUMB mode
        // Increment the program counter and fill the pipeline from pointer or fallback
        pipeline[1] = (((*registers[15] += 2) & 0xFFE) && pcData) ? U8TO16(pcData += 2, 0) : getOpcode16();

        // Execute a THUMB instruction
        return (this->*thumbInstrs[(opcode >> 6) & 0x3FF])(opcode);
    }
    else { // ARM mode
        // Increment the program counter and fill the pipeline from pointer or fallback
        pipeline[1] = (((*registers[15] += 4) & 0xFFC) && pcData) ? U8TO32(pcData += 4, 0) : getOpcode32();

        // Execute an ARM instruction based on its condition
        switch (condition[((opcode >> 24) & 0xF0) | (cpsr >> 28)]) {
            case 0: return 1; // False
            case 2: return handleReserved(opcode); // Reserved
            default: return (this->*armInstrs[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)])(opcode);
        }
    }
}

uint16_t Interpreter::getOpcode16() {
    // Set the opcode pointer or fall back to a regular 16-bit opcode read
    if (!(pcData = (arm7 ? core->memory.readMap7 : core->memory.readMap9A)[*registers[15] >> 12]))
        return core->memory.read<uint16_t>(arm7, *registers[15]);
    pcData += (*registers[15] & 0xFFE);
    return U8TO16(pcData, 0);
}

uint32_t Interpreter::getOpcode32() {
    // Set the opcode pointer or fall back to a regular 32-bit opcode read
    if (!(pcData = (arm7 ? core->memory.readMap7 : core->memory.readMap9A)[*registers[15] >> 12]))
        return core->memory.read<uint32_t>(arm7, *registers[15]);
    pcData += (*registers[15] & 0xFFC);
    return U8TO32(pcData, 0);
}

std::string Interpreter::compare(Interpreter &other) {
    // Describe the first difference in visible CPU state from another interpreter, for testing the JIT
    char text[64];
    for (int i = 0; i < 16; i++) {
        if (*registers[i] == *other.registers[i]) continue;
        snprintf(text, sizeof(text), "r%d %08X vs %08X", i, *registers[i], *other.registers[i]);
        return text;
    }
    if (cpsr != other.cpsr) snprintf(text, sizeof(text), "CPSR %08X vs %08X", cpsr, other.cpsr);
    else if (cycles != other.cycles) snprintf(text, sizeof(text), "cycles %u vs %u", cycles, other.cycles);
    else if (halted != other.halted) snprintf(text, sizeof(text), "halted %d vs %d", halted, other.halted);
    else return "";
    return text;
}

void Interpreter::halt(int bit) {
    // Set a halt bit and disable the CPU if newly halted
    bool before = halted;
    halted |= BIT(bit);
    if (before) return;
    core->schedule(UPDATE_RUN, 0);
    cycles = 0xFFFFFFFF;
}

void Interpreter::unhalt(int bit) {
    // Clear a halt bit and enable the CPU if newly halted
    bool before = halted;
    halted &= ~BIT(bit);
    if (!before) return;
#ifdef STATE_DEBUG
    if (!halted) wakes++;
#endif
    core->schedule(UPDATE_RUN, 0);
    cycles = 0;
}

void Interpreter::sendInterrupt(int bit) {
    // Set the interrupt's request bit
    irf |= BITL(bit);

    // Trigger an interrupt if the conditions are met, or unhalt the CPU even if interrupts are disabled
    // The ARM9 additionally needs IME to be set for it to unhalt, but the ARM7 doesn't care
    if (ie & irf) {
        if (ime && !(cpsr & BIT(7)))
            core->schedule(SchedTask(ARM9_INTERRUPT + arm7), (arm7 && !core->gbaMode) + 1);
        else if (ime || arm7)
            unhalt(0);
    }
}

void Interpreter::interrupt() {
    // Trigger an interrupt and unhalt the CPU if the conditions still hold
    if (ime && (ie & irf) && !(cpsr & BIT(7))) {
        exception(0x18);
        unhalt(0);
    }
}

int Interpreter::exception(uint8_t vector) {
    // Forward the call to HLE BIOS if enabled, unless on ARM9 with the exception address changed
    if (bios && (arm7 || core->cp15.exceptionAddr))
        return bios->execute(vector, registers);

    // Switch the CPU mode, save the return address, and jump to the exception vector
    static const uint8_t modes[] = { 0x13, 0x1B, 0x13, 0x17, 0x17, 0x13, 0x12, 0x11 };
    setCpsr((cpsr & ~0x3F) | BIT(7) | modes[vector >> 2], true); // ARM, interrupts off, new mode
    *registers[14] = *registers[15] + ((*spsr & BIT(5)) >> 4);
    *registers[15] = (arm7 ? 0 : core->cp15.exceptionAddr) + vector;
    flushPipeline();
    return 3;
}

void Interpreter::refillPipeline() {
    // Fill the pipeline for the current program counter, after the JIT ran instructions without it
    *registers[15] -= (cpsr & BIT(5)) ? 2 : 4;
    flushPipeline();
}

void Interpreter::flushPipeline() {
    // Adjust the program counter and refill the pipeline after a jump
    // While the JIT is running, the pipeline is only marked for refilling if the interpreter needs it
    pipelineStale = lazyPipeline;
    if (cpsr & BIT(5)) { // THUMB mode
        *registers[15] = (*registers[15] & ~0x1) + 2;
        if (lazyPipeline) return;
        pipeline[0] = core->memory.read<uint16_t>(arm7, *registers[15] - 2);
        pipeline[1] = getOpcode16();
    }
    else { // ARM mode
        *registers[15] = (*registers[15] & ~0x3) + 4;
        if (lazyPipeline) return;
        pipeline[0] = core->memory.read<uint32_t>(arm7, *registers[15] - 4);
        pipeline[1] = getOpcode32();
    }
}

void Interpreter::swapRegisters(uint32_t value) {
    // Swap banked registers based on a CPU mode value
    switch (value & 0x1F) {
    case 0x10: // User
    case 0x1F: // System
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersUsr[13];
        registers[14] = &registersUsr[14];
        spsr = nullptr;
        break;

    case 0x11: // FIQ
        registers[8] = &registersFiq[0];
        registers[9] = &registersFiq[1];
        registers[10] = &registersFiq[2];
        registers[11] = &registersFiq[3];
        registers[12] = &registersFiq[4];
        registers[13] = &registersFiq[5];
        registers[14] = &registersFiq[6];
        spsr = &spsrFiq;
        break;

    case 0x12: // IRQ
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersIrq[0];
        registers[14] = &registersIrq[1];
        spsr = &spsrIrq;
        break;

    case 0x13: // Supervisor
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersSvc[0];
        registers[14] = &registersSvc[1];
        spsr = &spsrSvc;
        break;

    case 0x17: // Abort
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersAbt[0];
        registers[14] = &registersAbt[1];
        spsr = &spsrAbt;
        break;

    case 0x1B: // Undefined
        registers[8] = &registersUsr[8];
        registers[9] = &registersUsr[9];
        registers[10] = &registersUsr[10];
        registers[11] = &registersUsr[11];
        registers[12] = &registersUsr[12];
        registers[13] = &registersUnd[0];
        registers[14] = &registersUnd[1];
        spsr = &spsrUnd;
        break;

    default:
        LOG_CRIT("Unknown ARM%d CPU mode: 0x%X\n", arm7 ? 7 : 9, value & 0x1F);
        break;
    }
}

void Interpreter::setCpsr(uint32_t value, bool save) {
    // Update registers if the CPU mode changed
    if ((value & 0x1F) != (cpsr & 0x1F))
        swapRegisters(value);

    // Set the CPSR, saving the old value if requested
    if (save && spsr) *spsr = cpsr;
    cpsr = value;

    // Trigger an interrupt if the conditions are met
    if (ime && (ie & irf) && !(cpsr & BIT(7)))
        core->schedule(SchedTask(ARM9_INTERRUPT + arm7), (arm7 && !core->gbaMode) + 1);
}

void Interpreter::restoreCpsr(uint32_t before) {
    // Restore the status flags when an ALU instruction with flags writes the program counter
    // In modes with no saved flags, a real ARM7 keeps them as they were before, and a real ARM9 clears the condition flags
    if (spsr) setCpsr(*spsr);
    else cpsr = arm7 ? before : (before & 0xFF);
}

int Interpreter::handleReserved(uint32_t opcode) {
    // The ARM9-exclusive BLX instruction uses the reserved condition code, so let it run
    if ((opcode & 0xE000000) == 0xA000000)
        return blx(opcode); // BLX label

    // If the special HLE BIOS opcode was jumped to, return from an HLE interrupt
    if (bios && opcode == 0xFF000000)
        return finishHleIrq();

    // If a DLDI function was jumped to, HLE it and return
    if (core->dldi.isPatched()) {
        uint32_t **r = registers;
        switch (opcode) {
            case DLDI_START: *r[0] = core->dldi.startup(); break;
            case DLDI_INSERT: *r[0] = core->dldi.isInserted(); break;
            case DLDI_READ: *r[0] = core->dldi.readSectors(arm7, *r[0], *r[1], *r[2]); break;
            case DLDI_WRITE: *r[0] = core->dldi.writeSectors(arm7, *r[0], *r[1], *r[2]); break;
            case DLDI_CLEAR: *r[0] = core->dldi.clearStatus(); break;
            case DLDI_STOP: *r[0] = core->dldi.shutdown(); break;
        }
        return bx(14);
    }

    // Treat anything else as an unknown opcode
    return unkArm(opcode);
}

int Interpreter::handleHleIrq() {
    // Switch to IRQ mode, save the return address, and push registers to the stack
    setCpsr((cpsr & ~0x3F) | BIT(7) | 0x12, true);
    *registers[14] = *registers[15] + ((*spsr & BIT(5)) ? 2 : 0);
    stmdbW((13 << 16) | BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(12) | BIT(14));

    // Set the return address to the special HLE BIOS opcode amd jump to the interrupt handler
    *registers[14] = arm7 ? 0x00000000 : 0xFFFF0000;
    *registers[15] = core->memory.read<uint32_t>(arm7, arm7 ? 0x3FFFFFC : (core->cp15.dtcmAddr + 0x3FFC));
    flushPipeline();
    return 3;
}

int Interpreter::finishHleIrq() {
    // Update the wait flags if in the middle of an HLE IntrWait function
    if (bios->shouldCheck())
        bios->checkWaitFlags();

    // Pop registers from the stack, jump to the return address, and restore the mode
    ldmiaW((13 << 16) | BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(12) | BIT(14));
    *registers[15] = *registers[14] - 4;
    if (spsr) setCpsr(*spsr);
    flushPipeline();
    return 3;
}

int Interpreter::unkArm(uint32_t opcode) {
    // Handle an unknown ARM opcode
    LOG_CRIT("Unknown ARM%d ARM opcode: 0x%X\n", arm7 ? 7 : 9, opcode);
    return 1;
}

int Interpreter::unkThumb(uint16_t opcode) {
    // Handle an unknown THUMB opcode
    LOG_CRIT("Unknown ARM%d THUMB opcode: 0x%X\n", arm7 ? 7 : 9, opcode);
    return 1;
}

void Interpreter::writeIme(uint8_t value) {
    // Write to the IME register
    ime = value & 0x01;

    // Trigger an interrupt if the conditions are met
    if (ime && (ie & irf) && !(cpsr & BIT(7)))
        core->schedule(SchedTask(ARM9_INTERRUPT + arm7), (arm7 && !core->gbaMode) + 1);
}

void Interpreter::writeIe(int i, uint32_t mask, uint32_t value) {
    // Write to one of the IE registers
    uint64_t mask2 = (uint64_t(mask) << (i * 32)) & (arm7 ? (core->gbaMode ? 0x3FFF : 0x7FF7FFFFFFFF) : 0xFFFFFF7F);
    ie = (ie & ~mask2) | ((uint64_t(value) << (i * 32)) & mask2);

    // Trigger an interrupt if the conditions are met
    if (ime && (ie & irf) && !(cpsr & BIT(7)))
        core->schedule(SchedTask(ARM9_INTERRUPT + arm7), (arm7 && !core->gbaMode) + 1);
}

void Interpreter::writeIrf(int i, uint32_t mask, uint32_t value) {
    // Clear bits in one of the IF registers
    irf &= ~(uint64_t(value & mask) << (i * 32));

    // The GXFIFO interrupt comes straight back if the FIFO still meets its condition
    if (!arm7) core->gpu3D.checkFifoIrq();
}

void Interpreter::writePostFlg(uint8_t value) {
    // Write to the unclearable POSTFLG bit (with an extra writable bit on ARM9)
    postFlg |= value & 0x01;
    if (!arm7) postFlg = (postFlg & ~0x02) | (value & 0x02);
}
