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

#include <algorithm>
#include <cstring>
#include "../core.h"
#include "wifi_ap.h"

#define MS_CYCLES 34418

Wifi::Wifi(Core *core): core(core) {
    // Set some default BB register values
    bbRegisters[0x00] = 0x6D;
    bbRegisters[0x5D] = 0x01;
    bbRegisters[0x64] = 0xFF;

    // Create the access point games can connect to the internet through, if enabled
    if (Settings::wifi)
        ap = new WifiAp();
}

Wifi::~Wifi() {
    delete ap;
}

void Wifi::stopNetwork() {
    // Remove the access point, ending its network thread and closing its sockets, while the emulator is stopped
    delete ap;
    ap = nullptr;
    apPending.clear();
}

void Wifi::saveState(FILE *file) {
    // Write state data to the file
    fwrite(&scheduled, sizeof(scheduled), 1, file);
    fwrite(&wModeWep, sizeof(wModeWep), 1, file);
    fwrite(&wTxstatCnt, sizeof(wTxstatCnt), 1, file);
    fwrite(&wIrf, sizeof(wIrf), 1, file);
    fwrite(&wIe, sizeof(wIe), 1, file);
    fwrite(wMacaddr, 2, sizeof(wMacaddr) / 2, file);
    fwrite(wBssid, 2, sizeof(wBssid) / 2, file);
    fwrite(&wAidFull, sizeof(wAidFull), 1, file);
    fwrite(&wRxcnt, sizeof(wRxcnt), 1, file);
    fwrite(&wPowerstate, sizeof(wPowerstate), 1, file);
    fwrite(&wPowerforce, sizeof(wPowerforce), 1, file);
    fwrite(&wRxbufBegin, sizeof(wRxbufBegin), 1, file);
    fwrite(&wRxbufEnd, sizeof(wRxbufEnd), 1, file);
    fwrite(&wRxbufWrcsr, sizeof(wRxbufWrcsr), 1, file);
    fwrite(&wRxbufWrAddr, sizeof(wRxbufWrAddr), 1, file);
    fwrite(&wRxbufRdAddr, sizeof(wRxbufRdAddr), 1, file);
    fwrite(&wRxbufReadcsr, sizeof(wRxbufReadcsr), 1, file);
    fwrite(&wRxbufGap, sizeof(wRxbufGap), 1, file);
    fwrite(&wRxbufGapdisp, sizeof(wRxbufGapdisp), 1, file);
    fwrite(wTxbufLoc, 2, sizeof(wTxbufLoc) / 2, file);
    fwrite(&wBeaconInt, sizeof(wBeaconInt), 1, file);
    fwrite(&wTxbufReply1, sizeof(wTxbufReply1), 1, file);
    fwrite(&wTxbufReply2, sizeof(wTxbufReply2), 1, file);
    fwrite(&wTxreqRead, sizeof(wTxreqRead), 1, file);
    fwrite(&wTxstat, sizeof(wTxstat), 1, file);
    fwrite(&wUsCountcnt, sizeof(wUsCountcnt), 1, file);
    fwrite(&wUsComparecnt, sizeof(wUsComparecnt), 1, file);
    fwrite(&wCmdCountcnt, sizeof(wCmdCountcnt), 1, file);
    fwrite(&wUsCompare, sizeof(wUsCompare), 1, file);
    fwrite(&wUsCount, sizeof(wUsCount), 1, file);
    fwrite(&wPreBeacon, sizeof(wPreBeacon), 1, file);
    fwrite(&wCmdCount, sizeof(wCmdCount), 1, file);
    fwrite(&wBeaconCount, sizeof(wBeaconCount), 1, file);
    fwrite(&wRxbufCount, sizeof(wRxbufCount), 1, file);
    fwrite(&wTxbufWrAddr, sizeof(wTxbufWrAddr), 1, file);
    fwrite(&wTxbufCount, sizeof(wTxbufCount), 1, file);
    fwrite(&wTxbufGap, sizeof(wTxbufGap), 1, file);
    fwrite(&wTxbufGapdisp, sizeof(wTxbufGapdisp), 1, file);
    fwrite(&wPostBeacon, sizeof(wPostBeacon), 1, file);
    fwrite(&wBbWrite, sizeof(wBbWrite), 1, file);
    fwrite(&wBbRead, sizeof(wBbRead), 1, file);
    fwrite(&wTxSeqno, sizeof(wTxSeqno), 1, file);
    fwrite(bbRegisters, 1, sizeof(bbRegisters), file);
    fwrite(wConfig, 2, sizeof(wConfig) / 2, file);
    fwrite(ports, 2, sizeof(ports) / 2, file);
    fwrite(rfRegisters, 4, sizeof(rfRegisters) / 4, file);
    fwrite(&random, sizeof(random), 1, file);
    fwrite(&channel, sizeof(channel), 1, file);
}

void Wifi::loadState(FILE *file) {
    // Read state data from the file
    fread(&scheduled, sizeof(scheduled), 1, file);
    fread(&wModeWep, sizeof(wModeWep), 1, file);
    fread(&wTxstatCnt, sizeof(wTxstatCnt), 1, file);
    fread(&wIrf, sizeof(wIrf), 1, file);
    fread(&wIe, sizeof(wIe), 1, file);
    fread(wMacaddr, 2, sizeof(wMacaddr) / 2, file);
    fread(wBssid, 2, sizeof(wBssid) / 2, file);
    fread(&wAidFull, sizeof(wAidFull), 1, file);
    fread(&wRxcnt, sizeof(wRxcnt), 1, file);
    fread(&wPowerstate, sizeof(wPowerstate), 1, file);
    fread(&wPowerforce, sizeof(wPowerforce), 1, file);
    fread(&wRxbufBegin, sizeof(wRxbufBegin), 1, file);
    fread(&wRxbufEnd, sizeof(wRxbufEnd), 1, file);
    fread(&wRxbufWrcsr, sizeof(wRxbufWrcsr), 1, file);
    fread(&wRxbufWrAddr, sizeof(wRxbufWrAddr), 1, file);
    fread(&wRxbufRdAddr, sizeof(wRxbufRdAddr), 1, file);
    fread(&wRxbufReadcsr, sizeof(wRxbufReadcsr), 1, file);
    fread(&wRxbufGap, sizeof(wRxbufGap), 1, file);
    fread(&wRxbufGapdisp, sizeof(wRxbufGapdisp), 1, file);
    fread(wTxbufLoc, 2, sizeof(wTxbufLoc) / 2, file);
    fread(&wBeaconInt, sizeof(wBeaconInt), 1, file);
    fread(&wTxbufReply1, sizeof(wTxbufReply1), 1, file);
    fread(&wTxbufReply2, sizeof(wTxbufReply2), 1, file);
    fread(&wTxreqRead, sizeof(wTxreqRead), 1, file);
    fread(&wTxstat, sizeof(wTxstat), 1, file);
    fread(&wUsCountcnt, sizeof(wUsCountcnt), 1, file);
    fread(&wUsComparecnt, sizeof(wUsComparecnt), 1, file);
    fread(&wCmdCountcnt, sizeof(wCmdCountcnt), 1, file);
    fread(&wUsCompare, sizeof(wUsCompare), 1, file);
    fread(&wUsCount, sizeof(wUsCount), 1, file);
    fread(&wPreBeacon, sizeof(wPreBeacon), 1, file);
    fread(&wCmdCount, sizeof(wCmdCount), 1, file);
    fread(&wBeaconCount, sizeof(wBeaconCount), 1, file);
    fread(&wRxbufCount, sizeof(wRxbufCount), 1, file);
    fread(&wTxbufWrAddr, sizeof(wTxbufWrAddr), 1, file);
    fread(&wTxbufCount, sizeof(wTxbufCount), 1, file);
    fread(&wTxbufGap, sizeof(wTxbufGap), 1, file);
    fread(&wTxbufGapdisp, sizeof(wTxbufGapdisp), 1, file);
    fread(&wPostBeacon, sizeof(wPostBeacon), 1, file);
    fread(&wBbWrite, sizeof(wBbWrite), 1, file);
    fread(&wBbRead, sizeof(wBbRead), 1, file);
    fread(&wTxSeqno, sizeof(wTxSeqno), 1, file);
    fread(bbRegisters, 1, sizeof(bbRegisters), file);
    fread(wConfig, 2, sizeof(wConfig) / 2, file);
    fread(ports, 2, sizeof(ports) / 2, file);
    fread(rfRegisters, 4, sizeof(rfRegisters) / 4, file);
    fread(&random, sizeof(random), 1, file);
    fread(&channel, sizeof(channel), 1, file);
}

void Wifi::addConnection(Core *core) {
    // Add an external core to this one's connection list
    mutex.lock();
    connections.push_back(&core->wifi);
    mutex.unlock();

    // Add this core to the external one's connection list
    core->wifi.mutex.lock();
    core->wifi.connections.push_back(this);
    core->wifi.mutex.unlock();
}

void Wifi::remConnection(Core *core) {
    // Remove an external core from this one's connection list
    mutex.lock();
    auto position = std::find(connections.begin(), connections.end(), &core->wifi);
    connections.erase(position);
    mutex.unlock();

    // Remove this core from the external one's connection list
    core->wifi.mutex.lock();
    position = std::find(core->wifi.connections.begin(), core->wifi.connections.end(), this);
    core->wifi.connections.erase(position);
    core->wifi.mutex.unlock();
}

void Wifi::scheduleInit() {
    // Schedule an initial millisecond tick (this will reschedule itself as needed)
    core->schedule(WIFI_COUNT_MS, MS_CYCLES);
    scheduled = true;
}

void Wifi::countMs() {
    // Process any queued packets
    if (!packets.empty())
        receivePackets();

    // Let the DSi's Wi-Fi chip scan and pass packets on
    if (core->dsiMode)
        core->dsiWifi.countMs();

    if (ap) {
        // Let the access point send its frames, if the DS is on its channel
        // Frames wait while the receive buffer is too full, which slows down the network to match
        ap->tick();
        uint8_t mac[6];
        for (int i = 0; i < 6; i++)
            mac[i] = wMacaddr[i >> 1] >> ((i & 1) * 8);
        for (int i = 0; i < 4; i++) {
            if (apPending.empty() && !ap->receive(apPending, mac)) break;
            if (!(channel == 0 || channel == WifiAp::channel)) {
                apPending.clear();
                continue;
            }
            if (!rxFits(apPending.size())) break;
            receiveFrame(apPending.data(), apPending.size());
            apPending.clear();
        }
    }

    if (wUsCountcnt) { // Counter enable
        // Decrement the beacon counter and trigger an interrupt if the pre-beacon value matches
        if (--wBeaconCount == wPreBeacon && wUsComparecnt)
            sendInterrupt(15);

        // Increment the main counter by a millisecond and handle compare events
        if ((wUsCount += 0x400) == wUsCompare || wBeaconCount == 0) {
            // Reload the beacon counter and trigger an interrupt with transmission if enabled
            wBeaconCount = wBeaconInt;
            if (wUsComparecnt) {
                sendInterrupt(14);
                if ((wTxbufLoc[BEACON_FRAME] & BIT(15)) && (wTxreqRead & BIT(BEACON_FRAME)))
                    transmitPacket(BEACON_FRAME);
            }
        }

        // Decrement the post-beacon counter and trigger an interrupt at zero
        if (wPostBeacon && --wPostBeacon == 0)
            sendInterrupt(13);
    }

    // Decrement the CMD counter every 10 microseconds and trigger an interrupt at zero
    if (wCmdCountcnt && (wCmdCount -= std::min<uint16_t>(0x400 / 10, wCmdCount)) == 0)
        sendInterrupt(12);

    // Reschedule the task as long as something is active
    if (!connections.empty() || ap || wUsCountcnt || wCmdCountcnt)
        core->schedule(WIFI_COUNT_MS, MS_CYCLES);
    else
        scheduled = false;
}

void Wifi::sendInterrupt(int bit) {
    // Trigger a WiFi interrupt if W_IF & W_IE changes from zero
    if (!(wIe & wIrf) && (wIe & BIT(bit)))
        core->interpreter[1].sendInterrupt(24);

    // Set the interrupt's request bit
    wIrf |= BIT(bit);

    // Perform additional actions for beacon interrupts
    if (bit == 14) {
        wPostBeacon = 0xFFFF;
        wTxreqRead &= 0xFFF2;
    }
}

void Wifi::receivePackets() {
    // Take the queued frames from other cores
    mutex.lock();
    std::vector<std::vector<uint8_t>> frames;
    frames.swap(packets);
    mutex.unlock();

    for (auto &frame : frames) {
        receiveFrame(frame.data(), frame.size());

        // Schedule a CMD reply or ack shortly after a packet is received
        uint16_t control = frame[0] | (frame[1] << 8);
        if (control == 0x0228) // CMD frame
            core->schedule(WIFI_TRANS_REPLY, 2048);
        else if ((control == 0x0118 || control == 0x0158) && wCmdCount) // CMD reply
            core->schedule(WIFI_TRANS_ACK, 2048);
    }
}

void Wifi::writeRxHalf(uint16_t &address, uint16_t value) {
    // Write a half-word to the circular receive buffer, wrapping at its end
    core->memory.write<uint16_t>(1, 0x4804000 + address, value);
    address = (address + 2) & 0x1FFE;
    if (address == (wRxbufEnd & 0x1FFE))
        address = wRxbufBegin & 0x1FFE;
}

bool Wifi::rxFits(size_t size) {
    // Check if a frame and its header fit in the receive buffer without reaching what hasn't been read yet
    int bufSize = (wRxbufEnd & 0x1FFE) - (wRxbufBegin & 0x1FFE);
    if (bufSize <= 0) return true;
    int used = ((int)wRxbufWrcsr - (int)(wRxbufReadcsr << 1) + bufSize) % bufSize;
    return used + (int)size + 16 < bufSize;
}

void Wifi::receiveFrame(const uint8_t *frame, size_t size) {
    // Only receive while powered on and enabled, with a buffer set up
    if ((wPowerstate & BIT(9)) || !(wRxcnt & BIT(15)) || wRxbufBegin == wRxbufEnd || size < 28)
        return;

    // Drop frames for other systems, checking the first address like the hardware does
    uint8_t mac[6], bssid[6];
    for (int i = 0; i < 6; i++) {
        mac[i] = wMacaddr[i >> 1] >> ((i & 1) * 8);
        bssid[i] = wBssid[i >> 1] >> ((i & 1) * 8);
    }
    if (!(frame[4] & 0x01) && memcmp(&frame[4], mac, 6))
        return;

    // Apply the receive filters, and work out the flags for the RX header, based on melonDS
    static const uint8_t mpCmdMac[6] = { 0x03, 0x09, 0xBF, 0x00, 0x00, 0x00 };
    static const uint8_t mpReplyMac[6] = { 0x03, 0x09, 0xBF, 0x00, 0x00, 0x10 };
    static const uint8_t mpAckMac[6] = { 0x03, 0x09, 0xBF, 0x00, 0x00, 0x03 };
    uint16_t control = frame[0] | (frame[1] << 8);
    uint16_t rxFilter = ports[0x0D0 >> 1], rxFilter2 = ports[0x0E0 >> 1];
    uint16_t flags = 0x0010;
    switch ((control >> 2) & 0x3) {
    case 0: { // Management
        if (!memcmp(&frame[16], bssid, 6)) flags |= 0x8000;
        int subtype = (control >> 4) & 0xF;
        if (subtype == 0x8) { // Beacon
            if (!(flags & 0x8000) && !(rxFilter & BIT(0))) return;
            flags |= 0x0001;
        }
        else if (subtype <= 0x5 || (subtype >= 0xA && subtype <= 0xC)) {
            if (!(flags & 0x8000) && !(rxFilter & (3 << 9))) return;
        }
        break;
    }

    case 1: // Control
        if ((control & 0xF0) != 0xA0) return; // Only PS-poll
        if (!memcmp(&frame[4], bssid, 6)) flags |= 0x8000;
        if (!(flags & 0x8000) && !(rxFilter & BIT(11))) return;
        flags |= 0x0005;
        break;

    case 2: { // Data
        int fromTo = (control >> 8) & 0x3;
        if (rxFilter2 & BIT(fromTo)) return;
        static const int bssidOffsets[] = { 16, 4, 10, 0 };
        if (bssidOffsets[fromTo] && !memcmp(&frame[bssidOffsets[fromTo]], bssid, 6)) flags |= 0x8000;
        if (!(flags & 0x8000) && !(rxFilter & BIT(11))) return;
        if ((control & BIT(11)) && !(rxFilter & BIT(0))) return; // Retransmission

        // Local multiplayer frames are recognized by their addresses
        if (!memcmp(&frame[16], mpReplyMac, 6)) flags |= ((control & 0xF0) == 0x50) ? 0x000F : 0x000E;
        else if (!memcmp(&frame[4], mpCmdMac, 6)) flags |= 0x000C;
        else if (!memcmp(&frame[4], mpAckMac, 6)) flags |= 0x000D;
        else flags |= 0x0008;

        switch ((control >> 4) & 0xF) {
        case 0x0: case 0x4: break;
        case 0x1:
            if ((flags & 0xF) == 0xD) { if (!(rxFilter & BIT(7))) return; }
            else if ((flags & 0xF) != 0xE) { if (!(rxFilter & BIT(1))) return; }
            break;
        case 0x2: if ((flags & 0xF) != 0xC && !(rxFilter & BIT(2))) return; break;
        case 0x3: if (!(rxFilter & BIT(3))) return; break;
        case 0x5:
            if ((flags & 0xF) == 0xF) { if (!(rxFilter & BIT(8))) return; }
            else if (!(rxFilter & BIT(4))) return;
            break;
        case 0x6: if (!(rxFilter & BIT(5))) return; break;
        case 0x7: if (!(rxFilter & BIT(6))) return; break;
        default: return;
        }
        break;
    }

    default:
        return;
    }

    // Crop the end of the frame if set to
    int length = size - ((ports[0x0DA >> 1] << 1) & 0x1FE);
    if (length < 0) length = 0;

    // Write the RX header and the frame to the circular buffer, signaling the start and end of reception
    sendInterrupt(6);
    uint16_t address = wRxbufWrcsr;
    writeRxHalf(address, flags);
    writeRxHalf(address, 0x0040);
    writeRxHalf(address, 0x0000);
    writeRxHalf(address, 0x0014); // 2 Mbit/s
    writeRxHalf(address, length);
    writeRxHalf(address, 0x4080); // Signal strength
    for (int i = 0; i < length; i += 2)
        writeRxHalf(address, frame[i] | ((i + 1 < length) ? (frame[i + 1] << 8) : 0));
    if (address & 0x2)
        writeRxHalf(address, 0);
    wRxbufWrcsr = address;
    sendInterrupt(0);

    // Copy the timestamp of beacons from the joined access point to the counter
    if ((flags & 0x800F) == 0x8001) {
        uint64_t timestamp = 0;
        for (int i = 0; i < 8; i++)
            timestamp |= uint64_t(frame[24 + i]) << (i * 8);
        wUsCount = timestamp;
    }
}

void Wifi::transmitPacket(PacketType type) {
    // Set the packet address and size, and modify the TX header
    uint16_t address, size;
    if (type == CMD_ACK) {
        // Set the size for a generated CMD ack
        address = 0;
        size = 40;
    }
    else {
        if (type == CMD_REPLY) {
            if (wTxbufReply1 & BIT(15)) {
                // Increment the retry count and swap the CMD reply address if set
                address = (wTxbufReply1 & 0xFFF) << 1;
                uint8_t value = core->memory.read<uint8_t>(1, 0x4804004 + address) + 1;
                if (value) core->memory.write<uint8_t>(1, 0x4804004 + address, value);
                wTxbufReply2 = wTxbufReply1;
                wTxbufReply1 = 0;
            }
            else {
                // Set the size for a generated empty CMD reply
                address = 0;
                size = 36;
                goto start;
            }
        }
        else {
            // Set the address for a regular packet
            address = (wTxbufLoc[type] & 0xFFF) << 1;
        }

        // Get the packet size and update the TX header in memory
        size = (core->memory.read<uint16_t>(1, 0x480400A + address) + 11) & ~0x3;
        core->memory.write<uint16_t>(1, 0x4804000 + address, 0x0001);
        core->memory.write<uint16_t>(1, 0x4804002 + address, 0x0000);
        core->memory.write<uint8_t>(1, 0x4804005 + address, 0x00);
    }

start:
    // Start transmitting a packet
    LOG_INFO("Instance %d sending packet of type %d with size 0x%X\n", core->id, type, size);
    sendInterrupt(7);
    mutex.lock();

    // Build the 802.11 frame, without the TX header
    uint16_t *data = new uint16_t[size / 2];
    if (type == CMD_ACK) {
        // Fill out the IEEE header and body for a CMD ack
        data[6] = 0x0218; // Frame control
        data[7] = 0x7FFF; // Duration
        data[8] = 0x0903; // Address 1
        data[9] = 0x00BF; // Address 1
        data[10] = 0x0003; // Address 1
        data[11] = wMacaddr[0]; // Address 2
        data[12] = wMacaddr[1]; // Address 2
        data[13] = wMacaddr[2]; // Address 2
        data[14] = wMacaddr[0]; // Address 3
        data[15] = wMacaddr[1]; // Address 3
        data[16] = wMacaddr[2]; // Address 3
        data[17] = 0x0000; // Sequence control
        data[18] = 0x0046; // Something?
        data[19] = 0x0000; // Error flags
    }
    else if (type == CMD_REPLY && !(wTxbufReply1 & BIT(15))) {
        // Fill out the IEEE header for an empty CMD reply
        data[6] = 0x0158; // Frame control
        data[7] = 0x7FFF; // Duration
        data[8] = 0x0903; // Address 1
        data[9] = 0x00BF; // Address 1
        data[10] = 0x0010; // Address 1
        data[11] = wMacaddr[0]; // Address 2
        data[12] = wMacaddr[1]; // Address 2
        data[13] = wMacaddr[2]; // Address 2
        data[14] = wMacaddr[0]; // Address 3
        data[15] = wMacaddr[1]; // Address 3
        data[16] = wMacaddr[2]; // Address 3
        data[17] = 0x0000; // Sequence control
    }
    else {
        // Read the rest of the packet from memory
        for (uint32_t j = 12; j < size; j += 2)
            data[j / 2] = core->memory.read<uint16_t>(1, 0x4804000 + address + j);
    }

    // Set and update the IEEE sequence number if enabled
    if (type >= BEACON_FRAME || !(wTxbufLoc[type] & BIT(13)))
        data[17] = (wTxSeqno++) << 4;

    // Convert the frame to bytes, which is how it's sent
    std::vector<uint8_t> frame(size - 12);
    for (uint32_t j = 0; j < frame.size(); j++)
        frame[j] = data[6 + j / 2] >> ((j & 1) * 8);
    delete[] data;

    // Send the frame to the access point if it's a regular one on its channel, without the checksum
    if (ap && (type == LOC1_FRAME || type == LOC2_FRAME || type == LOC3_FRAME) &&
            (channel == 0 || channel == WifiAp::channel)) {
        uint16_t length = core->memory.read<uint16_t>(1, 0x480400A + address) & 0x3FFF;
        uint8_t mac[6];
        for (int i = 0; i < 6; i++)
            mac[i] = wMacaddr[i >> 1] >> ((i & 1) * 8);
        if (length > 4 && length - 4u <= frame.size()) {
            ap->transmit(frame.data(), length - 4, mac);
            activity.store(activity.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    }

    // Add the frame to the queues of connected cores
    for (uint32_t i = 0; i < connections.size(); i++) {
        connections[i]->mutex.lock();
        connections[i]->packets.push_back(frame);
        connections[i]->mutex.unlock();
    }

    // Finish transmitting a packet
    mutex.unlock();
    sendInterrupt(1);

    // Handle special end events for certain packets
    if (type < BEACON_FRAME)
        wTxbufLoc[type] &= ~BIT(15);
    else if (type == CMD_ACK)
        sendInterrupt(12);

    // Update transmission status based on type and certain bits
    switch (type) {
        case LOC1_FRAME: wTxstat = (wTxbufLoc[0] & BIT(12)) ? 0x0701 : 0x0001; break;
        case CMD_FRAME: wTxstat = (wTxstatCnt & BIT(14)) ? 0x0801 : 0x0001; break;
        case LOC2_FRAME: wTxstat = (wTxbufLoc[2] & BIT(12)) ? 0x1701 : 0x1001; break;
        case LOC3_FRAME: wTxstat = (wTxbufLoc[3] & BIT(12)) ? 0x2701 : 0x2001; break;
        case BEACON_FRAME: wTxstat = (wTxstatCnt & BIT(15)) ? 0x0301 : 0x0001; break;
        case CMD_REPLY: wTxstat = (wTxstatCnt & BIT(12)) ? 0x0401 : 0x0001; break;
        case CMD_ACK: wTxstat = (wTxstatCnt & BIT(13)) ? 0x0B01 : 0x0001; break;
    }
}

void Wifi::writeWModeWep(uint16_t mask, uint16_t value) {
    // Write to the W_MODE_WEP register, which also sets how power is managed
    mask &= 0x007F;
    wModeWep = (wModeWep & ~mask) | (value & mask);
    uint16_t &powerDownCtrl = ports[0x048 >> 1];
    if (ports[0x038 >> 1] & BIT(1)) {
        if ((wModeWep & 0x7) == 1) powerDownCtrl |= BIT(1);
        else if ((wModeWep & 0x7) == 2) powerDownCtrl = 3;
        if ((wModeWep & 0x7) != 3) wPowerstate &= 0x0300;
        updatePower(0);
    }
}

void Wifi::writeWTxstatCnt(uint16_t mask, uint16_t value) {
    // Write to the W_TXSTAT_CNT register
    mask &= 0xF000;
    wTxstatCnt = (wTxstatCnt & ~mask) | (value & mask);
}

void Wifi::writeWIrf(uint16_t mask, uint16_t value) {
    // Write to the W_IF register
    // Setting a bit actually clears it to acknowledge an interrupt
    wIrf &= ~(value & mask);
}

void Wifi::writeWIe(uint16_t mask, uint16_t value) {
    // Trigger a WiFi interrupt if W_IF & W_IE changes from zero
    if (!(wIe & wIrf) && (value & mask & wIrf))
        core->interpreter[1].sendInterrupt(24);

    // Write to the W_IE register
    mask &= 0xFBFF;
    wIe = (wIe & ~mask) | (value & mask);
}

void Wifi::writeWMacaddr(int index, uint16_t mask, uint16_t value) {
    // Write to one of the W_MACADDR registers
    wMacaddr[index] = (wMacaddr[index] & ~mask) | (value & mask);
}

void Wifi::writeWBssid(int index, uint16_t mask, uint16_t value) {
    // Write to one of the W_BSSID registers
    wBssid[index] = (wBssid[index] & ~mask) | (value & mask);
}

void Wifi::writeWAidFull(uint16_t mask, uint16_t value) {
    // Write to the W_AID_FULL register
    mask &= 0x07FF;
    wAidFull = (wAidFull & ~mask) | (value & mask);
}

void Wifi::writeWRxcnt(uint16_t mask, uint16_t value) {
    // Write to the W_RXCNT register
    mask &= 0xFF0E;
    wRxcnt = (wRxcnt & ~mask) | (value & mask);

    // Latch W_RXBUF_WR_ADDR to W_RXBUF_WRCSR
    if (value & BIT(0))
        wRxbufWrcsr = wRxbufWrAddr << 1;
}

void Wifi::writeWPowerstate(uint16_t mask, uint16_t value) {
    // Write to the W_POWERSTATE register, which only requests power changes in manual power mode
    if ((wModeWep & 0x7) != 3) return;
    mask &= 0x0003;
    uint16_t state = (wPowerstate & 0x0300) | (((wPowerstate & ~mask) | (value & mask)) & 0x0003);
    if ((state & 0x0300) == 0x0200)
        state &= ~BIT(0);
    else
        state &= ~BIT(1);
    if (!(state & BIT(9)))
        state &= ~BIT(8);
    wPowerstate = state;
    updatePower(0);
}

void Wifi::writeWPowerforce(uint16_t mask, uint16_t value) {
    // Write to the W_POWERFORCE register, which can force the power on or off
    mask &= 0x8001;
    wPowerforce = (wPowerforce & ~mask) | (value & mask);
    updatePower(0);
}

void Wifi::setRfStatus(int status) {
    // Set the RF status and the pins that go with it, based on melonDS
    static const uint16_t pins[] = { 0x04, 0x84, 0x00, 0x46, 0x00, 0x84, 0x87, 0x00, 0x46, 0x04 };
    ports[0x214 >> 1] = status;
    ports[0x19C >> 1] = pins[status];
}

void Wifi::updatePower(int power) {
    // Turn the transceiver on or off as requested, based on melonDS's power management (1 = on, -1 = off)
    // Powering on takes effect right away instead of after 2ms, since the timer only runs while active
    uint16_t &modeReset = ports[0x004 >> 1], &trxPower = ports[0x034 >> 1];
    uint16_t &powerDownCtrl = ports[0x048 >> 1];
    int current = ((trxPower == 1) ? 1 : 0) | ((wPowerstate & BIT(9)) ? 0 : 2);
    int request = current;

    if (wPowerforce & BIT(15)) {
        request = (wPowerforce & BIT(0)) ? 0 : 3;
    }
    else if (!(modeReset & BIT(0))) {
        request = 0;
    }
    else {
        if (power == 0) {
            if ((wPowerstate & 0x0202) == 0x0202) power = 1;
            else if ((wPowerstate & 0x0201) == 0x0001) power = -1;
        }
        if (power == -1 && (powerDownCtrl & BIT(0))) power = 0;
        if (power == 1) request = 3;
        else if (power == -1) request = powerDownCtrl ? 3 : 0;
        else if (powerDownCtrl & BIT(1)) request = 3;
    }
    if (request == current) return;

    // Set the transceiver's power
    if (request & 1) {
        if (!(current & 1)) {
            trxPower = 1;
            setRfStatus(1);
        }
    }
    else {
        trxPower = 0;
        setRfStatus(9);
    }

    // Set the power state, signaling when it comes on
    if (request & 2) {
        if (!(current & 2)) {
            wPowerstate |= BIT(8);
            sendInterrupt(11);
            wPowerstate = 0;
            setRfStatus(1);
        }
    }
    else {
        wPowerstate = (wPowerstate & ~(BIT(0) | BIT(8))) | BIT(9);
    }
}

void Wifi::changeChannel() {
    // Find the channel the RF registers are set to, using the values for each one in the firmware
    // The firmware's RF chip type decides how they're stored, which the RF transfers also depend on
    channel = 0;
    size_t size;
    const uint8_t *firm = core->spi.getFirmware(size);
    if (!firm || size < 0x200) return;
    for (int i = 0; i < 14; i++) {
        uint32_t data1, data2;
        int index1, index2;
        if (firm[0x44] == 3) {
            index1 = firm[0x11A];
            index2 = firm[0x129];
            data1 = firm[0x11B + i];
            data2 = firm[0x12A + i];
        }
        else {
            const uint8_t *values = &firm[0xF6 + i * 6];
            index1 = firm[0xF6 + 2] >> 2;
            index2 = firm[0xF6 + 5] >> 2;
            data1 = values[0] | (values[1] << 8) | ((values[2] & 0x3) << 16);
            data2 = values[3] | (values[4] << 8) | ((values[5] & 0x3) << 16);
        }
        if (rfRegisters[index1 & 0x3F] == data1 && rfRegisters[index2 & 0x3F] == data2) {
            channel = i + 1;
            return;
        }
    }
}

void Wifi::writeWRxbufBegin(uint16_t mask, uint16_t value) {
    // Write to the W_RXBUF_BEGIN register
    wRxbufBegin = (wRxbufBegin & ~mask) | (value & mask);
}

void Wifi::writeWRxbufEnd(uint16_t mask, uint16_t value) {
    // Write to the W_RXBUF_END register
    wRxbufEnd = (wRxbufEnd & ~mask) | (value & mask);
}

void Wifi::writeWRxbufWrAddr(uint16_t mask, uint16_t value) {
    // Write to the W_RXBUF_WR_ADDR register
    mask &= 0x0FFF;
    wRxbufWrAddr = (wRxbufWrAddr & ~mask) | (value & mask);
}

void Wifi::writeWRxbufRdAddr(uint16_t mask, uint16_t value) {
    // Write to the W_RXBUF_RD_ADDR register
    mask &= 0x1FFE;
    wRxbufRdAddr = (wRxbufRdAddr & ~mask) | (value & mask);
}

void Wifi::writeWRxbufReadcsr(uint16_t mask, uint16_t value) {
    // Write to the W_RXBUF_READCSR register
    mask &= 0x0FFF;
    wRxbufReadcsr = (wRxbufReadcsr & ~mask) | (value & mask);
}

void Wifi::writeWRxbufGap(uint16_t mask, uint16_t value) {
    // Write to the W_RXBUF_GAP register
    mask &= 0x1FFE;
    wRxbufGap = (wRxbufGap & ~mask) | (value & mask);
}

void Wifi::writeWRxbufGapdisp(uint16_t mask, uint16_t value) {
    // Write to the W_RXBUF_GAPDISP register
    mask &= 0x0FFF;
    wRxbufGapdisp = (wRxbufGapdisp & ~mask) | (value & mask);
}

void Wifi::writeWRxbufCount(uint16_t mask, uint16_t value) {
    // Write to the W_RXBUF_COUNT register
    mask &= 0x0FFF;
    wRxbufCount = (wRxbufCount & ~mask) | (value & mask);
}

void Wifi::writeWTxbufWrAddr(uint16_t mask, uint16_t value) {
    // Write to the W_TXBUF_WR_ADDR register
    mask &= 0x1FFE;
    wTxbufWrAddr = (wTxbufWrAddr & ~mask) | (value & mask);
}

void Wifi::writeWTxbufCount(uint16_t mask, uint16_t value) {
    // Write to the W_TXBUF_COUNT register
    mask &= 0x0FFF;
    wTxbufCount = (wTxbufCount & ~mask) | (value & mask);
}

void Wifi::writeWTxbufWrData(uint16_t mask, uint16_t value) {
    // Write a value to WiFi RAM
    core->memory.write<uint16_t>(1, 0x4804000 + wTxbufWrAddr, value & mask);

    // Increment the write address
    wTxbufWrAddr += 2;
    if (wTxbufWrAddr == wTxbufGap)
        wTxbufWrAddr += wTxbufGapdisp << 1;
    wTxbufWrAddr &= 0x1FFF;

    // Decrement the write counter and trigger an interrupt at the end
    if (wTxbufCount > 0 && --wTxbufCount == 0)
        sendInterrupt(8);
}

void Wifi::writeWTxbufGap(uint16_t mask, uint16_t value) {
    // Write to the W_TXBUF_GAP register
    mask &= 0x1FFE;
    wTxbufGap = (wTxbufGap & ~mask) | (value & mask);
}

void Wifi::writeWTxbufGapdisp(uint16_t mask, uint16_t value) {
    // Write to the W_TXBUF_GAPDISP register
    mask &= 0x0FFF;
    wTxbufGapdisp = (wTxbufGapdisp & ~mask) | (value & mask);
}

void Wifi::writeWTxbufLoc(PacketType type, uint16_t mask, uint16_t value) {
    // Write to one of the W_TXBUF_[BEACON,CMD,LOC1,LOC2,LOC3] registers
    wTxbufLoc[type] = (wTxbufLoc[type] & ~mask) | (value & mask);

    // Send a packet to connected cores if triggered for non-beacons
    if (type != BEACON_FRAME && (wTxbufLoc[type] & BIT(15)) && (wTxreqRead & BIT(type)))
        transmitPacket(type);
}

void Wifi::writeWBeaconInt(uint16_t mask, uint16_t value) {
    // Write to the W_BEACON_INT register
    mask &= 0x03FF;
    wBeaconInt = (wBeaconInt & ~mask) | (value & mask);

    // Reload the beacon millisecond counter
    wBeaconCount = wBeaconInt;
}

void Wifi::writeWTxbufReply1(uint16_t mask, uint16_t value) {
    // Write to the W_TXBUF_REPLY1 register
    wTxbufReply1 = (wTxbufReply1 & ~mask) | (value & mask);
}

void Wifi::writeWTxreqReset(uint16_t mask, uint16_t value) {
    // Clear bits in W_TXREQ_READ
    mask &= 0x000F;
    wTxreqRead &= ~(value & mask);
}

void Wifi::writeWTxreqSet(uint16_t mask, uint16_t value) {
    // Set bits in W_TXREQ_READ
    mask &= 0x000F;
    wTxreqRead |= (value & mask);

    // Send a packet to connected cores if triggered for non-beacons
    for (int i = 0; i < 4; i++)
        if ((wTxbufLoc[i] & BIT(15)) && (wTxreqRead & BIT(i)))
            transmitPacket(PacketType(i));
}

void Wifi::writeWUsCountcnt(uint16_t mask, uint16_t value) {
    // Write to the W_US_COUNTCNT register
    mask &= 0x0001;
    wUsCountcnt = (wUsCountcnt & ~mask) | (value & mask);
}

void Wifi::writeWUsComparecnt(uint16_t mask, uint16_t value) {
    // Write to the W_US_COMPARECNT register
    mask &= 0x0001;
    wUsComparecnt = (wUsComparecnt & ~mask) | (value & mask);

    // Trigger an immediate beacon interrupt if requested
    if (value & BIT(1))
        sendInterrupt(14);
}

void Wifi::writeWCmdCountcnt(uint16_t mask, uint16_t value) {
    // Write to the W_CMD_COUNTCNT register
    mask &= 0x0001;
    wCmdCountcnt = (wCmdCountcnt & ~mask) | (value & mask);
}

void Wifi::writeWUsCompare(int index, uint16_t mask, uint16_t value) {
    // Write to part of the W_US_COMPARE register
    int shift = index * 16;
    mask &= (index ? 0xFFFF : 0xFC00);
    wUsCompare = (wUsCompare & ~(uint64_t(mask) << shift)) | (uint64_t(value & mask) << shift);
}

void Wifi::writeWUsCount(int index, uint16_t mask, uint16_t value) {
    // Write to part of the W_US_COUNT register
    int shift = index * 16;
    wUsCount = (wUsCount & ~(uint64_t(mask) << shift)) | (uint64_t(value & mask) << shift);
}

void Wifi::writeWPreBeacon(uint16_t mask, uint16_t value) {
    // Write to the W_PRE_BEACON register
    wPreBeacon = (wPreBeacon & ~mask) | (value & mask);
}

void Wifi::writeWCmdCount(uint16_t mask, uint16_t value) {
    // Write to the W_CMD_COUNT register
    wCmdCount = (wCmdCount & ~mask) | (value & mask);
}

void Wifi::writeWBeaconCount(uint16_t mask, uint16_t value) {
    // Write to the W_BEACON_COUNT register
    wBeaconCount = (wBeaconCount & ~mask) | (value & mask);
}

void Wifi::writeWConfig(int index, uint16_t mask, uint16_t value) {
    const uint16_t masks[] = {
        0x81FF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0FFF,
        0x8FFF, 0xFFFF, 0xFFFF, 0x00FF, 0x00FF,
        0x00FF, 0x00FF, 0xFFFF, 0xFF3F, 0x7A7F
    };

    // Write to one of the W_CONFIG registers
    mask &= masks[index];
    wConfig[index] = (wConfig[index] & ~mask) | (value & mask);
}

void Wifi::writeWPostBeacon(uint16_t mask, uint16_t value) {
    // Write to the W_POST_BEACON register
    wPostBeacon = (wPostBeacon & ~mask) | (value & mask);
}

void Wifi::writeWBbCnt(uint16_t mask, uint16_t value) {
    // Perform a BB register transfer
    int index = value & 0xFF;
    switch ((value & 0xF000) >> 12) {
    case 5: // Write
        if ((index >= 0x01 && index <= 0x0C) || (index >= 0x13 && index <= 0x15) || (index >= 0x1B && index <= 0x26) ||
            (index >= 0x28 && index <= 0x4C) || (index >= 0x4E && index <= 0x5C) || (index >= 0x62 && index <= 0x63) ||
            (index == 0x65) || (index >= 0x67 && index <= 0x68)) // Writable registers
            bbRegisters[index] = wBbWrite;
        break;

    case 6: // Read
        wBbRead = bbRegisters[index];
        break;
    }
}

void Wifi::writeWBbWrite(uint16_t mask, uint16_t value) {
    // Write to the W_BB_WRITE register
    wBbWrite = (wBbWrite & ~mask) | (value & mask);
}

void Wifi::writeWIrfSet(uint16_t mask, uint16_t value) {
    // Trigger a WiFi interrupt if W_IF & W_IE changes from zero
    if (!(wIe & wIrf) && (wIe & value & mask))
        core->interpreter[1].sendInterrupt(24);

    // Set bits in the W_IF register
    mask &= 0xFBFF;
    wIrf |= (value & mask);
}

uint16_t Wifi::readWRxbufRdData() {
    // Read a value from WiFi RAM
    uint16_t value = core->memory.read<uint16_t>(1, 0x4804000 + wRxbufRdAddr);

    // Increment the read address
    if ((wRxbufRdAddr += 2) == wRxbufGap)
        wRxbufRdAddr += wRxbufGapdisp << 1;
    if (int bufSize = (wRxbufEnd & 0x1FFE) - (wRxbufBegin & 0x1FFE))
        wRxbufRdAddr = ((wRxbufBegin & 0x1FFE) + (wRxbufRdAddr - (wRxbufBegin & 0x1FFE)) % bufSize) & 0x1FFE;

    // Decrement the read counter and trigger an interrupt at the end
    if (wRxbufCount > 0 && --wRxbufCount == 0)
        sendInterrupt(9);
    return value;
}

uint16_t Wifi::readPort(uint32_t address) {
    // Read from a register without its own handling, most of which just hold their value
    switch (address &= 0xFFE) {
    case 0x000: { // W_ID, which depends on the console the firmware is from
        size_t size;
        const uint8_t *firm = core->spi.getFirmware(size);
        return (firm && size > 0x1D && (firm[0x1D] == 0x20 || firm[0x1D] == 0x63)) ? 0xC340 : 0x1440;
    }

    case 0x044: // W_RANDOM, a simple generator
        random = (random & 0x1) ^ (((random & 0x3FF) << 1) | (random >> 10));
        return random;

    case 0x0B6: case 0x15E: case 0x180: // W_TXBUSY, W_BB_BUSY, W_RF_BUSY, never busy since transfers are instant
        return 0;

    case 0x1D0: case 0x1D2: case 0x1D4: case 0x1D6: case 0x1D8: case 0x1DA: case 0x1DC: case 0x1DE: { // W_CMD_STAT
        // These clear when read
        uint16_t value = ports[address >> 1];
        ports[address >> 1] = 0;
        return value;
    }

    default:
        return ports[address >> 1];
    }
}

void Wifi::writePort(uint32_t address, uint16_t mask, uint16_t value) {
    // Write to a register without its own handling, most of which just hold their value
    uint16_t &port = ports[(address &= 0xFFE) >> 1];
    switch (address) {
    case 0x000: case 0x034: case 0x044: case 0x0B6: case 0x15E: case 0x180: case 0x19C: case 0x214:
        // Read-only registers
        return;

    case 0x004: { // W_MODE_RST
        uint16_t old = port;
        port = (port & ~(mask & 0x0001)) | (value & mask & 0x0001);
        if ((old ^ port) & BIT(0))
            updatePower(0);
        if (value & mask & BIT(13)) { // Reset some registers
            wRxbufWrAddr = 0;
            ports[0x0C0 >> 1] = 0;
            ports[0x0C4 >> 1] = 0;
            ports[0x1A4 >> 1] = 0;
            ports[0x278 >> 1] = 0x000F;
        }
        if (value & mask & BIT(14)) { // Reset more registers
            wModeWep = 0;
            wTxstatCnt = 0;
            ports[0x00A >> 1] = 0;
            memset(wMacaddr, 0, sizeof(wMacaddr));
            memset(wBssid, 0, sizeof(wBssid));
            ports[0x028 >> 1] = 0;
            wAidFull = 0;
            ports[0x02C >> 1] = 0x0707;
            ports[0x02E >> 1] = 0;
            wRxbufBegin = 0x4000;
            wRxbufEnd = 0x4800;
            ports[0x084 >> 1] = 0;
            ports[0x0BC >> 1] = 0x0001;
            ports[0x0D0 >> 1] = 0x0401;
            ports[0x0D4 >> 1] = 0x0001;
            ports[0x0E0 >> 1] = 0x0008;
            ports[0x0EC >> 1] = 0x3F03;
            ports[0x194 >> 1] = 0;
            ports[0x198 >> 1] = 0;
            ports[0x1A2 >> 1] = 0x0001;
            ports[0x224 >> 1] = 0x0003;
            ports[0x230 >> 1] = 0x0047;
        }
        return;
    }

    case 0x036: // W_POWER_US
        port = (port & ~mask) | (value & mask & 0x0003);
        return;

    case 0x038: // W_POWER_TX, which applies the power mode when bit 1 is set
        port = (port & ~mask) | (value & mask & 0x0003);
        if (port & BIT(1)) {
            uint16_t &powerDownCtrl = ports[0x048 >> 1];
            if ((wModeWep & 0x7) == 1) powerDownCtrl |= BIT(1);
            else if ((wModeWep & 0x7) == 2) powerDownCtrl = 3;
            updatePower(0);
        }
        return;

    case 0x048: { // W_POWERDOWNCTRL
        port = (port & ~mask) | (value & mask & 0x0003);
        if (ports[0x038 >> 1] & BIT(1)) {
            if ((wModeWep & 0x7) == 1) port |= BIT(1);
            else if ((wModeWep & 0x7) == 2) port = 3;
        }
        updatePower(0);
        return;
    }

    case 0x0B4: // W_TXSLOT_RESET, which cancels queued transfers
        if (value & mask & BIT(0)) wTxbufLoc[LOC1_FRAME] &= ~BIT(15);
        if (value & mask & BIT(1)) wTxbufLoc[CMD_FRAME] &= ~BIT(15);
        if (value & mask & BIT(2)) wTxbufLoc[LOC2_FRAME] &= ~BIT(15);
        if (value & mask & BIT(3)) wTxbufLoc[LOC3_FRAME] &= ~BIT(15);
        if (value & mask & BIT(6)) wTxbufReply2 &= ~BIT(15);
        if (value & mask & BIT(7)) wTxbufReply1 &= ~BIT(15);
        return;

    case 0x17C: { // W_RF_DATA2, which starts an RF transfer for the firmware's RF chip type
        port = (port & ~mask) | (value & mask);
        uint16_t &data1 = ports[0x17E >> 1];
        size_t size;
        const uint8_t *firm = core->spi.getFirmware(size);
        if (firm && size > 0x44 && firm[0x44] == 3) {
            int index = (data1 >> 8) & 0x3F;
            if ((port & 0xF) == 6) { // Read
                data1 = (data1 & 0xFF00) | (rfRegisters[index] & 0xFF);
            }
            else if ((port & 0xF) == 5) { // Write
                rfRegisters[index] = data1 & 0xFF;
                changeChannel();
            }
        }
        else {
            int index = (port >> 2) & 0x1F;
            if (port & BIT(7)) { // Read
                data1 = rfRegisters[index];
                port = (port & 0xFFFC) | ((rfRegisters[index] >> 16) & 0x3);
            }
            else { // Write
                rfRegisters[index] = data1 | ((port & 0x3) << 16);
                changeChannel();
            }
        }
        return;
    }

    default:
        port = (port & ~mask) | (value & mask);
        return;
    }
}
