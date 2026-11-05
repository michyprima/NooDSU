// Unix socket addresses for libslirp on the Wii U, which has none; they're only used by unused features
#pragma once
#include <sys/socket.h>

#define AF_UNIX 1

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
};
