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

// PowerPC code generation for the JIT, used on Wii U
// Generated blocks do exactly what the portable executor in jit.cpp does, one instruction at a time

#include <cstddef>
#include <cstring>
#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/core.h>

#include "../core.h"


// Registers that hold state while generated code runs; they're saved by the entry code
enum PpcReg {
    R_BATCH = 20, // Cycles of a batch of instructions that share one check, zero outside of one
    R_TMP2 = 21, // Values kept across memory accesses
    R_TMP1 = 22,
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
    void andi_(int ra, int rs, uint32_t ui) { d(28, rs, ra, ui); }

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
    void srawi(int ra, int rs, int sh) { x(rs, ra, sh, 824); }
    void extsb(int ra, int rs) { x(rs, ra, 0, 954); }
    void extsh(int ra, int rs) { x(rs, ra, 0, 922); }
    void lhbrx(int rt, int ra, int rb) { x(rt, ra, rb, 790); }
    void sthbrx(int rs, int ra, int rb) { x(rs, ra, rb, 918); }
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
static const int MAX_INSTR_WORDS = 120;

// Get the code address of a non-virtual member function
template <typename T> static void *memberAddress(T func) {
    struct { uintptr_t ptr; ptrdiff_t adj; } parts;
    static_assert(sizeof(parts) == sizeof(func), "Unexpected member function pointer size");
    memcpy(&parts, &func, sizeof(parts));
    return (parts.ptr & 1 || parts.adj) ? nullptr : (void*)parts.ptr;
}

static_assert(sizeof(JitCacheEntry) == 16, "Generated code expects 16-byte cache entries");

// Memory accesses for native loads and stores that miss the memory maps, done like the interpreter
template <bool arm7> static uint32_t jitLoad32(Memory *memory, uint32_t address) {
    uint32_t value = memory->read<uint32_t>(arm7, address);
    uint8_t shift = (address & 0x3) << 3;
    return shift ? ((value << (32 - shift)) | (value >> shift)) : value;
}

template <bool arm7> static uint32_t jitLoad8(Memory *memory, uint32_t address) {
    return memory->read<uint8_t>(arm7, address);
}

template <bool arm7> static uint32_t jitLoad16(Memory *memory, uint32_t address) {
    return memory->read<uint16_t>(arm7, address);
}

template <bool arm7> static void jitStore16(Memory *memory, uint32_t address, uint32_t value) {
    memory->write<uint16_t>(arm7, address, value);
}

template <bool arm7> static void jitStore32(Memory *memory, uint32_t address, uint32_t value) {
    memory->write<uint32_t>(arm7, address, value);
}

template <bool arm7> static void jitStore8(Memory *memory, uint32_t address, uint32_t value) {
    memory->write<uint8_t>(arm7, address, value);
}

// The codegen area is shared by one ARM9 and one ARM7 JIT at a time, with the ARM9 getting most of it
static Jit *codeOwners[2] = {};
static uint8_t *sharedArea = nullptr;
static uint32_t sharedSize = 0;

void Jit::initCodegen() {
    if (codeArea) return;
    // Get the codegen area, which can only be used from one CPU core
    // Try again later if this thread isn't on that core yet, and give up for anything else
    if (OSGetCoreId() != (OSGetCodegenCore() & 3)) return;
    if (!sharedArea) {
        void *area = nullptr;
        OSGetCodegenVirtAddrRange(&area, &sharedSize);
        sharedArea = (uint8_t*)area;
    }
    JLOG("init: core %u, codegen core %X, area %p size %X, owner %p, mode %X, sec %X", OSGetCoreId(),
        OSGetCodegenCore(), sharedArea, sharedSize, codeOwners[arm7], OSGetCodegenMode(), OSGetSecCodeGenMode());
    codeFailed = true;
    if (!sharedArea || sharedSize < 0x100000 || codeOwners[arm7]) return;

    // Make sure the event vector can be read directly, as generated code does
    if ((void*)&core->events.head != (void*)&core->events) return;
    BOOL rw = OSSwitchSecCodeGenMode(CODEGEN_RW_);
    JLOG("init: switch to RW returned %X, sec %X", rw, OSGetSecCodeGenMode());
    if (!rw) return;
    codeFailed = false;
    uint32_t split = (sharedSize / 4 * 3) & ~0xFFF;
    uint8_t *area = arm7 ? (sharedArea + split) : sharedArea;
    uint32_t size = arm7 ? (sharedSize - split) : split;
    codeOwners[arm7] = this;
    codeCore = OSGetCoreId();
    codeArea = area;
    codeEnd = codeArea + size;

    // Get offsets of the interpreter state that generated code uses
    Interpreter &arm = core->interpreter[arm7];
    offPc = (uint8_t*)&arm.registersUsr[15] - (uint8_t*)&arm;
    offCpsr = (uint8_t*)&arm.cpsr - (uint8_t*)&arm;
    offCycles = (uint8_t*)&arm.cycles - (uint8_t*)&arm;
    offEventCycles = offsetof(SchedEvent, cycles);
    offUsr = (uint8_t*)&arm.registersUsr[0] - (uint8_t*)&arm;
    offRegs = (uint8_t*)&arm.registers[0] - (uint8_t*)&arm;
    addrCondition = (uint32_t)(uintptr_t)&Interpreter::condition[0];
    addrMemory = (uint32_t)(uintptr_t)&core->memory;
    addrReadMap = (uint32_t)(uintptr_t)(arm7 ? &core->memory.readMap7[0] : &core->memory.readMap9A[0]);
    addrWriteMap = (uint32_t)(uintptr_t)(arm7 ? &core->memory.writeMap7[0] : &core->memory.writeMap9A[0]);
    addrLoad32 = (uint32_t)(uintptr_t)(arm7 ? &jitLoad32<true> : &jitLoad32<false>);
    addrLoad8 = (uint32_t)(uintptr_t)(arm7 ? &jitLoad8<true> : &jitLoad8<false>);
    addrStore32 = (uint32_t)(uintptr_t)(arm7 ? &jitStore32<true> : &jitStore32<false>);
    addrStore8 = (uint32_t)(uintptr_t)(arm7 ? &jitStore8<true> : &jitStore8<false>);
    addrLoad16 = (uint32_t)(uintptr_t)(arm7 ? &jitLoad16<true> : &jitLoad16<false>);
    addrStore16 = (uint32_t)(uintptr_t)(arm7 ? &jitStore16<true> : &jitStore16<false>);
    PpcEmitter e(codeArea);
    emitStubs(e);
    codeStart = (uint8_t*)e.here();
    codePtr = codeStart;
    finishCode(codeArea, codePtr - codeArea, std::vector<uint32_t*>());
}

void Jit::emitStubs(PpcEmitter &e) {
    // Exit: restore saved registers and return to the caller
    codeExit = e.here();
    e.lwz(0, 84, 1);
    e.mtlr(0);
    e.lmw(R_BATCH, 24, 1);
    e.addi(1, 1, 80);
    e.blr();

    // Entry: save registers, load the state pointers, and jump to the block, which sets its program counter
    // Arguments are the interpreter, event vector, limit, block code, global cycles, and cache
    codeEnter = e.here();
    e.stwu(1, -80, 1);
    e.mflr(0);
    e.stw(0, 84, 1);
    e.stmw(R_BATCH, 24, 1);
    e.li(R_BATCH, 0);
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

    // After an instruction, with its cycles in r3: update the CPU's cycles, doubled for the half-speed ARM7, then
    // continue if nothing else needs to run first; the global cycles are only advanced here, since the helper
    // expects them at the start of the last instruction; impure versions also check if code changed, since the
    // block might be stale
    for (int store = 0; store < 2; store++) {
        for (int impure = 0; impure < 2; impure++) {
            codePost[impure][store] = e.here();
            if (arm7) e.add(3, 3, 3);
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
    // Types are word, byte and halfword loads, then the same for stores
    for (int type = 0; type < 6; type++) {
        bool load = type < 3;
        int size = (type % 3 == 0) ? 4 : (type % 3 == 1) ? 1 : 2;
        codeMemory[type] = e.here();
        e.rlwinm(6, 5, 22, 10, 29);
        e.lwzx(6, 6, load ? R_RMAP : R_WMAP);
        e.cmpwi(6, 0);
        uint32_t *slow = e.here();
        e.beq(nullptr);
        e.rlwinm(7, 5, 0, 20, (size == 4) ? 29 : (size == 2) ? 30 : 31);
        if (load && size == 1) {
            e.lbzx(8, 6, 7);
        }
        else if (load && size == 2) {
            e.lhbrx(8, 6, 7);
        }
        else if (load) {
            // Rotate misaligned words like the interpreter
            e.lwbrx(8, 6, 7);
            e.rlwinm(9, 5, 3, 27, 28);
            e.subfic(9, 9, 32);
            e.rlwnm(8, 8, 9, 0, 31);
        }
        else {
            (size == 1) ? e.stbx(8, 6, 7) : (size == 2) ? e.sthbrx(8, 6, 7) : e.stwbrx(8, 6, 7);
        }
        e.blr();

        e.patchBc(slow);
        e.stwu(1, -16, 1);
        e.mflr(0);
        e.stw(0, 20, 1);
        e.loadConst(3, addrMemory);
        e.mr(4, 5);
        if (!load) e.mr(5, 8);
        if (load) e.call((size == 4) ? addrLoad32 : (size == 1) ? addrLoad8 : addrLoad16);
        else e.call((size == 4) ? addrStore32 : (size == 1) ? addrStore8 : addrStore16);
        if (load) e.mr(8, 3);
        e.lwz(0, 20, 1);
        e.mtlr(0);
        e.addi(1, 1, 16);
        e.blr();
    }
}

void Jit::freeCodegen() {
    // Let another JIT use this part of the codegen area
    if (codeOwners[arm7] != this) return;
    codeOwners[arm7] = nullptr;
    codeArea = codeStart = codePtr = codeEnd = nullptr;
}

void Jit::patchJump(uint32_t *site, void *target) {
    // Point a jump in generated code somewhere else
    PpcEmitter e(site);
    e.b(target);
}

void Jit::applyUnlinks() {
    // Point queued jumps back at the dispatcher, now that no generated code is running
    if (!codeArea) {
        unlinks.clear();
        return;
    }
    if (OSGetCoreId() != codeCore || !OSSwitchSecCodeGenMode(CODEGEN_RW_)) return;
    for (size_t i = 0; i < unlinks.size(); i++)
        patchJump(unlinks[i], codeDispatch);
    finishCode(nullptr, 0, unlinks);
    unlinks.clear();
}

void Jit::finishCode(uint8_t *start, size_t size, const std::vector<uint32_t*> &patched) {
    // Write the code and any patched jumps to memory and make the area executable again
    if (start) DCFlushRange(start, size);
    for (size_t i = 0; i < patched.size(); i++)
        DCFlushRange(patched[i], 4);
    BOOL rx = OSSwitchSecCodeGenMode(CODEGEN_R_X);
    if (start) ICInvalidateRange(start, size);
    for (size_t i = 0; i < patched.size(); i++)
        ICInvalidateRange(patched[i], 4);
#ifdef JIT_LOG
    static uint32_t count = 0;
    if (++count <= 200 || (count % 10000) == 0)
        JLOG("code %u: %p size %X, switch to RX returned %X, sec %X", count, start, (uint32_t)size, rx, OSGetSecCodeGenMode());
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

enum NativeKind {
    NK_NONE = 0,
    NK_ALU, // Data processing with an immediate or register shifted by an immediate
    NK_BRANCH, // B/BL
    NK_TRANSFER, // LDR/STR/LDRB/STRB
    NK_BRANCH_REG, // BX/BLX
    NK_HALF, // LDRH/STRH/LDRSB/LDRSH at an offset
    NK_BLOCK, // LDM/STM
    NK_BLOCK_PC, // LDM that loads the program counter, like a function return
    NK_SWAP // SWP/SWPB
};

static int nativeKind(uint32_t op, bool arm7) {
    // Check which kind of native code an ARM instruction can be generated as, or NK_NONE
    // Anything that writes the program counter, other than branches, is left to the interpreter
    // The ARM7 leaves BLX to the interpreter, which ignores it, and stores with writeback of a listed base, which
    // store the new base unless it's first
    if ((op >> 28) == 0xF) return NK_NONE;
    uint32_t rd = (op >> 12) & 0xF, rn = (op >> 16) & 0xF, rm = op & 0xF;
    if ((op & 0x0C000000) == 0x00000000) {
        if (!(op & BIT(25)) && (op & BIT(4))) {
            if ((op & 0x0FB00FF0) == 0x01000090) // SWP/SWPB
                return (rd == 15 || rn == 15 || rm == 15) ? NK_NONE : NK_SWAP;
            if ((op & 0x0FFFFFD0) == 0x012FFF10) // BX/BLX
                return (rm == 15 || (arm7 && (op & BIT(5)))) ? NK_NONE : NK_BRANCH_REG;
            if ((op & 0x0E000090) == 0x00000090 && (op & 0x60)) { // Halfword and signed transfers
                if ((op & 0x01200000) != 0x01000000 || rd == 15) return NK_NONE; // Only offsets without writeback
                if (!(op & BIT(20)) && (op & 0x60) != 0x20) return NK_NONE; // LDRD/STRD
                return NK_HALF;
            }
            return NK_NONE; // Shifts by register, multiplies and misc
        }
        uint32_t opc = (op >> 21) & 0xF;
        bool s = op & BIT(20);
        if (opc >= 8 && opc <= 11) return s ? NK_ALU : NK_NONE; // Compares and tests, other encodings are misc
        if (opc >= 5 && opc <= 7 && s) return NK_NONE; // Flag-setting ADC/SBC/RSC
        return (rd == 15) ? NK_NONE : NK_ALU;
    }
    if ((op & 0x0E000000) == 0x0A000000) // B/BL
        return NK_BRANCH;
    if ((op & 0x0C000000) == 0x04000000) { // LDR/STR/LDRB/STRB
        if ((op & BIT(25)) && (op & BIT(4))) return NK_NONE; // Undefined
        bool writeback = !(op & BIT(24)) || (op & BIT(21));
        if (!(op & BIT(24)) && (op & BIT(21))) return NK_NONE; // LDRT/STRT
        if (rd == 15 || (writeback && rn == 15)) return NK_NONE;
        return NK_TRANSFER;
    }
    if ((op & 0x0E000000) == 0x08000000) { // LDM/STM
        uint32_t list = op & 0xFFFF;
        if (!list || (op & BIT(22)) || rn == 15) return NK_NONE;
        if ((op & BIT(20)) && (op & BIT(21)) && (list & BIT(rn))) return NK_NONE; // Loading the base with writeback
        if (arm7 && !(op & BIT(20)) && (op & BIT(21)) && (list & BIT(rn))) return NK_NONE; // Storing it on ARM7
        if (list & BIT(15)) return (op & BIT(20)) ? NK_BLOCK_PC : NK_NONE; // Only loads of the program counter
        return NK_BLOCK;
    }
    return NK_NONE;
}

static bool nativeJumps(int kind) {
    // Check if a native instruction kind can change the program counter
    return kind == NK_BRANCH || kind == NK_BRANCH_REG || kind == NK_BLOCK_PC;
}

bool Jit::emitNative(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Generate an ARM instruction as native code if possible, leaving its cycles in r3
    // Branches leave the program counter set in memory; other instructions leave it unchanged
    int kind = nativeKind(op, arm7);
    if (!kind) return false;

    // Skip the instruction if its condition fails, which takes 1 cycle
    uint32_t *skip = nullptr;
    if ((op >> 28) != 0xE) {
        e.call(codeCond[op >> 28]);
        skip = e.here();
        e.bne(nullptr);
    }

    switch (kind) {
        case NK_ALU: emitAlu(e, op, pc); break;
        case NK_BRANCH: emitBranch(e, op, pc); break;
        case NK_TRANSFER: emitTransfer(e, op, pc); break;
        case NK_BRANCH_REG: emitBranchReg(e, op, pc); break;
        case NK_HALF: emitHalf(e, op, pc); break;
        case NK_BLOCK: case NK_BLOCK_PC: emitBlock(e, op, pc); break;
        case NK_SWAP: emitSwap(e, op, pc); break;
    }

    if (skip) {
        uint32_t *done = e.here();
        e.b(nullptr);
        e.patchBc(skip);
        if (nativeJumps(kind)) e.stw(R_PC, offPc, R_ARM);
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
        carry = emitShift(e, op, pc, s && logical);
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

bool Jit::emitShift(PpcEmitter &e, uint32_t op, uint32_t pc, bool setCarry) {
    // Load a register shifted by an immediate into r4, like the interpreter's operand helpers
    // If requested, also put the shifter carry in r7, returning whether the carry changes
    readReg(e, 4, op & 0xF, pc);
    uint32_t type = (op >> 5) & 0x3, shift = (op >> 7) & 0x1F;

    // Get the carry from the last bit shifted out, before shifting
    int carryBit = -1;
    switch (type) {
        case 0: if (shift) carryBit = 32 - shift; break; // LSL
        case 1: case 2: carryBit = shift ? (shift - 1) : 31; break; // LSR, ASR
        case 3: carryBit = shift ? (shift - 1) : 0; break; // ROR, RRX
    }
    if (setCarry && carryBit >= 0)
        e.rlwinm(7, 4, (32 - carryBit) & 0x1F, 31, 31);

    switch (type) {
    case 0: // LSL
        if (shift) e.rlwinm(4, 4, shift, 0, 31 - shift);
        break;
    case 1: // LSR, where 0 means 32
        if (shift) e.rlwinm(4, 4, 32 - shift, shift, 31);
        else e.li(4, 0);
        break;
    case 2: // ASR, where 0 means 32
        e.srawi(4, 4, shift ? shift : 31);
        break;
    case 3: // ROR, where 0 means rotate right through carry
        if (shift) {
            e.rlwinm(4, 4, 32 - shift, 0, 31);
        }
        else {
            e.lwz(6, offCpsr, R_ARM);
            e.rlwinm(6, 6, 2, 0, 0);
            e.rlwinm(4, 4, 31, 1, 31);
            e.or_(4, 4, 6);
        }
        break;
    }
    return setCarry && carryBit >= 0;
}

void Jit::emitTransfer(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Word or byte load or store with any addressing mode, with the address in r5 and value in r8
    // Writeback happens before the memory access, which is the same as the interpreter since loads replace it
    // and stores read their value first; the memory call can change volatile registers, so it goes last
    uint32_t rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
    bool pre = op & BIT(24), up = op & BIT(23), byte = op & BIT(22), load = op & BIT(20);
    bool writeback = !pre || (op & BIT(21));

    // Get the offset, folding constant addresses from the program counter
    if (!(op & BIT(25))) {
        uint32_t offset = op & 0xFFF;
        if (rn == 15) {
            e.loadConst(5, up ? (pc + offset) : (pc - offset));
        }
        else {
            readReg(e, 3, rn, pc);
            e.addi(9, 3, up ? offset : -offset);
            e.mr(5, pre ? 9 : 3);
        }
    }
    else {
        emitShift(e, op, pc, false);
        readReg(e, 3, rn, pc);
        up ? e.add(9, 3, 4) : e.subf(9, 4, 3);
        e.mr(5, pre ? 9 : 3);
    }

    if (!load) readReg(e, 8, rd, pc);
    if (writeback) writeReg(e, 9, rn);
    e.call(codeMemory[(load ? 0 : 3) + byte]);
    if (load) writeReg(e, 8, rd);
    e.li(3, arm7 ? (load ? 3 : 2) : 1);
}

void Jit::emitHalf(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Halfword or signed load, or halfword store, at an offset without writeback
    uint32_t rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
    bool up = op & BIT(23), load = op & BIT(20);
    uint32_t type = (op >> 5) & 0x3; // 1 = halfword, 2 = signed byte, 3 = signed halfword
    if (op & BIT(22)) {
        uint32_t offset = ((op >> 4) & 0xF0) | (op & 0xF);
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
        up ? e.add(5, 3, 4) : e.subf(5, 4, 3);
    }

    if (load) {
        // The ARM7 rotates halfwords loaded from odd addresses, and shifts signed ones down, keeping the address
        // in a register that memory calls don't change
        if (arm7 && type != 2) e.mr(R_TMP1, 5);
        e.call(codeMemory[(type == 2) ? 1 : 2]);
        if (type == 2) e.extsb(8, 8);
        if (type == 3) e.extsh(8, 8);
        if (arm7 && type != 2) {
            e.andi_(7, R_TMP1, 1);
            uint32_t *even = e.here();
            e.beq(nullptr);
            (type == 1) ? e.rlwinm(8, 8, 24, 0, 31) : e.srawi(8, 8, 8);
            e.patchBc(even);
        }
        writeReg(e, 8, rd);
    }
    else {
        readReg(e, 8, rd, pc);
        e.call(codeMemory[5]);
    }
    e.li(3, arm7 ? (load ? 3 : 2) : 1);
}

void Jit::emitBlock(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Block load or store of registers other than the program counter, with words at aligned addresses
    // The base is kept in R_TMP1, since memory calls can change volatile registers
    uint32_t rn = (op >> 16) & 0xF, list = op & 0xFFFF;
    bool pre = op & BIT(24), up = op & BIT(23), writeback = op & BIT(21), load = op & BIT(20);
    int count = 0;
    for (int i = 0; i < 16; i++)
        count += (list >> i) & 1;
    int32_t first = up ? (pre ? 4 : 0) : (pre ? -count * 4 : -count * 4 + 4);
    int32_t end = up ? count * 4 : -count * 4;
    readReg(e, R_TMP1, rn, pc);

    // Loads write back first, since the base isn't loaded; stores write back last, storing the old base
    if (load && writeback) {
        e.addi(9, R_TMP1, end);
        writeReg(e, 9, rn);
    }
    int32_t offset = first;
    for (int i = 0; i < 16; i++) {
        if (!(list & BIT(i))) continue;
        e.addi(5, R_TMP1, offset);
        offset += 4;
        e.rlwinm(5, 5, 0, 0, 29);
        if (load) {
            e.call(codeMemory[0]);
            if (i == 15) emitJump(e, 8, !arm7);
            else writeReg(e, 8, i);
        }
        else {
            readReg(e, 8, i, pc);
            e.call(codeMemory[3]);
        }
    }
    if (!load && writeback) {
        e.addi(9, R_TMP1, end);
        writeReg(e, 9, rn);
    }
    if (list & BIT(15))
        e.li(3, count + 4);
    else
        e.li(3, count + (arm7 ? (load ? 2 : 1) : (count < 2)));
}

void Jit::emitSwap(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Swap a word or byte, reading the source and address first since memory calls can change volatile registers
    bool byte = op & BIT(22);
    readReg(e, R_TMP2, op & 0xF, pc);
    readReg(e, R_TMP1, (op >> 16) & 0xF, pc);
    e.mr(5, R_TMP1);
    e.call(codeMemory[byte ? 1 : 0]);
    writeReg(e, 8, (op >> 12) & 0xF);
    e.mr(5, R_TMP1);
    e.mr(8, R_TMP2);
    e.call(codeMemory[byte ? 4 : 3]);
    e.li(3, arm7 ? 4 : 2);
}

void Jit::emitBranchReg(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Branch to a register, switching to THUMB if bit 0 is set, with optional link
    readReg(e, 5, op & 0xF, pc);
    if (op & BIT(5)) {
        e.loadConst(6, pc - 4);
        writeReg(e, 6, 14);
    }
    emitJump(e, 5, true);
    e.li(3, 3);
}

void Jit::emitJump(PpcEmitter &e, int rs, bool interwork) {
    // Jump to an address in a register, switching to THUMB if bit 0 is set, like BX and loads of the program counter
    // Without interworking, like loads of the program counter on the ARM7, it stays in ARM mode
    if (rs != 5) e.mr(5, rs);
    if (!interwork) {
        e.rlwinm(8, 5, 0, 0, 29);
        e.addi(8, 8, 4);
        e.stw(8, offPc, R_ARM);
        return;
    }
    e.lwz(6, offCpsr, R_ARM);
    e.rlwinm(7, 5, 5, 26, 26);
    e.or_(6, 6, 7);
    e.stw(6, offCpsr, R_ARM);

    // Set the program counter as if the pipeline was flushed in the new mode
    e.andi_(7, 5, 1);
    uint32_t *arm = e.here();
    e.beq(nullptr);
    e.rlwinm(8, 5, 0, 0, 30);
    e.addi(8, 8, 2);
    uint32_t *done = e.here();
    e.b(nullptr);
    e.patchBc(arm);
    e.rlwinm(8, 5, 0, 0, 29);
    e.addi(8, 8, 4);
    e.patchB(done);
    e.stw(8, offPc, R_ARM);
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

bool Jit::batched(JitBlock *block, size_t i) {
    // Check if an instruction is in a run whose cycles are checked together, which is only done with fast timing
    // Runs are native ALU instructions and loads; stores always check, since they can halt the CPU, change code
    // or schedule something that has to happen straight away, and jumps end runs since they leave the block
    if (!codeFast || testing || block->thumb || i >= block->instrs.size()) return false;
    uint32_t op = block->instrs[i].opcode;
    int kind = nativeKind(op, arm7);
    if (kind == NK_ALU) return true;
    return (kind == NK_TRANSFER || kind == NK_HALF || kind == NK_BLOCK) && (op & BIT(20));
}

void Jit::emitInstr(PpcEmitter &e, JitBlock *block, size_t i, uint32_t pc, bool &ok) {
    // Generate one instruction; the program counter register already holds its value
    uint32_t op = block->instrs[i].opcode;
    bool native = !block->thumb && emitNative(e, op, pc);
    bool memoryPc = !native || nativeJumps(nativeKind(op, arm7));

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

    // With fast timing, runs of native ALU instructions and loads add up their cycles and only check whether to stop
    // after the last one, so events can happen a few instructions late and loads can see the time a little behind;
    // jumps into the middle of a run still count the cycles of what they run
    if (batched(block, i)) {
        if (batched(block, i + 1)) {
            e.add(R_BATCH, R_BATCH, 3);
            return;
        }
        e.add(3, 3, R_BATCH);
        e.li(R_BATCH, 0);
    }

    // Count cycles and check whether to stop; the program counter is stored first if it's only in the register
    // Then check for jumps, which leave the program counter in memory different from the register
    // Branches to known addresses jump through a site that's linked to the target block once it exists
    e.call(codePost[!isPure(op, block->thumb)][!memoryPc]);
    if (native && nativeKind(op, arm7) == NK_BRANCH) {
        uint32_t *same = nullptr;
        if ((op >> 28) != 0xE) {
            e.lwz(4, offPc, R_ARM);
            e.cmplw(4, R_PC);
            same = e.here();
            e.beq(nullptr);
        }
        uint32_t target = (pc + ((int32_t)(op << 8) >> 6)) & ~0x3;
        block->links.push_back({ e.here(), target, nullptr });
        e.b(codeDispatch);
        if (same) e.patchBc(same);
    }
    else if (i + 1 < block->instrs.size() && mayJump(op, block->thumb)) {
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
    // The codegen area can only be touched from its core; blocks use the portable executor otherwise
    if (OSGetCoreId() != codeCore || !OSSwitchSecCodeGenMode(CODEGEN_RW_)) return nullptr;

    // Set the program counter register at the start, then step it before each instruction
    // Record where each instruction starts after stepping, so other blocks can jump into the middle
    PpcEmitter e(codePtr);
    uint32_t step = block->thumb ? 2 : 4;
    uint32_t pc = block->address + step * 2;
    e.loadConst(R_PC, pc);
    block->entries.clear();
    block->links.clear();
    bool ok = true;
    for (size_t i = 0; i < block->instrs.size() && ok; i++, pc += step) {
        if (i > 0) e.addi(R_PC, R_PC, step);
        block->entries.push_back((uint8_t*)e.here() - codePtr);
        emitInstr(e, block, i, pc, ok);
    }

    // Continue to whatever block comes next
    e.b(codeDispatch);

    // Link jumps between this block and others while the area is writable
    uint8_t *code = codePtr;
    size_t size = (uint8_t*)e.here() - code;
    std::vector<uint32_t*> patched;
    if (ok) {
        block->code = code;
        linkBlock(block, patched);
    }
    else {
        block->links.clear();
    }
    finishCode(code, size, patched);
    if (!ok) return nullptr;
    codePtr += (size + 15) & ~15;
    return code;
}

void *Jit::generateEntry(JitBlock *host, size_t index) {
    // Generate a small entry point into the middle of another block's code, instead of compiling it again
    if (!codeArea || !host->code || index >= host->entries.size() || codePtr + 64 > codeEnd) return nullptr;
    if (OSGetCoreId() != codeCore || !OSSwitchSecCodeGenMode(CODEGEN_RW_)) return nullptr;
    PpcEmitter e(codePtr);
    e.loadConst(R_PC, host->address + (index + 2) * (host->thumb ? 2 : 4));
    e.b((uint8_t*)host->code + host->entries[index]);
    uint8_t *code = codePtr;
    size_t size = (uint8_t*)e.here() - code;
    codePtr += (size + 15) & ~15;
    return code;
}

void Jit::linkEntry(JitBlock *block) {
    // Link waiting jumps to a block that's an entry into another block's code
    if (!block->code || pendingLinks.find(block->address | block->thumb) == pendingLinks.end()) {
        finishCode((uint8_t*)block->code, 16, std::vector<uint32_t*>());
        return;
    }
    std::vector<uint32_t*> patched;
    linkBlock(block, patched);
    finishCode((uint8_t*)block->code, 16, patched);
}


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
