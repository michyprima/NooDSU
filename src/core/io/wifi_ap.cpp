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

#include <cstring>
#include "wifi_ap.h"
#include "../net/net_slirp.h"
#include "../defines.h"

const uint8_t WifiAp::mac[6] = { 0x00, 0xF0, 0x77, 0x77, 0x77, 0x77 };
const char WifiAp::ssid[8] = "NooDS U";

static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Appends little-endian values to a frame
static void add8(std::vector<uint8_t> &f, uint8_t value) { f.push_back(value); }
static void add16(std::vector<uint8_t> &f, uint16_t value) { f.push_back(value); f.push_back(value >> 8); }
static void add64(std::vector<uint8_t> &f, uint64_t value) { for (int i = 0; i < 8; i++) f.push_back(value >> (i * 8)); }
static void addBytes(std::vector<uint8_t> &f, const uint8_t *data, size_t size) { f.insert(f.end(), data, data + size); }
static uint16_t get16(const uint8_t *data) { return data[0] | (data[1] << 8); }

WifiAp::WifiAp() {
    // Start the network behind the access point
    net = new NetSlirp();
}

WifiAp::~WifiAp() {
    delete net;
}

std::vector<uint8_t> WifiAp::header(uint16_t control, const uint8_t *dest, const uint8_t *bssid, const uint8_t *src) {
    // Start a frame with an 802.11 header, with the addresses in the given order
    std::vector<uint8_t> f;
    add16(f, control);
    add16(f, 0); // Duration
    addBytes(f, dest, 6);
    addBytes(f, bssid, 6);
    addBytes(f, src, 6);
    add16(f, seqNo);
    seqNo += 0x10;
    return f;
}

std::vector<uint8_t> WifiAp::beacon(uint16_t control, const uint8_t *dest) {
    // Build a beacon or probe response, which describe the access point the same way
    std::vector<uint8_t> f = header(control, dest, mac, mac);
    add64(f, timestamp);
    add16(f, 128); // Beacon interval
    add16(f, 0x0021); // Capabilities: access point, short preamble
    add8(f, 0x00); add8(f, sizeof(ssid) - 1); // SSID
    addBytes(f, (const uint8_t*)ssid, sizeof(ssid) - 1);
    add8(f, 0x01); add8(f, 0x02); add8(f, 0x82); add8(f, 0x84); // Supported rates: 1 and 2 Mbit/s
    add8(f, 0x03); add8(f, 0x01); add8(f, channel); // Current channel
    if (control == 0x0080) {
        add8(f, 0x05); add8(f, 0x04); add8(f, 0); add8(f, 1); add8(f, 0); add8(f, 0); // Traffic indication map
    }
    return f;
}

void WifiAp::finish(std::vector<uint8_t> &frame) {
    // Pad the frame to a multiple of 4 bytes and add the checksum, which isn't checked
    while (frame.size() & 0x3) frame.push_back(0xFF);
    add16(frame, 0xBEEF);
    add16(frame, 0xDEAD);
}

void WifiAp::tick() {
    // Advance the timestamp by a millisecond, and send a beacon about every 128ms
    timestamp += 0x400;
    if (++beaconMs >= 128) {
        beaconMs = 0;
        std::vector<uint8_t> frame = beacon(0x0080, broadcast);
        finish(frame);
        replies.push_back(std::move(frame));
    }
}

void WifiAp::handleManagement(const uint8_t *frame, size_t size) {
    // Reply to the management frames for finding and joining the access point
    const uint8_t *source = &frame[10];
    bool forUs = !memcmp(&frame[16], mac, 6);
    std::vector<uint8_t> reply;

    switch ((get16(frame) >> 4) & 0xF) {
    case 0x0: // Association request
        if (!forUs || status < 1) return;
        status = 2;
        LOG_INFO("Wi-Fi access point: client associated\n");
        netLog("client associated\n");
        reply = header(0x0010, source, mac, mac);
        add16(reply, 0x0021); // Capabilities
        add16(reply, 0); // Success
        add16(reply, 0xC001); // Association ID 1
        add8(reply, 0x01); add8(reply, 0x02); add8(reply, 0x82); add8(reply, 0x84); // Supported rates
        break;

    case 0x4: // Probe request, answered unless it asks for another network
        if (size >= 26 && frame[24] == 0x00 && frame[25] != 0 &&
            (frame[25] != sizeof(ssid) - 1 || size < 26u + frame[25] || memcmp(&frame[26], ssid, frame[25])))
            return;
        reply = beacon(0x0050, source);
        break;

    case 0xA: // Disassociation
        if (!forUs) return;
        status = 1;
        LOG_INFO("Wi-Fi access point: client disassociated\n");
        netLog("client disassociated\n");
        return;

    case 0xB: // Authentication, which is open
        if (!forUs) return;
        status = 1;
        LOG_INFO("Wi-Fi access point: client authenticated\n");
        netLog("client authenticated\n");
        reply = header(0x00B0, source, mac, mac);
        add16(reply, 0); // Open system
        add16(reply, 2); // Sequence
        add16(reply, 0); // Success
        break;

    case 0xC: // Deauthentication
        if (!forUs) return;
        status = 0;
        LOG_INFO("Wi-Fi access point: client deauthenticated\n");
        netLog("client deauthenticated\n");
        return;

    default:
        return;
    }

    finish(reply);
    replies.push_back(std::move(reply));
}

void WifiAp::transmit(const uint8_t *frame, size_t size, const uint8_t *clientMac) {
    // Handle a frame the DS sent, without its checksum
    if (size < 24) return;
    uint16_t control = get16(frame);
    switch ((control >> 2) & 0x3) {
    case 0: // Management
        handleManagement(frame, size);
        return;

    case 2: // Data, passed to the network as Ethernet when it goes to the access point with an LLC/SNAP header
        if ((control & 0x0300) != 0x0100 || status != 2 || memcmp(&frame[4], mac, 6) || size < 32) return;
        if (frame[24] != 0xAA || frame[25] != 0xAA || frame[26] != 0x03 || frame[27] || frame[28] || frame[29])
            return;
        {
            std::vector<uint8_t> ethernet;
            addBytes(ethernet, &frame[16], 6); // Destination
            addBytes(ethernet, &frame[10], 6); // Source
            addBytes(ethernet, &frame[30], size - 30); // Type and payload
            net->send(ethernet.data(), ethernet.size());
        }
        return;
    }
}

bool WifiAp::receive(std::vector<uint8_t> &frame, const uint8_t *clientMac) {
    // Send replies and beacons first
    if (!replies.empty()) {
        frame = std::move(replies.front());
        replies.pop_front();
        return true;
    }

    // Pass on frames from the network for the DS once it's associated, as data frames from the access point
    std::vector<uint8_t> ethernet;
    while (status == 2 && net->receive(ethernet)) {
        if (ethernet.size() < 14) continue;
        if (!(ethernet[0] & 0x01) && memcmp(&ethernet[0], clientMac, 6)) continue;
        frame = header(0x0208, &ethernet[0], mac, &ethernet[6]);
        add8(frame, 0xAA); add8(frame, 0xAA); add8(frame, 0x03); // LLC/SNAP header
        add8(frame, 0); add8(frame, 0); add8(frame, 0);
        addBytes(frame, &ethernet[12], ethernet.size() - 12); // Type and payload
        finish(frame);
        return true;
    }
    return false;
}

void WifiAp::sendEthernet(const uint8_t *frame, size_t size) {
    net->send(frame, size);
}

bool WifiAp::receiveEthernet(std::vector<uint8_t> &frame) {
    return net->receive(frame);
}
