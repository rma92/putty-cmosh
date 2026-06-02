#include "cmosh_transport.h"

#include "cmosh_fragment.h"
#include "cmosh_ocb.h"
#include "cmosh_proto.h"
#include "cmosh_session.h"

#include <stdlib.h>
#include <string.h>

void cmosh_transport_init(struct cmosh_transport_state *st)
{
    if (st)
        memset(st, 0, sizeof(*st));
}

void cmosh_transport_clear(struct cmosh_transport_state *st)
{
    size_t i;

    if (!st)
        return;
    for (i = 0; i < CMOSH_TRANSPORT_FRAGMENT_MAX; i++) {
        free(st->fragments[i].payload);
        st->fragments[i].payload = NULL;
        st->fragments[i].len = 0;
        st->fragments[i].present = 0;
    }
    st->fragment_active = 0;
    st->fragment_id = 0;
    st->fragment_arrived = 0;
    st->fragment_total = 0;
    st->fragment_have_final = 0;
}

int cmosh_transport_note_recv(struct cmosh_transport_state *st, uint64_t seq)
{
    size_t i;

    if (!st)
        return -1;
    if (seq < st->recv_seq &&
        st->recv_seq - seq >= CMOSH_TRANSPORT_REPLAY_HISTORY)
        return -1;
    for (i = 0; i < st->recv_history_count; i++) {
        if (st->recv_history[i] == seq)
            return -1;
    }

    st->recv_history[st->recv_history_next] = seq;
    st->recv_history_next =
        (st->recv_history_next + 1) % CMOSH_TRANSPORT_REPLAY_HISTORY;
    if (st->recv_history_count < CMOSH_TRANSPORT_REPLAY_HISTORY)
        st->recv_history_count++;

    if (seq > st->recv_seq) {
        st->recv_seq = seq;
        st->latest_ack = seq;
    }
    return 0;
}

static int cmosh_transport_seen_recv(struct cmosh_transport_state *st,
                                     uint64_t seq)
{
    size_t i;

    if (!st)
        return 0;
    if (seq < st->recv_seq &&
        st->recv_seq - seq >= CMOSH_TRANSPORT_REPLAY_HISTORY)
        return 1;
    for (i = 0; i < st->recv_history_count; i++) {
        if (st->recv_history[i] == seq)
            return 1;
    }
    return 0;
}

int cmosh_transport_crypto_available(void)
{
    return 1;
}

int cmosh_transport_make_packet(const unsigned char key[16], uint64_t seq,
                                uint64_t old_num, uint64_t new_num,
                                uint64_t ack_num,
                                const unsigned char *diff, size_t diff_len,
                                unsigned int timestamp,
                                unsigned int echo_timestamp,
                                unsigned char *packet, size_t packet_cap,
                                size_t *packet_len)
{
    unsigned char instruction[512], compressed[640], fragment[704];
    unsigned char plain[CMOSH_MAX_PACKET];
    size_t instruction_len, compressed_len, fragment_len, plain_len;
    struct cmosh_transport_instruction ti;

    if (!key || (!diff && diff_len) || !packet || !packet_len)
        return -1;

    memset(&ti, 0, sizeof(ti));
    ti.protocol_version = CMOSH_PROTOCOL_VERSION;
    ti.old_num = old_num;
    ti.new_num = new_num;
    ti.ack_num = ack_num;
    ti.diff = diff;
    ti.diff_len = diff_len;
    if (cmosh_encode_transport_instruction(&ti, instruction,
                                           sizeof(instruction),
                                           &instruction_len) != 0)
        return -1;
    if (cmosh_zlib_store_compress(instruction, instruction_len, compressed,
                                  sizeof(compressed), &compressed_len) != 0)
        return -1;
    if (cmosh_encode_fragment(seq, 0, 1, compressed, compressed_len, fragment,
                              sizeof(fragment), &fragment_len) != 0)
        return -1;
    if (fragment_len + 4 > sizeof(plain))
        return -1;

    plain[0] = (unsigned char)(timestamp >> 8);
    plain[1] = (unsigned char)timestamp;
    plain[2] = (unsigned char)(echo_timestamp >> 8);
    plain[3] = (unsigned char)echo_timestamp;
    memcpy(plain + 4, fragment, fragment_len);
    plain_len = fragment_len + 4;

    return cmosh_transport_encrypt_packet(key, CMOSH_CLIENT_NONCE_BASE | seq,
                                          plain, plain_len, packet,
                                          packet_cap, packet_len);
}

static int cmosh_transport_decode_instruction(
    const unsigned char *compressed, size_t compressed_len,
    struct cmosh_transport_instruction *ti, unsigned char *diff_buf,
    size_t diff_buf_len)
{
    unsigned char decompressed[CMOSH_SERVER_DIFF_MAX + 1024];
    size_t decompressed_len;

    if (!compressed || !ti || (!diff_buf && diff_buf_len))
        return -1;
    if (cmosh_zlib_store_decompress(compressed, compressed_len,
                                    decompressed, sizeof(decompressed),
                                    &decompressed_len) != 0)
        return -1;
    if (cmosh_decode_transport_instruction(decompressed, decompressed_len,
                                           ti, NULL, NULL) != 0)
        return -1;
    if (ti->diff_len) {
        if (!diff_buf || ti->diff_len > diff_buf_len)
            return -1;
        memcpy(diff_buf, ti->diff, ti->diff_len);
        ti->diff = diff_buf;
    }
    if (ti->chaff_len)
        ti->chaff = NULL;
    return 0;
}

static int cmosh_transport_reassemble_fragment(
    struct cmosh_transport_state *st, const struct cmosh_fragment *frag,
    unsigned char *assembled, size_t assembled_len, size_t *written)
{
    unsigned char *copy;
    size_t i, pos = 0;

    if (!st || !frag || !assembled || !written ||
        frag->index >= CMOSH_TRANSPORT_FRAGMENT_MAX)
        return -1;

    if (st->fragment_active && st->fragment_id != frag->id &&
        frag->id < st->fragment_id)
        return 1;

    if (!st->fragment_active || st->fragment_id != frag->id) {
        cmosh_transport_clear(st);
        st->fragment_active = 1;
        st->fragment_id = frag->id;
    }

    if (st->fragment_have_final && frag->index >= st->fragment_total)
        return -1;

    if (st->fragments[frag->index].present) {
        if (st->fragments[frag->index].len != frag->payload_len ||
            memcmp(st->fragments[frag->index].payload, frag->payload,
                   frag->payload_len) != 0)
            return -1;
    } else {
        copy = NULL;
        if (frag->payload_len) {
            copy = (unsigned char *)malloc(frag->payload_len);
            if (!copy)
                return -1;
            memcpy(copy, frag->payload, frag->payload_len);
        }
        st->fragments[frag->index].payload = copy;
        st->fragments[frag->index].len = frag->payload_len;
        st->fragments[frag->index].present = 1;
        st->fragment_arrived++;
    }

    if (frag->final) {
        unsigned int total = frag->index + 1;

        if (st->fragment_have_final && st->fragment_total != total)
            return -1;
        st->fragment_have_final = 1;
        st->fragment_total = total;
    }

    if (!st->fragment_have_final ||
        st->fragment_arrived < st->fragment_total)
        return 1;

    for (i = 0; i < st->fragment_total; i++) {
        if (!st->fragments[i].present ||
            st->fragments[i].len > assembled_len - pos)
            return -1;
        if (st->fragments[i].len)
            memcpy(assembled + pos, st->fragments[i].payload,
                   st->fragments[i].len);
        pos += st->fragments[i].len;
    }

    *written = pos;
    cmosh_transport_clear(st);
    return 0;
}

int cmosh_transport_decode_packet(
    const unsigned char key[16], const unsigned char *packet,
    size_t packet_len, struct cmosh_transport_instruction *ti,
    unsigned char *diff_buf, size_t diff_buf_len, unsigned int *timestamp,
    unsigned int *echo_timestamp, uint64_t *seq)
{
    unsigned char plain[CMOSH_MAX_PACKET];
    size_t plain_len;
    struct cmosh_fragment frag;

    if (!timestamp || !echo_timestamp || !ti || (!diff_buf && diff_buf_len))
        return -1;
    if (cmosh_transport_decrypt_packet(key, packet, packet_len, plain,
                                       sizeof(plain), &plain_len, seq) != 0)
        return -1;
    if (plain_len < 4)
        return -1;
    *timestamp = ((unsigned)plain[0] << 8) | plain[1];
    *echo_timestamp = ((unsigned)plain[2] << 8) | plain[3];
    if (cmosh_decode_fragment(plain + 4, plain_len - 4, &frag) != 0 ||
        frag.index != 0 || !frag.final)
        return -1;
    return cmosh_transport_decode_instruction(frag.payload, frag.payload_len,
                                              ti, diff_buf, diff_buf_len);
}

int cmosh_transport_decode_packet_state(
    struct cmosh_transport_state *st, const unsigned char key[16],
    const unsigned char *packet, size_t packet_len,
    struct cmosh_transport_instruction *ti, unsigned char *diff_buf,
    size_t diff_buf_len, unsigned int *timestamp,
    unsigned int *echo_timestamp, uint64_t *seq)
{
    unsigned char plain[CMOSH_MAX_PACKET];
    unsigned char assembled[CMOSH_SERVER_DIFF_MAX + 1024];
    size_t plain_len, assembled_len;
    struct cmosh_fragment frag;
    int ret;

    if (!st || !timestamp || !echo_timestamp || !ti ||
        (!diff_buf && diff_buf_len))
        return -1;
    if (cmosh_transport_decrypt_packet(key, packet, packet_len, plain,
                                       sizeof(plain), &plain_len, seq) != 0)
        return -1;
    if ((*seq & CMOSH_SERVER_NONCE_BASE) == 0)
        return -1;
    if (cmosh_transport_seen_recv(st, *seq))
        return 2;
    if (plain_len < 4)
        return -1;
    *timestamp = ((unsigned)plain[0] << 8) | plain[1];
    *echo_timestamp = ((unsigned)plain[2] << 8) | plain[3];
    if (cmosh_decode_fragment(plain + 4, plain_len - 4, &frag) != 0) {
        cmosh_transport_clear(st);
        return -1;
    }

    if (frag.index == 0 && frag.final) {
        cmosh_transport_clear(st);
        ret = cmosh_transport_decode_instruction(frag.payload,
                                                 frag.payload_len, ti,
                                                 diff_buf, diff_buf_len);
        if (ret == 0)
            cmosh_transport_note_recv(st, *seq);
        return ret;
    }

    ret = cmosh_transport_reassemble_fragment(st, &frag, assembled,
                                              sizeof(assembled),
                                              &assembled_len);
    if (ret < 0)
        cmosh_transport_clear(st);
    if (ret != 0) {
        if (ret > 0)
            cmosh_transport_note_recv(st, *seq);
        return ret;
    }
    ret = cmosh_transport_decode_instruction(assembled, assembled_len, ti,
                                             diff_buf, diff_buf_len);
    if (ret == 0)
        cmosh_transport_note_recv(st, *seq);
    return ret;
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
