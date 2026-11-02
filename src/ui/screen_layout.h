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

#pragma once

class ScreenLayout {
public:
    static int screenPosition;
    static int screenRotation;
    static int screenArrangement;
    static int screenSizing;
    static int screenGap;
    static int aspectRatio;
    static int integerScale;
    static int gbaCrop;

    // Set by frontends while a second display is available; arrangements 4 and 5 then give it the bottom screen, like
    // single screen arrangement, and are horizontal and vertical otherwise
    static bool splitDisplays;
    static int arrangement() {
        if (screenArrangement < 4) return screenArrangement;
        return splitDisplays ? 3 : (screenArrangement == 4) ? 2 : 1;
    }

    int winWidth = 0, winHeight = 0;
    int minWidth = 0, minHeight = 0;
    int topX = 0, botX = 0;
    int topY = 0, botY = 0;
    int topWidth = 0, botWidth = 0;
    int topHeight = 0, botHeight = 0;

    static void addSettings();
    void update(int winWidth, int winHeight, bool gbaMode, bool splitScreens = false);

    int getTouchX(int x, int y);
    int getTouchY(int x, int y);
};
