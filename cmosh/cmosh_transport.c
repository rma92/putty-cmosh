#include "cmosh_transport.h"

#include "cmosh_ocb.h"

#include <string.h>

void cmosh_transport_init(struct cmosh_transport_state *st)
{
    if (st)
        memset(st, 0, sizeof(*st));
}

int cmosh_transport_note_recv(struct cmosh_transport_state *st, uint64_t seq)
{
    if (!st)
        return -1;
    if (seq <= st->recv_seq)
        return -1;
    st->recv_seq = seq;
    st->latest_ack = seq;
    return 0;
}

int cmosh_transport_crypto_available(void)
{
    return 1;
}

void cmosh_transport_nonce_from_seq(uint64_t seq, unsigned char nonce[8])
{
    int i;
    for (i = 7; i >= 0; i--) {
        nonce[i] = (unsigned char)(seq & 0xff);
        seq >>= 8;
    }
}

static uint64_t seq_from_nonce(const unsigned char nonce[8])
{
    unsigned int i;
    uint64_t seq = 0;
    for (i = 0; i < 8; i++)
        seq = (seq << 8) | nonce[i];
    return seq;
}

static void ocb_nonce_from_packet_nonce(const unsigned char packet_nonce[8],
                                        unsigned char ocb_nonce[12])
{
    memset(ocb_nonce, 0, 4);
    memcpy(ocb_nonce + 4, packet_nonce, 8);
}

int cmosh_transport_encrypt_packet(const unsigned char key[16], uint64_t seq,
                                   const unsigned char *plain,
                                   size_t plain_len, unsigned char *packet,
                                   size_t packet_len, size_t *written)
{
    unsigned char ocb_nonce[12];
    size_t crypt_len;

    if (!key || !packet || !written ||
        packet_len < CMOSH_PACKET_NONCE_LEN + plain_len + CMOSH_PACKET_TAG_LEN)
        return -1;

    cmosh_transport_nonce_from_seq(seq, packet);
    ocb_nonce_from_packet_nonce(packet, ocb_nonce);
    if (cmosh_ocb_encrypt(key, ocb_nonce, sizeof(ocb_nonce), 0, 0, plain,
                          plain_len, packet + CMOSH_PACKET_NONCE_LEN,
                          packet_len - CMOSH_PACKET_NONCE_LEN, &crypt_len) !=
        0)
        return -1;

    *written = CMOSH_PACKET_NONCE_LEN + crypt_len;
    return 0;
}

int cmosh_transport_decrypt_packet(const unsigned char key[16],
                                   const unsigned char *packet,
                                   size_t packet_len, unsigned char *plain,
                                   size_t plain_len, size_t *written,
                                   uint64_t *seq)
{
    unsigned char ocb_nonce[12];

    if (!key || !packet || packet_len < CMOSH_PACKET_NONCE_LEN +
                                      CMOSH_PACKET_TAG_LEN ||
        !plain || !written || !seq)
        return -1;

    ocb_nonce_from_packet_nonce(packet, ocb_nonce);
    if (cmosh_ocb_decrypt(key, ocb_nonce, sizeof(ocb_nonce), 0, 0,
                          packet + CMOSH_PACKET_NONCE_LEN,
                          packet_len - CMOSH_PACKET_NONCE_LEN, plain,
                          plain_len, written) != 0)
        return -1;
    *seq = seq_from_nonce(packet);
    return 0;
}
