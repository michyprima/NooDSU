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
    HB_COUNT
};

#ifdef HEARTBEAT
namespace Heartbeat {
    extern volatile uint32_t values[HB_COUNT];
}
#define HB_SET(slot, value) (Heartbeat::values[slot] = (value))
#define HB_INC(slot) (Heartbeat::values[slot]++)
#else
#define HB_SET(slot, value) do {} while (0)
#define HB_INC(slot) do {} while (0)
#endif
