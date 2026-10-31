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

// Renders DS 3D on the Wii U's GPU, as an alternative to the software renderer
// GX2 is shared with the UI, which holds gx2Mutex while it builds a frame

#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>

extern std::mutex gx2Mutex;
extern volatile uint32_t gpu3dStage, gpu3dFrames; // The renderer's step and frame count, for hang reports

// Appends a report of a hang to hang.txt with every thread's state, returning the file so more can be added
FILE *openHangReport(const char *reason);

void initGpu3D();
