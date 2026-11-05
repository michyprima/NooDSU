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

#ifdef FRAME_LOG

#include <algorithm>
#include <cstdio>
#include <string>
#include <coreinit/time.h>

#include "frame_log.h"
#include "../../core/file_lock.h"

namespace FrameLog {
    // An entry has a type, and for the emulator, when the frame finished, when its display wait ended, and its polygons;
    // for the UI, the time, and the ID of the new frame it got or 0; for audio, the time, the samples repeated so far
    // because the buffer ran dry, and how many samples the buffer had
    enum { TYPE_START = 1, TYPE_EMULATOR = 2, TYPE_UI = 3, TYPE_AUDIO = 4, TYPE_PRODUCED = 5, TYPE_OUTPUT = 6,
        TYPE_MARK = 7, TYPE_WRITE = 8, TYPE_LOST = 9 };
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

#ifdef AUDIO_CAPTURE
    static Ring writer;

    // Each stream has one thread adding samples, and the writing thread reading them, with room for about 32 seconds
    struct SampleRing {
        static const uint32_t SIZE = 1 << 20;
        uint32_t samples[SIZE];
        volatile uint32_t head = 0;
        uint32_t tail = 0;

        void drain(FILE *file, uint32_t stream) {
            // Note any samples lost to the ring wrapping, then write what's there in up to two pieces
            uint32_t end = head;
            __sync_synchronize();
            if (end - tail > SIZE) {
                writer.add({ TYPE_LOST, OSGetSystemTick(), stream, end - SIZE - tail });
                tail = end - SIZE;
            }
            while (tail != end) {
                uint32_t start = tail % SIZE, count = std::min(end - tail, SIZE - start);
                fwrite(&samples[start], sizeof(uint32_t), count, file);
                tail += count;
            }
        }
    };
    static SampleRing produced, output;
    static volatile uint32_t lastSlow = 0;
    static uint32_t lastWait = 0;

    void audioProduced(uint32_t sample) {
        produced.samples[produced.head % SampleRing::SIZE] = sample;
        __sync_synchronize();
        produced.head = produced.head + 1;
    }

    void audioOutput(const uint32_t *samples, int count) {
        for (int i = 0; i < count; i++)
            output.samples[(output.head + i) % SampleRing::SIZE] = samples[i];
        __sync_synchronize();
        output.head = output.head + count;
        audio.add({ TYPE_OUTPUT, OSGetSystemTick(), output.head, 0 });
    }

    void mark() {
        ui.add({ TYPE_MARK, OSGetSystemTick(), 0, 0 });
    }

    static void appendSamples(SampleRing &ring, const std::string &path, uint32_t stream, bool first) {
        std::lock_guard<FileLock> fileGuard(fileLock());
        FILE *file = fopen(path.c_str(), first ? "wb" : "ab");
        if (!file) return;
        ring.drain(file, stream);
        fclose(file);
    }

    bool shouldDeferWrite() {
        // Stay off the SD card for a second after a slow frame so writing doesn't slow scene changes down, unless the
        // samples are piling up
        uint32_t pending = std::max(produced.head - produced.tail, output.head - output.tail);
        return OSGetSystemTick() - lastSlow < OSMillisecondsToTicks(1000) && pending < SampleRing::SIZE / 2;
    }
#endif

    void emulatorStarted() {
        emulator.add({ TYPE_START, OSGetSystemTick(), 0, 0 });
    }

    void emulatorFrame(uint32_t finished, uint32_t polygons) {
        emulator.add({ TYPE_EMULATOR, finished, OSGetSystemTick(), polygons });
#ifdef AUDIO_CAPTURE
        emulator.add({ TYPE_PRODUCED, finished, produced.head, 0 });
        if (lastWait && finished - lastWait > OSMillisecondsToTicks(20)) lastSlow = finished;
        lastWait = OSGetSystemTick();
#endif
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
        std::lock_guard<FileLock> fileGuard(fileLock());
        FILE *file = fopen(path, started ? "ab" : "wb");
        if (!file) return;
        if (!started) {
            Entry header = { 0x4E464C47, uint32_t(OSTimerClockSpeed), 0, 0 };
            fwrite(&header, sizeof(Entry), 1, file);
            started = true;
        }
#ifdef AUDIO_CAPTURE
        // Write the samples next to the log, keeping when it happened to check it against any dropouts
        std::string base = path;
        base.resize(base.rfind('/') + 1);
        static bool samplesStarted = false;
        uint32_t writeStart = OSGetSystemTick();
        appendSamples(produced, base + "audio-produced.pcm", 0, !samplesStarted);
        appendSamples(output, base + "audio-output.pcm", 1, !samplesStarted);
        samplesStarted = true;
        writer.add({ TYPE_WRITE, writeStart, OSGetSystemTick(), 0 });
        writer.drain(file);
#endif
        emulator.drain(file);
        ui.drain(file);
        audio.drain(file);
        fclose(file);
    }
}

#endif // FRAME_LOG
