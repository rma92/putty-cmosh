#include "cmosh_bootstrap.h"
#include "cmosh_platform.h"
#include "cmosh_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct options {
    const char *ssh;
    const char *server;
    const char *locale;
    const char *port_range;
    const char *host;
    int ipv4, ipv6, no_init, verbose;
};

static void usage(FILE *fp)
{
    fputs("usage: cmosh [options] [user@]host\n"
          "  --ssh=COMMAND\n"
          "  --server=COMMAND\n"
          "  --locale=LOCALE      default en_US.UTF-8; empty disables prefix\n"
          "  -p, --port=PORT[:PORT2]\n"
          "  -4 | -6\n"
          "  --predict=never|adaptive|always\n"
          "  --no-init\n"
          "  --verbose\n", fp);
}

static int parse_args(int argc, char **argv, struct options *opt)
{
    int i;

    memset(opt, 0, sizeof(*opt));
#ifdef _WIN32
    opt->ssh = "plink";
#else
    opt->ssh = "ssh";
#endif
    opt->server = "mosh-server";
    opt->locale = "en_US.UTF-8";

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--ssh=", 6) == 0)
            opt->ssh = argv[i] + 6;
        else if (strncmp(argv[i], "--server=", 9) == 0)
            opt->server = argv[i] + 9;
        else if (strncmp(argv[i], "--locale=", 9) == 0)
            opt->locale = argv[i] + 9;
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            opt->port_range = argv[++i];
        else if (strncmp(argv[i], "--port=", 7) == 0)
            opt->port_range = argv[i] + 7;
        else if (strcmp(argv[i], "-4") == 0)
            opt->ipv4 = 1;
        else if (strcmp(argv[i], "-6") == 0)
            opt->ipv6 = 1;
        else if (strncmp(argv[i], "--predict=", 10) == 0) {
            const char *p = argv[i] + 10;
            if (strcmp(p, "never") && strcmp(p, "adaptive") &&
                strcmp(p, "always"))
                return -1;
        } else if (strcmp(argv[i], "--no-init") == 0)
            opt->no_init = 1;
        else if (strcmp(argv[i], "--verbose") == 0)
            opt->verbose = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return 1;
        else if (argv[i][0] == '-')
            return -1;
        else if (!opt->host)
            opt->host = argv[i];
        else
            return -1;
    }

    if (!opt->host || (opt->ipv4 && opt->ipv6))
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    struct options opt;
    struct cmosh_bootstrap boot;
    char remote_command[512];
    char output[8192];
    char udp_host[256];
    int prc = parse_args(argc, argv, &opt);

    if (prc == 1) {
        usage(stdout);
        return 0;
    } else if (prc != 0) {
        usage(stderr);
        return 2;
    }

    cmosh_console_setup();

    if (cmosh_build_remote_command(opt.server, opt.port_range, opt.locale,
                                   opt.ipv4, opt.ipv6, opt.no_init,
                                   remote_command, sizeof(remote_command)) !=
        0) {
        fputs("cmosh: could not build remote mosh-server command\n", stderr);
        return 2;
    }

    if (cmosh_run_bootstrap_command(opt.ssh, opt.host, remote_command, output,
                                    sizeof(output), opt.verbose) != 0) {
        fputs("cmosh: SSH bootstrap failed\n", stderr);
        return 1;
    }

    if (cmosh_parse_startup(output, &boot) != 0) {
        fputs("cmosh: did not receive a valid MOSH CONNECT line\n", stderr);
        return 1;
    }

    if (boot.ip[0]) {
        if (strlen(boot.ip) >= sizeof(udp_host)) {
            fputs("cmosh: server IP is too long\n", stderr);
            return 1;
        }
        strcpy(udp_host, boot.ip);
    } else if (cmosh_extract_network_host(opt.host, udp_host,
                                          sizeof(udp_host)) != 0) {
        fputs("cmosh: could not derive UDP host from SSH target\n", stderr);
        return 1;
    }

    if (cmosh_udp_check(udp_host, boot.port, opt.ipv4, opt.ipv6,
                        opt.verbose) != 0) {
        fprintf(stderr, "cmosh: could not open UDP path to %s:%u\n", udp_host,
                boot.port);
        return 1;
    }

    if (!cmosh_transport_crypto_available()) {
        fprintf(stderr,
                "cmosh: server is on UDP port %u%s%s, but AES-OCB transport "
                "is not implemented in this clean-room stub\n",
                boot.port, udp_host[0] ? " at " : "", udp_host);
        return 1;
    }

    if (cmosh_udp_probe_encrypted(udp_host, boot.port, opt.ipv4, opt.ipv6,
                                  boot.key, opt.verbose) < 0) {
        fprintf(stderr, "cmosh: encrypted UDP probe to %s:%u failed\n",
                udp_host, boot.port);
        return 1;
    }

    if (opt.verbose)
        fprintf(stderr, "cmosh: session ended for %s:%u\n", udp_host,
                boot.port);
    return 0;

    return 0;
}
