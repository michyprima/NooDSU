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

#include <cstdint>
#include <cstdio>
#include <cstring>

// Control how many cycles of 3D GPU commands are batched
#define GPU3D_BATCH 256
#define GPU3D_START 64
#define SPU_BATCH 16

// Print critical logs in red if enabled
#if LOG_LEVEL > 0
#define LOG_CRIT(...) printf("\x1b[31m" __VA_ARGS__)
#else
#define LOG_CRIT(...) (0)
#endif

// Print warning logs in yellow if enabled
#if LOG_LEVEL > 1
#define LOG_WARN(...) printf("\x1b[33m" __VA_ARGS__)
#else
#define LOG_WARN(...) (0)
#endif

// Print info logs normally if enabled
#if LOG_LEVEL > 2
#define LOG_INFO(...) printf("\x1b[0m" __VA_ARGS__)
#else
#define LOG_INFO(...) (0)
#endif

// Compatibility toggle for systems that don't have fdopen
#ifdef NO_FDOPEN
#define fdopen(...) (0)
#define ftruncate(...) (0)
#else
#include <unistd.h>
#endif

// Macro to handle differing mkdir arguments on Windows
#ifdef WINDOWS
#define MKDIR_ARGS
#else
#define MKDIR_ARGS , 0777
#endif

// Macros to force or prevent inlining
#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#define NO_INLINE __declspec(noinline)
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#define NO_INLINE __attribute__((noinline))
#endif

// Simple bit macros
#define BIT(i) (1U << (i))
#define BITL(i) (1ULL << (i))

// Macro to swap two values
#define SWAP(a, b) { \
    auto c = a; \
    a = b; \
    b = c; \
}

// Macros that read a value larger than 8 bits from an 8-bit array
#ifdef ENDIAN_BIG
#define U8TO16(data, index) __builtin_bswap16(*(uint16_t*)&(data)[index])
#define U8TO32(data, index) __builtin_bswap32(*(uint32_t*)&(data)[index])
#define U8TO64(data, index) __builtin_bswap64(*(uint64_t*)&(data)[index])
#else
#define U8TO16(data, index) (*(uint16_t*)&(data)[index])
#define U8TO32(data, index) (*(uint32_t*)&(data)[index])
#define U8TO64(data, index) (*(uint64_t*)&(data)[index])
#endif

// Macro that stores a 32-bit value to an 8-bit array
#ifdef ENDIAN_BIG
#define U32TO8(data, index, value) (*(uint32_t*)&(data)[index] = __builtin_bswap32(value))
#else
#define U32TO8(data, index, value) (*(uint32_t*)&(data)[index] = (value))
#endif

// Templates that read or write an LSB-first value of any size from an 8-bit array
// These compile to single accesses, which are byte-reversed on big-endian hosts
template <typename T> FORCE_INLINE T readLsb(const uint8_t *data) {
    T value;
    memcpy(&value, data, sizeof(T));
#ifdef ENDIAN_BIG
    switch (sizeof(T)) {
        case 2: return __builtin_bswap16(value);
        case 4: return __builtin_bswap32(value);
    }
#endif
    return value;
}

template <typename T> FORCE_INLINE void writeLsb(uint8_t *data, T value) {
#ifdef ENDIAN_BIG
    switch (sizeof(T)) {
        case 2: value = __builtin_bswap16(value); break;
        case 4: value = __builtin_bswap32(value); break;
    }
#endif
    memcpy(data, &value, sizeof(T));
}

// Claim a 32-byte cache block on Wii U, where memory accesses that miss the cache are slow
// This zeroes the block in the cache without reading it, so it's only safe when the whole block will be overwritten
FORCE_INLINE void claimBlock(void *data) {
    __asm__ volatile("dcbz 0, %0" :: "r"(data) : "memory");
}

// Macros that perform bit rotations
#define ROL8(v, s) ((v << s) | (v >> (8 - s)))
#define ROL32(v, s) ((v << s) | (v >> (32 - s)))
#define ROR32(v, s) ((v >> s) | (v << (32 - s)))
