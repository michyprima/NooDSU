// Interface names for libslirp on the Wii U, which has none; only used for IPv6 DNS scopes
#pragma once

static inline unsigned int if_nametoindex(const char *name) { (void)name; return 0; }
