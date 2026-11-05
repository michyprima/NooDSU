// Scatter/gather buffers for libslirp on the Wii U, whose headers don't have them
#pragma once
#include <stddef.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};
