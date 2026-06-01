#include "cmosh_proto.h"

#include <string.h>

size_t cmosh_pb_varint_size(uint64_t value)
{
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        n++;
    }
    return n;
}

int cmosh_pb_put_varint(unsigned char *buf, size_t buflen, size_t *pos,
                        uint64_t value)
{
    if (!buf || !pos)
        return -1;
    do {
        unsigned char b = (unsigned char)(value & 0x7f);
        value >>= 7;
        if (value)
            b |= 0x80;
        if (*pos >= buflen)
            return -1;
        buf[(*pos)++] = b;
    } while (value);
    return 0;
}

int cmosh_pb_get_varint(const unsigned char *buf, size_t buflen, size_t *pos,
                        uint64_t *value)
{
    unsigned int shift = 0;
    uint64_t result = 0;

    if (!buf || !pos || !value)
        return -1;
    while (*pos < buflen && shift < 64) {
        unsigned char b = buf[(*pos)++];
        result |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *value = result;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

static int put_key(unsigned char *buf, size_t buflen, size_t *pos,
                   unsigned int field, unsigned int wire_type)
{
    return cmosh_pb_put_varint(buf, buflen, pos,
                               ((uint64_t)field << 3) | wire_type);
}

static int put_bytes(unsigned char *buf, size_t buflen, size_t *pos,
                     unsigned int field, const unsigned char *data,
                     size_t data_len)
{
    if (put_key(buf, buflen, pos, field, 2) ||
        cmosh_pb_put_varint(buf, buflen, pos, data_len))
        return -1;
    if (*pos + data_len > buflen)
        return -1;
    if (data_len)
        memcpy(buf + *pos, data, data_len);
    *pos += data_len;
    return 0;
}

int cmosh_encode_transport_instruction(
    const struct cmosh_transport_instruction *msg, unsigned char *buf,
    size_t buflen, size_t *written)
{
    size_t pos = 0;

    if (!msg || !buf || !written)
        return -1;

    if (put_key(buf, buflen, &pos, 1, 0) ||
        cmosh_pb_put_varint(buf, buflen, &pos, msg->protocol_version))
        return -1;
    if (put_key(buf, buflen, &pos, 2, 0) ||
        cmosh_pb_put_varint(buf, buflen, &pos, msg->old_num))
        return -1;
    if (put_key(buf, buflen, &pos, 3, 0) ||
        cmosh_pb_put_varint(buf, buflen, &pos, msg->new_num))
        return -1;
    if (put_key(buf, buflen, &pos, 4, 0) ||
        cmosh_pb_put_varint(buf, buflen, &pos, msg->ack_num))
        return -1;
    if (put_key(buf, buflen, &pos, 5, 0) ||
        cmosh_pb_put_varint(buf, buflen, &pos, msg->throwaway_num))
        return -1;
    if (msg->diff || msg->diff_len) {
        if (!msg->diff)
            return -1;
        if (put_bytes(buf, buflen, &pos, 6, msg->diff, msg->diff_len))
            return -1;
    }
    if (msg->chaff || msg->chaff_len) {
        if (!msg->chaff)
            return -1;
        if (put_bytes(buf, buflen, &pos, 7, msg->chaff, msg->chaff_len))
            return -1;
    }

    *written = pos;
    return 0;
}

int cmosh_decode_transport_instruction(const unsigned char *buf, size_t buflen,
                                       struct cmosh_transport_instruction *out,
                                       const unsigned char **diff,
                                       const unsigned char **chaff)
{
    size_t pos = 0;

    if (!buf || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (diff)
        *diff = NULL;
    if (chaff)
        *chaff = NULL;

    while (pos < buflen) {
        uint64_t key, value, len;
        unsigned int field, wire_type;

        if (cmosh_pb_get_varint(buf, buflen, &pos, &key) != 0)
            return -1;
        field = (unsigned int)(key >> 3);
        wire_type = (unsigned int)(key & 7);

        if (wire_type == 0) {
            if (cmosh_pb_get_varint(buf, buflen, &pos, &value) != 0)
                return -1;
            switch (field) {
              case 1: out->protocol_version = (unsigned int)value; break;
              case 2: out->old_num = value; break;
              case 3: out->new_num = value; break;
              case 4: out->ack_num = value; break;
              case 5: out->throwaway_num = value; break;
              default: break;
            }
        } else if (wire_type == 2) {
            if (cmosh_pb_get_varint(buf, buflen, &pos, &len) != 0 ||
                len > buflen - pos)
                return -1;
            if (field == 6) {
                out->diff_len = (size_t)len;
                out->diff = buf + pos;
                if (diff)
                    *diff = buf + pos;
            } else if (field == 7) {
                out->chaff_len = (size_t)len;
                out->chaff = buf + pos;
                if (chaff)
                    *chaff = buf + pos;
            }
            pos += (size_t)len;
        } else {
            return -1;
        }
    }

    return 0;
}

int cmosh_encode_user_input(const struct cmosh_user_input *msg,
                            unsigned char *buf, size_t buflen,
                            size_t *written)
{
    size_t pos = 0;

    if (!msg || !buf || !written)
        return -1;

    if (put_key(buf, buflen, &pos, 1, 0) ||
        cmosh_pb_put_varint(buf, buflen, &pos, msg->frame_id))
        return -1;

    if (msg->keys && msg->keys_len) {
        if (put_key(buf, buflen, &pos, 2, 2) ||
            cmosh_pb_put_varint(buf, buflen, &pos, msg->keys_len))
            return -1;
        if (pos + msg->keys_len > buflen)
            return -1;
        memcpy(buf + pos, msg->keys, msg->keys_len);
        pos += msg->keys_len;
    }

    if (msg->rows) {
        if (put_key(buf, buflen, &pos, 3, 0) ||
            cmosh_pb_put_varint(buf, buflen, &pos, msg->rows))
            return -1;
    }
    if (msg->cols) {
        if (put_key(buf, buflen, &pos, 4, 0) ||
            cmosh_pb_put_varint(buf, buflen, &pos, msg->cols))
            return -1;
    }

    *written = pos;
    return 0;
}

int cmosh_encode_user_resize_message(unsigned int cols, unsigned int rows,
                                     unsigned char *buf, size_t buflen,
                                     size_t *written)
{
    unsigned char resize[16], instruction[32];
    size_t rpos = 0, ipos = 0, pos = 0;

    if (!buf || !written || cols == 0 || rows == 0)
        return -1;

    if (put_key(resize, sizeof(resize), &rpos, 5, 0) ||
        cmosh_pb_put_varint(resize, sizeof(resize), &rpos, cols) ||
        put_key(resize, sizeof(resize), &rpos, 6, 0) ||
        cmosh_pb_put_varint(resize, sizeof(resize), &rpos, rows))
        return -1;

    if (put_bytes(instruction, sizeof(instruction), &ipos, 3, resize, rpos))
        return -1;

    if (put_bytes(buf, buflen, &pos, 1, instruction, ipos))
        return -1;

    *written = pos;
    return 0;
}

int cmosh_encode_user_keystroke_message(const unsigned char *keys,
                                        size_t keys_len, unsigned char *buf,
                                        size_t buflen, size_t *written)
{
    unsigned char keymsg[512], instruction[544];
    size_t kpos = 0, ipos = 0, pos = 0;

    if (!keys || keys_len == 0 || !buf || !written)
        return -1;

    if (put_bytes(keymsg, sizeof(keymsg), &kpos, 4, keys, keys_len))
        return -1;
    if (put_bytes(instruction, sizeof(instruction), &ipos, 2, keymsg, kpos))
        return -1;
    if (put_bytes(buf, buflen, &pos, 1, instruction, ipos))
        return -1;

    *written = pos;
    return 0;
}

struct cmosh_host_output_copy_ctx {
    unsigned char *out;
    size_t outlen;
    size_t written;
};

static int cmosh_host_output_copy(void *vctx, const unsigned char *data,
                                  size_t len)
{
    struct cmosh_host_output_copy_ctx *ctx =
        (struct cmosh_host_output_copy_ctx *)vctx;

    if (ctx->written + len > ctx->outlen)
        return -1;
    if (len)
        memcpy(ctx->out + ctx->written, data, len);
    ctx->written += len;
    return 0;
}

static int decode_host_bytes(const unsigned char *buf, size_t buflen,
                             cmosh_host_output_fn output, void *ctx)
{
    size_t pos = 0;

    while (pos < buflen) {
        uint64_t key, len;
        unsigned int field, wire_type;

        if (cmosh_pb_get_varint(buf, buflen, &pos, &key) != 0)
            return -1;
        field = (unsigned int)(key >> 3);
        wire_type = (unsigned int)(key & 7);
        if (wire_type == 2) {
            if (cmosh_pb_get_varint(buf, buflen, &pos, &len) != 0 ||
                len > buflen - pos)
                return -1;
            if (field == 4) {
                if (output(ctx, buf + pos, (size_t)len) != 0)
                    return -1;
            }
            pos += (size_t)len;
        } else if (wire_type == 0) {
            uint64_t ignored;
            if (cmosh_pb_get_varint(buf, buflen, &pos, &ignored) != 0)
                return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static int decode_host_instruction(const unsigned char *buf, size_t buflen,
                                   cmosh_host_output_fn output, void *ctx)
{
    size_t pos = 0;

    while (pos < buflen) {
        uint64_t key, len;
        unsigned int field, wire_type;

        if (cmosh_pb_get_varint(buf, buflen, &pos, &key) != 0)
            return -1;
        field = (unsigned int)(key >> 3);
        wire_type = (unsigned int)(key & 7);
        if (wire_type == 2) {
            if (cmosh_pb_get_varint(buf, buflen, &pos, &len) != 0 ||
                len > buflen - pos)
                return -1;
            if (field == 2 &&
                decode_host_bytes(buf + pos, (size_t)len, output, ctx) != 0)
                return -1;
            pos += (size_t)len;
        } else if (wire_type == 0) {
            uint64_t ignored;
            if (cmosh_pb_get_varint(buf, buflen, &pos, &ignored) != 0)
                return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

int cmosh_decode_host_output_cb(const unsigned char *buf, size_t buflen,
                                cmosh_host_output_fn output, void *ctx)
{
    size_t pos = 0;

    if (!buf || !output)
        return -1;

    while (pos < buflen) {
        uint64_t key, len;
        unsigned int field, wire_type;

        if (cmosh_pb_get_varint(buf, buflen, &pos, &key) != 0)
            return -1;
        field = (unsigned int)(key >> 3);
        wire_type = (unsigned int)(key & 7);
        if (wire_type == 2) {
            if (cmosh_pb_get_varint(buf, buflen, &pos, &len) != 0 ||
                len > buflen - pos)
                return -1;
            if (field == 1 &&
                decode_host_instruction(buf + pos, (size_t)len, output,
                                        ctx) != 0)
                return -1;
            pos += (size_t)len;
        } else if (wire_type == 0) {
            uint64_t ignored;
            if (cmosh_pb_get_varint(buf, buflen, &pos, &ignored) != 0)
                return -1;
        } else {
            return 0;
        }
    }
    return 0;
}

int cmosh_decode_host_output(const unsigned char *buf, size_t buflen,
                             unsigned char *out, size_t outlen,
                             size_t *written)
{
    struct cmosh_host_output_copy_ctx ctx;
    int ret;

    if (!out || !written)
        return -1;
    ctx.out = out;
    ctx.outlen = outlen;
    ctx.written = 0;
    ret = cmosh_decode_host_output_cb(buf, buflen, cmosh_host_output_copy,
                                      &ctx);
    *written = ctx.written;
    return ret;
}
