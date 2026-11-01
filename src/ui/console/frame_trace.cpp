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

#if defined(__WIIU__) && defined(PROFILE_EMU) && defined(HEARTBEAT)

#include <vector>
#include <coreinit/alarm.h>
#include <coreinit/codegen.h>
#include <coreinit/context.h>
#include <coreinit/time.h>

#include "frame_trace.h"
#include "../../core/emu_profile.h"

namespace FrameTrace {
    // Where the emulator's core was when sampled: the record count at the time, which is the index of the frame record
    // the sample ends up in, the interrupted address and link register, saved return addresses from the stack, and
    // the last JIT block entered
    struct Sample {
        uint32_t record, pc, lr, returns[4], block;
    };

    static const uint32_t SAMPLES = 1 << 16;
    static Sample samples[SAMPLES];
    static volatile uint32_t sampleHead = 0;
    static OSAlarm alarm;

    static void callback(OSAlarm *alarm, OSContext *context) {
        // Record a sample, following the stack's back chain while it stays in the range stacks are in
        Sample &sample = samples[sampleHead % SAMPLES];
        sample.record = EmuProfile::recordCount;
        sample.pc = context->srr0;
        sample.lr = context->lr;
        uint32_t sp = context->gpr[1];
        for (int i = 0; i < 4; i++) {
            uint32_t frame = (sp - 0x10000000 < 0x40000000) ? *(uint32_t*)(uintptr_t)sp : 0;
            bool valid = (frame - 0x10000000 < 0x40000000);
            sample.returns[i] = valid ? *(uint32_t*)(uintptr_t)(frame + 4) : 0;
            sp = valid ? frame : 0;
        }
        sample.block = Heartbeat::values[HB_JIT_BLOCK];
        __sync_synchronize();
        sampleHead = sampleHead + 1;
    }

    void start() {
        // Sample the current core 5,000 times a second
        OSCreateAlarm(&alarm);
        OSSetPeriodicAlarm(&alarm, OSMicrosecondsToTicks(200), OSMicrosecondsToTicks(200), callback);
    }

    void stop() {
        OSCancelAlarm(&alarm);
    }

    static void chunk(FILE *file, const char *tag, const void *data, uint32_t words) {
        // Write a tagged chunk with its size in words
        fwrite(tag, 1, 4, file);
        fwrite(&words, 4, 1, file);
        fwrite(data, 4, words, file);
    }

    void write(const char *path) {
        // Start a new file with what's needed to read it and match addresses offline: this function's address, to
        // find where the code was loaded, the codegen area, the timer speed, and the sizes of frame records
        static bool started = false;
        static uint32_t sampleTail = 0, traceTail = 0, lastKept = -1;
        FILE *file = fopen(path, started ? "ab" : "wb");
        if (!file) return;
        if (!started) {
            void *area = nullptr;
            uint32_t size = 0;
            OSGetCodegenVirtAddrRange(&area, &size);
            uint32_t header[] = { 0x4E545243, 1, uint32_t(uintptr_t(&write)), uint32_t(uintptr_t(area)), size,
                uint32_t(OSTimerClockSpeed), EP_COUNT, EC_COUNT, sizeof(EmuFrameRecord) / 4, sizeof(Sample) / 4 };
            fwrite(header, 4, sizeof(header) / 4, file);
            started = true;
        }

        // Write the JIT log since last time, or mark it as lost if it was overwritten before it could be copied
        uint32_t head = EmuProfile::traceHead;
        __sync_synchronize();
        if (head - traceTail <= EmuProfile::TRACE_WORDS) {
            std::vector<uint32_t> words;
            for (uint32_t i = traceTail; i != head; i++)
                words.push_back(EmuProfile::traceWords[i % EmuProfile::TRACE_WORDS]);
            __sync_synchronize();
            if (EmuProfile::traceHead - traceTail <= EmuProfile::TRACE_WORDS)
                chunk(file, "BLKS", words.data(), words.size());
            else
                chunk(file, "LOST", nullptr, 0);
        }
        else {
            chunk(file, "LOST", nullptr, 0);
        }
        traceTail = head;

        // Write the samples of finished frames that were slow, and of every 60th frame to compare with, each kept
        // frame's record first
        head = sampleHead;
        __sync_synchronize();
        if (head - sampleTail > SAMPLES) sampleTail = head - SAMPLES;
        uint32_t records = EmuProfile::recordCount;
        std::vector<uint32_t> kept;
        for (; sampleTail != head; sampleTail++) {
            const Sample &sample = samples[sampleTail % SAMPLES];
            if (sample.record >= records) break;
            if (records - sample.record > EmuProfile::RECORDS - 8) continue;
            const EmuFrameRecord &record = EmuProfile::records[sample.record % EmuProfile::RECORDS];
            bool slow = (record.wall - record.ticks[EP_DISPLAY] > 16000 || record.wall > 17500);
            if (!slow && sample.record % 60) continue;
            if (sample.record != lastKept) {
                if (!kept.empty()) chunk(file, "SMPS", kept.data(), kept.size());
                kept.clear();
                uint32_t index = sample.record;
                fwrite("FRAM", 1, 4, file);
                uint32_t words = 1 + sizeof(record) / 4;
                fwrite(&words, 4, 1, file);
                fwrite(&index, 4, 1, file);
                fwrite(&record, 4, sizeof(record) / 4, file);
                lastKept = sample.record;
            }
            const uint32_t *data = (const uint32_t*)&sample;
            kept.insert(kept.end(), data, data + sizeof(Sample) / 4);
        }
        if (!kept.empty()) chunk(file, "SMPS", kept.data(), kept.size());
        fclose(file);
    }
}

#endif // __WIIU__ && PROFILE_EMU && HEARTBEAT
