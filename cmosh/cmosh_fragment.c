#include "cmosh_fragment.h"

#include "ssh.h"

#include <string.h>

static unsigned int adler32(const unsigned char *p, size_t len)
{
    unsigned int a = 1, b = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        a = (a + p[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

int cmosh_zlib_store_compress(const unsigned char *in, size_t inlen,
                              unsigned char *out, size_t outlen,
                              size_t *written)
{
    unsigned int sum;

    if ((!in && inlen) || !out || !written || inlen > 65535 ||
        outlen < 11 || inlen > outlen - 11)
        return -1;

    out[0] = 0x78;
    out[1] = 0x01;
    out[2] = 0x01; /* Final uncompressed deflate block. */
    out[3] = (unsigned char)(inlen & 0xff);
    out[4] = (unsigned char)(inlen >> 8);
    out[5] = (unsigned char)(~inlen & 0xff);
    out[6] = (unsigned char)((~inlen >> 8) & 0xff);
    if (inlen)
        memcpy(out + 7, in, inlen);

    sum = adler32(in, inlen);
    out[7 + inlen] = (unsigned char)(sum >> 24);
    out[8 + inlen] = (unsigned char)(sum >> 16);
    out[9 + inlen] = (unsigned char)(sum >> 8);
    out[10 + inlen] = (unsigned char)sum;
    *written = inlen + 11;
    return 0;
}

static int cmosh_zlib_store_decompress_simple(const unsigned char *in,
                                              size_t inlen,
                                              unsigned char *out,
                                              size_t outlen, size_t *written)
{
    size_t pos = 2, opos = 0;

    if (!in || inlen < 6 || !out || !written)
        return -1;
    if (in[0] != 0x78)
        return -1;

    while (pos + 5 <= inlen) {
        unsigned int final = in[pos] & 1;
        unsigned int type = (in[pos] >> 1) & 3;
        unsigned int len, nlen;

        pos++;
        if (type != 0)
            return -1;
        len = (unsigned int)in[pos] | ((unsigned int)in[pos + 1] << 8);
        nlen = (unsigned int)in[pos + 2] | ((unsigned int)in[pos + 3] << 8);
        pos += 4;
        if (((len ^ nlen) & 0xffffU) != 0xffffU ||
            pos > inlen || len > inlen - pos ||
            inlen - pos - len < 4 ||
            opos > outlen || len > outlen - opos)
            return -1;
        memcpy(out + opos, in + pos, len);
        pos += len;
        opos += len;
        if (final) {
            *written = opos;
            return 0;
        }
    }

    return -1;
}

int cmosh_zlib_store_decompress(const unsigned char *in, size_t inlen,
                                unsigned char *out, size_t outlen,
                                size_t *written)
{
    ssh_decompressor *zd;
    unsigned char *zout = NULL;
    int zoutlen = 0;

    if (!in || !out || !written || inlen <= 4 || inlen > 0x7fffffffU)
        return -1;
    if (cmosh_zlib_store_decompress_simple(in, inlen, out, outlen, written) ==
        0)
        return 0;

    zd = ssh_zlib.decompress_new();
    if (!zd)
        return -1;
    if (ssh_zlib.decompress(zd, in, (int)(inlen - 4), &zout, &zoutlen) &&
        zoutlen > 0 && (size_t)zoutlen <= outlen) {
        if (zoutlen)
            memcpy(out, zout, (size_t)zoutlen);
        *written = (size_t)zoutlen;
        sfree(zout);
        ssh_zlib.decompress_free(zd);
        return 0;
    }
    sfree(zout);
    ssh_zlib.decompress_free(zd);
    return cmosh_zlib_store_decompress_simple(in, inlen, out, outlen, written);
}

int cmosh_zlib_store_decompress_legacy(const unsigned char *in, size_t inlen,
                                       unsigned char *out, size_t outlen,
                                       size_t *written)
{
    return cmosh_zlib_store_decompress_simple(in, inlen, out, outlen, written);
}

int cmosh_encode_fragment(uint64_t id, unsigned int index,
                          int final, const unsigned char *payload,
                          size_t payload_len, unsigned char *out,
                          size_t outlen, size_t *written)
{
    unsigned int combined;
    int i;

    if ((!payload && payload_len) || !out || !written || index > 0x7fff ||
        outlen < 10 || payload_len > outlen - 10)
        return -1;

    for (i = 7; i >= 0; i--) {
        out[i] = (unsigned char)(id & 0xff);
        id >>= 8;
    }
    combined = index | (final ? 0x8000U : 0);
    out[8] = (unsigned char)(combined >> 8);
    out[9] = (unsigned char)combined;
    if (payload_len)
        memcpy(out + 10, payload, payload_len);
    *written = payload_len + 10;
    return 0;
}

int cmosh_decode_fragment(const unsigned char *buf, size_t buflen,
                          struct cmosh_fragment *out)
{
    unsigned int i, combined;
    uint64_t id = 0;

    if (!buf || buflen < 10 || !out)
        return -1;

    for (i = 0; i < 8; i++)
        id = (id << 8) | buf[i];
    combined = ((unsigned int)buf[8] << 8) | buf[9];
    out->id = id;
    out->index = combined & 0x7fffU;
    out->final = (combined & 0x8000U) != 0;
    out->payload = buf + 10;
    out->payload_len = buflen - 10;
    return 0;
}
