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
    EP_GEOM_SYNC, // Waiting for geometry commands on the worker thread
    EP_3D_HELP, // Drawing 3D scanlines for threads that fell behind, on the emulator thread
    EP_3D_WAIT, // Waiting for 3D scanlines, on the emulator thread
    EP_CAPTURE, // Display capture
    EP_2D_CLEAR, // Clearing layers before drawing a 2D scanline, on the emulator thread
    EP_2D_OBJ, // Drawing objects, on the emulator thread
    EP_2D_BG, // Drawing backgrounds, on the emulator thread
    EP_2D_BLEND, // Blending layers, on the emulator thread
    EP_2D_OUT, // Copying the final scanline and applying master brightness, on the emulator thread
    EP_SWAP3D, // Swapping 3D buffers at V-blank
    EP_FRAMECOPY, // Copying finished frames for the frontend
    EP_HIRES, // Within copying frames: getting the high-res 3D frame
    EP_JOIN2D_A, // Waiting for engine A's 2D thread to finish the frame at V-blank
    EP_JOIN2D_B, // Waiting for engine B's 2D thread to finish the frame at V-blank
    EP_DISPLAY, // Waiting for the display after a frame, when syncing to it
    EP_COUNT
};

enum RenderProfileSlot {
    RP_DRAW, // 3D threads clearing scanlines and drawing polygons
    RP_FINISH, // 3D threads finishing scanlines with edge marking, fog and anti-aliasing
    RP_WAIT, // 3D threads waiting for neighboring scanlines
    RP_COUNT
};

enum PixelProfileSlot {
    PX_CLEAR, // Clearing a 3D scanline
    PX_SPAN, // Setting up a polygon's span on a scanline
    PX_DEPTH, // Per pixel: interpolation factor, depth and depth test
    PX_INTERP, // Per pixel: color and texture coordinate interpolation
    PX_TEXEL, // Per pixel: reading a texel
    PX_WRITE, // Per pixel: blending and writing
    PX_COUNT
};

enum EmuCountSlot {
    EC_LINES_EMU, // Engine A scanlines drawn on the emulator thread
    EC_LINES_HALF, // Engine B scanlines drawn on the emulator thread
    EC_LINES_THREAD, // Engine B scanlines drawn by its thread
    EC_JIT_COMPILED, // JIT blocks compiled
    EC_JIT_RETIRED, // JIT blocks dropped because their code was overwritten
    EC_JIT_RESETS, // Times the JIT dropped all its code
    EC_JIT_WRITES, // Writes to pages with compiled code, which take a slow path
    EC_INTERP9, // ARM9 instructions run by the interpreter in the run loops, without compiled code
    EC_COUNT
};

#if defined(PROFILE_EMU) && defined(HEARTBEAT)
// With both, the emulator records every frame's breakdown for the heartbeat log to list the slow ones
struct EmuFrameRecord {
    uint32_t frame; // Emulated frame number
    uint32_t wall; // Microseconds from the end of the last frame to the end of this one, including the display wait
    uint32_t ticks[EP_COUNT]; // Time in each slot during the frame, in microseconds
    uint32_t counts[EC_COUNT]; // Counts during the frame
    uint32_t gpuFrames, gpuBuild, gpuDraw, wait3D; // Heartbeat values during the frame: 3D frames and their times
};
namespace EmuProfile {
    static const int RECORDS = 256;
    extern EmuFrameRecord records[RECORDS];
    extern volatile uint32_t recordCount; // Records made so far; the latest is at (count - 1) % RECORDS
    void recordFrame(uint32_t frame);

    // A log of compiled JIT blocks and resets, so the frame tracer can match samples in generated code to guest code
    // Each entry is a word with the type, CPU and opcode count, then the record count, guest address (bit 0 set for
    // THUMB), host code start and end, and the opcodes; the heartbeat logger writes it out and drops what it missed
    enum { TRACE_BLOCK = 1, TRACE_RESET = 2 };
    static const uint32_t TRACE_WORDS = 1 << 20;
    extern uint32_t traceWords[TRACE_WORDS];
    extern volatile uint32_t traceHead; // Words written so far; the latest is at (head - 1) % TRACE_WORDS
    void traceBlock(int type, bool arm7, uint32_t key, const void *start, const void *end, const uint32_t *opcodes,
        uint32_t count);
}
#endif

#ifdef PROFILE_EMU
#include <coreinit/core.h>
#include <coreinit/time.h>

namespace EmuProfile {
    extern uint64_t ticks[EP_COUNT];
    extern uint64_t taskTicks[64];
    extern uint32_t counts[EC_COUNT];
    extern uint64_t renderTicks[4][RP_COUNT]; // Kept separately for each 3D thread
    extern uint64_t pixelTicks[3][PX_COUNT]; // Kept separately for each CPU core

    // Read the time base directly, which is cheaper than calling the OS for timing inside pixel loops
    inline uint32_t tick() {
        uint32_t t;
        __asm__ volatile("mftb %0" : "=r"(t));
        return t;
    }

    // Times the phases of drawing a pixel, adding the rest to the current phase when the pixel is skipped
    struct PixelTimer {
        uint64_t *slots;
        int phase;
        uint32_t last;
        PixelTimer(uint64_t *slots, int phase): slots(slots), phase(phase), last(tick()) {}
        void next() { uint32_t t = tick(); slots[phase++] += t - last; last = t; }
        ~PixelTimer() { slots[phase] += tick() - last; }
    };
    inline uint32_t now() { return OSGetSystemTick(); }
}

// Time or count something only when it happens on the emulator thread, the only one on core 1
#define EP_START_EMU(name) uint32_t name = (OSGetCoreId() == 1) ? EmuProfile::now() : 0
#define EP_ADD_EMU(slot, start) do { if (OSGetCoreId() == 1) EP_ADD(slot, start); } while (0)
#define EP_INC(slot) (EmuProfile::counts[slot]++)
#define RP_ADD(thread, slot, start) (EmuProfile::renderTicks[(thread) & 3][slot] += EmuProfile::now() - (start))
#define PX_SLOTS uint64_t *pxSlots = EmuProfile::pixelTicks[OSGetCoreId() % 3]
#define PX_TIMER(phase) EmuProfile::PixelTimer pxTimer(pxSlots, phase)
#define PX_NEXT() pxTimer.next()
#define PX_START(name) uint32_t name = EmuProfile::tick()
#define PX_ADD(slot, start) (pxSlots[slot] += EmuProfile::tick() - (start))

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
#define RP_ADD(thread, slot, start) do {} while (0)
#define PX_SLOTS do {} while (0)
#define PX_TIMER(phase) do {} while (0)
#define PX_NEXT() do {} while (0)
#define PX_START(name) do {} while (0)
#define PX_ADD(slot, start) do {} while (0)
#ifdef HEARTBEAT
#define EP_TASK(core) do { HB_SET(HB_EMU_TASK, (core).events[0].task); (core).tasks[(core).events[0].task](); \
    HB_SET(HB_EMU_TASK, 0xFF); } while (0)
#else
#define EP_TASK(core) (core).tasks[(core).events[0].task]()
#endif
#endif
