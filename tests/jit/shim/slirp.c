// libslirp is only built for the console; the tests keep Wi-Fi off, so its functions just have to link
#include <stdio.h>
#include <stdlib.h>

static void missing(const char *name) { fprintf(stderr, "%s isn't in the test build\n", name); abort(); }
void slirp_new(void) { missing("slirp_new"); }
void slirp_cleanup(void) { missing("slirp_cleanup"); }
void slirp_input(void) { missing("slirp_input"); }
void slirp_pollfds_fill(void) { missing("slirp_pollfds_fill"); }
void slirp_pollfds_poll(void) { missing("slirp_pollfds_poll"); }
