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

// Progress counters for builds made with HEARTBEAT, which a frontend logs regularly to find where things stop

#pragma once

#include <chrono>
#include <cstdint>

enum HeartbeatSlot {
    HB_EMU_FRAMES, // Frames finished by the emulator
    HB_EMU_TASK, // Scheduled task running, or 0xFF between tasks
    HB_EMU_WAIT, // What the emulator is waiting on: 1 = 2D start, 2 = 2D finish, 3 = 3D line, 4 = audio
    HB_JIT_BLOCK, // Address of the last block entered
    HB_JIT_INSIDE, // Whether generated code is running
    HB_JIT_ENTRIES, // Times generated code was entered
    HB_2D_LINES, // Scanlines drawn by the 2D thread
    HB_3D_LINES, // Scanlines finished by 3D threads
    HB_UI_FRAMES, // Frames drawn by the UI
    HB_UI_STAGE, // Step of the UI loop: 1 = get frame, 2 = start frame, 3 = draw, 4 = end frame, 5 = input
    HB_AUDIO, // Audio callbacks
    HB_GPU_STAGE, // Step of the GPU 3D renderer: 1 = waiting, 2 = building, 3 = locking GX2, 4 = drawing, 5 = converting
    HB_GPU_FRAMES, // Frames finished by the GPU 3D renderer
    HB_GPU_DISP3DCNT, // 3D display control of the frame the GPU 3D renderer is drawing
    HB_GPU_BUILD_US, // Total microseconds the GPU 3D renderer spent building vertices
    HB_GPU_LOCK_US, // Total microseconds the GPU 3D renderer waited for GX2
    HB_GPU_DRAW_US, // Total microseconds the GPU 3D renderer spent drawing, until the GPU was done
    HB_GPU_CONVERT_US, // Total microseconds the GPU 3D renderer spent converting scanlines
    HB_GPU_TEXTURE_US, // Total microseconds the GPU 3D renderer spent checking and decoding textures, within building
    HB_3D_WAIT_US, // Total microseconds the 2D engine waited for 3D scanlines
    HB_EMU_3D_WAIT_US, // Total microseconds the emulator waited for the GPU 3D renderer to finish a frame
    HB_2D_JOIN_US, // Total microseconds the emulator waited for the 2D thread at the end of frames
    HB_2D_EMU_LINES, // Scanlines the emulator drew because the 2D thread hadn't started them
    HB_2D_CORE, // Core the 2D thread last ran on, plus 1
    HB_GEOM_CORE, // Core the geometry worker last ran on, plus 1
    HB_COUNT
};

#ifdef HEARTBEAT
namespace Heartbeat {
    extern volatile uint32_t values[HB_COUNT];
}
#define HB_SET(slot, value) (Heartbeat::values[slot] = (value))
#define HB_INC(slot) (Heartbeat::values[slot]++)
#ifdef __WIIU__
#include <coreinit/core.h>
#define HB_SET_CORE(slot) HB_SET(slot, OSGetCoreId() + 1)
#else
#define HB_SET_CORE(slot) do {} while (0)
#endif
#define HB_TIMER(name) auto name = std::chrono::steady_clock::now()
#define HB_ADD_US(slot, name) (Heartbeat::values[slot] += \
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - name).count())
#else
#define HB_SET(slot, value) do {} while (0)
#define HB_INC(slot) do {} while (0)
#define HB_SET_CORE(slot) do {} while (0)
#define HB_TIMER(name) do {} while (0)
#define HB_ADD_US(slot, name) do {} while (0)
#endif
