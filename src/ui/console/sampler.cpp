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

#if defined(__WIIU__) && defined(PROFILE_SAMPLER)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <coreinit/alarm.h>
#include <coreinit/codegen.h>
#include <coreinit/context.h>
#include <coreinit/dynload.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include "sampler.h"
#include "../../core/worker_thread.h"

// Function symbols from an earlier build of the same code, generated from its ELF, if provided
// They give the link-time address of each function and of the alarm callback, which is used to find where the
// loader put the code; values are volatile so a filled-in table doesn't change any code from that build
#if __has_include("sampler_symbols.inc")
#include "sampler_symbols.inc"
#else
static volatile const uint32_t symCallback = 0;
static volatile const uint32_t symCount = 0;
static const uint32_t symAddrs[1] = {};
static const char *const symNames[1] = { "" };
static volatile const uint32_t osNameCount = 0;
static const char *const osNames[1] = { "" };
#endif

namespace Sampler {
    static const uint32_t LINK_START = 0x02000000, TEXT_SIZE = 0x200000;
    static const uint32_t OS_START = 0x01000000, OS_SIZE = 0x100000;
    static uint32_t textStart;
    static uint32_t text[TEXT_SIZE >> 4]; // The app's code, in 16-byte buckets
    static uint32_t os[OS_SIZE >> 8]; // System library code, in 256-byte buckets
    static uint32_t stubs[0x1000 >> 4]; // The JIT's shared routines at the start of the codegen area
    static uint32_t blocked[TEXT_SIZE >> 4]; // Where the sampled thread called into the system when it was blocked
    static uint32_t jitBlocks, other, total, idle;
    static OSThread *sampled;
    static uint32_t codegenStart, codegenSize;
    static OSAlarm alarm;

    // A thread on the lowest priority that only runs when the core would otherwise be idle, so idle time shows up
    // as samples in it instead of in the system's scheduler
    static volatile bool idling;
    static OSThread *idleThread;

    static int idleSpin(int argc, const char **argv) {
        while (idling);
        return 0;
    }

    static void callback(OSAlarm *alarm, OSContext *context) {
        // Count where the emulator's core was interrupted
        uint32_t pc = context->srr0;
        total++;
        if ((pc - (uint32_t)(uintptr_t)(&idleSpin)) < 0x20) {
            // When the core is idle, the sampled thread is blocked, so follow its stack back to the app's code
            // to find what it was waiting for
            // Runtime library frames like the condition variable itself are skipped, to find the emulator code that waited
            idle++;
            uint32_t sp = sampled->context.gpr[1];
            for (int i = 0; i < 16 && sp - 0x10000000 < 0x40000000; i++) {
                uint32_t frame = *(uint32_t*)(uintptr_t)sp;
                if (frame - 0x10000000 >= 0x40000000) break;
                uint32_t lr = *(uint32_t*)(uintptr_t)(frame + 4);
                sp = frame;
                if (lr - textStart >= TEXT_SIZE) continue;
                uint32_t index = std::upper_bound(symAddrs, symAddrs + symCount, LINK_START + (lr - textStart)) - symAddrs;
                const char *name = (index > 0) ? symNames[index - 1] : "";
                if (!strncmp(name, "std::", 5) || !strncmp(name, "pthread_", 8) || !strncmp(name, "__", 2) ||
                    !strncmp(name, "void std::", 10))
                    continue;
                blocked[(lr - textStart) >> 4]++;
                break;
            }
        }
        else if (pc - textStart < TEXT_SIZE) text[(pc - textStart) >> 4]++;
        else if (pc - OS_START < OS_SIZE) os[(pc - OS_START) >> 8]++;
        else if (pc - codegenStart < 0x1000) stubs[(pc - codegenStart) >> 4]++;
        else if (pc - codegenStart < codegenSize) jitBlocks++;
        else other++;
    }

    void start() {
        // Find where the code was loaded, using the callback's link-time address if known
        uint32_t callbackAddr = (uint32_t)(uintptr_t)&callback;
        textStart = symCallback ? (callbackAddr - (symCallback - LINK_START)) : ((callbackAddr - TEXT_SIZE / 2) & ~0xFFFFF);

        // Start sampling the current core 10,000 times a second, clearing the last results
        void *area = nullptr;
        OSGetCodegenVirtAddrRange(&area, &codegenSize);
        codegenStart = (uint32_t)(uintptr_t)area;
        memset(text, 0, sizeof(text));
        memset(os, 0, sizeof(os));
        memset(stubs, 0, sizeof(stubs));
        memset(blocked, 0, sizeof(blocked));
        jitBlocks = other = total = idle = 0;
        sampled = OSGetCurrentThread();
        OSCreateAlarm(&alarm);
        OSSetPeriodicAlarm(&alarm, OSMicrosecondsToTicks(100), OSMicrosecondsToTicks(100), callback);
        idling = true;
        idleThread = WorkerThread::createThread(idleSpin, nullptr, 0x4000, 31, OS_THREAD_ATTRIB_AFFINITY_CPU1);
    }

    void stop() {
        OSCancelAlarm(&alarm);
        idling = false;
        if (idleThread) OSJoinThread(idleThread, nullptr);
        idleThread = nullptr;
    }

    static std::vector<std::pair<uint32_t, uint32_t>> topCounts(const uint32_t *counts, uint32_t size, uint32_t scale) {
        // Get the nonzero counts with their offsets, highest first
        std::vector<std::pair<uint32_t, uint32_t>> top;
        for (uint32_t i = 0; i < size; i++)
            if (counts[i]) top.push_back({ counts[i], i * scale });
        std::sort(top.rbegin(), top.rend());
        return top;
    }

    std::string report() {
        // Summarize the samples by where they were
        double percent = std::max(total, 1U) / 100.0;
        uint32_t textTotal = 0, osTotal = 0, stubTotal = 0;
        for (uint32_t count : text) textTotal += count;
        for (uint32_t count : os) osTotal += count;
        for (uint32_t count : stubs) stubTotal += count;
        char line[256];
        snprintf(line, sizeof(line), "%u samples: app %.1f%%, idle %.1f%%, system %.1f%%, JIT blocks %.1f%%, JIT stubs %.1f%%\n",
            total, textTotal / percent, idle / percent, osTotal / percent, jitBlocks / percent, stubTotal / percent);
        std::string result = line;

        // Show the busiest functions, or code addresses if there are no symbols
        std::vector<std::pair<uint32_t, uint32_t>> top;
        if (symCount) {
            std::vector<uint32_t> perSymbol(symCount);
            for (uint32_t i = 0; i < (TEXT_SIZE >> 4); i++) {
                if (!text[i]) continue;
                uint32_t addr = LINK_START + (i << 4);
                uint32_t index = std::upper_bound(symAddrs, symAddrs + symCount, addr) - symAddrs;
                if (index > 0) perSymbol[index - 1] += text[i];
            }
            top = topCounts(perSymbol.data(), symCount, 1);
        }
        else {
            top = topCounts(text, TEXT_SIZE >> 4, 16);
        }
        for (size_t i = 0; i < 16 && i < top.size(); i++) {
            if (symCount)
                snprintf(line, sizeof(line), "%5.1f%% %-34.34s", top[i].first / percent, symNames[top[i].second]);
            else
                snprintf(line, sizeof(line), "%5.1f%% %08X%-26s", top[i].first / percent, textStart + top[i].second, "");
            result += line;
            result += (i & 1) ? "\n" : "  ";
        }

        // Show which app functions the thread was in when it blocked
        if (symCount) {
            std::vector<uint32_t> perSymbol(symCount);
            for (uint32_t i = 0; i < (TEXT_SIZE >> 4); i++) {
                if (!blocked[i]) continue;
                uint32_t index = std::upper_bound(symAddrs, symAddrs + symCount, LINK_START + (i << 4)) - symAddrs;
                if (index > 0) perSymbol[index - 1] += blocked[i];
            }
            std::vector<std::pair<uint32_t, uint32_t>> blockedTop = topCounts(perSymbol.data(), symCount, 1);
            result += "Blocked in:";
            for (size_t i = 0; i < 4 && i < blockedTop.size(); i++) {
                snprintf(line, sizeof(line), "%s %.1f%% %.30s", (i == 2) ? "\n" : "", blockedTop[i].first / percent,
                    symNames[blockedTop[i].second]);
                result += line;
            }
            result += "\n";
        }

        // Also show exactly where it blocked, as link-time addresses to look up in the ELF
        std::vector<std::pair<uint32_t, uint32_t>> blockedAddrs = topCounts(blocked, TEXT_SIZE >> 4, 16);
        result += "Blocked at:";
        for (size_t i = 0; i < 4 && i < blockedAddrs.size(); i++) {
            snprintf(line, sizeof(line), " %08X %.1f%%", LINK_START + blockedAddrs[i].second, blockedAddrs[i].first / percent);
            result += line;
        }
        result += "\n";

        // Find the system library's exported functions, to name the busiest system code by the nearest one before it
        std::vector<std::pair<uint32_t, const char*>> known;
        OSDynLoad_Module coreinit;
        if (OSDynLoad_Acquire("coreinit.rpl", &coreinit) == OS_DYNLOAD_OK) {
            for (uint32_t i = 0; i < osNameCount; i++) {
                void *addr = nullptr;
                if (OSDynLoad_FindExport(coreinit, OS_DYNLOAD_EXPORT_FUNC, osNames[i], &addr) == OS_DYNLOAD_OK && addr)
                    known.push_back({ (uint32_t)(uintptr_t)addr, osNames[i] });
            }
            OSDynLoad_Release(coreinit);
        }
        std::sort(known.begin(), known.end());
        std::vector<std::pair<uint32_t, uint32_t>> osTop = topCounts(os, OS_SIZE >> 8, 256);
        for (size_t i = 0; i < 1 && i < osTop.size(); i++) {
            uint32_t addr = OS_START + osTop[i].second;
            size_t index = std::upper_bound(known.begin(), known.end(), std::make_pair(addr, (const char*)nullptr)) - known.begin();
            if (index > 0)
                snprintf(line, sizeof(line), "System %08X %.1f%%: after %s+%X\n", addr, osTop[i].first / percent,
                    known[index - 1].second, addr - known[index - 1].first);
            else
                snprintf(line, sizeof(line), "System %08X %.1f%%\n", addr, osTop[i].first / percent);
            result += line;
        }
        return result;
    }
}

#endif // __WIIU__ && PROFILE_SAMPLER
