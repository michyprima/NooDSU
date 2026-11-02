/*
    Copyright 2019-2026 Hydr8gon
    Copyright 2026 michyprima

    This file is part of NooDSU.

    NooDSU is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDSU is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDSU. If not, see <https://www.gnu.org/licenses/>.
*/

#ifdef FRAME_LOG

#include <cstdio>
#include <coreinit/time.h>

#include "frame_log.h"

namespace FrameLog {
    // An entry has a type, and for the emulator, when the frame finished, when its display wait ended, and its polygons;
    // for the UI, the time, and the ID of the new frame it got or 0; for audio, the time, the samples repeated so far
    // because the buffer ran dry, and how many samples the buffer had
    enum { TYPE_START = 1, TYPE_EMULATOR = 2, TYPE_UI = 3, TYPE_AUDIO = 4 };
    struct Entry {
        uint32_t type, time, value, polygons;
    };

    // Each ring has one thread adding entries, and the writing thread reading them
    struct Ring {
        static const uint32_t SIZE = 1 << 14;
        Entry entries[SIZE];
        volatile uint32_t head = 0;
        uint32_t tail = 0;

        void add(const Entry &entry) {
            entries[head % SIZE] = entry;
            __sync_synchronize();
            head = head + 1;
        }

        void drain(FILE *file) {
            uint32_t end = head;
            __sync_synchronize();
            if (end - tail > SIZE) tail = end - SIZE;
            for (; tail != end; tail++)
                fwrite(&entries[tail % SIZE], sizeof(Entry), 1, file);
        }
    };
    static Ring emulator, ui, audio;

    void emulatorStarted() {
        emulator.add({ TYPE_START, OSGetSystemTick(), 0, 0 });
    }

    void emulatorFrame(uint32_t finished, uint32_t polygons) {
        emulator.add({ TYPE_EMULATOR, finished, OSGetSystemTick(), polygons });
    }

    void uiFrame(uint32_t frameId) {
        ui.add({ TYPE_UI, OSGetSystemTick(), frameId, 0 });
    }

    void audioBuffer(uint32_t dry, uint32_t fill) {
        audio.add({ TYPE_AUDIO, OSGetSystemTick(), dry, fill });
    }

    void write(const char *path) {
        // Start a new file the first time, with the time base's speed
        static bool started = false;
        FILE *file = fopen(path, started ? "ab" : "wb");
        if (!file) return;
        if (!started) {
            Entry header = { 0x4E464C47, uint32_t(OSTimerClockSpeed), 0, 0 };
            fwrite(&header, sizeof(Entry), 1, file);
            started = true;
        }
        emulator.drain(file);
        ui.drain(file);
        audio.drain(file);
        fclose(file);
    }
}

#endif // FRAME_LOG
