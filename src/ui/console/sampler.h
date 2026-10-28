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

// A sampling profiler for Wii U builds made with PROFILE_SAMPLER
// A periodic alarm on the emulator's core records where it was interrupted, and the pause menu shows a summary

#pragma once

#include <string>

namespace Sampler {
    // The pause menu item comes after the 3D benchmark when that's included too
#ifdef PROFILE_EMU
    static const int MENU_INDEX = 8;
#else
    static const int MENU_INDEX = 7;
#endif

    void start();
    void stop();
    std::string report(int page);
}
