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

#include <functional>
#include <thread>

#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <coreinit/thread.h>

// Waits in a loop for something another thread does, by yielding; it's one place to change how waits behave
class SpinWait {
public:
    void pause() { std::this_thread::yield(); }

    // Start a new wait, for loops that keep the same one
    void reset() {}
};

// A helper thread for work like rendering, used in place of std::thread
// On Wii U it's kept off core 1, which is left to the emulator thread since only that core can run JIT code;
// threads can't change cores once running, so this has to be set when they're created
class WorkerThread {
public:
    // Thread entry point; wut before 1.9.2 declares the argument as const char**, but it's passed the same way
    typedef int (*Entry)(int argc, void *argv);

    // Create and start a thread on Wii U with its memory in one block, freed by the OS once the thread has
    // fully exited, like std::thread does; freeing it right after joining isn't safe, since the thread can
    // still be exiting on its stack at that point
    static OSThread *createThread(Entry entry, void *arg, int stackSize, int priority,
            uint32_t affinity) {
        uint8_t *block = (uint8_t*)memalign(16, sizeof(OSThread) + stackSize);
        if (!block) return nullptr;
        memset(block, 0, sizeof(OSThread));
        OSThread *thread = (OSThread*)block;
        if (!OSCreateThread(thread, (OSThreadEntryPointFn)entry, 0, (char*)arg, block + sizeof(OSThread) + stackSize,
                stackSize, priority, (OSThreadAttributes)affinity)) {
            free(block);
            return nullptr;
        }
        OSSetThreadDeallocator(thread, [](OSThread *thread, void *stack) { free(thread); });
        OSSetThreadRunQuantum(thread, 1000);
        OSResumeThread(thread);
        return thread;
    }

    // A core to keep a thread on, for platforms that let threads be placed
    enum Core { ANY_CORE, CORE_0, CORE_2 };

    template <typename... Args> WorkerThread(Args&&... args): WorkerThread(ANY_CORE, std::forward<Args>(args)...) {}

    template <typename... Args> WorkerThread(Core core, Args&&... args): func(std::bind(std::forward<Args>(args)...)) {
        // Create the thread like std::thread does on Wii U, but only allowed on cores 0 and 2, or just one of them
        uint32_t affinity = ((core != CORE_2) ? OS_THREAD_ATTRIB_AFFINITY_CPU0 : 0) |
            ((core != CORE_0) ? OS_THREAD_ATTRIB_AFFINITY_CPU2 : 0);
        thread = createThread(entry, this, STACK_SIZE, 16, affinity);
        if (thread) return;
        fallback = new std::thread(func);
    }

    ~WorkerThread() {
        delete fallback;
    }

    void setPriority(int priority) {
        if (thread) OSSetThreadPriority(thread, priority);
    }

    void join() {
        if (thread) {
            OSJoinThread(thread, nullptr);
            thread = nullptr;
            return;
        }
        fallback->join();
    }

private:
    std::function<void()> func;
    std::thread *fallback = nullptr;

    static const int STACK_SIZE = 0x20000;
    OSThread *thread = nullptr;

    static int entry(int argc, void *argv) {
        ((WorkerThread*)argv)->func();
        return 0;
    }
};
