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

// Optional frames that fill the space around the screens with the look of a Nintendo DSi, drawn on the CPU whenever the
// colour or layout changes so that showing them only costs one textured quad per display

#pragma once

#include <cstdint>

namespace ScreenFrame {
    const int COLOR_COUNT = 8;
    extern const char *const colorNames[COLOR_COUNT];

    struct Screen {
        float x, y, width, height;
        bool bottom; // Whether this is the bottom screen, which has the buttons beside it instead of the speakers
    };

    // Draw a frame for a display showing one or two screens, with its pixels in the same format as the screen textures
    // The hinge's lights and camera are only drawn if asked for, since the real system only has one hinge to show them on
    void draw(uint32_t *pixels, int width, int height, int color, const Screen *screens, int count, bool lights,
        const uint8_t *fontBmp, const uint8_t *charWidths);
}
