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

// Frame capture for debug builds with FRAME_CAPTURE: records consecutive frames at each step of the pipeline, at
// native resolution, so a dump can show where a frame first goes wrong. The 3D layer is recorded as the 2D engine
// reads it, with which 3D frame and scanline each row was read at, then both screens as the 2D engine finishes them,
// then what the UI draws, with which frame it was; 3D frames record what they were drawn with.

#pragma once

#ifdef FRAME_CAPTURE

#include <atomic>
#include <cstdint>

namespace FrameCapture {
    const int FRAMES = 24; // Consecutive frames to capture
    const int RENDERS = 64; // 3D frames to remember, by their ID

    struct Frame {
        uint32_t emuFrame; // The emulator's frame number
        uint32_t uiFrame; // The UI frame that drew it, or 0 if it wasn't drawn
        uint32_t layer[256 * 192]; // The 3D layer as the 2D engine read it
        uint32_t rowRender[192]; // The 3D frame each row came from
        uint32_t rowVCount[192]; // The scanline the emulator was on when each row was read
        uint32_t screens[256 * 192 * 2]; // Both screens as the 2D engine finished them
        uint32_t ui[256 * 192 * 2]; // The screens as the UI got them, in RGBA8
    };

    struct Render {
        uint32_t id; // The 3D frame's ID, counting every 3D frame started
        uint32_t emuFrame; // The emulator's frame number when it started
        uint32_t vCount; // The scanline it started on
        uint32_t disp3DCnt; // The 3D display control it was drawn with
        uint32_t clearImageOffset; // The clear image offset it was drawn with
        uint32_t polygons; // How many polygons it had
        uint64_t startTime, endTime; // When the renderer started and finished it, in OS ticks
    };

    inline std::atomic<int> state(0); // 0 = idle, 1 = capturing, 2 = captured, waiting to be written, 3 = written
    inline uint32_t emuFrame = 0; // The emulator's frame number, counted when frames are finished
    inline uint32_t firstFrame = 0; // The first captured frame number
    inline uint32_t renderId = 0; // The current 3D frame's ID
    inline Frame *frames = nullptr;
    inline Render renders[RENDERS];

    // Get the capture slot for an emulator frame, if it's being captured
    inline Frame *slot(uint32_t frame) {
        if (state.load() != 1 || !frames || frame < firstFrame || frame >= firstFrame + FRAMES) return nullptr;
        return &frames[frame - firstFrame];
    }
}

#endif
