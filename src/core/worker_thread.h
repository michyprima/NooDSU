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

#include <functional>
#include <thread>

#ifdef __WIIU__
#include <cstdlib>
#include <malloc.h>
#include <coreinit/thread.h>
#endif

// A helper thread for work like rendering, used in place of std::thread
// On Wii U it's kept off core 1, which is left to the emulator thread since only that core can run JIT code;
// threads can't change cores once running, so this has to be set when they're created
class WorkerThread {
public:
    template <typename... Args> WorkerThread(Args&&... args): func(std::bind(std::forward<Args>(args)...)) {
#ifdef __WIIU__
        // Create the thread like std::thread does on Wii U, but only allowed on cores 0 and 2
        thread = (OSThread*)memalign(16, sizeof(OSThread));
        stack = (uint8_t*)memalign(16, STACK_SIZE);
        if (thread && stack && OSCreateThread(thread, entry, 0, (char*)this, stack + STACK_SIZE,
                STACK_SIZE, 16, OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
            OSSetThreadRunQuantum(thread, 1000);
            OSResumeThread(thread);
            return;
        }
        free(thread);
        free(stack);
        thread = nullptr;
        stack = nullptr;
#endif
        fallback = new std::thread(func);
    }

    ~WorkerThread() {
#ifdef __WIIU__
        free(thread);
        free(stack);
#endif
        delete fallback;
    }

    void join() {
#ifdef __WIIU__
        if (thread) {
            OSJoinThread(thread, nullptr);
            return;
        }
#endif
        fallback->join();
    }

private:
    std::function<void()> func;
    std::thread *fallback = nullptr;

#ifdef __WIIU__
    static const int STACK_SIZE = 0x20000;
    OSThread *thread = nullptr;
    uint8_t *stack = nullptr;

    static int entry(int argc, const char **argv) {
        ((WorkerThread*)argv)->func();
        return 0;
    }
#endif
};
