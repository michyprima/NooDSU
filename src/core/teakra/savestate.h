/*
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

#include <cstdint>
#include <cstdio>

namespace melonDS {

// Stands in for melonDS's save state class, which its copy of Teakra saves itself through, with a state file instead
class Savestate {
public:
    bool Saving;

    Savestate(FILE *file, bool saving): Saving(saving), file(file) {}

    void Section(const char *magic) {}
    void Var8(uint8_t *var) { VarArray(var, sizeof(*var)); }
    void Var16(uint16_t *var) { VarArray(var, sizeof(*var)); }
    void Var32(uint32_t *var) { VarArray(var, sizeof(*var)); }
    void Var64(uint64_t *var) { VarArray(var, sizeof(*var)); }

    void Bool32(bool *var) {
        // Keep a flag as a byte
        uint8_t value = Saving ? *var : 0;
        Var8(&value);
        *var = value;
    }

    void VarArray(void *data, uint32_t len) {
        // Write the data to the file, or read it back
        if (Saving)
            fwrite(data, 1, len, file);
        else
            fread(data, 1, len, file);
    }

private:
    FILE *file;
};

}
