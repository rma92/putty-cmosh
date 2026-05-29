#ifndef CMOSH_OCB_H
#define CMOSH_OCB_H

#include <stddef.h>
#include <stdint.h>

#define CMOSH_OCB_TAG_LEN 16

int cmosh_ocb_encrypt(const uint8_t key[16], const uint8_t *nonce,
                      size_t nonce_len, const uint8_t *ad, size_t ad_len,
                      const uint8_t *plain, size_t plain_len, uint8_t *out,
                      size_t out_len, size_t *written);
int cmosh_ocb_decrypt(const uint8_t key[16], const uint8_t *nonce,
                      size_t nonce_len, const uint8_t *ad, size_t ad_len,
                      const uint8_t *cipher, size_t cipher_len, uint8_t *out,
                      size_t out_len, size_t *written);

#endif
