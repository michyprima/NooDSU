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

#pragma once

#include <mutex>

// File accesses made while a game runs take this lock, so only one happens at a time
// On Wii U, Aroma's file redirection can leave its IO thread spinning at top priority when several threads use files
// at once, which starves everything else on that core; it's recursive, so nested accesses can't deadlock
inline std::recursive_mutex &fileLock() {
    static std::recursive_mutex lock;
    return lock;
}
