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

// A frame tracer for Wii U builds made with both PROFILE_EMU and HEARTBEAT
// A periodic alarm on the emulator's core samples where it is, and the heartbeat logger writes the samples of slow
// frames to trace.bin, with their frame records and the JIT's compiled blocks, to be matched to symbols offline

#pragma once

#include <cstdio>

namespace FrameTrace {
    void start();
    void stop();
    void write(const char *path);
}
