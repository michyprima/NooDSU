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

// Wall-time profiling of the emulator thread on Wii U, for builds made with PROFILE_EMU
// The console frontend shows the results under the FPS counter

#pragma once

#include <cstdint>

#include "heartbeat.h"

enum EmuProfileSlot {
    EP_ARM9, // Only the ARM9 running
    EP_ARM7, // Only the ARM7 running
    EP_BOTH, // Both CPUs running, including the ARM7's part
    EP_BOTH_ARM7, // The ARM7's instructions while both are running
    EP_TASKS, // Scheduled tasks
    EP_WAIT2D, // Waiting for the 2D thread at each scanline
    EP_DRAW2D, // Drawing engine B's scanline on the emulator thread
    EP_COMPILE, // Compiling JIT blocks
    EP_JOIN3D, // Waiting for the previous frame's 3D threads and starting new ones
    EP_JOIN2D, // Waiting for the 2D thread at the end of a frame and starting it again
    EP_GEOM, // Running 3D geometry commands
    EP_SPU, // Generating audio samples
    EP_GEOM_VTX, // Vertex commands, including making and clipping polygons
    EP_GEOM_CLIP, // Clipping polygons
    EP_GEOM_MTX, // Matrix commands
    EP_GEOM_TEX, // Texture coordinate commands
    EP_GEOM_OTHER, // Other geometry commands
    EP_GXFIFO, // Sending words to the GXFIFO
    EP_3D_HELP, // Drawing 3D scanlines for threads that fell behind, on the emulator thread
    EP_3D_WAIT, // Waiting for 3D scanlines, on the emulator thread
    EP_CAPTURE, // Display capture
    EP_COUNT
};

enum EmuCountSlot {
    EC_LINES_EMU, // 2D scanlines drawn fully on the emulator thread
    EC_LINES_HALF, // 2D scanlines where the emulator thread drew engine B
    EC_LINES_THREAD, // 2D scanlines drawn fully by the 2D thread
    EC_COUNT
};

#ifdef PROFILE_EMU
#include <coreinit/core.h>
#include <coreinit/time.h>

namespace EmuProfile {
    extern uint64_t ticks[EP_COUNT];
    extern uint64_t taskTicks[64];
    extern uint32_t counts[EC_COUNT];
    inline uint32_t now() { return OSGetSystemTick(); }
}

// Time or count something only when it happens on the emulator thread, the only one on core 1
#define EP_START_EMU(name) uint32_t name = (OSGetCoreId() == 1) ? EmuProfile::now() : 0
#define EP_ADD_EMU(slot, start) do { if (OSGetCoreId() == 1) EP_ADD(slot, start); } while (0)
#define EP_INC(slot) (EmuProfile::counts[slot]++)

#define EP_START(name) uint32_t name = EmuProfile::now()
#define EP_ADD(slot, start) EmuProfile::ticks[slot] += EmuProfile::now() - (start)
#define EP_TASK(core) do { uint32_t t = EmuProfile::now(); int id = (core).events[0].task; (core).tasks[id](); \
    EmuProfile::taskTicks[id] += EmuProfile::now() - t; } while (0)
#else
#define EP_START(name) do {} while (0)
#define EP_ADD(slot, start) do {} while (0)
#define EP_START_EMU(name) do {} while (0)
#define EP_ADD_EMU(slot, start) do {} while (0)
#define EP_INC(slot) do {} while (0)
#ifdef HEARTBEAT
#define EP_TASK(core) do { HB_SET(HB_EMU_TASK, (core).events[0].task); (core).tasks[(core).events[0].task](); \
    HB_SET(HB_EMU_TASK, 0xFF); } while (0)
#else
#define EP_TASK(core) (core).tasks[(core).events[0].task]()
#endif
#endif
