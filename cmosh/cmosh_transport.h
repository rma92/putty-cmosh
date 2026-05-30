#ifndef CMOSH_TRANSPORT_H
#define CMOSH_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define CMOSH_PACKET_NONCE_LEN 8
#define CMOSH_PACKET_TAG_LEN 16
#define CMOSH_MAX_PACKET 1500
#define CMOSH_CLIENT_NONCE_BASE UINT64_C(0x0000000000000000)
#define CMOSH_SERVER_NONCE_BASE UINT64_C(0x8000000000000000)
#define CMOSH_PROTOCOL_VERSION 2
#define CMOSH_TRANSPORT_REPLAY_HISTORY 64

struct cmosh_transport_instruction;

struct cmosh_transport_state {
    uint64_t send_seq;
    uint64_t recv_seq;
    uint64_t latest_ack;
    uint64_t recv_history[CMOSH_TRANSPORT_REPLAY_HISTORY];
    size_t recv_history_next;
    size_t recv_history_count;
};

void cmosh_transport_init(struct cmosh_transport_state *st);
int cmosh_transport_note_recv(struct cmosh_transport_state *st, uint64_t seq);
int cmosh_transport_crypto_available(void);
void cmosh_transport_nonce_from_seq(uint64_t seq, unsigned char nonce[8]);
int cmosh_transport_make_packet(const unsigned char key[16], uint64_t seq,
                                uint64_t old_num, uint64_t new_num,
                                uint64_t ack_num,
                                const unsigned char *diff, size_t diff_len,
                                unsigned int timestamp,
                                unsigned int echo_timestamp,
                                unsigned char *packet, size_t packet_cap,
                                size_t *packet_len);
int cmosh_transport_decode_packet(
    const unsigned char key[16], const unsigned char *packet,
    size_t packet_len, struct cmosh_transport_instruction *ti,
    unsigned char *diff_buf, size_t diff_buf_len, unsigned int *timestamp,
    unsigned int *echo_timestamp, uint64_t *seq);
int cmosh_transport_encrypt_packet(const unsigned char key[16], uint64_t seq,
                                   const unsigned char *plain,
                                   size_t plain_len, unsigned char *packet,
                                   size_t packet_len, size_t *written);
int cmosh_transport_decrypt_packet(const unsigned char key[16],
                                   const unsigned char *packet,
                                   size_t packet_len, unsigned char *plain,
                                   size_t plain_len, size_t *written,
                                   uint64_t *seq);

#endif
