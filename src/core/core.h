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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "defines.h"
#include "atomic.h"
#include "save_states.h"
#include "settings.h"
#include "arm/cp15.h"
#include "arm/interpreter.h"
#include "arm/jit.h"
#include "arm/timers.h"
#include "gpu/gpu.h"
#include "gpu/gpu_2d.h"
#include "gpu/gpu_3d.h"
#include "gpu/gpu_3d_renderer.h"
#include "hle/action_replay.h"
#include "hle/dldi.h"
#include "hle/hle_arm7.h"
#include "hle/hle_bios.h"
#include "io/aes.h"
#include "io/div_sqrt.h"
#include "io/dsi_wifi.h"
#include "io/i2c.h"
#include "io/input.h"
#include "io/ipc.h"
#include "io/rtc.h"
#include "io/spi.h"
#include "io/spu.h"
#include "io/wifi.h"
#include "memory/cartridge.h"
#include "memory/dma.h"
#include "memory/memory.h"
#include "memory/ndma.h"
#include "memory/sd_mmc.h"

enum CoreError {
    ERROR_NDS_BIOS = 1,
    ERROR_NDS_FIRM,
    ERROR_DSI_BIOS,
    ERROR_DSI_FIRM,
    ERROR_DSI_NAND,
    ERROR_ROM,
    ERROR_CANCELLED // Loading was cancelled by the user
};

enum SchedTask {
    UPDATE_RUN,
    RESET_CYCLES,
    CART9_WORD_READY,
    CART7_WORD_READY,
    DMA9_TRANSFER0,
    DMA9_TRANSFER1,
    DMA9_TRANSFER2,
    DMA9_TRANSFER3,
    DMA7_TRANSFER0,
    DMA7_TRANSFER1,
    DMA7_TRANSFER2,
    DMA7_TRANSFER3,
    NDS_SCANLINE256,
    NDS_SCANLINE355,
    GBA_SCANLINE240,
    GBA_SCANLINE308,
    GPU3D_COMMANDS,
    ARM9_INTERRUPT,
    ARM7_INTERRUPT,
    NDS_SPU_SAMPLE,
    GBA_SPU_SAMPLE,
    TIMER9_OVERFLOW0,
    TIMER9_OVERFLOW1,
    TIMER9_OVERFLOW2,
    TIMER9_OVERFLOW3,
    TIMER7_OVERFLOW0,
    TIMER7_OVERFLOW1,
    TIMER7_OVERFLOW2,
    TIMER7_OVERFLOW3,
    WIFI_COUNT_MS,
    WIFI_TRANS_REPLY,
    WIFI_TRANS_ACK,
    AES_UPDATE,
    NDMA9_UPDATE,
    NDMA7_UPDATE,
    SDMMC_READ_BLOCK,
    SDMMC_WRITE_BLOCK,
    SDIO_READ_BLOCK,
    SDIO_WRITE_BLOCK,
    MAX_TASKS
};

struct SchedEvent {
    SchedTask task;
    uint32_t cycles;

    SchedEvent(SchedTask task, uint32_t cycles): task(task), cycles(cycles) {}
    bool operator<(const SchedEvent &event) const { return cycles < event.cycles; }
};

// The scheduler's events, sorted by when they're due, with events due at the same time kept in the order they were added
// Emulation takes events from the front thousands of times per frame, so that only moves a pointer, and new events
// shift whichever side of the queue is shorter
class EventQueue {
public:
    SchedEvent *head; // The first event, which generated code reads through the queue's address, so it comes first

    EventQueue(): storage(64, SchedEvent(MAX_TASKS, 0)) { clear(); }

    size_t size() const { return count; }
    SchedEvent &operator[](size_t i) { return head[i]; }
    void clear() { count = 0; head = &storage[storage.size() / 2]; }

    void popFront() {
        // Remove the first event, moving back to the middle when empty
        if (--count == 0) head = &storage[storage.size() / 2];
        else head++;
    }

    void insert(const SchedEvent &event) {
        // Make room on both sides if needed, then shift the shorter side to fit the event in order
        if (head == storage.data() || head + count == storage.data() + storage.size())
            recenter();
        SchedEvent *pos = std::upper_bound(head, head + count, event);
        size_t before = pos - head;
        if (before < count - before) {
            memmove(head - 1, head, before * sizeof(SchedEvent));
            (--head)[before] = event;
        }
        else {
            memmove(pos + 1, pos, (count - before) * sizeof(SchedEvent));
            *pos = event;
        }
        count++;
    }

private:
    std::vector<SchedEvent> storage;
    size_t count = 0;

    void recenter() {
        // Move the events to the middle of storage, growing it if it's getting full
        std::vector<SchedEvent> bigger(std::max<size_t>(storage.size(), count * 4), SchedEvent(MAX_TASKS, 0));
        SchedEvent *start = &bigger[(bigger.size() - count) / 2];
        memcpy(start, head, count * sizeof(SchedEvent));
        storage.swap(bigger);
        head = start;
    }
};

class Core {
public:
    int id = 0;
    int fps = 0;
    bool arm7Hle = false;
    bool dsiMode = false;
    bool gbaMode = false;

    ActionReplay actionReplay;
    Aes aes;
    CartridgeGba cartridgeGba;
    CartridgeNds cartridgeNds;
    Cp15 cp15;
    DivSqrt divSqrt;
    Dldi dldi;
    Dma dma[2];
    Gpu gpu;
    Gpu2D gpu2D[2];
    Gpu3D gpu3D;
    Gpu3DRenderer gpu3DRenderer;
    HleArm7 hleArm7;
    HleBios hleBios[3];
    I2c i2c;
    Input input;
    Interpreter interpreter[2];
    Ipc ipc;
    Jit jit, jit7;
    Memory memory;
    Ndma ndma[2];
    Rtc rtc;
    SaveStates saveStates;
    SdMmc sdMmc;
    SdMmc sdio;
    DsiWifi dsiWifi;
    Spi spi;
    Spu spu;
    Timers timers[2];
    Wifi wifi;

    Atomic32<bool> running;
    std::atomic<bool> frameEnded; // Set at the end of each frame, for frontends that pace frames themselves
    EventQueue events;
    std::function<void()> tasks[MAX_TASKS];
    uint32_t globalCycles = 0;

    // With fast CPU timing, the ARM9 can run up to this many cycles ahead of the ARM7 before it catches up
    // Longer slices let both CPUs run more instructions at a time; tasks still bring the ARM7 up to date
    static const uint32_t FAST_SLICE = 1024;
    uint32_t sliceLimit = 0;

    // Compiled ARM7 code stops before reaching this, which is just before the ARM9 is due
    uint32_t arm7Limit = 0;

    Core(std::string ndsRom = "", std::string gbaRom = "", int id = 0, int ndsRomFd = -1, int gbaRomFd = -1,
        int ndsSaveFd = -1, int gbaSaveFd = -1, int ndsStateFd = -1, int gbaStateFd = -1, int ndsCheatFd = -1);
    void saveState(FILE *file);
    void loadState(FILE *file);

    void runCore() { (*runFunc)(*this); }
#ifdef STATE_DEBUG
    std::string debugSnapshot(const char *reason);
#endif

    // Pages with compiled code from either CPU are protected from writes the same way
    bool jitHasCode() { return jit.hasCode() || jit7.hasCode(); }
    bool jitIsCodePage(uint8_t *host) { return jit.isCodePage(host) || jit7.isCodePage(host); }
    bool jitWriteCode(uint8_t *host, int size) {
        // Both JITs drop overwritten code; returns whether either still has code in the page
        bool code9 = jit.writeCode(host, size);
        bool code7 = jit7.writeCode(host, size);
        return code9 || code7;
    }
    bool jitLineHasCode(uint8_t *host) { return jit.lineHasCode(host) || jit7.lineHasCode(host); }
    void schedule(SchedTask task, uint32_t cycles);
    void enterGbaMode();
    void endFrame();

private:
    friend class SaveStates; // Picks which CPUs run after loading their states
    bool realGbaBios;
    void (*runFunc)(Core&) = &Interpreter::runCoreNds<false>;
    std::chrono::steady_clock::time_point lastFpsTime;
    int fpsCount = 0;

    void updateRun();
    void resetCycles();
};
