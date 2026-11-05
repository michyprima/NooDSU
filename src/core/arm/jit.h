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

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

// How long each step of compiling a block takes, for builds made with JIT_SPLIT
#ifdef JIT_SPLIT
#include <coreinit/time.h>
enum JitSplitSlot {
    JS_TOTAL, // Compiling a block, including everything below and decoding it
    JS_NEW, // Allocating the block
    JS_REUSE, // Looking for a block to reuse code from, and generating an entry point into it
    JS_DECODE, // Decoding instructions
    JS_RETIRE, // Replacing a block already at this position
    JS_TRACK, // Tracking the block in its page, other than unmapping it
    JS_GENERATE, // Generating and finishing host code, including the steps below
    JS_PROTECT, // Unmapping pages that got their first code from the write maps
    JS_SWITCH_RW, // Making the codegen area writable
    JS_EMIT, // Generating host code
    JS_LINK, // Linking the block with others
    JS_FLUSH, // Flushing the code from the data cache
    JS_SWITCH_RX, // Making the codegen area executable again
    JS_INVALIDATE, // Invalidating the instruction cache
    JS_COUNT
};
extern uint64_t jitSplit[JS_COUNT];
extern uint32_t jitSplitBlocks, jitSplitBytes;
#define JS_START(name) uint32_t name = OSGetSystemTick()
#define JS_ADD(slot, name) jitSplit[slot] += uint32_t(OSGetSystemTick() - name)
#else
#define JS_START(name)
#define JS_ADD(slot, name)
#endif

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

struct JitBlock;

// A jump in generated code to a block at a known address, which goes through the dispatcher until it's
// patched to jump to the block's code directly
struct JitLink {
    uint32_t *site;
    uint32_t key; // Target address, with bit 0 set for THUMB
    JitBlock *target;
};

// A run of consecutive instructions from one 4KB page of host memory
struct JitBlock {
    uint32_t address; // Guest address of the first instruction
    bool thumb;
    uint8_t *start, *end; // Host memory covered by the block
    int index; // Position in the block lookup table
    std::vector<JitInstr> instrs;
    void *code = nullptr; // Generated host code, or null to use the portable executor
    std::vector<uint32_t> entries; // Offsets in the code where each instruction starts
    std::vector<uint8_t> entriesBehind; // How far the program counter register is behind at each of those
    std::vector<JitLink> links; // Jumps from this block's code to other blocks
    std::vector<std::pair<JitBlock*, uint32_t*>> incoming; // Linked jumps to this block, and their blocks

    static void *operator new(size_t size);
    static void operator delete(void *block);
};

// A direct-mapped cache of recently run blocks by guest address, for jumping between blocks quickly
// Entries are 16 bytes on 32-bit hosts, so generated code can index them with a shift
struct alignas(16) JitCacheEntry {
    uint32_t key; // Guest address, with bit 0 set for THUMB
    void *code;
    JitBlock *block;
};

// Per-page list of compiled blocks, with how many blocks cover each halfword, so retiring a block only has to
// update its own range; counts stop at 255, and a page that reached that is recounted when blocks are retired
struct JitPage {
    std::vector<JitBlock*> blocks;
    uint8_t covered[0x1000 / 2] = {};
    bool saturated = false;
    uint16_t overwrites = 0; // Writes over its code since the page last got code
    uint8_t cleared = 0; // Times its code was dropped for being overwritten, which makes that take more writes
};

// Host memory that code is compiled from, and whether code there can be overwritten
struct JitRegion {
    uint8_t *base;
    uint32_t size;
    bool writable;
};

// Runs one CPU's code in compiled blocks, matching the interpreter instruction for instruction
// Blocks remove the per-instruction fetch and dispatch overhead, and generate native code where possible
class Jit {
public:
    friend bool jitTestEmit(Jit &jit, uint8_t *buf, uint32_t op, uint32_t pc, uint32_t *size, uint32_t *start);
    bool enabled = false;
    static bool testing; // Set by the self-test, so compiled code always checks every instruction like the interpreter

    Jit(Core *core, bool arm7);
    ~Jit();

    void enable(bool value);
    void reset();
    bool run(const uint32_t *limit);

    bool isNative() { return codeArea != nullptr; }
    bool hasCode() { return codePages != 0; }
    bool isCodePage(uint8_t *host);
    bool lineHasCode(uint8_t *host);
    bool writeCode(uint8_t *host, int size);
    void mapChanged(uint32_t start);

private:
    Core *core;
    bool arm7;

    // Blocks are looked up by the host address of their first halfword
    // The ARM9 compiles code from main RAM, ITCM and its BIOS; the ARM7 from main RAM, both WRAMs and its BIOS
    static const int RAM_SIZE = 0x400000, RAM_PAGES = RAM_SIZE >> 12, MAX_PAGES = RAM_PAGES + 24;
    JitRegion regions[4] = {};
    int regionCount = 0;
    uint32_t tableSize = 0;
    std::vector<JitBlock*> table;
    JitPage pages[MAX_PAGES];
    int codePages = 0;

    // The entry before the cache holds a flag that's set when code changes, so it's next to the cache
    // Running code stops at the next instruction when the flag is set, in case its own code changed
    static const int CACHE_SIZE = 0x1000;
    JitCacheEntry cacheData[CACHE_SIZE + 1] = {};
    JitCacheEntry *cache = &cacheData[1];
    uint32_t &codeChanged = cacheData[0].key;

    // Links waiting for their target to be compiled, and jumps to put back to the dispatcher before running again
    std::unordered_multimap<uint32_t, std::pair<JitBlock*, uint32_t*>> pendingLinks;
    std::vector<uint32_t*> unlinks;

    // Blocks can be invalidated while running, so they're deleted later
    std::vector<JitBlock*> graveyard;
    std::vector<JitBlock*> hitBlocks; // Blocks a write overlaps, kept to avoid allocating for every write
    bool running = false;

    // Generated code, starting with shared entry, exit, dispatch and per-instruction routines
    uint8_t *codeArea = nullptr, *codeStart = nullptr, *codePtr = nullptr, *codeEnd = nullptr;
    void *codeEnter = nullptr, *codeExit = nullptr, *codeDispatch = nullptr;
    void *codeRefresh = nullptr;
    uint32_t pcBehind = 0; // How far the program counter register will be behind at this point in a block being generated
    bool thumbCode = false; // Whether the block being generated is THUMB
    void *codePost[2][2] = {}, *codeSlowPost[2] = {}; // Indexed by impure, then by storing the program counter
    void *codeCond[14] = {}, *codeFlags[5] = {}, *codeMemory[6] = {}, *codeShift[4] = {}, *codeShiftCarry[4] = {}, *codeMultiplyCycles = nullptr, *codeOverflowQ = nullptr, *codeMsr[2] = {};
    int offPc = 0, offCpsr = 0, offSpsr = 0, offCycles = 0, offEventCycles = 0, offUsr = 0, offRegs = 0, offIe = 0, offIrf = 0;
    uint32_t codeCore = 0;
    bool codeFailed = false;
    bool codeFast = false; // Whether code was generated for fast timing, which checks cycles less often

    // Addresses that native instructions use, which tests can point elsewhere
    uint32_t addrCondition = 0, addrMemory = 0, addrReadMap = 0, addrWriteMap = 0;
    uint32_t addrDtcmReg = 0, addrWriteCpsr = 0;
    uint32_t addrLoad32 = 0, addrLoad8 = 0, addrStore32 = 0, addrStore8 = 0, addrLoad16 = 0, addrStore16 = 0;

    uint8_t **codeMap();
    int tableIndex(uint8_t *host);
    int pageIndex(uint8_t *host);
    uint8_t *pageHost(int page);
    JitBlock *compile(uint32_t address, bool thumb, uint8_t *host, int index);
    void retire(JitBlock *block);
    JitBlock *lookup(uint32_t address, bool thumb);
    void unlinkIncoming(JitBlock *block);
    void removeLinks(JitBlock *block);
    void linkBlock(JitBlock *block, std::vector<uint32_t*> &patched);
    void applyUnlinks();
    void patchJump(uint32_t *site, void *target);
    void protect(int page);
    void execute(JitBlock *block, const uint32_t *limit);
    int interleave(const uint32_t *limit);
    static int interleaveCall(Jit *jit, const uint32_t *limit);
    static void writeCpsrCall(Interpreter *arm, uint32_t opcode, uint32_t value);

    static bool endsBlock(uint32_t opcode, bool thumb);

    void initCodegen();
    void freeCodegen();
    bool codeFull();
    void finishCode(uint8_t *start, size_t size, const std::vector<uint32_t*> &patched);
    void *generate(JitBlock *block);
    void *generateEntry(JitBlock *host, size_t index);
    void linkEntry(JitBlock *block);
    void emitStubs(PpcEmitter &e);
    bool emitNative(PpcEmitter &e, uint32_t op, uint32_t pc);
    uint32_t armOpcode(JitBlock *block, size_t i);
    bool batched(JitBlock *block, size_t i);
    bool endsRun(JitBlock *block, size_t i);
    void stepPc(PpcEmitter &e);
    void emitInstr(PpcEmitter &e, JitBlock *block, size_t i, uint32_t pc, bool &ok);
    void emitAlu(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitBranch(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitBlx(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitThumbBranch(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitTransfer(PpcEmitter &e, uint32_t op, uint32_t pc);
    bool emitShift(PpcEmitter &e, uint32_t op, uint32_t pc, bool setCarry);
    void emitHalf(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitBlock(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitSwap(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitMultiply(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitDsp(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitMisc(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitCoprocessor(PpcEmitter &e, uint32_t op);
    void emitMsr(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitBranchReg(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitJump(PpcEmitter &e, int rs, bool interwork);
    void readReg(PpcEmitter &e, int rt, int reg, uint32_t pc);
    void writeReg(PpcEmitter &e, int rs, int reg);
    void clearCache();
};
