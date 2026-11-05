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

#include <atomic>
#include <mutex>
#include <thread>

#include "atomic.h"

// File accesses made while a game runs take this lock, so only one happens at a time
// On Wii U, Aroma's file redirection can leave its IO thread spinning at top priority when several threads use files
// at once, which starves everything else on that core; it's recursive, so nested accesses can't deadlock
// It also counts the threads waiting for it, so the ROM loaders, which take it again right after every block, can let
// others go first; a mutex isn't fair, and a waiting thread would otherwise wait until the whole ROM was loaded
class FileLock {
public:
    void lock() {
        add(1);
        mutex.lock();
        add(-1);
        if (depth++ == 0) owner.store(std::this_thread::get_id());
    }

    void unlock() {
        if (--depth == 0) owner.store(std::thread::id());
        mutex.unlock();
    }

    // Whether another thread is waiting for the lock, which never counts a thread that already holds it
    bool contended() const {
        return waiting.load() > 0 && owner.load() != std::this_thread::get_id();
    }

private:
    std::recursive_mutex mutex;
    Atomic32<int> waiting{0};
    std::atomic<std::thread::id> owner{};
    int depth = 0; // Only changed by the thread holding the lock

    void add(int amount) {
        // The OS's compare-and-swap is used, since Espresso's read-modify-write instructions need its workaround
        int value = waiting.load();
        while (!waiting.compare_exchange_strong(value, value + amount));
    }
};

inline FileLock &fileLock() {
    static FileLock lock;
    return lock;
}
