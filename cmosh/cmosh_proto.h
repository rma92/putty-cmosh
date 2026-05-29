#ifndef CMOSH_PROTO_H
#define CMOSH_PROTO_H

#include <stddef.h>
#include <stdint.h>

struct cmosh_user_input {
    uint64_t frame_id;
    const unsigned char *keys;
    size_t keys_len;
    unsigned int rows;
    unsigned int cols;
};

struct cmosh_transport_instruction {
    unsigned int protocol_version;
    uint64_t old_num;
    uint64_t new_num;
    uint64_t ack_num;
    uint64_t throwaway_num;
    const unsigned char *diff;
    size_t diff_len;
    const unsigned char *chaff;
    size_t chaff_len;
};

size_t cmosh_pb_varint_size(uint64_t value);
int cmosh_pb_put_varint(unsigned char *buf, size_t buflen, size_t *pos,
                        uint64_t value);
int cmosh_pb_get_varint(const unsigned char *buf, size_t buflen, size_t *pos,
                        uint64_t *value);
int cmosh_encode_transport_instruction(
    const struct cmosh_transport_instruction *msg, unsigned char *buf,
    size_t buflen, size_t *written);
int cmosh_decode_transport_instruction(const unsigned char *buf, size_t buflen,
                                       struct cmosh_transport_instruction *out,
                                       const unsigned char **diff,
                                       const unsigned char **chaff);
int cmosh_encode_user_input(const struct cmosh_user_input *msg,
                            unsigned char *buf, size_t buflen,
                            size_t *written);
int cmosh_encode_user_resize_message(unsigned int cols, unsigned int rows,
                                     unsigned char *buf, size_t buflen,
                                     size_t *written);
int cmosh_encode_user_keystroke_message(const unsigned char *keys,
                                        size_t keys_len, unsigned char *buf,
                                        size_t buflen, size_t *written);
int cmosh_decode_host_output(const unsigned char *buf, size_t buflen,
                             unsigned char *out, size_t outlen,
                             size_t *written);

#endif
