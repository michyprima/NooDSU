// Runs whole games on a Mac with the JIT's generated PowerPC in Unicorn, to compare with the interpreter frame by frame
// See README.md; game.sh builds this against a copy of the core changed by patch.py in game mode

#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <thread>
#include <vector>

#define private public
#include "core.h"
#undef private
#include "settings.h"
#include <unicorn/unicorn.h>

extern void (*rigSetup)(Jit &jit);
extern uint64_t rigAudioHash;
extern FILE *rigAudioFile;
#ifdef ROM_TEST
#include "rom_test.h"
#endif
extern int rigPoison;
extern uint64_t rigFlagStats[2];

// Addresses in the emulator; areas marked live are read through callbacks to the core's own state, and areas marked
// copied are the core's values in the emulator's byte order, copied in when generated code is entered or a call from it
// returns and back when it calls or returns, which is when C++ runs, so neither side ever sees a stale value
static const uint32_t
    ARM_STATE = 0x10000, // Copied: the interpreter, at its own offsets, with the banked register pointers after it
    VIRT_REGS = 0x7000, VIRT_SPSR = 0x7040, // Offsets of the pointers, which are 64-bit on the Mac; displacements are 16-bit
    STATE_SIZE = 0x8000,
    EVENTS = 0x20000, EVENT_SLOT = 0x100, // Copied: the event queue, pointing to a slot for the first event
    LIMIT = 0x21000, GLOBAL = 0x22000, DTCM_REG = 0x23000, MATH = 0x23800, WORDS_SIZE = 0x4000, // Copied
    CACHE_AREA = 0x30000, CACHE = CACHE_AREA + 0x1000, // Live: the code changed flag, then the block cache
    STACK = 0x60000, STACK_TOP = 0x7FF00, END = 0x80000,
    COND_TABLE = 0x200000, MODE_BANKS = COND_TABLE + 0x400, // Fixed
    FAKE_HANDLERS = 0x3E0000, FAKE = 0x3F0000, FAKE_MEMORY = 0x3F0800, FAKE_MSR = 0x3F1800, FAKE_RESTORE = 0x3F1840, FAKE_FETCH = 0x3F1880, FAKE_CP15 = 0x3F18C0, FAKE_INTERLEAVE = 0x3F2000,
    READ_MAP = 0x400000, WRITE_MAP = 0x800000, // Live: memory maps, with host pointers turned into emulator addresses
    CODE = 0x10000000, CODE_SIZE = 0x2000000,
    RAM = 0x20000000, // Main RAM, shared with the core
    CORE = 0x40000000; // The whole core object, shared, for every other memory the maps point to

// The math registers as a big-endian host lays them out: three 64-bit values, the two control registers and the dirty flags
static const uint32_t MATH_NUMER = 0, MATH_DENOM = 8, MATH_PARAM = 16, MATH_DIV_CNT = 24, MATH_SQRT_CNT = 26, MATH_DIV_DIRTY = 28, MATH_SQRT_DIRTY = 29, MATH_GX_STAT = 32;

// Memory generated code doesn't run from, which Unicorn then doesn't watch for code being written over
static const int DATA = UC_PROT_READ | UC_PROT_WRITE;

struct Engine {
    uc_engine *uc = nullptr;
    Jit *jit = nullptr;
    Interpreter *arm = nullptr;
    bool arm7 = false;
    const uint32_t *limit = nullptr; // The limit generated code was entered with
    bool running = false;
    uint8_t **readMap = nullptr, **writeMap = nullptr;
    uint32_t *state = nullptr, *words = nullptr; // The copies, mapped at ARM_STATE and EVENTS
    uint32_t given[sizeof(Interpreter) / 4 + 17]; // The interpreter's words and pointers as they were copied in
};

struct Handler {
    uint8_t raw[16];
    size_t size;
    bool thumb;
};

static Core *core;
static Engine engines[2];
static std::vector<Handler> handlers;
static uint8_t *codeBase, *ramMapped;
static uint32_t ramMappedSize, coreSize;
static uint64_t enters, calls, ppcOps[2];

static void fatal(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stdout);
    _exit(3);
}

static uint32_t swap32(uint32_t v) { return __builtin_bswap32(v); }
static uint32_t reg(uc_engine *uc, int r) { uint32_t v = 0; uc_reg_read(uc, r, &v); return v; }
static void setReg(uc_engine *uc, int r, uint32_t value) { uc_reg_write(uc, r, &value); }

static uint32_t emuCode(void *host) {
    uintptr_t offset = (uint8_t*)host - codeBase;
    if (offset >= CODE_SIZE) fatal("code at %p is outside the codegen area", host);
    return CODE + uint32_t(offset);
}

static uint32_t emuMemory(uint8_t *host) {
    // Where generated code can reach host memory a map points to; anything else goes through the slow path, which does
    // the same as a mapped access
    if (!host) return 0;
    uintptr_t c = host - (uint8_t*)core;
    if (c < coreSize) return CORE + uint32_t(c);
    uintptr_t r = host - ramMapped;
    if (ramMapped && r < ramMappedSize) return RAM + uint32_t(r);
    return 0;
}

static uint32_t emuState(Engine &en, void *host) {
    uintptr_t offset = (uint8_t*)host - (uint8_t*)en.arm;
    if (offset >= sizeof(Interpreter)) fatal("register pointer %p is outside the interpreter", host);
    return ARM_STATE + uint32_t(offset);
}

static uint64_t part(uint32_t word, uint64_t offset, unsigned size) {
    // Live values other than the interpreter's fields are 32-bit words, only read whole
    if (size != 4 || (offset & 3)) fatal("access of %u bytes at offset %llX of a word", size, (unsigned long long)offset);
    return word;
}

// Where the interpreter's words are in the copy: the pointers after it, and its only field read by the byte
static const uint32_t VIRT_WORD = VIRT_REGS / 4, DSI_CYCLE = offsetof(Interpreter, dsiCycle), IME = offsetof(Interpreter, ime);
static const uint32_t IE = offsetof(Interpreter, ie), IRF = offsetof(Interpreter, irf);
static const uint32_t WORD_EVENT = 0, WORD_SLOT = (EVENT_SLOT + offsetof(SchedEvent, cycles)) / 4, WORD_LIMIT = (LIMIT - EVENTS) / 4,
    WORD_GLOBAL = (GLOBAL - EVENTS) / 4, WORD_DTCM = (DTCM_REG - EVENTS) / 4;

static void swapHalves(uint32_t *words) {
    // Exchange the halves of the 64-bit interrupt registers, which a big-endian host has with the high word first
    for (uint32_t field: { IE, IRF }) std::swap(words[field / 4], words[field / 4 + 1]);
}

static void copyIn(Engine &en) {
    // Copy the core's values for generated code, the interpreter's words as a big-endian host would read them
    const uint32_t *arm = (const uint32_t*)en.arm;
    for (size_t i = 0; i < sizeof(Interpreter) / 4; i++)
        en.state[i] = swap32(arm[i]);
    swapHalves(en.state);
    ((uint8_t*)en.state)[DSI_CYCLE] = en.arm->dsiCycle;
    ((uint8_t*)en.state)[IME] = en.arm->ime;
    for (int r = 0; r < 16; r++)
        en.state[VIRT_WORD + r] = swap32(emuState(en, en.arm->registers[r]));
    en.state[VIRT_WORD + 16] = swap32(en.arm->spsr ? emuState(en, en.arm->spsr) : 0);
    memcpy(en.given, en.state, sizeof(Interpreter));
    memcpy(&en.given[sizeof(Interpreter) / 4], &en.state[VIRT_WORD], 17 * 4);
    en.words[WORD_EVENT] = swap32(EVENTS + EVENT_SLOT);
    en.words[WORD_SLOT] = swap32(core->events[0].cycles);
    en.words[WORD_LIMIT] = swap32(*en.limit);
    en.words[WORD_GLOBAL] = swap32(core->globalCycles);
    en.words[WORD_DTCM] = swap32(core->cp15.dtcmReg);
    uint8_t *math = (uint8_t*)en.words + (MATH - EVENTS);
    DivSqrt &div = core->divSqrt;
    auto put8 = [&](uint32_t at, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(math + at, &be, 8); };
    put8(MATH_NUMER, div.divNumer);
    put8(MATH_DENOM, div.divDenom);
    put8(MATH_PARAM, div.sqrtParam);
    math[MATH_DIV_CNT] = div.divCnt >> 8;
    math[MATH_DIV_CNT + 1] = div.divCnt;
    math[MATH_SQRT_CNT] = div.sqrtCnt >> 8;
    math[MATH_SQRT_CNT + 1] = div.sqrtCnt;
    math[MATH_DIV_DIRTY] = div.divDirty;
    math[MATH_SQRT_DIRTY] = div.sqrtDirty;
    uint32_t gxStat = swap32(core->gpu3D.gxStat);
    memcpy(math + MATH_GX_STAT, &gxStat, 4);
}

static void copyOut(Engine &en) {
    // Copy back what generated code changed, which may only be the registers, flags and cycle counts
    uint32_t *arm = (uint32_t*)en.arm;
    const uint32_t usr = offsetof(Interpreter, registersUsr), cpsr = offsetof(Interpreter, cpsr);
    const uint32_t spsrs = offsetof(Interpreter, spsrFiq), cycles = offsetof(Interpreter, cycles);
    swapHalves(en.state);
    swapHalves(en.given);
    for (uint32_t i = 0; i < sizeof(Interpreter) / 4; i++) {
        if (en.state[i] == en.given[i]) continue;
        uint32_t offset = i * 4;
        if (offset == (DSI_CYCLE & ~3) || offset == (IME & ~3)) {
            // Only the byte it's read by may change, since the word holds other fields too
            const uint8_t *now = (const uint8_t*)&en.state[i], *before = (const uint8_t*)&en.given[i];
            for (uint32_t b = 0; b < 4; b++) {
                if (now[b] == before[b]) continue;
                if (offset + b == DSI_CYCLE) en.arm->dsiCycle = now[b];
                else if (offset + b == IME) en.arm->ime = now[b];
                else fatal("write to the interpreter at offset %X", offset + b);
            }
            continue;
        }
        if ((offset < usr || offset > cpsr) && (offset < spsrs || offset > cycles) && (offset < IE || offset >= IRF + 8))
            fatal("write to the interpreter at offset %X", offset);
        arm[i] = swap32(en.state[i]);
    }

    // Only returning from an exception changes the pointers, and only to what the interpreter would have set them to
    bool swapped = false;
    for (int r = 0; r < 17; r++) {
        uint32_t now = en.state[VIRT_WORD + r];
        if (now == en.given[sizeof(Interpreter) / 4 + r]) continue;
        if (r != 13 && r != 14 && r != 16) fatal("write to the interpreter's pointer for register %d", r);
        uint32_t *host = now ? (uint32_t*)((uint8_t*)en.arm + (swap32(now) - ARM_STATE)) : nullptr;
        (r == 16 ? en.arm->spsr : en.arm->registers[r]) = host;
        swapped = true;
    }
    if (swapped) {
        uint32_t *r13 = en.arm->registers[13], *r14 = en.arm->registers[14], *spsr = en.arm->spsr;
        en.arm->swapRegisters(en.arm->cpsr);
        if (r13 != en.arm->registers[13] || r14 != en.arm->registers[14] || spsr != en.arm->spsr)
            fatal("return to mode %02X left the wrong banked registers", en.arm->cpsr & 0x1F);
    }
    if (en.words[WORD_EVENT] != swap32(EVENTS + EVENT_SLOT) || en.words[WORD_SLOT] != swap32(core->events[0].cycles) ||
        en.words[WORD_LIMIT] != swap32(*en.limit) || en.words[WORD_DTCM] != swap32(core->cp15.dtcmReg))
        fatal("write to the event queue, the limit or the DTCM register");
    core->globalCycles = swap32(en.words[WORD_GLOBAL]);
    const uint8_t *math = (const uint8_t*)en.words + (MATH - EVENTS);
    DivSqrt &div = core->divSqrt;
    auto get8 = [&](uint32_t at) { uint64_t be; memcpy(&be, math + at, 8); return __builtin_bswap64(be); };
    div.divNumer = get8(MATH_NUMER);
    div.divDenom = get8(MATH_DENOM);
    div.sqrtParam = get8(MATH_PARAM);
    div.divCnt = (math[MATH_DIV_CNT] << 8) | math[MATH_DIV_CNT + 1];
    div.sqrtCnt = (math[MATH_SQRT_CNT] << 8) | math[MATH_SQRT_CNT + 1];
    div.divDirty = math[MATH_DIV_DIRTY];
    div.sqrtDirty = math[MATH_SQRT_DIRTY];
}

static uint64_t readCache(uc_engine*, uint64_t offset, unsigned size, void *user) {
    // Entries are 16 bytes in generated code, with the key and then the code; the flag is an entry's size before them
    Engine &en = *(Engine*)user;
    if (offset == 0x1000 - sizeof(JitCacheEntry)) return part(en.jit->codeChanged, 0, size);
    if (offset < 0x1000) fatal("read of block cache offset -%llX", (unsigned long long)(0x1000 - offset));
    uint32_t index = (offset - 0x1000) >> 4, field = (offset - 0x1000) & 0xF;
    JitCacheEntry &entry = en.jit->cache[index];
    if (field == 0) return part(entry.key, 0, size);
    if (field == 4) return part(entry.code ? emuCode(entry.code) : 0, 0, size);
    fatal("read of block cache field %u", field);
    return 0;
}

static uint64_t readMap(uc_engine*, uint64_t offset, unsigned size, void *user) {
    if ((offset & 3) || size != 4) fatal("memory map read at offset %llX, size %u", (unsigned long long)offset, size);
    return emuMemory(((uint8_t**)user)[offset >> 2]);
}

static void writeFixed(uc_engine*, uint64_t offset, unsigned, uint64_t, void *user) {
    fatal("write to %s at offset %llX", (const char*)user, (unsigned long long)offset);
}

static void clobberVolatile(uc_engine *uc, bool keepR3) {
    // The ABI lets C++ change these, so generated code can't be relying on them; r3 comes first, so it can be left out
    static const int regs[] = { UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_5, UC_PPC_REG_6, UC_PPC_REG_7, UC_PPC_REG_8,
        UC_PPC_REG_9, UC_PPC_REG_10, UC_PPC_REG_11, UC_PPC_REG_12, UC_PPC_REG_0, UC_PPC_REG_CTR, UC_PPC_REG_CR, UC_PPC_REG_XER };
    static const uint32_t values[] = { 0xDEAD0003, 0xDEAD0004, 0xDEAD0005, 0xDEAD0006, 0xDEAD0007, 0xDEAD0008, 0xDEAD0009,
        0xDEAD000A, 0xDEAD000B, 0xDEAD000C, 0xDEAD0000, 0xDEAD1000, 0x20000000, 0 };
    static void *const pointers[] = { (void*)&values[0], (void*)&values[1], (void*)&values[2], (void*)&values[3],
        (void*)&values[4], (void*)&values[5], (void*)&values[6], (void*)&values[7], (void*)&values[8], (void*)&values[9],
        (void*)&values[10], (void*)&values[11], (void*)&values[12], (void*)&values[13] };
    int skip = keepR3 ? 1 : 0;
    uc_reg_write_batch(uc, regs + skip, pointers + skip, 14 - skip);
}

static void hookCall(uc_engine *uc, uint64_t address, uint32_t, void *user) {
    // Generated code calling into C++, which happens here for real, then returns through the blr at the address
    Engine &en = *(Engine*)user;
    uint32_t r3 = reg(uc, UC_PPC_REG_3), r4 = reg(uc, UC_PPC_REG_4), r5 = reg(uc, UC_PPC_REG_5);
    uint32_t result = 0;
    bool keepR3 = true;
    calls++;
    copyOut(en);
    if (address >= FAKE_HANDLERS && address < FAKE) {
        size_t index = (address - FAKE_HANDLERS) / 4;
        if (index >= handlers.size() || (address & 3)) fatal("call to unknown handler %llX", (unsigned long long)address);
        if (r3 != ARM_STATE) fatal("handler called without the CPU: %08X", r3);
        Handler &h = handlers[index];
        if (h.thumb) {
            int (Interpreter::*func)(uint16_t);
            memcpy(&func, h.raw, sizeof(func));
            result = (en.arm->*func)(r4);
        }
        else {
            int (Interpreter::*func)(uint32_t);
            memcpy(&func, h.raw, sizeof(func));
            result = (en.arm->*func)(r4);
        }
    }
    else if (address >= FAKE && address < FAKE + 48) {
        // Load32, load8, store32, store8, load16, store16, like jitLoad32 and the others
        if (r3 != FAKE_MEMORY) fatal("memory call without the memory object: %08X", r3);
        Memory &mem = core->memory;
        switch ((address - FAKE) / 8) {
            case 0: {
                uint32_t value = mem.read<uint32_t>(en.arm7, r4);
                uint8_t shift = (r4 & 0x3) << 3;
                result = shift ? ((value << (32 - shift)) | (value >> shift)) : value;
                break;
            }
            case 1: result = mem.read<uint8_t>(en.arm7, r4); break;
            case 2: mem.write<uint32_t>(en.arm7, r4, r5); keepR3 = false; break;
            case 3: mem.write<uint8_t>(en.arm7, r4, r5); keepR3 = false; break;
            case 4: result = mem.read<uint16_t>(en.arm7, r4); break;
            case 5: mem.write<uint16_t>(en.arm7, r4, r5); keepR3 = false; break;
        }
    }
    else if (address == FAKE_MSR) {
        if (r3 != ARM_STATE) fatal("MSR called without the CPU: %08X", r3);
        Jit::writeCpsrCall(en.arm, r4, r5);
        keepR3 = false;
    }
    else if (address == FAKE_RESTORE) {
        if (r3 != ARM_STATE) fatal("restore called without the CPU: %08X", r3);
        Jit::restoreCpsrCall(en.arm, r4);
        keepR3 = false;
    }
    else if (address == FAKE_FETCH) {
        // A line of code the ARM9's code cache doesn't hold, like Cp15::codeMissCall
        if (r3 != FAKE_CP15) fatal("fetch called without the coprocessor: %08X", r3);
        result = core->cp15.codeMiss(r4);
    }
    else if (address == FAKE_INTERLEAVE) {
        if (r4 != LIMIT) fatal("interleave called without the limit: %08X", r4);
        result = en.jit->interleave(en.limit);
    }
    else {
        fatal("call to %llX", (unsigned long long)address);
    }
    copyIn(en);
    clobberVolatile(uc, keepR3);
    if (keepR3) setReg(uc, UC_PPC_REG_3, result);
}

uint32_t rigHandler(const void *func, size_t size, bool thumb) {
    // Give each interpreter handler that generated code calls an address of its own
    if (size > sizeof(Handler::raw)) fatal("member function pointer of %zu bytes", size);
    for (size_t i = 0; i < handlers.size(); i++)
        if (handlers[i].size == size && handlers[i].thumb == thumb && !memcmp(handlers[i].raw, func, size))
            return FAKE_HANDLERS + i * 4;
    Handler h = {};
    memcpy(h.raw, func, size);
    h.size = size;
    h.thumb = thumb;
    handlers.push_back(h);
    if (FAKE_HANDLERS + handlers.size() * 4 > FAKE) fatal("too many handlers");
    return FAKE_HANDLERS + (handlers.size() - 1) * 4;
}

static void writeWords(uc_engine *uc, uint32_t address, const uint8_t *from, size_t size) {
    // Generated words are in the Mac's byte order, and the emulator is big-endian
    static std::vector<uint32_t> words;
    words.resize(size / 4);
    memcpy(words.data(), from, size);
    for (uint32_t &word: words) word = swap32(word);
    if (uc_mem_write(uc, address, words.data(), size) != UC_ERR_OK) fatal("couldn't write code at %08X", address);
    uc_ctl_remove_cache(uc, address, address + size);
}

void rigCodeWritten(Jit &jit, uint8_t *start, size_t size, const std::vector<uint32_t*> &patched) {
    // Code is finished being written, so give it to the emulator
    uc_engine *uc = engines[jit.arm7].uc;
    if (start && size) writeWords(uc, emuCode(start), start, size);
    for (uint32_t *site: patched)
        writeWords(uc, emuCode(site), (uint8_t*)site, 4);
}

static void mapRam() {
    // Main RAM is allocated apart from the core, and is bigger in DSi mode
    Memory &mem = core->memory;
    if (mem.ram == ramMapped && mem.ramSize == ramMappedSize) return;
    for (Engine &en: engines) {
        if (ramMapped) uc_mem_unmap(en.uc, RAM, ramMappedSize);
        if (uc_mem_map_ptr(en.uc, RAM, mem.ramSize, DATA, mem.ram) != UC_ERR_OK) fatal("couldn't map main RAM");
    }
    ramMapped = mem.ram;
    ramMappedSize = mem.ramSize;
}

void rigEnter(Jit &jit, const uint32_t *limit, void *code) {
    // Run generated code from a block until it returns to the end address
    Engine &en = engines[jit.arm7];
    if (en.running) fatal("generated code entered again while running");
    mapRam();
    enters++;
    en.running = true;
    en.limit = limit;
    uc_engine *uc = en.uc;
    setReg(uc, UC_PPC_REG_1, STACK_TOP);
    setReg(uc, UC_PPC_REG_3, ARM_STATE);
    setReg(uc, UC_PPC_REG_4, EVENTS);
    setReg(uc, UC_PPC_REG_5, LIMIT);
    setReg(uc, UC_PPC_REG_6, emuCode(code));
    setReg(uc, UC_PPC_REG_7, GLOBAL);
    setReg(uc, UC_PPC_REG_8, CACHE);
    setReg(uc, UC_PPC_REG_LR, END);
    copyIn(en);
    uc_err err = uc_emu_start(uc, emuCode(jit.codeEnter), END, 0, 0);
    copyOut(en);
    uint32_t pc = reg(uc, UC_PPC_REG_PC);
    if (err != UC_ERR_OK || pc != END) {
        uint32_t op = 0;
        uc_mem_read(uc, pc, &op, 4);
        fprintf(stderr, "registers:");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%s r%d %08X", (i % 8) ? "" : "\n", i, reg(uc, UC_PPC_REG_0 + i));
        fprintf(stderr, "\nblock code at %08X, entered at %08X\n", emuCode(code), emuCode(jit.codeEnter));
        fatal("%s generated code stopped: %s at %08X (code offset %X, op %08X), LR %08X", jit.arm7 ? "ARM7" : "ARM9",
            uc_strerror(err), pc, pc - CODE, swap32(op), reg(uc, UC_PPC_REG_LR));
    }
    en.running = false;
}

static void setup(Jit &jit) {
    // Point generated code at the emulator's areas; the interpreter's own offsets stay, apart from the pointers
    jit.offRegs = VIRT_REGS;
    jit.offSpsr = VIRT_SPSR;
    jit.addrCondition = COND_TABLE;
    jit.addrMemory = FAKE_MEMORY;
    jit.addrReadMap = READ_MAP;
    jit.addrWriteMap = WRITE_MAP;
    jit.addrDtcmReg = DTCM_REG;
    jit.addrWriteCpsr = FAKE_MSR;
    jit.addrRestoreCpsr = FAKE_RESTORE;
    jit.addrModeBanks = MODE_BANKS;

    // The JIT's real table of banked registers, with its host pointers turned into where the emulator has them
    Engine &en = engines[jit.arm7];
    for (uint32_t mode = 0; mode < 32; mode++) {
        uint32_t entry[3];
        for (int field = 0; field < 3; field++) {
            uint32_t host = jit.modeBanks[mode][field], offset = host - uint32_t(uintptr_t(en.arm));
            if (host && offset >= sizeof(Interpreter)) fatal("the JIT's table of banked registers points outside the interpreter");
            entry[field] = host ? swap32(ARM_STATE + offset) : 0;
        }
        uc_mem_write(en.uc, MODE_BANKS + mode * 16, entry, sizeof(entry));
    }
    jit.addrGxStat = MATH + MATH_GX_STAT;
    jit.addrDivNumer = MATH + MATH_NUMER;
    jit.addrSqrtParam = MATH + MATH_PARAM;
    jit.addrDivCnt = MATH + MATH_DIV_CNT;
    jit.addrSqrtCnt = MATH + MATH_SQRT_CNT;
    jit.addrDivDirty = MATH + MATH_DIV_DIRTY;
    jit.addrSqrtDirty = MATH + MATH_SQRT_DIRTY;
    jit.addrLoad32 = FAKE;
    jit.addrLoad8 = FAKE + 8;
    jit.addrStore32 = FAKE + 16;
    jit.addrStore8 = FAKE + 24;
    jit.addrLoad16 = FAKE + 32;
    jit.addrStore16 = FAKE + 40;

    // The bytes that say what the code cache holds are in the core, which is mapped whole
    jit.addrCp15 = FAKE_CP15;
    jit.addrCodeLines = CORE + uint32_t((uint8_t*)core->cp15.codeLines - (uint8_t*)core);
    jit.addrCodeMiss = FAKE_FETCH;
}

static void countBlock(uc_engine*, uint64_t, uint32_t size, void *arm7) {
    ppcOps[uintptr_t(arm7)] += size / 4;
}

static bool exactCount; // Count every instruction as it runs, since a block's size isn't how much of it ran
static void countInstruction(uc_engine*, uint64_t, uint32_t, void *arm7) {
    ppcOps[uintptr_t(arm7)]++;
}

static void startEngine(Engine &en, bool arm7) {
    en.arm7 = arm7;
    en.jit = arm7 ? &core->jit7 : &core->jit;
    en.arm = &core->interpreter[arm7];
    en.readMap = arm7 ? core->memory.readMap7 : core->memory.readMap9A;
    en.writeMap = arm7 ? core->memory.writeMap7 : core->memory.writeMap9A;
    if (uc_open(UC_ARCH_PPC, uc_mode(UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN), &en.uc) != UC_ERR_OK) fatal("couldn't start Unicorn");
    uc_engine *uc = en.uc;
    uc_ctl_set_cpu_model(uc, UC_CPU_PPC32_750_V3_1);
    static const char *names[] = { "the block cache", "a memory map" };
    if (posix_memalign((void**)&en.state, 0x1000, STATE_SIZE) || posix_memalign((void**)&en.words, 0x1000, WORDS_SIZE))
        fatal("couldn't allocate the copies of the core's state");
    memset(en.state, 0, STATE_SIZE);
    memset(en.words, 0, WORDS_SIZE);
    bool ok = uc_mem_map_ptr(uc, ARM_STATE, STATE_SIZE, DATA, en.state) == UC_ERR_OK &&
        uc_mem_map_ptr(uc, EVENTS, WORDS_SIZE, DATA, en.words) == UC_ERR_OK &&
        uc_mmio_map(uc, CACHE_AREA, 0x11000, readCache, &en, writeFixed, (void*)names[0]) == UC_ERR_OK &&
        uc_mmio_map(uc, READ_MAP, 0x400000, readMap, en.readMap, writeFixed, (void*)names[1]) == UC_ERR_OK &&
        uc_mmio_map(uc, WRITE_MAP, 0x400000, readMap, en.writeMap, writeFixed, (void*)names[1]) == UC_ERR_OK &&
        uc_mem_map(uc, STACK, END + 0x1000 - STACK, DATA) == UC_ERR_OK &&
        uc_mem_map(uc, COND_TABLE, 0x1000, DATA) == UC_ERR_OK &&
        uc_mem_map(uc, FAKE_HANDLERS, FAKE_INTERLEAVE + 0x1000 - FAKE_HANDLERS, UC_PROT_ALL) == UC_ERR_OK &&
        uc_mem_map(uc, CODE, CODE_SIZE, UC_PROT_ALL) == UC_ERR_OK &&
        uc_mem_map_ptr(uc, CORE, coreSize, DATA, core) == UC_ERR_OK;
    if (!ok) fatal("couldn't set up the emulator's memory");
    uc_mem_write(uc, COND_TABLE, Interpreter::condition, 256);
    std::vector<uint32_t> blr((FAKE_INTERLEAVE + 0x1000 - FAKE_HANDLERS) / 4, swap32(0x4E800020));
    uc_mem_write(uc, FAKE_HANDLERS, blr.data(), blr.size() * 4);
    uc_hook hook, blocks;
    uc_hook_add(uc, &hook, UC_HOOK_CODE, (void*)hookCall, &en, FAKE_HANDLERS, FAKE_INTERLEAVE + 4);

    // Count the PowerPC instructions generated code runs, for comparing builds; a block left early still counts in full
    uc_hook_add(uc, &blocks, exactCount ? UC_HOOK_CODE : UC_HOOK_BLOCK, exactCount ? (void*)countInstruction : (void*)countBlock,
        (void*)uintptr_t(arm7), CODE, CODE + CODE_SIZE - 1);
}

// Set by window.cpp when game.sh links it in for --window
bool (*windowOpen)(const char *title, int position) = nullptr;
void (*windowShow)(const uint32_t *frame) = nullptr;
bool (*windowInput)(int &x, int &y, uint32_t &keys) = nullptr;
int (*windowState)() = nullptr;

static void vramStress() {
    // Read VRAM from another thread like a 2D thread does while the VRAM banks change, which crashes if a read can
    // follow a mapping that's only partly there; first with a slot that's mapped and unmapped, then with two banks
    // overlapping in a slot, which keeps it on the slow path while another bank changes
    Memory &mem = core->memory;
    const int changes = 300000;
    for (int overlap = 0; overlap < 2; overlap++) {
        for (int i = 0; i < 9; i++) mem.writeVramCnt(i, 0);
        if (overlap) {
            mem.writeVramCnt(0, 0x81);
            mem.writeVramCnt(1, 0x81);
        }
        std::atomic<bool> stop(false);
        uint64_t reads = 0;
        std::thread reader([&]() {
            for (uint32_t i = 0; !stop.load(std::memory_order_relaxed); i += 2, reads++)
                mem.read<uint16_t>(0, 0x6000000 + (i & 0x3FFE));
        });
        for (int i = 0; i < changes; i++) {
            mem.writeVramCnt(overlap ? 2 : 0, 0x81);
            mem.writeVramCnt(overlap ? 2 : 0, 0);
        }
        stop.store(true);
        reader.join();
        fprintf(stderr, "vramstress: %d changes with%s overlapping banks, during %llu reads\n", changes, overlap ? "" : "out",
            (unsigned long long)reads);
    }
}

static uint64_t hash(const void *data, size_t size, uint64_t h = 0xCBF29CE484222325ULL) {
    const uint8_t *p = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ULL;
    return h;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        printf("Usage: game <system files folder> <rom, or - for none> <interp|portable|native|clear|set> <frames> [fast] [dsi] [firmware] [press=N] "
            "[start=N] [touch=N] [trace=frame] [ppm=path] [shots=N] [sd=path] [dsplle] [mictone=Hz] [state=save:load] [headphones=N] [results=path] [ram=path] [audio] [wav=path] [keys=frame:key,...] [taps=frame:x:y,...] [wifi] [realtime] [live] [window] [input] [exact] [romtest=path]\n");
        return 2;
    }
    std::string files = argv[1], mode = argv[3];
    int frames = atoi(argv[4]), shots = 0, headphones = -1, press = 0, startKey = 0, touch = 0, traceFrom = -1;
    int stateSave = -1, stateLoad = -1, micTone = 0;
    bool fast = false, dsi = false, firmware = false, window = false, wifi = false, realtime = false, input = false;
    bool threads = false, stress = false, audio = false;
    std::string ppm, results, ramFile, wav, sdImage, romtest;
    std::vector<std::pair<int, int>> inputs; // Frame and key to press then, for 2 frames
    std::vector<std::array<int, 3>> taps; // Frame and bottom screen position to touch then, for 8 frames
    for (int i = 5; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "fast") fast = true;
        else if (arg == "dsi") dsi = true;
        else if (arg == "firmware") firmware = true;
        else if (arg == "live") setvbuf(stdout, nullptr, _IOLBF, 0); // Each line as it's printed, for game.sh's --live
        else if (arg == "window") window = true;
        else if (arg == "input") input = realtime = true; // Mouse and keyboard from the window, at a playable speed
        else if (arg == "realtime") realtime = true; // 60 frames a second, to watch a run or give a network time
        else if (arg == "wifi") wifi = true; // Needs game.sh's --wifi for the real libslirp
        else if (arg == "threads") threads = true; // 2D threads drawing behind the emulator, as the console does
        else if (arg == "audio") audio = true; // Mix the sound, and print a hash of it after the memory
        else if (arg == "vramstress") stress = true; // Only run the VRAM test, see vramStress
        else if (arg == "exact") exactCount = true; // Count the PowerPC instructions one by one, which is much slower
        else if (arg.rfind("press=", 0) == 0) press = atoi(arg.c_str() + 6);
        else if (arg.rfind("start=", 0) == 0) startKey = atoi(arg.c_str() + 6);
        else if (arg.rfind("touch=", 0) == 0) touch = atoi(arg.c_str() + 6);
        else if (arg.rfind("trace=", 0) == 0) traceFrom = atoi(arg.c_str() + 6);
        else if (arg.rfind("ppm=", 0) == 0) ppm = arg.substr(4);
        else if (arg.rfind("shots=", 0) == 0) shots = atoi(arg.c_str() + 6); // Save the screens every N frames, with ppm's name
        else if (arg.rfind("headphones=", 0) == 0) headphones = atoi(arg.c_str() + 11); // Plug them in at frame N
        else if (arg.rfind("sd=", 0) == 0) sdImage = arg.substr(3); // An SD card image for DSi mode, none by default
        else if (arg == "dsplle") Settings::dspHle = 0; // Run Nintendo's DSP programs in Teakra too
        else if (arg.rfind("mictone=", 0) == 0) micTone = atoi(arg.c_str() + 8); // A sine in the microphone, at 32 kHz
        else if (arg.rfind("state=", 0) == 0) { // Save a state before one frame and load it before another
            stateSave = atoi(arg.c_str() + 6);
            stateLoad = atoi(arg.c_str() + arg.find(':') + 1);
        }
        else if (arg.rfind("results=", 0) == 0) results = arg.substr(8);
        else if (arg.rfind("ram=", 0) == 0) ramFile = arg.substr(4);
        else if (arg.rfind("romtest=", 0) == 0) romtest = arg.substr(8); // Where tests/rom's ROM saves its results
        else if (arg.rfind("wav=", 0) == 0) { wav = arg.substr(4); audio = true; }
        else if (arg.rfind("keys=", 0) == 0) {
            // Comma-separated frame:key pairs, keys numbered as KEYINPUT bits, then 10 and 11 for X and Y
            for (size_t at = 5; at < arg.size();) {
                size_t end = arg.find(',', at);
                std::string pair = arg.substr(at, end == std::string::npos ? std::string::npos : end - at);
                inputs.push_back({ atoi(pair.c_str()), atoi(pair.c_str() + pair.find(':') + 1) });
                at = (end == std::string::npos) ? arg.size() : end + 1;
            }
        }
        else if (arg.rfind("taps=", 0) == 0) {
            // Comma-separated frame:x:y triples
            for (size_t at = 5; at < arg.size();) {
                size_t end = arg.find(',', at);
                std::string tap = arg.substr(at, end == std::string::npos ? std::string::npos : end - at);
                size_t c1 = tap.find(':'), c2 = tap.find(':', c1 + 1);
                taps.push_back({ atoi(tap.c_str()), atoi(tap.c_str() + c1 + 1), atoi(tap.c_str() + c2 + 1) });
                at = (end == std::string::npos) ? arg.size() : end + 1;
            }
        }
        else fatal("unknown option %s", arg.c_str());
    }
    if (mode != "interp" && mode != "portable" && mode != "native" && mode != "clear" && mode != "set")
        fatal("unknown mode %s", mode.c_str());

    // Generated code, with the flags that the JIT leaves out for nothing reading them forced to 0 or to 1; if any of
    // them is read after all, one of the two runs differs from the plain one
    rigPoison = (mode == "clear") ? 1 : (mode == "set") ? 2 : 0;
    if (window) {
        if (!windowOpen) fatal("the window needs game.sh's --window");
        if (input && !window) fatal("input needs the window");
        int position = (mode == "interp") ? 0 : (mode == "portable") ? 1 : 2;
        if (!windowOpen(("NooDS U harness: " + mode).c_str(), position)) fatal("couldn't open a window");
    }

    // Run from the current folder, keeping saves there, with everything that could differ between runs fixed
    Settings::load(".");
    Settings::ndsBios9Path = files + "/bios9.bin";
    Settings::ndsBios7Path = files + "/bios7.bin";
    Settings::ndsFirmPath = files + "/firmware.bin";
    Settings::dsiBios9Path = files + "/bios9i.bin";
    Settings::dsiBios7Path = files + "/bios7i.bin";
    Settings::dsiFirmPath = files + "/firmwarei.bin";
    Settings::dsiNandPath = "nand.bin";
    Settings::sdImagePath = sdImage.empty() ? "sd.img" : sdImage;
    Settings::jit = mode != "interp";
    Settings::fastTiming = fast;
    Settings::dsiMode = dsi;
    Settings::directBoot = !firmware;
    Settings::savesFolder = 1;
    Settings::threaded2D = threads;
    Settings::threaded3D = 0;
    Settings::arm7Hle = 0;
    Settings::emulateAudio = audio;
    if (!wav.empty()) {
        // Room for the WAV header, filled in at the end
        if (!(rigAudioFile = fopen(wav.c_str(), "wb"))) fatal("couldn't write %s", wav.c_str());
        fseek(rigAudioFile, 44, SEEK_SET);
    }
    Settings::fpsLimiter = 0;
    Settings::fast2D = threads;
    Settings::highRes3D = 0;
    Settings::hardware3D = 0;
    Settings::screenFilter = 0;
    Settings::wifi = wifi;
    Rtc::fixedTime = 1767225600; // 2026-01-01
#ifdef ROM_TEST
    // tests/rom's ROM writes its results through registers that only exist in this build, and says when it's done
    if (!romtest.empty()) {
        remove(romtest.c_str());
        RomTest::path = romtest;
        RomTest::detail = 1;
    }
#else
    if (!romtest.empty()) fatal("romtest needs JIT_TEST_CFLAGS=-DROM_TEST");
#endif
    try {
        core = new Core(std::string(argv[2]) == "-" ? "" : argv[2]);
    }
    catch (CoreError error) {
        fatal("couldn't start the core: %d", int(error));
    }

    // Native code runs in the emulator; the portable executor runs when there's no code area
    void *area;
    uint32_t areaSize;
    OSGetCodegenVirtAddrRange(&area, &areaSize);
    codeBase = (uint8_t*)area;
    coreSize = (sizeof(Core) + 0xFFF) & ~0xFFF;
    if (sizeof(Interpreter) > VIRT_REGS) fatal("the interpreter is too big for its area");
    rigSetup = setup;
    startEngine(engines[0], false);
    startEngine(engines[1], true);
    if (mode == "portable") core->jit.codeFailed = core->jit7.codeFailed = true;
    if (stress) {
        vramStress();
        _exit(0);
    }

    // From the frame to trace, also print the CPUs' registers at the start of each scanline, to narrow down a difference
    static int frameNow = 0;
    static std::function<void()> scanline = core->tasks[NDS_SCANLINE256];
    if (traceFrom >= 0) core->tasks[NDS_SCANLINE256] = [traceFrom]() {
        if (frameNow >= traceFrom) {
            for (int c = 0; c < 2; c++) {
                Interpreter &arm = core->interpreter[c];
                printf("line %d arm%d cycles %u/%u cpsr %08X", core->gpu.vCount, c ? 7 : 9, core->globalCycles, arm.cycles,
                    arm.cpsr);
                for (int r = 0; r < 16; r++) printf(" %08X", *arm.registers[r]);
                printf("\n");
            }
        }
        scanline();
    };

    // Print a line per frame that the runs can be compared by
    static uint32_t frame[256 * 192 * 2 * 4];
    auto savePpm = [](const std::string &path) {
        FILE *out = fopen(path.c_str(), "wb");
        fprintf(out, "P6\n256 384\n255\n");
        for (int p = 0; p < 256 * 384; p++) {
            uint8_t rgb[3] = { uint8_t(frame[p]), uint8_t(frame[p] >> 8), uint8_t(frame[p] >> 16) };
            fwrite(rgb, 1, 3, out);
        }
        fclose(out);
    };
    uint64_t last = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; i++) {
        // Quitting from the menu bar ends the run early, and so does closing the window when it gives the input
        if (window && (windowState() == 2 || (input && windowState() == 1))) {
            frames = i;
            break;
        }
        frameNow = i;
        if (press && i % press == 0) core->input.pressKey(0);
        if (press && i % press == 2) core->input.releaseKey(0);
        if (startKey && i % startKey == startKey - 1) core->input.pressKey(3);
        if (startKey && i % startKey == 1) core->input.releaseKey(3);
        for (auto &input: inputs) {
            if (i == input.first) core->input.pressKey(input.second);
            if (i == input.first + 2) core->input.releaseKey(input.second);
        }
        if (touch && i % touch == 0) { core->input.pressScreen(); core->spi.setTouch(128, 96); }
        if (touch && i % touch == 10) { core->input.releaseScreen(); core->spi.clearTouch(); }
        if (input) {
            // Press and release what changed in the window since the last frame
            static uint32_t held = 0;
            static bool touching = false;
            int x, y;
            uint32_t keys;
            bool touch = windowInput(x, y, keys);
            for (int k = 0; k < 12; k++) {
                if ((keys & ~held) & (1 << k)) core->input.pressKey(k);
                if ((held & ~keys) & (1 << k)) core->input.releaseKey(k);
            }
            held = keys;
            if (touch) { core->input.pressScreen(); core->spi.setTouch(x, y); }
            else if (touching) { core->input.releaseScreen(); core->spi.clearTouch(); }
            touching = touch;
        }
        if (i == headphones) DsiGpio::headphonesIn.store(true);
        if (micTone) {
            // A frame of the tone, the way the console sends the GamePad's microphone
            static uint32_t phase = 0;
            int16_t tone[534];
            uint32_t count = (i + 1) * 32000 / 60 - i * 32000 / 60;
            for (uint32_t j = 0; j < count; j++, phase++)
                tone[j] = int16_t(8000 * sin(2 * M_PI * micTone * phase / 32000));
            core->spi.sendMicData(tone, count, 32000);
        }
        for (auto &tap: taps) {
            if (i == tap[0]) { core->input.pressScreen(); core->spi.setTouch(tap[1], tap[2]); }
            if (i == tap[0] + 8) { core->input.releaseScreen(); core->spi.clearTouch(); }
        }
        if (i == stateSave) {
            core->saveStates.setPath("test.state", false);
            printf("state saved at frame %d: %d\n", i, core->saveStates.saveState());
        }
        if (i == stateLoad) printf("state loaded at frame %d: %d\n", i, core->saveStates.loadState());
        do core->runCore(); while (!core->frameEnded.exchange(false));
        if (realtime) std::this_thread::sleep_until(start + std::chrono::microseconds(int64_t(i + 1) * 1000000 / 60));
        if (core->gpu.getFrame(frame, false)) {
            last = hash(frame, 256 * 192 * 2 * 4);
            if (window) windowShow(frame);
        }
        if (shots && !ppm.empty() && i % shots == 0) savePpm(ppm + "." + std::to_string(i) + ".ppm");

        // The DSi asking to be reset, as it does to go back to the menu, ends the run, since the harness can't start
        // a new core; the NAND is left as it was for the next run
#ifdef ROM_TEST
        if (RomTest::done) {
            printf("the ROM finished at frame %d\n", i);
            frames = i + 1;
            break;
        }
#endif
        if (core->restartRequested.load()) {
            printf("restart requested at frame %d\n", i);
            frames = i + 1;
            break;
        }
        Interpreter &arm9 = core->interpreter[0], &arm7 = core->interpreter[1];
        printf("frame %d\tscreen %016llX\tcycles %u %u %u\tpc %08X %08X", i, (unsigned long long)last,
            core->globalCycles, arm9.cycles, arm7.cycles, arm9.registersUsr[15], arm7.registersUsr[15]);
        if (audio) printf("\taudio %016llX", (unsigned long long)rigAudioHash);
        printf("\n");
    }
    if (rigAudioFile) {
        // Fill in the header now that the length is known
        uint32_t bytes = ftell(rigAudioFile) - 44, rate = 32768, wide = 4 | (uint32_t(16) << 16);
        uint32_t words[11] = { 0x46464952, bytes + 36, 0x45564157, 0x20746D66, 16, 0x00020001, rate, rate * 4, wide, 0x61746164, bytes };
        fseek(rigAudioFile, 0, SEEK_SET);
        fwrite(words, 4, 11, rigAudioFile);
        fclose(rigAudioFile);
        rigAudioFile = nullptr;
    }
    core->cartridgeNds.writeSave();
    Memory &mem = core->memory;
    printf("ram %016llX\twram %016llX\n", (unsigned long long)hash(mem.ram, mem.ramSize),
        (unsigned long long)hash(mem.wram7, sizeof(mem.wram7), hash(mem.wram, sizeof(mem.wram))));
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    fprintf(stderr, "%s: %d frames in %.1f s, %llu entries into generated code, %llu calls from it, %llu and %llu PowerPC "
        "instructions run for the ARM9 and ARM7\n", mode.c_str(), frames, seconds, (unsigned long long)enters,
        (unsigned long long)calls, (unsigned long long)ppcOps[0], (unsigned long long)ppcOps[1]);
    if (mode != "interp" && mode != "portable")
        fprintf(stderr, "%s: %llu flag-setting instructions were generated without their flags, and %llu with only the carry\n",
            mode.c_str(), (unsigned long long)rigFlagStats[0], (unsigned long long)rigFlagStats[1]);
    if (!ramFile.empty()) {
        FILE *out = fopen(ramFile.c_str(), "wb");
        fwrite(mem.ram, 1, mem.ramSize, out);
        fclose(out);
    }
    if (!results.empty()) {
        // What tests/hardware's ROM records for each CPU, which its build takes as the results to compare with; they're
        // in a block in main RAM marked with two words
        uint32_t block = 0;
        for (uint32_t a = 0x02000000; a < 0x02000000 + mem.ramSize && !block; a += 32)
            if (mem.read<uint32_t>(0, a) == 0x54535748 && mem.read<uint32_t>(0, a + 4) == 0x544C5352) block = a;
        if (!block) fatal("no results from tests/hardware's ROM");
        FILE *out = fopen(results.c_str(), "wb");
        for (uint32_t a = 0; a < 2 * 110 * 12 * 4; a += 4) {
            uint32_t word = mem.read<uint32_t>(0, block + 32 + a);
            fwrite(&word, 4, 1, out);
        }
        fclose(out);
    }
    if (!ppm.empty()) savePpm(ppm);
    core->sdMmc.flush(); // As the frontend does when it stops, since the core isn't deleted
    fflush(stdout);
    _exit(0);
}
