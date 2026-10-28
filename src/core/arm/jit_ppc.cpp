/*
    Copyright 2019-2026 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

// PowerPC code generation for the JIT, used on Wii U
// Generated blocks do exactly what the portable executor in jit.cpp does, one instruction at a time

#include <cstddef>
#include <cstring>
#ifdef __WIIU__
#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/core.h>
#endif

#include "../core.h"

#if defined(__WIIU__) || defined(JIT_PPC_TEST)

// Registers that hold state while generated code runs; they're saved by the entry code
enum PpcReg {
    R_WMAP = 23, // ARM9 write memory map
    R_RMAP = 24, // ARM9 read memory map
    R_PC = 25, // Program counter of the current instruction, which is only stored when needed
    R_CACHE = 26, // Block cache, with the code changed flag before it
    R_EVENTS = 27, // Scheduler event vector
    R_LAST = 28, // Global cycles at the start of the last instruction
    R_LIMIT = 29, // Pointer to the cycle limit, normally the ARM7's cycles
    R_GLOBAL = 30, // Pointer to the global cycles
    R_ARM = 31 // ARM9 interpreter
};

// Writes PowerPC instructions to memory
class PpcEmitter {
public:
    uint32_t *ptr;

    PpcEmitter(void *ptr): ptr((uint32_t*)ptr) {}

    void emit(uint32_t value) { *ptr++ = value; }
    uint32_t *here() { return ptr; }

    // D-form instructions
    void d(int op, int rt, int ra, int32_t imm) { emit((op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)); }
    void addi(int rt, int ra, int32_t si) { d(14, rt, ra, si); }
    void li(int rt, int32_t si) { d(14, rt, 0, si); }
    void lis(int rt, int32_t si) { d(15, rt, 0, si); }
    void ori(int ra, int rs, uint32_t ui) { d(24, rs, ra, ui); }
    void lwz(int rt, int32_t disp, int ra) { d(32, rt, ra, disp); }
    void stw(int rs, int32_t disp, int ra) { d(36, rs, ra, disp); }
    void stwu(int rs, int32_t disp, int ra) { d(37, rs, ra, disp); }
    void lmw(int rt, int32_t disp, int ra) { d(46, rt, ra, disp); }
    void stmw(int rs, int32_t disp, int ra) { d(47, rs, ra, disp); }
    void cmpwi(int ra, int32_t si) { d(11, 0, ra, si); }
    void oris(int ra, int rs, uint32_t ui) { d(25, rs, ra, ui); }
    void subfic(int rt, int ra, int32_t si) { d(8, rt, ra, si); }

    // X-form instructions
    void x(int rt, int ra, int rb, int xo) { emit((31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1)); }
    void add(int rt, int ra, int rb) { x(rt, ra, rb, 266); }
    void addc(int rt, int ra, int rb) { x(rt, ra, rb, 10); }
    void adde(int rt, int ra, int rb) { x(rt, ra, rb, 138); }
    void subf(int rt, int ra, int rb) { x(rt, ra, rb, 40); } // rt = rb - ra
    void subfc(int rt, int ra, int rb) { x(rt, ra, rb, 8); } // rt = rb - ra, with carry
    void or_(int ra, int rs, int rb) { x(rs, ra, rb, 444); }
    void and_(int ra, int rs, int rb) { x(rs, ra, rb, 28); }
    void xor_(int ra, int rs, int rb) { x(rs, ra, rb, 316); }
    void andc(int ra, int rs, int rb) { x(rs, ra, rb, 60); }
    void nor(int ra, int rs, int rb) { x(rs, ra, rb, 124); }
    void eqv(int ra, int rs, int rb) { x(rs, ra, rb, 284); }
    void cntlzw(int ra, int rs) { x(rs, ra, 0, 26); }
    void lwzx(int rt, int ra, int rb) { x(rt, ra, rb, 23); }
    void stbx(int rs, int ra, int rb) { x(rs, ra, rb, 215); }
    void lwbrx(int rt, int ra, int rb) { x(rt, ra, rb, 534); }
    void stwbrx(int rs, int ra, int rb) { x(rs, ra, rb, 662); }
    void rlwnm(int ra, int rs, int rb, int mb, int me) {
        emit((23 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (mb << 6) | (me << 1));
    }
    void mr(int ra, int rs) { or_(ra, rs, rs); }
    void lbzx(int rt, int ra, int rb) { x(rt, ra, rb, 87); }
    void cmpw(int ra, int rb) { x(0, ra, rb, 0); }
    void cmplw(int ra, int rb) { x(0, ra, rb, 32); }
    void rlwinm(int ra, int rs, int sh, int mb, int me) {
        emit((21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1));
    }

    // Special registers and branches
    void mflr(int rt) { emit(0x7C0802A6 | (rt << 21)); }
    void mtlr(int rs) { emit(0x7C0803A6 | (rs << 21)); }
    void mtctr(int rs) { emit(0x7C0903A6 | (rs << 21)); }
    void bctr() { emit(0x4E800420); }
    void bctrl() { emit(0x4E800421); }
    void blr() { emit(0x4E800020); }

    static bool inRange(uint32_t *from, void *to) {
        intptr_t offset = (intptr_t)to - (intptr_t)from;
        return offset >= -0x2000000 && offset < 0x2000000;
    }
    void b(void *to, bool link = false) {
        emit((18 << 26) | ((uint32_t)((intptr_t)to - (intptr_t)ptr) & 0x3FFFFFC) | link);
    }
    void bc(int bo, int bi, void *to) {
        emit((16 << 26) | (bo << 21) | (bi << 16) | ((uint32_t)((intptr_t)to - (intptr_t)ptr) & 0xFFFC));
    }
    void beq(void *to) { bc(12, 2, to); }
    void bne(void *to) { bc(4, 2, to); }
    void bgt(void *to) { bc(12, 1, to); }
    void bge(void *to) { bc(4, 0, to); }

    // Patch a branch emitted earlier to point here
    void patchBc(uint32_t *at) { *at = (*at & ~0xFFFC) | ((uint32_t)((intptr_t)ptr - (intptr_t)at) & 0xFFFC); }
    void patchB(uint32_t *at) { *at = (*at & ~0x3FFFFFC) | ((uint32_t)((intptr_t)ptr - (intptr_t)at) & 0x3FFFFFC); }

    // Load a 32-bit constant in as few instructions as possible
    void loadConst(int rt, uint32_t value) {
        if ((int32_t)value == (int16_t)value) {
            li(rt, (int16_t)value);
            return;
        }
        lis(rt, value >> 16);
        if (value & 0xFFFF) ori(rt, rt, value & 0xFFFF);
    }

    // Call a function at a 32-bit address
    void call(uint32_t func) { call((void*)(uintptr_t)func); }

    // Call a function, directly if it's in range
    void call(void *func) {
        if (inRange(ptr, func)) {
            b(func, true);
            return;
        }
        loadConst(12, (uint32_t)(uintptr_t)func);
        mtctr(12);
        bctrl();
    }
};

// The most words of host code generated for one instruction
static const int MAX_INSTR_WORDS = 48;

// Get the code address of a non-virtual member function
template <typename T> static void *memberAddress(T func) {
    struct { uintptr_t ptr; ptrdiff_t adj; } parts;
    static_assert(sizeof(parts) == sizeof(func), "Unexpected member function pointer size");
    memcpy(&parts, &func, sizeof(parts));
    return (parts.ptr & 1 || parts.adj) ? nullptr : (void*)parts.ptr;
}

#ifdef __WIIU__
static_assert(sizeof(JitCacheEntry) == 16, "Generated code expects 16-byte cache entries");
#endif

// Memory accesses for native loads and stores that miss the memory maps, done like the interpreter
static uint32_t jitLoad32(Memory *memory, uint32_t address) {
    uint32_t value = memory->read<uint32_t>(false, address);
    uint8_t shift = (address & 0x3) << 3;
    return shift ? ((value << (32 - shift)) | (value >> shift)) : value;
}

static uint32_t jitLoad8(Memory *memory, uint32_t address) {
    return memory->read<uint8_t>(false, address);
}

static void jitStore32(Memory *memory, uint32_t address, uint32_t value) {
    memory->write<uint32_t>(false, address, value);
}

static void jitStore8(Memory *memory, uint32_t address, uint32_t value) {
    memory->write<uint8_t>(false, address, value);
}

// Only one JIT can own the codegen area at a time
static Jit *codeOwner = nullptr;

void Jit::initCodegen() {
    if (codeArea) return;
#ifdef __WIIU__
    // Get the codegen area, which can only be used from one CPU core
    // Try again later if this thread isn't on that core yet, and give up for anything else
    if (OSGetCoreId() != (OSGetCodegenCore() & 3)) return;
    void *area = nullptr;
    uint32_t size = 0;
    OSGetCodegenVirtAddrRange(&area, &size);
    JLOG("init: core %u, codegen core %X, area %p size %X, owner %p, mode %X, sec %X", OSGetCoreId(),
        OSGetCodegenCore(), area, size, codeOwner, OSGetCodegenMode(), OSGetSecCodeGenMode());
    codeFailed = true;
    if (!area || size < 0x100000 || codeOwner) return;

    // Make sure the event vector can be read directly, as generated code does
    if (*(SchedEvent**)&core->events != core->events.data()) return;
    BOOL rw = OSSwitchSecCodeGenMode(CODEGEN_RW_);
    JLOG("init: switch to RW returned %X, sec %X", rw, OSGetSecCodeGenMode());
    if (!rw) return;
    codeFailed = false;
#else
    // When testing on other platforms, generate code into normal memory without running it
    uint32_t size = 0x2000000;
    void *area = new uint8_t[size];
#endif
    codeOwner = this;
#ifdef __WIIU__
    codeCore = OSGetCoreId();
#endif
    codeArea = (uint8_t*)area;
    codeEnd = codeArea + size;

    // Get offsets of the interpreter state that generated code uses
    Interpreter &arm = core->interpreter[0];
    offPc = (uint8_t*)&arm.registersUsr[15] - (uint8_t*)&arm;
    offCpsr = (uint8_t*)&arm.cpsr - (uint8_t*)&arm;
    offCycles = (uint8_t*)&arm.cycles - (uint8_t*)&arm;
    offEventCycles = offsetof(SchedEvent, cycles);
    offUsr = (uint8_t*)&arm.registersUsr[0] - (uint8_t*)&arm;
    offRegs = (uint8_t*)&arm.registers[0] - (uint8_t*)&arm;
    addrCondition = (uint32_t)(uintptr_t)&Interpreter::condition[0];
    addrMemory = (uint32_t)(uintptr_t)&core->memory;
    addrReadMap = (uint32_t)(uintptr_t)&core->memory.readMap9A[0];
    addrWriteMap = (uint32_t)(uintptr_t)&core->memory.writeMap9A[0];
    addrLoad32 = (uint32_t)(uintptr_t)&jitLoad32;
    addrLoad8 = (uint32_t)(uintptr_t)&jitLoad8;
    addrStore32 = (uint32_t)(uintptr_t)&jitStore32;
    addrStore8 = (uint32_t)(uintptr_t)&jitStore8;
    PpcEmitter e(codeArea);
    emitStubs(e);
    codeStart = (uint8_t*)e.here();
    codePtr = codeStart;
    finishCode(codeArea, codePtr - codeArea);
}

void Jit::emitStubs(PpcEmitter &e) {
    // Exit: restore saved registers and return to the caller
    codeExit = e.here();
    e.lwz(0, 68, 1);
    e.mtlr(0);
    e.lmw(R_WMAP, 24, 1);
    e.addi(1, 1, 64);
    e.blr();

    // Entry: save registers, load the state pointers, and jump to the block, which sets its program counter
    // Arguments are the interpreter, event vector, limit, block code, global cycles, and cache
    codeEnter = e.here();
    e.stwu(1, -64, 1);
    e.mflr(0);
    e.stw(0, 68, 1);
    e.stmw(R_WMAP, 24, 1);
    e.mr(R_ARM, 3);
    e.mr(R_EVENTS, 4);
    e.mr(R_LIMIT, 5);
    e.mr(R_GLOBAL, 7);
    e.mr(R_CACHE, 8);
    e.loadConst(R_RMAP, addrReadMap);
    e.loadConst(R_WMAP, addrWriteMap);
    e.mtctr(6);
    e.bctr();

    // When the next instruction can't run straight away, call the interleave helper, which runs the ARM7
    // like the interpreter would and says whether to continue; it needs a stack frame to keep the return address
    // The first version stores the program counter first, for instructions that don't set it themselves
    codeSlowPost[1] = e.here();
    e.stw(R_PC, offPc, R_ARM);
    codeSlowPost[0] = e.here();
    e.stwu(1, -16, 1);
    e.mflr(0);
    e.stw(0, 20, 1);
    e.loadConst(3, (uint32_t)(uintptr_t)this);
    e.mr(4, R_LIMIT);
    e.call((void*)&Jit::interleaveCall);
    e.lwz(0, 20, 1);
    e.mtlr(0);
    e.addi(1, 1, 16);
    e.cmpwi(3, 0);
    e.beq(codeExit);
    e.blr();

    // After an instruction, with its cycles in r3: update the ARM9's cycles, then continue if nothing else
    // needs to run first; the global cycles are only advanced here, since the helper expects them at the start
    // of the last instruction; impure versions also check if code changed, since the block might be stale
    for (int store = 0; store < 2; store++) {
        for (int impure = 0; impure < 2; impure++) {
            codePost[impure][store] = e.here();
            e.lwz(R_LAST, 0, R_GLOBAL);
            e.add(3, 3, R_LAST);
            e.stw(3, offCycles, R_ARM);
            if (impure) {
                e.lwz(4, -int(sizeof(JitCacheEntry)), R_CACHE);
                e.cmpwi(4, 0);
                e.bne(codeSlowPost[store]);
            }
            e.lwz(4, 0, R_LIMIT);
            e.cmplw(3, 4);
            e.bgt(codeSlowPost[store]);
            e.lwz(4, 0, R_EVENTS);
            e.lwz(4, offEventCycles, 4);
            e.cmplw(3, 4);
            e.bge(codeSlowPost[store]);
            e.stw(3, 0, R_GLOBAL);
            e.blr();
        }
    }

    // Dispatch: find the next block in the cache from the program counter and jump to it
    // On a miss, exit with the state as it is, which matches the top of the run loop
    codeDispatch = e.here();
    e.lwz(3, offCpsr, R_ARM);
    e.rlwinm(3, 3, 27, 31, 31); // THUMB bit
    e.lwz(4, offPc, R_ARM);
    e.addi(4, 4, -4);
    e.rlwinm(5, 3, 1, 0, 30);
    e.add(4, 4, 5); // Address of the next instruction
    e.or_(5, 4, 3); // Cache key
    e.rlwinm(6, 4, 3, 16, 27); // Cache index * 16
    e.add(6, 6, R_CACHE);
    e.lwz(7, 0, 6);
    e.cmpw(7, 5);
    uint32_t *miss1 = e.here();
    e.bne(nullptr);
    e.lwz(7, 4, 6);
    e.cmpwi(7, 0);
    uint32_t *miss2 = e.here();
    e.beq(nullptr);
    e.mtctr(7);
    e.bctr();
    e.patchBc(miss1);
    e.patchBc(miss2);
    e.b(codeExit);

    // Conditions: set cr0 to equal if an ARM condition passes, from the interpreter's lookup table
    for (int cond = 0; cond < 14; cond++) {
        codeCond[cond] = e.here();
        e.lwz(3, offCpsr, R_ARM);
        e.rlwinm(3, 3, 4, 28, 31);
        e.loadConst(4, addrCondition + cond * 16);
        e.lbzx(3, 4, 3);
        e.cmpwi(3, 1);
        e.blr();
    }

    // Flags for logical operations, from the result in r5; the second version also sets C from r7
    for (int carry = 0; carry < 2; carry++) {
        codeFlags[carry] = e.here();
        e.rlwinm(8, 5, 0, 0, 0);
        e.cntlzw(9, 5);
        e.rlwinm(9, 9, 25, 1, 1);
        e.or_(8, 8, 9);
        e.lwz(6, offCpsr, R_ARM);
        if (carry) {
            e.rlwinm(7, 7, 29, 2, 2);
            e.or_(8, 8, 7);
            e.rlwinm(6, 6, 0, 3, 31);
        }
        else {
            e.rlwinm(6, 6, 0, 2, 31);
        }
        e.or_(6, 6, 8);
        e.stw(6, offCpsr, R_ARM);
        e.blr();
    }

    // Flags for arithmetic, from the result in r5, the carry out, and the operands in r3 and r4
    // Versions are for addition, subtraction of r4 from r3, and subtraction of r3 from r4
    for (int type = 0; type < 3; type++) {
        codeFlags[2 + type] = e.here();
        e.li(9, 0);
        e.adde(9, 9, 9);
        e.rlwinm(9, 9, 29, 2, 2);
        e.rlwinm(8, 5, 0, 0, 0);
        e.or_(8, 8, 9);
        e.cntlzw(9, 5);
        e.rlwinm(9, 9, 25, 1, 1);
        e.or_(8, 8, 9);
        if (type == 0) {
            e.eqv(10, 4, 3);
            e.xor_(11, 5, 4);
            e.and_(10, 10, 11);
        }
        else {
            int sub = (type == 1) ? 4 : 3, min = (type == 1) ? 3 : 4;
            e.xor_(10, sub, min);
            e.xor_(11, 5, sub);
            e.andc(10, 10, 11);
        }
        e.rlwinm(10, 10, 29, 3, 3);
        e.or_(8, 8, 10);
        e.lwz(6, offCpsr, R_ARM);
        e.rlwinm(6, 6, 0, 4, 31);
        e.or_(6, 6, 8);
        e.stw(6, offCpsr, R_ARM);
        e.blr();
    }

    // Memory accesses at the address in r5, with the value in r8; mapped memory is accessed directly, and
    // anything else goes through the memory handlers, which also catch writes to compiled code
    for (int type = 0; type < 4; type++) {
        bool load = type < 2, byte = type & 1;
        codeMemory[type] = e.here();
        e.rlwinm(6, 5, 22, 10, 29);
        e.lwzx(6, 6, load ? R_RMAP : R_WMAP);
        e.cmpwi(6, 0);
        uint32_t *slow = e.here();
        e.beq(nullptr);
        e.rlwinm(7, 5, 0, 20, byte ? 31 : 29);
        if (load && byte) {
            e.lbzx(8, 6, 7);
        }
        else if (load) {
            // Rotate misaligned words like the interpreter
            e.lwbrx(8, 6, 7);
            e.rlwinm(9, 5, 3, 27, 28);
            e.subfic(9, 9, 32);
            e.rlwnm(8, 8, 9, 0, 31);
        }
        else {
            byte ? e.stbx(8, 6, 7) : e.stwbrx(8, 6, 7);
        }
        e.blr();

        e.patchBc(slow);
        e.stwu(1, -16, 1);
        e.mflr(0);
        e.stw(0, 20, 1);
        e.loadConst(3, addrMemory);
        e.mr(4, 5);
        if (!load) e.mr(5, 8);
        e.call(load ? (byte ? addrLoad8 : addrLoad32) : (byte ? addrStore8 : addrStore32));
        if (load) e.mr(8, 3);
        e.lwz(0, 20, 1);
        e.mtlr(0);
        e.addi(1, 1, 16);
        e.blr();
    }
}

void Jit::freeCodegen() {
    // Let another JIT use the codegen area
    if (codeOwner != this) return;
#ifndef __WIIU__
    delete[] codeArea;
#endif
    codeOwner = nullptr;
    codeArea = codeStart = codePtr = codeEnd = nullptr;
}

void Jit::finishCode(uint8_t *start, size_t size) {
#ifdef __WIIU__
    // Write the code to memory and make the area executable again
    DCFlushRange(start, size);
    BOOL rx = OSSwitchSecCodeGenMode(CODEGEN_R_X);
    ICInvalidateRange(start, size);
#ifdef JIT_LOG
    static uint32_t count = 0;
    if (++count <= 200 || (count % 10000) == 0)
        JLOG("code %u: %p size %X, switch to RX returned %X, sec %X", count, start, (uint32_t)size, rx, OSGetSecCodeGenMode());
#endif
#endif
}

bool Jit::codeFull() {
    // Check if a block of the largest size might not fit in the codegen area
    return codeArea && codeEnd - codePtr < 64 * MAX_INSTR_WORDS * 4 + 16;
}

void Jit::readReg(PpcEmitter &e, int rt, int reg, uint32_t pc) {
    // Load an ARM register, reading the program counter as a constant
    // Only registers 8-14 can be banked, so the others are read directly
    if (reg == 15) {
        e.loadConst(rt, pc);
    }
    else if (reg < 8) {
        e.lwz(rt, offUsr + reg * 4, R_ARM);
    }
    else {
        e.lwz(rt, offRegs + reg * 4, R_ARM);
        e.lwz(rt, 0, rt);
    }
}

void Jit::writeReg(PpcEmitter &e, int rs, int reg) {
    // Store an ARM register other than the program counter, using r12 for banked ones
    if (reg < 8) {
        e.stw(rs, offUsr + reg * 4, R_ARM);
    }
    else {
        e.lwz(12, offRegs + reg * 4, R_ARM);
        e.stw(rs, 0, 12);
    }
}

static int nativeKind(uint32_t op) {
    // Check which kind of native code an ARM instruction can be generated as, or 0 for none
    // Anything that writes the program counter, other than branches, is left to the interpreter
    if ((op >> 28) == 0xF) return 0;
    uint32_t rd = (op >> 12) & 0xF;
    if ((op & 0x0C000000) == 0x00000000) { // Data processing
        uint32_t opc = (op >> 21) & 0xF;
        bool s = op & BIT(20);
        if (!(op & BIT(25)) && (op & 0x70) != 0) return 0; // Only LSL by immediate for registers
        if (opc >= 8 && opc <= 11) return s ? 1 : 0; // Compares and tests, other encodings are misc
        if (opc >= 5 && opc <= 7 && s) return 0; // Flag-setting ADC/SBC/RSC
        return (rd == 15) ? 0 : 1;
    }
    if ((op & 0x0E000000) == 0x0A000000) // B/BL
        return 2;
    if ((op & 0x0C000000) == 0x04000000) { // LDR/STR/LDRB/STRB
        if ((op & 0x01200000) != 0x01000000) return 0; // Only offsets without writeback
        if ((op & BIT(25)) && (op & 0x70) != 0) return 0; // Only LSL by immediate for registers
        return (rd == 15) ? 0 : 3;
    }
    return 0;
}

bool Jit::emitNative(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Generate an ARM instruction as native code if possible, leaving its cycles in r3
    // Branches leave the program counter set in memory; other instructions leave it unchanged
    int kind = nativeKind(op);
    if (!kind) return false;

    // Skip the instruction if its condition fails, which takes 1 cycle
    uint32_t *skip = nullptr;
    if ((op >> 28) != 0xE) {
        e.call(codeCond[op >> 28]);
        skip = e.here();
        e.bne(nullptr);
    }

    switch (kind) {
        case 1: emitAlu(e, op, pc); break;
        case 2: emitBranch(e, op, pc); break;
        case 3: emitTransfer(e, op, pc); break;
    }

    if (skip) {
        uint32_t *done = e.here();
        e.b(nullptr);
        e.patchBc(skip);
        if (kind == 2) e.stw(R_PC, offPc, R_ARM);
        e.li(3, 1);
        e.patchB(done);
    }
    return true;
}

void Jit::emitAlu(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Data processing with an immediate or a register shifted left by an immediate
    // Operands go in r3 and r4, the result in r5, and a shifter carry in r7 for flag-setting logical operations
    uint32_t opc = (op >> 21) & 0xF;
    bool s = op & BIT(20);
    bool logical = (opc <= 1) || (opc >= 8 && opc <= 9) || opc >= 12;
    if (opc != 13 && opc != 15)
        readReg(e, 3, (op >> 16) & 0xF, pc);

    // Get the second operand and the shifter carry, if it changes
    bool carry = false;
    if (op & BIT(25)) {
        uint32_t shift = (op >> 7) & 0x1E;
        uint32_t value = ((op & 0xFF) << (32 - shift)) | ((op & 0xFF) >> shift);
        if (shift == 0) value = op & 0xFF;
        e.loadConst(4, value);
        if (shift > 0 && s && logical) {
            e.li(7, value >> 31);
            carry = true;
        }
    }
    else {
        readReg(e, 4, op & 0xF, pc);
        uint32_t shift = (op >> 7) & 0x1F;
        if (shift > 0) {
            if (s && logical) {
                e.rlwinm(7, 4, shift, 31, 31);
                carry = true;
            }
            e.rlwinm(4, 4, shift, 0, 31 - shift);
        }
    }

    // Get the carry flag for operations that use it
    if (opc >= 5 && opc <= 7) {
        e.lwz(6, offCpsr, R_ARM);
        e.rlwinm(6, 6, 3, 31, 31);
    }

    // Do the operation, with carry out for flag-setting arithmetic
    bool arith = s && !logical;
    switch (opc) {
        case 0x0: case 0x8: e.and_(5, 3, 4); break; // AND/TST
        case 0x1: case 0x9: e.xor_(5, 3, 4); break; // EOR/TEQ
        case 0x2: case 0xA: arith ? e.subfc(5, 4, 3) : e.subf(5, 4, 3); break; // SUB/CMP
        case 0x3: arith ? e.subfc(5, 3, 4) : e.subf(5, 3, 4); break; // RSB
        case 0x4: case 0xB: arith ? e.addc(5, 3, 4) : e.add(5, 3, 4); break; // ADD/CMN
        case 0x5: e.add(5, 3, 4); e.add(5, 5, 6); break; // ADC
        case 0x6: e.subf(5, 4, 3); e.add(5, 5, 6); e.addi(5, 5, -1); break; // SBC
        case 0x7: e.subf(5, 3, 4); e.add(5, 5, 6); e.addi(5, 5, -1); break; // RSC
        case 0xC: e.or_(5, 3, 4); break; // ORR
        case 0xD: e.mr(5, 4); break; // MOV
        case 0xE: e.andc(5, 3, 4); break; // BIC
        case 0xF: e.nor(5, 4, 4); break; // MVN
    }

    // Set flags with a shared routine
    if (s) {
        if (logical)
            e.call(codeFlags[carry]);
        else
            e.call(codeFlags[(opc == 0x4 || opc == 0xB) ? 2 : (opc == 0x3) ? 4 : 3]);
    }

    // Store the result unless it's a compare or test
    if (opc < 8 || opc > 11)
        writeReg(e, 5, (op >> 12) & 0xF);
    e.li(3, 1);
}

void Jit::emitBranch(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Branch with optional link, setting the program counter as if the pipeline was flushed
    uint32_t target = pc + ((int32_t)(op << 8) >> 6);
    if (op & BIT(24)) {
        e.loadConst(5, pc - 4);
        writeReg(e, 5, 14);
    }
    e.loadConst(5, (target & ~0x3) + 4);
    e.stw(5, offPc, R_ARM);
    e.li(3, 3);
}

void Jit::emitTransfer(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Word or byte load or store at an offset without writeback, with the address in r5 and value in r8
    uint32_t rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
    bool up = op & BIT(23), byte = op & BIT(22), load = op & BIT(20);
    if (!(op & BIT(25))) {
        uint32_t offset = op & 0xFFF;
        if (rn == 15) {
            e.loadConst(5, up ? (pc + offset) : (pc - offset));
        }
        else {
            readReg(e, 3, rn, pc);
            e.addi(5, 3, up ? offset : -offset);
        }
    }
    else {
        readReg(e, 3, rn, pc);
        readReg(e, 4, op & 0xF, pc);
        uint32_t shift = (op >> 7) & 0x1F;
        if (shift) e.rlwinm(4, 4, shift, 0, 31 - shift);
        up ? e.add(5, 3, 4) : e.subf(5, 4, 3);
    }

    // Access memory with a shared routine
    if (!load) readReg(e, 8, rd, pc);
    e.call(codeMemory[(load ? 0 : 2) + byte]);
    if (load) writeReg(e, 8, rd);
    e.li(3, 1);
}

static bool isPure(uint32_t op, bool thumb) {
    // Check if an instruction can't write memory or remap it, so code can't change under it
    if (thumb) {
        if (op < 0x4800) return true; // Shifts, immediate and register ALU operations
        if ((op & 0xF000) == 0xA000) return true; // ADD PC/SP
        if ((op & 0xFF00) == 0xB000) return true; // ADD SP
        if ((op & 0xF000) == 0xD000) return (op & 0xFF00) != 0xDF00; // Conditional branches, not SWI
        return op >= 0xE000; // Branches
    }
    if ((op & 0x0E000000) == 0x02000000) return true; // Data processing with immediate
    if ((op & 0x0E000090) != 0x00000090 && (op & 0x0C000000) == 0) return true; // Data processing and misc
    if ((op & 0x0FC000F0) == 0x00000090) return true; // MUL/MLA
    if ((op & 0x0F8000F0) == 0x00800090) return true; // Long multiplies
    return (op & 0x0E000000) == 0x0A000000; // B/BL
}

static bool mayJump(uint32_t op, bool thumb) {
    // Check if an instruction might change the program counter, even if it doesn't always
    if (thumb) {
        if ((op & 0xF000) == 0xD000) return true; // Conditional branches and SWI
        return op >= 0xE000 && (op & 0xF800) != 0xF000; // Branches, except the first half of BL
    }
    if ((op >> 28) == 0xF) return true; // Reserved condition, including BLX label
    if ((op & 0x0F000000) == 0x0F000000) return true; // SWI
    if ((op & 0x0E000000) == 0x0A000000) return true; // B/BL
    if ((op & 0x0FFFFFD0) == 0x012FFF10) return true; // BX/BLX
    if ((op & 0x0C00F000) == 0x0000F000) return true; // Data processing with Rd = PC
    if ((op & 0x0C10F000) == 0x0410F000) return true; // LDR PC
    if ((op & 0x0E108000) == 0x08108000) return true; // LDM with PC
    return (op & 0x0E000010) == 0x06000010; // Undefined
}

void Jit::emitInstr(PpcEmitter &e, JitBlock *block, size_t i, uint32_t pc, bool &ok) {
    // Generate one instruction; the program counter register already holds its value
    uint32_t op = block->instrs[i].opcode;
    bool native = !block->thumb && emitNative(e, op, pc);
    bool memoryPc = !native || nativeKind(op) == 2;

    if (!native) {
        // Call the interpreter handler, with the program counter stored for it to read
        void *handler = block->thumb ? memberAddress(block->instrs[i].thumb) : memberAddress(block->instrs[i].arm);
        void *reserved = memberAddress(&Interpreter::handleReserved);
        if (!handler || !reserved) {
            ok = false;
            return;
        }
        e.stw(R_PC, offPc, R_ARM);
        uint32_t cond = op >> 28;
        uint32_t *skip = nullptr;
        if (!block->thumb && cond < 0xE) {
            e.call(codeCond[cond]);
            skip = e.here();
            e.bne(nullptr);
        }
        e.mr(3, R_ARM);
        e.loadConst(4, op);
        e.call((!block->thumb && cond == 0xF) ? reserved : handler);
        if (skip) {
            uint32_t *done = e.here();
            e.b(nullptr);
            e.patchBc(skip);
            e.li(3, 1);
            e.patchB(done);
        }
    }

    // Count cycles and check whether to stop; the program counter is stored first if it's only in the register
    // Then check for jumps, which leave the program counter in memory different from the register
    e.call(codePost[!isPure(op, block->thumb)][!memoryPc]);
    if (i + 1 < block->instrs.size() && mayJump(op, block->thumb)) {
        e.lwz(4, offPc, R_ARM);
        e.cmplw(4, R_PC);
        uint32_t *same = e.here();
        e.beq(nullptr);
        e.b(codeDispatch);
        e.patchBc(same);
    }

    // Leave the program counter in memory after the last instruction, for the dispatcher
    if (i + 1 == block->instrs.size() && !memoryPc)
        e.stw(R_PC, offPc, R_ARM);
}

void *Jit::generate(JitBlock *block) {
    if (!codeArea) return nullptr;

    // Make sure there's room for the block, starting over if the area is full
    size_t maxSize = block->instrs.size() * MAX_INSTR_WORDS * 4 + 16;
    if (codePtr + maxSize > codeEnd) return nullptr;
#ifdef __WIIU__
    // The codegen area can only be touched from its core; blocks use the portable executor otherwise
    if (OSGetCoreId() != codeCore || !OSSwitchSecCodeGenMode(CODEGEN_RW_)) return nullptr;
#endif

    // Set the program counter register at the start, then step it before each instruction
    // Record where each instruction starts after stepping, so other blocks can jump into the middle
    PpcEmitter e(codePtr);
    uint32_t step = block->thumb ? 2 : 4;
    uint32_t pc = block->address + step * 2;
    e.loadConst(R_PC, pc);
    block->entries.clear();
    bool ok = true;
    for (size_t i = 0; i < block->instrs.size() && ok; i++, pc += step) {
        if (i > 0) e.addi(R_PC, R_PC, step);
        block->entries.push_back((uint8_t*)e.here() - codePtr);
        emitInstr(e, block, i, pc, ok);
    }

    // Continue to whatever block comes next
    e.b(codeDispatch);

    uint8_t *code = codePtr;
    size_t size = (uint8_t*)e.here() - code;
    finishCode(code, size);
    if (!ok) return nullptr;
    codePtr += (size + 15) & ~15;
    return code;
}

void *Jit::generateEntry(JitBlock *host, size_t index) {
    // Generate a small entry point into the middle of another block's code, instead of compiling it again
    if (!codeArea || !host->code || index >= host->entries.size() || codePtr + 64 > codeEnd) return nullptr;
#ifdef __WIIU__
    if (OSGetCoreId() != codeCore || !OSSwitchSecCodeGenMode(CODEGEN_RW_)) return nullptr;
#endif
    PpcEmitter e(codePtr);
    e.loadConst(R_PC, host->address + (index + 2) * (host->thumb ? 2 : 4));
    e.b((uint8_t*)host->code + host->entries[index]);
    uint8_t *code = codePtr;
    size_t size = (uint8_t*)e.here() - code;
    finishCode(code, size);
    codePtr += (size + 15) & ~15;
    return code;
}

#else

void Jit::initCodegen() {
    // There's no native code backend on this platform, so blocks use the portable executor
    codeFailed = true;
}

void Jit::freeCodegen() {}

bool Jit::codeFull() {
    return false;
}

void Jit::finishCode(uint8_t *start, size_t size) {}

void *Jit::generate(JitBlock *block) {
    return nullptr;
}

void *Jit::generateEntry(JitBlock *host, size_t index) {
    return nullptr;
}

#endif

#ifdef JIT_PPC_TEST
// Generate the shared routines and then one native instruction, for testing generated code in an emulator
// The buffer is loaded at a fixed address, and the instruction starts at the returned offset
bool jitTestEmit(Jit &jit, uint8_t *buf, uint32_t op, uint32_t pc, uint32_t *size, uint32_t *start) {
    PpcEmitter e(buf);
    jit.emitStubs(e);
    *start = (uint8_t*)e.here() - buf;
    if (!jit.emitNative(e, op, pc)) return false;
    e.emit(0x48000002 | 0x50000); // Absolute branch to the test's end address, since calls change LR
    *size = (uint8_t*)e.here() - buf;
    for (uint32_t *p = (uint32_t*)buf; p < e.here(); p++) *p = __builtin_bswap32(*p);
    return true;
}
#endif
