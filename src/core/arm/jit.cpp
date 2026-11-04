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

#include <cstring>
#include <coreinit/core.h>

#include "../core.h"
#include "../emu_profile.h"

// The most instructions to put in one block, to limit how much is recompiled when code changes
static const int MAX_INSTRS = 64;

#ifdef JIT_LOG
#include <cstdarg>
#include <coreinit/thread.h>

void jitLog(const char *format, ...) {
    // Append a line and close the file every time, so it survives a freeze
    static int lines = 0;
    if (++lines > 2000) return;
    FILE *file = fopen((Settings::basePath + "/jit.log").c_str(), lines == 1 ? "w" : "a");
    if (!file) return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}
#endif

bool Jit::testing = false;

Jit::Jit(Core *core, bool arm7): core(core), arm7(arm7) {}

Jit::~Jit() {
    // Free all blocks and give up the codegen area
    freeCodegen();
    for (size_t i = 0; i < table.size(); i++)
        delete table[i];
    for (size_t i = 0; i < graveyard.size(); i++)
        delete graveyard[i];
}

void Jit::enable(bool value) {
    // Only run code compiled in regular DS mode
    if (value && (core->dsiMode || core->gbaMode))
        value = false;
    if (!value) reset();
    enabled = value;

    // Set up the memory that code is compiled from, with the writable parts first so their pages are numbered first
    Memory &mem = core->memory;
    regionCount = 0;
    regions[regionCount++] = { mem.ram, RAM_SIZE, true };
    if (arm7) {
        regions[regionCount++] = { mem.wram, 0x8000, true };
        regions[regionCount++] = { mem.wram7, 0x10000, true };
        regions[regionCount++] = { mem.bios7, 0x4000, false };
    }
    else {
        regions[regionCount++] = { mem.instrTcm, 0x8000, true };
        regions[regionCount++] = { mem.bios9, 0x10000, false };
    }
    tableSize = 0;
    for (int i = 0; i < regionCount; i++)
        tableSize += regions[i].size;
    if (enabled && table.empty())
        table.resize(tableSize / 2);
}

void Jit::reset() {
    EP_INC(EC_JIT_RESETS);
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
    EmuProfile::traceBlock(EmuProfile::TRACE_RESET, arm7, 0, codeStart, codeEnd, nullptr, 0);
#endif
    // Free all blocks and forget which pages have code
    for (size_t i = 0; i < table.size(); i++) {
        if (table[i]) graveyard.push_back(table[i]);
        table[i] = nullptr;
    }
    for (int i = 0; i < MAX_PAGES; i++)
        pages[i] = JitPage();
    pendingLinks.clear();
    unlinks.clear();
    clearCache();
    codeChanged = 1;
    codePtr = codeStart;

    // Map the pages that were unmapped for having code, which the other CPU's code might still keep unmapped
    if (codePages) {
        codePages = 0;
        core->memory.updateMap9(0x00000000, 0x04000000);
        core->memory.updateMap7(0x02000000, 0x04000000);
    }
}

uint8_t **Jit::codeMap() {
    // Get the memory map that this CPU fetches code through
    return arm7 ? core->memory.readMap7 : core->memory.readMap9A;
}

int Jit::tableIndex(uint8_t *host) {
    // Get the lookup table position for a host address, or -1 if code isn't compiled from there
    // Regions are laid out one after another in the table
    for (int i = 0, offset = 0; i < regionCount; offset += regions[i++].size)
        if (uintptr_t(host - regions[i].base) < regions[i].size)
            return (offset + (host - regions[i].base)) >> 1;
    return -1;
}

int Jit::pageIndex(uint8_t *host) {
    // Get the code page for a host address, or -1 if it's not in writable code memory
    for (int i = 0, page = 0; i < regionCount && regions[i].writable; page += regions[i++].size >> 12)
        if (uintptr_t(host - regions[i].base) < regions[i].size)
            return page + ((host - regions[i].base) >> 12);
    return -1;
}

uint8_t *Jit::pageHost(int page) {
    // Get the host memory of a code page
    for (int i = 0; i < regionCount; page -= regions[i++].size >> 12)
        if (page < int(regions[i].size >> 12))
            return &regions[i].base[page << 12];
    return nullptr;
}

void Jit::clearCache() {
    // Empty the cache of blocks by guest address
    memset(cache, 0, CACHE_SIZE * sizeof(JitCacheEntry));
}

void Jit::mapChanged(uint32_t start) {
    // Forget cached guest addresses if ITCM or WRAM might have been remapped, and stop any running code
    // Main RAM and the BIOS are always at the same addresses, so other changes don't matter
    if (start >= (arm7 ? 0x04000000 : 0x02000000)) return;
    clearCache();
    codeChanged = 1;

    // Linked jumps might go to code for the old mapping, so send them all through the dispatcher again
    for (size_t i = 0; i < table.size(); i++)
        if (table[i]) unlinkIncoming(table[i]);
}

bool Jit::isCodePage(uint8_t *host) {
    // Check if a host address is in a page that has compiled code
    int page = pageIndex(host);
    return page >= 0 && !pages[page].blocks.empty();
}

bool Jit::lineHasCode(uint8_t *host) {
    // Check if any of the 32-byte line around a host address is compiled code
    int page = pageIndex(host);
    if (page < 0) return false;
    uint32_t start = (uintptr_t(host) & 0xFE0) >> 1;
    for (uint32_t i = start; i < start + 16; i++)
        if (pages[page].covered[i]) return true;
    return false;
}

bool Jit::endsBlock(uint32_t op, bool thumb) {
    // Check if an instruction always changes the program counter or CPU state, so nothing after it can run
    // Conditional branches don't end blocks, since execution continues if they aren't taken
    if (thumb) {
        if ((op & 0xF800) == 0xE000) return true; // B label
        if ((op & 0xFF00) == 0xDF00) return true; // SWI #i
        if ((op & 0xE800) == 0xE800) return true; // BL/BLX label (second half)
        if ((op & 0xFF00) == 0x4700) return true; // BX/BLX Rs
        if ((op & 0xFC87) == 0x4487) return true; // ADD/CMP/MOV PC,Rs
        if ((op & 0xFF00) == 0xBD00) return true; // POP {Rlist,PC}
        return false;
    }

    // Stop at anything unusual, like coprocessor or status register accesses, to be safe
    if ((op >> 28) == 0xF) return true; // Reserved condition, including BLX label
    if ((op & 0x0F000000) == 0x0F000000) return true; // SWI #i
    if ((op & 0x0F000010) == 0x0E000010) return true; // MRC/MCR
    if ((op & 0x0D900000) == 0x01000000) return true; // MRS/MSR, BX, BLX and other miscellaneous opcodes
    if ((op >> 28) != 0xE) return false;
    if ((op & 0x0E000000) == 0x0A000000) return true; // B/BL label
    if ((op & 0x0C00F000) == 0x0000F000) return true; // Data processing or loads with Rd = PC
    if ((op & 0x0C10F000) == 0x0410F000) return true; // LDR PC
    if ((op & 0x0E108000) == 0x08108000) return true; // LDM with PC
    return false;
}

JitBlock *Jit::compile(uint32_t address, bool thumb, uint8_t *host, int index) {
    JitBlock *block = new JitBlock();
    block->address = address;
    block->thumb = thumb;
    block->start = host;
    block->index = index;

    // If this starts in the middle of a block with generated code, reuse that block's code from there
    // Its instructions are the same, so only a small entry point is needed instead of another copy
    int page = pageIndex(host);
    uint32_t step = thumb ? 2 : 4;
    for (size_t i = 0; page >= 0 && i < pages[page].blocks.size(); i++) {
        JitBlock *other = pages[page].blocks[i];
        if (other->thumb != thumb || !other->code || host <= other->start || host >= other->end) continue;
        size_t skip = (host - other->start) / step;
        if ((host - other->start) % step || other->address + skip * step != address) continue;
        if (void *code = generateEntry(other, skip)) {
            block->instrs.assign(other->instrs.begin() + skip, other->instrs.end());
            block->end = other->end;
            block->code = code;
            linkEntry(block);
            break;
        }
    }

    // Otherwise decode instructions until one that ends the block, the end of the page, or the size limit
    // The page is the guest's, since host memory isn't page aligned and the next guest page can be somewhere else
    uint8_t *pageEnd = host + (0x1000 - (address & 0xFFF));
    uint8_t *ptr = host;
    while (!block->code && ptr < pageEnd && block->instrs.size() < MAX_INSTRS) {
        JitInstr instr;
        if (thumb) {
            instr.opcode = readLsb<uint16_t>(ptr);
            instr.thumb = Interpreter::thumbInstrs[(instr.opcode >> 6) & 0x3FF];
            ptr += 2;
        }
        else {
            instr.opcode = readLsb<uint32_t>(ptr);
            instr.arm = Interpreter::armInstrs[((instr.opcode >> 16) & 0xFF0) | ((instr.opcode >> 4) & 0xF)];
            ptr += 4;
        }
        block->instrs.push_back(instr);
        if (endsBlock(instr.opcode, thumb)) break;
    }
    if (!block->code) block->end = ptr;

    // Replace any block already at this position
    if (table[index]) retire(table[index]);
    table[index] = block;

    // Track the block in its page, unmapping the page for writes if it's the first code there
    if (page >= 0) {
        JitPage &p = pages[page];
        if (p.blocks.empty()) {
            p.blocks.push_back(block);
            p.overwrites = 0;
            protect(page);
        }
        else {
            p.blocks.push_back(block);
        }
        for (uint8_t *i = block->start; i < block->end; i += 2) {
            uint8_t &count = p.covered[(uintptr_t(i) & 0xFFF) >> 1];
            if (count == 0xFF) p.saturated = true;
            else count++;
        }
        core->memory.forgetCleanLines();
    }

    // Generate host code for the block if a backend is available
    if (!block->code) block->code = generate(block);
    return block;
}

void Jit::retire(JitBlock *block) {
    // Remove a block from the lookup table and its page, and delete it once it's not running
    if (table[block->index] == block)
        table[block->index] = nullptr;
    int page = pageIndex(block->start);
    if (page >= 0) {
        std::vector<JitBlock*> &blocks = pages[page].blocks;
        for (size_t i = 0; i < blocks.size(); i++) {
            if (blocks[i] != block) continue;
            blocks.erase(blocks.begin() + i);
            break;
        }

        // Take the block out of the coverage counts, or count the remaining blocks again if they were saturated
        JitPage &p = pages[page];
        if (p.saturated) {
            memset(p.covered, 0, sizeof(p.covered));
            p.saturated = false;
            for (size_t i = 0; i < blocks.size(); i++) {
                for (uint8_t *j = blocks[i]->start; j < blocks[i]->end; j += 2) {
                    uint8_t &count = p.covered[(uintptr_t(j) & 0xFFF) >> 1];
                    if (count == 0xFF) p.saturated = true;
                    else count++;
                }
            }
        }
        else {
            for (uint8_t *j = block->start; j < block->end; j += 2)
                p.covered[(uintptr_t(j) & 0xFFF) >> 1]--;
        }
        if (blocks.empty()) codePages--;
    }
    JitCacheEntry &entry = cache[(block->address >> 1) & (CACHE_SIZE - 1)];
    if (entry.block == block) entry = JitCacheEntry();
    unlinkIncoming(block);
    removeLinks(block);
    codeChanged = 1;
    graveyard.push_back(block);
    EP_INC(EC_JIT_RETIRED);
}

void Jit::unlinkIncoming(JitBlock *block) {
    // Send jumps linked to a block through the dispatcher again, waiting to relink if it's compiled again
    // Patching code has to wait until nothing is running, so it's queued
    uint32_t key = block->address | block->thumb;
    for (size_t i = 0; i < block->incoming.size(); i++) {
        JitBlock *owner = block->incoming[i].first;
        uint32_t *site = block->incoming[i].second;
        for (size_t j = 0; j < owner->links.size(); j++)
            if (owner->links[j].site == site) owner->links[j].target = nullptr;
        pendingLinks.insert(std::make_pair(key, block->incoming[i]));
        unlinks.push_back(site);
    }
    block->incoming.clear();
}

void Jit::removeLinks(JitBlock *block) {
    // Forget the jumps in a block's code, which won't run again
    for (size_t i = 0; i < block->links.size(); i++) {
        JitLink &link = block->links[i];
        if (link.target) {
            std::vector<std::pair<JitBlock*, uint32_t*>> &in = link.target->incoming;
            for (size_t j = 0; j < in.size(); j++) {
                if (in[j].second != link.site) continue;
                in.erase(in.begin() + j);
                break;
            }
        }
        else {
            auto range = pendingLinks.equal_range(link.key);
            for (auto it = range.first; it != range.second; it++) {
                if (it->second.second != link.site) continue;
                pendingLinks.erase(it);
                break;
            }
        }
    }
    block->links.clear();
}

JitBlock *Jit::lookup(uint32_t address, bool thumb) {
    // Find a compiled block with generated code for a guest address, if there is one
    uint8_t *host = codeMap()[address >> 12];
    if (!host) return nullptr;
    int index = tableIndex(host + (address & 0xFFF));
    if (index < 0) return nullptr;
    JitBlock *block = table[index];
    return (block && block->address == address && block->thumb == thumb && block->code) ? block : nullptr;
}

void Jit::linkBlock(JitBlock *block, std::vector<uint32_t*> &patched) {
    // Link a new block's jumps to blocks that already exist, and link waiting jumps to the new block
    // This is called while the codegen area is writable, and the patched jumps need flushing
    for (size_t i = 0; i < block->links.size(); i++) {
        JitLink &link = block->links[i];
        JitBlock *target = lookup(link.key & ~1, link.key & 1);
        if (target && target != block) {
            patchJump(link.site, target->code);
            link.target = target;
            target->incoming.push_back(std::make_pair(block, link.site));
            patched.push_back(link.site);
        }
        else {
            pendingLinks.insert(std::make_pair(link.key, std::make_pair(block, link.site)));
        }
    }
    auto range = pendingLinks.equal_range(block->address | block->thumb);
    for (auto it = range.first; it != range.second;) {
        JitBlock *owner = it->second.first;
        uint32_t *site = it->second.second;
        patchJump(site, block->code);
        for (size_t j = 0; j < owner->links.size(); j++)
            if (owner->links[j].site == site) owner->links[j].target = block;
        block->incoming.push_back(it->second);
        patched.push_back(site);
        it = pendingLinks.erase(it);
    }
}

void Jit::protect(int page) {
    // Unmap every mirror of a page from the write maps, so writes to it go through the JIT
    // Mirrors are only unmapped if they point to the page, in case something else is mapped over them
    // Main RAM and WRAM can be written by either CPU; ITCM only appears in the ARM9's lower addresses
    Memory &mem = core->memory;
    uint8_t *host = pageHost(page);
    codePages++;
    if (uintptr_t(host - mem.instrTcm) < 0x8000) {
        for (uint32_t i = 0; i < 0x02000; i++)
            if (mem.writeMap9A[i] == host) mem.writeMap9A[i] = nullptr;
        return;
    }
    uint32_t start = (uintptr_t(host - mem.ram) < RAM_SIZE) ? 0x02000 : 0x03000;
    for (uint32_t i = start; i < start + 0x1000; i++) {
        if (mem.writeMap9A[i] == host) mem.unmapJit(mem.writeMap9A, i);
        if (mem.writeMap9B[i] == host) mem.unmapJit(mem.writeMap9B, i);
        if (mem.writeMap7[i] == host) mem.unmapJit(mem.writeMap7, i);
    }
}

bool Jit::writeCode(uint8_t *host, int size) {
    // Check if a write overlaps compiled code, and return whether the page still has code after it
    int page = pageIndex(host);
    if (page < 0) return false;
    JitPage &p = pages[page];
    bool overlap = false;
    for (int i = 0; i < size; i += 2)
        if (p.covered[((uintptr_t(host) + i) & 0xFFF) >> 1]) overlap = true;

    // Invalidate every block that overlaps the write
    // After several such writes, the page is probably being loaded over, so drop all of its code; the page can then be
    // mapped back, which makes the rest of the writes much faster
    // Code that overwrites its own page would keep compiling itself again, so each time takes twice as many writes,
    // until it's more than a page can hold and blocks only go when they're overwritten
    if (overlap) {
        bool clear = ++p.overwrites >= (8 << p.cleared);
        if (clear) {
            p.overwrites = 0;
            if (p.cleared < 10) p.cleared++;
        }
        hitBlocks.clear();
        for (size_t i = 0; i < p.blocks.size(); i++)
            if (clear || (host < p.blocks[i]->end && host + size > p.blocks[i]->start))
                hitBlocks.push_back(p.blocks[i]);
        for (size_t i = 0; i < hitBlocks.size(); i++)
            retire(hitBlocks[i]);
    }
    return !p.blocks.empty();
}

bool Jit::run(const uint32_t *limit) {
    // Put back unlinked jumps and delete blocks that were invalidated, now that none are running
    if (!unlinks.empty()) applyUnlinks();
    for (size_t i = 0; i < graveyard.size(); i++)
        delete graveyard[i];
    graveyard.clear();

    // Set up code generation the first time it's possible, since it only works from one CPU core
    // This happens here so it's done by the thread that runs the emulator
#ifdef JIT_LOG
    static bool logged = false;
    if (!logged) {
        logged = true;
        JLOG("run: core %u, thread %p, priority %d, affinity %X", OSGetCoreId(), OSGetCurrentThread(),
            OSGetThreadPriority(OSGetCurrentThread()), OSGetThreadAffinity(OSGetCurrentThread()));
    }
#endif
    if (!codeArea && !codeFailed)
        initCodegen();

    // Code generated with fast timing counts cycles differently, so start over if the setting changed
    if (bool(Settings::fastTiming) != codeFast) {
        reset();
        for (size_t i = 0; i < graveyard.size(); i++)
            delete graveyard[i];
        graveyard.clear();
        codeFast = !codeFast;
    }

    // Find the host memory for the next instruction, if it's somewhere code can be compiled from
    Interpreter &arm = core->interpreter[arm7];
    bool thumb = arm.cpsr & BIT(5);
    uint32_t address = arm.registersUsr[15] - (thumb ? 2 : 4);
    uint8_t *host = codeMap()[address >> 12];
    if (!host) return false;
    host += address & 0xFFF;
    int index = tableIndex(host);
    if (index < 0) return false;

    // Look up the block, compiling it if it's missing or was compiled for another address or mode
    // If generated code is close to filling its area, start over to make room
    JitBlock *block = table[index];
    if (!block || block->address != address || block->thumb != thumb) {
        if (codeFull()) {
            reset();
            for (size_t i = 0; i < graveyard.size(); i++)
                delete graveyard[i];
            graveyard.clear();
        }
        EP_START(comp);
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
        uint8_t *codeBefore = codePtr;
#endif
        block = compile(address, thumb, host, index);
        EP_ADD(EP_COMPILE, comp);
        EP_INC(EC_JIT_COMPILED);
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
        // Log the block with the code it generated, for the frame tracer
        uint32_t opcodes[MAX_INSTRS];
        uint32_t count = std::min<size_t>(block->instrs.size(), MAX_INSTRS);
        for (uint32_t i = 0; i < count; i++)
            opcodes[i] = block->instrs[i].opcode;
        EmuProfile::traceBlock(EmuProfile::TRACE_BLOCK, arm7, address | thumb, codeBefore, codePtr, opcodes, count);
#endif
    }

    // Cache the block so running code can jump to it, and run it
    JitCacheEntry &entry = cache[(address >> 1) & (CACHE_SIZE - 1)];
    entry.key = address | thumb;
    entry.code = block->code;
    entry.block = block;
    execute(block, limit);
    return true;
}

int Jit::interleave(const uint32_t *limit) {
    // Called after an instruction when the next one can't run straight away, with the global cycles
    // still at the start of that instruction; returns whether the CPU can continue
    // The ARM7 always stops, letting its run loop decide what's next, since only the ARM9 or a task can be due
    // With only the ARM9 running, it stops, and the run loop catches up the global cycles
    if (arm7) return 0;
    // With fast timing, the ARM7 catches up from behind and the ARM9 gets a new slice, like the run loop does
    if (limit == &core->sliceLimit) {
        if (!Interpreter::runArm7Behind(*core) || codeChanged) return 0;
        core->sliceLimit = core->interpreter[1].cycles + Core::FAST_SLICE;
        return 1;
    }
    if (limit != &core->interpreter[1].cycles)
        return 0;

    // Otherwise run the ARM7 like the interpreter's loop would, until the ARM9 or a task is due
    return Interpreter::runArm7UntilArm9(*core) && !codeChanged;
}

int Jit::interleaveCall(Jit *jit, const uint32_t *limit) {
    // Entry point for generated code
    return jit->interleave(limit);
}

void Jit::execute(JitBlock *block, const uint32_t *limit) {
    // Let jumps skip filling the pipeline, since it only matters once the interpreter takes over
    Interpreter &arm = core->interpreter[arm7];
    arm.lazyPipeline = true;
    running = true;
    codeChanged = 0;

    // Run generated code if there is any, which only works on the codegen core
    if (block->code && OSGetCoreId() == codeCore) {
#ifdef JIT_LOG
        static uint32_t entries = 0;
        bool log = (++entries <= 200) || (entries % 100000) == 0;
        if (log) JLOG("enter %u: %08X %s code %p", entries, block->address, block->thumb ? "T" : "A", block->code);
#endif
        HB_SET(HB_JIT_BLOCK, block->address);
        HB_SET(HB_JIT_INSIDE, 1);
        HB_INC(HB_JIT_ENTRIES);
        ((void(*)(Interpreter*, EventQueue*, const uint32_t*, void*, uint32_t*, JitCacheEntry*))
            codeEnter)(&arm, &core->events, limit, block->code, &core->globalCycles, cache);
        HB_SET(HB_JIT_INSIDE, 0);
#ifdef JIT_LOG
        if (log) JLOG("exit %u: pc %08X cycles %u", entries, arm.registersUsr[15], arm.cycles);
#endif
        block = nullptr;
    }

    // Run each instruction like the interpreter does, setting the program counter as if it was fetched
    // When the next instruction can't follow straight away, the other CPU and scheduler are handled like the
    // interpreter would; otherwise execution continues into the next block if it's cached
    while (block) {
        uint32_t step = block->thumb ? 2 : 4;
        uint32_t pc = block->address + step * 2;
        size_t count = block->instrs.size();
        bool stop = false;
        for (size_t i = 0; i < count; i++, pc += step) {
            JitInstr &instr = block->instrs[i];
            arm.registersUsr[15] = pc;
            if (!arm7) EP_INC(EC_INTERP9);
            int cycles;
            if (block->thumb) {
                cycles = (arm.*instr.thumb)(instr.opcode);
            }
            else {
                switch (Interpreter::condition[((instr.opcode >> 24) & 0xF0) | (arm.cpsr >> 28)]) {
                    case 0: cycles = 1; break; // False
                    case 2: cycles = arm.handleReserved(instr.opcode); break; // Reserved
                    default: cycles = (arm.*instr.arm)(instr.opcode); break;
                }
            }

            // Count the cycles, doubled for the half-speed ARM7, and continue to the next instruction if nothing
            // else needs to run first
            uint32_t next = core->globalCycles + (cycles << arm7);
            arm.cycles = next;
            if (next > *limit || next >= core->events[0].cycles || codeChanged) {
                if (!interleave(limit)) {
                    stop = true;
                    break;
                }
            }
            else {
                core->globalCycles = next;
            }
            if (arm.registersUsr[15] != pc) break; // Jumped
        }
        if (stop) break;

        // Find the next block in the cache, or stop and let the run loop look it up
        bool thumb = arm.cpsr & BIT(5);
        uint32_t address = arm.registersUsr[15] - (thumb ? 2 : 4);
        JitCacheEntry &entry = cache[(address >> 1) & (CACHE_SIZE - 1)];
        if (entry.key != (address | thumb) || !entry.block)
            break;
        block = entry.block;
    }

    // The program counter is always past the next instruction here, but the pipeline needs filling
    arm.pipelineStale = true;
    arm.lazyPipeline = false;
    running = false;
}
