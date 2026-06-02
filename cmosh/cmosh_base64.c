#include "cmosh_base64.h"

#include <string.h>

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

int cmosh_base64_decode(const char *in, unsigned char *out, size_t outlen,
                        size_t *written)
{
    size_t i, n = 0, data_chars = 0, pad_chars = 0;
    int val = 0, valb = -8;

    if (!in || !out || !written)
        return -1;

    for (i = 0; in[i]; i++) {
        int d;
        unsigned char c = (unsigned char)in[i];

        if (c == '=')
            break;
        d = b64_value(c);
        if (d < 0)
            return -1;
        data_chars++;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            if (n >= outlen)
                return -1;
            out[n++] = (unsigned char)((val >> valb) & 0xff);
            valb -= 8;
        }
    }

    while (in[i] == '=') {
        pad_chars++;
        i++;
    }
    if (in[i] != '\0')
        return -1;
    if (data_chars % 4 == 1 || pad_chars > 2)
        return -1;
    if (pad_chars && (data_chars + pad_chars) % 4 != 0)
        return -1;
    if (!pad_chars && data_chars % 4 != 0)
        return -1;

    *written = n;
    return 0;
}

int cmosh_base64_encode(const unsigned char *in, size_t inlen, char *out,
                        size_t outlen, size_t *written)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, n = 0;

    if (!in || !out || !written)
        return -1;

    for (i = 0; i < inlen; i += 3) {
        unsigned int x = (unsigned int)in[i] << 16;
        size_t rem = inlen - i;
        if (rem > 1)
            x |= (unsigned int)in[i + 1] << 8;
        if (rem > 2)
            x |= in[i + 2];
        if (n + 4 >= outlen)
            return -1;
        out[n++] = alphabet[(x >> 18) & 0x3f];
        out[n++] = alphabet[(x >> 12) & 0x3f];
        out[n++] = rem > 1 ? alphabet[(x >> 6) & 0x3f] : '=';
        out[n++] = rem > 2 ? alphabet[x & 0x3f] : '=';
    }

    if (n >= outlen)
        return -1;
    out[n] = '\0';
    *written = n;
    return 0;
}
