#ifndef CMOSH_CLIENT_H
#define CMOSH_CLIENT_H

#include "cmosh_proto.h"
#include "cmosh_session.h"
#include "cmosh_transport.h"

#include <stddef.h>
#include <stdint.h>

#define CMOSH_CLIENT_MISSING_STATE_DIAG_MS 5000U
#define CMOSH_CLIENT_UDP_TIMEOUT_DIAG_MS 30000U
#define CMOSH_CLIENT_SERVER_SHUTDOWN_STATE UINT64_MAX

struct cmosh_client {
    unsigned char key[16];
    uint64_t send_seq;
    uint64_t server_state;
    uint64_t last_recv_ms;
    uint64_t last_missing_diag_ms;
    uint64_t last_timeout_diag_ms;
    unsigned int echo_timestamp;
    struct cmosh_input_state input;
    struct cmosh_transport_state recv_transport;
    struct cmosh_server_queue server_queue;
};

enum cmosh_client_recv_result {
    CMOSH_CLIENT_RECV_OK,
    CMOSH_CLIENT_RECV_DUPLICATE,
    CMOSH_CLIENT_RECV_BAD_PACKET,
};

typedef int (*cmosh_client_host_output_fn)(void *ctx,
                                           const unsigned char *diff,
                                           size_t diff_len);

struct cmosh_client_recv_event {
    enum cmosh_client_recv_result result;
    uint64_t seq;
    uint64_t old_num;
    uint64_t new_num;
    uint64_t ack_num;
    uint64_t previous_server_state;
    size_t diff_len;
    int queued_future;
    int server_shutdown;
    int should_ack;
};

struct cmosh_client_idle_event {
    int missing_state;
    int udp_timeout;
    int retransmitted;
    uint64_t gap_old_num;
    uint64_t gap_new_num;
};

void cmosh_client_init(struct cmosh_client *client,
                       const unsigned char key[16],
                       uint64_t initial_client_ack,
                       uint64_t initial_server_state,
                       uint64_t initial_server_seq,
                       unsigned int initial_echo_timestamp,
                       uint64_t next_send_seq);
int cmosh_client_make_initial_packet(const unsigned char key[16],
                                     unsigned int cols, unsigned int rows,
                                     unsigned int now16,
                                     unsigned char *packet,
                                     size_t packet_cap, size_t *packet_len,
                                     size_t *diff_len);
int cmosh_client_make_start_ack(const unsigned char key[16],
                                unsigned int remote_timestamp,
                                unsigned int now16, unsigned char *packet,
                                size_t packet_cap, size_t *packet_len);
int cmosh_client_make_ack(struct cmosh_client *client, unsigned int now16,
                          unsigned char *packet, size_t packet_cap,
                          size_t *packet_len);
int cmosh_client_make_resize(struct cmosh_client *client, unsigned int cols,
                             unsigned int rows, uint64_t now_ms,
                             unsigned int now16, unsigned char *packet,
                             size_t packet_cap, size_t *packet_len);
int cmosh_client_make_input(struct cmosh_client *client,
                            const unsigned char *keys, size_t keys_len,
                            uint64_t now_ms, unsigned int now16,
                            unsigned char *packet, size_t packet_cap,
                            size_t *packet_len);
int cmosh_client_make_idle(struct cmosh_client *client, uint64_t now_ms,
                           unsigned int now16, unsigned char *packet,
                           size_t packet_cap, size_t *packet_len,
                           int *retransmitted);
int cmosh_client_make_idle_event(struct cmosh_client *client, uint64_t now_ms,
                                 unsigned int now16, unsigned char *packet,
                                 size_t packet_cap, size_t *packet_len,
                                 struct cmosh_client_idle_event *event);
enum cmosh_client_recv_result cmosh_client_recv_packet(
    struct cmosh_client *client, const unsigned char *packet,
    size_t packet_len, struct cmosh_transport_instruction *ti,
    unsigned char *diff_buf, size_t diff_buf_len, unsigned int *timestamp,
    uint64_t *seq);
enum cmosh_client_recv_result cmosh_client_process_packet(
    struct cmosh_client *client, const unsigned char *packet,
    size_t packet_len, unsigned char *diff_buf, size_t diff_buf_len,
    cmosh_client_host_output_fn output, void *ctx,
    struct cmosh_client_recv_event *event);
int cmosh_client_queue_server_diff(struct cmosh_client *client,
                                   const struct cmosh_transport_instruction *ti);
int cmosh_client_note_server_instruction(
    struct cmosh_client *client, const struct cmosh_transport_instruction *ti,
    int *queued_future, uint64_t *previous_server_state);
int cmosh_client_apply_server_diffs(struct cmosh_client *client,
                                    cmosh_client_host_output_fn output,
                                    void *ctx);
int cmosh_client_should_ack(const struct cmosh_client *client,
                            const struct cmosh_transport_instruction *ti,
                            uint64_t previous_server_state);
int cmosh_client_waiting_for_gap(const struct cmosh_client *client,
                                 uint64_t *old_num, uint64_t *new_num);
void cmosh_client_note_recv_time(struct cmosh_client *client,
                                 uint64_t now_ms);
int cmosh_client_missing_state_diag_due(struct cmosh_client *client,
                                        uint64_t now_ms, uint64_t *old_num,
                                        uint64_t *new_num);
int cmosh_client_udp_timeout_due(struct cmosh_client *client,
                                 uint64_t now_ms);

#endif
