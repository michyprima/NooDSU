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
#include <unicorn/arm.h>

extern void (*rigSetup)(Jit &jit);
uint32_t rigArm(uint32_t op);
int rigKind(uint32_t op, bool thumb, bool arm7);
bool rigMayJump(uint32_t op, bool thumb);
bool rigKindJumps(int kind);
extern int rigPoison;
extern uint64_t rigFlagStats[2];

enum { NK_NONE, NK_ALU, NK_BRANCH, NK_TRANSFER, NK_BRANCH_REG, NK_HALF, NK_BLOCK, NK_BLOCK_PC, NK_SWAP, NK_MULTIPLY, NK_MISC,
    NK_COPROCESSOR, NK_MSR, NK_BLX, NK_TRANSFER_PC, NK_ALU_PC, NK_DSP, NK_THUMB_BRANCH = 100 };

// Addresses in the emulator
static const uint32_t ARM_STATE = 0x10000, CACHE_FLAG = 0x50000, CACHE = 0x50010, EVENTS = 0x70000, LIMIT = 0x71000,
    GLOBAL = 0x72000, STACK = 0x7F000, END = 0x80000, COND_TABLE = 0x200000, READ_MAP = 0x400000, WRITE_MAP = 0x800000,
    FAKE = 0x3F0000, FAKE_MEMORY = 0x3F0800, FAKE_HANDLER = 0x3F1000, FAKE_INTERLEAVE = 0x3F2000, FAKE_MSR = 0x3F1800, FAKE_RESTORE = 0x3F1840, MODE_BANKS = 0x3F3000, MATH = 0x3F3800, DTCM_REG = 0x3F4000, CODE = 0x10000000;
// The math registers as a big-endian host lays them out: three 64-bit values, the two control registers and the dirty flags
static const uint32_t MATH_NUMER = 0, MATH_DENOM = 8, MATH_PARAM = 16, MATH_DIV_CNT = 24, MATH_SQRT_CNT = 26, MATH_DIV_DIRTY = 28, MATH_SQRT_DIRTY = 29, MATH_GX_STAT = 32;
static const uint32_t OFF_REGS = 0x40, OFF_CPSR = 0x80, OFF_CYCLES = 0x84, OFF_SPSR = 0x88, OFF_BANK = 0xA0, OFF_SPSR_VALUE = 0xC0, OFF_IE = 0xD0, OFF_IRF = 0xD8, OFF_IME = 0xE0, OFF_EXTRA = 0x100;

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
static std::set<uint32_t> ranThumb[2], ranArm[2], ranNativeThumb[2], ranNativeArm[2];

struct State {
    uint32_t usr[16], bank[7], spsr, cpsr;
    uint32_t extra[9]; // R13, R14 and the SPSR of Supervisor, Abort and Undefined modes, which returns can switch to
    uint32_t views[3]; // Where R13, R14 and the SPSR are in the emulator, which returns change
    int mode; // What the case starts in: 0 is system, 1 is IRQ, with its own R13 and R14, 2 is FIQ, with its own R8-R14
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
    uint64_t ie, irf;
    int msrCalls, msrWanted, restoreCalls, restoreWanted, mathCalls;
    bool mathLive; // Whether memory calls to the math registers go to the real ones, with the emulator's copy kept in step
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
    uint32_t words[(OFF_EXTRA + 9 * 4) / 4];
    uc_mem_read(uc, ARM_STATE, words, sizeof(words));
    for (int i = 0; i < 9; i++) s.extra[i] = swap32(words[OFF_EXTRA / 4 + i]);
    s.views[0] = swap32(words[OFF_REGS / 4 + 13]);
    s.views[1] = swap32(words[OFF_REGS / 4 + 14]);
    s.views[2] = swap32(words[OFF_SPSR / 4]);
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
    for (int i = 0; i < 9; i++)
        if (got.extra[i] != want.extra[i]) n += snprintf(text + n, sizeof(text) - n, "mode bank %d %08X want %08X; ", i, got.extra[i], want.extra[i]);
    for (int i = 0; i < 3; i++)
        if (got.views[i] != want.views[i]) n += snprintf(text + n, sizeof(text) - n, "%s at %08X want %08X; ", i == 2 ? "SPSR" : i ? "R14" : "R13", got.views[i], want.views[i]);
    text[n] = 0;
    return text;
}

static void scheduleEvent() {
    // Something C++ did made an event due now, which generated code has to notice at its next check
    if (now.stopMode != STOP_SCHEDULED) return;
    writeBe(EVENTS + 0x10 + offsetof(SchedEvent, cycles), 0);
    now.scheduled = true;
}

// The math registers in the emulator, as a big-endian host lays them out
static void putMath(const DivSqrt &div) {
    auto put8 = [&](uint32_t at, uint64_t v) { writeBe(MATH + at, v >> 32); writeBe(MATH + at + 4, uint32_t(v)); };
    put8(MATH_NUMER, div.divNumer);
    put8(MATH_DENOM, div.divDenom);
    put8(MATH_PARAM, div.sqrtParam);
    uint8_t bytes[] = { uint8_t(div.divCnt >> 8), uint8_t(div.divCnt), uint8_t(div.sqrtCnt >> 8), uint8_t(div.sqrtCnt),
        uint8_t(div.divDirty), uint8_t(div.sqrtDirty) };
    uc_mem_write(uc, MATH + MATH_DIV_CNT, bytes, sizeof(bytes));
}

static void getMath(DivSqrt &div) {
    auto get8 = [&](uint32_t at) { return (uint64_t(readBe(MATH + at)) << 32) | readBe(MATH + at + 4); };
    div.divNumer = get8(MATH_NUMER);
    div.divDenom = get8(MATH_DENOM);
    div.sqrtParam = get8(MATH_PARAM);
    uint8_t bytes[6];
    uc_mem_read(uc, MATH + MATH_DIV_CNT, bytes, sizeof(bytes));
    div.divCnt = (bytes[0] << 8) | bytes[1];
    div.sqrtCnt = (bytes[2] << 8) | bytes[3];
    div.divDirty = bytes[4];
    div.sqrtDirty = bytes[5];
}

static void hookFake(uc_engine *, uint64_t address, uint32_t, void *) {
    if (address >= FAKE && address < FAKE_MEMORY) {
        // Memory access from the slow path: load32, load8, store32, store8, load16, store16
        int type = (address - FAKE) / 8;
        count("slow memory access");
        uint32_t target = reg(UC_PPC_REG_4), value = reg(UC_PPC_REG_5);
        if (reg(UC_PPC_REG_3) != FAKE_MEMORY) fail("memory call without the memory object: %08X", reg(UC_PPC_REG_3));
        if ((target >> 12) == 0x4000) { // What the math suite leaves to C++
            now.mathCalls++;
            uint32_t loaded = 0xDEAD0003;
            if (now.mathLive) {
                DivSqrt &div = core->divSqrt;
                Memory &mem = core->memory;
                getMath(div);
                switch (type) {
                    case 0: {
                        loaded = mem.read<uint32_t>(0, target);
                        int shift = (target & 3) * 8;
                        if (shift) loaded = (loaded << (32 - shift)) | (loaded >> shift);
                        break;
                    }
                    case 1: loaded = mem.read<uint8_t>(0, target); break;
                    case 2: mem.write<uint32_t>(0, target, value); break;
                    case 3: mem.write<uint8_t>(0, target, value); break;
                    case 4: loaded = mem.read<uint16_t>(0, target); break;
                    case 5: mem.write<uint16_t>(0, target, value); break;
                }
                putMath(div);
            }
            clobberVolatile(true);
            setReg(UC_PPC_REG_3, loaded);
            return;
        }
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
    else if (address == FAKE_MSR) {
        // An MSR that does what only the interpreter can, like changing the CPU's mode; the registers don't change here
        count("msr slow path");
        now.msrCalls++;
        uint32_t bits = reg(UC_PPC_REG_4), value = reg(UC_PPC_REG_5), cpsr = readBe(ARM_STATE + OFF_CPSR);
        if (reg(UC_PPC_REG_3) != ARM_STATE) fail("MSR called without the CPU: %08X", reg(UC_PPC_REG_3));
        if (readBe(GLOBAL) != reg(UC_PPC_REG_28) || readBe(ARM_STATE + OFF_CYCLES) != reg(UC_PPC_REG_28))
            fail("MSR called without saved time");
        if ((bits & BIT(16)) && (cpsr & 0x1F) != 0x10) cpsr = (cpsr & ~0xFF) | (value & 0xFF) | 0x10;
        if (bits & BIT(19)) cpsr = (cpsr & 0x00FFFFFF) | (value & (now.cpu ? 0xF0000000 : 0xF8000000));
        writeBe(ARM_STATE + OFF_CPSR, cpsr);
        scheduleEvent();
        clobberVolatile(false);
    }
    else if (address == FAKE_RESTORE) {
        // Restoring the CPSR in a way only the interpreter can, like with no SPSR or an interrupt to take
        count("restore slow path");
        now.restoreCalls++;
        uint32_t before = reg(UC_PPC_REG_4), spsr = readBe(ARM_STATE + OFF_SPSR);
        if (reg(UC_PPC_REG_3) != ARM_STATE) fail("restore called without the CPU: %08X", reg(UC_PPC_REG_3));
        if (before != readBe(ARM_STATE + OFF_CPSR)) fail("restore called with CPSR %08X, want %08X", before, readBe(ARM_STATE + OFF_CPSR));
        if (readBe(GLOBAL) != reg(UC_PPC_REG_28) || readBe(ARM_STATE + OFF_CYCLES) != reg(UC_PPC_REG_28))
            fail("restore called without saved time");
        uint32_t after = spsr ? readBe(spsr) : (now.cpu ? before : (before & 0xFF));
        writeBe(ARM_STATE + OFF_CPSR, after);

        // The interpreter swaps the registers too; FIQ and invalid modes are left out of this test
        uint32_t entry = MODE_BANKS + (after & 0x1F) * 16;
        if (spsr && ((after ^ before) & 0x1F) && (before & 0x1F) != 0x11 && readBe(entry)) {
            writeBe(ARM_STATE + OFF_REGS + 13 * 4, readBe(entry));
            writeBe(ARM_STATE + OFF_REGS + 14 * 4, readBe(entry + 4));
            writeBe(ARM_STATE + OFF_SPSR, readBe(entry + 8));
        }
        scheduleEvent();
        clobberVolatile(false);
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

// The interpreter's banked registers during a case, laid out like the emulator's
static uint32_t oracleBank[7], oracleSpsr, oracleExtra[9];

static void loadOracle(Interpreter &arm, State &s) {
    for (int i = 0; i < 16; i++) arm.registersUsr[i] = s.usr[i];
    for (int i = 0; i < 7; i++) oracleBank[i] = s.bank[i];
    for (int i = 0; i < 9; i++) oracleExtra[i] = s.extra[i];
    oracleSpsr = s.spsr;
    for (int i = 0; i < 16; i++) arm.registers[i] = banked(s.mode, i) ? &oracleBank[i - 8] : &arm.registersUsr[i];
    arm.spsr = s.mode ? &oracleSpsr : nullptr;
    arm.cpsr = s.cpsr;
    arm.lazyPipeline = true;
}

static uint32_t emuSlot(Interpreter &arm, uint32_t *p) {
    // Where the emulator keeps what a pointer in the interpreter points to during a case
    if (!p) return 0;
    if (p >= arm.registersUsr && p < arm.registersUsr + 16) return ARM_STATE + uint32_t(p - arm.registersUsr) * 4;
    if (p >= oracleBank && p < oracleBank + 7) return ARM_STATE + OFF_BANK + uint32_t(p - oracleBank) * 4;
    if (p >= oracleExtra && p < oracleExtra + 9) return ARM_STATE + OFF_EXTRA + uint32_t(p - oracleExtra) * 4;
    return ARM_STATE + OFF_SPSR_VALUE;
}

static void saveOracle(Interpreter &arm, State &s) {
    for (int i = 0; i < 16; i++) s.usr[i] = arm.registersUsr[i];
    for (int i = 0; i < 7; i++) s.bank[i] = oracleBank[i];
    for (int i = 0; i < 9; i++) s.extra[i] = oracleExtra[i];
    s.spsr = oracleSpsr;
    s.cpsr = arm.cpsr;
    s.views[0] = emuSlot(arm, arm.registers[13]);
    s.views[1] = emuSlot(arm, arm.registers[14]);
    s.views[2] = emuSlot(arm, arm.spsr);
}

static uint32_t emuBank(Interpreter &arm, uint32_t value) {
    // Where the emulator keeps what an entry in the JIT's real table of banked registers points to; the case's IRQ registers
    // are the ones it starts with
    auto is = [&](uint32_t &field) { return value == uint32_t(uintptr_t(&field)); };
    if (!value) return 0;
    if (is(arm.registersUsr[13])) return ARM_STATE + 13 * 4;
    if (is(arm.registersUsr[14])) return ARM_STATE + 14 * 4;
    if (is(arm.registersIrq[0])) return ARM_STATE + OFF_BANK + 5 * 4;
    if (is(arm.registersIrq[1])) return ARM_STATE + OFF_BANK + 6 * 4;
    if (is(arm.spsrIrq)) return ARM_STATE + OFF_SPSR_VALUE;
    uint32_t *fields[9] = { &arm.registersSvc[0], &arm.registersSvc[1], &arm.spsrSvc, &arm.registersAbt[0], &arm.registersAbt[1], &arm.spsrAbt,
        &arm.registersUnd[0], &arm.registersUnd[1], &arm.spsrUnd };
    for (int i = 0; i < 9; i++)
        if (is(*fields[i])) return ARM_STATE + OFF_EXTRA + i * 4;
    fail("the JIT's table of banked registers has %08X", value);
    return 0;
}

static void bankOracle(Interpreter &arm, uint32_t mode) {
    // Point the interpreter at the registers of a mode, the same way as the JIT's table
    int slot = (mode == 0x13) ? 0 : (mode == 0x17) ? 3 : (mode == 0x1B) ? 6 : -1;
    arm.registers[13] = (mode == 0x12) ? &oracleBank[5] : (slot < 0) ? &arm.registersUsr[13] : &oracleExtra[slot];
    arm.registers[14] = (mode == 0x12) ? &oracleBank[6] : (slot < 0) ? &arm.registersUsr[14] : &oracleExtra[slot + 1];
    arm.spsr = (mode == 0x12) ? &oracleSpsr : (slot < 0) ? nullptr : &oracleExtra[slot + 2];
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
    saveOracle(arm, s);
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
        else if (pass && ((kind == NK_MSR && !(op & BIT(22))) || (!block->thumb && kind == NK_ALU_PC && (op & BIT(20))))) {
            // The interpreter's way changes which registers are visible and can schedule an interrupt, which the test leaves
            // out; generated code has to call it whenever that could happen, or for a return swap the same registers itself
            uint32_t before = arm.cpsr, *registers[16], *spsr = arm.spsr;
            uint8_t ime = arm.ime;
            memcpy(registers, arm.registers, sizeof(registers));
            arm.ime = 0;
            cost = (arm.*Interpreter::armInstrs[((op >> 16) & 0xFF0) | ((op >> 4) & 0xF)])(op);
            arm.ime = ime;
            memcpy(arm.registers, registers, sizeof(registers));
            arm.spsr = spsr;
            if (kind == NK_MSR) {
                bool control = (op & BIT(16)) && (before & 0x1F) != 0x10;
                if (control && (((arm.cpsr ^ before) & 0x3F) || (!(arm.cpsr & BIT(7)) && (now.ie & now.irf)))) now.msrWanted++;
            }
            else {
                uint32_t mode = arm.cpsr & 0x1F;
                bool known = mode == 0x10 || mode == 0x1F || mode == 0x12 || mode == 0x13 || mode == 0x17 || mode == 0x1B;
                bool swapped = spsr && ((arm.cpsr ^ before) & 0x1F);
                if (!spsr || (!(arm.cpsr & BIT(7)) && (now.ie & now.irf)) || (swapped && ((before & 0x1F) == 0x11 || !known)))
                    now.restoreWanted++;
                if (swapped && (before & 0x1F) != 0x11 && known) bankOracle(arm, mode);
                jumped = true;
            }
        }
        else if (pass) {
            cost = block->thumb ? (arm.*Interpreter::thumbInstrs[(op >> 6) & 0x3FF])(op) : (op >> 28) == 0xF ? arm.handleReserved(op) :
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
            // A jump to the next instruction just carries on, since generated code sees the same program counter,
            // except for branches, which always go through their links
            if (next == address + step && thumb == block->thumb && index < block->instrs.size() && kind != NK_BRANCH &&
                kind != NK_THUMB_BRANCH) continue;
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
    now.msrCalls = now.msrWanted = now.restoreCalls = now.restoreWanted = 0;
    static std::mt19937 interrupts(4321);
    now.ie = uint64_t(1) << (interrupts() % 64);
    now.irf = (interrupts() % 4) ? 0 : now.ie;
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
    uint32_t words[(OFF_EXTRA + 9 * 4) / 4] = {};
    for (int i = 0; i < 16; i++) words[i] = swap32(c.in.usr[i]);
    for (int i = 0; i < 16; i++)
        words[OFF_REGS / 4 + i] = swap32(banked(c.in.mode, i) ? (ARM_STATE + OFF_BANK + (i - 8) * 4) : (ARM_STATE + i * 4));
    words[OFF_CPSR / 4] = swap32(c.in.cpsr);
    words[OFF_CYCLES / 4] = swap32(START_CYCLES);
    words[OFF_SPSR / 4] = swap32(c.in.mode ? (ARM_STATE + OFF_SPSR_VALUE) : 0);
    for (int i = 0; i < 7; i++) words[OFF_BANK / 4 + i] = swap32(c.in.bank[i]);
    words[OFF_SPSR_VALUE / 4] = swap32(c.in.spsr);

    for (int i = 0; i < 9; i++) words[OFF_EXTRA / 4 + i] = swap32(c.in.extra[i]);

    // The JIT's real table of banked registers, with its host pointers turned into where the emulator has them
    Interpreter &real = core->interpreter[c.cpu];
    for (uint32_t mode = 0; mode < 32; mode++)
        for (int field = 0; field < 3; field++)
            writeBe(MODE_BANKS + mode * 16 + field * 4, emuBank(real, jit.modeBanks[mode][field]));
    words[OFF_IE / 4] = swap32(now.ie >> 32);
    words[OFF_IE / 4 + 1] = swap32(now.ie);
    words[OFF_IRF / 4] = swap32(now.irf >> 32);
    words[OFF_IRF / 4 + 1] = swap32(now.irf);
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
            if (now.msrCalls != now.msrWanted) fail("%d MSR calls, want %d", now.msrCalls, now.msrWanted);
            if (now.restoreCalls != now.restoreWanted) fail("%d CPSR restore calls, want %d", now.restoreCalls, now.restoreWanted);
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
    for (int i = 0; i < 9; i++) s.extra[i] = interesting();
    s.spsr = interesting();
    s.usr[15] = 0;
    s.mode = mode;
    s.cpsr = (mode == 0 ? (rnd(2) ? 0x1F : 0x10) : mode == 1 ? 0x12 : 0x11) | (thumb ? 0x20 : 0) | (rnd(16) << 28) | (rnd(2) ? BIT(7) : 0);
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
        if (kind == NK_TRANSFER || kind == NK_TRANSFER_PC) {
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
    return kind == NK_TRANSFER || kind == NK_TRANSFER_PC || kind == NK_HALF || kind == NK_BLOCK || kind == NK_BLOCK_PC || kind == NK_SWAP;
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
        case NK_COPROCESSOR: return "coprocessor";
        case NK_MSR: return "msr";
        case NK_BLX: return "blx";
        case NK_TRANSFER_PC: return "transfer pc";
        case NK_ALU_PC: return "alu pc";
        case NK_DSP: return "dsp";
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
        uint32_t position = thumb ? op : (((((op >> 16) & 0xFF0) | ((op >> 4) & 0xF)) << 4) | (op >> 28));
        (thumb ? ranThumb : ranArm)[cpu].insert(position);
        if (kind) (thumb ? ranNativeThumb : ranNativeArm)[cpu].insert(position);
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

static void testSequence(const std::string &suite, int cpu, bool thumb, const std::vector<uint32_t> &seq, const State &s,
        int memoryMode, int stopMode, size_t enterAt = 0) {
    // A short block, run three times: with the flags the JIT leaves out as they were, then forced to 0, then to 1 from
    // where it stops keeping them. A flag that's wrongly left out then has the wrong value in at least one of those,
    // whatever its old value was, and shows in whatever reads it or in the state at the next stop
    uint32_t address = codeAddress(thumb), step = thumb ? 2 : 4;
    std::vector<uint32_t> ops = seq;
    ops.push_back(terminator(thumb, address + seq.size() * step, 64));
    for (int poison = 0; poison < 3; poison++) {
        rigPoison = poison;
        clearCode(address, 64);
        putCode(address, ops.data(), ops.size(), thumb);
        JitBlock *host = compileAt(cpu, address, thumb), *block = host;
        if (host && enterAt > 0 && enterAt < host->instrs.size()) block = compileAt(cpu, address + enterAt * step, thumb);
        if (block) {
            Case c;
            c.name = suite;
            c.cpu = cpu;
            c.in = s;
            c.memoryMode = memoryMode;
            c.stopMode = stopMode;
            c.blocks = { block };
            if (block != host) c.blocks.push_back(host);
            c.fakeTarget = WINDOW + 0x3F008;
            runCase(c);
            count("suite:" + suite);
        }
        restoreRam();
        finishBlocks(cpu);
    }
    rigPoison = 0;
}

// Suites

static bool mine(uint64_t index) { return index % shards == uint64_t(shard); }

static void suiteThumbOps() {
    // Every THUMB encoding on both CPUs, in each kind of memory, in each CPU mode
    for (int cpu = 0; cpu < 2; cpu++) {
        for (uint32_t op = 0; op < 0x10000; op++) {
            if (!mine(op)) continue;
            rng.seed(op * 2 + cpu + 1000);
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

    // The test can't see a halt or a remap, so check that what does those stays with the interpreter
    now.name = "arm forms";
    now.description = "kinds";
    for (uint32_t op: { 0xEE070F90, 0xEE070F58, 0xEE010F10, 0xEE090F11, 0xEE090F31 })
        if (rigKind(op, false, false) != NK_NONE) fail("%08X isn't left to the interpreter", op);
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
    for (int i = 0; i < 20000; i++) {
        // Halfword multiplies, QADD and QSUB, whose random values often overflow
        ops.push_back(0x01000080 | ((fields() & 3) << 21) | ((fields() & 3) << 5) | (r4() << 16) | (r4() << 12) | (r4() << 8) | r4());
        ops.push_back(0x01000050 | ((fields() & 1) << 21) | (r4() << 16) | (r4() << 12) | r4());
    }
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
    for (int i = 0; i < 20000; i++) {
        // MSR to the CPSR with a value that keeps the mode, which random values rarely do, for every combination of fields
        if (!mine(index++)) continue;
        rng.seed(index + 90000);
        int mode = i % 3, cpu = (i >> 2) & 1;
        State s = randomState(false, mode);
        uint32_t rm = rnd(15), fieldBits = (i & 8 ? BIT(16) : 0) | (i & 16 ? BIT(19) : 0);
        visible(s, rm) = (rng() & 0xFFFFFF00) | (s.cpsr & 0x1F) | (rng() & 0xC0);
        testOneOp("arm forms", cpu, false, 0xE120F000 | fieldBits | rm, mode, MEMORY_MAPPED, &s);
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
    for (uint32_t op: { 0xE0100291, 0xE0303291, 0xE0930291, 0xE0D30291, 0xE0B30291, 0xE0F30291 })
        ops.push_back({ false, op }); // MULS, MLAS, UMULLS, SMULLS, UMLALS, SMLALS

    // Each is also followed by something that makes the JIT leave out some of its flags: a compare, so none are needed;
    // ADCS, so only the carry is; ANDS, so N and Z aren't; and a shift, so only V is
    static const uint32_t after[2][4] = { { 0xE1550006, 0xE0B54006, 0xE0154006, 0xE1B04085 }, { 0x42B5, 0x416C, 0x402C, 0x006C } };
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
                        for (int next = 0; next < 4; next++)
                            testSequence("flag grid, then another", cpu, entry.first, { entry.second, after[entry.first][next] }, s,
                                MEMORY_MAPPED, STOP_NEVER);
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

        // The same with user registers, as a thread switch saves and restores them
        forms.push_back({ false, ((bits & ~0x00200000u) | 0x00400000u) | (list & 0x7FFF) });
    }
    // Every LDM/STM table position, with a base that isn't in the list so both CPUs have native code for it
    std::mt19937 lists(4321);
    for (uint32_t position = 0; position < 0x200; position++) {
        for (int i = 0; i < 8; i++) {
            uint32_t low = position & 0xF, top = position >> 4;
            uint32_t rn = lists() % 11; // Not one of the registers this position's list needs
            rn = (rn < 4) ? rn : (rn + 4);
            uint32_t list = ((lists() & 0xFF0F) | (low << 4)) & ~BIT(rn);
            if (!list) list = BIT((rn + 1) % 4);
            forms.push_back({ false, 0xE8000000 | (top << 20) | (rn << 16) | list });
        }
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
    switch (rnd(24)) {
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
        case 21: { // LDM and STM of the user registers
            uint32_t l = list();
            op = 0x08400000 | (rnd(2) << 24) | (rnd(2) << 23) | (rnd(2) << 20) | (base() << 16) | (l ? l : 1);
            break;
        }
        case 22: // A return to the next instruction, or the one after, restoring the CPSR
            op = rnd(4) ? 0x025FF004 : rnd(2) ? 0x025FF000 : 0x01B0F00F;
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

static void usableBanks(State &s) {
    // Make every mode's stack pointer usable and the saved state one a return can really go to, as blocks can switch modes
    static const uint32_t modes[] = { 0x10, 0x1F, 0x12, 0x13, 0x17, 0x1B, 0x11 };
    auto stack = []() { return (DATA_START + 0x1000 + rnd(0xE000)) & ~3u; };
    s.usr[13] = stack();
    s.bank[5] = stack();
    for (int i = 0; i < 9; i += 3) s.extra[i] = stack();
    auto saved = [&]() { return (rnd(16) << 28) | modes[rnd(7)] | (rnd(8) ? 0 : BIT(5)) | (rnd(2) ? BIT(7) : 0); };
    s.spsr = saved();
    for (int i = 2; i < 9; i += 3) s.extra[i] = saved();
}

static void suiteBlocks() {
    // Random blocks entered at the start or the middle, stopping at checks or not, in each kind of memory
    const uint64_t total = 400000;
    for (uint64_t index = 0; index < total; index++) {
        if (!mine(index)) continue;
        rng.seed(index + 40000);
        rigPoison = index % 3; // The flags the JIT leaves out as they were, cleared or set, see testSequence
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
        usableBanks(s);
        s.usr[7] = (DATA_START + 0x1000 + rnd(0xE000)) & ~3u;

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
    rigPoison = 0;
}

// Instructions for the flag sequences, with fixed registers so that what one leaves is what another reads: R0 and R2 for
// results that set flags, R4 for ones that read them, R6 for ones that do neither, R7 for a base and R8 or R3 for shifts
static const uint32_t FLAG_ARM[] = {
    0xE0900001, 0xE0500001, 0xE2700005, 0xE1500001, 0xE3700001, // ADDS, SUBS, RSBS, CMP, CMN: every flag
    0xE0B22003, 0xE0D22003, 0xE0F22003, // ADCS, SBCS, RSCS: every flag, from the carry
    0xE0100001, 0xE0300201, 0xE1B000A1, 0xE1B00041, // ANDS, EORS with LSL, MOVS with LSR and ASR #32: N and Z, or C too
    0xE1B00061, 0xE1B00811, 0xE1900871, // MOVS with RRX, and by a register, which keeps the carry if it's 0
    0xE3B0020F, 0xE3B00003, 0xE1100001, 0xE1300081, // MOVS of a rotated and a plain immediate, TST, TEQ with LSL
    0xE0100391, 0xE0920391, 0xE0F20391, // MULS, UMULLS, SMLALS: N and Z
    0x10900001, 0x21B00121, 0x01500001, 0x30D22003, // ADDSNE, MOVSCS, CMPEQ, SBCSCC: only sometimes
    0xE0A44005, 0xE0C44005, 0xE2E44000, 0xE1A04065, 0xE10F4000, // ADC, SBC, RSC, MOV with RRX, MRS: readers
    0x03A04001, 0x13A04002, 0x23A04003, 0x33A04004, 0x43A04005, 0x53A04006, 0x63A04007, // MOV on each condition
    0x73A04008, 0x83A04009, 0x93A0400A, 0xA3A0400B, 0xB3A0400C, 0xC3A0400D, 0xD3A0400E,
    0xE1A06001, 0xE2866001, 0xE5976000, 0xE16F6F11, // MOV, ADD, LDR, CLZ: no flags
    0xE5876004, 0xE1C760D0, 0xE128F005, // STR, LDRD through a handler, MSR of the flags: not in a run
    0x1A00000E, 0x2A00000E, 0x4A00000E, 0x6A00000E, 0x8A00000E, 0xAA00000E, 0xCA00000E, // Branches that can leave
};
static const uint32_t FLAG_THUMB[] = {
    0x00C8, 0x0008, 0x0848, 0x1008, // LSLS #3 and #0, LSRS #1, ASRS #32
    0x1840, 0x1A40, 0x1C48, 0x1FC8, 0x2005, 0x2803, 0x30C8, 0x3A01, // ADDS, SUBS, MOVS, CMP with registers and immediates
    0x4008, 0x4048, 0x4098, 0x40D8, 0x4118, 0x414A, 0x418A, 0x41D8, // ANDS, EORS, LSLS, LSRS, ASRS, ADCS, SBCS, RORS
    0x4208, 0x4248, 0x4288, 0x42C8, 0x4308, 0x4348, 0x4388, 0x43C8, // TST, NEGS, CMP, CMN, ORRS, MULS, BICS, MVNS
    0x444E, 0x4548, 0x464E, 0x683E, 0x4E00, 0xAE00, // ADD, CMP and MOV with a high register, LDR, LDR from the PC, ADD from SP
    0x607E, 0xB410, // STR, PUSH: not in a run
    0xD010, 0xD110, 0xD210, 0xD310, 0xD410, 0xD610, 0xD810, 0xDA10, 0xDC10, // Branches that can leave
};

static State flagState(bool thumb) {
    // Somewhere to load from and push to, and shift amounts around the ones that matter
    static const uint32_t amounts[] = { 0, 0, 1, 5, 31, 32, 33, 64, 255, 256, 0x80000001 };
    State s = randomState(thumb, 0);
    s.usr[7] = (DATA_START + 0x1000 + rnd(0xE000)) & ~3u;
    s.usr[13] = (DATA_START + 0x1000 + rnd(0xE000)) & ~3u;
    s.usr[thumb ? 3 : 8] = amounts[rnd(sizeof(amounts) / sizeof(amounts[0]))];
    return s;
}

static void suiteLiveness() {
    // Which flags the JIT works out: every run of three of the instructions above, which between them set, keep and
    // read each flag in every way the JIT tells apart, then longer random ones, entered in the middle sometimes
    uint64_t index = 0;
    for (int thumb = 0; thumb < 2; thumb++) {
        const uint32_t *set = thumb ? FLAG_THUMB : FLAG_ARM;
        size_t size = thumb ? sizeof(FLAG_THUMB) / sizeof(FLAG_THUMB[0]) : sizeof(FLAG_ARM) / sizeof(FLAG_ARM[0]);
        for (size_t a = 0; a < size; a++) {
            for (size_t b = 0; b < size; b++) {
                for (size_t c = 0; c < size; c++) {
                    if (!mine(index++)) continue;
                    rng.seed(index + 70000);
                    for (int cpu = 0; cpu < 2; cpu++)
                        testSequence(thumb ? "thumb flag triples" : "arm flag triples", cpu, thumb, { set[a], set[b], set[c] },
                            flagState(thumb), rnd(3), rnd(2) ? STOP_ALWAYS : STOP_NEVER);
                }
            }
        }
        for (uint64_t i = 0; i < 120000; i++) {
            if (!mine(index++)) continue;
            rng.seed(index + 70000);
            std::vector<uint32_t> seq;
            int length = 4 + rnd(5);
            for (int k = 0; k < length; k++) seq.push_back(set[rnd(size)]);
            int cpu = rnd(4) == 0;
            testSequence(thumb ? "thumb flag runs" : "arm flag runs", cpu, thumb, seq, flagState(thumb), rnd(3),
                rnd(2) ? STOP_ALWAYS : STOP_NEVER, rnd(3) ? 0 : 1 + rnd(length - 1));
        }
    }

    // What the JIT works out for a few blocks, directly; MRS ends a block when one is compiled, so only this shows that
    // it counts as reading the flags. Each is the flags read after every instruction but the branch that ends it
    static const struct { std::vector<uint32_t> ops; std::vector<uint8_t> live; } known[] = {
        { { 0xE0900001, 0xE0100001, 0xE0A44005, 0xE1500001, 0xEA000000 }, { 0x2, 0x2, 0x0, 0xF } }, // ADDS, ANDS, ADC, CMP
        { { 0xE0900001, 0xE10F4000, 0xE1500001, 0xEA000000 }, { 0xF, 0x0, 0xF } }, // ADDS, MRS, CMP
        { { 0xE0900001, 0xE1B00081, 0xEA000000 }, { 0x1, 0xF } }, // ADDS, MOVS with LSL
        { { 0xE0900001, 0xE5876004, 0xE1500001, 0xEA000000 }, { 0xF, 0xF, 0xF } }, // ADDS, STR, CMP
        { { 0xE0900001, 0x03A04001, 0xE1500001, 0xEA000000 }, { 0xF, 0x0, 0xF } }, // ADDS, MOVEQ, CMP
    };
    if (shard == 0) {
        for (auto &entry: known) {
            for (int cpu = 0; cpu < 2; cpu++) {
                JitBlock *block = new JitBlock();
                block->thumb = false;
                for (uint32_t op: entry.ops) {
                    JitInstr instr;
                    instr.opcode = op;
                    block->instrs.push_back(instr);
                }
                Jit &jit = jitOf(cpu);
                jit.findLiveFlags(block);
                now.name = "flags read after";
                now.description = "block starting " + std::to_string(entry.ops[0]);
                for (size_t i = 0; i < entry.live.size(); i++)
                    if (jit.liveFlags[i] != entry.live[i]) fail("instruction %d has %X, want %X", int(i), jit.liveFlags[i], entry.live[i]);
                count("suite:flags read after");
                delete block;
            }
        }
    }

    // A load whose offset is rotated through the carry reads it too: with the carry known to be set or clear, the address
    // is only in the test's memory if the load sees the right one
    static const uint32_t loads[] = { 0xE7974065, 0xE7D74065, 0xE7B74065, 0xE7374065, 0xE6974065 }; // LDR, LDRB, writeback forms
    for (int known = 0; known < 2; known++) {
        for (uint32_t load: loads) {
            for (int cpu = 0; cpu < 2; cpu++) {
                if (!mine(index++)) continue;
                rng.seed(index + 70000);
                State s = flagState(false);
                bool down = !(load & BIT(23)), post = !(load & BIT(24));
                uint32_t offset = known ? 0x80000004 : 4;
                s.usr[5] = 8;
                if (!post) s.usr[7] = down ? (s.usr[7] + offset) : (s.usr[7] - offset);
                // CMP R0,R0 always sets the carry, CMN R0,#0 always clears it, and the CMP after the load sets it again
                testSequence("flag rotated loads", cpu, false, { known ? 0xE1500000 : 0xE3700000, load, 0xE1510001 }, s,
                    MEMORY_MAPPED, STOP_NEVER);
            }
        }
    }
}

static void suiteLinks() {
    // Blocks jumping to each other directly and through the dispatcher, as they're compiled and dropped
    const uint64_t total = 30000;
    for (uint64_t index = 0; index < total; index++) {
        if (!mine(index)) continue;
        rng.seed(index + 60000);
        rigPoison = index % 3;
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
                bool jumps = rigKindJumps(kind) || kind == NK_THUMB_BRANCH;
                if (!jumps && !jitOf(cpu).endsBlock(op, thumb) && !rigMayJump(op, thumb)) return op;
            }
        };
        for (int i = 0; i < size; i++) a.push_back(safe());
        uint32_t end = addressA + a.size() * step;
        int ending = rnd(thumb ? 3 : 4);
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
        else if (ending == 3) { // A return through the dispatcher
            a.push_back(0xE256F000); // SUBS PC, R6, #0
            count("links from returns");
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
                if (!rigKindJumps(kind) && kind != NK_THUMB_BRANCH &&
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
        usableBanks(s);
        s.spsr &= ~BIT(5);
        s.usr[7] = (DATA_START + 0x1000 + rnd(0xE000)) & ~3u;
        s.usr[6] = addressB | ((otherMode ? !thumb : thumb) ? 1 : 0);
        if (!thumb && !otherMode) s.usr[6] = addressB & ~3u;

        auto run = [&](const std::vector<JitBlock*> &blocks, const char *name) {
            // Without anything in the block cache, only a real link can reach a block directly branched to
            Case c;
            c.useCache = ending >= 2;
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

    // ARM BLX label only links to THUMB, so an ARM block at its target has to be left alone, and a THUMB one used
    for (uint64_t index = 0; index < 4000; index++) {
        if (!mine(index)) continue;
        rng.seed(index + 65000);
        rigPoison = index % 3;
        uint32_t page = CODE_START + 0x1000 * rnd(12), addressA = page + 0x100, addressB = page + 0x180;
        clearCode(page, 0x1000);
        uint32_t blx = 0xFA000000 | (((addressB - (addressA + 8)) >> 2) & 0xFFFFFF);
        uint32_t target[3] = { 0xE3A01000 | rnd(256), 0xE1A00000, terminator(false, addressB + 8, 32) };
        putCode(addressA, &blx, 1, false);
        putCode(addressB, target, 3, false);
        bool thumbTarget = index & 1;
        JitBlock *blockB = compileAt(0, addressB, thumbTarget);
        JitBlock *blockA = compileAt(0, addressA, false);
        if (!blockA || !blockB) continue;
        Case c;
        c.name = "links: BLX";
        c.cpu = 0;
        c.in = randomState(false, rnd(3));
        c.blocks = { blockA, blockB };
        c.memoryMode = MEMORY_MAPPED;
        c.compareMemory = false;
        runCase(c);
        count("suite:links");
        restoreRam();
        finishBlocks(0);
    }
}

// The interpreter's handlers against Unicorn's ARM946, an independent model of the same CPU
// CP15, SWI, undefined encodings and timing are left out, since it doesn't model the DS for those

static uc_engine *ucArm;
static const uint32_t REF_MODES[] = { 0x10, 0x11, 0x12, 0x13, 0x17, 0x1B, 0x1F };

struct RefState {
    uint32_t usr[16], fiq[7], svc[2], abt[2], irq[2], und[2];
    uint32_t spsrFiq, spsrSvc, spsrAbt, spsrIrq, spsrUnd, cpsr;
};

static uint32_t *refBank(RefState &s, uint32_t mode, int reg) {
    // Where a register visible in a mode is kept
    switch (mode & 0x1F) {
        case 0x11: if (reg >= 8 && reg <= 14) return &s.fiq[reg - 8]; break;
        case 0x13: if (reg >= 13 && reg <= 14) return &s.svc[reg - 13]; break;
        case 0x17: if (reg >= 13 && reg <= 14) return &s.abt[reg - 13]; break;
        case 0x12: if (reg >= 13 && reg <= 14) return &s.irq[reg - 13]; break;
        case 0x1B: if (reg >= 13 && reg <= 14) return &s.und[reg - 13]; break;
    }
    return &s.usr[reg];
}

static uint32_t *refSpsr(RefState &s, uint32_t mode) {
    switch (mode & 0x1F) {
        case 0x11: return &s.spsrFiq;
        case 0x13: return &s.spsrSvc;
        case 0x17: return &s.spsrAbt;
        case 0x12: return &s.spsrIrq;
        case 0x1B: return &s.spsrUnd;
    }
    return nullptr;
}

static void loadInterpreter(Interpreter &arm, RefState &s) {
    for (int i = 0; i < 16; i++) arm.registers[i] = &arm.registersUsr[i];
    arm.cpsr = 0x1F;
    arm.spsr = nullptr;
    arm.lazyPipeline = true;
    arm.ime = 0;
    arm.swapRegisters(s.cpsr);
    arm.cpsr = s.cpsr;
    memcpy(arm.registersUsr, s.usr, sizeof(s.usr));
    memcpy(arm.registersFiq, s.fiq, sizeof(s.fiq));
    memcpy(arm.registersSvc, s.svc, sizeof(s.svc));
    memcpy(arm.registersAbt, s.abt, sizeof(s.abt));
    memcpy(arm.registersIrq, s.irq, sizeof(s.irq));
    memcpy(arm.registersUnd, s.und, sizeof(s.und));
    arm.spsrFiq = s.spsrFiq; arm.spsrSvc = s.spsrSvc; arm.spsrAbt = s.spsrAbt; arm.spsrIrq = s.spsrIrq; arm.spsrUnd = s.spsrUnd;
}

static void saveInterpreter(Interpreter &arm, RefState &s) {
    memcpy(s.usr, arm.registersUsr, sizeof(s.usr));
    memcpy(s.fiq, arm.registersFiq, sizeof(s.fiq));
    memcpy(s.svc, arm.registersSvc, sizeof(s.svc));
    memcpy(s.abt, arm.registersAbt, sizeof(s.abt));
    memcpy(s.irq, arm.registersIrq, sizeof(s.irq));
    memcpy(s.und, arm.registersUnd, sizeof(s.und));
    s.spsrFiq = arm.spsrFiq; s.spsrSvc = arm.spsrSvc; s.spsrAbt = arm.spsrAbt; s.spsrIrq = arm.spsrIrq; s.spsrUnd = arm.spsrUnd;
    s.cpsr = arm.cpsr;
}

static void armReg(int r, uint32_t value) { uc_reg_write(ucArm, r, &value); }
static uint32_t armRead(int r) { uint32_t value = 0; uc_reg_read(ucArm, r, &value); return value; }
static int armR(int i) { return i == 13 ? UC_ARM_REG_R13 : i == 14 ? UC_ARM_REG_R14 : i == 15 ? UC_ARM_REG_R15 : UC_ARM_REG_R0 + i; }

static void loadUnicorn(RefState &s) {
    for (uint32_t mode: REF_MODES) {
        armReg(UC_ARM_REG_CPSR, mode | 0xC0);
        for (int i = 8; i <= 14; i++) armReg(armR(i), *refBank(s, mode, i));
        if (uint32_t *spsr = refSpsr(s, mode)) armReg(UC_ARM_REG_SPSR, *spsr);
    }
    armReg(UC_ARM_REG_CPSR, s.cpsr);
    for (int i = 0; i < 8; i++) armReg(armR(i), s.usr[i]);
}

static void saveUnicorn(RefState &s, uint32_t &next) {
    next = armRead(UC_ARM_REG_R15);
    uint32_t cpsr = armRead(UC_ARM_REG_CPSR);
    s.cpsr = cpsr;
    for (int i = 0; i < 8; i++) s.usr[i] = armRead(armR(i));
    s.usr[15] = 0;
    for (uint32_t mode: REF_MODES) {
        armReg(UC_ARM_REG_CPSR, mode | 0xC0);
        for (int i = 8; i <= 14; i++) *refBank(s, mode, i) = armRead(armR(i));
        if (uint32_t *spsr = refSpsr(s, mode)) *spsr = armRead(UC_ARM_REG_SPSR);
    }
}

static const uint32_t REF_CPSR_MASK = 0xF80000FF; // NZCVQ, IFT and the mode, which are all the ARM946 has

static std::string diffRef(const RefState &got, const RefState &want) {
    char text[900];
    int n = 0;
    auto one = [&](const char *name, uint32_t a, uint32_t b) {
        if (a != b && n < 800) n += snprintf(text + n, sizeof(text) - n, "%s interp %08X, unicorn %08X; ", name, a, b);
    };
    static const char *names[] = { "r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12","r13","r14" };
    for (int i = 0; i < 15; i++) one(names[i], got.usr[i], want.usr[i]);
    for (int i = 0; i < 7; i++) one(names[8 + i], got.fiq[i], want.fiq[i]);
    for (int i = 0; i < 2; i++) { one(i ? "svc r14" : "svc r13", got.svc[i], want.svc[i]); one(i ? "abt r14" : "abt r13", got.abt[i], want.abt[i]);
        one(i ? "irq r14" : "irq r13", got.irq[i], want.irq[i]); one(i ? "und r14" : "und r13", got.und[i], want.und[i]); }
    // Reserved bits may or may not be kept
    one("spsr fiq", got.spsrFiq & REF_CPSR_MASK, want.spsrFiq & REF_CPSR_MASK); one("spsr svc", got.spsrSvc & REF_CPSR_MASK, want.spsrSvc & REF_CPSR_MASK);
    one("spsr abt", got.spsrAbt & REF_CPSR_MASK, want.spsrAbt & REF_CPSR_MASK);
    one("spsr irq", got.spsrIrq & REF_CPSR_MASK, want.spsrIrq & REF_CPSR_MASK); one("spsr und", got.spsrUnd & REF_CPSR_MASK, want.spsrUnd & REF_CPSR_MASK);
    one("cpsr", got.cpsr & REF_CPSR_MASK, want.cpsr & REF_CPSR_MASK);
    text[n] = 0;
    return text;
}

static int refKind(uint32_t a) {
    // How an ARM instruction touches memory, going by what the interpreter's table decodes
    if ((a & 0x0C000000) == 0x04000000) return NK_TRANSFER;
    if ((a & 0x0E000000) == 0x08000000) return NK_BLOCK;
    if ((a & 0x0FB000F0) == 0x01000090) return NK_SWAP;
    if ((a & 0x0E000090) == 0x00000090 && (a & 0x60)) return NK_HALF;
    return NK_NONE;
}

static std::string refClass(uint32_t op, bool thumb) {
    // The group a disagreement is listed under
    uint32_t a = thumb ? rigArm(op) : op;
    if (thumb) {
        if ((op & 0xF000) == 0xD000) return "THUMB conditional branch";
        if ((op & 0xF800) == 0xE000) return "THUMB B";
        if ((op & 0xF000) == 0xF000 || (op & 0xF800) == 0xE800) return "THUMB BL/BLX label";
        if ((op & 0xFF00) == 0x4700) return "THUMB BX/BLX";
        if (a >> 28 == 0xF) return "THUMB other";
    }
    const char *prefix = thumb ? "THUMB as " : "";
    if (a >> 28 == 0xF) return std::string(prefix) + "reserved condition";
    if ((a & 0x0E000000) == 0x0A000000) return std::string(prefix) + "B/BL";
    if ((a & 0x0FF000F0) == 0x01200010 || (a & 0x0FF000F0) == 0x01200030) return std::string(prefix) + "BX/BLX register";
    if ((a & 0x0FF000F0) == 0x01600010) return std::string(prefix) + "CLZ";
    if ((a & 0x0F9000F0) == 0x01000050) return std::string(prefix) + "QADD/QSUB";
    if ((a & 0x0F900090) == 0x01000080) return std::string(prefix) + "SMLAxy/SMULxy";
    if ((a & 0x0F900000) == 0x01000000 && (a & 0xF0) == 0) return std::string(prefix) + (((a >> 21) & 1) ? "MSR register" : "MRS");
    if ((a & 0x0FB00000) == 0x03200000) return std::string(prefix) + "MSR immediate";
    if ((a & 0x0F8000F0) == 0x00800090) return std::string(prefix) + "long multiply";
    if ((a & 0x0FC000F0) == 0x00000090) return std::string(prefix) + "MUL/MLA";
    int kind = refKind(a);
    if (kind == NK_SWAP) return std::string(prefix) + "SWP";
    if (kind == NK_BLOCK) return std::string(prefix) + ((a & BIT(20)) ? "LDM" : "STM") + ((a & BIT(22)) ? " with S" : "");
    if (kind == NK_TRANSFER) return std::string(prefix) + ((a & BIT(20)) ? "LDR" : "STR") + ((a & BIT(22)) ? "B" : "");
    if (kind == NK_HALF) {
        uint32_t type = (a >> 5) & 3;
        if (!(a & BIT(20))) return std::string(prefix) + (type == 1 ? "STRH" : type == 2 ? "LDRD" : "STRD");
        return std::string(prefix) + (type == 1 ? "LDRH" : type == 2 ? "LDRSB" : "LDRSH");
    }
    if ((a & 0x0C000000) == 0) {
        static const char *names[] = { "AND","EOR","SUB","RSB","ADD","ADC","SBC","RSC","TST","TEQ","CMP","CMN","ORR","MOV","BIC","MVN" };
        return std::string(prefix) + names[(a >> 21) & 0xF] + ((a & BIT(20)) ? "S" : "");
    }
    return std::string(prefix) + "other";
}

static std::string refFlags(uint32_t a, bool thumb, const RefState &s, int kind, uint32_t ea, bool taken) {
    // Forms where the architecture doesn't promise a result or a real DS's bus differs, listed apart so they can be judged apart
    std::string text;
    uint32_t rn = (a >> 16) & 0xF, rd = (a >> 12) & 0xF, rm = a & 0xF, rs = (a >> 8) & 0xF;
    bool load = a & BIT(20);
    bool writeback = !(a & BIT(24)) || (a & BIT(21));
    if (kind == NK_TRANSFER && (ea & 3) && !(a & BIT(22))) text += ", misaligned";
    if (kind == NK_HALF && (ea & 1)) text += ", misaligned";
    if (kind == NK_HALF && !load && ((a >> 5) & 3) != 1 && (ea & 3)) text += ", misaligned";
    if (kind == NK_HALF && load && ((a >> 5) & 3) == 2 && !(a & BIT(20))) text += ", misaligned";
    if (kind == NK_HALF && !load && ((a >> 5) & 3) != 1 && rd >= 14) text += ", uses PC";
    if (kind == NK_HALF && !load && ((a >> 5) & 3) != 1 && writeback && (rn == rd || rn == rd + 1)) text += ", writeback over the data";
    if (kind == NK_HALF && !load && ((a >> 5) & 3) == 2 && !(a & BIT(22)) && (rm == rd || rm == rd + 1)) text += ", offset loaded over";
    if (kind == NK_SWAP && (ea & 3)) text += ", misaligned";
    if (kind == NK_BLOCK && (ea & 3)) text += ", misaligned";
    if (kind == NK_TRANSFER && writeback && (rn == rd || rn == 15)) text += ", writeback over the data";
    if (kind == NK_HALF && writeback && (rn == rd || rn == 15)) text += ", writeback over the data";
    if (kind == NK_HALF && (rd == 15 || rm == 15)) text += ", uses PC";
    if (kind == NK_SWAP && (rn == 15 || rd == 15 || rm == 15 || rn == rd || rn == rm)) text += ", unpredictable registers";
    if (kind == NK_BLOCK) {
        uint32_t list = a & 0xFFFF;
        if (!list) text += ", empty list";
        if ((a & BIT(21)) && (list & BIT(rn))) text += ", base in list";
        if (rn == 15) text += ", PC base";
        if ((a & BIT(22)) && ((s.cpsr & 0x1F) == 0x10 || (s.cpsr & 0x1F) == 0x1F)) text += ", S without an SPSR";
    }
    if ((kind == NK_NONE || kind == NK_ALU_PC) && !thumb && (a & 0x0C000000) == 0 && (a & BIT(20)) && rd == 15 && !(((a >> 21) & 0xF) >= 8 && ((a >> 21) & 0xF) <= 11) &&
        ((s.cpsr & 0x1F) == 0x10 || (s.cpsr & 0x1F) == 0x1F)) text += ", S to PC without an SPSR";
    if ((a & 0x0E000090) == 0x00000090 && !(a & 0x60) && (rn == 15 || rd == 15 || rm == 15 || rs == 15)) text += ", uses PC";
    if ((a & 0x0E000090) == 0x00000010 && (rn == 15 || rm == 15 || rs == 15 || rd == 15)) text += ", PC with a register shift";
    if (kind == NK_TRANSFER && !load && rd == 15) text += ", stores PC";
    if (kind == NK_HALF && !load && rd == 15) text += ", stores PC";
    if (kind == NK_BLOCK && !load && (a & BIT(15))) text += ", stores PC";

    // ARMv5 additions don't promise anything with the PC, and only some instructions in Unicorn return from exceptions
    if ((a & 0x0F900090) == 0x01000080 && (rn == 15 || rd == 15 || rm == 15 || rs == 15)) text += ", uses PC";
    if ((a & 0x0F9000F0) == 0x01000050 && (rn == 15 || rd == 15 || rm == 15)) text += ", uses PC";
    if (!thumb && (a & 0x0C000000) == 0 && (a & BIT(20)) && rd == 15) {
        uint32_t opc = (a >> 21) & 0xF;
        if (opc < 8 || opc > 11) {
            bool isMul = !(a & BIT(25)) && (a & 0x90) == 0x90;
            if (!isMul && opc != 2 && opc != 13) text += ", S to PC that Unicorn doesn't treat as a return";
        }
    }

    // There's no SPSR to read or write in these modes
    uint32_t cpuMode = s.cpsr & 0x1F;
    if ((cpuMode == 0x10 || cpuMode == 0x1F) && (a & 0x0FF00000) == 0x03600000) text += ", SPSR without one";
    if ((cpuMode == 0x10 || cpuMode == 0x1F) && (a & 0x0FF000F0) == 0x01400000) text += ", SPSR without one";
    if ((cpuMode == 0x10 || cpuMode == 0x1F) && (a & 0x0FF000F0) == 0x01600000) text += ", SPSR without one";

    // MSR with an invalid mode, which Unicorn refuses to enter, or changing the THUMB bit, which it ignores
    bool msrImmediate = (a & 0x0FB00000) == 0x03200000, msrRegister = (a & 0x0FB000F0) == 0x01200000;
    if ((msrImmediate || msrRegister) && (a & BIT(16))) {
        uint32_t value = msrImmediate ? (((a & 0xFF) >> ((a >> 7) & 0x1E)) | ((a & 0xFF) << (32 - ((a >> 7) & 0x1E)))) : s.usr[0];
        if (msrImmediate && !((a >> 7) & 0x1E)) value = a & 0xFF;
        if (msrRegister) value = rm < 8 ? s.usr[rm] : 0;
        uint32_t mode = value & 0x1F;
        if (!(a & BIT(22)) && msrImmediate && (value & 0x20)) text += ", MSR changing the THUMB bit";
        if (!(a & BIT(22)) && msrRegister && rm < 8 && (value & 0x20)) text += ", MSR changing the THUMB bit";
        if (msrRegister && rm >= 8) text += ", invalid mode";
        else if (mode != 0x10 && mode != 0x11 && mode != 0x12 && mode != 0x13 && mode != 0x17 && mode != 0x1B && mode != 0x1F) text += ", invalid mode";
    }
    return text;
}

static bool refSharedWithArm7(uint32_t op, bool thumb) {
    // ARM7 behavior that's the same as the ARM946's: not ARMv5 additions, and not loads of the PC, which differ
    uint32_t a = thumb ? rigArm(op) : op;
    if (thumb) {
        if ((op & 0xF800) == 0xE800 || (op & 0xFF80) == 0x4780) return false; // BLX
        if ((op & 0xFF00) == 0xBD00) return false; // POP PC
        if ((op & 0xF000) == 0xD000 || (op & 0xF000) == 0xE000 || (op & 0xF000) == 0xF000 || (op & 0xFF00) == 0x4700) return true;
    }
    if (a >> 28 == 0xF) return false;
    std::string name = refClass(op, false);
    if ((a & 0x0E000090) == 0x00000090 && (a & 0x60) && ((a >> 5) & 3) != 1 && !(a & BIT(20))) return false; // LDRD/STRD
    if ((a & 0x0FF000F0) == 0x01600010 || (a & 0x0FF000F0) == 0x01200030) return false; // CLZ, BLX
    if ((a & 0x0F900090) == 0x01000080 || (a & 0x0F9000F0) == 0x01000050) return false; // ARMv5 DSP additions
    if ((a & 0x0E108000) == 0x08108000) return false; // LDM with the PC
    if ((a & 0x0C10F000) == 0x0410F000) return false; // LDR into the PC
    return true;
}

static uint32_t refAddress(uint32_t a, int kind, State &t, uint32_t pc) {
    // Where a fitted load or store starts
    uint32_t rn = (a >> 16) & 0xF, rm = a & 0xF;
    uint32_t from = rn == 15 ? pc : visible(t, rn);
    if (kind == NK_BLOCK || kind == NK_SWAP) return from;
    uint32_t offset;
    if (kind == NK_TRANSFER) {
        offset = !(a & BIT(25)) ? (a & 0xFFF) : shifted(rm == 15 ? pc : visible(t, rm), (a >> 5) & 3, (a >> 7) & 0x1F, t.cpsr);
    }
    else {
        offset = (a & BIT(22)) ? (((a >> 4) & 0xF0) | (a & 0xF)) : (rm == 15 ? pc : visible(t, rm));
    }
    if (!(a & BIT(24))) return from;
    return (a & BIT(23)) ? from + offset : from - offset;
}

static uint32_t refNormalize(uint32_t op) {
    // Random bits fill fields the architecture says to leave as 0 or 1, which real code doesn't do and Unicorn rejects
    if ((op & 0x0C000000) == 0 && !((op & 0x02000000) == 0 && (op & 0x90) == 0x90)) {
        uint32_t opc = (op >> 21) & 0xF;
        if ((op & BIT(20)) || (opc < 8 || opc > 11)) {
            if (opc == 13 || opc == 15) op &= ~0x000F0000; // MOV, MVN
            if (opc >= 8 && opc <= 11) op &= ~0x0000F000; // TST, TEQ, CMP, CMN
        }
        else if ((op & 0x0FBF0000) == 0x010F0000 && !(op & 0xF0)) op &= 0xFFFFF000 | (op & 0xF000); // MRS
        else if ((op & 0x0FB00000) == 0x01200000 && !(op & 0xF0)) op = (op & 0xFFFF000F) | 0xF000; // MSR register
        else if ((op & 0x0FB00000) == 0x03200000) op |= 0xF000; // MSR immediate
        else if ((op & 0x0FF000D0) == 0x01200010) op = (op & 0xF00000FF) | 0x012FFF00; // BX, BLX
        else if ((op & 0x0FF000F0) == 0x01600010) op = (op & 0xF000F00F) | 0x016F0F00; // CLZ
    }
    if ((op & 0x0FB000F0) == 0x01000090) op &= ~0x00000F00; // SWP
    if ((op & 0x0E400090) == 0x00000090 && (op & 0x60)) op &= ~0x00000F00; // Halfword transfers at a register offset
    if ((op & 0x0F9000F0) == 0x01000050) op &= ~0x00000F00; // QADD, QSUB
    if ((op & 0x0E000000) == 0x08000000) {} // LDM, STM have nothing to clear
    return op;
}

static void testReference(uint32_t op, bool thumb, int cpu, uint32_t second = 0) {
    bool pair = second != 0;
    Interpreter &arm = core->interpreter[cpu];
    if (!thumb) op = refNormalize(op);
    uint32_t a = thumb ? rigArm(op) : op;

    // Leave out what Unicorn can't speak for
    if (thumb) {
        if (Interpreter::thumbInstrs[(op >> 6) & 0x3FF] == &Interpreter::unkThumb) { count("reference skipped: undefined"); return; }
        if ((op & 0xFF00) == 0xDF00) { count("reference skipped: SWI"); return; }
        if (!pair && ((op & 0xF800) == 0xF800 || (op & 0xF800) == 0xE800)) { count("reference skipped: second half of BL alone"); return; }
    }
    else {
        if ((op >> 28) == 0xF && (op & 0x0E000000) != 0x0A000000) { count("reference skipped: reserved condition"); return; }
        if ((op >> 28) != 0xF && Interpreter::armInstrs[((op >> 16) & 0xFF0) | ((op >> 4) & 0xF)] == &Interpreter::unkArm) { count("reference skipped: undefined"); return; }
        if ((op & 0x0C000000) == 0x0C000000) { count("reference skipped: coprocessor and SWI"); return; }
    }
    if (cpu && !refSharedWithArm7(op, thumb)) { count("reference skipped: differs on the ARM7"); return; }

    uint32_t step = thumb ? 2 : 4;
    uint32_t address = codeAddress(thumb), pc = address + step * 2;
    if (thumb && a >> 28 != 0xF) {}
    RefState in;
    uint32_t *words = (uint32_t*)&in;
    for (size_t i = 0; i < sizeof(in) / 4; i++) words[i] = interesting();
    static const uint32_t modes[] = { 0x1F, 0x1F, 0x13, 0x12, 0x11, 0x10, 0x17, 0x1B };
    uint32_t mode = modes[rnd(8)];
    in.cpsr = (rnd(16) << 28) | (cpu ? 0 : (rnd(2) << 27)) | (rnd(4) << 6) | (thumb ? 0x20 : 0) | mode;
    in.spsrFiq = (in.spsrFiq & 0xF80000C0) | modes[rnd(8)];
    in.spsrSvc = (in.spsrSvc & 0xF80000C0) | modes[rnd(8)];
    in.spsrAbt = (in.spsrAbt & 0xF80000C0) | modes[rnd(8)];
    in.spsrIrq = (in.spsrIrq & 0xF80000C0) | modes[rnd(8)];
    in.spsrUnd = (in.spsrUnd & 0xF80000C0) | modes[rnd(8)];
    in.usr[15] = pc;

    // Fit a load or store into the window, using the fitting from the other suites
    int kind = refKind(a);
    uint32_t ea = 0;
    if (a >> 28 != 0xF && kind != NK_NONE) {
        State t;
        t.mode = (mode == 0x11) ? 2 : (mode == 0x10 || mode == 0x1F) ? 0 : 1;
        t.cpsr = in.cpsr;
        uint32_t temporary[16];
        for (int i = 0; i < 15; i++) visible(t, i) = *refBank(in, mode, i);
        for (int i = 0; i < 16; i++) t.usr[i] = i < 8 || !banked(t.mode, i) ? *refBank(in, mode, i) : t.usr[i];
        t.usr[15] = pc;
        uint32_t wordPc = (thumb && ((op >> 11) == 0x09)) ? (pc & ~3u) : pc;
        if (!fitAddress(a, kind == NK_SWAP ? NK_SWAP : kind, t, wordPc, false)) { count("reference skipped: no address fits"); return; }
        for (int i = 0; i < 15; i++) *refBank(in, mode, i) = visible(t, i);
        ea = refAddress(a, kind, t, wordPc);
        if (ea < WINDOW + 0x100 || ea >= WINDOW + WINDOW_SIZE - 0x200) { count("reference skipped: no address fits"); return; }
    }

    // Forms where the architecture doesn't promise a result, or where QEMU is known not to act like a DS, aren't compared
    std::string flags = refFlags(a, thumb, in, a >> 28 == 0xF ? NK_NONE : kind, ea, true);
    if (!flags.empty()) {
        size_t comma = flags.find(',', 2);
        count("reference skipped:" + flags.substr(1, comma == std::string::npos ? std::string::npos : comma - 1));
        return;
    }

    // The instruction, in both memories, which go back to the seed afterwards
    // A NOP follows, since Unicorn runs a THUMB BL as one instruction if its second half is next, and whatever else
    // the window has there can be something it rejects, after which it runs past the one instruction it's asked for
    uint8_t *ramCode = ram + (address & 0x3FFFFF);
    uint32_t nop = thumb ? 0x46C0 : 0xE1A00000;
    memcpy(ramCode, &op, step);
    memcpy(ramCode + step, &nop, step);
    if (pair) memcpy(ramCode + 2, &second, 2);
    uc_mem_write(ucArm, address, ramCode, step * 2);
    uc_ctl_remove_cache(ucArm, address, address + step * 2);

    // The interpreter
    bool pass = thumb || (op >> 28) == 0xF || Interpreter::condition[((op >> 24) & 0xF0) | (in.cpsr >> 28)] == 1;
    loadInterpreter(arm, in);
    if (pass) {
        if (thumb) (arm.*Interpreter::thumbInstrs[(op >> 6) & 0x3FF])(op);
        else if ((op >> 28) == 0xF) arm.handleReserved(op);
        else (arm.*Interpreter::armInstrs[((op >> 16) & 0xFF0) | ((op >> 4) & 0xF)])(op);
    }
    RefState got;
    saveInterpreter(arm, got);
    bool newThumb = got.cpsr & BIT(5);
    uint32_t gotNext = got.usr[15] != pc ? got.usr[15] - (newThumb ? 2 : 4) : address + step;
    if (pair) {
        // Both halves of BL are one instruction to Unicorn
        arm.registersUsr[15] = pc + 2;
        (arm.*Interpreter::thumbInstrs[(second >> 6) & 0x3FF])(second);
        saveInterpreter(arm, got);
        newThumb = got.cpsr & BIT(5);
        gotNext = got.usr[15] - (newThumb ? 2 : 4);
    }

    // Where a jump lands gets NOPs as well, in both memories: Unicorn stops there, and after stopping in front of some of
    // what the window holds, it has run past the one instruction it's asked for in every case that followed
    uint32_t landing = gotNext & (newThumb ? ~1u : ~3u);
    bool landed = gotNext != address + step && landing >= WINDOW && landing + 8 <= WINDOW + WINDOW_SIZE &&
        (landing + 8 <= address || landing >= address + step * 2);
    if (landed) {
        uint32_t nops[2] = { newThumb ? 0x46C046C0 : 0xE1A00000, newThumb ? 0x46C046C0 : 0xE1A00000 };
        memcpy(ram + (landing & 0x3FFFFF), nops, 8);
        uc_mem_write(ucArm, landing, nops, 8);
        uc_ctl_remove_cache(ucArm, landing, landing + 8);
    }

    // Unicorn
    loadUnicorn(in);
    uc_err err = uc_emu_start(ucArm, address | (thumb ? 1 : 0), 0xFFFFFFF0, 0, 1);

    // Unicorn runs the two halves of BL as one instruction, but BLX as two
    if (pair && err == UC_ERR_OK && armRead(UC_ARM_REG_R15) == address + 2)
        err = uc_emu_start(ucArm, (address + 2) | 1, 0xFFFFFFF0, 0, 1);
    now.inCpsr = in.cpsr;
    std::string name = (cpu ? "ARM7 " : "ARM9 ") + refClass(op, thumb) + refFlags(a, thumb, in, kind, ea, pass);
    char text[160];
    snprintf(text, sizeof(text), "%s %08X mode %02X r0-3 %08X %08X %08X %08X", thumb ? "T" : "A", op, mode, in.usr[0], in.usr[1], in.usr[2], in.usr[3]);
    now.description = text;
    now.name = "reference: " + name;
    count("suite:reference");
    count(std::string("reference ") + (cpu ? "ARM7 " : "ARM9 ") + (thumb ? "THUMB" : "ARM"));
    if (err == UC_ERR_INSN_INVALID) {
        // It doesn't accept this encoding, so there's nothing to compare with
        count("reference skipped: Unicorn rejects it");
        armReg(UC_ARM_REG_CPSR, 0x1F | 0xC0);
        count("reference rejected: " + name);
    }
    else if (err != UC_ERR_OK && err != UC_ERR_FETCH_UNMAPPED && err != UC_ERR_FETCH_PROT) {
        fail("Unicorn stopped: %s", uc_strerror(err));
        armReg(UC_ARM_REG_CPSR, 0x1F | 0xC0);
    }
    else {
        // A branch out of the window is fine: the instruction ran, and it's only the next fetch that failed
        RefState want;
        memcpy(&want, &in, sizeof(want));
        uint32_t wantNext;
        saveUnicorn(want, wantNext);

        // The ARM7 doesn't have the Q flag that Unicorn's ARM946 does
        if (cpu) {
            for (RefState *state: { &got, &want }) {
                state->cpsr &= ~BIT(27);
                state->spsrFiq &= ~BIT(27); state->spsrSvc &= ~BIT(27); state->spsrAbt &= ~BIT(27);
                state->spsrIrq &= ~BIT(27); state->spsrUnd &= ~BIT(27);
            }
        }
        std::string difference = diffRef(got, want);
        // ARM ignores the low bits of a jump, while Unicorn keeps them in its program counter
        uint32_t nextMask = (want.cpsr & BIT(5)) ? ~1u : ~3u;
        if ((gotNext & nextMask) != (wantNext & nextMask)) difference += "next address interp " + std::to_string(gotNext) + ", unicorn " + std::to_string(wantNext) + "; ";
        if (!difference.empty()) fail("%s", difference.c_str());

        // Memory, only when something could have been written
        if (kind != NK_NONE && !(a & BIT(20)) && a >> 28 != 0xF) {
            static uint8_t window[WINDOW_SIZE];
            uc_mem_read(ucArm, WINDOW, window, WINDOW_SIZE);
            uint8_t *host = ram + (WINDOW & 0x3FFFFF);
            if (memcmp(window, host, WINDOW_SIZE)) {
                for (uint32_t x = 0; x < WINDOW_SIZE; x += 4) {
                    if (memcmp(window + x, host + x, 4)) {
                        uint32_t one, two;
                        memcpy(&one, host + x, 4);
                        memcpy(&two, window + x, 4);
                        fail("memory at %08X interp %08X, unicorn %08X", WINDOW + x, one, two);
                        break;
                    }
                }
            }
        }
    }
    if (kind == NK_SWAP || (kind != NK_NONE && a >> 28 != 0xF && !(a & BIT(20))))
        uc_mem_write(ucArm, WINDOW, seed, WINDOW_SIZE);
    else
        uc_mem_write(ucArm, address, seed + (address - WINDOW), step * 2);
    if (landed) uc_mem_write(ucArm, landing, seed + (landing - WINDOW), 8);
    restoreRam();
}

static void suiteReference() {
    // Earlier suites leave pages unmapped for writes, as if they had code, so map everything back
    finishBlocks(0);
    finishBlocks(1);
    core->memory.updateMap9(0x00000000, 0x04000000);
    core->memory.updateMap7(0x02000000, 0x04000000);
    core->jit.codePages = 0;
    core->jit7.codePages = 0;

    uc_open(UC_ARCH_ARM, UC_MODE_ARM, &ucArm);
    uc_ctl_set_cpu_model(ucArm, UC_CPU_ARM_946);
    uc_mem_map(ucArm, WINDOW, WINDOW_SIZE, UC_PROT_ALL);
    uc_mem_write(ucArm, WINDOW, seed, WINDOW_SIZE);
    restoreRam();
    armReg(UC_ARM_REG_CPSR, 0xDF);

    for (int cpu = 0; cpu < 2; cpu++) {
        for (uint32_t op = 0; op < 0x10000; op++) {
            if (!mine(op)) continue;
            rng.seed(op * 2 + cpu + 80000);
            for (int i = 0; i < 4; i++) testReference(op, true, cpu);
            if ((op & 0xF800) == 0xF000) {
                // Whole BLs, and BLXs on the ARM9, which is the only way Unicorn accepts their second halves
                testReference(op, true, cpu, 0xF800 | (rng() & 0x7FF));
                if (!cpu) testReference(op, true, cpu, 0xE800 | (rng() & 0x7FE));
            }
        }
        for (uint32_t index = 0; index < 0x1000; index++) {
            for (uint32_t cond = 0; cond < 16; cond++) {
                if (!mine(index * 16 + cond)) continue;
                rng.seed((index * 16 + cond) * 2 + cpu + 90000);
                for (int fill = 0; fill < 4; fill++) {
                    uint32_t op = (cond << 28) | ((index & 0xFF0) << 16) | ((index & 0xF) << 4) | (rng() & 0x000FFF0F);
                    for (int i = 0; i < 2; i++) testReference(op, false, cpu);
                }
            }
        }
    }
}

static void suiteReturns() {
    // Returns from exceptions, with every kind of saved state, since random SPSRs are rarely valid
    uint64_t index = 0;
    static const uint32_t ops[] = { 0xE25EF004, 0xE1B0F00E, 0xE3A0F102, 0xE29EF000, 0xE1B0F00E | (1 << 5), 0xE05EF10E, 0x125EF004, 0xE1F0F00E,
        0x125FF002, 0xB25FF002 }; // The last two return to where a THUMB program counter is the same as the next instruction's
    static const uint32_t modes[] = { 0x10, 0x1F, 0x12, 0x13, 0x17, 0x1B, 0x11, 0x00, 0x15 };
    for (uint32_t op: ops) {
        for (uint32_t saved: modes) {
            for (int variant = 0; variant < 24; variant++) {
                if (!mine(index++)) continue;
                rng.seed(index + 90000);
                for (int cpu = 0; cpu < 2; cpu++) {
                    for (int mode = 0; mode < 3; mode++) {
                        State s = randomState(false, mode);
                        s.spsr = (rnd(16) << 28) | saved | (rnd(2) ? BIT(5) : 0) | (rnd(2) ? BIT(7) : 0);
                        testOneOp("returns", cpu, false, op, mode, MEMORY_MAPPED, &s);
                    }
                }
            }
        }
    }
}

static void suiteReturnsThenCode() {
    // A return followed by code using the banked registers of the mode it went to, for every pair of modes
    static const uint32_t modes[] = { 0x10, 0x1F, 0x12, 0x13, 0x17, 0x1B, 0x11 };
    uint64_t index = 0;
    for (int startMode = 0; startMode < 3; startMode++) {
        for (uint32_t saved: modes) {
            for (int thumb = 0; thumb < 2; thumb++) {
                for (int variant = 0; variant < 40; variant++) {
                    if (!mine(index++)) continue;
                    rng.seed(index + 100000);
                    int cpu = rnd(4) == 0;
                    uint32_t page = CODE_START + 0x1000 * rnd(12), addressA = page + 0x100, addressB = page + 0x180;
                    clearCode(page, 0x1000);
                    uint32_t a = 0xE25EF004; // SUBS PC, LR, #4
                    putCode(addressA, &a, 1, false);
                    if (thumb) {
                        uint32_t b[] = { 0x4668, 0x4671, 0xB001, 0x46A6, 0, 0 }; // MOV R0, SP; MOV R1, LR; ADD SP, #4; MOV LR, R4
                        b[4] = terminator(true, addressB + 8, 32);
                        putCode(addressB, b, 5, true);
                    }
                    else {
                        // MOV R0, SP; MOV R1, LR; MRS R2, SPSR; ADD SP, SP, #4; SUB LR, LR, #8; MSR SPSR_f, R5
                        uint32_t b[] = { 0xE1A0000D, 0xE1A0100E, 0xE14F2000, 0xE28DD004, 0xE24EE008, 0xE168F005, 0 };
                        b[6] = terminator(false, addressB + 24, 32);
                        putCode(addressB, b, 7, false);
                    }
                    JitBlock *blockA = compileAt(cpu, addressA, false);
                    JitBlock *blockB = compileAt(cpu, addressB, thumb);
                    if (!blockA || !blockB) continue;
                    State s = randomState(false, startMode);
                    s.spsr = (rnd(16) << 28) | saved | (thumb ? BIT(5) : 0) | (rnd(2) ? BIT(7) : 0);
                    visible(s, 14) = addressB + 4;
                    Case c;
                    c.name = "returns then code";
                    c.cpu = cpu;
                    c.in = s;
                    c.blocks = { blockA, blockB };
                    c.memoryMode = MEMORY_MAPPED;
                    c.stopMode = rnd(5);
                    c.deadline = 1 + rnd(12);
                    c.limitDeadline = rnd(2);
                    c.stopAfter = rnd(5);
                    runCase(c);
                    count("suite:returns then code");
                    restoreRam();
                    finishBlocks(cpu);
                }
            }
        }
    }
}

static void suiteMath() {
    // The math registers generated code reaches directly, against the real ones, so byte order and offsets are checked
    Jit &jit = core->jit;
    DivSqrt &div = core->divSqrt;
    uint64_t index = 0;
    now.name = "math";
    auto get8 = [&](uint32_t at) { return (uint64_t(readBe(MATH + at)) << 32) | readBe(MATH + at + 4); };
    auto enter = [&](int type, uint32_t address, uint32_t value) {
        setReg(UC_PPC_REG_1, STACK);
        setReg(UC_PPC_REG_5, address);
        setReg(UC_PPC_REG_8, value);
        setReg(UC_PPC_REG_0 + 23, WRITE_MAP);
        setReg(UC_PPC_REG_0 + 24, READ_MAP);
        setReg(UC_PPC_REG_0 + 27, EVENTS);
        setReg(UC_PPC_REG_0 + 28, START_CYCLES);
        setReg(UC_PPC_REG_0 + 29, LIMIT);
        setReg(UC_PPC_REG_0 + 30, GLOBAL);
        setReg(UC_PPC_REG_0 + 31, ARM_STATE);
        setReg(UC_PPC_REG_LR, END);
        return uc_emu_start(uc, emuCode(jit.codeMemory[type]), END, 0, 2000);
    };
    now.mathLive = false;
    for (uint32_t address = 0x4000278; address < 0x40002C8; address++) {
        for (int type = 0; type < 6; type++) {
            for (int variant = 0; variant < 16; variant++) {
                if (!mine(index++)) continue;
                rng.seed(index + 120000);
                bool load = type < 3;
                int size = (type % 3 == 0) ? 4 : (type % 3 == 1) ? 1 : 2;
                uint32_t value = interesting();
                div.divCnt = rng() & 0xC003;
                div.sqrtCnt = rng() & 0x8001;
                div.divNumer = (uint64_t(rng()) << 32) | rng();
                div.divDenom = (uint64_t(rng()) << 32) | rng();
                div.sqrtParam = (uint64_t(rng()) << 32) | rng();
                div.divDirty = rnd(2);
                div.sqrtDirty = rnd(2);
                putMath(div);

                // What should be done without leaving generated code
                uint32_t o = address - 0x4000280;
                bool inside = address >= 0x4000280 && address < 0x40002C0, direct = false;
                if (inside && size == 2 && o == 0x30) direct = true;
                if (inside && size == 2 && o == 0) direct = !load || !div.divDirty;
                if (inside && size == 4 && !(o & 3) && ((o >= 0x10 && o < 0x20) || o >= 0x38)) direct = true;

                char text[96];
                snprintf(text, sizeof(text), "address %08X size %d %s", address, size, load ? "load" : "store");
                now.description = text;
                now.failed = false;
                now.mathCalls = 0;
                uc_err err = enter(type, address, value);
                count("suite:math");
                if (err != UC_ERR_OK || reg(UC_PPC_REG_PC) != END) { fail("didn't finish: %s", uc_strerror(err)); continue; }
                if (now.mathCalls != (direct ? 0 : 1)) { fail("%d memory calls, want %d", now.mathCalls, direct ? 0 : 1); continue; }
                if (!direct) { count("math left to C++"); continue; }
                count("math done directly");

                if (load) {
                    uint32_t want = (size == 4) ? core->memory.read<uint32_t>(0, address) : core->memory.read<uint16_t>(0, address);
                    if (reg(UC_PPC_REG_8) != want) fail("loaded %08X, want %08X", reg(UC_PPC_REG_8), want);
                    continue;
                }
                if (size == 4) core->memory.write<uint32_t>(0, address, value);
                else core->memory.write<uint16_t>(0, address, value);
                uint8_t got[6];
                uc_mem_read(uc, MATH + MATH_DIV_CNT, got, sizeof(got));
                if (get8(MATH_NUMER) != uint64_t(div.divNumer)) fail("numerator %llX, want %llX", (unsigned long long)get8(MATH_NUMER), (unsigned long long)div.divNumer);
                if (get8(MATH_DENOM) != uint64_t(div.divDenom)) fail("denominator %llX, want %llX", (unsigned long long)get8(MATH_DENOM), (unsigned long long)div.divDenom);
                if (get8(MATH_PARAM) != div.sqrtParam) fail("square root parameter %llX, want %llX", (unsigned long long)get8(MATH_PARAM), (unsigned long long)div.sqrtParam);
                if (((got[0] << 8) | got[1]) != div.divCnt) fail("DIVCNT %04X, want %04X", (got[0] << 8) | got[1], div.divCnt);
                if (((got[2] << 8) | got[3]) != div.sqrtCnt) fail("SQRTCNT %04X, want %04X", (got[2] << 8) | got[3], div.sqrtCnt);
                if (got[4] != div.divDirty || got[5] != div.sqrtDirty) fail("dirty flags %d %d, want %d %d", got[4], got[5], div.divDirty, div.sqrtDirty);
            }
        }
    }

    // Random runs of accesses against the same run done only in C++, where generated code and C++ share the dirty flags
    // and results are worked out from what generated code wrote
    now.mathLive = true;
    static const uint32_t common[] = { 0x4000280, 0x40002B0, 0x4000290, 0x4000294, 0x4000298, 0x400029C, 0x40002A0, 0x40002A4, 0x40002A8,
        0x40002AC, 0x40002B4, 0x40002B8, 0x40002BC };
    for (uint64_t run = 0; run < 8000; run++) {
        if (!mine(index++)) continue;
        rng.seed(run + 130000);
        DivSqrt reference = div;
        reference.divCnt = reference.sqrtCnt = 0;
        reference.divNumer = reference.divDenom = reference.divResult = reference.divRemResult = 0;
        reference.sqrtParam = reference.sqrtResult = 0;
        reference.divDirty = reference.sqrtDirty = false;
        DivSqrt tested = reference;
        now.failed = false;
        for (int step = 0; step < 40 && !now.failed; step++) {
            uint32_t address = common[rnd(13)] + (rnd(16) ? 0 : rnd(4));
            int type = rnd(6);
            if (rnd(4)) type = (type & 3) ? ((address & 2) || address == 0x4000280 || address == 0x40002B0 ? 2 + 3 * rnd(2) : 3 * rnd(2)) : type;
            if ((address == 0x4000280 || address == 0x40002B0) && rnd(4)) type = rnd(2) ? 2 : 5;
            bool load = type < 3;
            int size = (type % 3 == 0) ? 4 : (type % 3 == 1) ? 1 : 2;
            uint32_t value = rnd(4) ? (rng() >> rnd(32)) : interesting();

            // The run done only in C++
            div = reference;
            uint32_t want = 0;
            if (load) {
                want = (size == 4) ? core->memory.read<uint32_t>(0, address) : (size == 2) ? core->memory.read<uint16_t>(0, address) :
                    core->memory.read<uint8_t>(0, address);
                int shift = (size == 4) ? (address & 3) * 8 : 0;
                if (shift) want = (want << (32 - shift)) | (want >> shift);
            }
            else if (size == 4) core->memory.write<uint32_t>(0, address, value);
            else if (size == 2) core->memory.write<uint16_t>(0, address, value);
            else core->memory.write<uint8_t>(0, address, value);
            reference = div;

            // The same through generated code, with C++ taking what falls through
            char text[96];
            snprintf(text, sizeof(text), "step %d address %08X size %d %s", step, address, size, load ? "load" : "store");
            now.description = text;
            div = tested;
            putMath(div);
            now.mathCalls = 0;
            uc_err err = enter(type, address, value);
            count("math sequence steps");
            if (err != UC_ERR_OK || reg(UC_PPC_REG_PC) != END) { fail("didn't finish: %s", uc_strerror(err)); break; }
            getMath(div);
            tested = div;
            if (load && reg(UC_PPC_REG_8) != want) fail("loaded %08X, want %08X", reg(UC_PPC_REG_8), want);

            // Results might not be worked out yet on one side, so compare what the DS can see
            if (tested.divNumer != reference.divNumer || tested.divDenom != reference.divDenom || tested.sqrtParam != reference.sqrtParam ||
                tested.divCnt != reference.divCnt || tested.sqrtCnt != reference.sqrtCnt || tested.divDirty != reference.divDirty ||
                tested.sqrtDirty != reference.sqrtDirty)
                fail("registers differ: numerator %llX want %llX, denominator %llX want %llX, parameter %llX want %llX, control %X %X want %X %X, dirty %d %d want %d %d",
                    (unsigned long long)tested.divNumer, (unsigned long long)reference.divNumer, (unsigned long long)tested.divDenom,
                    (unsigned long long)reference.divDenom, (unsigned long long)tested.sqrtParam, (unsigned long long)reference.sqrtParam,
                    tested.divCnt, tested.sqrtCnt, reference.divCnt, reference.sqrtCnt, tested.divDirty, tested.sqrtDirty, reference.divDirty, reference.sqrtDirty);
        }
        count("suite:math");
    }
    now.mathLive = false;
}

static void suiteInterruptRegisters() {
    // IME, IE and IF as generated code reaches them directly, against the real ones
    Jit &jit = core->jit;
    Interpreter &arm = core->interpreter[0];
    uint64_t index = 0;
    now.name = "interrupt registers";
    now.mathLive = false;
    for (uint32_t address = 0x40001F8; address < 0x4000248; address++) {
        for (int type = 0; type < 6; type++) {
            for (int variant = 0; variant < 64; variant++) {
                if (!mine(index++)) continue;
                rng.seed(index + 160000);
                bool load = type < 3;
                int size = (type % 3 == 0) ? 4 : (type % 3 == 1) ? 1 : 2;
                uint32_t value = rnd(4) ? rng() : (rng() & 1);
                uint8_t ime = rnd(2);
                uint64_t ie = (uint64_t(rnd(4) ? 0 : rng()) << 32) | (rng() & (rnd(2) ? 0xFFFFFFFFu : BIT(rnd(32))));
                uint64_t irf = (uint64_t(rnd(4) ? 0 : rng()) << 32) | (rng() & (rnd(2) ? 0xFFFFFFFFu : BIT(rnd(32))));
                uint32_t cpsr = 0x1F | (rnd(2) ? BIT(7) : 0), gxStat = rng();

                arm.ime = ime;
                arm.ie = ie;
                arm.irf = irf;
                arm.cpsr = cpsr;
                core->gpu3D.gxStat = gxStat;
                writeBe(ARM_STATE + OFF_CPSR, cpsr);
                writeBe(ARM_STATE + OFF_IE, ie >> 32);
                writeBe(ARM_STATE + OFF_IE + 4, uint32_t(ie));
                writeBe(ARM_STATE + OFF_IRF, irf >> 32);
                writeBe(ARM_STATE + OFF_IRF + 4, uint32_t(irf));
                uc_mem_write(uc, ARM_STATE + OFF_IME, &ime, 1);
                writeBe(MATH + MATH_GX_STAT, gxStat);

                // What should be done without leaving generated code
                uint32_t mode = gxStat >> 30;
                bool fifoIrq = (mode == 1 && (gxStat & BIT(25))) || (mode == 2 && (gxStat & BIT(26)));
                bool masked = cpsr & BIT(7), direct = false;
                bool imeRegister = address == 0x4000208 && size >= 2;
                uint64_t newIe = (ie & ~uint64_t(0xFFFFFF7Fu)) | (value & 0xFFFFFF7Fu);
                if (load) direct = imeRegister || (size == 4 && (address == 0x4000210 || address == 0x4000214));
                else if (imeRegister) direct = !(value & 1) || masked || !(ie & irf);
                else if (size == 4 && address == 0x4000210) direct = !ime || masked || !(newIe & irf);
                else if (size == 4 && address == 0x4000214) direct = !fifoIrq;

                char text[128];
                snprintf(text, sizeof(text), "address %08X size %d %s value %08X ime %d", address, size, load ? "load" : "store", value, ime);
                now.description = text;
                now.failed = false;
                now.mathCalls = 0;
                setReg(UC_PPC_REG_1, STACK);
                setReg(UC_PPC_REG_5, address);
                setReg(UC_PPC_REG_8, value);
                setReg(UC_PPC_REG_0 + 23, WRITE_MAP);
                setReg(UC_PPC_REG_0 + 24, READ_MAP);
                setReg(UC_PPC_REG_0 + 27, EVENTS);
                setReg(UC_PPC_REG_0 + 28, START_CYCLES);
                setReg(UC_PPC_REG_0 + 29, LIMIT);
                setReg(UC_PPC_REG_0 + 30, GLOBAL);
                setReg(UC_PPC_REG_0 + 31, ARM_STATE);
                setReg(UC_PPC_REG_LR, END);
                uc_err err = uc_emu_start(uc, emuCode(jit.codeMemory[type]), END, 0, 2000);
                count("suite:interrupt registers");
                if (err != UC_ERR_OK || reg(UC_PPC_REG_PC) != END) { fail("didn't finish: %s", uc_strerror(err)); continue; }
                if (now.mathCalls != (direct ? 0 : 1)) { fail("%d memory calls, want %d", now.mathCalls, direct ? 0 : 1); continue; }
                if (!direct) { count("interrupt registers left to C++"); continue; }
                count("interrupt registers done directly");

                if (load) {
                    uint32_t want = (size == 4) ? core->memory.read<uint32_t>(0, address) : core->memory.read<uint16_t>(0, address);
                    if (reg(UC_PPC_REG_8) != want) fail("loaded %08X, want %08X", reg(UC_PPC_REG_8), want);
                    continue;
                }
                if (size == 4) core->memory.write<uint32_t>(0, address, value);
                else core->memory.write<uint16_t>(0, address, value);
                uint8_t gotIme;
                uc_mem_read(uc, ARM_STATE + OFF_IME, &gotIme, 1);
                uint64_t gotIe = (uint64_t(readBe(ARM_STATE + OFF_IE)) << 32) | readBe(ARM_STATE + OFF_IE + 4);
                uint64_t gotIrf = (uint64_t(readBe(ARM_STATE + OFF_IRF)) << 32) | readBe(ARM_STATE + OFF_IRF + 4);
                if (gotIme != arm.ime) fail("IME %d, want %d", gotIme, arm.ime);
                if (gotIe != arm.ie) fail("IE %llX, want %llX", (unsigned long long)gotIe, (unsigned long long)arm.ie);
                if (gotIrf != arm.irf) fail("IF %llX, want %llX", (unsigned long long)gotIrf, (unsigned long long)arm.irf);
            }
        }
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
    jit.addrDtcmReg = DTCM_REG;
    jit.addrWriteCpsr = FAKE_MSR;
    jit.addrRestoreCpsr = FAKE_RESTORE;
    jit.addrModeBanks = MODE_BANKS;
    jit.addrGxStat = MATH + MATH_GX_STAT;
    jit.offIme = OFF_IME;
    jit.addrDivNumer = MATH + MATH_NUMER;
    jit.addrSqrtParam = MATH + MATH_PARAM;
    jit.addrDivCnt = MATH + MATH_DIV_CNT;
    jit.addrSqrtCnt = MATH + MATH_SQRT_CNT;
    jit.addrDivDirty = MATH + MATH_DIV_DIRTY;
    jit.addrSqrtDirty = MATH + MATH_SQRT_DIRTY;
    jit.offIe = OFF_IE;
    jit.offIrf = OFF_IRF;
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
    core->cp15.dtcmReg = 0x027E000A;
    writeBe(DTCM_REG, core->cp15.dtcmReg);
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
        rigPoison = 0;
    };
    run("thumb", suiteThumbOps);
    run("arm", suiteArmOps);
    run("forms", suiteArmForms);
    run("flags", suiteFlagGrid);
    run("conditions", suiteConditionGrid);
    run("lists", suiteBlockLists);
    run("returns", suiteReturns);
    run("returns", suiteReturnsThenCode);
    run("math", suiteMath);
    run("math", suiteInterruptRegisters);
    run("liveness", suiteLiveness);
    run("blocks", suiteBlocks);
    run("links", suiteLinks);
    run("reference", suiteReference);

    for (int cpu = 0; cpu < 2; cpu++) {
        const char *name = cpu ? "ARM7" : "ARM9";
        for (uint32_t x: ranThumb[cpu]) printf("RAN\tTHUMB encodings run, %s\t%X\n", name, x);
        for (uint32_t x: ranNativeThumb[cpu]) printf("RAN\tTHUMB encodings with native code run, %s\t%X\n", name, x);
        for (uint32_t x: ranArm[cpu]) printf("RAN\tARM table positions by condition run, %s\t%X\n", name, x);
        for (uint32_t x: ranNativeArm[cpu]) printf("RAN\tARM table positions by condition with native code run, %s\t%X\n", name, x);
    }
    static const char *left[] = { "flags left out: all", "flags left out: all but the carry" };
    for (int i = 0; i < 2; i++) {
        counts[left[i]] += rigFlagStats[i];
        if (!rigFlagStats[i] && (wanted.empty() || wanted.count("liveness"))) fails[std::string("no ") + left[i]]++;
    }
    for (auto &entry: counts) printf("COUNT\t%s\t%llu\n", entry.first.c_str(), (unsigned long long)entry.second);
    for (auto &entry: fails) printf("FAILS\t%s\t%llu\n", entry.first.c_str(), (unsigned long long)entry.second);
    printf("DONE\n");
    fflush(stdout);
    _exit(fails.empty() ? 0 : 1);
}
