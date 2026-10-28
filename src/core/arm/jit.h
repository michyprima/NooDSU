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

#pragma once

#include <cstdint>
#include <vector>

class Core;
class Interpreter;
class PpcEmitter;

// Diagnostic logging for builds made with JIT_LOG, written to the SD card a line at a time
#ifdef JIT_LOG
void jitLog(const char *format, ...);
#define JLOG(...) jitLog(__VA_ARGS__)
#else
#define JLOG(...) do {} while (0)
#endif

// A decoded instruction, with the interpreter handler that executes it
struct JitInstr {
    uint32_t opcode;
    union {
        int (Interpreter::*arm)(uint32_t);
        int (Interpreter::*thumb)(uint16_t);
    };
};

// A run of consecutive ARM9 instructions from one 4KB page of host memory
struct JitBlock {
    uint32_t address; // Guest address of the first instruction
    bool thumb;
    uint8_t *start, *end; // Host memory covered by the block
    int index; // Position in the block lookup table
    std::vector<JitInstr> instrs;
    void *code = nullptr; // Generated host code, or null to use the portable executor
    std::vector<uint32_t> entries; // Offsets in the code where each instruction starts
};

// A direct-mapped cache of recently run blocks by guest address, for jumping between blocks quickly
// Entries are 16 bytes on 32-bit hosts, so generated code can index them with a shift
struct alignas(16) JitCacheEntry {
    uint32_t key; // Guest address, with bit 0 set for THUMB
    void *code;
    JitBlock *block;
};

// Per-page list of compiled blocks, with a bitmap of the halfwords they cover
struct JitPage {
    std::vector<JitBlock*> blocks;
    uint32_t covered[0x1000 / 2 / 32] = {};
};

// Runs ARM9 code in compiled blocks, matching the interpreter instruction for instruction
// Each instruction still runs through its interpreter handler; blocks remove the per-instruction
// fetch and dispatch overhead, and are the base for generating native code later
class Jit {
public:
    friend bool jitTestEmit(Jit &jit, uint8_t *buf, uint32_t op, uint32_t pc, uint32_t *size, uint32_t *start);
    bool enabled = false;

    Jit(Core *core);
    ~Jit();

    void enable(bool value);
    void reset();
    bool run(const uint32_t *limit);

    bool isNative() { return codeArea != nullptr; }
    bool hasCode() { return codePages != 0; }
    bool isCodePage(uint8_t *host);
    bool writeCode(uint8_t *host, int size);
    void mapChanged(uint32_t start);

private:
    Core *core;

    // Blocks are looked up by the host address of their first halfword
    // Main RAM, ITCM and the ARM9 BIOS are the only memory that code is compiled from
    static const int RAM_SIZE = 0x400000, ITCM_SIZE = 0x8000, BIOS_SIZE = 0x10000;
    static const int RAM_PAGES = RAM_SIZE >> 12, ITCM_PAGES = ITCM_SIZE >> 12;
    std::vector<JitBlock*> table;
    JitPage pages[RAM_PAGES + ITCM_PAGES];
    int codePages = 0;

    // The entry before the cache holds a flag that's set when code changes, so it's next to the cache
    // Running code stops at the next instruction when the flag is set, in case its own code changed
    static const int CACHE_SIZE = 0x1000;
    JitCacheEntry cacheData[CACHE_SIZE + 1] = {};
    JitCacheEntry *cache = &cacheData[1];
    uint32_t &codeChanged = cacheData[0].key;

    // Blocks can be invalidated while running, so they're deleted later
    std::vector<JitBlock*> graveyard;
    bool running = false;

    // Generated code, starting with shared entry, exit, dispatch and per-instruction routines
    uint8_t *codeArea = nullptr, *codeStart = nullptr, *codePtr = nullptr, *codeEnd = nullptr;
    void *codeEnter = nullptr, *codeExit = nullptr, *codeDispatch = nullptr;
    void *codePost[2][2] = {}, *codeSlowPost[2] = {}; // Indexed by impure, then by storing the program counter
    void *codeCond[14] = {}, *codeFlags[5] = {}, *codeMemory[4] = {};
    int offPc = 0, offCpsr = 0, offCycles = 0, offEventCycles = 0, offUsr = 0, offRegs = 0;
    uint32_t codeCore = 0;
    bool codeFailed = false;

    // Addresses that native instructions use, which tests can point elsewhere
    uint32_t addrCondition = 0, addrMemory = 0, addrReadMap = 0, addrWriteMap = 0;
    uint32_t addrLoad32 = 0, addrLoad8 = 0, addrStore32 = 0, addrStore8 = 0;

    int tableIndex(uint8_t *host);
    int pageIndex(uint8_t *host);
    JitBlock *compile(uint32_t address, bool thumb, uint8_t *host, int index);
    void retire(JitBlock *block);
    JitBlock *lookup(uint32_t address, bool thumb);
    void protect(int page);
    void execute(JitBlock *block, const uint32_t *limit);
    int interleave(const uint32_t *limit);
    static int interleaveCall(Jit *jit, const uint32_t *limit);

    static bool endsBlock(uint32_t opcode, bool thumb);

    void initCodegen();
    void freeCodegen();
    bool codeFull();
    void finishCode(uint8_t *start, size_t size);
    void *generate(JitBlock *block);
    void *generateEntry(JitBlock *host, size_t index);
    void emitStubs(PpcEmitter &e);
    bool emitNative(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitInstr(PpcEmitter &e, JitBlock *block, size_t i, uint32_t pc, bool &ok);
    void emitAlu(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitBranch(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitTransfer(PpcEmitter &e, uint32_t op, uint32_t pc);
    void readReg(PpcEmitter &e, int rt, int reg, uint32_t pc);
    void writeReg(PpcEmitter &e, int rs, int reg);
    void clearCache();
};
