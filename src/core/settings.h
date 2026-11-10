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

#include <string>
#include <vector>

struct Setting {
    std::string name;
    void *value;
    bool isString;

    Setting(std::string name, void *value, bool isString):
        name(name), value(value), isString(isString) {}
};

class Settings {
public:
    static int directBoot;
    static int romInRam;
    static int fpsLimiter;
    static int frameskip;
    static int threaded2D;
    static int threaded3D;
    static int highRes3D;
    static int screenGhost;
    static int emulateAudio;
    static int audio16Bit;
    static int savesFolder;
    static int statesFolder;
    static int cheatsFolder;
    static int screenFilter;
    static int dsiMode;
    static int arm7Hle;
    static int jit;
    static int fastTiming;
    static int hardware3D;
    static int fast2D;
    static int syncToDisplay;
    static int gpuAntiAlias;
    static int gpuOverlap;
    static int wifi; // Whether games can connect to the internet through an emulated access point
    static int slot2Device; // The accessory in the GBA slot when no GBA ROM is loaded, from Slot2Device
    static int sunlight; // How much light the solar sensor of Boktai carts sees, from 0 to 10

    // The threaded settings only choose for the software renderer, since the GPU renderer's setup relies on the threads
    static bool threads2D() { return threaded2D || hardware3D; }
    static bool worker3D() { return (threaded3D & 0xF) || hardware3D; }

    static std::string gbaBiosPath;
    static std::string ndsBios9Path;
    static std::string ndsBios7Path;
    static std::string ndsFirmPath;
    static std::string dsiBios9Path;
    static std::string dsiBios7Path;
    static std::string dsiFirmPath;
    static std::string dsiNandPath;
    static std::string sdImagePath;
    static std::string wfcDns; // The DNS server games use, which sends Nintendo WFC's servers to a revival service
    static const char *const defaultWfcDns; // WiiLink WFC's, whose login server accepts unpatched DS games
    static std::string basePath;

    static void add(std::vector<Setting> &settings);
    static bool load(std::string path = ".");
    static bool save();

private:
    static std::vector<Setting> settings;
    Settings() {} // Private to prevent instantiation
};
