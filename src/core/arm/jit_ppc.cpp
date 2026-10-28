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
static const int MAX_INSTR_WORDS = 80;

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

    // Exit: restore saved registers and return to the caller
    codeExit = e.here();
    e.lwz(0, 52, 1);
    e.mtlr(0);
    e.lmw(R_CACHE, 24, 1);
    e.addi(1, 1, 48);
    e.blr();

    // Entry: save registers, load the state pointers, and jump to the block
    // Arguments are the interpreter, event vector, limit, block code, global cycles, and cache
    codeEnter = e.here();
    e.stwu(1, -48, 1);
    e.mflr(0);
    e.stw(0, 52, 1);
    e.stmw(R_CACHE, 24, 1);
    e.mr(R_ARM, 3);
    e.mr(R_EVENTS, 4);
    e.mr(R_LIMIT, 5);
    e.mr(R_GLOBAL, 7);
    e.mr(R_CACHE, 8);
    e.mtctr(6);
    e.bctr();

    // After an instruction, with its cycles in r3: update the ARM9's cycles, then return to continue
    // or exit if the interpreter would switch to the ARM7 or a task first; the global cycles are only
    // advanced when continuing, since the interpreter leaves them at the start of the last instruction
    // The second version also exits if code changed, since the running block might be stale
    codePost[1] = e.here();
    e.lwz(R_LAST, 0, R_GLOBAL);
    e.add(3, 3, R_LAST);
    e.stw(3, offCycles, R_ARM);
    e.lwz(4, -int(sizeof(JitCacheEntry)), R_CACHE);
    e.cmpwi(4, 0);
    e.bne(codeExit);
    uint32_t *checks = e.here();
    e.lwz(4, 0, R_LIMIT);
    e.cmplw(3, 4);
    e.bgt(codeExit);
    e.lwz(4, 0, R_EVENTS);
    e.lwz(4, offEventCycles, 4);
    e.cmplw(3, 4);
    e.bge(codeExit);
    e.stw(3, 0, R_GLOBAL);
    e.blr();

    codePost[0] = e.here();
    e.lwz(R_LAST, 0, R_GLOBAL);
    e.add(3, 3, R_LAST);
    e.stw(3, offCycles, R_ARM);
    e.b(checks);

    // Dispatch: find the next block in the cache from the program counter and jump to it
    // On a miss, the global cycles are put back to the start of the last instruction before exiting
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
    e.stw(R_LAST, 0, R_GLOBAL);
    e.b(codeExit);

    codeStart = (uint8_t*)e.here();
    codePtr = codeStart;
    finishCode(codeArea, codePtr - codeArea);
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
    int kind = nativeKind(op);
    if (!kind) return false;

    // Skip the instruction if its condition fails, like the interpreter's condition lookup
    uint32_t *skip = nullptr;
    if ((op >> 28) != 0xE) {
        e.lwz(3, offCpsr, R_ARM);
        e.rlwinm(3, 3, 4, 28, 31);
        e.loadConst(4, addrCondition + ((op >> 24) & 0xF0));
        e.lbzx(3, 4, 3);
        e.cmpwi(3, 1);
        skip = e.here();
        e.bne(nullptr);
    }

    switch (kind) {
        case 1: emitAlu(e, op, pc); break;
        case 2: emitBranch(e, op, pc); break;
        case 3: emitTransfer(e, op, pc); break;
    }

    // A failed condition takes 1 cycle
    if (skip) {
        uint32_t *done = e.here();
        e.b(nullptr);
        e.patchBc(skip);
        e.li(3, 1);
        e.patchB(done);
    }
    return true;
}

void Jit::emitAlu(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Data processing with an immediate or a register shifted left by an immediate
    // Operands go in r3 and r4, the result in r5, and a shifter carry in r7 if it comes from a register
    uint32_t opc = (op >> 21) & 0xF;
    bool s = op & BIT(20);
    bool logical = (opc <= 1) || (opc >= 8 && opc <= 9) || opc >= 12;
    if (opc != 13 && opc != 15)
        readReg(e, 3, (op >> 16) & 0xF, pc);

    // Get the second operand and the shifter carry for flag-setting logical operations
    enum { CARRY_SAME, CARRY_CLEAR, CARRY_SET, CARRY_REG } carry = CARRY_SAME;
    if (op & BIT(25)) {
        uint32_t shift = (op >> 7) & 0x1E;
        uint32_t value = ((op & 0xFF) << (32 - shift)) | ((op & 0xFF) >> shift);
        if (shift == 0) value = op & 0xFF;
        e.loadConst(4, value);
        if (shift > 0) carry = (value >> 31) ? CARRY_SET : CARRY_CLEAR;
    }
    else {
        readReg(e, 4, op & 0xF, pc);
        uint32_t shift = (op >> 7) & 0x1F;
        if (shift > 0) {
            e.rlwinm(7, 4, shift, 31, 31);
            e.rlwinm(4, 4, shift, 0, 31 - shift);
            carry = CARRY_REG;
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

    if (s) {
        // Build the N and Z flags in r8
        e.rlwinm(8, 5, 0, 0, 0);
        e.cntlzw(9, 5);
        e.rlwinm(9, 9, 25, 1, 1);
        e.or_(8, 8, 9);
        e.lwz(6, offCpsr, R_ARM);

        if (logical) {
            // Logical operations keep V, and set C from the shifter if it shifted
            switch (carry) {
            case CARRY_SAME:
                e.rlwinm(6, 6, 0, 2, 31);
                break;
            case CARRY_SET:
                e.oris(8, 8, 0x2000);
            case CARRY_CLEAR:
                e.rlwinm(6, 6, 0, 3, 31);
                break;
            case CARRY_REG:
                e.rlwinm(7, 7, 29, 2, 2);
                e.or_(8, 8, 7);
                e.rlwinm(6, 6, 0, 3, 31);
                break;
            }
        }
        else {
            // Arithmetic sets C from the carry out, and V from the operand and result signs
            // For subtraction, C is set when there's no borrow and V uses the subtrahend
            e.li(9, 0);
            e.adde(9, 9, 9);
            e.rlwinm(9, 9, 29, 2, 2);
            e.or_(8, 8, 9);
            if (opc == 0x4 || opc == 0xB) {
                e.eqv(10, 4, 3);
                e.xor_(11, 5, 4);
                e.and_(10, 10, 11);
            }
            else {
                int sub = (opc == 0x3) ? 3 : 4, min = (opc == 0x3) ? 4 : 3;
                e.xor_(10, sub, min);
                e.xor_(11, 5, sub);
                e.andc(10, 10, 11);
            }
            e.rlwinm(10, 10, 29, 3, 3);
            e.or_(8, 8, 10);
            e.rlwinm(6, 6, 0, 4, 31);
        }
        e.or_(6, 6, 8);
        e.stw(6, offCpsr, R_ARM);
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
    // Word or byte load or store at an offset without writeback
    // The address goes in r5, and the value in r8; the memory map is checked before calling into memory
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

    // Look up the 4KB block in the memory map
    e.loadConst(12, load ? addrReadMap : addrWriteMap);
    e.rlwinm(6, 5, 22, 10, 29);
    e.lwzx(6, 6, 12);
    e.cmpwi(6, 0);
    uint32_t *slow = e.here();
    e.beq(nullptr);

    if (load) {
        // Read from mapped memory, rotating misaligned words like the interpreter
        if (byte) {
            e.rlwinm(7, 5, 0, 20, 31);
            e.lbzx(8, 6, 7);
        }
        else {
            e.rlwinm(7, 5, 0, 20, 29);
            e.lwbrx(8, 6, 7);
            e.rlwinm(9, 5, 3, 27, 28);
            e.subfic(9, 9, 32);
            e.rlwnm(8, 8, 9, 0, 31);
        }
        uint32_t *done = e.here();
        e.b(nullptr);

        // Otherwise read through the memory handlers
        e.patchBc(slow);
        e.loadConst(3, addrMemory);
        e.mr(4, 5);
        e.call(byte ? addrLoad8 : addrLoad32);
        e.mr(8, 3);
        e.patchB(done);
        writeReg(e, 8, rd);
    }
    else {
        // Write to mapped memory
        readReg(e, 8, rd, pc);
        e.rlwinm(7, 5, 0, 20, byte ? 31 : 29);
        byte ? e.stbx(8, 6, 7) : e.stwbrx(8, 6, 7);
        uint32_t *done = e.here();
        e.b(nullptr);

        // Otherwise write through the memory handlers, which also catches writes to compiled code
        e.patchBc(slow);
        e.loadConst(3, addrMemory);
        e.mr(4, 5);
        readReg(e, 5, rd, pc);
        e.call(byte ? addrStore8 : addrStore32);
        e.patchB(done);
    }
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

bool Jit::codeFull() {
    // Check if a block of the largest size might not fit in the codegen area
    return codeArea && codeEnd - codePtr < 64 * MAX_INSTR_WORDS * 4 + 16;
}

void Jit::emitChecked(PpcEmitter &e, JitBlock *block, size_t i, uint32_t pc, bool &ok) {
    // Set the program counter as if the instruction was fetched
    uint32_t op = block->instrs[i].opcode;
    e.loadConst(0, pc);
    e.stw(0, offPc, R_ARM);

    void *handler = block->thumb ? memberAddress(block->instrs[i].thumb) : memberAddress(block->instrs[i].arm);
    void *reserved = memberAddress(&Interpreter::handleReserved);
    if (!handler || !reserved) {
        ok = false;
        return;
    }

    if (!block->thumb && emitNative(e, op, pc)) {
        // The instruction was generated as native code, leaving its cycles in r3
    }
    else if (block->thumb || (op >> 28) == 0xE) {
        // Call the handler directly when there's no condition to check
        e.mr(3, R_ARM);
        e.loadConst(4, op);
        e.call(handler);
    }
    else {
        // Look up the condition result, like the interpreter does
        e.lwz(3, offCpsr, R_ARM);
        e.rlwinm(3, 3, 4, 28, 31);
        e.loadConst(4, addrCondition + ((op >> 24) & 0xF0));
        e.lbzx(3, 4, 3);
        e.cmpwi(3, 1);
        uint32_t *notTrue = e.here();
        e.bne(nullptr);

        // Condition true: call the handler
        e.mr(3, R_ARM);
        e.loadConst(4, op);
        e.call(handler);
        uint32_t *done1 = e.here();
        e.b(nullptr);

        // Condition false: take 1 cycle; reserved: call the reserved handler
        e.patchBc(notTrue);
        e.cmpwi(3, 0);
        e.li(3, 1);
        uint32_t *done2 = e.here();
        e.beq(nullptr);
        e.mr(3, R_ARM);
        e.loadConst(4, op);
        e.call(reserved);
        e.patchB(done1);
        e.patchBc(done2);
    }

    // Count cycles and check whether to stop, then check for jumps
    e.call(codePost[!isPure(op, block->thumb)]);
    if (i + 1 < block->instrs.size() && mayJump(op, block->thumb)) {
        e.lwz(4, offPc, R_ARM);
        e.loadConst(5, pc);
        e.cmpw(4, 5);
        uint32_t *same = e.here();
        e.beq(nullptr);
        e.b(codeDispatch);
        e.patchBc(same);
    }
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

    PpcEmitter e(codePtr);
    uint32_t step = block->thumb ? 2 : 4;
    bool ok = true;
    for (size_t i = 0; i < block->instrs.size() && ok; i++)
        emitChecked(e, block, i, block->address + step * (i + 2), ok);

    // Continue to whatever block comes next
    e.b(codeDispatch);

    uint8_t *code = codePtr;
    size_t size = (uint8_t*)e.here() - code;
    finishCode(code, size);
    if (!ok) return nullptr;
    codePtr += (size + 31) & ~31;
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

#endif

#ifdef JIT_PPC_TEST
// Generate one native instruction followed by a return, for testing generated code in an emulator
bool jitTestEmit(Jit &jit, uint8_t *buf, uint32_t op, uint32_t pc, uint32_t *size) {
    PpcEmitter e(buf);
    if (!jit.emitNative(e, op, pc)) return false;
    e.emit(0x48000002 | 0x50000); // Absolute branch to the test's end address, since calls change LR
    *size = (uint8_t*)e.here() - buf;
    for (uint32_t *p = (uint32_t*)buf; p < e.here(); p++) *p = __builtin_bswap32(*p);
    return true;
}
#endif
