#ifndef CMOSH_FRAGMENT_H
#define CMOSH_FRAGMENT_H

#include <stddef.h>
#include <stdint.h>

struct cmosh_fragment {
    uint64_t id;
    unsigned int index;
    int final;
    const unsigned char *payload;
    size_t payload_len;
};

int cmosh_zlib_store_compress(const unsigned char *in, size_t inlen,
                              unsigned char *out, size_t outlen,
                              size_t *written);
int cmosh_zlib_store_decompress(const unsigned char *in, size_t inlen,
                                unsigned char *out, size_t outlen,
                                size_t *written);
int cmosh_encode_fragment(uint64_t id, unsigned int index,
                          int final, const unsigned char *payload,
                          size_t payload_len, unsigned char *out,
                          size_t outlen, size_t *written);
int cmosh_decode_fragment(const unsigned char *buf, size_t buflen,
                          struct cmosh_fragment *out);

#endif
