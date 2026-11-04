// Tests the JIT's generated PowerPC against the interpreter, on a Mac, by running it in the Unicorn emulator
// The real compile(), generate(), linking and retiring are used, with C++ calls from generated code caught by the test
// See README.md for what this does and doesn't cover

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

#define private public
#include "core.h"
#include "settings.h"
#undef private

#include <unicorn/unicorn.h>

extern void (*rigSetup)(Jit &jit);
uint32_t rigArm(uint32_t op);
int rigKind(uint32_t op, bool thumb, bool arm7);
bool rigMayJump(uint32_t op, bool thumb);
bool rigKindJumps(int kind);

enum { NK_NONE, NK_ALU, NK_BRANCH, NK_TRANSFER, NK_BRANCH_REG, NK_HALF, NK_BLOCK, NK_BLOCK_PC, NK_SWAP, NK_MULTIPLY, NK_MISC,
    NK_THUMB_BRANCH = 100 };

// Addresses in the emulator
static const uint32_t ARM_STATE = 0x10000, CACHE_FLAG = 0x50000, CACHE = 0x50010, EVENTS = 0x70000, LIMIT = 0x71000,
    GLOBAL = 0x72000, STACK = 0x7F000, END = 0x80000, COND_TABLE = 0x200000, READ_MAP = 0x400000, WRITE_MAP = 0x800000,
    FAKE = 0x3F0000, FAKE_MEMORY = 0x3F0800, FAKE_HANDLER = 0x3F1000, FAKE_INTERLEAVE = 0x3F2000, CODE = 0x10000000;
static const uint32_t OFF_REGS = 0x40, OFF_CPSR = 0x80, OFF_CYCLES = 0x84, OFF_SPSR = 0x88, OFF_BANK = 0xA0, OFF_SPSR_VALUE = 0xC0;

// Guest memory: data, then code, which is real main RAM for the interpreter and a copy in the emulator
static const uint32_t WINDOW = 0x02100000, WINDOW_SIZE = 0x40000, DATA_START = WINDOW + 0x2000, DATA_END = WINDOW + 0x1E000,
    CODE_START = WINDOW + 0x20000;
static const uint32_t START_CYCLES = 0x1000, FAKE_HANDLER_CYCLES = 2;

enum { MEMORY_MAPPED, MEMORY_SLOW, MEMORY_ODD_PAGES };
enum { STOP_NEVER, STOP_ALWAYS, STOP_AT, STOP_SCHEDULED, STOP_DEADLINE };

static Core *core;
static uc_engine *uc;
static uint8_t *ram;
static uint8_t seed[WINDOW_SIZE];
static uint8_t *codeBase;
static int shard, shards;
static std::map<std::string, uint64_t> counts, fails;
static std::map<std::string, std::set<std::string>> seen;

struct State {
    uint32_t usr[16], bank[7], spsr, cpsr;
    int mode; // 0 is system, 1 is IRQ, with its own R13 and R14, 2 is FIQ, with its own R8-R14
};

static bool banked(int mode, int reg) { return (mode == 1 && reg >= 13 && reg <= 14) || (mode == 2 && reg >= 8 && reg <= 14); }
static uint32_t &visible(State &s, int reg) { return banked(s.mode, reg) ? s.bank[reg - 8] : s.usr[reg]; }

struct Compiled { JitBlock *block; };

struct Snapshot { State state; uint32_t cycles; };

// One expected call to a fake interpreter handler
struct FakeCall { uint32_t op, pc; bool jump; uint32_t target; };

// What the current case expects from C++ calls in generated code
static struct {
    int cpu, stopMode, stopAfter, stops, memoryMode;
    bool setFlagOnSlowStore, failed, scheduled;
    std::vector<Snapshot> trace;
    size_t traceAt;
    std::vector<FakeCall> fakes;
    size_t fakeAt;
    std::string name, description;
    uint32_t inCpsr;
} now;

static uint32_t swap32(uint32_t v) { return __builtin_bswap32(v); }
static uint32_t readBe(uint32_t address) { uint32_t v; uc_mem_read(uc, address, &v, 4); return swap32(v); }
static void writeBe(uint32_t address, uint32_t value) { value = swap32(value); uc_mem_write(uc, address, &value, 4); }
static uint32_t reg(int r) { uint32_t v = 0; uc_reg_read(uc, r, &v); return v; }
static void setReg(int r, uint32_t value) { uc_reg_write(uc, r, &value); }

static void fail(const char *format, ...) {
    now.failed = true;
    fails[now.name]++;
    if (fails[now.name] > 6) return;
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    printf("FAIL\t%s\t%s in-cpsr %08X: %s\n", now.name.c_str(), now.description.c_str(), now.inCpsr, message);
}

static void count(const std::string &name, uint64_t n = 1) { counts[name] += n; }

static void clobberVolatile(bool keepR3) {
    // The ABI lets C++ change these, so generated code can't be relying on them
    for (int i = keepR3 ? 4 : 3; i <= 12; i++)
        setReg(UC_PPC_REG_0 + i, 0xDEAD0000 + i);
    setReg(UC_PPC_REG_0, 0xDEAD0000);
    setReg(UC_PPC_REG_CTR, 0xDEAD1000);
    setReg(UC_PPC_REG_CR, 0x20000000);
    setReg(UC_PPC_REG_XER, 0);
}

static void readState(State &s) {
    uint32_t words[64];
    uc_mem_read(uc, ARM_STATE, words, sizeof(words));
    for (int i = 0; i < 16; i++) s.usr[i] = swap32(words[i]);
    for (int i = 0; i < 7; i++) s.bank[i] = swap32(words[OFF_BANK / 4 + i]);
    s.spsr = swap32(words[OFF_SPSR_VALUE / 4]);
    s.cpsr = swap32(words[OFF_CPSR / 4]);
}

static std::string diffState(const State &got, const State &want) {
    char text[1024];
    int n = 0;
    for (int i = 0; i < 16; i++)
        if (got.usr[i] != want.usr[i]) n += snprintf(text + n, sizeof(text) - n, "r%d %08X want %08X; ", i, got.usr[i], want.usr[i]);
    for (int i = 0; i < 7; i++)
        if (got.bank[i] != want.bank[i]) n += snprintf(text + n, sizeof(text) - n, "bank r%d %08X want %08X; ", i + 8, got.bank[i], want.bank[i]);
    if (got.spsr != want.spsr) n += snprintf(text + n, sizeof(text) - n, "spsr %08X want %08X; ", got.spsr, want.spsr);
    if (got.cpsr != want.cpsr) n += snprintf(text + n, sizeof(text) - n, "cpsr %08X want %08X; ", got.cpsr, want.cpsr);
    text[n] = 0;
    return text;
}

static void scheduleEvent() {
    // Something C++ did made an event due now, which generated code has to notice at its next check
    if (now.stopMode != STOP_SCHEDULED) return;
    writeBe(EVENTS + 0x10 + offsetof(SchedEvent, cycles), 0);
    now.scheduled = true;
}

static void hookFake(uc_engine *, uint64_t address, uint32_t, void *) {
    if (address >= FAKE && address < FAKE_MEMORY) {
        // Memory access from the slow path: load32, load8, store32, store8, load16, store16
        int type = (address - FAKE) / 8;
        count("slow memory access");
        uint32_t target = reg(UC_PPC_REG_4), value = reg(UC_PPC_REG_5);
        if (reg(UC_PPC_REG_3) != FAKE_MEMORY) fail("memory call without the memory object: %08X", reg(UC_PPC_REG_3));
        if (target < WINDOW || target >= WINDOW + WINDOW_SIZE - 4) {
            fail("memory call at %08X, outside what the test set up", target);
            uc_emu_stop(uc);
            return;
        }
        // Time must be saved for anything the access might do
        if (readBe(GLOBAL) != reg(UC_PPC_REG_28) || readBe(ARM_STATE + OFF_CYCLES) != reg(UC_PPC_REG_28))
            fail("memory call without saved time");
        uint32_t result = 0;
        switch (type) {
            case 0: {
                uint32_t word;
                uc_mem_read(uc, target & ~3, &word, 4);
                int shift = (target & 3) * 8;
                result = shift ? ((word << (32 - shift)) | (word >> shift)) : word;
                break;
            }
            case 1: { uint8_t b; uc_mem_read(uc, target, &b, 1); result = b; break; }
            case 4: { uint16_t h; uc_mem_read(uc, target & ~1, &h, 2); result = h; break; }
            case 2: { uint32_t w = value; uc_mem_write(uc, target & ~3, &w, 4); break; }
            case 3: { uint8_t b = value; uc_mem_write(uc, target, &b, 1); break; }
            case 5: { uint16_t h = value; uc_mem_write(uc, target & ~1, &h, 2); break; }
        }
        bool store = type == 2 || type == 3 || type == 5;
        if (store && now.setFlagOnSlowStore) writeBe(CACHE_FLAG, 1);
        scheduleEvent();
        clobberVolatile(true);
        setReg(UC_PPC_REG_3, store ? 0xDEAD0003 : result);
    }
    else if (address == FAKE_HANDLER) {
        // An interpreter handler; these are what the JIT is compared against, so only the call around them is tested
        count("handler call");
        if (now.fakeAt >= now.fakes.size()) {
            fail("unexpected handler call for %08X", reg(UC_PPC_REG_4));
            uc_emu_stop(uc);
            return;
        }
        FakeCall &call = now.fakes[now.fakeAt++];
        if (reg(UC_PPC_REG_3) != ARM_STATE) fail("handler called without the CPU: %08X", reg(UC_PPC_REG_3));
        if (reg(UC_PPC_REG_4) != call.op) fail("handler for %08X called with %08X", call.op, reg(UC_PPC_REG_4));
        if (readBe(ARM_STATE + 60) != call.pc) fail("handler sees PC %08X, want %08X", readBe(ARM_STATE + 60), call.pc);
        if (readBe(GLOBAL) != reg(UC_PPC_REG_28) || readBe(ARM_STATE + OFF_CYCLES) != reg(UC_PPC_REG_28))
            fail("handler called without saved time");
        if (call.jump) writeBe(ARM_STATE + 60, call.target);
        scheduleEvent();
        clobberVolatile(true);
        setReg(UC_PPC_REG_3, FAKE_HANDLER_CYCLES);
    }
    else if (address == FAKE_INTERLEAVE) {
        // A stop for another CPU or a task, which must see exactly the state after an instruction, flags included
        count("stop");
        now.stops++;
        State got;
        readState(got);
        got.mode = 0;
        uint32_t cycles = readBe(ARM_STATE + OFF_CYCLES);
        bool found = false;

        // The global cycles stay at the start of the instruction for the helper, and they have to be up to date
        if (readBe(GLOBAL) != reg(UC_PPC_REG_28)) fail("stop with global cycles %u, want %u", readBe(GLOBAL), reg(UC_PPC_REG_28));
        if (now.stopMode == STOP_SCHEDULED || now.stopMode == STOP_DEADLINE) {
            writeBe(EVENTS + 0x10 + offsetof(SchedEvent, cycles), 0xF0000000);
            writeBe(LIMIT, 0xFFFFFFFF);
            now.scheduled = false;
        }
        for (size_t i = now.traceAt; i < now.trace.size() && !found; i++) {
            if (now.trace[i].cycles == cycles && now.trace[i].state.usr[15] == got.usr[15]) {
                now.traceAt = i + 1;
                found = true;
                std::string difference = diffState(got, now.trace[i].state);
                if (!difference.empty()) fail("wrong state at a stop after instruction %d: %s", int(i), difference.c_str());
            }
        }
        if (!found) fail("stop at PC %08X, cycles %u, doesn't follow any instruction", got.usr[15], cycles - START_CYCLES);
        bool changed = readBe(CACHE_FLAG);
        bool stop = changed || (now.stopMode == STOP_AT && now.stops > now.stopAfter);
        if (!stop) {
            writeBe(GLOBAL, cycles);
            writeBe(ARM_STATE + OFF_CYCLES, cycles);
        }
        clobberVolatile(false);
        setReg(UC_PPC_REG_3, stop ? 0 : 1);
    }
}

static void setupMemoryMode(int mode) {
    // Point the emulator's memory maps at the window, or leave pages out so accesses go through the slow path
    static int current = -1;
    if (mode == current) return;
    current = mode;
    for (uint32_t page = 0; page < WINDOW_SIZE >> 12; page++) {
        bool mapped = mode == MEMORY_MAPPED || (mode == MEMORY_ODD_PAGES && (page & 1));
        uint32_t index = ((WINDOW >> 12) + page) * 4;
        writeBe(READ_MAP + index, mapped ? WINDOW + (page << 12) : 0);
        writeBe(WRITE_MAP + index, mapped ? WINDOW + (page << 12) : 0);
    }
}

static uint32_t emuCode(void *host) { return CODE + uint32_t((uint8_t*)host - codeBase); }

// Pieces of one case
struct Case {
    std::string name;
    int cpu = 0, memoryMode = MEMORY_MAPPED, stopMode = STOP_NEVER, stopAfter = 0;
    bool setFlagOnSlowStore = false, compareMemory = true, useCache = true;
    uint32_t deadline = 0; // When an event is due, or with limitDeadline, when the other CPU is
    bool limitDeadline = false;
    State in;
    uint32_t start; // Guest address to enter at
    std::vector<JitBlock*> blocks; // Blocks that can run: entered from the first, and reachable from it
    std::function<bool(uint32_t op, bool thumb)> fakeJumps;
    uint32_t fakeTarget = 0;
};

static Jit &jitOf(int cpu) { return cpu ? core->jit7 : core->jit; }

static void loadOracle(Interpreter &arm, State &s) {
    static uint32_t bank[7], spsr;
    for (int i = 0; i < 16; i++) arm.registersUsr[i] = s.usr[i];
    for (int i = 0; i < 7; i++) bank[i] = s.bank[i];
    spsr = s.spsr;
    for (int i = 0; i < 16; i++) arm.registers[i] = banked(s.mode, i) ? &bank[i - 8] : &arm.registersUsr[i];
    arm.spsr = s.mode ? &spsr : nullptr;
    arm.cpsr = s.cpsr;
    arm.lazyPipeline = true;
}

static void saveOracle(Interpreter &arm, State &s) {
    for (int i = 0; i < 16; i++) s.usr[i] = arm.registersUsr[i];
    for (int i = 0; i < 7; i++) s.bank[i] = (banked(s.mode, i + 8)) ? *arm.registers[i + 8] : s.bank[i];
    if (arm.spsr) s.spsr = *arm.spsr;
    s.cpsr = arm.cpsr;
}

static JitBlock *findBlock(Case &c, uint32_t address, bool thumb) {
    for (JitBlock *b: c.blocks)
        if (b->address == address && b->thumb == thumb) return b;
    return nullptr;
}

static bool runOracle(Case &c, State &out, uint32_t &cycles, std::vector<Snapshot> &trace, std::vector<FakeCall> &fakes) {
    // Run the blocks as the portable executor would, one instruction at a time with the interpreter's handlers
    // Handlers for instructions without native code aren't called, since the emulator only sees their fake
    Interpreter &arm = core->interpreter[c.cpu];
    State s = c.in;
    loadOracle(arm, s);
    JitBlock *block = c.blocks[0];
    size_t index = 0;
    cycles = 0;
    int guard = 0;
    while (block && guard++ < 400) {
        JitInstr &instr = block->instrs[index];
        uint32_t op = instr.opcode;
        uint32_t step = block->thumb ? 2 : 4;
        uint32_t address = block->address + index * step;
        uint32_t pc = address + step * 2;
        arm.registersUsr[15] = pc;
        int kind = rigKind(op, block->thumb, c.cpu);
        bool pass = block->thumb || Interpreter::condition[((op >> 24) & 0xF0) | (arm.cpsr >> 28)] != 0;
        bool jumped = false;
        int cost = 1;
        if (kind == NK_NONE) {
            if (pass) {
                bool jump = c.fakeJumps && c.fakeJumps(op, block->thumb);
                fakes.push_back({ op, pc, jump, c.fakeTarget });
                cost = FAKE_HANDLER_CYCLES;
                if (jump) {
                    arm.registersUsr[15] = c.fakeTarget;
                    jumped = true;
                }
            }
        }
        else if (pass) {
            cost = block->thumb ? (arm.*Interpreter::thumbInstrs[(op >> 6) & 0x3FF])(op) :
                (arm.*Interpreter::armInstrs[((op >> 16) & 0xFF0) | ((op >> 4) & 0xF)])(op);
            bool thumbJump = block->thumb && kind == NK_THUMB_BRANCH && (op & 0xF800) != 0xF000 &&
                ((op & 0xF000) != 0xD000 || cost == 3);
            jumped = arm.registersUsr[15] != pc || thumbJump || (!block->thumb && rigKindJumps(kind));
        }
        cycles += cost << c.cpu;
        saveOracle(arm, s);
        trace.push_back({ s, START_CYCLES + cycles });

        index++;
        if (jumped || index == block->instrs.size()) {
            bool thumb = arm.cpsr & BIT(5);
            uint32_t next = jumped ? (arm.registersUsr[15] - (thumb ? 2 : 4)) : (address + step);
            block = findBlock(c, next, thumb);
            index = 0;
        }
    }
    if (guard >= 400) return false;
    out = s;
    return true;
}

static void writeWords(uint32_t address, const uint8_t *from, size_t size) {
    // Generated words are in the Mac's byte order, and the emulator is big-endian
    static std::vector<uint32_t> words;
    words.resize(size / 4);
    memcpy(words.data(), from, size);
    for (uint32_t &word: words) word = swap32(word);
    uc_mem_write(uc, address, words.data(), size);
}

static void writeCodeToEmulator(Jit &jit) {
    // Everything generated since the stubs, which includes jumps patched in earlier blocks
    size_t size = jit.codePtr - jit.codeStart;
    uint32_t start = emuCode(jit.codeStart);
    if (size) {
        writeWords(start, jit.codeStart, size);
        uc_ctl_remove_cache(uc, start, start + size + 4);
    }
}

static void runCase(Case &c) {
    Jit &jit = jitOf(c.cpu);
    now.name = c.name;
    now.inCpsr = c.in.cpsr;
    now.cpu = c.cpu;
    now.stopMode = c.stopMode;
    now.stopAfter = c.stopAfter;
    now.memoryMode = c.memoryMode;
    now.setFlagOnSlowStore = c.setFlagOnSlowStore;
    now.failed = false;
    now.stops = 0;
    now.scheduled = false;
    now.trace.clear();
    now.traceAt = 0;
    now.fakes.clear();
    now.fakeAt = 0;
    JitBlock *first = c.blocks[0];
    char text[256];
    int n = snprintf(text, sizeof(text), "cpu %d %s mode %d mem %d stop %d/%d at %08X:", c.cpu, first->thumb ? "T" : "A", c.in.mode,
        c.memoryMode, c.stopMode, c.stopAfter, first->address);
    for (size_t i = 0; i < first->instrs.size() && i < 12; i++) n += snprintf(text + n, sizeof(text) - n, " %08X", first->instrs[i].opcode);
    now.description = text;

    // Get the expected result, which can write to main RAM
    State want;
    uint32_t cycles;
    if (!runOracle(c, want, cycles, now.trace, now.fakes)) {
        count("skipped: branches back forever");
        return;
    }
    want.mode = c.in.mode;

    // Set up the emulator; generated code expects the words big-endian, and guest memory stays little-endian
    writeCodeToEmulator(jit);
    setupMemoryMode(c.memoryMode);
    uint32_t words[64] = {};
    for (int i = 0; i < 16; i++) words[i] = swap32(c.in.usr[i]);
    for (int i = 0; i < 16; i++)
        words[OFF_REGS / 4 + i] = swap32(banked(c.in.mode, i) ? (ARM_STATE + OFF_BANK + (i - 8) * 4) : (ARM_STATE + i * 4));
    words[OFF_CPSR / 4] = swap32(c.in.cpsr);
    words[OFF_CYCLES / 4] = swap32(START_CYCLES);
    words[OFF_SPSR / 4] = swap32(c.in.mode ? (ARM_STATE + OFF_SPSR_VALUE) : 0);
    for (int i = 0; i < 7; i++) words[OFF_BANK / 4 + i] = swap32(c.in.bank[i]);
    words[OFF_SPSR_VALUE / 4] = swap32(c.in.spsr);
    uc_mem_write(uc, ARM_STATE, words, sizeof(words));
    writeBe(GLOBAL, START_CYCLES);
    writeBe(LIMIT, (c.stopMode == STOP_DEADLINE && c.limitDeadline) ? START_CYCLES + c.deadline : 0xFFFFFFFF);
    uint32_t due = (c.stopMode == STOP_NEVER || c.stopMode == STOP_SCHEDULED) ? 0xF0000000 : 0;
    if (c.stopMode == STOP_DEADLINE) due = c.limitDeadline ? 0xF0000000 : START_CYCLES + c.deadline;
    writeBe(EVENTS + 0x10 + offsetof(SchedEvent, cycles), due);
    writeBe(CACHE_FLAG, 0);

    // Fill the block cache with what can be reached, which the dispatcher uses
    static std::vector<uint32_t> usedEntries;
    for (uint32_t at: usedEntries) { writeBe(at, 0); writeBe(at + 4, 0); }
    usedEntries.clear();
    for (JitBlock *b: c.blocks) {
        if (!b->code) { fail("block has no code"); return; }
        if (!c.useCache) continue;
        uint32_t at = CACHE + ((b->address >> 1) & 0xFFF) * 16;
        writeBe(at, b->address | b->thumb);
        writeBe(at + 4, emuCode(b->code));
        usedEntries.push_back(at);
    }

    setReg(UC_PPC_REG_1, STACK);
    setReg(UC_PPC_REG_3, ARM_STATE);
    setReg(UC_PPC_REG_4, EVENTS);
    setReg(UC_PPC_REG_5, LIMIT);
    setReg(UC_PPC_REG_6, emuCode(first->code));
    setReg(UC_PPC_REG_7, GLOBAL);
    setReg(UC_PPC_REG_8, CACHE);
    setReg(UC_PPC_REG_LR, END);

    uc_err err = uc_emu_start(uc, emuCode(jit.codeEnter), END, 0, 200000);
    if (err != UC_ERR_OK || reg(UC_PPC_REG_PC) != END) {
        if (!now.failed) fail("didn't finish: %s at %08X", uc_strerror(err), reg(UC_PPC_REG_PC));
    }
    else if (!now.failed) {
        State got;
        readState(got);
        got.mode = c.in.mode;
        bool stopped = c.stopMode == STOP_AT && now.traceAt > 0 && now.stops > c.stopAfter;
        bool changed = readBe(CACHE_FLAG);
        if (now.scheduled) fail("an event made due by a C++ call wasn't noticed");
        if (!stopped && !changed) {
            std::string difference = diffState(got, want);
            if (!difference.empty()) fail("%s", difference.c_str());
            uint32_t gotCycles = readBe(ARM_STATE + OFF_CYCLES);
            if (gotCycles != START_CYCLES + cycles) fail("cycles %u want %u", gotCycles - START_CYCLES, cycles);
            if (readBe(GLOBAL) != gotCycles) fail("global cycles %u want %u", readBe(GLOBAL), gotCycles);
            if (now.fakeAt != now.fakes.size()) fail("%d handlers called, want %d", int(now.fakeAt), int(now.fakes.size()));
        }
    }

    // Compare memory with the interpreter's, and put both back
    static uint8_t emuWindow[WINDOW_SIZE];
    uc_mem_read(uc, WINDOW, emuWindow, WINDOW_SIZE);
    bool emuChanged = memcmp(emuWindow, seed, WINDOW_SIZE) != 0;
    if (c.compareMemory && !(c.stopMode == STOP_AT || c.setFlagOnSlowStore) && !now.failed) {
        uint8_t *host = ram + (WINDOW & 0x3FFFFF);
        if (memcmp(emuWindow, host, WINDOW_SIZE)) {
            for (uint32_t a = 0; a < WINDOW_SIZE; a += 4) {
                if (memcmp(emuWindow + a, host + a, 4)) {
                    uint32_t x, y;
                    memcpy(&x, emuWindow + a, 4);
                    memcpy(&y, host + a, 4);
                    fail("memory at %08X is %08X, want %08X", WINDOW + a, x, y);
                    break;
                }
            }
        }
    }
    if (emuChanged) uc_mem_write(uc, WINDOW, seed, WINDOW_SIZE);
}

// Writing and compiling code

static void restoreRam() { memcpy(ram + (WINDOW & 0x3FFFFF), seed, WINDOW_SIZE); }

static void putCode(uint32_t address, const uint32_t *ops, size_t size, bool thumb) {
    // Code goes in main RAM to compile from, in the seed so it's what the window goes back to, and in the emulator
    uint32_t step = thumb ? 2 : 4;
    uint8_t *to = ram + (address & 0x3FFFFF);
    for (size_t i = 0; i < size; i++) memcpy(to + i * step, &ops[i], step);
    memcpy(seed + (address - WINDOW), to, size * step);
    uc_mem_write(uc, address, to, size * step);
}

static void clearCode(uint32_t address, size_t bytes) {
    static std::mt19937 fill(99);
    uint8_t *to = seed + (address - WINDOW);
    for (size_t i = 0; i < bytes; i++) to[i] = fill();
    memcpy(ram + (address & 0x3FFFFF), to, bytes);
    uc_mem_write(uc, address, to, bytes);
}

static JitBlock *compileAt(int cpu, uint32_t address, bool thumb) {
    Jit &jit = jitOf(cpu);
    uint8_t *host = ram + (address & 0x3FFFFF);
    int index = jit.tableIndex(host);
    if (index < 0) { fail("can't compile at %08X", address); return nullptr; }
    return jit.compile(address, thumb, host, index);
}

static void finishBlocks(int cpu) {
    // Drop every block, going back to an empty code area
    Jit &jit = jitOf(cpu);
    static std::vector<JitBlock*> live;
    live.clear();
    int pages = (WINDOW_SIZE + 0x10000) >> 12;
    for (int i = 0; i < pages; i++) {
        int page = jit.pageIndex(ram + (WINDOW & 0x3FFFFF) + (i << 12));
        if (page < 0) continue;
        std::vector<JitBlock*> blocks = jit.pages[page].blocks;
        for (JitBlock *b: blocks) jit.retire(b);
    }
    if (!jit.unlinks.empty()) jit.applyUnlinks();
    for (JitBlock *b: jit.graveyard) delete b;
    jit.graveyard.clear();
    jit.pendingLinks.clear();
    jit.codePtr = jit.codeStart;
    jit.codeChanged = 0;
    for (int i = 0; i < pages; i++) {
        int page = jit.pageIndex(ram + (WINDOW & 0x3FFFFF) + (i << 12));
        if (page >= 0) {
            jit.pages[page].overwrites = 0;
            jit.pages[page].cleared = 0;
        }
    }
}

// Random machine states

static std::mt19937 rng;
static uint32_t rnd(uint32_t n) { return rng() % n; }

static uint32_t interesting() {
    static const uint32_t edge[] = { 0, 1, 2, 3, 7, 31, 32, 33, 255, 256, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE,
        0xFFFF, 0x10000, 0xFFFFFF, 0x1000000, 0x7F, 0x80 };
    switch (rnd(4)) {
        case 0: return rng();
        case 1: return edge[rnd(sizeof(edge) / sizeof(edge[0]))];
        case 2: return rnd(64);
        default: return (rng() >> rnd(32)) * (rnd(2) ? 1 : 0xFFFFFFFF);
    }
}

static State randomState(bool thumb, int mode) {
    State s;
    for (int i = 0; i < 15; i++) s.usr[i] = interesting();
    for (int i = 0; i < 7; i++) s.bank[i] = interesting();
    s.spsr = interesting();
    s.usr[15] = 0;
    s.mode = mode;
    s.cpsr = (mode == 0 ? 0x1F : mode == 1 ? 0x12 : 0x11) | (thumb ? 0x20 : 0) | (rnd(16) << 28) | (rnd(2) ? BIT(7) : 0);
    return s;
}

static uint32_t randomData(bool misaligned) {
    uint32_t a = DATA_START + 0x100 + rnd(DATA_END - DATA_START - 0x200);
    return misaligned ? a : (a & ~3);
}

static uint32_t randomNearPage() {
    // Close enough to a page end for a list of registers to cross it
    uint32_t page = DATA_START + 0x1000 * (1 + rnd(20));
    return (page - 64 + rnd(16) * 8) & ~3;
}

static bool inData(uint32_t a) { return a >= WINDOW + 0x100 && a < WINDOW + WINDOW_SIZE - 0x200; }

static uint32_t shifted(uint32_t value, uint32_t type, uint32_t amount, uint32_t cpsr) {
    switch (type) {
        case 0: return value << amount;
        case 1: return amount ? value >> amount : 0;
        case 2: return amount ? uint32_t(int32_t(value) >> amount) : uint32_t(int32_t(value) >> 31);
        default: return amount ? ((value >> amount) | (value << (32 - amount))) : (((cpsr >> 29) & 1) << 31 | (value >> 1));
    }
}

static bool fitAddress(uint32_t a, int kind, State &s, uint32_t pc, bool nearPage) {
    // Make a load or store land in the window, returning false if this instruction can't
    uint32_t rn = (a >> 16) & 0xF, rm = a & 0xF;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t base = nearPage ? randomNearPage() : randomData(rnd(4) == 0);
        if (kind == NK_BLOCK || kind == NK_BLOCK_PC || kind == NK_SWAP) {
            if (kind == NK_SWAP) base &= ~3u;
            visible(s, rn) = base;
            return true;
        }
        bool up = a & BIT(23), pre = a & BIT(24);
        uint32_t offset;
        if (kind == NK_TRANSFER) {
            if (!(a & BIT(25))) {
                offset = a & 0xFFF;
                if (rn != 15) visible(s, rn) = base;
            }
            else {
                if (rn != 15) visible(s, rn) = base;
                if (rm != 15 && rm != rn) visible(s, rm) = rnd(64);
                if (rm == rn && rn != 15) visible(s, rn) = (base >> 1) & ~1u;
                uint32_t value = rm == 15 ? pc : visible(s, rm);
                uint32_t type = (a >> 5) & 3, amount = (a >> 7) & 0x1F;
                offset = shifted(value, type, amount, s.cpsr);
            }
        }
        else {
            if (a & BIT(22)) {
                offset = ((a >> 4) & 0xF0) | (a & 0xF);
                if (rn != 15) visible(s, rn) = base;
            }
            else {
                if (rn != 15) visible(s, rn) = base;
                if (rm != 15 && rm != rn) visible(s, rm) = rnd(64);
                if (rm == rn && rn != 15) visible(s, rn) = (base >> 1) & ~1u;
                offset = rm == 15 ? pc : visible(s, rm);
            }
        }
        uint32_t from = rn == 15 ? pc : visible(s, rn);
        uint32_t target = pre ? (up ? from + offset : from - offset) : from;
        if (inData(target)) return true;
    }
    return false;
}

static bool isMemoryKind(int kind) {
    return kind == NK_TRANSFER || kind == NK_HALF || kind == NK_BLOCK || kind == NK_BLOCK_PC || kind == NK_SWAP;
}

static const char *kindName(int kind) {
    switch (kind) {
        case NK_NONE: return "handler";
        case NK_ALU: return "alu";
        case NK_BRANCH: return "branch";
        case NK_TRANSFER: return "transfer";
        case NK_BRANCH_REG: return "branch reg";
        case NK_HALF: return "half";
        case NK_BLOCK: return "block";
        case NK_BLOCK_PC: return "block pc";
        case NK_SWAP: return "swap";
        case NK_MULTIPLY: return "multiply";
        case NK_MISC: return "misc";
        default: return "thumb branch";
    }
}

static uint32_t codeAddress(bool thumb) {
    // Somewhere not at a page end, so the block has room for its last instruction
    uint32_t a = CODE_START + rnd(0x18000);
    if ((a & 0xFFF) > 0xF00) a -= 0x200;
    return thumb ? (a & ~1u) : (a & ~3u);
}

static uint32_t terminator(bool thumb, uint32_t address, uint32_t offset) {
    // An unconditional branch to a point later in memory, which ends the block
    if (thumb) return 0xE000 | ((((address + offset) - (address + 4)) >> 1) & 0x7FF);
    return 0xEA000000 | ((((address + offset) - (address + 8)) >> 2) & 0xFFFFFF);
}

static void testOneOp(const std::string &suite, int cpu, bool thumb, uint32_t op, int mode, int memoryMode, const State *given,
        bool nearPage = false) {
    // One instruction followed by a branch, through every way to the C++ code it can use
    uint32_t address = codeAddress(thumb), step = thumb ? 2 : 4;
    uint32_t ops[2] = { op, terminator(thumb, address + step, 32) };
    clearCode(address, 16);
    putCode(address, ops, 2, thumb);
    JitBlock *block = compileAt(cpu, address, thumb);
    if (!block) return;

    State s = given ? *given : randomState(thumb, mode);
    int kind = rigKind(op, thumb, cpu);
    uint32_t pc = address + step * 2;
    bool fits = true;
    if (isMemoryKind(kind)) {
        uint32_t a = thumb ? rigArm(op) : op;
        fits = fitAddress(a, kind, s, thumb && ((op >> 11) == 0x09 || (op >> 11) == 0x14) ? (pc & ~3u) : pc, nearPage);
    }

    if (fits) {
        Case c;
        c.name = suite;
        c.cpu = cpu;
        c.in = s;
        c.memoryMode = memoryMode;
        c.blocks = { block };
        c.fakeJumps = [&](uint32_t, bool) { return false; };
        if (kind == NK_NONE && rigMayJump(op, thumb) && rnd(2)) {
            c.fakeTarget = WINDOW + 0x3F008;
            c.fakeJumps = [&](uint32_t o, bool t) { return o == op && t == thumb; };
        }
        runCase(c);
        count("suite:" + suite);
        count(std::string(thumb ? "thumb " : "arm ") + kindName(kind));
        if (cpu == 1) count("arm7 cases");
    }
    else {
        count("skipped: no address fits");
    }
    restoreRam();
    finishBlocks(cpu);
}

// Suites

static bool mine(uint64_t index) { return index % shards == uint64_t(shard); }

static void suiteThumbOps() {
    // Every THUMB encoding on both CPUs, in each kind of memory, in each CPU mode
    for (int cpu = 0; cpu < 2; cpu++) {
        for (uint32_t op = 0; op < 0x10000; op++) {
            if (!mine(op)) continue;
            rng.seed(op * 2 + cpu + 1000);
            printf("SEEN\tthumb%d\t%04X\n", cpu, op);
            for (int i = 0; i < 6; i++)
                testOneOp("thumb ops", cpu, true, op, i % 3, i < 3 ? MEMORY_MAPPED : (i == 3 ? MEMORY_SLOW : MEMORY_ODD_PAGES), nullptr);
        }
    }
}

static void suiteArmOps() {
    // Every decode position of the interpreter's ARM table with each condition, and random registers and offsets
    const int fills = 3;
    for (int cpu = 0; cpu < 2; cpu++) {
        for (uint32_t index = 0; index < 0x1000; index++) {
            for (uint32_t cond = 0; cond < 16; cond++) {
                if (!mine(index * 16 + cond)) continue;
                rng.seed((index * 16 + cond) * 2 + cpu + 5000);
                for (int fill = 0; fill < fills; fill++) {
                    uint32_t op = (cond << 28) | ((index & 0xFF0) << 16) | ((index & 0xF) << 4) | (rng() & 0x000FFF0F);
                    printf("SEEN\tarm%d\t%03X%X\n", cpu, index, cond);
                    for (int i = 0; i < 6; i++)
                        testOneOp("arm ops", cpu, false, op, i % 3, i < 3 ? MEMORY_MAPPED : (i == 3 ? MEMORY_SLOW : MEMORY_ODD_PAGES), nullptr);
                }
            }
        }
    }
}

static void suiteArmForms() {
    // ARM instructions with exact encodings, which random bits rarely make, with every combination of registers
    uint64_t index = 0;
    std::vector<uint32_t> ops;
    for (uint32_t rm = 0; rm < 16; rm++) {
        ops.push_back(0x012FFF10 | rm); // BX
        ops.push_back(0x012FFF30 | rm); // BLX
        for (uint32_t rd = 0; rd < 16; rd++) {
            ops.push_back(0x016F0F10 | (rd << 12) | rm); // CLZ
            for (uint32_t rn = 0; rn < 16; rn++) {
                ops.push_back(0x01000090 | (rn << 16) | (rd << 12) | rm); // SWP
                ops.push_back(0x01400090 | (rn << 16) | (rd << 12) | rm); // SWPB
            }
        }
    }
    for (uint32_t rd = 0; rd < 16; rd++) {
        ops.push_back(0x010F0000 | (rd << 12)); // MRS from CPSR
        ops.push_back(0x014F0000 | (rd << 12)); // MRS from SPSR
    }
    std::mt19937 fields(777);
    auto r4 = [&]() { return fields() & 0xF; };
    for (int i = 0; i < 40000; i++) {
        uint32_t type = fields() % 8; // MUL, MLA, UMULL, UMLAL, SMULL, SMLAL, with and without flags
        static const uint32_t forms[] = { 0x00000090, 0x00200090, 0x00800090, 0x00A00090, 0x00C00090, 0x00E00090 };
        ops.push_back(forms[type % 6] | ((fields() & 1) << 20) | (r4() << 16) | (r4() << 12) | (r4() << 8) | r4());
    }
    for (int i = 0; i < 40000; i++) {
        // Halfword and signed transfers in every addressing mode, including forms the JIT leaves to handlers
        ops.push_back(0x00000090 | ((fields() & 1) << 24) | ((fields() & 1) << 23) | ((fields() & 1) << 22) | ((fields() & 1) << 21) |
            ((fields() & 1) << 20) | (r4() << 16) | (r4() << 12) | (r4() << 8) | ((1 + fields() % 3) << 5) | r4());
    }
    for (int i = 0; i < 60000; i++) {
        // Words and bytes in every addressing mode
        uint32_t reg = fields() & 1;
        ops.push_back(0x04000000 | (reg << 25) | ((fields() & 0x1F) << 20) | (r4() << 16) | (r4() << 12) |
            (reg ? (((fields() & 0x1F) << 7) | ((fields() & 3) << 5) | r4()) : (fields() & 0xFFF)));
    }
    for (uint32_t op: ops) {
        if (!mine(index++)) continue;
        rng.seed(index + 70000);
        for (int cpu = 0; cpu < 2; cpu++) {
            for (int i = 0; i < 3; i++) {
                uint32_t cond = (i == 2) ? rnd(14) : 14;
                testOneOp("arm forms", cpu, false, (cond << 28) | op, i, i == 0 ? MEMORY_MAPPED : (i == 1 ? MEMORY_SLOW : MEMORY_ODD_PAGES), nullptr);
            }
        }
    }
}

static void suiteFlagGrid() {
    // Each arithmetic and logical flag-setting instruction, over every pair of edge values and starting carry
    static const uint32_t values[] = { 0, 1, 2, 0x7FFFFFFE, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF, 0x55555555, 0xAAAAAAAA };
    const int size = sizeof(values) / sizeof(values[0]);
    std::vector<std::pair<bool, uint32_t>> ops;
    for (uint32_t dp = 0; dp < 16; dp++) ops.push_back({ true, 0x4000 | (dp << 6) | (1 << 3) | 0 }); // Data processing r0,r1
    ops.push_back({ true, 0x1880 }); // ADD r0,r0,r2
    ops.push_back({ true, 0x1A80 }); // SUB r0,r0,r2
    ops.push_back({ true, 0x4288 }); // CMP r0,r1
    ops.push_back({ true, 0x4548 }); // CMP r0,r9, with a high register
    for (uint32_t opc = 0; opc < 16; opc++) {
        ops.push_back({ false, 0xE0100000 | (opc << 21) | (1 << 16) | (0 << 12) | 2 }); // Register
        ops.push_back({ false, 0xE0100010 | (opc << 21) | (1 << 16) | (0 << 12) | (3 << 8) | 2 }); // LSL by register
        ops.push_back({ false, 0xE0100030 | (opc << 21) | (1 << 16) | (0 << 12) | (3 << 8) | 2 }); // LSR by register
        ops.push_back({ false, 0xE0100050 | (opc << 21) | (1 << 16) | (0 << 12) | (3 << 8) | 2 }); // ASR by register
        ops.push_back({ false, 0xE0100070 | (opc << 21) | (1 << 16) | (0 << 12) | (3 << 8) | 2 }); // ROR by register
        ops.push_back({ false, 0xE0100060 | (opc << 21) | (1 << 16) | (0 << 12) | 2 }); // RRX
    }
    uint64_t index = 0;
    for (auto &entry: ops) {
        for (int a = 0; a < size; a++) {
            for (int b = 0; b < size; b++) {
                for (int carry = 0; carry < 2; carry++) {
                    if (!mine(index++)) continue;
                    rng.seed(index + 9000);
                    for (int cpu = 0; cpu < 2; cpu++) {
                        State s = randomState(entry.first, 0);
                        s.usr[0] = values[a];
                        s.usr[1] = values[b];
                        s.usr[2] = values[b];
                        s.usr[3] = rnd(2) ? values[a] : (rnd(40));
                        s.usr[9] = values[b];
                        s.cpsr = (s.cpsr & 0x0FFFFFFF) | (carry << 29) | (rnd(2) << 28) | (rnd(4) << 30);
                        testOneOp("flag grid", cpu, entry.first, entry.second, 0, MEMORY_MAPPED, &s);
                    }
                }
            }
        }
    }
}

static void suiteConditionGrid() {
    // Every condition with every flag value, on instructions that go different ways through conditions
    uint64_t index = 0;
    for (uint32_t cond = 0; cond < 15; cond++) {
        for (uint32_t flags = 0; flags < 16; flags++) {
            std::vector<std::pair<bool, uint32_t>> ops = {
                { false, 0x0A000004 }, // B
                { false, 0x0B000004 }, // BL
                { false, 0x012FFF12 }, // BX r2
                { false, 0x00810002 }, // ADD r0,r1,r2
                { false, 0x00910002 }, // ADDS r0,r1,r2
                { false, 0x03A00005 }, // MOV r0,#5
                { false, 0x01500001 }, // CMP r0,r1
                { false, 0x05910000 }, // LDR r0,[r1]
                { false, 0x05810000 }, // STR r0,[r1]
                { false, 0x01D100B0 }, // LDRH r0,[r1]
                { false, 0x08910003 }, // LDMIA r1,{r0,r1}
                { false, 0x08810003 }, // STMIA r1,{r0,r1}
                { false, 0x08BD8001 }, // LDMIA SP!,{r0,PC}
                { false, 0x00000291 }, // MUL r0,r1,r2
                { false, 0x010F0000 }, // MRS r0,CPSR
            };
            if (cond < 14) ops.push_back({ true, 0xD000 | (cond << 8) | 0x04 }); // THUMB branch
            for (auto &entry: ops) {
                if (!mine(index++)) continue;
                rng.seed(index + 20000);
                for (int cpu = 0; cpu < 2; cpu++) {
                    for (int i = 0; i < 2; i++) {
                        uint32_t op = entry.first ? entry.second : ((cond << 28) | entry.second);
                        State s = randomState(entry.first, i);
                        s.usr[2] = 0x02100000 + rnd(0x1000) * 2 + rnd(2);
                        s.cpsr = (s.cpsr & 0x0FFFFFFF) | (flags << 28);
                        testOneOp("condition grid", cpu, entry.first, op, i, i ? MEMORY_SLOW : MEMORY_MAPPED, &s);
                    }
                }
            }
        }
    }
}

static void suiteBlockLists() {
    // PUSH, POP, LDM and STM with register lists crossing into pages that have to take the slow path
    uint64_t index = 0;
    std::vector<std::pair<bool, uint32_t>> forms;
    for (uint32_t list = 0; list < 0x100; list++) {
        forms.push_back({ true, 0xB400 | list });
        forms.push_back({ true, 0xB500 | list });
        forms.push_back({ true, 0xBC00 | list });
        forms.push_back({ true, 0xBD00 | list });
        forms.push_back({ true, 0xC000 | (rnd(8) << 8) | list });
        forms.push_back({ true, 0xC800 | (rnd(8) << 8) | list });
    }
    for (int form = 0; form < 500; form++) {
        uint32_t list = (rng() & 0xFFFF) >> rnd(14);
        uint32_t bits = 0xE8000000 | (rnd(16) << 16) | (rnd(16) << 20) | (rnd(16) << 21 & 0x01800000);
        forms.push_back({ false, (bits & ~0x00400000u) | list });
    }
    for (auto &entry: forms) {
        if (!mine(index++)) continue;
        rng.seed(index + 30000);
        for (int cpu = 0; cpu < 2; cpu++)
            for (int memory = 0; memory < 3; memory++)
                testOneOp("block lists", cpu, entry.first, entry.second, rnd(3), memory, nullptr, true);
    }
}

// Random blocks

static uint32_t randomArmOp(int cpu) {
    // Anything that keeps the bases in R7 and SP usable, with any condition now and then
    static const int writable[] = { 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 14 };
    auto rd = [&]() { return uint32_t(writable[rnd(13)]); };
    auto any = [&]() { return rnd(16); };
    auto base = [&]() { return rnd(2) ? 7u : 13u; };
    auto list = [&]() { return ((rng() & 0xFFFF) >> rnd(10)) & 0x5F7F; }; // No R7, SP or PC
    uint32_t cond = rnd(4) == 0 ? rnd(15) : 14;
    uint32_t op;
    switch (rnd(22)) {
        case 0: case 1: case 2: case 3: case 4: { // ALU with an immediate
            uint32_t opc = rnd(16), s = (opc >= 8 && opc <= 11) ? 1 : rnd(2);
            op = 0x02000000 | (opc << 21) | (s << 20) | (any() << 16) | (rd() << 12) | (rnd(16) << 8) | rnd(256);
            break;
        }
        case 5: case 6: case 7: case 8: { // ALU with a register shifted by an immediate
            uint32_t opc = rnd(16), s = (opc >= 8 && opc <= 11) ? 1 : rnd(2);
            op = (opc << 21) | (s << 20) | (any() << 16) | (rd() << 12) | (rnd(32) << 7) | (rnd(4) << 5) | any();
            break;
        }
        case 9: { // ALU with a register shifted by a register
            uint32_t opc = rnd(16), s = (opc >= 8 && opc <= 11) ? 1 : rnd(2);
            op = (opc << 21) | (s << 20) | (rnd(15) << 16) | (rd() << 12) | (rnd(15) << 8) | (rnd(4) << 5) | 0x10 | rnd(15);
            break;
        }
        case 10: case 11: { // LDR/LDRB, with writeback sometimes
            uint32_t b = base(), mode = rnd(3); // Offset, pre-index, post-index
            op = 0x04100000 | (mode != 2 ? BIT(24) : 0) | (mode == 1 ? BIT(21) : 0) | (rnd(2) << 23) | (rnd(2) << 22) |
                (b << 16) | (rd() << 12) | rnd(128);
            break;
        }
        case 12: { // STR/STRB
            uint32_t b = base(), mode = rnd(3);
            op = 0x04000000 | (mode != 2 ? BIT(24) : 0) | (mode == 1 ? BIT(21) : 0) | (rnd(2) << 23) | (rnd(2) << 22) |
                (b << 16) | (rnd(15) << 12) | rnd(128);
            break;
        }
        case 13: { // Halfwords and signed loads
            uint32_t load = rnd(2), type = load ? 1 + rnd(3) : 1;
            op = 0x01C00090 | (load << 20) | (rnd(2) << 23) | (base() << 16) | (rd() << 12) | (type << 5) | (rnd(16) << 8) | rnd(16);
            break;
        }
        case 14: { // LDM
            uint32_t l = list();
            op = 0x08100000 | (rnd(2) << 24) | (rnd(2) << 23) | (rnd(2) << 21) | (base() << 16) | (l ? l : 1);
            break;
        }
        case 15: { // STM
            uint32_t l = list();
            op = 0x08000000 | (rnd(2) << 24) | (rnd(2) << 23) | (rnd(2) << 21) | (base() << 16) | (l ? l : 1);
            break;
        }
        case 16: // Multiplies
            if (rnd(2)) op = 0x00000090 | (rnd(4) << 20) | (rnd(7) << 16) | (rnd(15) << 12) | (rnd(15) << 8) | rnd(15);
            else op = 0x00800090 | (rnd(8) << 20) | (rnd(7) << 16) | (rnd(7) << 12) | (rnd(15) << 8) | rnd(15);
            break;
        case 17: // CLZ, MRS
            if (rnd(2)) op = 0x016F0F10 | (rd() << 12) | rnd(15);
            else op = 0x010F0000 | (rnd(2) << 22) | (rd() << 12);
            break;
        case 18: // SWP
            op = 0x01000090 | (rnd(2) << 22) | (base() << 16) | (rd() << 12) | rnd(15);
            break;
        case 19: // A branch that may leave the block
            op = 0x0A000000 | (rnd(2) << 24) | ((rnd(40) - 20) & 0xFFFFFF);
            cond = rnd(14);
            break;
        case 20: // BX
            op = 0x012FFF10 | (rnd(2) << 5) | rnd(15);
            cond = rnd(14);
            break;
        default: // Things that call handlers without ending the block: LDRD, STRD, a multiply with the PC
            switch (rnd(3)) {
                case 0: op = 0x01C000D0 | (base() << 16) | (rnd(6) << 12); break;
                case 1: op = 0x01C000F0 | (base() << 16) | (rnd(6) << 12); break;
                default: op = 0x0000F090 | (rnd(7) << 16); break;
            }
            break;
    }
    return (cond << 28) | op;
}

static uint32_t randomThumbOp() {
    auto low = [&]() { return rnd(7); }; // No R7, which is a base
    auto any = [&]() { return rnd(8); };
    auto list = [&]() { return (rng() & 0x7F) >> rnd(6); };
    switch (rnd(24)) {
        case 0: case 1: return (rnd(3) << 11) | (rnd(32) << 6) | (any() << 3) | low(); // Shift by an immediate
        case 2: case 3: return 0x1800 | (rnd(4) << 9) | (rnd(8) << 6) | (any() << 3) | low(); // ADD/SUB
        case 4: return 0x2000 | (low() << 8) | rnd(256); // MOV
        case 5: return 0x2800 | (any() << 8) | rnd(256); // CMP
        case 6: return 0x3000 | (rnd(2) << 11) | (low() << 8) | rnd(256); // ADD/SUB
        case 7: case 8: case 9: return 0x4000 | (rnd(16) << 6) | (any() << 3) | low(); // Data processing
        case 10: { // High registers, not writing bases or the PC
            uint32_t rm = rnd(16);
            uint32_t kind = rnd(3), high = kind == 1 ? rnd(15) : ((rnd(2) ? rnd(7) : 8 + rnd(5)));
            return 0x4400 | (kind << 8) | ((high & 8) << 4) | (rm << 3) | (high & 7);
        }
        case 11: return 0x6800 | (rnd(32) << 6) | (7 << 3) | low(); // LDR [R7]
        case 12: return 0x7800 | (rnd(32) << 6) | (7 << 3) | low(); // LDRB [R7]
        case 13: return 0x8800 | (rnd(32) << 6) | (7 << 3) | low(); // LDRH [R7]
        case 14: return 0x4800 | (low() << 8) | rnd(256); // LDR [PC]
        case 15: return 0x9800 | (low() << 8) | rnd(256); // LDR [SP]
        case 16: switch (rnd(3)) { // Stores
            case 0: return 0x6000 | (rnd(32) << 6) | (7 << 3) | any();
            case 1: return 0x7000 | (rnd(32) << 6) | (7 << 3) | any();
            default: return 0x8000 | (rnd(32) << 6) | (7 << 3) | any();
        }
        case 17: return 0x9000 | (any() << 8) | rnd(256); // STR [SP]
        case 18: return 0xD000 | (rnd(14) << 8) | rnd(256); // Conditional branch
        case 19: return 0xA000 | (rnd(2) << 11) | (low() << 8) | rnd(256); // ADD PC/SP
        case 20: return 0xB000 | rnd(16); // ADD SP, small enough to keep it usable
        case 21: { uint32_t l = list(); return 0xB400 | rnd(2) << 8 | (l ? l : 1); } // PUSH
        case 22: { uint32_t l = list(); return 0xBC00 | (l ? l : 1); } // POP
        default: { uint32_t l = list(); return (rnd(2) ? 0xC000 : 0xC800) | (7 << 8) | (l ? l : 1); } // STMIA/LDMIA R7!
    }
}

static void suiteBlocks() {
    // Random blocks entered at the start or the middle, stopping at checks or not, in each kind of memory
    const uint64_t total = 400000;
    for (uint64_t index = 0; index < total; index++) {
        if (!mine(index)) continue;
        rng.seed(index + 40000);
        int cpu = rnd(4) == 0;
        bool thumb = rnd(2);
        uint32_t step = thumb ? 2 : 4;
        int size = 2 + rnd(30);
        bool pageEnd = rnd(8) == 0;
        uint32_t address = codeAddress(thumb);
        if (pageEnd) address = (((address >> 12) + 1) << 12) - size * step;
        std::vector<uint32_t> ops;
        for (int i = 0; i < size; i++) ops.push_back(thumb ? randomThumbOp() : randomArmOp(cpu));
        if (!pageEnd) ops.push_back(terminator(thumb, address + size * step, 32));

        clearCode(address & ~0xFFFu, 0x1000);
        putCode(address, ops.data(), ops.size(), thumb);
        JitBlock *host = compileAt(cpu, address, thumb);
        if (!host) continue;

        // Enter in the middle sometimes, which is a new block that reuses the code
        JitBlock *block = host;
        if (rnd(3) == 0 && host->instrs.size() > 2) {
            uint32_t skip = 1 + rnd(host->instrs.size() - 1);
            block = compileAt(cpu, address + skip * step, thumb);
            count("entered mid-block");
            if (!block) continue;
        }

        int mode = rnd(3);
        State s = randomState(thumb, mode);
        s.usr[7] = (DATA_START + 0x1000 + rnd(0xE000)) & ~3u;
        visible(s, 13) = (DATA_START + 0x1000 + rnd(0xE000)) & ~3u;

        Case c;
        c.name = std::string(thumb ? "thumb blocks" : "arm blocks") + (block != host ? ", entered mid-block" : "");
        c.cpu = cpu;
        c.in = s;
        c.blocks = { block };
        if (block != host) c.blocks.push_back(host); // A branch back can link to the block being reused
        c.memoryMode = rnd(3);
        c.stopMode = rnd(5);
        c.deadline = 1 + rnd(40);
        c.limitDeadline = rnd(2);
        c.stopAfter = rnd(6);
        c.setFlagOnSlowStore = c.memoryMode != MEMORY_MAPPED && rnd(4) == 0;
        c.fakeTarget = WINDOW + 0x3F008;
        runCase(c);
        count("suite:blocks");
        count(c.stopMode == STOP_NEVER ? "blocks without stops" : c.stopMode == STOP_ALWAYS ? "blocks stopping at every check" :
            c.stopMode == STOP_AT ? "blocks stopping partway" : c.stopMode == STOP_DEADLINE ? "blocks reaching a deadline" :
            "blocks with events from C++ calls");
        if (c.setFlagOnSlowStore) count("blocks with code changing");
        restoreRam();
        finishBlocks(cpu);
    }
}

static void suiteLinks() {
    // Blocks jumping to each other directly and through the dispatcher, as they're compiled and dropped
    const uint64_t total = 30000;
    for (uint64_t index = 0; index < total; index++) {
        if (!mine(index)) continue;
        rng.seed(index + 60000);
        int cpu = rnd(4) == 0;
        bool thumb = rnd(2);
        uint32_t step = thumb ? 2 : 4;
        int size = 1 + rnd(8);
        uint32_t page = CODE_START + 0x1000 * rnd(12);
        uint32_t addressA = page + 0x100, addressB = page + 0x180; // Close enough for THUMB conditional branches

        clearCode(page, 0x1000);
        std::vector<uint32_t> a, b;
        auto safe = [&]() {
            // Instructions that don't leave the block early
            for (int tries = 0; ; tries++) {
                uint32_t op = thumb ? randomThumbOp() : randomArmOp(cpu);
                int kind = rigKind(op, thumb, cpu);
                bool jumps = (kind == NK_BRANCH || kind == NK_BRANCH_REG || kind == NK_BLOCK_PC || kind == NK_THUMB_BRANCH);
                if (!jumps && !jitOf(cpu).endsBlock(op, thumb) && !rigMayJump(op, thumb)) return op;
            }
        };
        for (int i = 0; i < size; i++) a.push_back(safe());
        uint32_t end = addressA + a.size() * step;
        int ending = rnd(thumb ? 3 : 3);
        uint32_t condition = rnd(14);
        if (ending == 0) { // Unconditional branch
            a.push_back(terminator(thumb, end, addressB - end));
        }
        else if (ending == 1) { // Conditional, then the unconditional one so either way reaches it
            if (thumb) {
                uint32_t offset = addressB - (end + 4);
                a.push_back(0xD000 | (condition << 8) | (offset >> 1));
                count("links from THUMB conditional branches");
            }
            else {
                a.push_back((condition << 28) | 0x0A000000 | (((addressB - (end + 8)) >> 2) & 0xFFFFFF));
            }
            uint32_t next = addressA + a.size() * step;
            a.push_back(terminator(thumb, next, addressB - next));
        }
        else { // Back and forth through the dispatcher, changing mode
            a.push_back(thumb ? 0x4700 | (6 << 3) : 0xE12FFF16); // BX r6
        }
        for (int i = 0; i < size; i++) b.push_back(safe());
        uint32_t endB = addressB + b.size() * step;
        b.push_back(terminator(thumb, endB, 32));

        bool otherMode = ending == 2 && rnd(2);
        std::vector<uint32_t> bOther;
        for (int i = 0; i < size; i++) bOther.push_back(thumb ? randomArmOp(cpu) : randomThumbOp());
        if (otherMode) {
            b.clear();
            for (uint32_t op: bOther) {
                int kind = rigKind(op, !thumb, cpu);
                if (kind != NK_BRANCH && kind != NK_BRANCH_REG && kind != NK_BLOCK_PC && kind != NK_THUMB_BRANCH &&
                    !jitOf(cpu).endsBlock(op, !thumb) && !rigMayJump(op, !thumb)) b.push_back(op);
            }
            uint32_t stepB = thumb ? 4 : 2;
            uint32_t at = addressB + b.size() * stepB;
            b.push_back(terminator(!thumb, at, 32));
        }

        putCode(addressA, a.data(), a.size(), thumb);
        putCode(addressB, b.data(), b.size(), otherMode ? !thumb : thumb);

        int mode = rnd(3);
        State s = randomState(thumb, mode);
        s.usr[7] = (DATA_START + 0x1000 + rnd(0xE000)) & ~3u;
        visible(s, 13) = (DATA_START + 0x1000 + rnd(0xE000)) & ~3u;
        s.usr[6] = addressB | ((otherMode ? !thumb : thumb) ? 1 : 0);
        if (!thumb && !otherMode) s.usr[6] = addressB & ~3u;

        auto run = [&](const std::vector<JitBlock*> &blocks, const char *name) {
            // Without anything in the block cache, only a real link can reach a block directly branched to
            Case c;
            c.useCache = ending == 2;
            c.name = name;
            c.cpu = cpu;
            c.in = s;
            c.in.cpsr = (s.cpsr & ~BIT(5)) | (blocks[0]->thumb ? BIT(5) : 0);
            c.blocks = blocks;
            c.memoryMode = rnd(3);
            runCase(c);
            count("suite:links");
            restoreRam();
        };

        bool thumbB = otherMode ? !thumb : thumb;
        JitBlock *blockA = compileAt(cpu, addressA, thumb);
        if (!blockA) continue;
        std::vector<JitBlock*> onlyA = { blockA };
        run(onlyA, "links: target missing");

        JitBlock *blockB = compileAt(cpu, addressB, thumbB);
        if (!blockB) continue;
        std::vector<JitBlock*> both = { blockA, blockB };
        run(both, "links: target compiled after");

        Jit &jit = jitOf(cpu);
        jit.retire(blockB);
        if (!jit.unlinks.empty()) jit.applyUnlinks();
        run(onlyA, "links: target retired");

        blockB = compileAt(cpu, addressB, thumbB);
        if (!blockB) continue;
        both = { blockA, blockB };
        run(both, "links: target compiled again");

        std::vector<JitBlock*> onlyB = { blockB };
        run(onlyB, "links: entered at the target");

        // Compile in the other order, so the target exists when the branch is generated
        jit.retire(blockA);
        if (!jit.unlinks.empty()) jit.applyUnlinks();
        blockA = compileAt(cpu, addressA, thumb);
        if (!blockA) continue;
        both = { blockA, blockB };
        run(both, "links: target compiled first");

        finishBlocks(cpu);
    }
}

static void setup(Jit &jit) {
    // Everything generated code reads from outside itself goes to a known place in the emulator
    jit.offUsr = 0;
    jit.offRegs = OFF_REGS;
    jit.offPc = 60;
    jit.offCpsr = OFF_CPSR;
    jit.offCycles = OFF_CYCLES;
    jit.offSpsr = OFF_SPSR;
    jit.addrCondition = COND_TABLE;
    jit.addrMemory = FAKE_MEMORY;
    jit.addrReadMap = READ_MAP;
    jit.addrWriteMap = WRITE_MAP;
    jit.addrLoad32 = FAKE;
    jit.addrLoad8 = FAKE + 8;
    jit.addrStore32 = FAKE + 16;
    jit.addrStore8 = FAKE + 24;
    jit.addrLoad16 = FAKE + 32;
    jit.addrStore16 = FAKE + 40;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        printf("Usage: jit_test <settings folder> <rom> <shard> <shards> [suites]\n");
        return 2;
    }
    Settings::load(argv[1]);
    Settings::jit = 0;
    Settings::fastTiming = 1;
    Settings::dsiMode = 0;
    Settings::directBoot = 1;
    Settings::threaded2D = 0;
    Settings::threaded3D = 0;
    Settings::arm7Hle = 0;
    Settings::emulateAudio = 0;
    Settings::fpsLimiter = 0;
    Settings::fast2D = 0;
    Settings::highRes3D = 0;
    Settings::hardware3D = 0;
    shard = atoi(argv[3]);
    shards = atoi(argv[4]);
    try {
        core = new Core(argv[2], "");
    }
    catch (CoreError error) {
        printf("Couldn't start the core: %d\n", int(error));
        return 2;
    }
    for (int i = 0; i < 40; i++) core->runCore();
    ram = core->memory.ram;

    // Start the emulator
    if (uc_open(UC_ARCH_PPC, uc_mode(UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN), &uc) != UC_ERR_OK) {
        printf("Couldn't start Unicorn\n");
        return 2;
    }
    uc_ctl_set_cpu_model(uc, UC_CPU_PPC32_750_V3_1);
    uc_mem_map(uc, 0, 0x1000000, UC_PROT_ALL);
    uc_mem_map(uc, WINDOW, WINDOW_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, CODE, 0x4000000, UC_PROT_ALL);
    uc_mem_write(uc, COND_TABLE, Interpreter::condition, 256);
    uint32_t blr = swap32(0x4E800020);
    for (uint32_t a = FAKE; a < FAKE_INTERLEAVE + 0x10; a += 4) uc_mem_write(uc, a, &blr, 4);
    writeBe(EVENTS, EVENTS + 0x10);
    uc_hook hook;
    uc_hook_add(uc, &hook, UC_HOOK_CODE, (void*)hookFake, nullptr, FAKE, FAKE_INTERLEAVE + 8);

    // Get the real stubs in the real code area, for both CPUs
    std::mt19937 fill(1234);
    for (uint32_t i = 0; i < WINDOW_SIZE; i++) seed[i] = fill();
    memcpy(ram + (WINDOW & 0x3FFFFF), seed, WINDOW_SIZE);
    uc_mem_write(uc, WINDOW, seed, WINDOW_SIZE);
    rigSetup = setup;
    for (int cpu = 0; cpu < 2; cpu++) {
        Jit &jit = jitOf(cpu);
        jit.enable(true);
        jit.initCodegen();
        if (!jit.codeArea) {
            printf("Couldn't get a code area\n");
            return 2;
        }
    }
    codeBase = core->jit.codeArea;
    for (int cpu = 0; cpu < 2; cpu++) {
        Jit &jit = jitOf(cpu);
        jit.codeFast = true;
        writeWords(emuCode(jit.codeArea), jit.codeArea, jit.codeStart - jit.codeArea);
    }

    std::set<std::string> wanted;
    for (int i = 5; i < argc; i++) wanted.insert(argv[i]);
    auto run = [&](const char *name, void (*suite)()) {
        if (wanted.empty() || wanted.count(name)) suite();
    };
    run("thumb", suiteThumbOps);
    run("arm", suiteArmOps);
    run("forms", suiteArmForms);
    run("flags", suiteFlagGrid);
    run("conditions", suiteConditionGrid);
    run("lists", suiteBlockLists);
    run("blocks", suiteBlocks);
    run("links", suiteLinks);

    for (auto &entry: counts) printf("COUNT\t%s\t%llu\n", entry.first.c_str(), (unsigned long long)entry.second);
    for (auto &entry: fails) printf("FAILS\t%s\t%llu\n", entry.first.c_str(), (unsigned long long)entry.second);
    printf("DONE\n");
    fflush(stdout);
    _exit(fails.empty() ? 0 : 1);
}
