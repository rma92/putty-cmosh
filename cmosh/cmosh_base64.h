#ifndef CMOSH_BASE64_H
#define CMOSH_BASE64_H

#include <stddef.h>

int cmosh_base64_decode(const char *in, unsigned char *out, size_t outlen,
                        size_t *written);
int cmosh_base64_encode(const unsigned char *in, size_t inlen, char *out,
                        size_t outlen, size_t *written);

#endif
