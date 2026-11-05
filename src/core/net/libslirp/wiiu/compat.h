// Definitions libslirp needs that the Wii U's network headers don't have, included before every libslirp file
// IPv6 is never enabled, but its types are still used throughout
#pragma once
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define AF_INET6 10
#define PF_INET6 AF_INET6
#define INET6_ADDRSTRLEN 46

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    uint16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define IN6ADDR_ANY_INIT { { 0 } }
#define IN6ADDR_LOOPBACK_INIT { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } }

#define IPPROTO_IPV6 41
#define IPV6_V6ONLY 26
#define IPV6_UNICAST_HOPS 16
#define IPPROTO_ICMPV6 58
#define IN6_IS_ADDR_MULTICAST(a) ((a)->s6_addr[0] == 0xFF)

static const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
static const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

// Raw sockets don't exist, so creating one fails and ICMP echo isn't forwarded
#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif
#ifndef SOCK_RAW
#define SOCK_RAW 3
#endif
#ifndef IN_CLASSA_NET
#define IN_CLASSA_NET 0xFF000000
#endif

#ifndef __cplusplus
#include <string.h>

// The Wii U's recvfrom() fails with EMSGSIZE for UDP unless the buffer is large, whatever the datagram's size, so reads
// go through a big buffer; only the network thread calls libslirp, so one buffer is enough
static inline int wiiuRecvfrom(int s, void *buf, size_t n, int flags, struct sockaddr *addr, socklen_t *len) {
    static char big[8192] __attribute__((aligned(0x40)));
    if (n >= sizeof(big)) return recvfrom(s, buf, n, flags, addr, len);
    int count = recvfrom(s, big, sizeof(big), flags, addr, len);
    if (count > (int)n) count = n;
    if (count > 0) memcpy(buf, big, count);
    return count;
}
#ifndef NET_LOG
#define recvfrom(s, b, n, f, a, l) wiiuRecvfrom(s, b, n, f, a, l)
#endif
#endif

#if defined(NET_LOG) && !defined(__cplusplus)
// Log every socket call libslirp makes before and after it runs, so the call a problem happens in can be found
void netLogC(const char *format, ...);
#define NET_LOG_CALL(name, call, format, ...) ({ \
    netLogC("%s " format "\n", name, ##__VA_ARGS__); \
    int result_ = (int)(call); \
    netLogC("%s = %d errno %d\n", name, result_, errno); \
    result_; })
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#define socket(d, t, p) NET_LOG_CALL("socket", socket(d, t, p), "%d %d %d", d, t, p)
#define bind(s, a, l) NET_LOG_CALL("bind", bind(s, a, l), "%d", s)
#define connect(s, a, l) NET_LOG_CALL("connect", connect(s, a, l), "%d", s)
#define listen(s, n) NET_LOG_CALL("listen", listen(s, n), "%d", s)
#define accept(s, a, l) NET_LOG_CALL("accept", accept(s, a, l), "%d", s)
#define setsockopt(s, l, o, v, n) NET_LOG_CALL("setsockopt", setsockopt(s, l, o, v, n), "%d %d 0x%X len %d", s, l, o, (int)(n))
#define getsockopt(s, l, o, v, n) NET_LOG_CALL("getsockopt", getsockopt(s, l, o, v, n), "%d %d 0x%X", s, l, o)
#define fcntl(s, c, ...) NET_LOG_CALL("fcntl", fcntl(s, c, ##__VA_ARGS__), "%d %d", s, c)
#define ioctl(s, r, v) NET_LOG_CALL("ioctl", ioctl(s, r, v), "%d 0x%X", s, (unsigned)(r))
#define sendto(s, b, n, f, a, l) NET_LOG_CALL("sendto", sendto(s, b, n, f, a, l), "%d len %d flags %d", s, (int)(n), f)
#define send(s, b, n, f) NET_LOG_CALL("send", send(s, b, n, f), "%d len %d flags %d", s, (int)(n), f)
#define recvfrom(s, b, n, f, a, l) NET_LOG_CALL("recvfrom", wiiuRecvfrom(s, b, n, f, a, l), "%d len %d flags %d", s, (int)(n), f)
#define recv(s, b, n, f) NET_LOG_CALL("recv", recv(s, b, n, f), "%d len %d flags %d", s, (int)(n), f)
#define shutdown(s, h) NET_LOG_CALL("shutdown", shutdown(s, h), "%d %d", s, h)
#define getsockname(s, a, l) NET_LOG_CALL("getsockname", getsockname(s, a, l), "%d", s)
#define getpeername(s, a, l) NET_LOG_CALL("getpeername", getpeername(s, a, l), "%d", s)
#define close(s) NET_LOG_CALL("close", close(s), "%d", s)
#endif
