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

#include <sys/stat.h>
#include "core.h"
#include "file_lock.h"

int Settings::directBoot = 1;
int Settings::romInRam = 1;
int Settings::fpsLimiter = 1;
int Settings::frameskip = 0;
int Settings::threaded2D = 1;
int Settings::threaded3D = 2;
int Settings::highRes3D = 0;
int Settings::screenGhost = 0;
int Settings::emulateAudio = 1;
int Settings::audio16Bit = 1;
int Settings::savesFolder = 0;
int Settings::statesFolder = 1;
int Settings::cheatsFolder = 1;
int Settings::screenFilter = 2;
int Settings::dsiMode = 0;
int Settings::dspHle = 1;
int Settings::arm7Hle = 0;
int Settings::jit = 1;
int Settings::fastTiming = 1;
int Settings::hardware3D = 1;
int Settings::fast2D = 1;
int Settings::syncToDisplay = 1;
int Settings::gpuAntiAlias = 0;
int Settings::gpuOverlap = 1;
int Settings::wifi = 0;
int Settings::slot2Device = 0;
int Settings::sunlight = 5;

std::string Settings::gbaBiosPath = "gba_bios.bin";
std::string Settings::ndsBios9Path = "bios9.bin";
std::string Settings::ndsBios7Path = "bios7.bin";
std::string Settings::ndsFirmPath = "firmware.bin";
std::string Settings::dsiBios9Path = "bios9i.bin";
std::string Settings::dsiBios7Path = "bios7i.bin";
std::string Settings::dsiFirmPath = "firmwarei.bin";
std::string Settings::dsiNandPath = "nand.bin";
std::string Settings::sdImagePath = "sd.img";
const char *const Settings::defaultWfcDns = "167.235.229.36";
std::string Settings::wfcDns = defaultWfcDns;
std::string Settings::basePath = ".";

std::vector<Setting> Settings::settings = {
    Setting("directBoot", &directBoot, false),
    Setting("romInRam", &romInRam, false),
    Setting("fpsLimiter", &fpsLimiter, false),
    Setting("frameskip", &frameskip, false),
    Setting("threaded2D", &threaded2D, false),
    Setting("threaded3D", &threaded3D, false),
    Setting("highRes3D", &highRes3D, false),
    Setting("screenGhost", &screenGhost, false),
    Setting("emulateAudio", &emulateAudio, false),
    Setting("audio16Bit", &audio16Bit, false),
    Setting("savesFolder", &savesFolder, false),
    Setting("statesFolder", &statesFolder, false),
    Setting("cheatsFolder", &cheatsFolder, false),
    Setting("screenFilter", &screenFilter, false),
    Setting("dsiMode", &dsiMode, false),
    Setting("dspHle", &dspHle, false),
    Setting("arm7Hle", &arm7Hle, false),
    Setting("jit", &jit, false),
    Setting("fastTiming", &fastTiming, false),
    Setting("hardware3D", &hardware3D, false),
    Setting("fast2D", &fast2D, false),
    Setting("syncToDisplay", &syncToDisplay, false),
    Setting("gpuAntiAlias", &gpuAntiAlias, false),
    Setting("gpuOverlap", &gpuOverlap, false),
    Setting("wifi", &wifi, false),
    Setting("slot2Device", &slot2Device, false),
    Setting("sunlight", &sunlight, false),
    Setting("gbaBiosPath", &gbaBiosPath, true),
    Setting("ndsBios9Path", &ndsBios9Path, true),
    Setting("ndsBios7Path", &ndsBios7Path, true),
    Setting("ndsFirmPath", &ndsFirmPath, true),
    Setting("dsiBios9Path", &dsiBios9Path, true),
    Setting("dsiBios7Path", &dsiBios7Path, true),
    Setting("dsiFirmPath", &dsiFirmPath, true),
    Setting("dsiNandPath", &dsiNandPath, true),
    Setting("sdImagePath", &sdImagePath, true),
    Setting("wfcDns", &wfcDns, true)
};

void Settings::add(std::vector<Setting> &settings) {
    // Add additional settings to be loaded from the settings file
    Settings::settings.insert(Settings::settings.end(), settings.begin(), settings.end());
}

bool Settings::load(std::string path) {
    // Set the base path and ensure all folders exist
    mkdir((basePath = path).c_str() MKDIR_ARGS);
    mkdir((basePath + "/saves").c_str() MKDIR_ARGS);
    mkdir((basePath + "/states").c_str() MKDIR_ARGS);
    mkdir((basePath + "/cheats").c_str() MKDIR_ARGS);
    mkdir((basePath + "/bios").c_str() MKDIR_ARGS);

    // Open the settings file or set defaults if it doesn't exist, with system files in the bios folder
    FILE *file = fopen((basePath + "/noodsu.ini").c_str(), "r");
    if (!file) {
        gbaBiosPath = basePath + "/bios/gba_bios.bin";
        ndsBios9Path = basePath + "/bios/bios9.bin";
        ndsBios7Path = basePath + "/bios/bios7.bin";
        ndsFirmPath = basePath + "/bios/firmware.bin";
        dsiBios9Path = basePath + "/bios/bios9i.bin";
        dsiBios7Path = basePath + "/bios/bios7i.bin";
        dsiFirmPath = basePath + "/bios/firmwarei.bin";
        dsiNandPath = basePath + "/bios/nand.bin";
        sdImagePath = basePath + "/sd.img";
        save();
        return false;
    }

    // Read each line of the settings file and load values from them
    char data[512];
    while (fgets(data, 512, file) != nullptr) {
        std::string line = data;
        size_t split = line.find('=');
        std::string name = line.substr(0, split);
        for (size_t i = 0; i < settings.size(); i++) {
            if (name != settings[i].name) continue;
            std::string value = line.substr(split + 1, line.size() - split - 2);
            if (settings[i].isString)
                *(std::string*)settings[i].value = value;
            else if (value[0] >= '0' && value[0] <= '9')
                *(int*)settings[i].value = stoi(value);
            break;
        }
    }

    // Close the file after reading it
    fclose(file);
    return true;
}

bool Settings::save() {
    std::lock_guard<FileLock> guard(fileLock());
    // Attempt to open the settings file
    FILE *file = fopen((basePath + "/noodsu.ini").c_str(), "w");
    if (!file) return false;

    // Write each setting to the settings file
    for (size_t i = 0; i < settings.size(); i++) {
        std::string value = settings[i].isString ?
            *(std::string*)settings[i].value : std::to_string(*(int*)settings[i].value);
        fprintf(file, "%s=%s\n", settings[i].name.c_str(), value.c_str());
    }

    // Close the file after writing it
    fclose(file);
    return true;
}
