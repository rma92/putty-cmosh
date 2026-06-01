#include "cmosh_client.h"

#include <stdlib.h>
#include <string.h>

void cmosh_client_init(struct cmosh_client *client,
                       const unsigned char key[16],
                       uint64_t initial_client_ack,
                       uint64_t initial_server_state,
                       uint64_t initial_server_seq,
                       unsigned int initial_echo_timestamp,
                       uint64_t next_send_seq)
{
    memset(client, 0, sizeof(*client));
    memcpy(client->key, key, sizeof(client->key));
    client->send_seq = next_send_seq;
    client->server_state = initial_server_state;
    client->echo_timestamp = initial_echo_timestamp;
    cmosh_input_init(&client->input, initial_client_ack);
    cmosh_transport_init(&client->recv_transport);
    cmosh_transport_note_recv(&client->recv_transport, initial_server_seq);
    cmosh_server_queue_init(&client->server_queue);
}

int cmosh_client_make_initial_packet(const unsigned char key[16],
                                     unsigned int cols, unsigned int rows,
                                     unsigned int now16,
                                     unsigned char *packet,
                                     size_t packet_cap, size_t *packet_len,
                                     size_t *diff_len)
{
    unsigned char diff[64];
    size_t local_diff_len;

    if (cmosh_encode_user_resize_message(cols, rows, diff, sizeof(diff),
                                         &local_diff_len) != 0)
        return -1;
    if (diff_len)
        *diff_len = local_diff_len;
    return cmosh_transport_make_packet(key, 1, 0, 1, 0, diff, local_diff_len,
                                       now16, 0, packet, packet_cap,
                                       packet_len);
}

int cmosh_client_make_start_ack(const unsigned char key[16],
                                unsigned int remote_timestamp,
                                unsigned int now16, unsigned char *packet,
                                size_t packet_cap, size_t *packet_len)
{
    return cmosh_transport_make_packet(key, 2, 1, 1, 1, NULL, 0, now16,
                                       remote_timestamp, packet, packet_cap,
                                       packet_len);
}

int cmosh_client_make_ack(struct cmosh_client *client, unsigned int now16,
                          unsigned char *packet, size_t packet_cap,
                          size_t *packet_len)
{
    if (!client)
        return -1;
    return cmosh_transport_make_packet(
        client->key, client->send_seq++, client->input.acked,
        client->input.acked, client->server_state, NULL, 0, now16,
        client->echo_timestamp, packet, packet_cap, packet_len);
}

int cmosh_client_make_resize(struct cmosh_client *client, unsigned int cols,
                             unsigned int rows, uint64_t now_ms,
                             unsigned int now16, unsigned char *packet,
                             size_t packet_cap, size_t *packet_len)
{
    unsigned char diff[64];
    size_t diff_len;
    uint64_t old_client_state;

    if (!client)
        return -1;
    if (cmosh_encode_user_resize_message(cols, rows, diff, sizeof(diff),
                                         &diff_len) != 0)
        return -1;
    if (cmosh_input_append_diff(&client->input, diff, diff_len, now_ms) != 0)
        return -1;
    old_client_state = client->input.current - 1;
    return cmosh_transport_make_packet(
        client->key, client->send_seq++, old_client_state,
        client->input.current, client->server_state, diff, diff_len, now16,
        client->echo_timestamp, packet, packet_cap, packet_len);
}

int cmosh_client_make_input(struct cmosh_client *client,
                            const unsigned char *keys, size_t keys_len,
                            uint64_t now_ms, unsigned int now16,
                            unsigned char *packet, size_t packet_cap,
                            size_t *packet_len)
{
    unsigned char diff[1024];
    size_t diff_len;
    uint64_t old_client_state;

    if (!client || !keys || !keys_len)
        return -1;
    if (cmosh_input_append(&client->input, keys, keys_len, now_ms) != 0 ||
        cmosh_encode_user_keystroke_message(keys, keys_len, diff,
                                            sizeof(diff), &diff_len) != 0)
        return -1;
    old_client_state = client->input.current - 1;
    return cmosh_transport_make_packet(
        client->key, client->send_seq++, old_client_state,
        client->input.current, client->server_state, diff, diff_len, now16,
        client->echo_timestamp, packet, packet_cap, packet_len);
}

int cmosh_client_make_idle(struct cmosh_client *client, uint64_t now_ms,
                           unsigned int now16, unsigned char *packet,
                           size_t packet_cap, size_t *packet_len,
                           int *retransmitted)
{
    unsigned char diff[1024];
    const unsigned char *diff_ptr = NULL;
    size_t diff_len = 0;
    uint64_t old_client_state, new_client_state;
    struct cmosh_input_record *retry;

    if (!client)
        return -1;
    retry = cmosh_input_retransmit_record(&client->input, now_ms);
    if (retry) {
        if (cmosh_input_record_diff(&client->input, retry, diff,
                                    sizeof(diff), &diff_len) != 0)
            return -1;
        old_client_state = retry->state - 1;
        new_client_state = retry->state;
        retry->last_sent_ms = now_ms;
        retry->send_count++;
        diff_ptr = diff;
        if (retransmitted)
            *retransmitted = 1;
    } else {
        old_client_state = client->input.acked;
        new_client_state = client->input.acked;
        if (retransmitted)
            *retransmitted = 0;
    }

    return cmosh_transport_make_packet(
        client->key, client->send_seq++, old_client_state, new_client_state,
        client->server_state, diff_ptr, diff_len, now16,
        client->echo_timestamp, packet, packet_cap, packet_len);
}

int cmosh_client_make_idle_event(struct cmosh_client *client, uint64_t now_ms,
                                 unsigned int now16, unsigned char *packet,
                                 size_t packet_cap, size_t *packet_len,
                                 struct cmosh_client_idle_event *event)
{
    int retransmitted = 0;

    if (event)
        memset(event, 0, sizeof(*event));
    if (!client)
        return -1;

    if (event) {
        event->missing_state = cmosh_client_missing_state_diag_due(
            client, now_ms, &event->gap_old_num, &event->gap_new_num);
        event->udp_timeout = cmosh_client_udp_timeout_due(client, now_ms);
    } else {
        (void)cmosh_client_missing_state_diag_due(client, now_ms, NULL, NULL);
        (void)cmosh_client_udp_timeout_due(client, now_ms);
    }

    if (cmosh_client_make_idle(client, now_ms, now16, packet, packet_cap,
                               packet_len, &retransmitted) != 0)
        return -1;
    if (event)
        event->retransmitted = retransmitted;
    return 0;
}

enum cmosh_client_recv_result cmosh_client_recv_packet(
    struct cmosh_client *client, const unsigned char *packet,
    size_t packet_len, struct cmosh_transport_instruction *ti,
    unsigned char *diff_buf, size_t diff_buf_len, unsigned int *timestamp,
    uint64_t *seq)
{
    unsigned int echo_timestamp;
    int decode_result;

    if (!client)
        return CMOSH_CLIENT_RECV_BAD_PACKET;
    decode_result = cmosh_transport_decode_packet_state(
        &client->recv_transport, client->key, packet, packet_len, ti,
        diff_buf, diff_buf_len, timestamp, &echo_timestamp, seq);
    if (decode_result == 1)
        return CMOSH_CLIENT_RECV_PENDING;
    if (decode_result == 2)
        return CMOSH_CLIENT_RECV_DUPLICATE;
    if (decode_result != 0)
        return CMOSH_CLIENT_RECV_BAD_PACKET;

    client->echo_timestamp = *timestamp;
    cmosh_input_note_ack(&client->input, ti->ack_num);
    cmosh_input_note_ack(&client->input, ti->throwaway_num);
    return CMOSH_CLIENT_RECV_OK;
}

enum cmosh_client_recv_result cmosh_client_process_packet(
    struct cmosh_client *client, const unsigned char *packet,
    size_t packet_len, unsigned char *diff_buf, size_t diff_buf_len,
    cmosh_client_host_output_fn output, void *ctx,
    struct cmosh_client_recv_event *event)
{
    struct cmosh_transport_instruction ti;
    enum cmosh_client_recv_result result;
    unsigned int timestamp;
    uint64_t seq, previous_server_state = 0;
    int queued_future = 0;

    if (event)
        memset(event, 0, sizeof(*event));
    memset(&ti, 0, sizeof(ti));
    result = cmosh_client_recv_packet(client, packet, packet_len, &ti,
                                      diff_buf, diff_buf_len, &timestamp,
                                      &seq);
    if (event) {
        event->result = result;
        event->seq = seq;
        event->old_num = ti.old_num;
        event->new_num = ti.new_num;
        event->ack_num = ti.ack_num;
        event->diff_len = ti.diff_len;
    }
    if (result != CMOSH_CLIENT_RECV_OK)
        return result;

    if (cmosh_client_note_server_instruction(client, &ti, &queued_future,
                                             &previous_server_state) != 0) {
        if (event)
            event->result = CMOSH_CLIENT_RECV_BAD_PACKET;
        return CMOSH_CLIENT_RECV_BAD_PACKET;
    }
    if (ti.new_num > previous_server_state &&
        cmosh_client_apply_server_diffs(client, output, ctx) != 0) {
        if (event)
            event->result = CMOSH_CLIENT_RECV_BAD_PACKET;
        return CMOSH_CLIENT_RECV_BAD_PACKET;
    }
    if (event) {
        event->queued_future = queued_future;
        event->previous_server_state = previous_server_state;
        event->server_shutdown =
            ti.new_num == CMOSH_CLIENT_SERVER_SHUTDOWN_STATE;
        event->should_ack =
            cmosh_client_should_ack(client, &ti, previous_server_state);
    }
    return CMOSH_CLIENT_RECV_OK;
}

int cmosh_client_queue_server_diff(struct cmosh_client *client,
                                   const struct cmosh_transport_instruction *ti)
{
    if (!client || !ti)
        return -1;
    if (ti->diff_len || ti->old_num > client->server_state) {
        return cmosh_server_queue_add(&client->server_queue, ti->old_num,
                                      ti->new_num, ti->diff, ti->diff_len);
    } else if (ti->old_num <= client->server_state) {
        client->server_state = ti->new_num;
    }
    return 0;
}

int cmosh_client_note_server_instruction(
    struct cmosh_client *client, const struct cmosh_transport_instruction *ti,
    int *queued_future, uint64_t *previous_server_state)
{
    if (!client || !ti)
        return -1;
    if (queued_future)
        *queued_future = 0;
    if (previous_server_state)
        *previous_server_state = client->server_state;
    if (ti->new_num <= client->server_state)
        return 0;
    if (ti->old_num > client->server_state && queued_future)
        *queued_future = 1;
    return cmosh_client_queue_server_diff(client, ti);
}

int cmosh_client_apply_server_diffs(struct cmosh_client *client,
                                    cmosh_client_host_output_fn output,
                                    void *ctx)
{
    struct cmosh_server_diff *entry;

    if (!client || !output)
        return -1;
    while ((entry = cmosh_server_queue_pop_next(&client->server_queue,
                                                client->server_state)) !=
           NULL) {
        if (output(ctx, entry->diff, entry->len) != 0)
            return -1;
        client->server_state = entry->new_num;
        free(entry->diff);
        entry->diff = NULL;
        entry->used = 0;
    }
    return 0;
}

int cmosh_client_should_ack(const struct cmosh_client *client,
                            const struct cmosh_transport_instruction *ti,
                            uint64_t previous_server_state)
{
    if (!client || !ti)
        return 0;
    return client->server_state > previous_server_state ||
           ti->new_num == client->server_state;
}

int cmosh_client_waiting_for_gap(const struct cmosh_client *client,
                                 uint64_t *old_num, uint64_t *new_num)
{
    if (!client)
        return 0;
    return cmosh_server_queue_waiting_for_gap(&client->server_queue,
                                              client->server_state, old_num,
                                              new_num);
}

void cmosh_client_note_recv_time(struct cmosh_client *client,
                                 uint64_t now_ms)
{
    if (client)
        client->last_recv_ms = now_ms;
}

int cmosh_client_missing_state_diag_due(struct cmosh_client *client,
                                        uint64_t now_ms, uint64_t *old_num,
                                        uint64_t *new_num)
{
    if (!client || !cmosh_client_waiting_for_gap(client, old_num, new_num))
        return 0;
    if (now_ms - client->last_missing_diag_ms <
        CMOSH_CLIENT_MISSING_STATE_DIAG_MS)
        return 0;
    client->last_missing_diag_ms = now_ms;
    return 1;
}

int cmosh_client_udp_timeout_due(struct cmosh_client *client,
                                 uint64_t now_ms)
{
    if (!client)
        return 0;
    if (now_ms - client->last_recv_ms < CMOSH_CLIENT_UDP_TIMEOUT_DIAG_MS ||
        now_ms - client->last_timeout_diag_ms <
            CMOSH_CLIENT_UDP_TIMEOUT_DIAG_MS)
        return 0;
    client->last_timeout_diag_ms = now_ms;
    return 1;
}
