#ifndef CMOSH_PLATFORM_H
#define CMOSH_PLATFORM_H

#include <stddef.h>

void cmosh_console_setup(void);
int cmosh_run_bootstrap_command(const char *ssh_command, const char *host,
                                const char *remote_command, char *output,
                                size_t output_len, int verbose);
int cmosh_udp_check(const char *host, unsigned short port, int ipv4, int ipv6,
                    int verbose);
int cmosh_udp_probe_encrypted(const char *host, unsigned short port, int ipv4,
                              int ipv6, const unsigned char key[16],
                              int verbose);

#endif
