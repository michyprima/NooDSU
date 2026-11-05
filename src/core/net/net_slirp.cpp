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
#include <chrono>
#include <cstring>
#include <poll.h>
#include <thread>

#ifdef __WIIU__
#include "libslirp/wiiu/compat.h"
#include <coreinit/thread.h>
#include <nn/ac/ac_c.h>
#endif
#include "libslirp/src/libslirp.h"

#include "net_slirp.h"
#include "../defines.h"
#include "../worker_thread.h"

// The virtual network's addresses, which are libslirp's defaults
#define NET_ADDR(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))
static const uint32_t NETWORK = NET_ADDR(10, 0, 2, 0);
static const uint32_t NETMASK = NET_ADDR(255, 255, 255, 0);
static const uint32_t GATEWAY = NET_ADDR(10, 0, 2, 2);
static const uint32_t DHCP_START = NET_ADDR(10, 0, 2, 15);
static const uint32_t NAMESERVER = NET_ADDR(10, 0, 2, 3);

#ifdef NET_LOG
#include <cstdarg>
#include <cstdio>

void netLog(const char *format, ...) {
    // Send a line to anything listening on UDP port 4405 on the local network, numbered so drops show
    static std::mutex logMutex;
    static int logSocket = -2;
    static uint32_t number = 0;
    std::lock_guard<std::mutex> guard(logMutex);
    if (logSocket == -2) {
        logSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int enabled = 1;
        if (logSocket >= 0) setsockopt(logSocket, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
    }
    if (logSocket < 0) return;
    char line[512];
    int length = snprintf(line, sizeof(line), "%u ", number++);
    va_list args;
    va_start(args, format);
    length += vsnprintf(line + length, sizeof(line) - length, format, args);
    va_end(args);
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(4405);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(logSocket, line, std::min<int>(length, sizeof(line) - 1), 0, (sockaddr*)&destination, sizeof(destination));
}

extern "C" void netLogC(const char *format, ...) {
    // The same for libslirp's C code, with the line formatted here first
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    netLog("%s", line);
}

void netLogFrame(const char *direction, const uint8_t *e, size_t size) {
    // Describe an Ethernet frame: its type, or for IPv4 its addresses, protocol and TCP flags
    if (size < 14) return;
    int type = (e[12] << 8) | e[13];
    if (type != 0x0800 || size < 34) {
        netLog("%s type %04X size %u\n", direction, type, (unsigned)size);
        return;
    }
    const uint8_t *ip = e + 14, *t = ip + (ip[0] & 0xF) * 4;
    netLog("%s %u.%u.%u.%u:%u > %u.%u.%u.%u:%u proto %u size %u flags %02X\n", direction, ip[12], ip[13], ip[14],
        ip[15], (t[0] << 8) | t[1], ip[16], ip[17], ip[18], ip[19], (t[2] << 8) | t[3], ip[9], (unsigned)size,
        (ip[9] == 6) ? t[13] : 0);
}
#endif

struct SlirpGlue {
    static int64_t clockNs(void *opaque) {
        // Give libslirp a monotonic clock
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static slirp_ssize_t sendPacket(const void *buf, size_t len, void *opaque) {
        // Queue a frame from the network for the DS
        NetSlirp *net = (NetSlirp*)opaque;
        const uint8_t *data = (const uint8_t*)buf;
        netLogFrame("net>ds", data, len);
        std::lock_guard<std::mutex> guard(net->mutex);
        if (net->fromNet.size() < 256)
            net->fromNet.emplace_back(data, data + len);
        return len;
    }

    static void guestError(const char *msg, void *opaque) {
        LOG_WARN("Slirp guest error: %s\n", msg);
    }

    static void *timerNew(SlirpTimerCb callback, void *cbOpaque, void *opaque) {
        // Create a timer that's checked after every poll
        NetSlirp *net = (NetSlirp*)opaque;
        NetSlirp::Timer *timer = new NetSlirp::Timer { callback, cbOpaque, -1 };
        net->timers.push_back(timer);
        return timer;
    }

    static void timerFree(void *timer, void *opaque) {
        // Remove a timer
        NetSlirp *net = (NetSlirp*)opaque;
        auto position = std::find(net->timers.begin(), net->timers.end(), (NetSlirp::Timer*)timer);
        if (position != net->timers.end()) net->timers.erase(position);
        delete (NetSlirp::Timer*)timer;
    }

    static void timerMod(void *timer, int64_t expireMs, void *opaque) {
        // Set when a timer fires, in milliseconds on the clock above
        ((NetSlirp::Timer*)timer)->expire = expireMs;
    }

    static void registerPollFd(int fd, void *opaque) {}
    static void unregisterPollFd(int fd, void *opaque) {}
    static void notify(void *opaque) {}

    static int addPoll(int fd, int events, void *opaque) {
        // Add a socket to wait on, converting libslirp's event flags
        std::vector<pollfd> &polls = *((NetSlirp*)opaque)->polls;
        pollfd entry = {};
        entry.fd = fd;
        if (events & SLIRP_POLL_IN) entry.events |= POLLIN;
        if (events & SLIRP_POLL_OUT) entry.events |= POLLOUT;
        if (events & SLIRP_POLL_PRI) entry.events |= POLLPRI;
        polls.push_back(entry);
        return polls.size() - 1;
    }

    static int getEvents(int index, void *opaque) {
        // Return what happened on a socket, converting to libslirp's event flags
        std::vector<pollfd> &polls = *((NetSlirp*)opaque)->polls;
        if (index < 0 || index >= (int)polls.size()) return 0;
        int events = 0, revents = polls[index].revents;
        if (revents & POLLIN) events |= SLIRP_POLL_IN;
        if (revents & POLLOUT) events |= SLIRP_POLL_OUT;
        if (revents & POLLPRI) events |= SLIRP_POLL_PRI;
        if (revents & POLLERR) events |= SLIRP_POLL_ERR;
        if (revents & POLLHUP) events |= SLIRP_POLL_HUP;
        return events;
    }
};

NetSlirp::NetSlirp() {
    // Start the network thread on core 2, at the same priority as the other worker threads there; it always waits in
    // poll() when idle, and a lower priority would let the 2D threads, which spin while the emulator is stopped
    // mid-frame, keep it from ever finishing when a game is closed
    polls = new std::vector<pollfd>();
    thread = new WorkerThread(WorkerThread::CORE_2, &NetSlirp::run, this);
}

NetSlirp::~NetSlirp() {
    // Stop the network thread, which cleans up libslirp
    running.store(false);
    thread->join();
    delete thread;
    delete polls;
}

void NetSlirp::send(const uint8_t *frame, size_t size) {
    // Queue a frame from the DS for the network
    std::lock_guard<std::mutex> guard(mutex);
    if (toNet.size() < 256)
        toNet.emplace_back(frame, frame + size);
}

bool NetSlirp::receive(std::vector<uint8_t> &frame) {
    // Take the next frame from the network for the DS, if there is one
    std::lock_guard<std::mutex> guard(mutex);
    if (fromNet.empty()) return false;
    frame = std::move(fromNet.front());
    fromNet.pop_front();
    return true;
}

void NetSlirp::run() {
#ifdef __WIIU__
    // Name the thread for hang reports, and connect the console to its network if it isn't already; connecting can
    // block for a while, so check if the game was closed in the meantime
    OSSetThreadName(OSGetCurrentThread(), "NooDS U Network");
    ACInitialize();
    BOOL connected = FALSE;
    if (NNResult_IsFailure(ACIsApplicationConnected(&connected)) || !connected)
        ACConnect();
    if (!running.load()) {
        ACFinalize();
        return;
    }
#endif
    netLog("network connected\n");

    // Set up the virtual network with IPv4 only, since the Wii U has no IPv6
    SlirpConfig config = {};
    config.version = 1;
    config.in_enabled = true;
    config.vnetwork.s_addr = htonl(NETWORK);
    config.vnetmask.s_addr = htonl(NETMASK);
    config.vhost.s_addr = htonl(GATEWAY);
    config.vdhcp_start.s_addr = htonl(DHCP_START);
    config.vnameserver.s_addr = htonl(NAMESERVER);
    config.if_mtu = 1500;
    config.if_mru = 1500;

    static const SlirpCb callbacks = {
        SlirpGlue::sendPacket, SlirpGlue::guestError, SlirpGlue::clockNs, SlirpGlue::timerNew,
        SlirpGlue::timerFree, SlirpGlue::timerMod, SlirpGlue::registerPollFd, SlirpGlue::unregisterPollFd,
        SlirpGlue::notify
    };
    netLog("network thread started\n");
    slirp = slirp_new(&config, &callbacks, this);
    netLog("slirp created\n");

    std::vector<std::vector<uint8_t>> frames;
    while (running.load()) {
        // Pass on frames from the DS
        {
            std::lock_guard<std::mutex> guard(mutex);
            frames.assign(std::make_move_iterator(toNet.begin()), std::make_move_iterator(toNet.end()));
            toNet.clear();
        }
        for (auto &frame : frames) {
            netLogFrame("ds>net", frame.data(), frame.size());
            slirp_input(slirp, frame.data(), frame.size());
            netLog("input done\n");
        }

        // Wait on the network's sockets, briefly so new frames from the DS aren't held up
        // Sleep instead without sockets or if waiting fails, since poll returns right away then on Wii U, and
        // spinning would starve the threads below this one on its core
        uint32_t timeout = 5;
        polls->clear();
        slirp_pollfds_fill(slirp, &timeout, SlirpGlue::addPoll, this);
        timeout = std::min<uint32_t>(timeout, 5);
        int result = polls->empty() ? -1 : poll(polls->data(), polls->size(), timeout);
#ifdef NET_LOG
        if (!polls->empty()) {
            static size_t lastCount = 0;
            if (polls->size() != lastCount || result < 0) {
                netLog("poll %u sockets, first fd %d, result %d\n", (unsigned)polls->size(), (*polls)[0].fd, result);
                lastCount = polls->size();
            }
        }
#endif
        if (result < 0 && !polls->empty()) {
            // On Wii U, poll() refuses all sockets if one is numbered above 31, so check them one at a time then,
            // leaving the ones it refuses until they expire
            result = 0;
            for (pollfd &entry : *polls) {
                pollfd single = entry;
                int count = poll(&single, 1, 0);
                entry.revents = (count > 0) ? single.revents : 0;
                if (count > 0) result++;
            }
            if (result == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(std::max<uint32_t>(timeout, 1)));
        }
        else if (result < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max<uint32_t>(timeout, 1)));
        }
        slirp_pollfds_poll(slirp, result < 0, SlirpGlue::getEvents, this);

        // Fire timers that are due
        int64_t now = SlirpGlue::clockNs(this) / 1000000;
        for (size_t i = 0; i < timers.size(); i++) {
            Timer *timer = timers[i];
            if (timer->expire < 0 || timer->expire > now) continue;
            timer->expire = -1;
            timer->callback(timer->opaque);
        }
    }

    // Clean up libslirp, closing its sockets, and release the network connection
    slirp_cleanup(slirp);
    for (Timer *timer : timers) delete timer;
    timers.clear();
#ifdef __WIIU__
    ACFinalize();
#endif
}
