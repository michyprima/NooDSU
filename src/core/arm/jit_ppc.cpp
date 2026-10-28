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

    // X-form instructions
    void x(int rt, int ra, int rb, int xo) { emit((31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1)); }
    void add(int rt, int ra, int rb) { x(rt, ra, rb, 266); }
    void or_(int ra, int rs, int rb) { x(rs, ra, rb, 444); }
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

// Only one JIT can own the codegen area at a time
static Jit *codeOwner = nullptr;

void Jit::initCodegen() {
    if (codeArea) return;
#ifdef __WIIU__
    // Get the codegen area, which can only be used from one CPU core
    void *area = nullptr;
    uint32_t size = 0;
    OSGetCodegenVirtAddrRange(&area, &size);
    if (!area || size < 0x100000 || codeOwner || OSGetCoreId() != (OSGetCodegenCore() & 3)) return;

    // Make sure the event vector can be read directly, as generated code does
    if (*(SchedEvent**)&core->events != core->events.data()) return;
    if (!OSSwitchSecCodeGenMode(CODEGEN_RW_)) return;
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
    OSSwitchSecCodeGenMode(CODEGEN_R_X);
    ICInvalidateRange(start, size);
#endif
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

void *Jit::generate(JitBlock *block) {
    if (!codeArea) return nullptr;

    // Make sure there's room for the block, starting over if the area is full
    size_t maxSize = block->instrs.size() * 36 * 4 + 16;
    if (codePtr + maxSize > codeEnd) return nullptr;
#ifdef __WIIU__
    if (!OSSwitchSecCodeGenMode(CODEGEN_RW_)) return nullptr;
#endif

    PpcEmitter e(codePtr);
    void *reserved = memberAddress(&Interpreter::handleReserved);
    uint32_t step = block->thumb ? 2 : 4;
    uint32_t pc = block->address + step * 2;
    size_t count = block->instrs.size();
    bool ok = reserved;

    for (size_t i = 0; i < count && ok; i++, pc += step) {
        // Set the program counter as if the instruction was fetched
        uint32_t op = block->instrs[i].opcode;
        e.loadConst(0, pc);
        e.stw(0, offPc, R_ARM);

        void *handler = block->thumb ? memberAddress(block->instrs[i].thumb) : memberAddress(block->instrs[i].arm);
        if (!handler) {
            ok = false;
            break;
        }

        if (block->thumb || (op >> 28) == 0xE) {
            // Call the handler directly when there's no condition to check
            e.mr(3, R_ARM);
            e.loadConst(4, op);
            e.call(handler);
        }
        else {
            // Look up the condition result, like the interpreter does
            e.lwz(3, offCpsr, R_ARM);
            e.rlwinm(3, 3, 4, 28, 31);
            e.loadConst(4, (uint32_t)(uintptr_t)&Interpreter::condition[(op >> 24) & 0xF0]);
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
        if (i + 1 < count && mayJump(op, block->thumb)) {
            e.lwz(4, offPc, R_ARM);
            e.loadConst(5, pc);
            e.cmpw(4, 5);
            uint32_t *same = e.here();
            e.beq(nullptr);
            e.b(codeDispatch);
            e.patchBc(same);
        }
    }

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
}

void Jit::freeCodegen() {}

void Jit::finishCode(uint8_t *start, size_t size) {}

void *Jit::generate(JitBlock *block) {
    return nullptr;
}

#endif
