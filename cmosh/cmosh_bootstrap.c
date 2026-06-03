#include "cmosh_bootstrap.h"

#include "cmosh_base64.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

int cmosh_parse_startup(const char *text, struct cmosh_bootstrap *out)
{
    const char *p;
    int saw_connect = 0;

    if (!text || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    for (p = text; *p;) {
        const char *line = p;
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);

        if (len > 0 && line[len - 1] == '\r')
            len--;

        if (len >= 8 && memcmp(line, "MOSH IP ", 8) == 0) {
            size_t n = len - 8;
            if (n >= sizeof(out->ip))
                return -1;
            memcpy(out->ip, line + 8, n);
            out->ip[n] = '\0';
        } else if (len >= 13 && memcmp(line, "MOSH CONNECT ", 13) == 0) {
            char key64[128];
            char *endptr;
            unsigned long port;
            size_t key64_len, key_len;
            const char *q = line + 13;

            if (saw_connect)
                return -1;
            q = skip_space(q);
            port = strtoul(q, &endptr, 10);
            if (endptr == q || port == 0 || port > 65535)
                return -1;
            q = skip_space(endptr);
            key64_len = len - (size_t)(q - line);
            if (key64_len == 0 || key64_len >= sizeof(key64))
                return -1;
            memcpy(key64, q, key64_len);
            key64[key64_len] = '\0';
            if (cmosh_base64_decode(key64, out->key, sizeof(out->key),
                                    &key_len) != 0)
                return -1;
            if (key_len != CMOSH_KEY_LEN)
                return -1;
            out->port = (unsigned short)port;
            saw_connect = 1;
        }

        if (!eol)
            break;
        p = eol + 1;
    }

    return saw_connect ? 0 : -1;
}

static int locale_is_safe(const char *locale)
{
    const unsigned char *p = (const unsigned char *)locale;

    if (!locale)
        return 1;
    for (; *p; p++) {
        if (!(isalnum(*p) || *p == '_' || *p == '.' || *p == '-'))
            return 0;
    }
    return 1;
}

int cmosh_build_remote_command(const char *server, const char *port_range,
                               const char *locale, int ipv4, int ipv6,
                               int no_init, char *out, size_t outlen)
{
    int n;
    const char *srv = server && *server ? server : "mosh-server";
    const char *family = ipv4 ? " -4" : (ipv6 ? " -6" : "");
    const char *init = no_init ? " --no-init" : "";

    if (!out || ipv4 + ipv6 > 1 || !locale_is_safe(locale))
        return -1;

    if (port_range && *port_range) {
        if (locale && *locale) {
            n = snprintf(out, outlen,
                         "LC_ALL=%s LANG=%s %s new%s%s -p %s 2>&1", locale,
                         locale, srv, family, init, port_range);
        } else {
            n = snprintf(out, outlen, "%s new%s%s -p %s 2>&1", srv, family,
                         init, port_range);
        }
    } else {
        if (locale && *locale) {
            n = snprintf(out, outlen, "LC_ALL=%s LANG=%s %s new%s%s 2>&1",
                         locale, locale, srv, family, init);
        } else {
            n = snprintf(out, outlen, "%s new%s%s 2>&1", srv, family, init);
        }
    }
    if (n < 0 || (size_t)n >= outlen)
        return -1;
    return 0;
}

int cmosh_extract_network_host(const char *ssh_host, char *out, size_t outlen)
{
    const char *host, *end;
    size_t len;

    if (!ssh_host || !*ssh_host || !out || outlen == 0)
        return -1;

    host = strrchr(ssh_host, '@');
    host = host ? host + 1 : ssh_host;

    if (*host == '[') {
        host++;
        end = strchr(host, ']');
        if (!end)
            return -1;
    } else {
        end = host + strlen(host);
    }

    len = (size_t)(end - host);
    if (len == 0 || len >= outlen)
        return -1;
    memcpy(out, host, len);
    out[len] = '\0';
    return 0;
}
