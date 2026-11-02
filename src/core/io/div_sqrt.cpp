/*
    Copyright 2019-2026 Hydr8gon
    Copyright 2026 michyprima

    This file is part of NooDSU.

    NooDSU is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDSU is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDSU. If not, see <https://www.gnu.org/licenses/>.
*/

#include "../core.h"

template <typename T> static T intSqrt(T value) {
    // Find the integer square root one result bit at a time, starting from the highest bit pair that's set
    T root = 0;
    for (T bit = T(1) << ((sizeof(T) * 8 - 1 - (sizeof(T) > 4 ? __builtin_clzll(value | 1) :
        __builtin_clz(value | 1))) & ~1); bit; bit >>= 2) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        }
        else {
            root >>= 1;
        }
    }
    return root;
}

void DivSqrt::saveState(FILE *file) {
    // Write state data to the file, with the results up to date
    updateResults();
    fwrite(&divCnt, sizeof(divCnt), 1, file);
    fwrite(&divNumer, sizeof(divNumer), 1, file);
    fwrite(&divDenom, sizeof(divDenom), 1, file);
    fwrite(&divResult, sizeof(divResult), 1, file);
    fwrite(&divRemResult, sizeof(divRemResult), 1, file);
    fwrite(&sqrtCnt, sizeof(sqrtCnt), 1, file);
    fwrite(&sqrtResult, sizeof(sqrtResult), 1, file);
    fwrite(&sqrtParam, sizeof(sqrtParam), 1, file);
}

void DivSqrt::loadState(FILE *file) {
    // Read state data from the file
    fread(&divCnt, sizeof(divCnt), 1, file);
    fread(&divNumer, sizeof(divNumer), 1, file);
    fread(&divDenom, sizeof(divDenom), 1, file);
    fread(&divResult, sizeof(divResult), 1, file);
    fread(&divRemResult, sizeof(divRemResult), 1, file);
    fread(&sqrtCnt, sizeof(sqrtCnt), 1, file);
    fread(&sqrtResult, sizeof(sqrtResult), 1, file);
    fread(&sqrtParam, sizeof(sqrtParam), 1, file);
    divDirty = sqrtDirty = false;
}

void DivSqrt::updateResults() {
    // Calculate results that were invalidated by writes, which is done when they're read so that setting up
    // an operation over several writes only calculates it once
    if (divDirty) divide();
    if (sqrtDirty) squareRoot();
}

void DivSqrt::divide() {
    divDirty = false;
    // Set the division by zero bit based on the full 64-bit denominator, even in 32-bit mode
    divDenom ? (divCnt &= ~BIT(14)) : (divCnt |= BIT(14));

    // Calculate the division result and remainder
    switch (divCnt & 0x3) { // Division mode
    case 0: // 32-bit / 32-bit
        if ((int32_t)divNumer == INT32_MIN && (int32_t)divDenom == -1) { // Overflow
            divResult = (int32_t)divNumer ^ (0xFFFFFFFFULL << 32);
            divRemResult = 0;
        }
        else if ((int32_t)divDenom != 0) {
            divResult = (int32_t)divNumer / (int32_t)divDenom;
            divRemResult = (int32_t)divNumer % (int32_t)divDenom;
        }
        else { // Division by 0
            divResult = (((int32_t)divNumer < 0) ? 1 : -1) ^ (0xFFFFFFFFULL << 32);
            divRemResult = (int32_t)divNumer;
        }
        break;

    case 1: case 3: // 64-bit / 32-bit
        if (divNumer == INT64_MIN && (int32_t)divDenom == -1) { // Overflow
            divResult = divNumer;
            divRemResult = 0;
        }
        else if ((int32_t)divDenom != 0) {
            divResult = divNumer / (int32_t)divDenom;
            divRemResult = divNumer % (int32_t)divDenom;
        }
        else { // Division by 0
            divResult = (divNumer < 0) ? 1 : -1;
            divRemResult = divNumer;
        }
        break;

    case 2: // 64-bit / 64-bit
        if (divNumer == INT64_MIN && divDenom == -1) { // Overflow
            divResult = divNumer;
            divRemResult = 0;
        }
        else if (divDenom != 0) {
            divResult = divNumer / divDenom;
            divRemResult = divNumer % divDenom;
        }
        else { // Division by 0
            divResult = (divNumer < 0) ? 1 : -1;
            divRemResult = divNumer;
        }
        break;
    }
}

void DivSqrt::squareRoot() {
    // Calculate the square root result
    sqrtDirty = false;
    switch (sqrtCnt & 0x1) { // Square root mode
    case 0: // 32-bit
        sqrtResult = intSqrt<uint32_t>(sqrtParam);
        break;

    case 1: // 64-bit
        sqrtResult = (sqrtParam >> 32) ? intSqrt<uint64_t>(sqrtParam) : intSqrt<uint32_t>(sqrtParam);
        break;
    }
}

void DivSqrt::writeDivCnt(uint16_t mask, uint16_t value) {
    // Write to the DIVCNT register and invalidate the division result
    mask &= 0x0003;
    divCnt = (divCnt & ~mask) | (value & mask);
    divDirty = true;
}

void DivSqrt::writeDivNumerL(uint32_t mask, uint32_t value) {
    // Write to the DIVNUMER register and invalidate the division result
    divNumer = (divNumer & ~((uint64_t)mask)) | (value & mask);
    divDirty = true;
}

void DivSqrt::writeDivNumerH(uint32_t mask, uint32_t value) {
    // Write to the DIVNUMER register and invalidate the division result
    divNumer = (divNumer & ~((uint64_t)mask << 32)) | ((uint64_t)(value & mask) << 32);
    divDirty = true;
}

void DivSqrt::writeDivDenomL(uint32_t mask, uint32_t value) {
    // Write to the DIVDENOM register and invalidate the division result
    divDenom = (divDenom & ~((uint64_t)mask)) | (value & mask);
    divDirty = true;
}

void DivSqrt::writeDivDenomH(uint32_t mask, uint32_t value) {
    // Write to the DIVDENOM register and invalidate the division result
    divDenom = (divDenom & ~((uint64_t)mask << 32)) | ((uint64_t)(value & mask) << 32);
    divDirty = true;
}

void DivSqrt::writeSqrtCnt(uint16_t mask, uint16_t value) {
    // Write to the SQRTCNT register and invalidate the square root result
    mask &= 0x0001;
    sqrtCnt = (sqrtCnt & ~mask) | (value & mask);
    sqrtDirty = true;
}

void DivSqrt::writeSqrtParamL(uint32_t mask, uint32_t value) {
    // Write to the SQRTPARAM register and invalidate the square root result
    sqrtParam = (sqrtParam & ~((uint64_t)mask)) | (value & mask);
    sqrtDirty = true;
}

void DivSqrt::writeSqrtParamH(uint32_t mask, uint32_t value) {
    // Write to the SQRTPARAM register and invalidate the square root result
    sqrtParam = (sqrtParam & ~((uint64_t)mask << 32)) | ((uint64_t)(value & mask) << 32);
    sqrtDirty = true;
}
