#include "cmosh_ocb.h"

#include "cmosh_aes.h"

#include <string.h>

static void xor16(uint8_t out[16], const uint8_t a[16], const uint8_t b[16])
{
    unsigned int i;
    for (i = 0; i < 16; i++)
        out[i] = (uint8_t)(a[i] ^ b[i]);
}

static void xor16_in(uint8_t out[16], const uint8_t in[16])
{
    unsigned int i;
    for (i = 0; i < 16; i++)
        out[i] ^= in[i];
}

static void double_block(uint8_t out[16], const uint8_t in[16])
{
    unsigned int i;
    uint8_t carry = 0, next;
    for (i = 16; i-- > 0;) {
        next = (uint8_t)(in[i] >> 7);
        out[i] = (uint8_t)((in[i] << 1) | carry);
        carry = next;
    }
    if (carry)
        out[15] ^= 0x87;
}

static unsigned int ntz_size(size_t n)
{
    unsigned int z = 0;
    while ((n & 1) == 0) {
        z++;
        n >>= 1;
    }
    return z;
}

static void get_l(const uint8_t l0[16], unsigned int idx, uint8_t out[16])
{
    unsigned int i;
    memcpy(out, l0, 16);
    for (i = 0; i < idx; i++) {
        uint8_t t[16];
        double_block(t, out);
        memcpy(out, t, 16);
    }
}

static void ocb_hash(const struct cmosh_aes128 *aes, const uint8_t lstar[16],
                     const uint8_t ldollar[16], const uint8_t *ad,
                     size_t ad_len, uint8_t sum[16])
{
    uint8_t l0[16], offset[16], tmp[16], block[16];
    size_t full = ad_len / 16, rem = ad_len % 16, i;

    (void)ldollar;
    memset(sum, 0, 16);
    memset(offset, 0, 16);
    double_block(l0, ldollar);

    for (i = 1; i <= full; i++) {
        uint8_t li[16];
        get_l(l0, ntz_size(i), li);
        xor16_in(offset, li);
        xor16(tmp, ad + 16 * (i - 1), offset);
        cmosh_aes128_encrypt_block(aes, tmp, block);
        xor16_in(sum, block);
    }

    if (rem) {
        xor16_in(offset, lstar);
        memset(tmp, 0, 16);
        memcpy(tmp, ad + 16 * full, rem);
        tmp[rem] = 0x80;
        xor16_in(tmp, offset);
        cmosh_aes128_encrypt_block(aes, tmp, block);
        xor16_in(sum, block);
    }
}

static void initial_offset(const struct cmosh_aes128 *aes, const uint8_t *nonce,
                           size_t nonce_len, uint8_t offset[16])
{
    uint8_t formatted[16], ktop[16], stretch[24];
    unsigned int bottom, bit;

    memset(formatted, 0, 16);
    formatted[0] = 0; /* 128-bit tag encodes as zero. */
    formatted[15 - nonce_len] = 1;
    memcpy(formatted + 16 - nonce_len, nonce, nonce_len);
    bottom = formatted[15] & 0x3f;
    formatted[15] &= 0xc0;

    cmosh_aes128_encrypt_block(aes, formatted, ktop);
    memcpy(stretch, ktop, 16);
    for (bit = 0; bit < 8; bit++)
        stretch[16 + bit] = (uint8_t)(ktop[bit] ^ ktop[bit + 1]);

    for (bit = 0; bit < 16; bit++) {
        unsigned int bitpos = bottom + bit * 8;
        offset[bit] = (uint8_t)((stretch[bitpos / 8] << (bitpos % 8)) |
                                (stretch[bitpos / 8 + 1] >>
                                 (8 - (bitpos % 8))));
    }
}

int cmosh_ocb_encrypt(const uint8_t key[16], const uint8_t *nonce,
                      size_t nonce_len, const uint8_t *ad, size_t ad_len,
                      const uint8_t *plain, size_t plain_len, uint8_t *out,
                      size_t out_len, size_t *written)
{
    struct cmosh_aes128 aes;
    uint8_t zero[16], lstar[16], ldollar[16], l0[16], offset[16], checksum[16];
    uint8_t tmp[16], pad[16], tag[16], hash[16];
    size_t full = plain_len / 16, rem = plain_len % 16, i;

    if (!key || !nonce || nonce_len == 0 || nonce_len > 15 || !out ||
        !written || out_len < plain_len + CMOSH_OCB_TAG_LEN ||
        (plain_len && !plain) || (ad_len && !ad))
        return -1;

    cmosh_aes128_init(&aes, key);
    memset(zero, 0, 16);
    cmosh_aes128_encrypt_block(&aes, zero, lstar);
    double_block(ldollar, lstar);
    double_block(l0, ldollar);
    ocb_hash(&aes, lstar, ldollar, ad, ad_len, hash);
    initial_offset(&aes, nonce, nonce_len, offset);
    memset(checksum, 0, 16);

    for (i = 1; i <= full; i++) {
        uint8_t li[16];
        get_l(l0, ntz_size(i), li);
        xor16_in(offset, li);
        xor16(tmp, plain + 16 * (i - 1), offset);
        cmosh_aes128_encrypt_block(&aes, tmp, pad);
        xor16(out + 16 * (i - 1), pad, offset);
        xor16_in(checksum, plain + 16 * (i - 1));
    }

    if (rem) {
        xor16_in(offset, lstar);
        cmosh_aes128_encrypt_block(&aes, offset, pad);
        for (i = 0; i < rem; i++)
            out[16 * full + i] = (uint8_t)(plain[16 * full + i] ^ pad[i]);
        memset(tmp, 0, 16);
        memcpy(tmp, plain + 16 * full, rem);
        tmp[rem] = 0x80;
        xor16_in(checksum, tmp);
    }

    xor16(tmp, checksum, offset);
    xor16_in(tmp, ldollar);
    cmosh_aes128_encrypt_block(&aes, tmp, tag);
    xor16_in(tag, hash);
    memcpy(out + plain_len, tag, CMOSH_OCB_TAG_LEN);
    *written = plain_len + CMOSH_OCB_TAG_LEN;
    return 0;
}

int cmosh_ocb_decrypt(const uint8_t key[16], const uint8_t *nonce,
                      size_t nonce_len, const uint8_t *ad, size_t ad_len,
                      const uint8_t *cipher, size_t cipher_len, uint8_t *out,
                      size_t out_len, size_t *written)
{
    struct cmosh_aes128 aes;
    uint8_t zero[16], lstar[16], ldollar[16], l0[16], offset[16], checksum[16];
    uint8_t tmp[16], pad[16], tag[16], hash[16], diff = 0;
    size_t plain_len, full, rem, i;

    if (!key || !nonce || nonce_len == 0 || nonce_len > 15 || !cipher ||
        cipher_len < CMOSH_OCB_TAG_LEN || !out || !written ||
        (ad_len && !ad))
        return -1;

    plain_len = cipher_len - CMOSH_OCB_TAG_LEN;
    if (out_len < plain_len)
        return -1;
    full = plain_len / 16;
    rem = plain_len % 16;

    cmosh_aes128_init(&aes, key);
    memset(zero, 0, 16);
    cmosh_aes128_encrypt_block(&aes, zero, lstar);
    double_block(ldollar, lstar);
    double_block(l0, ldollar);
    ocb_hash(&aes, lstar, ldollar, ad, ad_len, hash);
    initial_offset(&aes, nonce, nonce_len, offset);
    memset(checksum, 0, 16);

    for (i = 1; i <= full; i++) {
        uint8_t li[16];
        get_l(l0, ntz_size(i), li);
        xor16_in(offset, li);
        xor16(tmp, cipher + 16 * (i - 1), offset);
        cmosh_aes128_decrypt_block(&aes, tmp, pad);
        xor16(out + 16 * (i - 1), pad, offset);
        xor16_in(checksum, out + 16 * (i - 1));
    }

    if (rem) {
        xor16_in(offset, lstar);
        cmosh_aes128_encrypt_block(&aes, offset, pad);
        for (i = 0; i < rem; i++)
            out[16 * full + i] = (uint8_t)(cipher[16 * full + i] ^ pad[i]);
        memset(tmp, 0, 16);
        memcpy(tmp, out + 16 * full, rem);
        tmp[rem] = 0x80;
        xor16_in(checksum, tmp);
    }

    xor16(tmp, checksum, offset);
    xor16_in(tmp, ldollar);
    cmosh_aes128_encrypt_block(&aes, tmp, tag);
    xor16_in(tag, hash);
    for (i = 0; i < CMOSH_OCB_TAG_LEN; i++)
        diff |= (uint8_t)(tag[i] ^ cipher[plain_len + i]);
    if (diff)
        return -1;
    *written = plain_len;
    return 0;
}
