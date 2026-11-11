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
    R_NEXT = 19, // Global cycles when something else needs to run, from the limit and the next scheduled event
    R_BATCH = 20, // Cycles of a batch of instructions that share one check, zero outside of one
    R_TMP2 = 21, // Values kept across memory accesses
    R_TMP1 = 22,
    R_WMAP = 23, // ARM9 write memory map
    R_RMAP = 24, // ARM9 read memory map
    R_PC = 25, // Program counter of the current instruction, which is only stored when needed
    R_CACHE = 26, // Block cache, with the code changed flag before it
    R_EVENTS = 27, // Scheduler event vector
    R_CYCLES = 28, // Global cycles, which are only written to memory before calling into the emulator
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
    void xori(int ra, int rs, uint32_t ui) { d(26, rs, ra, ui); }
    void xoris(int ra, int rs, uint32_t ui) { d(27, rs, ra, ui); }
    void lwz(int rt, int32_t disp, int ra) { d(32, rt, ra, disp); }
    void lbz(int rt, int32_t disp, int ra) { d(34, rt, ra, disp); }
    void lhz(int rt, int32_t disp, int ra) { d(40, rt, ra, disp); }
    void stw(int rs, int32_t disp, int ra) { d(36, rs, ra, disp); }
    void stb(int rs, int32_t disp, int ra) { d(38, rs, ra, disp); }
    void sth(int rs, int32_t disp, int ra) { d(44, rs, ra, disp); }
    void stwu(int rs, int32_t disp, int ra) { d(37, rs, ra, disp); }
    void lmw(int rt, int32_t disp, int ra) { d(46, rt, ra, disp); }
    void stmw(int rs, int32_t disp, int ra) { d(47, rs, ra, disp); }
    void cmpwi(int ra, int32_t si) { d(11, 0, ra, si); }
    void oris(int ra, int rs, uint32_t ui) { d(25, rs, ra, ui); }
    void subfic(int rt, int ra, int32_t si) { d(8, rt, ra, si); }
    void addic(int rt, int ra, int32_t si) { d(12, rt, ra, si); }
    void andi_(int ra, int rs, uint32_t ui) { d(28, rs, ra, ui); }

    // X-form instructions
    void x(int rt, int ra, int rb, int xo) { emit((31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1)); }
    void slw(int ra, int rs, int rb) { x(rs, ra, rb, 24); }
    void srw(int ra, int rs, int rb) { x(rs, ra, rb, 536); }
    void sraw(int ra, int rs, int rb) { x(rs, ra, rb, 792); }
    void subfe(int rt, int ra, int rb) { x(rt, ra, rb, 136); } // rt = rb - ra - 1 + carry
    void mullw(int rt, int ra, int rb) { x(rt, ra, rb, 235); }
    void mulhw(int rt, int ra, int rb) { x(rt, ra, rb, 75); }
    void mulhwu(int rt, int ra, int rb) { x(rt, ra, rb, 11); }
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
    void stwx(int rs, int ra, int rb) { x(rs, ra, rb, 151); }
    void lwbrx(int rt, int ra, int rb) { x(rt, ra, rb, 534); }
    void stwbrx(int rs, int ra, int rb) { x(rs, ra, rb, 662); }
    void rlwnm(int ra, int rs, int rb, int mb, int me) {
        emit((23 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (mb << 6) | (me << 1));
    }
    void rlwimi(int ra, int rs, int sh, int mb, int me) {
        emit((20 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1));
    }
    void mr(int ra, int rs) { or_(ra, rs, rs); }
    void lbzx(int rt, int ra, int rb) { x(rt, ra, rb, 87); }
    void cmpw(int ra, int rb) { x(0, ra, rb, 0); }
    void cmplw(int ra, int rb) { x(0, ra, rb, 32); }
    void rlwinm(int ra, int rs, int sh, int mb, int me) {
        emit((21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1));
    }
    void rlwinm_(int ra, int rs, int sh, int mb, int me) { // Also compares the result with 0 in cr0
        emit((21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | 1);
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
    offSpsr = (uint8_t*)&arm.spsr - (uint8_t*)&arm;
    offCycles = (uint8_t*)&arm.cycles - (uint8_t*)&arm;
    offEventCycles = offsetof(SchedEvent, cycles);
    offUsr = (uint8_t*)&arm.registersUsr[0] - (uint8_t*)&arm;
    offRegs = (uint8_t*)&arm.registers[0] - (uint8_t*)&arm;
    offIe = (uint8_t*)&arm.ie - (uint8_t*)&arm;
    offIrf = (uint8_t*)&arm.irf - (uint8_t*)&arm;
    offDsiCycle = (uint8_t*)&arm.dsiCycle - (uint8_t*)&arm;
    offIme = (uint8_t*)&arm.ime - (uint8_t*)&arm;
    addrWriteCpsr = (uint32_t)(uintptr_t)&Jit::writeCpsrCall;
    addrRestoreCpsr = (uint32_t)(uintptr_t)&Jit::restoreCpsrCall;
    addrCondition = (uint32_t)(uintptr_t)&Interpreter::condition[0];
    addrMemory = (uint32_t)(uintptr_t)&core->memory;
    addrReadMap = (uint32_t)(uintptr_t)(arm7 ? &core->memory.readMap7[0] : &core->memory.readMap9A[0]);
    addrWriteMap = (uint32_t)(uintptr_t)(arm7 ? &core->memory.writeMap7[0] : &core->memory.writeMap9A[0]);
    addrDtcmReg = (uint32_t)(uintptr_t)&core->cp15.dtcmReg;
    addrDivCnt = (uint32_t)(uintptr_t)&core->divSqrt.divCnt;
    addrDivNumer = (uint32_t)(uintptr_t)&core->divSqrt.divNumer;
    addrDivDirty = (uint32_t)(uintptr_t)&core->divSqrt.divDirty;
    addrSqrtCnt = (uint32_t)(uintptr_t)&core->divSqrt.sqrtCnt;
    addrSqrtParam = (uint32_t)(uintptr_t)&core->divSqrt.sqrtParam;
    addrSqrtDirty = (uint32_t)(uintptr_t)&core->divSqrt.sqrtDirty;
    addrGxStat = (uint32_t)(uintptr_t)&core->gpu3D.gxStat;

    // Banked registers for each mode that only banks R13 and R14; FIQ and invalid modes stay 0, which leaves them to C++
    auto banks = [&](int mode, uint32_t *r13, uint32_t *r14, uint32_t *spsr) {
        modeBanks[mode][0] = (uint32_t)(uintptr_t)r13;
        modeBanks[mode][1] = (uint32_t)(uintptr_t)r14;
        modeBanks[mode][2] = (uint32_t)(uintptr_t)spsr;
    };
    banks(0x10, &arm.registersUsr[13], &arm.registersUsr[14], nullptr);
    banks(0x1F, &arm.registersUsr[13], &arm.registersUsr[14], nullptr);
    banks(0x12, &arm.registersIrq[0], &arm.registersIrq[1], &arm.spsrIrq);
    banks(0x13, &arm.registersSvc[0], &arm.registersSvc[1], &arm.spsrSvc);
    banks(0x17, &arm.registersAbt[0], &arm.registersAbt[1], &arm.spsrAbt);
    banks(0x1B, &arm.registersUnd[0], &arm.registersUnd[1], &arm.spsrUnd);
    addrModeBanks = (uint32_t)(uintptr_t)&modeBanks[0][0];
    addrLoad32 = (uint32_t)(uintptr_t)(arm7 ? &jitLoad32<true> : &jitLoad32<false>);
    addrLoad8 = (uint32_t)(uintptr_t)(arm7 ? &jitLoad8<true> : &jitLoad8<false>);
    addrStore32 = (uint32_t)(uintptr_t)(arm7 ? &jitStore32<true> : &jitStore32<false>);
    addrStore8 = (uint32_t)(uintptr_t)(arm7 ? &jitStore8<true> : &jitStore8<false>);
    addrLoad16 = (uint32_t)(uintptr_t)(arm7 ? &jitLoad16<true> : &jitLoad16<false>);
    addrStore16 = (uint32_t)(uintptr_t)(arm7 ? &jitStore16<true> : &jitStore16<false>);
    addrCp15 = (uint32_t)(uintptr_t)&core->cp15;
    addrCodeLines = (uint32_t)(uintptr_t)&core->cp15.codeLines[0];
    addrCodeMiss = (uint32_t)(uintptr_t)&Cp15::codeMissCall;
    PpcEmitter e(codeArea);
    emitStubs(e);
    codeStart = (uint8_t*)e.here();
    codePtr = codeStart;
    finishCode(codeArea, codePtr - codeArea, std::vector<uint32_t*>());
}

void Jit::emitStubs(PpcEmitter &e) {
    // Refresh: load the global cycles and when to stop next, after anything that could have changed them
    // This only changes r4 and r5, so a return value in r3 or a loaded value in r8 stays intact
    codeRefresh = e.here();
    e.lwz(R_CYCLES, 0, R_GLOBAL);
    e.lwz(4, 0, R_LIMIT);
    e.addi(4, 4, 1);
    e.lwz(5, 0, R_EVENTS);
    e.lwz(R_NEXT, offEventCycles, 5);
    e.cmpwi(4, 0); // A limit of the highest value can't be passed
    uint32_t *refreshed1 = e.here();
    e.beq(nullptr);
    e.cmplw(4, R_NEXT);
    uint32_t *refreshed2 = e.here();
    e.bge(nullptr);
    e.mr(R_NEXT, 4);
    e.patchBc(refreshed1);
    e.patchBc(refreshed2);
    e.blr();

    // Exit: restore saved registers and return to the caller
    codeExit = e.here();
    e.lwz(0, 84, 1);
    e.mtlr(0);
    e.lmw(R_NEXT, 20, 1);
    e.addi(1, 1, 80);
    e.blr();

    // Entry: save registers, load the state pointers, and jump to the block, which sets its program counter
    // Arguments are the interpreter, event vector, limit, block code, global cycles, and cache
    codeEnter = e.here();
    e.stwu(1, -80, 1);
    e.mflr(0);
    e.stw(0, 84, 1);
    e.stmw(R_NEXT, 20, 1);
    e.li(R_BATCH, 0);
    e.mr(R_ARM, 3);
    e.mr(R_EVENTS, 4);
    e.mr(R_LIMIT, 5);
    e.mr(R_GLOBAL, 7);
    e.mr(R_CACHE, 8);
    e.loadConst(R_RMAP, addrReadMap);
    e.loadConst(R_WMAP, addrWriteMap);
    e.call(codeRefresh);
    e.mtctr(6);
    e.bctr();

    // When the next instruction can't run straight away, call the interleave helper, which runs the ARM7
    // like the interpreter would and says whether to continue; it needs a stack frame to keep the return address
    // The first version stores the program counter first, for instructions that don't set it themselves
    // The CPU's cycles are from the new value in r3, while the global cycles are still from the start of the instruction
    codeSlowPost[1] = e.here();
    e.stw(R_PC, offPc, R_ARM);
    codeSlowPost[0] = e.here();
    e.stw(3, offCycles, R_ARM);
    e.stw(R_CYCLES, 0, R_GLOBAL);
    e.stwu(1, -16, 1);
    e.mflr(0);
    e.stw(0, 20, 1);
    e.loadConst(3, (uint32_t)(uintptr_t)this);
    e.mr(4, R_LIMIT);
    e.call((void*)&Jit::interleaveCall);
    e.call(codeRefresh);
    e.lwz(0, 20, 1);
    e.mtlr(0);
    e.addi(1, 1, 16);
    e.cmpwi(3, 0);
    e.beq(codeExit);
    e.blr();

    // After an instruction, with its cycles in r3: add them, doubled for the half-speed ARM7 and halved for the DSi's
    // ARM9 at 133MHz, then continue if nothing else needs to run first; impure versions also check if code changed,
    // since the block might be stale. The DSi's ARM9 gets versions for both of its speeds, and at 133MHz a cheaper one
    // for fast timing; its ARM7 gets a second version that doesn't double, for code outside main RAM
    for (int half = 0; half < (!core->dsiMode ? 1 : arm7 ? 2 : 3); half++) {
        for (int store = 0; store < 2; store++) {
            for (int impure = 0; impure < 2; impure++) {
                codePost[half][impure][store] = e.here();
                if (arm7 && !half) e.add(3, 3, 3);
                uint32_t *none = nullptr;
                if (arm7) {
                }
                else if (half == 2) {
                    // Fast timing doesn't need the odd half cycle; an instruction that rounds down to no time is fine,
                    // since a loop always has a taken branch, which takes at least one
                    e.rlwinm(3, 3, 31, 1, 31);
                }
                else if (half) {
                    // Keep an odd half cycle for the next instruction, like the interpreter
                    e.lbz(4, offDsiCycle, R_ARM);
                    e.add(3, 3, 4);
                    e.rlwinm(4, 3, 0, 31, 31);
                    e.stb(4, offDsiCycle, R_ARM);
                    e.rlwinm_(3, 3, 31, 1, 31);
                    none = e.here();
                    e.beq(nullptr);
                }
                e.add(3, 3, R_CYCLES);
                if (impure) {
                    e.lwz(4, -int(sizeof(JitCacheEntry)), R_CACHE);
                    e.cmpwi(4, 0);
                    e.bne(codeSlowPost[store]);
                }
                e.cmplw(3, R_NEXT);
                e.bge(codeSlowPost[store]);
                e.mr(R_CYCLES, 3);
                e.blr();
                if (!none) continue;

                // An instruction that took no time also stops at the limit, since the run loop would run the ARM7
                // next if it was due when the instruction started, which is now; it stops for an event made due now
                // as well
                e.patchBc(none);
                e.mr(3, R_CYCLES);
                if (impure) {
                    e.lwz(4, -int(sizeof(JitCacheEntry)), R_CACHE);
                    e.cmpwi(4, 0);
                    e.bne(codeSlowPost[store]);
                }
                e.cmplw(3, R_NEXT);
                e.bge(codeSlowPost[store]);
                e.lwz(4, 0, R_LIMIT);
                e.cmplw(3, 4);
                e.bge(codeSlowPost[store]);
                e.blr();
            }
        }
    }

    // Fetch: before an instruction in a line of main RAM that the ARM9's code cache doesn't hold, with the program
    // counter register at that instruction, bring the line in and count the time it takes, with any run's so far, like
    // an instruction that does nothing else; the program counter is stored as it is before the instruction, so stopping
    // here leaves the instruction to run next
    for (int half = 0; half < ((core->dsiMode && !arm7) ? 3 : 0); half++) {
        for (int thumb = 0; thumb < 2; thumb++) {
            codeFetch[half][thumb] = e.here();
            e.stwu(1, -16, 1);
            e.mflr(0);
            e.stw(0, 20, 1);
            e.loadConst(3, addrCp15);
            e.addi(4, R_PC, thumb ? -4 : -8);
            e.call(addrCodeMiss);
            e.lwz(0, 20, 1);
            e.mtlr(0);
            e.addi(1, 1, 16);
            e.addi(4, R_PC, thumb ? -2 : -4);
            e.stw(4, offPc, R_ARM);
            if (half) e.add(3, 3, 3);
            e.add(3, 3, R_BATCH);
            e.li(R_BATCH, 0);
            e.b(codePost[half][0][0]);
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
    e.stw(R_CYCLES, 0, R_GLOBAL);
    e.stw(R_CYCLES, offCycles, R_ARM);
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

    // Shifts of r4 by the low byte of r6, which is changed, for each type of shift
    for (int type = 0; type < 4; type++) {
        codeShift[type] = e.here();
        if (type == 3) {
            e.subfic(6, 6, 32);
            e.rlwnm(4, 4, 6, 0, 31);
            e.blr();
            continue;
        }
        e.rlwinm(6, 6, 0, 24, 31);
        e.li(7, 32);
        e.cmplw(6, 7);
        uint32_t *large = e.here();
        e.bge(nullptr);
        (type == 0) ? e.slw(4, 4, 6) : (type == 1) ? e.srw(4, 4, 6) : e.sraw(4, 4, 6);
        e.blr();
        e.patchBc(large);
        (type == 2) ? e.srawi(4, 4, 31) : e.li(4, 0);
        e.blr();
    }

    // The same with the shifter carry in r7, which is the current carry if the shift is 0
    for (int type = 0; type < 4; type++) {
        codeShiftCarry[type] = e.here();
        e.rlwinm(6, 6, 0, 24, 31);
        e.cmpwi(6, 0);
        uint32_t *shifted = e.here();
        e.bne(nullptr);
        e.lwz(7, offCpsr, R_ARM);
        e.rlwinm(7, 7, 3, 31, 31);
        e.blr();
        e.patchBc(shifted);

        // Rotates keep every bit, using the amount modulo 32
        if (type == 3) {
            e.addi(8, 6, -1);
            e.rlwinm(8, 8, 0, 27, 31);
            e.srw(7, 4, 8);
            e.rlwinm(7, 7, 0, 31, 31);
            e.subfic(6, 6, 32);
            e.rlwnm(4, 4, 6, 0, 31);
            e.blr();
            continue;
        }

        // Host shifts use 6 bits of the amount, so a shift of 32 works, leaving only larger ones to handle apart
        e.li(7, 32);
        e.cmplw(6, 7);
        uint32_t *large = e.here();
        e.bgt(nullptr);
        if (type == 0) {
            e.subfic(8, 6, 32);
            e.srw(7, 4, 8);
            e.slw(4, 4, 6);
        }
        else {
            e.addi(8, 6, -1);
            (type == 1) ? e.srw(7, 4, 8) : e.sraw(7, 4, 8);
            (type == 1) ? e.srw(4, 4, 6) : e.sraw(4, 4, 6);
        }
        e.rlwinm(7, 7, 0, 31, 31);
        e.blr();
        e.patchBc(large);
        if (type == 2) {
            e.srawi(4, 4, 31);
            e.rlwinm(7, 4, 0, 31, 31);
        }
        else {
            e.li(4, 0);
            e.li(7, 0);
        }
        e.blr();
    }

    // The ARM7's multiply timing in r6 from the signed value of r4: 1 to 4, for how many bytes the value needs
    codeMultiplyCycles = e.here();
    e.srawi(6, 4, 31);
    e.xor_(6, 4, 6);
    e.ori(6, 6, 1);
    e.cntlzw(6, 6);
    e.subfic(6, 6, 32);
    e.addi(6, 6, 7);
    e.rlwinm(6, 6, 29, 3, 31);
    e.blr();

    // MSR to the CPSR with the value in r4, for the bottom byte and then also the top one
    // Only the same mode and THUMB state with no interrupt to take are done here, since the interpreter has the rest
    // User mode can only change the top byte, and bit 4 of the mode is always set
    for (int top = 0; top < 2; top++) {
        codeMsr[top] = e.here();
        e.lwz(6, offCpsr, R_ARM);
        e.andi_(7, 6, 0x1F);
        e.cmpwi(7, 0x10);
        uint32_t *user = e.here();
        e.beq(nullptr);
        e.rlwinm(7, 4, 0, 24, 31);
        e.ori(7, 7, 0x10);
        e.xor_(8, 7, 6);
        e.andi_(8, 8, 0x3F);
        uint32_t *slow1 = e.here();
        e.bne(nullptr);
        e.andi_(8, 7, BIT(7));
        uint32_t *masked = e.here();
        e.bne(nullptr);
        e.lwz(8, offIe, R_ARM);
        e.lwz(9, offIrf, R_ARM);
        e.and_(8, 8, 9);
        e.lwz(9, offIe + 4, R_ARM);
        e.lwz(10, offIrf + 4, R_ARM);
        e.and_(9, 9, 10);
        e.or_(8, 8, 9);
        e.cmpwi(8, 0);
        uint32_t *slow2 = e.here();
        e.bne(nullptr);
        e.patchBc(masked);
        e.rlwimi(6, 7, 0, 24, 31);
        e.patchBc(user);
        if (top) {
            e.rlwinm(6, 6, 0, 8, 31);
            e.rlwimi(6, 4, 0, 0, arm7 ? 3 : 4);
        }
        e.stw(6, offCpsr, R_ARM);
        e.blr();

        e.patchBc(slow1);
        e.patchBc(slow2);
        e.stwu(1, -16, 1);
        e.mflr(0);
        e.stw(0, 20, 1);
        e.stw(R_CYCLES, 0, R_GLOBAL);
        e.stw(R_CYCLES, offCycles, R_ARM);
        e.mr(3, R_ARM);
        e.mr(5, 4);
        e.loadConst(4, top ? (BIT(16) | BIT(19)) : BIT(16));
        e.call(addrWriteCpsr);
        e.call(codeRefresh);
        e.lwz(0, 20, 1);
        e.mtlr(0);
        e.addi(1, 1, 16);
        e.blr();
    }

    // Return from an exception to the address in r5, restoring the CPSR from the SPSR like an ALU instruction with flags
    // that writes the program counter; only modes without FIQ's banked R8-R12 and no interrupt to take are done here
    {
        codeReturn = e.here();
        e.lwz(6, offSpsr, R_ARM);
        e.cmpwi(6, 0);
        uint32_t *slow1 = e.here();
        e.beq(nullptr);
        e.lwz(6, 0, 6);
        e.andi_(8, 6, BIT(7));
        uint32_t *masked = e.here();
        e.bne(nullptr);
        e.lwz(8, offIe, R_ARM);
        e.lwz(9, offIrf, R_ARM);
        e.and_(8, 8, 9);
        e.lwz(9, offIe + 4, R_ARM);
        e.lwz(10, offIrf + 4, R_ARM);
        e.and_(9, 9, 10);
        e.or_(8, 8, 9);
        e.cmpwi(8, 0);
        uint32_t *slow2 = e.here();
        e.bne(nullptr);
        e.patchBc(masked);

        // Swap the banked registers if the mode changes
        e.lwz(7, offCpsr, R_ARM);
        e.xor_(8, 6, 7);
        e.andi_(8, 8, 0x1F);
        uint32_t *same = e.here();
        e.beq(nullptr);
        e.andi_(8, 7, 0x1F);
        e.cmpwi(8, 0x11);
        uint32_t *slow3 = e.here();
        e.beq(nullptr);
        e.loadConst(8, addrModeBanks);
        e.rlwinm(9, 6, 4, 23, 27);
        e.add(8, 8, 9);
        e.lwz(9, 0, 8);
        e.cmpwi(9, 0);
        uint32_t *slow4 = e.here();
        e.beq(nullptr);
        e.stw(9, offRegs + 13 * 4, R_ARM);
        e.lwz(9, 4, 8);
        e.stw(9, offRegs + 14 * 4, R_ARM);
        e.lwz(9, 8, 8);
        e.stw(9, offSpsr, R_ARM);
        e.patchBc(same);
        e.stw(6, offCpsr, R_ARM);

        // Set the program counter as if the pipeline was flushed in the restored state
        uint32_t *jump = e.here();
        e.andi_(7, 6, BIT(5));
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
        e.blr();

        // The interpreter does the rest, which can schedule an interrupt; the address is kept on the stack during the call
        e.patchBc(slow1);
        e.patchBc(slow2);
        e.patchBc(slow3);
        e.patchBc(slow4);
        e.stwu(1, -16, 1);
        e.mflr(0);
        e.stw(0, 20, 1);
        e.stw(5, 8, 1);
        e.stw(R_CYCLES, 0, R_GLOBAL);
        e.stw(R_CYCLES, offCycles, R_ARM);
        e.mr(3, R_ARM);
        e.lwz(4, offCpsr, R_ARM);
        e.call(addrRestoreCpsr);
        e.call(codeRefresh);
        e.lwz(5, 8, 1);
        e.lwz(0, 20, 1);
        e.mtlr(0);
        e.addi(1, 1, 16);
        e.lwz(6, offCpsr, R_ARM);
        e.b(jump);
    }

    // Set the sticky Q flag if the sum in r5 of r6 and r7 overflowed
    codeOverflowQ = e.here();
    e.xor_(8, 5, 6);
    e.xor_(9, 5, 7);
    e.and_(8, 8, 9);
    e.rlwinm(8, 8, 28, 4, 4);
    e.lwz(9, offCpsr, R_ARM);
    e.or_(9, 9, 8);
    e.stw(9, offCpsr, R_ARM);
    e.blr();

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
        if (!arm7 && size > 1) emitMath(e, load, size);
        e.stwu(1, -16, 1);
        e.mflr(0);
        e.stw(0, 20, 1);
        e.stw(R_CYCLES, 0, R_GLOBAL);
        e.stw(R_CYCLES, offCycles, R_ARM);
        e.loadConst(3, addrMemory);
        e.mr(4, 5);
        if (!load) e.mr(5, 8);
        if (load) e.call((size == 4) ? addrLoad32 : (size == 1) ? addrLoad8 : addrLoad16);
        else e.call((size == 4) ? addrStore32 : (size == 1) ? addrStore8 : addrStore16);
        if (load) e.mr(8, 3);
        e.call(codeRefresh);
        e.lwz(0, 20, 1);
        e.mtlr(0);
        e.addi(1, 1, 16);
        e.blr();
    }
}

void Jit::emitMath(PpcEmitter &e, bool load, int size) {
    // Access the math registers that games save and restore on every thread switch, with the address in r5 and value in r8
    // Anything else falls through to the memory handlers, including results that might have to be calculated first
    static_assert(offsetof(DivSqrt, divDenom) == offsetof(DivSqrt, divNumer) + 8, "Generated code expects these together");
    static_assert(sizeof(DivSqrt::divCnt) == 2 && sizeof(DivSqrt::sqrtCnt) == 2 && sizeof(DivSqrt::sqrtParam) == 8, "Generated code expects these sizes");
    std::vector<uint32_t*> others;
    auto other = [&]() {
        others.push_back(e.here());
        e.bne(nullptr);
    };
    auto setDirty = [&](uint32_t address) {
        e.li(7, 1);
        e.loadConst(9, address);
        e.stb(7, 0, 9);
    };
    e.xoris(6, 5, 0x0400);
    e.rlwinm(7, 6, 0, 0, 25);
    e.cmpwi(7, 0x200);
    uint32_t *notInterrupts = e.here();
    e.bne(nullptr);
    e.rlwinm(6, 6, 0, 26, 31);

    // A register's address only matches if it's aligned, so misaligned accesses are left to the memory handlers
    // Interrupt registers keep their high words first on the host, and the top half only matters on the ARM7
    auto skip = [&](int offset) {
        e.cmpwi(6, offset);
        uint32_t *at = e.here();
        e.bne(nullptr);
        return at;
    };

    // Whether an interrupt would be sent, from the low word of IE in r7, when the CPU isn't masking them
    // Taking it is left to the interpreter, so this is checked before anything is stored
    auto pending = [&]() {
        e.lwz(9, offIrf + 4, R_ARM);
        e.and_(9, 9, 7);
        e.lwz(10, offIe, R_ARM);
        e.lwz(0, offIrf, R_ARM);
        e.and_(10, 10, 0);
        e.or_(9, 9, 10);
        e.cmpwi(9, 0);
        other();
    };

    if (load) {
        uint32_t *notIme = skip(0x08);
        e.lbz(8, offIme, R_ARM);
        e.blr();
        e.patchBc(notIme);
        if (size == 4) {
            uint32_t *notIe = skip(0x10);
            e.lwz(8, offIe + 4, R_ARM);
            e.blr();
            e.patchBc(notIe);
            uint32_t *notIrf = skip(0x14);
            e.lwz(8, offIrf + 4, R_ARM);
            e.blr();
            e.patchBc(notIrf);
        }
    }
    else {
        uint32_t *notIme = skip(0x08);
        e.andi_(7, 8, 1);
        uint32_t *disabling = e.here();
        e.beq(nullptr);
        e.lwz(9, offCpsr, R_ARM);
        e.andi_(9, 9, BIT(7));
        uint32_t *masked = e.here();
        e.bne(nullptr);
        e.lwz(7, offIe + 4, R_ARM);
        pending();
        e.patchBc(disabling);
        e.patchBc(masked);
        e.rlwinm(7, 8, 0, 31, 31);
        e.stb(7, offIme, R_ARM);
        e.blr();
        e.patchBc(notIme);

        if (size == 4) {
            // The ARM9 can't change bit 7 of IE
            uint32_t *notIe = skip(0x10);
            e.mr(7, 8);
            e.lwz(9, offIe + 4, R_ARM);
            e.rlwimi(7, 9, 0, 24, 24);
            e.lbz(9, offIme, R_ARM);
            e.cmpwi(9, 0);
            uint32_t *disabled = e.here();
            e.beq(nullptr);
            e.lwz(9, offCpsr, R_ARM);
            e.andi_(9, 9, BIT(7));
            uint32_t *maskedIe = e.here();
            e.bne(nullptr);
            pending();
            e.patchBc(disabled);
            e.patchBc(maskedIe);
            e.stw(7, offIe + 4, R_ARM);
            e.blr();
            e.patchBc(notIe);

            // Clearing IF can send the GXFIFO interrupt again if the FIFO still meets its condition
            uint32_t *notIrf = skip(0x14);
            e.loadConst(9, addrGxStat);
            e.lwz(9, 0, 9);
            e.rlwinm(10, 9, 2, 30, 31);
            e.cmpwi(10, 1);
            uint32_t *notHalf = e.here();
            e.bne(nullptr);
            e.rlwinm_(9, 9, 7, 31, 31);
            other();
            uint32_t *clear = e.here();
            e.b(nullptr);
            e.patchBc(notHalf);
            e.cmpwi(10, 2);
            uint32_t *noCondition = e.here();
            e.bne(nullptr);
            e.rlwinm_(9, 9, 6, 31, 31);
            other();
            e.patchB(clear);
            e.patchBc(noCondition);
            e.lwz(7, offIrf + 4, R_ARM);
            e.andc(7, 7, 8);
            e.stw(7, offIrf + 4, R_ARM);
            e.blr();
            e.patchBc(notIrf);
        }
    }
    uint32_t *done = e.here();
    e.b(nullptr);

    // The math registers
    e.patchBc(notInterrupts);
    e.cmpwi(7, 0x280);
    other();
    e.rlwinm(6, 6, 0, 26, 31);

    if (size == 2) {
        // Only the control bits can be written, and DIVCNT has to be up to date to read it
        for (int sqrt = 1; sqrt >= 0; sqrt--) {
            e.cmpwi(6, sqrt ? 0x30 : 0);
            uint32_t *next = e.here();
            e.bne(nullptr);
            if (load && !sqrt) {
                e.loadConst(9, addrDivDirty);
                e.lbz(7, 0, 9);
                e.cmpwi(7, 0);
                other();
            }
            e.loadConst(9, sqrt ? addrSqrtCnt : addrDivCnt);
            if (load) {
                e.lhz(8, 0, 9);
            }
            else {
                e.lhz(7, 0, 9);
                e.rlwimi(7, 8, 0, sqrt ? 31 : 30, 31);
                e.sth(7, 0, 9);
                setDirty(sqrt ? addrSqrtDirty : addrDivDirty);
            }
            e.blr();
            e.patchBc(next);
        }
    }
    else {
        // The numerator and denominator are next to each other, as 64-bit values with their high words first on the host
        e.andi_(7, 5, 3);
        other();
        for (int sqrt = 0; sqrt < 2; sqrt++) {
            e.rlwinm(7, 6, 0, 26, sqrt ? 28 : 27);
            e.cmpwi(7, sqrt ? 0x38 : 0x10);
            uint32_t *next = e.here();
            e.bne(nullptr);
            e.addi(6, 6, sqrt ? -0x38 : -0x10);
            e.xori(6, 6, 4);
            e.loadConst(9, sqrt ? addrSqrtParam : addrDivNumer);
            if (load) {
                e.lwzx(8, 9, 6);
            }
            else {
                e.stwx(8, 9, 6);
                setDirty(sqrt ? addrSqrtDirty : addrDivDirty);
            }
            e.blr();
            e.patchBc(next);
        }
    }
    e.patchB(done);
    for (uint32_t *at: others)
        e.patchBc(at);
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
    JS_START(flushStart);
    if (start) DCFlushRange(start, size);
    for (size_t i = 0; i < patched.size(); i++)
        DCFlushRange(patched[i], 4);
    JS_ADD(JS_FLUSH, flushStart);
    JS_START(switchStart);
    BOOL rx = OSSwitchSecCodeGenMode(CODEGEN_R_X);
    JS_ADD(JS_SWITCH_RX, switchStart);
    JS_START(invalidateStart);
    if (start) ICInvalidateRange(start, size);
    for (size_t i = 0; i < patched.size(); i++)
        ICInvalidateRange(patched[i], 4);
    JS_ADD(JS_INVALIDATE, invalidateStart);
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
    NK_HALF, // LDRH/STRH/LDRSB/LDRSH
    NK_BLOCK, // LDM/STM
    NK_BLOCK_PC, // LDM that loads the program counter, like a function return
    NK_SWAP, // SWP/SWPB
    NK_MULTIPLY, // MUL/MLA and long multiplies
    NK_MISC, // CLZ and MRS
    NK_COPROCESSOR, // MCR that only changes the caches, and MRC of the DTCM base
    NK_MSR, // MSR
    NK_BLX, // BLX label
    NK_TRANSFER_PC, // LDR into the program counter
    NK_ALU_PC, // Data processing into the program counter, which with flags returns from an exception
    NK_DSP // The ARM9's halfword multiplies, QADD and QSUB
};

static bool cacheControl(uint32_t op) {
    // Check if an MCR is for the cache or write buffer, which aren't emulated, and not one of the writes that halts
    uint32_t reg = (((op >> 16) & 0xF) << 16) | ((op & 0xF) << 8) | ((op >> 5) & 0x7);
    // Writes that empty the code cache go to the interpreter too, which keeps track of what the cache holds
    return (op & 0x0F100010) == 0x0E000010 && (reg >> 16) == 7 && (reg >> 8) != 0x0705 && reg != 0x070004 && reg != 0x070802;
}

static bool restoresFlags(uint32_t op, bool arm7) {
    // Check for a return with a shifter that can change the carry, which the ARM7 keeps when it has no SPSR to restore
    uint32_t opc = (op >> 21) & 0xF;
    return arm7 && (op & BIT(20)) && (opc <= 1 || opc >= 12);
}

static int nativeKind(uint32_t op, bool arm7) {
    // Check which kind of native code an ARM instruction can be generated as, or NK_NONE
    // Anything else that writes the program counter or changes the CPU's mode is left to the interpreter
    // The ARM7 leaves BLX to the interpreter, which ignores it, and stores with writeback of a listed base, which
    // store the new base unless it's first
    if ((op >> 28) == 0xF) return (!arm7 && (op & 0x0E000000) == 0x0A000000) ? NK_BLX : NK_NONE;
    uint32_t rd = (op >> 12) & 0xF, rn = (op >> 16) & 0xF, rm = op & 0xF;
    if ((op & 0x0C000000) == 0x00000000) {
        if (!(op & BIT(25)) && (op & BIT(4))) {
            if ((op & 0x0FB00FF0) == 0x01000090) // SWP/SWPB
                return (rd == 15 || rn == 15 || rm == 15) ? NK_NONE : NK_SWAP;
            if ((op & 0x0FFFFFD0) == 0x012FFF10) // BX/BLX
                return (rm == 15 || (arm7 && (op & BIT(5)))) ? NK_NONE : NK_BRANCH_REG;
            if ((op & 0x0E000090) == 0x00000090 && (op & 0x60)) { // Halfword and signed transfers
                bool writeback = !(op & BIT(24)) || (op & BIT(21));
                if (rd == 15 || (writeback && rn == 15)) return NK_NONE;
                if (!(op & BIT(20)) && (op & 0x60) != 0x20) return NK_NONE; // LDRD/STRD
                return NK_HALF;
            }
            uint32_t rs = (op >> 8) & 0xF;
            if ((op & 0x0FD000F0) == 0x01000050) // QADD/QSUB
                return (arm7 || rd == 15 || rn == 15 || rm == 15) ? NK_NONE : NK_DSP;
            if ((op & 0x0FC000F0) == 0x00000090 || (op & 0x0F8000F0) == 0x00800090)
                return (rd == 15 || rn == 15 || rm == 15 || rs == 15) ? NK_NONE : NK_MULTIPLY;
            if (!arm7 && (op & 0x0FFF0FF0) == 0x016F0F10) // CLZ
                return (rd == 15 || rm == 15) ? NK_NONE : NK_MISC;
            if ((op & 0x0E000090) == 0x00000010) { // Data processing shifted by a register
                uint32_t opc = (op >> 21) & 0xF;
                if (!(op & BIT(20)) && opc >= 8 && opc <= 11) return NK_NONE; // Compares and tests only set flags
                if (rn == 15 || rm == 15 || rs == 15) return NK_NONE;
                if (rd == 15 && opc >= 8 && opc <= 11) return NK_NONE;
                return (rd == 15) ? (restoresFlags(op, arm7) ? NK_NONE : NK_ALU_PC) : NK_ALU;
            }
            return NK_NONE; // Other misc
        }
        uint32_t opc = (op >> 21) & 0xF;
        bool s = op & BIT(20);
        if ((op & 0x0F900090) == 0x01000080) { // Halfword multiplies
            uint32_t rs = (op >> 8) & 0xF;
            return (arm7 || rd == 15 || rn == 15 || rm == 15 || rs == 15) ? NK_NONE : NK_DSP;
        }
        if ((op & 0x0FBF0FFF) == 0x010F0000) return (rd == 15) ? NK_NONE : NK_MISC; // MRS
        if (((op & 0x0FB000F0) == 0x01200000 || (op & 0x0FB00000) == 0x03200000) && (op & 0x00090000)) // MSR
            return NK_MSR;
        if (opc >= 8 && opc <= 11) return s ? NK_ALU : NK_NONE; // Compares and tests, other encodings are misc
        return (rd == 15) ? (restoresFlags(op, arm7) ? NK_NONE : NK_ALU_PC) : NK_ALU;
    }
    if ((op & 0x0E000000) == 0x0A000000) // B/BL
        return NK_BRANCH;
    if ((op & 0x0F000010) == 0x0E000010) { // MCR/MRC, undefined on the ARM7 and for coprocessors other than CP15
        if (arm7 || ((op >> 8) & 0xF) != 15) return NK_NONE;
        if (cacheControl(op)) return NK_COPROCESSOR;
        return ((op & 0x0F1F00FF) == 0x0E190011 && rd != 15) ? NK_COPROCESSOR : NK_NONE;
    }
    if ((op & 0x0C000000) == 0x04000000) { // LDR/STR/LDRB/STRB
        if ((op & BIT(25)) && (op & BIT(4))) return NK_NONE; // Undefined
        bool writeback = !(op & BIT(24)) || (op & BIT(21));
        if (!(op & BIT(24)) && (op & BIT(21))) return NK_NONE; // LDRT/STRT
        if (writeback && rn == 15) return NK_NONE;
        if (rd == 15) return ((op & 0x00500000) == 0x00100000) ? NK_TRANSFER_PC : NK_NONE; // Only words loaded into it
        return NK_TRANSFER;
    }
    if ((op & 0x0E000000) == 0x08000000) { // LDM/STM
        uint32_t list = op & 0xFFFF;
        if (!list || rn == 15) return NK_NONE;
        if ((op & BIT(22)) && ((op & BIT(21)) || (list & BIT(15)))) return NK_NONE; // Only user registers without writeback
        if ((op & BIT(20)) && (op & BIT(21)) && (list & BIT(rn))) return NK_NONE; // Loading the base with writeback
        if (arm7 && !(op & BIT(20)) && (op & BIT(21)) && (list & BIT(rn))) return NK_NONE; // Storing it on ARM7
        if (list & BIT(15)) return (op & BIT(20)) ? NK_BLOCK_PC : NK_NONE; // Only loads of the program counter
        return NK_BLOCK;
    }
    return NK_NONE;
}

static uint32_t blxTarget(uint32_t op, uint32_t pc) {
    // Get the THUMB address that BLX label goes to, using the condition's place for bit 1
    return pc + ((int32_t)(op << 8) >> 6) + ((op & BIT(24)) >> 23);
}

static bool nativeJumps(int kind) {
    // Check if a native instruction kind can change the program counter
    return kind == NK_BRANCH || kind == NK_BRANCH_REG || kind == NK_BLOCK_PC || kind == NK_BLX || kind == NK_TRANSFER_PC || kind == NK_ALU_PC;
}

// The condition flags as generated code keeps track of them, in the order of bits 28 to 31 of the CPSR
enum { FLAG_V = BIT(0), FLAG_C = BIT(1), FLAG_Z = BIT(2), FLAG_N = BIT(3), FLAG_ALL = 0xF };

static void flagUse(uint32_t op, int kind, int &reads, int &writes, int &always) {
    // Get which flags a native instruction of a kind that can be in a run reads, which it can change, and which of those
    // it changes whenever it runs, so their old values can't be seen after it unless it has a condition, which reads them
    reads = writes = always = 0;
    if (kind == NK_ALU) {
        // Arithmetic sets every flag; logical operations leave V alone, and C too unless the shifter gives a new one,
        // which a shift by a register only does if the amount isn't 0
        uint32_t opc = (op >> 21) & 0xF;
        bool logical = (opc <= 1) || (opc >= 8 && opc <= 9) || opc >= 12;
        bool byRegister = !(op & BIT(25)) && (op & BIT(4));
        bool rrx = !(op & BIT(25)) && (op & 0xFF0) == 0x060;
        if ((opc >= 5 && opc <= 7) || rrx) reads |= FLAG_C;
        if (op & BIT(20)) {
            writes = always = logical ? (FLAG_N | FLAG_Z) : FLAG_ALL;
            if (logical && byRegister) writes |= FLAG_C;
            else if (logical && (op & ((op & BIT(25)) ? 0xF00 : 0xFE0))) writes = always = FLAG_N | FLAG_Z | FLAG_C;
        }
    }
    else if (kind == NK_MULTIPLY) {
        if (op & BIT(20)) writes = always = FLAG_N | FLAG_Z;
    }
    else if (kind == NK_MISC) {
        if (!(op & BIT(4))) reads = FLAG_ALL; // MRS
    }
    else if (kind == NK_TRANSFER) {
        if ((op & BIT(25)) && (op & 0xFF0) == 0x060) reads = FLAG_C; // An offset rotated through the carry
    }

    // A condition can read any of them
    if ((op >> 28) != 0xE) reads = FLAG_ALL;
}

static uint32_t thumbToArm(uint32_t op) {
    // Get the ARM instruction that does the same as a THUMB instruction, for the same native code, or one that doesn't
    // have any if there's none; the callers handle how the program counter and timing differ
    const uint32_t none = 0xF0000000;
    uint32_t rd = op & 0x7, rs = (op >> 3) & 0x7, rn = (op >> 6) & 0x7;
    uint32_t r8 = (op >> 8) & 0x7, imm = op & 0xFF;
    switch (op >> 11) {
    case 0x00: case 0x01: case 0x02: // LSL/LSR/ASR by an immediate
        return 0xE1B00000 | (rd << 12) | (((op >> 6) & 0x1F) << 7) | (((op >> 11) & 0x3) << 5) | rs;
    case 0x03: // ADD/SUB with a register or a small immediate
        return ((op & BIT(9)) ? 0xE0500000 : 0xE0900000) | ((op & BIT(10)) ? BIT(25) : 0) | (rs << 16) | (rd << 12) | rn;
    case 0x04: return 0xE3B00000 | (r8 << 12) | imm; // MOV
    case 0x05: return 0xE3500000 | (r8 << 16) | imm; // CMP
    case 0x06: return 0xE2900000 | (r8 << 16) | (r8 << 12) | imm; // ADD
    case 0x07: return 0xE2500000 | (r8 << 16) | (r8 << 12) | imm; // SUB
    case 0x08:
        if (!(op & BIT(10))) {
            switch ((op >> 6) & 0xF) { // Data processing
            case 0x0: return 0xE0100000 | (rd << 16) | (rd << 12) | rs; // AND
            case 0x1: return 0xE0300000 | (rd << 16) | (rd << 12) | rs; // EOR
            case 0x2: return 0xE1B00010 | (rd << 12) | (rs << 8) | rd; // LSL
            case 0x3: return 0xE1B00030 | (rd << 12) | (rs << 8) | rd; // LSR
            case 0x4: return 0xE1B00050 | (rd << 12) | (rs << 8) | rd; // ASR
            case 0x5: return 0xE0B00000 | (rd << 16) | (rd << 12) | rs; // ADC
            case 0x6: return 0xE0D00000 | (rd << 16) | (rd << 12) | rs; // SBC
            case 0x7: return 0xE1B00070 | (rd << 12) | (rs << 8) | rd; // ROR
            case 0x8: return 0xE1100000 | (rd << 16) | rs; // TST
            case 0x9: return 0xE2700000 | (rs << 16) | (rd << 12); // NEG
            case 0xA: return 0xE1500000 | (rd << 16) | rs; // CMP
            case 0xB: return 0xE1700000 | (rd << 16) | rs; // CMN
            case 0xC: return 0xE1900000 | (rd << 16) | (rd << 12) | rs; // ORR
            case 0xD: return 0xE0100090 | (rd << 16) | (rd << 8) | rs; // MUL
            case 0xE: return 0xE1D00000 | (rd << 16) | (rd << 12) | rs; // BIC
            case 0xF: return 0xE1F00000 | (rd << 12) | rs; // MVN
            }
        }
        else {
            uint32_t high = ((op >> 4) & 0x8) | rd, rm = (op >> 3) & 0xF;
            switch ((op >> 8) & 0x3) { // High registers
            case 0x0: return 0xE0800000 | (high << 16) | (high << 12) | rm; // ADD
            case 0x1: return 0xE1500000 | (high << 16) | rm; // CMP
            case 0x2: return 0xE1A00000 | (high << 12) | rm; // MOV
            }
            return none;
        }
    case 0x09: return 0xE59F0000 | (r8 << 12) | (imm << 2); // LDR from the program counter
    case 0x0A: case 0x0B: // Loads and stores at a register offset
        switch ((op >> 9) & 0x7) {
        case 0x0: return 0xE7800000 | (rs << 16) | (rd << 12) | rn; // STR
        case 0x1: return 0xE18000B0 | (rs << 16) | (rd << 12) | rn; // STRH
        case 0x2: return 0xE7C00000 | (rs << 16) | (rd << 12) | rn; // STRB
        case 0x3: return 0xE19000D0 | (rs << 16) | (rd << 12) | rn; // LDRSB
        case 0x4: return 0xE7900000 | (rs << 16) | (rd << 12) | rn; // LDR
        case 0x5: return 0xE19000B0 | (rs << 16) | (rd << 12) | rn; // LDRH
        case 0x6: return 0xE7D00000 | (rs << 16) | (rd << 12) | rn; // LDRB
        default: return 0xE19000F0 | (rs << 16) | (rd << 12) | rn; // LDRSH
        }
    case 0x0C: return 0xE5800000 | (rs << 16) | (rd << 12) | (((op >> 6) & 0x1F) << 2); // STR
    case 0x0D: return 0xE5900000 | (rs << 16) | (rd << 12) | (((op >> 6) & 0x1F) << 2); // LDR
    case 0x0E: return 0xE5C00000 | (rs << 16) | (rd << 12) | ((op >> 6) & 0x1F); // STRB
    case 0x0F: return 0xE5D00000 | (rs << 16) | (rd << 12) | ((op >> 6) & 0x1F); // LDRB
    case 0x10: case 0x11: { // STRH/LDRH
        uint32_t offset = (op >> 5) & 0x3E;
        return ((op & BIT(11)) ? 0xE1D000B0 : 0xE1C000B0) | (rs << 16) | (rd << 12) | ((offset & 0xF0) << 4) | (offset & 0xF);
    }
    case 0x12: return 0xE58D0000 | (r8 << 12) | (imm << 2); // STR to the stack pointer
    case 0x13: return 0xE59D0000 | (r8 << 12) | (imm << 2); // LDR from the stack pointer
    case 0x14: return 0xE28F0F00 | (r8 << 12) | imm; // ADD Rd,PC,#i, which is rotated to make it a word offset
    case 0x15: return 0xE28D0F00 | (r8 << 12) | imm; // ADD Rd,SP,#i
    case 0x16: case 0x17:
        if ((op & 0xFF00) == 0xB000) // ADD SP,#i
            return ((op & BIT(7)) ? 0xE24DDF00 : 0xE28DDF00) | (op & 0x7F);
        if ((op & 0xFE00) == 0xB400) // PUSH, with LR in place of bit 8
            return 0xE92D0000 | imm | ((op & BIT(8)) ? BIT(14) : 0);
        if ((op & 0xFE00) == 0xBC00) // POP, with PC in place of bit 8
            return 0xE8BD0000 | imm | ((op & BIT(8)) ? BIT(15) : 0);
        return none;
    case 0x18: return 0xE8A00000 | (r8 << 16) | imm; // STMIA
    case 0x19: // LDMIA, where a listed base is overwritten by what's loaded, so it's the same without writeback
        return ((imm & BIT(r8)) ? 0xE8900000 : 0xE8B00000) | (r8 << 16) | imm;
    }
    return none;
}

static bool thumbBranch(uint32_t op, bool arm7) {
    // Check if a THUMB instruction is one of the branches that has its own native code
    // BX/BLX from the program counter and the ARM7's BLX are left to the interpreter
    if ((op & 0xF000) == 0xD000) return (op & 0x0F00) < 0x0E00; // Conditional branches
    if ((op & 0xF800) == 0xE000 || (op & 0xF000) == 0xF000) return true; // B, BL
    if ((op & 0xF800) == 0xE800) return !arm7; // BLX label
    if ((op & 0xFF00) == 0x4700) return ((op >> 3) & 0xF) != 15 && !(arm7 && (op & BIT(7))); // BX/BLX
    return false;
}

uint32_t Jit::armOpcode(JitBlock *block, size_t i) {
    // Get an instruction as ARM, which is what decides its native code
    uint32_t op = block->instrs[i].opcode;
    return block->thumb ? thumbToArm(op) : op;
}

bool Jit::emitNative(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Generate an ARM instruction as native code if possible, leaving its cycles in r3
    // Branches leave the program counter set in memory; other instructions leave it unchanged
    int kind = nativeKind(op, arm7);
    if (!kind) return false;

    // Skip the instruction if its condition fails, which takes 1 cycle; BLX label has no condition
    uint32_t *skip = nullptr;
    if ((op >> 28) < 0xE) {
        e.call(codeCond[op >> 28]);
        skip = e.here();
        e.bne(nullptr);
    }

    switch (kind) {
        case NK_ALU: case NK_ALU_PC: emitAlu(e, op, pc); break;
        case NK_BRANCH: emitBranch(e, op, pc); break;
        case NK_TRANSFER: case NK_TRANSFER_PC: emitTransfer(e, op, pc); break;
        case NK_BRANCH_REG: emitBranchReg(e, op, pc); break;
        case NK_HALF: emitHalf(e, op, pc); break;
        case NK_BLOCK: case NK_BLOCK_PC: emitBlock(e, op, pc); break;
        case NK_SWAP: emitSwap(e, op, pc); break;
        case NK_MULTIPLY: emitMultiply(e, op, pc); break;
        case NK_MISC: emitMisc(e, op, pc); break;
        case NK_COPROCESSOR: emitCoprocessor(e, op); break;
        case NK_MSR: emitMsr(e, op, pc); break;
        case NK_BLX: emitBlx(e, op, pc); break;
        case NK_DSP: emitDsp(e, op, pc); break;
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

    // Writing the program counter is a jump, without switching to or from THUMB, and takes 2 more cycles
    // With flags, the CPSR is restored instead, so the flags aren't worked out
    uint32_t rd = (op >> 12) & 0xF;
    bool restore = rd == 15 && s && (opc < 8 || opc > 11);
    if (restore) s = false;
    bool jump = rd == 15 && !s;

    // Flags that nothing reads before they're set again aren't worked out, which only happens inside a run, where
    // nothing can stop to see them; the instruction is then generated as if it didn't set any
    int reads, writes, always;
    flagUse(op, NK_ALU, reads, writes, always);
    int need = s ? (writes & flagsLive) : 0;
    if (!need) s = false;
    auto store = [&](int rs) {
        if (restore) {
            if (rs != 5) e.mr(5, rs);
            e.call(codeReturn);
        }
        else if (jump) {
            emitJump(e, rs, false);
        }
        else {
            writeReg(e, rs, rd);
        }
    };

    // Without flags, moves and additions or subtractions of small values don't need their operands in r3 and r4
    if (!s) {
        if (op & BIT(25)) {
            uint32_t shift = (op >> 7) & 0x1E;
            uint32_t value = shift ? (((op & 0xFF) << (32 - shift)) | ((op & 0xFF) >> shift)) : (op & 0xFF);
            if (opc == 0xD || ((opc == 0x2 || opc == 0x4) && value < 0x8000)) {
                if (opc == 0xD) {
                    e.loadConst(5, value);
                }
                else {
                    readReg(e, 3, (op >> 16) & 0xF, pc);
                    e.addi(5, 3, (opc == 0x4) ? value : -int32_t(value));
                }
                store(5);
                e.li(3, jump ? 3 : 1);
                return;
            }
        }
        else if (opc == 0xD && !(op & BIT(4))) {
            emitShift(e, op, pc, false);
            store(4);
            e.li(3, jump ? 3 : 1);
            return;
        }
    }

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
    else if (op & BIT(4)) {
        readReg(e, 4, op & 0xF, pc);
        readReg(e, 6, (op >> 8) & 0xF, pc);
        carry = s && logical;
        e.call(carry ? codeShiftCarry[(op >> 5) & 0x3] : codeShift[(op >> 5) & 0x3]);
    }
    else {
        carry = emitShift(e, op, pc, s && logical);
    }

    // Get the carry flag for operations that use it, in the host's carry if flags are also set
    bool arith = s && !logical;
    if (opc >= 5 && opc <= 7) {
        e.lwz(6, offCpsr, R_ARM);
        e.rlwinm(6, 6, 3, 31, 31);
        if (arith) e.addic(6, 6, -1);
    }

    // Do the operation, with carry out for flag-setting arithmetic
    switch (opc) {
        case 0x0: case 0x8: e.and_(5, 3, 4); break; // AND/TST
        case 0x1: case 0x9: e.xor_(5, 3, 4); break; // EOR/TEQ
        case 0x2: case 0xA: arith ? e.subfc(5, 4, 3) : e.subf(5, 4, 3); break; // SUB/CMP
        case 0x3: arith ? e.subfc(5, 3, 4) : e.subf(5, 3, 4); break; // RSB
        case 0x4: case 0xB: arith ? e.addc(5, 3, 4) : e.add(5, 3, 4); break; // ADD/CMN
        case 0x5: // ADC
            if (arith) e.adde(5, 3, 4);
            else { e.add(5, 3, 4); e.add(5, 5, 6); }
            break;
        case 0x6: // SBC
            if (arith) e.subfe(5, 4, 3);
            else { e.subf(5, 4, 3); e.add(5, 5, 6); e.addi(5, 5, -1); }
            break;
        case 0x7: // RSC
            if (arith) e.subfe(5, 3, 4);
            else { e.subf(5, 3, 4); e.add(5, 5, 6); e.addi(5, 5, -1); }
            break;
        case 0xC: e.or_(5, 3, 4); break; // ORR
        case 0xD: e.mr(5, 4); break; // MOV
        case 0xE: e.andc(5, 3, 4); break; // BIC
        case 0xF: e.nor(5, 4, 4); break; // MVN
    }

    // Set flags with a shared routine, except for arithmetic that's only needed for its carry, as between the words of
    // a longer addition, which just copies the host's; nothing sets the carry without N and Z, so logical operations
    // have no such case
    if (s) {
        if (logical) {
            e.call(codeFlags[carry]);
        }
        else if (need == FLAG_C) {
            e.adde(9, 9, 9); // The host's carry in the low bit, whatever was there
            e.lwz(6, offCpsr, R_ARM);
            e.rlwimi(6, 9, 29, 2, 2);
            e.stw(6, offCpsr, R_ARM);
        }
        else {
            e.call(codeFlags[(opc == 0x4 || opc == 0x5 || opc == 0xB) ? 2 : (opc == 0x3 || opc == 0x7) ? 4 : 3]);
        }
    }

    // Store the result unless it's a compare or test
    if (opc < 8 || opc > 11)
        store(5);

    // Shifting by a register takes another cycle, except in THUMB
    e.li(3, ((!thumbCode && !(op & BIT(25)) && (op & BIT(4))) ? 2 : 1) + (jump ? 2 : 0));
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

void Jit::emitBlx(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Branch with link to THUMB, setting the program counter as if the pipeline was flushed
    e.loadConst(5, pc - 4);
    writeReg(e, 5, 14);
    e.lwz(5, offCpsr, R_ARM);
    e.ori(5, 5, BIT(5));
    e.stw(5, offCpsr, R_ARM);
    e.loadConst(5, blxTarget(op, pc) + 2);
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
        else if (!writeback) {
            readReg(e, 5, rn, pc);
            if (offset) e.addi(5, 5, up ? offset : -offset);
        }
        else {
            readReg(e, 3, rn, pc);
            e.addi(9, 3, up ? offset : -offset);
            e.mr(5, pre ? 9 : 3);
        }
    }
    else if (!writeback) {
        emitShift(e, op, pc, false);
        readReg(e, 3, rn, pc);
        up ? e.add(5, 3, 4) : e.subf(5, 4, 3);
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
    if (load && rd == 15) {
        emitJump(e, 8, !arm7);
        e.li(3, 5);
        return;
    }
    if (load) writeReg(e, 8, rd);
    e.li(3, arm7 ? (load ? 3 : 2) : 1);
}

void Jit::emitHalf(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Halfword or signed load, or halfword store, with any addressing mode
    // Writeback happens before the memory access, as with words
    uint32_t rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
    bool pre = op & BIT(24), up = op & BIT(23), load = op & BIT(20);
    bool writeback = !pre || (op & BIT(21));
    uint32_t type = (op >> 5) & 0x3; // 1 = halfword, 2 = signed byte, 3 = signed halfword
    if (op & BIT(22)) {
        uint32_t offset = ((op >> 4) & 0xF0) | (op & 0xF);
        if (rn == 15) {
            e.loadConst(5, up ? (pc + offset) : (pc - offset));
        }
        else if (!writeback) {
            readReg(e, 5, rn, pc);
            if (offset) e.addi(5, 5, up ? offset : -offset);
        }
        else {
            readReg(e, 3, rn, pc);
            e.addi(9, 3, up ? offset : -offset);
            e.mr(5, pre ? 9 : 3);
        }
    }
    else {
        readReg(e, 3, rn, pc);
        readReg(e, 4, op & 0xF, pc);
        int sum = writeback ? 9 : 5;
        up ? e.add(sum, 3, 4) : e.subf(sum, 4, 3);
        if (writeback) e.mr(5, pre ? 9 : 3);
    }

    if (load) {
        if (writeback) writeReg(e, 9, rn);
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
        if (writeback) writeReg(e, 9, rn);
        e.call(codeMemory[5]);
    }
    e.li(3, arm7 ? (load ? 3 : 2) : 1);
}

void Jit::emitBlock(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Block load or store of registers other than the program counter, with words at aligned addresses
    // The base is kept in R_TMP1, since memory calls can change volatile registers
    // With the S bit, the listed registers are the user mode ones; only the base is read from the current mode
    uint32_t rn = (op >> 16) & 0xF, list = op & 0xFFFF;
    bool pre = op & BIT(24), up = op & BIT(23), writeback = op & BIT(21), load = op & BIT(20), user = op & BIT(22);
    int count = 0;
    for (int i = 0; i < 16; i++)
        count += (list >> i) & 1;
    int32_t first = up ? (pre ? 4 : 0) : (pre ? -count * 4 : -count * 4 + 4);
    int32_t end = up ? count * 4 : -count * 4;
    readReg(e, R_TMP1, rn, pc);
    e.rlwinm(R_TMP2, R_TMP1, 0, 0, 29);

    // Loads write back first, since the base isn't loaded; stores write back last, storing the old base
    if (load && writeback) {
        e.addi(9, R_TMP1, end);
        writeReg(e, 9, rn);
    }
    int32_t offset = first;
    for (int i = 0; i < 16; i++) {
        if (!(list & BIT(i))) continue;
        e.addi(5, R_TMP2, offset);
        offset += 4;
        if (load) {
            e.call(codeMemory[0]);
            if (i == 15) emitJump(e, 8, !arm7);
            else if (user) e.stw(8, offUsr + i * 4, R_ARM);
            else writeReg(e, 8, i);
        }
        else {
            if (user) e.lwz(8, offUsr + i * 4, R_ARM);
            else readReg(e, 8, i, pc);
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

void Jit::emitMisc(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // CLZ, or MRS
    if (op & BIT(4)) {
        readReg(e, 3, op & 0xF, pc);
        e.cntlzw(5, 3);
        writeReg(e, 5, (op >> 12) & 0xF);
        e.li(3, 1);
    }
    else if (op & BIT(22)) {
        e.lwz(5, offSpsr, R_ARM);
        e.cmpwi(5, 0);
        uint32_t *none = e.here();
        e.beq(nullptr);
        e.lwz(5, 0, 5);
        uint32_t *store1 = e.here();
        e.b(nullptr);

        // In modes with none, a real DS reads the current ones, except the condition flags in the ARM9's system mode
        e.patchBc(none);
        e.lwz(5, offCpsr, R_ARM);
        uint32_t *store2 = nullptr;
        if (!arm7) {
            e.andi_(6, 5, 0x1F);
            e.cmpwi(6, 0x10);
            store2 = e.here();
            e.beq(nullptr);
            e.rlwinm(5, 5, 0, 24, 31);
            e.patchBc(store2);
        }
        e.patchB(store1);
        writeReg(e, 5, (op >> 12) & 0xF);
        e.li(3, arm7 ? 1 : 2);
    }
    else {
        e.lwz(5, offCpsr, R_ARM);
        writeReg(e, 5, (op >> 12) & 0xF);
        e.li(3, arm7 ? 1 : 2);
    }
}

void Jit::emitCoprocessor(PpcEmitter &e, uint32_t op) {
    // Cache and write buffer operations do nothing, and the only register read is the DTCM base
    if (op & BIT(20)) {
        e.loadConst(5, addrDtcmReg);
        e.lwz(5, 0, 5);
        writeReg(e, 5, (op >> 12) & 0xF);
    }
    e.li(3, 1);
}

void Jit::emitMsr(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // MSR, where the status registers only have the top and bottom bytes on a real DS
    if (op & BIT(25)) {
        uint32_t shift = (op >> 7) & 0x1E;
        e.loadConst(4, shift ? (((op & 0xFF) << (32 - shift)) | ((op & 0xFF) >> shift)) : (op & 0xFF));
    }
    else {
        readReg(e, 4, op & 0xF, pc);
    }

    // Writes to the CPSR's bottom byte use a shared routine, since they can change the CPU's mode or let an interrupt in
    if (!(op & BIT(22))) {
        if (op & BIT(16)) {
            e.call(codeMsr[(op >> 19) & 1]);
        }
        else {
            e.lwz(6, offCpsr, R_ARM);
            e.rlwinm(6, 6, 0, 8, 31);
            e.rlwimi(6, 4, 0, 0, arm7 ? 3 : 4);
            e.stw(6, offCpsr, R_ARM);
        }
        e.li(3, 1);
        return;
    }

    // Nothing happens for the SPSR in modes without one
    e.lwz(5, offSpsr, R_ARM);
    e.cmpwi(5, 0);
    uint32_t *none = e.here();
    e.beq(nullptr);
    e.lwz(6, 0, 5);
    if (op & BIT(16)) {
        e.rlwimi(6, 4, 0, 24, 31);
        e.ori(6, 6, 0x10);
    }
    if (op & BIT(19)) {
        e.rlwinm(6, 6, 0, 8, 31);
        e.rlwimi(6, 4, 0, 0, arm7 ? 3 : 4);
    }
    e.stw(6, 0, 5);
    e.patchBc(none);
    e.li(3, 1);
}

void Jit::emitMultiply(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // Multiply, with an optional accumulate and flags; only N and Z change
    // Timing is fixed on the ARM9, and depends on the value in r4 on the ARM7
    uint32_t high = (op >> 16) & 0xF, low = (op >> 12) & 0xF;
    // Flags that nothing reads aren't worked out, though the ARM9 still takes the time for them
    bool accumulate = op & BIT(21), flags = op & BIT(20);
    bool setFlags = flags && (flagsLive & (FLAG_N | FLAG_Z));
    readReg(e, 3, op & 0xF, pc);
    readReg(e, 4, (op >> 8) & 0xF, pc);
    e.mullw(5, 3, 4);
    if (!(op & BIT(23))) {
        if (accumulate) {
            readReg(e, 6, low, pc);
            e.add(5, 5, 6);
        }
        writeReg(e, 5, high);
        if (setFlags) e.call(codeFlags[0]);
        if (arm7) {
            e.call(codeMultiplyCycles);
            e.addi(3, 6, accumulate ? 2 : 1);
        }
        else {
            e.li(3, flags ? 4 : 2);
        }
        return;
    }

    // Long multiplies also get the high half, which is signed or not, and write the low half first
    (op & BIT(22)) ? e.mulhw(6, 3, 4) : e.mulhwu(6, 3, 4);
    if (accumulate) {
        readReg(e, 7, low, pc);
        readReg(e, 8, high, pc);
        e.addc(5, 5, 7);
        e.adde(6, 6, 8);
    }
    writeReg(e, 5, low);
    writeReg(e, 6, high);

    // Flags are from all 64 bits
    if (setFlags) {
        e.or_(7, 5, 6);
        e.cntlzw(7, 7);
        e.rlwinm(7, 7, 25, 1, 1);
        e.rlwinm(8, 6, 0, 0, 0);
        e.or_(8, 8, 7);
        e.lwz(9, offCpsr, R_ARM);
        e.rlwinm(9, 9, 0, 2, 31);
        e.or_(9, 9, 8);
        e.stw(9, offCpsr, R_ARM);
    }

    // On the ARM7, the unsigned ones always take the longest, since the interpreter's timing check is always true for them
    if (!arm7) {
        e.li(3, flags ? 5 : 3);
    }
    else if (op & BIT(22)) {
        e.call(codeMultiplyCycles);
        e.addi(3, 6, accumulate ? 3 : 2);
    }
    else {
        e.li(3, accumulate ? 7 : 6);
    }
}

void Jit::emitDsp(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // The ARM9's halfword multiplies, then saturated additions and subtractions, which set the sticky Q flag on overflow
    uint32_t rd = (op >> 16) & 0xF, rn = (op >> 12) & 0xF;
    if (op & BIT(4)) {
        bool sub = op & BIT(21);
        readReg(e, 3, op & 0xF, pc);
        readReg(e, 4, rd, pc);
        if (sub) {
            e.subf(5, 4, 3);
            e.xor_(6, 3, 4);
            e.xor_(7, 3, 5);
        }
        else {
            e.add(5, 3, 4);
            e.xor_(6, 5, 3);
            e.xor_(7, 5, 4);
        }
        e.and_(6, 6, 7);
        e.cmpwi(6, 0);
        uint32_t *fits = e.here();
        e.bge(nullptr);
        e.srawi(5, 3, 31);
        e.loadConst(6, 0x7FFFFFFF);
        e.xor_(5, 5, 6);
        e.lwz(6, offCpsr, R_ARM);
        e.oris(6, 6, BIT(27) >> 16);
        e.stw(6, offCpsr, R_ARM);
        e.patchBc(fits);
        writeReg(e, 5, rn);
        e.li(3, 1);
        return;
    }

    // Get the halfwords, which bits 5 and 6 choose; words multiply with a whole first operand and don't use bit 5 for that
    uint32_t type = (op >> 21) & 0x3; // 0 = SMLA, 1 = SMLAW/SMULW, 2 = SMLAL, 3 = SMUL
    readReg(e, 3, op & 0xF, pc);
    readReg(e, 4, (op >> 8) & 0xF, pc);
    if (type != 1) (op & BIT(5)) ? e.srawi(3, 3, 16) : e.extsh(3, 3);
    (op & BIT(6)) ? e.srawi(4, 4, 16) : e.extsh(4, 4);
    e.mullw(5, 3, 4);

    // Long multiplies add to 64 bits, and take another cycle
    if (type == 2) {
        e.srawi(6, 5, 31);
        readReg(e, 7, rn, pc);
        readReg(e, 8, rd, pc);
        e.addc(5, 5, 7);
        e.adde(6, 6, 8);
        writeReg(e, 5, rn);
        writeReg(e, 6, rd);
        e.li(3, 2);
        return;
    }

    // Word multiplies keep the middle 32 of the 48 bits
    bool accumulate = (type == 0) || (type == 1 && !(op & BIT(5)));
    if (type == 1) {
        e.mulhw(6, 3, 4);
        e.rlwinm(5, 5, 16, 16, 31);
        e.rlwimi(5, 6, 16, 0, 15);
    }
    if (accumulate) {
        readReg(e, 6, rn, pc);
        e.mr(7, 5);
        e.add(5, 7, 6);
        e.call(codeOverflowQ);
    }
    writeReg(e, 5, rd);
    e.li(3, 1);
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
    // Without interworking, like loads of the program counter on the ARM7, it stays in the same mode
    // THUMB code switches to ARM if bit 0 is clear
    if (rs != 5) e.mr(5, rs);
    if (!interwork) {
        e.rlwinm(8, 5, 0, 0, thumbCode ? 30 : 29);
        e.addi(8, 8, thumbCode ? 2 : 4);
        e.stw(8, offPc, R_ARM);
        return;
    }
    e.lwz(6, offCpsr, R_ARM);
    if (thumbCode) e.rlwinm(6, 6, 0, 27, 25);
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

void Jit::emitThumbBranch(PpcEmitter &e, uint32_t op, uint32_t pc) {
    // B, the two halves of BL and BLX, or BX and BLX with a register, leaving the cycles in r3
    // The first half of BL only sets the link register, and the second half can be reached without it, so it adds to that
    if ((op & 0xF800) == 0xE000) { // B
        e.loadConst(5, pc + ((int32_t)(op << 21) >> 20) + 2);
        e.stw(5, offPc, R_ARM);
        e.li(3, 3);
    }
    else if ((op & 0xF800) == 0xF000) { // First half of BL/BLX
        e.loadConst(5, pc + ((int32_t)(op << 21) >> 9));
        writeReg(e, 5, 14);
        e.li(3, 1);
    }
    else if ((op & 0xF000) == 0xF000 || (op & 0xF800) == 0xE800) { // Second half of BL/BLX
        readReg(e, 5, 14, pc);
        e.addi(5, 5, (op & 0x7FF) << 1);
        e.loadConst(6, pc - 1);
        writeReg(e, 6, 14);
        if (op & BIT(12)) { // BL
            emitJump(e, 5, false);
        }
        else { // BLX to ARM
            e.lwz(6, offCpsr, R_ARM);
            e.rlwinm(6, 6, 0, 27, 25);
            e.stw(6, offCpsr, R_ARM);
            e.rlwinm(8, 5, 0, 0, 29);
            e.addi(8, 8, 4);
            e.stw(8, offPc, R_ARM);
        }
        e.li(3, 3);
    }
    else { // BX/BLX with a register
        readReg(e, 5, (op >> 3) & 0xF, pc);
        if (op & BIT(7)) {
            e.loadConst(6, pc - 1);
            writeReg(e, 6, 14);
        }
        emitJump(e, 5, true);
        e.li(3, 3);
    }
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
    if ((op & 0x0F100010) == 0x0E100010 || cacheControl(op)) return true; // MRC and cache control
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

void Jit::findLiveFlags(JitBlock *block) {
    // Work out which flags are read after each instruction of a block before something sets them again, going
    // backwards from what follows it. Every flag counts as read at the end of the block and before any instruction
    // that isn't in a run, which covers everywhere code can stop or leave the block, so the state there is always
    // exact. Only instructions in a run that's followed by more of it can have flags nothing reads after them
    size_t count = block->instrs.size();
    liveFlags.assign(count, FLAG_ALL);
    int live = FLAG_ALL;
    for (size_t i = count; i-- > 0;) {
        if (!batched(block, i)) {
            live = FLAG_ALL;
            continue;
        }
        liveFlags[i] = live;
        uint32_t op = armOpcode(block, i);
        int reads, writes, always;
        flagUse(op, nativeKind(op, arm7), reads, writes, always);
        live = (live & ~always) | reads;

        // Fetching a line into the code cache can stop before the instruction, so the flags have to be exact there
        if (fetches(block, i)) live = FLAG_ALL;
    }
}

bool Jit::fetches(JitBlock *block) {
    // Check if a block's code goes through the ARM9's code cache, which is only counted for main RAM in DSi software,
    // which is what the code is generated for when it counts half cycles
    return !arm7 && halfCycles && (block->address >> 24) == 0x02;
}

bool Jit::fetches(JitBlock *block, size_t i) {
    // Check if an instruction is where its line of code has to be in the code cache: the first of the block, since
    // jumps can come from anywhere, and the first of each line after that
    return fetches(block) && (i == 0 || !((block->address + i * (block->thumb ? 2 : 4)) & 0x1F));
}

void Jit::emitFetch(PpcEmitter &e, uint32_t address) {
    // Check the byte that says the code cache holds an instruction's line, and fetch the line if not; the program
    // counter register has to be at the instruction
    uint32_t held = addrCodeLines + ((address & 0xFFFFFF) >> 5);
    e.lis(4, (held + 0x8000) >> 16);
    e.lbz(4, int16_t(held), 4);
    e.cmpwi(4, 0);
    uint32_t *there = e.here();
    e.bne(nullptr);
    e.call(codeFetch[postHalf()][thumbCode]);
    e.patchBc(there);
}

bool Jit::batched(JitBlock *block, size_t i) {
    // Check if an instruction is in a run whose cycles are checked together, which is only done with fast timing
    // Runs are native ALU instructions and loads; stores always check, since they can halt the CPU, change code
    // or schedule something that has to happen straight away, and jumps end runs since they leave the block
    if (!codeFast || testing || i >= block->instrs.size()) return false;
    uint32_t op = armOpcode(block, i);
    int kind = nativeKind(op, arm7);
    if (kind == NK_ALU || kind == NK_MULTIPLY || kind == NK_MISC) return true;
    return (kind == NK_TRANSFER || kind == NK_HALF || kind == NK_BLOCK) && (op & BIT(20));
}

bool Jit::endsRun(JitBlock *block, size_t i) {
    // Check if an instruction outside of a run can do the check for the run before it, instead of checking twice in a row
    // This is left out for the interpreter's handlers, which are the ones that use the time more than in passing
    if (i >= block->instrs.size() || batched(block, i)) return false;
    if (block->thumb && thumbBranch(block->instrs[i].opcode, arm7)) return true;
    return nativeKind(armOpcode(block, i), arm7) != NK_NONE;
}

void Jit::stepPc(PpcEmitter &e) {
    // Bring the program counter register up to the current instruction, which is only needed before something uses it
    if (pcBehind) e.addi(R_PC, R_PC, pcBehind);
    pcBehind = 0;
}

void Jit::emitInstr(PpcEmitter &e, JitBlock *block, size_t i, uint32_t pc, bool &ok) {
    // Generate one instruction; the program counter register only holds its value after stepping
    // Instructions in a run don't use it until the check at the end, so they leave stepping until then
    uint32_t raw = block->instrs[i].opcode, op = armOpcode(block, i);
    flagsLive = liveFlags[i];
    if (!batched(block, i)) stepPc(e);

    // Instructions with a condition in the middle of a run add the cycle a failed condition takes after the instruction,
    // which adds the rest of its cycles, so both ways through can share the end
    if (batched(block, i) && (op >> 28) != 0xE && (batched(block, i + 1) || endsRun(block, i + 1))) {
        e.call(codeCond[op >> 28]);
        uint32_t *skip = e.here();
        e.bne(nullptr);
        emitNative(e, (op & 0x0FFFFFFF) | 0xE0000000, pc);
        uint32_t *last = e.here() - 1;
        if ((*last & 0xFFFF0000) == 0x38600000 && (*last & 0xFFFF) > 0) {
            *last = (14 << 26) | (R_BATCH << 21) | (R_BATCH << 16) | ((*last & 0xFFFF) - 1);
            if ((*last & 0xFFFF) == 0) e.ptr--;
            e.patchBc(skip);
            e.addi(R_BATCH, R_BATCH, 1);
            return;
        }

        // Otherwise, its cycles aren't known until it runs, so the ways through have to join after adding them
        e.add(R_BATCH, R_BATCH, 3);
        uint32_t *done = e.here();
        e.b(nullptr);
        e.patchBc(skip);
        e.addi(R_BATCH, R_BATCH, 1);
        e.patchB(done);
        return;
    }

    // Conditional branches go their own way straight after the condition, so neither path has to check where it went
    bool thumbCond = block->thumb && (raw & 0xF000) == 0xD000 && thumbBranch(raw, arm7);
    if (thumbCond || (!block->thumb && nativeKind(op, arm7) == NK_BRANCH && (op >> 28) != 0xE)) {
        bool run = i > 0 && batched(block, i - 1);
        uint32_t target = thumbCond ? (pc + ((int32_t)(int8_t)raw << 1)) : ((pc + ((int32_t)(op << 8) >> 6)) & ~0x3);
        e.call(codeCond[thumbCond ? ((raw >> 8) & 0xF) : (op >> 28)]);
        uint32_t *skip = e.here();
        e.bne(nullptr);

        // Branch, with cycles that include any run before this
        if (!thumbCond && (op & BIT(24))) {
            e.loadConst(5, pc - 4);
            writeReg(e, 5, 14);
        }
        e.loadConst(5, target + (thumbCond ? 2 : 4));
        e.stw(5, offPc, R_ARM);
        e.addi(3, R_BATCH, 3);
        if (run) e.li(R_BATCH, 0);
        e.call(codePost[postHalf()][0][0]);
        block->links.push_back({ e.here(), target | thumbCond, nullptr });
        e.b(codeDispatch);

        // Continue to the next instruction
        e.patchBc(skip);
        e.addi(3, R_BATCH, 1);
        if (run) e.li(R_BATCH, 0);
        e.call(codePost[postHalf()][0][1]);
        if (i + 1 == block->instrs.size()) e.stw(R_PC, offPc, R_ARM);
        return;
    }

    // THUMB reads the program counter as word aligned for its offsets, and has its own branches
    bool wordPc = block->thumb && ((raw >> 11) == 0x09 || (raw >> 11) == 0x14);
    bool branch = block->thumb && thumbBranch(raw, arm7);
    if (branch) emitThumbBranch(e, raw, pc);
    bool native = branch || emitNative(e, op, wordPc ? (pc & ~0x3) : pc);
    bool memoryPc = !native || (branch ? (raw & 0xF800) != 0xF000 : nativeJumps(nativeKind(op, arm7)));

    if (!native) {
        // Call the interpreter handler, with the program counter stored for it to read
        void *handler = block->thumb ? memberAddress(block->instrs[i].thumb) : memberAddress(block->instrs[i].arm);
        void *reserved = memberAddress(&Interpreter::handleReserved);
        if (!handler || !reserved) {
            ok = false;
            return;
        }
        e.stw(R_PC, offPc, R_ARM);
        uint32_t cond = raw >> 28;
        uint32_t *skip = nullptr;
        if (!block->thumb && cond < 0xE) {
            e.call(codeCond[cond]);
            skip = e.here();
            e.bne(nullptr);
        }
        e.stw(R_CYCLES, 0, R_GLOBAL);
        e.stw(R_CYCLES, offCycles, R_ARM);
        e.mr(3, R_ARM);
        e.loadConst(4, raw);
        e.call((!block->thumb && cond == 0xF) ? reserved : handler);
        e.call(codeRefresh);
        if (skip) {
            uint32_t *done = e.here();
            e.b(nullptr);
            e.patchBc(skip);
            e.li(3, 1);
            e.patchB(done);
        }
    }

    // With fast timing, runs of native ALU instructions and loads add up their cycles and only check whether to stop
    // after the last one, or after the native instruction that follows it, so events can happen a few instructions late
    // and loads and stores can see the time a little behind; jumps into the middle of a run still count the cycles of
    // what they run
    bool inRun = batched(block, i);
    if (inRun && (batched(block, i + 1) || endsRun(block, i + 1))) {
        // Instructions without a condition end by setting their cycles, so add them directly in place of that
        uint32_t *last = e.here() - 1;
        if ((op >> 28) == 0xE && (*last & 0xFFFF0000) == 0x38600000)
            *last = (14 << 26) | (R_BATCH << 21) | (R_BATCH << 16) | (*last & 0xFFFF);
        else
            e.add(R_BATCH, R_BATCH, 3);
        return;
    }
    if (inRun || (i > 0 && batched(block, i - 1) && endsRun(block, i))) {
        e.add(3, 3, R_BATCH);
        e.li(R_BATCH, 0);
    }

    // Count cycles and check whether to stop; the program counter is stored first if it's only in the register
    // Then check for jumps, which leave the program counter in memory different from the register
    // Branches to known addresses jump through a site that's linked to the target block once it exists
    stepPc(e);
    e.call(codePost[postHalf()][!isPure(raw, block->thumb)][!memoryPc]);
    if (block->thumb && (raw & 0xF800) == 0xE000) {
        // Unconditional branches jump straight to their target like ARM ones
        block->links.push_back({ e.here(), (pc + ((int32_t)(raw << 21) >> 20)) | 1, nullptr });
        e.b(codeDispatch);
    }
    else if (!block->thumb && native && nativeKind(op, arm7) == NK_BRANCH) {
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
    else if (!block->thumb && native && nativeKind(op, arm7) == NK_BLX) {
        block->links.push_back({ e.here(), blxTarget(op, pc) | 1, nullptr });
        e.b(codeDispatch);
    }
    else if (i + 1 < block->instrs.size() && mayJump(raw, block->thumb)) {
        e.lwz(4, offPc, R_ARM);
        e.cmplw(4, R_PC);

        // A return can also go to THUMB at the address that's next here
        uint32_t *differ = nullptr;
        if (!block->thumb && native && nativeKind(op, arm7) == NK_ALU_PC && (op & BIT(20))) {
            differ = e.here();
            e.bne(nullptr);
            e.lwz(4, offCpsr, R_ARM);
            e.andi_(4, 4, BIT(5));
        }
        uint32_t *same = e.here();
        e.beq(nullptr);
        if (differ) e.patchBc(differ);
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
    JS_START(writableStart);
    if (OSGetCoreId() != codeCore || !OSSwitchSecCodeGenMode(CODEGEN_RW_)) return nullptr;
    JS_ADD(JS_SWITCH_RW, writableStart);

    // Set the program counter register at the start, then step it as instructions need it
    // Record where each instruction starts after stepping, so other blocks can jump into the middle
    PpcEmitter e(codePtr);
    uint32_t step = block->thumb ? 2 : 4;
    uint32_t pc = block->address + step * 2;
    e.loadConst(R_PC, pc);
    block->entries.clear();
    block->entriesBehind.clear();
    pcBehind = 0;
    thumbCode = block->thumb;
    fullSpeed = arm7 && codeFullSpeed && !Interpreter::mainRam(block->address);
    block->links.clear();
    bool ok = true;
    JS_START(emitStart);
    findLiveFlags(block);
    for (size_t i = 0; i < block->instrs.size() && ok; i++, pc += step) {
        if (i > 0) pcBehind += step;
        block->entries.push_back((uint8_t*)e.here() - codePtr);
        block->entriesBehind.push_back(pcBehind);
        if (fetches(block, i)) {
            stepPc(e);
            emitFetch(e, pc - step * 2);
        }
        emitInstr(e, block, i, pc, ok);
    }

    // Continue to whatever block comes next
    e.b(codeDispatch);
    flagsLive = FLAG_ALL;
    JS_ADD(JS_EMIT, emitStart);

    // Link jumps between this block and others while the area is writable
    uint8_t *code = codePtr;
    size_t size = (uint8_t*)e.here() - code;
    std::vector<uint32_t*> patched;
    if (ok) {
        block->code = code;
        JS_START(linkStart);
        linkBlock(block, patched);
        JS_ADD(JS_LINK, linkStart);
#ifdef JIT_SPLIT
        jitSplitBytes += size;
#endif
    }
    else {
        block->links.clear();
    }
    finishCode(code, size, patched);
    if (!ok) return nullptr;
    codePtr += (size + 15) & ~15;
    return code;
}

// The most host code an entry point into the middle of a block takes, which is with a check of the code cache
static const size_t ENTRY_SIZE = 48;

void *Jit::generateEntry(JitBlock *host, size_t index) {
    // Generate a small entry point into the middle of another block's code, instead of compiling it again
    if (!codeArea || !host->code || index >= host->entries.size() || codePtr + 64 > codeEnd) return nullptr;
    JS_START(writableStart);
    if (OSGetCoreId() != codeCore || !OSSwitchSecCodeGenMode(CODEGEN_RW_)) return nullptr;
    JS_ADD(JS_SWITCH_RW, writableStart);
    PpcEmitter e(codePtr);
    if (fetches(host) && !fetches(host, index)) {
        // Nothing there checks that the code cache holds the instruction's line, so that's done here, with the program
        // counter register at the instruction until it's done
        uint32_t step = host->thumb ? 2 : 4;
        thumbCode = host->thumb;
        e.loadConst(R_PC, host->address + (index + 2) * step);
        emitFetch(e, host->address + index * step);
        if (host->entriesBehind[index]) e.addi(R_PC, R_PC, -host->entriesBehind[index]);
    }
    else {
        e.loadConst(R_PC, host->address + (index + 2) * (host->thumb ? 2 : 4) - host->entriesBehind[index]);
    }
    e.b((uint8_t*)host->code + host->entries[index]);
    uint8_t *code = codePtr;
    size_t size = (uint8_t*)e.here() - code;
    codePtr += (size + 15) & ~15;
    return code;
}

void Jit::linkEntry(JitBlock *block) {
    // Link waiting jumps to a block that's an entry into another block's code
    if (!block->code || pendingLinks.find(block->address | block->thumb) == pendingLinks.end()) {
        finishCode((uint8_t*)block->code, ENTRY_SIZE, std::vector<uint32_t*>());
        return;
    }
    std::vector<uint32_t*> patched;
    linkBlock(block, patched);
    finishCode((uint8_t*)block->code, ENTRY_SIZE, patched);
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
