#ifndef CMOSH_AES_H
#define CMOSH_AES_H

#include <stdint.h>

struct cmosh_aes128 {
    uint8_t round_key[176];
};

void cmosh_aes128_init(struct cmosh_aes128 *ctx, const uint8_t key[16]);
void cmosh_aes128_encrypt_block(const struct cmosh_aes128 *ctx,
                                const uint8_t in[16], uint8_t out[16]);
void cmosh_aes128_decrypt_block(const struct cmosh_aes128 *ctx,
                                const uint8_t in[16], uint8_t out[16]);

#endif
