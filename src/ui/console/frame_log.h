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

// A frame log for Wii U builds made with FRAME_LOG, cheap enough not to change what it measures
// The emulator and UI threads note the time of each frame in memory, which is written to framelog.bin in small batches

#pragma once

#include <cstdint>

namespace FrameLog {
    void emulatorStarted();
    void emulatorFrame(uint32_t finished, uint32_t polygons);
    void uiFrame(uint32_t frameId);
    void audioBuffer(uint32_t dry, uint32_t fill);
    void write(const char *path);
#ifdef AUDIO_CAPTURE
    void audioProduced(uint32_t sample);
    void audioOutput(const uint32_t *samples, int count);
    void audioFinal(int device, const int32_t *left, const int32_t *right, int count);
    void mark();
    bool shouldDeferWrite();
#endif
}
