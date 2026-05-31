#ifndef CMOSH_SESSION_H
#define CMOSH_SESSION_H

#include <stddef.h>
#include <stdint.h>

#define CMOSH_INPUT_RETRY_FIRST_MS 5000U
#define CMOSH_INPUT_RETRY_LATER_MS 10000U
#define CMOSH_INPUT_MAX_RECORDS 128
#define CMOSH_INPUT_MAX_BYTES 4096
#define CMOSH_SERVER_QUEUE 16
#define CMOSH_SERVER_DIFF_MAX 8192

struct cmosh_input_record {
    uint64_t state;
    size_t off;
    size_t len;
    uint64_t last_sent_ms;
    unsigned int send_count;
    int encoded_diff;
};

struct cmosh_input_state {
    uint64_t acked;
    uint64_t current;
    unsigned char bytes[CMOSH_INPUT_MAX_BYTES];
    size_t bytes_len;
    struct cmosh_input_record records[CMOSH_INPUT_MAX_RECORDS];
    size_t nrecords;
};

struct cmosh_server_diff {
    int used;
    uint64_t old_num;
    uint64_t new_num;
    size_t len;
    unsigned char diff[CMOSH_SERVER_DIFF_MAX];
};

struct cmosh_server_queue {
    struct cmosh_server_diff entries[CMOSH_SERVER_QUEUE];
};

void cmosh_input_init(struct cmosh_input_state *st, uint64_t initial_state);
int cmosh_input_append(struct cmosh_input_state *st,
                       const unsigned char *keys, size_t keys_len,
                       uint64_t now_ms);
int cmosh_input_append_diff(struct cmosh_input_state *st,
                            const unsigned char *diff, size_t diff_len,
                            uint64_t now_ms);
void cmosh_input_note_ack(struct cmosh_input_state *st, uint64_t acked);
struct cmosh_input_record *cmosh_input_retransmit_record(
    struct cmosh_input_state *st, uint64_t now_ms);
int cmosh_input_record_diff(struct cmosh_input_state *st,
                            struct cmosh_input_record *rec,
                            unsigned char *diffbuf, size_t diffbuf_len,
                            size_t *diff_len);

void cmosh_server_queue_init(struct cmosh_server_queue *queue);
int cmosh_server_queue_add(struct cmosh_server_queue *queue, uint64_t old_num,
                           uint64_t new_num, const unsigned char *diff,
                           size_t diff_len);
struct cmosh_server_diff *cmosh_server_queue_pop_next(
    struct cmosh_server_queue *queue, uint64_t server_state);
int cmosh_server_queue_waiting_for_gap(const struct cmosh_server_queue *queue,
                                       uint64_t server_state,
                                       uint64_t *old_num, uint64_t *new_num);

#endif
