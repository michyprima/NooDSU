/*
    Copyright 2019-2026 Hydr8gon

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
#include <deque>
#include <vector>

class NetSlirp;

// An access point the DS can connect to, made in software like melonDS's, which passes its data frames to a virtual
// network as Ethernet frames
// Frames are 802.11 frames without the hardware's headers, and frames for the DS end with a dummy checksum
class WifiAp {
public:
    static const uint8_t mac[6];
    static const char ssid[8];
    static const int channel = 6;

    WifiAp();
    ~WifiAp();

    void tick();
    void transmit(const uint8_t *frame, size_t size, const uint8_t *clientMac);
    bool receive(std::vector<uint8_t> &frame, const uint8_t *clientMac);

private:
    NetSlirp *net = nullptr;
    std::deque<std::vector<uint8_t>> replies;
    uint64_t timestamp = 0x428888000ULL; // Microseconds, from an arbitrary start like a real access point
    uint16_t seqNo = 0x120;
    uint32_t beaconMs = 0;
    int status = 0; // 0 = not authenticated, 1 = authenticated, 2 = associated

    std::vector<uint8_t> header(uint16_t control, const uint8_t *dest, const uint8_t *bssid, const uint8_t *src);
    std::vector<uint8_t> beacon(uint16_t control, const uint8_t *dest);
    void finish(std::vector<uint8_t> &frame);
    void handleManagement(const uint8_t *frame, size_t size);
};
