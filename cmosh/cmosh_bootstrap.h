#ifndef CMOSH_BOOTSTRAP_H
#define CMOSH_BOOTSTRAP_H

#include <stddef.h>

#define CMOSH_KEY_LEN 16

struct cmosh_bootstrap {
    char host[256];
    char ip[128];
    unsigned short port;
    unsigned char key[CMOSH_KEY_LEN];
};

int cmosh_parse_startup(const char *text, struct cmosh_bootstrap *out);
int cmosh_build_remote_command(const char *server, const char *port_range,
                               const char *locale, int ipv4, int ipv6,
                               int no_init, char *out, size_t outlen);
int cmosh_extract_network_host(const char *ssh_host, char *out, size_t outlen);

#endif
