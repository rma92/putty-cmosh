#include "cmosh_session.h"

#include "cmosh_proto.h"

#include <stdlib.h>
#include <string.h>

void cmosh_input_init(struct cmosh_input_state *st, uint64_t initial_state)
{
    memset(st, 0, sizeof(*st));
    st->acked = initial_state;
    st->current = initial_state;
}

int cmosh_input_append(struct cmosh_input_state *st,
                       const unsigned char *keys, size_t keys_len,
                       uint64_t now_ms)
{
    struct cmosh_input_record *rec;

    if (!keys_len)
        return 0;
    if (!st || !keys || st->nrecords >= CMOSH_INPUT_MAX_RECORDS ||
        keys_len > sizeof(st->bytes) - st->bytes_len)
        return -1;

    st->current++;
    rec = &st->records[st->nrecords++];
    rec->state = st->current;
    rec->off = st->bytes_len;
    rec->len = keys_len;
    rec->last_sent_ms = now_ms;
    rec->send_count = 1;
    rec->encoded_diff = 0;
    memcpy(st->bytes + st->bytes_len, keys, keys_len);
    st->bytes_len += keys_len;
    return 0;
}

int cmosh_input_append_diff(struct cmosh_input_state *st,
                            const unsigned char *diff, size_t diff_len,
                            uint64_t now_ms)
{
    struct cmosh_input_record *rec;

    if (!diff_len)
        return 0;
    if (!st || !diff || st->nrecords >= CMOSH_INPUT_MAX_RECORDS ||
        diff_len > sizeof(st->bytes) - st->bytes_len)
        return -1;

    st->current++;
    rec = &st->records[st->nrecords++];
    rec->state = st->current;
    rec->off = st->bytes_len;
    rec->len = diff_len;
    rec->last_sent_ms = now_ms;
    rec->send_count = 1;
    rec->encoded_diff = 1;
    memcpy(st->bytes + st->bytes_len, diff, diff_len);
    st->bytes_len += diff_len;
    return 0;
}

void cmosh_input_note_ack(struct cmosh_input_state *st, uint64_t acked)
{
    size_t cut = 0, cut_bytes, i;

    if (!st)
        return;
    if (acked > st->current)
        acked = st->current;
    if (acked <= st->acked)
        return;
    st->acked = acked;
    while (cut < st->nrecords && st->records[cut].state <= acked)
        cut++;
    if (!cut)
        return;

    cut_bytes = st->records[cut - 1].off + st->records[cut - 1].len;
    if (cut_bytes < st->bytes_len)
        memmove(st->bytes, st->bytes + cut_bytes, st->bytes_len - cut_bytes);
    st->bytes_len -= cut_bytes;

    for (i = cut; i < st->nrecords; i++) {
        st->records[i - cut] = st->records[i];
        st->records[i - cut].off -= cut_bytes;
    }
    st->nrecords -= cut;
}

struct cmosh_input_record *cmosh_input_retransmit_record(
    struct cmosh_input_state *st, uint64_t now_ms)
{
    size_t i;

    if (!st)
        return NULL;
    for (i = 0; i < st->nrecords; i++) {
        struct cmosh_input_record *rec = &st->records[i];
        uint64_t age = now_ms >= rec->last_sent_ms ?
            now_ms - rec->last_sent_ms : 0;
        uint64_t retry_ms = rec->send_count < 2 ?
            CMOSH_INPUT_RETRY_FIRST_MS : CMOSH_INPUT_RETRY_LATER_MS;

        if (rec->state <= st->acked)
            continue;
        if (rec->send_count == 0)
            return rec;
        if (age >= retry_ms)
            return rec;
    }
    return NULL;
}

int cmosh_input_record_diff(struct cmosh_input_state *st,
                            struct cmosh_input_record *rec,
                            unsigned char *diffbuf, size_t diffbuf_len,
                            size_t *diff_len)
{
    if (!st || !rec || rec->off + rec->len > st->bytes_len)
        return -1;
    if (rec->encoded_diff) {
        if (rec->len > diffbuf_len || !diff_len)
            return -1;
        memcpy(diffbuf, st->bytes + rec->off, rec->len);
        *diff_len = rec->len;
        return 0;
    }
    return cmosh_encode_user_keystroke_message(st->bytes + rec->off, rec->len,
                                               diffbuf, diffbuf_len,
                                               diff_len);
}

void cmosh_server_queue_init(struct cmosh_server_queue *queue)
{
    memset(queue, 0, sizeof(*queue));
}

static void cmosh_server_diff_clear(struct cmosh_server_diff *entry)
{
    if (!entry)
        return;
    free(entry->diff);
    memset(entry, 0, sizeof(*entry));
}

void cmosh_server_queue_clear(struct cmosh_server_queue *queue)
{
    size_t i;

    if (!queue)
        return;
    for (i = 0; i < CMOSH_SERVER_QUEUE; i++)
        cmosh_server_diff_clear(&queue->entries[i]);
}

int cmosh_server_queue_add(struct cmosh_server_queue *queue, uint64_t old_num,
                           uint64_t new_num, const unsigned char *diff,
                           size_t diff_len)
{
    size_t i, slot = CMOSH_SERVER_QUEUE, oldest = CMOSH_SERVER_QUEUE;
    uint64_t oldest_new = UINT64_MAX;

    if (!queue || (!diff && diff_len) || diff_len > CMOSH_SERVER_DIFF_MAX ||
        new_num <= old_num)
        return -1;
    for (i = 0; i < CMOSH_SERVER_QUEUE; i++) {
        if (queue->entries[i].used &&
            queue->entries[i].old_num == old_num &&
            queue->entries[i].new_num == new_num) {
            if (queue->entries[i].len == diff_len &&
                (!diff_len ||
                 memcmp(queue->entries[i].diff, diff, diff_len) == 0))
                return 0;
            return -1;
        }
        if (!queue->entries[i].used && slot == CMOSH_SERVER_QUEUE)
            slot = i;
        if (queue->entries[i].used && queue->entries[i].new_num < oldest_new) {
            oldest_new = queue->entries[i].new_num;
            oldest = i;
        }
    }
    if (slot == CMOSH_SERVER_QUEUE)
        slot = oldest;
    if (slot == CMOSH_SERVER_QUEUE)
        return -1;

    cmosh_server_diff_clear(&queue->entries[slot]);
    queue->entries[slot].used = 1;
    queue->entries[slot].old_num = old_num;
    queue->entries[slot].new_num = new_num;
    queue->entries[slot].len = diff_len;
    if (diff_len) {
        queue->entries[slot].diff = malloc(diff_len);
        if (!queue->entries[slot].diff) {
            cmosh_server_diff_clear(&queue->entries[slot]);
            return -1;
        }
        memcpy(queue->entries[slot].diff, diff, diff_len);
    }
    return 0;
}

struct cmosh_server_diff *cmosh_server_queue_pop_next(
    struct cmosh_server_queue *queue, uint64_t server_state)
{
    size_t i, best = CMOSH_SERVER_QUEUE;
    uint64_t best_new = UINT64_MAX;

    if (!queue)
        return NULL;
    for (i = 0; i < CMOSH_SERVER_QUEUE; i++) {
        struct cmosh_server_diff *entry = &queue->entries[i];

        if (!entry->used || entry->new_num <= server_state ||
            entry->old_num != server_state)
            continue;
        if (entry->new_num < best_new) {
            best = i;
            best_new = entry->new_num;
        }
    }
    return best == CMOSH_SERVER_QUEUE ? NULL : &queue->entries[best];
}

int cmosh_server_queue_waiting_for_gap(const struct cmosh_server_queue *queue,
                                       uint64_t server_state,
                                       uint64_t *old_num, uint64_t *new_num)
{
    size_t i;
    uint64_t best_old = UINT64_MAX, best_new = 0;

    if (!queue)
        return 0;
    for (i = 0; i < CMOSH_SERVER_QUEUE; i++) {
        const struct cmosh_server_diff *entry = &queue->entries[i];

        if (!entry->used || entry->new_num <= server_state ||
            entry->old_num <= server_state)
            continue;
        if (entry->old_num < best_old) {
            best_old = entry->old_num;
            best_new = entry->new_num;
        }
    }
    if (best_old == UINT64_MAX)
        return 0;
    if (old_num)
        *old_num = best_old;
    if (new_num)
        *new_num = best_new;
    return 1;
}
