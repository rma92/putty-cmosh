#include "cmosh_base64.h"
#include "cmosh_bootstrap.h"
#include "cmosh_aes.h"
#include "cmosh_client.h"
#include "cmosh_fragment.h"
#include "cmosh_ocb.h"
#include "cmosh_proto.h"
#include "cmosh_session.h"
#include "cmosh_transport.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int cond, const char *name)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static int count_host_output(void *vctx, const unsigned char *data, size_t len)
{
    size_t *count = (size_t *)vctx;

    (void)data;
    *count += len;
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static size_t fromhex(const char *hex, unsigned char *out, size_t outlen)
{
    size_t n = 0;
    while (*hex) {
        int hi, lo;
        while (*hex == ' ' || *hex == '\n' || *hex == '\r' || *hex == '\t')
            hex++;
        if (!*hex)
            break;
        hi = hexval(*hex++);
        lo = hexval(*hex++);
        if (hi < 0 || lo < 0 || n >= outlen)
            return (size_t)-1;
        out[n++] = (unsigned char)((hi << 4) | lo);
    }
    return n;
}

static void test_base64(void)
{
    unsigned char out[32];
    char enc[64];
    size_t n;

    check(cmosh_base64_decode("TWFu", out, sizeof(out), &n) == 0 &&
              n == 3 && memcmp(out, "Man", 3) == 0,
          "base64 decode simple");
    check(cmosh_base64_encode((const unsigned char *)"hello", 5, enc,
                              sizeof(enc), &n) == 0 &&
              strcmp(enc, "aGVsbG8=") == 0,
          "base64 encode padded");
    check(cmosh_base64_decode("!!!!", out, sizeof(out), &n) != 0,
          "base64 rejects invalid input");
}

static void test_aes_ocb(void)
{
    struct cmosh_aes128 aes;
    unsigned char key[16], plain[64], cipher[96], expect[96], got[96];
    unsigned char nonce[16], ad[64], out[96];
    size_t n, outlen;

    fromhex("000102030405060708090A0B0C0D0E0F", key, sizeof(key));
    fromhex("00112233445566778899AABBCCDDEEFF", plain, sizeof(plain));
    fromhex("69C4E0D86A7B0430D8CDB78070B4C55A", expect, sizeof(expect));
    cmosh_aes128_init(&aes, key);
    cmosh_aes128_encrypt_block(&aes, plain, cipher);
    check(memcmp(cipher, expect, 16) == 0, "aes encrypt known answer");
    cmosh_aes128_decrypt_block(&aes, cipher, got);
    check(memcmp(got, plain, 16) == 0, "aes decrypt known answer");

    n = fromhex("BBAA99887766554433221100", nonce, sizeof(nonce));
    outlen = 0;
    check(cmosh_ocb_encrypt(key, nonce, n, 0, 0, 0, 0, out, sizeof(out),
                            &outlen) == 0,
          "ocb encrypt empty");
    fromhex("785407BFFFC8AD9EDCC5520AC9111EE6", expect, sizeof(expect));
    check(outlen == 16 && memcmp(out, expect, 16) == 0,
          "ocb empty known answer");
    check(cmosh_ocb_decrypt(key, nonce, n, 0, 0, out, outlen, got,
                            sizeof(got), &outlen) == 0 &&
              outlen == 0,
          "ocb decrypt empty");

    n = fromhex("BBAA99887766554433221101", nonce, sizeof(nonce));
    fromhex("0001020304050607", ad, sizeof(ad));
    fromhex("0001020304050607", plain, sizeof(plain));
    fromhex("6820B3657B6F615A5725BDA0D3B4EB3A257C9AF1F8F03009",
            expect, sizeof(expect));
    check(cmosh_ocb_encrypt(key, nonce, n, ad, 8, plain, 8, out, sizeof(out),
                            &outlen) == 0 &&
              outlen == 24 && memcmp(out, expect, 24) == 0,
          "ocb short known answer");
    check(cmosh_ocb_decrypt(key, nonce, n, ad, 8, out, outlen, got,
                            sizeof(got), &outlen) == 0 &&
              outlen == 8 && memcmp(got, plain, 8) == 0,
          "ocb short decrypt");
}

static void test_bootstrap(void)
{
    struct cmosh_bootstrap boot;
    char command[256];
    char host[256];
    const char *startup =
        "noise\n"
        "MOSH IP 203.0.113.9\n"
        "MOSH CONNECT 60001 AAECAwQFBgcICQoLDA0ODw==\n";

    check(cmosh_parse_startup(startup, &boot) == 0,
          "parse startup succeeds");
    check(boot.port == 60001, "parse startup port");
    check(strcmp(boot.ip, "203.0.113.9") == 0, "parse startup ip");
    check(boot.key[0] == 0 && boot.key[15] == 15, "parse startup key");
    check(cmosh_build_remote_command("mosh-server", "60000:61000",
                                     "en_US.UTF-8", 1, 0, 1, command,
                                     sizeof(command)) == 0 &&
              strcmp(command,
                     "LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 mosh-server new "
                     "-4 --no-init -p 60000:61000 2>&1") == 0,
          "build remote command with locale");
    check(cmosh_build_remote_command("mosh-server", 0, "bad locale", 0, 0, 0,
                                     command, sizeof(command)) != 0,
          "reject unsafe locale");
    check(cmosh_extract_network_host("user@example.com", host, sizeof(host)) ==
              0 &&
              strcmp(host, "example.com") == 0,
          "extract network host from user host");
    check(cmosh_extract_network_host("user@[2001:db8::1]", host,
                                     sizeof(host)) == 0 &&
              strcmp(host, "2001:db8::1") == 0,
          "extract network host from bracketed ipv6 host");
}

static void test_proto(void)
{
    unsigned char buf[64];
    size_t n;
    struct cmosh_user_input msg;
    struct cmosh_transport_instruction ti, ti2;
    const unsigned char diff[] = "d";

    memset(&msg, 0, sizeof(msg));
    msg.frame_id = 150;
    msg.keys = (const unsigned char *)"x";
    msg.keys_len = 1;
    msg.rows = 24;
    msg.cols = 80;
    check(cmosh_encode_user_input(&msg, buf, sizeof(buf), &n) == 0,
          "encode user input");
    check(n == 10 && buf[0] == 0x08 && buf[1] == 0x96 && buf[2] == 0x01 &&
              buf[3] == 0x12 && buf[4] == 0x01 && buf[5] == 'x' &&
              buf[6] == 0x18 && buf[7] == 24 && buf[8] == 0x20 &&
              buf[9] == 80,
          "encode user input bytes");

    memset(&ti, 0, sizeof(ti));
    ti.protocol_version = CMOSH_PROTOCOL_VERSION;
    ti.old_num = 0;
    ti.new_num = 1;
    ti.ack_num = 2;
    ti.throwaway_num = 3;
    ti.diff = diff;
    ti.diff_len = 1;
    check(cmosh_encode_transport_instruction(&ti, buf, sizeof(buf), &n) == 0,
          "encode transport instruction");
    check(n == 13 && buf[0] == 0x08 && buf[1] == 0x02 &&
              buf[2] == 0x10 && buf[3] == 0x00 && buf[4] == 0x18 &&
              buf[5] == 0x01 && buf[6] == 0x20 && buf[7] == 0x02 &&
              buf[8] == 0x28 && buf[9] == 0x03 && buf[10] == 0x32 &&
              buf[11] == 0x01 && buf[12] == 'd',
          "encode transport instruction bytes");
    check(cmosh_decode_transport_instruction(buf, n, &ti2, NULL, NULL) == 0 &&
              ti2.protocol_version == CMOSH_PROTOCOL_VERSION &&
              ti2.new_num == 1 && ti2.ack_num == 2 && ti2.diff_len == 1 &&
              ti2.diff[0] == 'd',
          "decode transport instruction");

    check(cmosh_encode_user_resize_message(80, 24, buf, sizeof(buf), &n) == 0,
          "encode user resize message");
    check(n == 8 && buf[0] == 0x0a && buf[1] == 0x06 && buf[2] == 0x1a &&
              buf[3] == 0x04 && buf[4] == 0x28 && buf[5] == 80 &&
              buf[6] == 0x30 && buf[7] == 24,
          "encode user resize message bytes");

    check(cmosh_decode_host_output(
              (const unsigned char *)"\x0a\x05\x12\x03\x22\x01\x0a", 7,
              buf, sizeof(buf), &n) == 0 &&
              n == 1 && buf[0] == '\n',
          "decode host output");
    check(cmosh_decode_host_output(
              (const unsigned char *)"\x0a\x04\x3a\x02\x40\x7b", 6,
              buf, sizeof(buf), &n) == 0 &&
              n == 0,
          "decode echo ack without output");
    check(cmosh_decode_host_output(
              (const unsigned char *)
                  "\x0a\x05\x12\x03\x22\x01" "a"
                  "\x0a\x05\x12\x03\x22\x01" "b",
              14, buf, sizeof(buf), &n) == 0 &&
              n == 2 && memcmp(buf, "ab", 2) == 0,
          "decode multiple host output instructions");
    check(cmosh_decode_host_output(
              (const unsigned char *)"\x12\x05\x12\x03\x22\x01X", 7,
              buf, sizeof(buf), &n) == 0 &&
              n == 0,
          "ignore unknown top-level host bytes");
    check(cmosh_decode_host_output(
              (const unsigned char *)"\x0a\x05\x1a\x03\x22\x01Y", 7,
              buf, sizeof(buf), &n) == 0 &&
              n == 0,
          "ignore unknown host instruction bytes");
    check(cmosh_decode_host_output(
              (const unsigned char *)
                  "\x0a\x0a\x12\x08\x2d\x01\x02\x03\x04\x22\x01Z",
              12, buf, sizeof(buf), &n) == 0 &&
              n == 1 && buf[0] == 'Z',
          "skip unknown fixed-width host fields");
    {
        unsigned char top[9300], instruction[9200], hostbytes[9100];
        unsigned char data[9000];
        size_t hpos = 0, ipos = 0, tpos = 0, total = 0;

        memset(data, 'x', sizeof(data));
        check(cmosh_pb_put_varint(hostbytes, sizeof(hostbytes), &hpos,
                                  (4U << 3) | 2) == 0,
              "encode large host bytes key");
        check(cmosh_pb_put_varint(hostbytes, sizeof(hostbytes), &hpos,
                                  sizeof(data)) == 0,
              "encode large host bytes length");
        memcpy(hostbytes + hpos, data, sizeof(data));
        hpos += sizeof(data);
        check(cmosh_pb_put_varint(instruction, sizeof(instruction), &ipos,
                                  (2U << 3) | 2) == 0,
              "encode large host instruction key");
        check(cmosh_pb_put_varint(instruction, sizeof(instruction), &ipos,
                                  hpos) == 0,
              "encode large host instruction length");
        memcpy(instruction + ipos, hostbytes, hpos);
        ipos += hpos;
        check(cmosh_pb_put_varint(top, sizeof(top), &tpos,
                                  (1U << 3) | 2) == 0,
              "encode large host top-level key");
        check(cmosh_pb_put_varint(top, sizeof(top), &tpos, ipos) == 0,
              "encode large host top-level length");
        memcpy(top + tpos, instruction, ipos);
        tpos += ipos;
        check(cmosh_decode_host_output(top, tpos, buf, 32, &n) != 0,
              "large host output does not fit fixed buffer");
        check(cmosh_decode_host_output_cb(top, tpos, count_host_output,
                                          &total) == 0 &&
                  total == sizeof(data),
              "large host output streams through callback");
    }
    check(cmosh_encode_user_keystroke_message((const unsigned char *)"x", 1,
                                              buf, sizeof(buf), &n) == 0 &&
              n == 7 && memcmp(buf, "\x0a\x05\x12\x03\x22\x01", 6) == 0 &&
              buf[6] == 'x',
          "encode user keystroke message");
}

static void test_fragment(void)
{
    unsigned char compressed[64], fragment[96], server_zlib[64];
    unsigned char decompressed[64];
    size_t n, fn;
    struct cmosh_fragment frag;

    check(cmosh_zlib_store_compress((const unsigned char *)"abc", 3,
                                    compressed, sizeof(compressed), &n) == 0,
          "zlib stored compress");
    check(n == 14 && memcmp(compressed,
                            "\x78\x01\x01\x03\x00\xfc\xff"
                            "abc\x02\x4d\x01\x27",
                            14) == 0,
          "zlib stored bytes");
    check(cmosh_zlib_store_decompress(compressed, n, decompressed,
                                      sizeof(decompressed), &fn) == 0 &&
              fn == 3 && memcmp(decompressed, "abc", 3) == 0,
          "zlib stored decompress");
    check(cmosh_encode_fragment(0x0102030405060708ULL, 0, 0, compressed, n,
                                fragment, sizeof(fragment), &fn) == 0,
          "encode fragment");
    check(fn == 24 && memcmp(fragment,
                             "\x01\x02\x03\x04\x05\x06\x07\x08"
                             "\x00\x00",
                             10) == 0,
          "fragment header bytes");
    check(cmosh_decode_fragment(fragment, fn, &frag) == 0 &&
              frag.id == 0x0102030405060708ULL && frag.index == 0 &&
              !frag.final && frag.payload_len == n,
          "decode fragment");
    check(cmosh_encode_fragment(0x0102030405060708ULL, 0, 1, compressed, n,
                                fragment, sizeof(fragment), &fn) == 0 &&
              fragment[8] == 0x80 && fragment[9] == 0x00,
          "encode final fragment flag");

    n = fromhex("78 9c e3 60 12 60 90 60 54 60 d4 60 30 62 b0 62 "
                "62 61 06 00 07 67 00 f2",
                server_zlib, sizeof(server_zlib));
    check(cmosh_zlib_store_decompress(server_zlib, n, decompressed,
                                      sizeof(decompressed), &fn) == 0 &&
              fn == 16 &&
              memcmp(decompressed,
                     "\x08\x02\x10\x00\x18\x01\x20\x01\x28\x00\x32\x00"
                     "\x3a\x02\x04\x03",
                     16) == 0,
          "zlib dynamic server payload decompress");
}

static void test_transport(void)
{
    struct cmosh_transport_state st;
    unsigned char key[16], packet[256], plain[128], nonce[8], diff_copy[64];
    const unsigned char msg[] = "transport";
    const unsigned char diff[] = "xy";
    struct cmosh_transport_instruction ti;
    unsigned int timestamp, echo_timestamp;
    size_t n;
    uint64_t seq;

    cmosh_transport_init(&st);
    check(cmosh_transport_note_recv(&st, 2) == 0, "transport accepts new seq");
    check(cmosh_transport_note_recv(&st, 2) != 0, "transport rejects replay");
    check(cmosh_transport_note_recv(&st, 1) == 0,
          "transport accepts out-of-order seq");
    check(st.latest_ack == 2, "transport latest ack");
    check(cmosh_transport_note_recv(&st, 1) != 0,
          "transport rejects out-of-order replay");
    fromhex("000102030405060708090A0B0C0D0E0F", key, sizeof(key));
    cmosh_transport_nonce_from_seq(CMOSH_CLIENT_NONCE_BASE | 1, nonce);
    check(memcmp(nonce, "\x00\x00\x00\x00\x00\x00\x00\x01", 8) == 0,
          "transport packet nonce");
    check(cmosh_transport_encrypt_packet(key, CMOSH_CLIENT_NONCE_BASE | 1, msg,
                                         sizeof(msg) - 1, packet,
                                         sizeof(packet), &n) == 0,
          "transport encrypt packet");
    check(cmosh_transport_decrypt_packet(key, packet, n, plain, sizeof(plain),
                                         &n, &seq) == 0 &&
              seq == (CMOSH_CLIENT_NONCE_BASE | 1) && n == sizeof(msg) - 1 &&
              memcmp(plain, msg, n) == 0,
          "transport decrypt packet");
    packet[12] ^= 1;
    check(cmosh_transport_decrypt_packet(key, packet,
                                         CMOSH_PACKET_NONCE_LEN +
                                             sizeof(msg) - 1 +
                                             CMOSH_PACKET_TAG_LEN,
                                         plain, sizeof(plain), &n, &seq) != 0,
          "transport rejects tampered packet");

    check(cmosh_transport_make_packet(key, 7, 3, 4, 5, diff, sizeof(diff),
                                      0x1234, 0x5678, packet,
                                      sizeof(packet), &n) == 0,
          "transport make instruction packet");
    memset(&ti, 0, sizeof(ti));
    memset(diff_copy, 0, sizeof(diff_copy));
    check(cmosh_transport_decode_packet(key, packet, n, &ti, diff_copy,
                                        sizeof(diff_copy), &timestamp,
                                        &echo_timestamp, &seq) == 0 &&
              seq == (CMOSH_CLIENT_NONCE_BASE | 7) &&
              timestamp == 0x1234 && echo_timestamp == 0x5678 &&
              ti.old_num == 3 && ti.new_num == 4 && ti.ack_num == 5 &&
              ti.diff == diff_copy && ti.diff_len == sizeof(diff) &&
              memcmp(diff_copy, diff, sizeof(diff)) == 0,
          "transport decode instruction packet owns diff");
    {
        unsigned char malformed[8];

        cmosh_transport_init(&st);
        check(cmosh_transport_encrypt_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 13,
                  (const unsigned char *)"", 0, malformed,
                  sizeof(malformed), &n) != 0,
              "transport rejects undersized malformed packet buffer");
        check(cmosh_transport_encrypt_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 13,
                  (const unsigned char *)"", 0, packet,
                  sizeof(packet), &n) == 0,
              "transport encrypt malformed packet");
        check(cmosh_transport_decode_packet_state(
                  &st, key, packet, n, &ti, diff_copy, sizeof(diff_copy),
                  &timestamp, &echo_timestamp, &seq) < 0,
              "transport rejects malformed packet without recording seq");
        check(cmosh_transport_make_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 13, 4, 5, 6, diff,
                  sizeof(diff), 0x1234, 0x5678, packet, sizeof(packet),
                  &n) == 0,
              "transport make valid packet after malformed same seq");
        memset(&ti, 0, sizeof(ti));
        check(cmosh_transport_decode_packet_state(
                  &st, key, packet, n, &ti, diff_copy, sizeof(diff_copy),
                  &timestamp, &echo_timestamp, &seq) == 0 &&
                  ti.old_num == 4 && ti.new_num == 5 &&
                  ti.ack_num == 6,
              "transport accepts valid packet after malformed same seq");
        check(cmosh_transport_make_packet(
                  key, CMOSH_CLIENT_NONCE_BASE | 14, 5, 6, 7, diff,
                  sizeof(diff), 0x1234, 0x5678, packet, sizeof(packet),
                  &n) == 0,
              "transport make reflected client packet");
        memset(&ti, 0, sizeof(ti));
        check(cmosh_transport_decode_packet_state(
                  &st, key, packet, n, &ti, diff_copy, sizeof(diff_copy),
                  &timestamp, &echo_timestamp, &seq) < 0,
              "transport rejects reflected client nonce");
        check(cmosh_transport_make_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 14, 5, 6, 7, diff,
                  sizeof(diff), 0x1234, 0x5678, packet, sizeof(packet),
                  &n) == 0,
              "transport make server packet after reflected client packet");
        memset(&ti, 0, sizeof(ti));
        check(cmosh_transport_decode_packet_state(
                  &st, key, packet, n, &ti, diff_copy, sizeof(diff_copy),
                  &timestamp, &echo_timestamp, &seq) == 0 &&
                  ti.old_num == 5 && ti.new_num == 6 &&
                  ti.ack_num == 7,
              "transport accepts server packet after reflected client packet");
    }

    {
        unsigned char instruction[128], compressed[256], fragment[256];
        unsigned char packet2[256], plain2[300];
        const unsigned char fdiff[] = "fragmented";
        size_t instruction_len, compressed_len, fragment_len, plain_len;
        size_t split;
        int decode_result;

        cmosh_transport_init(&st);
        memset(&ti, 0, sizeof(ti));
        ti.protocol_version = CMOSH_PROTOCOL_VERSION;
        ti.old_num = 4;
        ti.new_num = 5;
        ti.ack_num = 6;
        ti.diff = fdiff;
        ti.diff_len = sizeof(fdiff);
        check(cmosh_encode_transport_instruction(&ti, instruction,
                                                 sizeof(instruction),
                                                 &instruction_len) == 0,
              "encode fragmented transport instruction");
        check(cmosh_zlib_store_compress(instruction, instruction_len,
                                        compressed, sizeof(compressed),
                                        &compressed_len) == 0,
              "compress fragmented transport instruction");
        split = compressed_len / 2;

        check(cmosh_encode_fragment(99, 0, 0, compressed, split, fragment,
                                    sizeof(fragment), &fragment_len) == 0,
              "encode first transport fragment");
        plain2[0] = 0x12;
        plain2[1] = 0x34;
        plain2[2] = 0x56;
        plain2[3] = 0x78;
        memcpy(plain2 + 4, fragment, fragment_len);
        plain_len = fragment_len + 4;
        check(cmosh_transport_encrypt_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 40, plain2, plain_len,
                  packet2, sizeof(packet2), &n) == 0,
              "encrypt first transport fragment");
        memset(&ti, 0, sizeof(ti));
        decode_result = cmosh_transport_decode_packet_state(
            &st, key, packet2, n, &ti, diff_copy, sizeof(diff_copy),
            &timestamp, &echo_timestamp, &seq);
        check(decode_result == 1 &&
                  seq == (CMOSH_SERVER_NONCE_BASE | 40),
              "transport waits for fragmented instruction");

        check(cmosh_encode_fragment(99, 1, 1, compressed + split,
                                    compressed_len - split, fragment,
                                    sizeof(fragment), &fragment_len) == 0,
              "encode final transport fragment");
        memcpy(plain2 + 4, fragment, fragment_len);
        plain_len = fragment_len + 4;
        check(cmosh_transport_encrypt_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 41, plain2, plain_len,
                  packet2, sizeof(packet2), &n) == 0,
              "encrypt final transport fragment");
        memset(&ti, 0, sizeof(ti));
        memset(diff_copy, 0, sizeof(diff_copy));
        decode_result = cmosh_transport_decode_packet_state(
            &st, key, packet2, n, &ti, diff_copy, sizeof(diff_copy),
            &timestamp, &echo_timestamp, &seq);
        check(decode_result == 0 && ti.old_num == 4 && ti.new_num == 5 &&
                  ti.ack_num == 6 && ti.diff_len == sizeof(fdiff) &&
                  memcmp(diff_copy, fdiff, sizeof(fdiff)) == 0,
              "transport reassembles fragmented instruction");

        check(cmosh_encode_fragment(100, 0, 0, compressed, split,
                                    fragment, sizeof(fragment),
                                    &fragment_len) == 0,
              "encode stale first transport fragment");
        memcpy(plain2 + 4, fragment, fragment_len);
        plain_len = fragment_len + 4;
        check(cmosh_transport_encrypt_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 42, plain2, plain_len,
                  packet2, sizeof(packet2), &n) == 0,
              "encrypt stale first transport fragment");
        memset(&ti, 0, sizeof(ti));
        check(cmosh_transport_decode_packet_state(
                  &st, key, packet2, n, &ti, diff_copy,
                  sizeof(diff_copy), &timestamp, &echo_timestamp, &seq) == 1,
              "transport waits for stale fragment");
        check(cmosh_transport_make_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 12, 5, 6, 7, fdiff,
                  sizeof(fdiff), 0x1234, 0x5678, packet2, sizeof(packet2),
                  &n) == 0,
              "make complete packet after stale fragment");
        memset(&ti, 0, sizeof(ti));
        memset(diff_copy, 0, sizeof(diff_copy));
        check(cmosh_transport_decode_packet_state(
                  &st, key, packet2, n, &ti, diff_copy,
                  sizeof(diff_copy), &timestamp, &echo_timestamp, &seq) == 0 &&
                  ti.old_num == 5 && ti.new_num == 6 &&
                  memcmp(diff_copy, fdiff, sizeof(fdiff)) == 0 &&
                  !st.fragment_active,
              "transport complete packet clears stale fragment");

        cmosh_transport_clear(&st);
        check(cmosh_encode_fragment(101, 0, 0, compressed, split,
                                    fragment, sizeof(fragment),
                                    &fragment_len) == 0,
              "encode conflict first transport fragment");
        memcpy(plain2 + 4, fragment, fragment_len);
        plain_len = fragment_len + 4;
        check(cmosh_transport_encrypt_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 43, plain2, plain_len,
                  packet2, sizeof(packet2), &n) == 0,
              "encrypt conflict first transport fragment");
        memset(&ti, 0, sizeof(ti));
        check(cmosh_transport_decode_packet_state(
                  &st, key, packet2, n, &ti, diff_copy,
                  sizeof(diff_copy), &timestamp, &echo_timestamp, &seq) == 1,
              "transport waits for conflict fragment");
        fragment[10] ^= 1;
        memcpy(plain2 + 4, fragment, fragment_len);
        check(cmosh_transport_encrypt_packet(
                  key, CMOSH_SERVER_NONCE_BASE | 44, plain2, plain_len,
                  packet2, sizeof(packet2), &n) == 0,
              "encrypt conflicting transport fragment");
        memset(&ti, 0, sizeof(ti));
        check(cmosh_transport_decode_packet_state(
                  &st, key, packet2, n, &ti, diff_copy,
                  sizeof(diff_copy), &timestamp, &echo_timestamp, &seq) < 0 &&
                  !st.fragment_active,
              "transport clears conflicting fragment state");
        cmosh_transport_clear(&st);
    }
}

static void test_session(void)
{
    struct cmosh_input_state input;
    struct cmosh_input_record *retry;
    struct cmosh_server_queue queue;
    struct cmosh_server_diff *entry;
    unsigned char diff[64];
    size_t diff_len;
    uint64_t gap_old, gap_new;

    cmosh_input_init(&input, 10);
    check(cmosh_input_append(&input, (const unsigned char *)"ab", 2, 100) ==
              0 &&
              input.current == 11 && input.nrecords == 1 &&
              input.bytes_len == 2,
          "session input append");
    check(cmosh_input_append(&input, (const unsigned char *)"c", 1, 200) ==
              0 &&
              input.current == 12 && input.nrecords == 2 &&
              input.bytes_len == 3,
          "session input append second");
    cmosh_input_note_ack(&input, 11);
    check(input.acked == 11 && input.nrecords == 1 && input.bytes_len == 1 &&
              input.records[0].state == 12 && input.records[0].off == 0 &&
              input.bytes[0] == 'c',
          "session input ack trims records");
    retry = cmosh_input_retransmit_record(&input, 200);
    check(retry == NULL, "session input no early retransmit");
    retry = cmosh_input_retransmit_record(&input, 199);
    check(retry == NULL, "session input no retransmit on backwards time");
    retry = cmosh_input_retransmit_record(&input,
                                          200 + CMOSH_INPUT_RETRY_FIRST_MS);
    check(retry && retry->state == 12, "session input retransmit due");
    check(cmosh_input_record_diff(&input, retry, diff, sizeof(diff),
                                  &diff_len) == 0 &&
              diff_len == 7 && memcmp(diff, "\x0a\x05\x12\x03\x22\x01", 6) == 0 &&
              diff[6] == 'c',
          "session input retransmit diff");

    cmosh_server_queue_init(&queue);
    check(cmosh_server_queue_add(&queue, 1, 2,
                                 (const unsigned char *)"a", 1) == 0,
          "session queue add next");
    check(cmosh_server_queue_add(&queue, 4, 5,
                                 (const unsigned char *)"e", 1) == 0,
          "session queue add future");
    check(cmosh_server_queue_add(&queue, 2, 3,
                                 (const unsigned char *)"b", 1) == 0,
          "session queue add second");
    entry = cmosh_server_queue_pop_next(&queue, 1);
    check(entry && entry->old_num == 1 && entry->new_num == 2 &&
              entry->diff[0] == 'a',
          "session queue pop next");
    entry->used = 0;
    entry = cmosh_server_queue_pop_next(&queue, 2);
    check(entry && entry->old_num == 2 && entry->new_num == 3 &&
              entry->diff[0] == 'b',
          "session queue pop second");
    check(cmosh_server_queue_add(&queue, 2, 4,
                                 (const unsigned char *)"bc", 2) == 0,
          "session queue add overlapping stale");
    check(cmosh_server_queue_pop_next(&queue, 3) == NULL,
          "session queue ignores overlapping stale diff");
    check(cmosh_server_queue_waiting_for_gap(&queue, 3, &gap_old,
                                             &gap_new) &&
              gap_old == 4 && gap_new == 5,
          "session queue detects gap");
}

struct test_output_sink {
    unsigned char bytes[16];
    size_t len;
};

static int test_output_callback(void *vctx, const unsigned char *diff,
                                size_t diff_len)
{
    struct test_output_sink *sink = (struct test_output_sink *)vctx;

    if (sink->len + diff_len > sizeof(sink->bytes))
        return -1;
    memcpy(sink->bytes + sink->len, diff, diff_len);
    sink->len += diff_len;
    return 0;
}

static int make_test_transport_packet(
    const unsigned char key[16], uint64_t seq, uint64_t old_num,
    uint64_t new_num, uint64_t ack_num, uint64_t throwaway_num,
    const unsigned char *diff, size_t diff_len, unsigned int timestamp,
    unsigned char *packet, size_t packet_cap, size_t *packet_len)
{
    unsigned char instruction[512], compressed[640], fragment[704];
    unsigned char plain[CMOSH_MAX_PACKET];
    size_t instruction_len, compressed_len, fragment_len, plain_len;
    struct cmosh_transport_instruction ti;

    memset(&ti, 0, sizeof(ti));
    ti.protocol_version = CMOSH_PROTOCOL_VERSION;
    ti.old_num = old_num;
    ti.new_num = new_num;
    ti.ack_num = ack_num;
    ti.throwaway_num = throwaway_num;
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
    plain[0] = (unsigned char)(timestamp >> 8);
    plain[1] = (unsigned char)timestamp;
    plain[2] = 0;
    plain[3] = 0;
    memcpy(plain + 4, fragment, fragment_len);
    plain_len = fragment_len + 4;
    return cmosh_transport_encrypt_packet(key, CMOSH_SERVER_NONCE_BASE | seq,
                                          plain, plain_len, packet,
                                          packet_cap, packet_len);
}

static void test_client(void)
{
    struct cmosh_client client;
    struct cmosh_transport_instruction ti;
    unsigned char key[16], packet[256], diff[128];
    struct test_output_sink sink;
    size_t n, diff_len;
    uint64_t seq, previous_state;
    unsigned int timestamp;
    int queued_future;

    fromhex("000102030405060708090A0B0C0D0E0F", key, sizeof(key));
    check(cmosh_client_make_initial_packet(key, 80, 24, 0x1001, packet,
                                           sizeof(packet), &n,
                                           &diff_len) == 0 &&
              diff_len == 8,
          "client make initial packet");
    memset(&ti, 0, sizeof(ti));
    check(cmosh_transport_decode_packet(key, packet, n, &ti, diff,
                                        sizeof(diff), &timestamp, &timestamp,
                                        &seq) == 0 &&
              seq == (CMOSH_CLIENT_NONCE_BASE | 1) &&
              ti.old_num == 0 && ti.new_num == 1 && ti.ack_num == 0 &&
              ti.diff_len == 8,
          "client initial packet fields");
    check(cmosh_client_make_start_ack(key, 0x2002, 0x1002, packet,
                                      sizeof(packet), &n) == 0,
          "client make start ack");
    memset(&ti, 0, sizeof(ti));
    check(cmosh_transport_decode_packet(key, packet, n, &ti, diff,
                                        sizeof(diff), &timestamp, &timestamp,
                                        &seq) == 0 &&
              seq == (CMOSH_CLIENT_NONCE_BASE | 2) &&
              ti.old_num == 1 && ti.new_num == 1 && ti.ack_num == 1 &&
              ti.diff_len == 0,
          "client start ack fields");

    cmosh_client_init(&client, key, 10, 20, CMOSH_SERVER_NONCE_BASE | 2,
                      0x1111, 3);
    check(client.send_seq == 3 && client.server_state == 20 &&
              client.input.acked == 10 && client.echo_timestamp == 0x1111,
          "client init state");

    check(cmosh_client_make_ack(&client, 0x2222, packet, sizeof(packet),
                                &n) == 0 &&
              client.send_seq == 4,
          "client make ack");
    {
        uint64_t send_seq = client.send_seq;

        check(cmosh_client_make_ack(&client, 0x2223, packet, 1, &n) != 0 &&
                  client.send_seq == send_seq,
              "client failed ack does not advance send seq");
    }
    memset(&ti, 0, sizeof(ti));
    check(cmosh_transport_decode_packet(key, packet, n, &ti, diff,
                                        sizeof(diff), &timestamp, &timestamp,
                                        &seq) == 0 &&
              seq == (CMOSH_CLIENT_NONCE_BASE | 3) &&
              ti.old_num == 10 && ti.new_num == 10 &&
              ti.ack_num == 20 && ti.diff_len == 0,
          "client ack packet fields");

    check(cmosh_client_make_input(&client, (const unsigned char *)"x", 1,
                                  100, 0x3333, packet, sizeof(packet),
                                  &n) == 0 &&
              client.input.current == 11 && client.send_seq == 5,
          "client make input");
    {
        uint64_t current = client.input.current;
        uint64_t send_seq = client.send_seq;
        size_t nrecords = client.input.nrecords;
        size_t bytes_len = client.input.bytes_len;

        check(cmosh_client_make_input(&client, (const unsigned char *)"y",
                                      1, 101, 0x3334, packet, 1, &n) != 0 &&
                  client.input.current == current &&
                  client.send_seq == send_seq &&
                  client.input.nrecords == nrecords &&
                  client.input.bytes_len == bytes_len,
              "client failed input packet does not advance state");
    }
    {
        unsigned char bigkeys[CMOSH_CLIENT_INPUT_CHUNK_MAX + 1];
        uint64_t current = client.input.current;
        uint64_t send_seq = client.send_seq;
        size_t nrecords = client.input.nrecords;
        size_t bytes_len = client.input.bytes_len;

        memset(bigkeys, 'p', sizeof(bigkeys));
        check(cmosh_client_make_input(&client, bigkeys, sizeof(bigkeys),
                                      101, 0x3334, packet, sizeof(packet),
                                      &n) != 0 &&
                  client.input.current == current &&
                  client.send_seq == send_seq &&
                  client.input.nrecords == nrecords &&
                  client.input.bytes_len == bytes_len,
              "client oversized input does not advance state");
    }
    memset(&ti, 0, sizeof(ti));
    check(cmosh_transport_decode_packet(key, packet, n, &ti, diff,
                                        sizeof(diff), &timestamp, &timestamp,
                                        &seq) == 0 &&
              seq == (CMOSH_CLIENT_NONCE_BASE | 4) &&
              ti.old_num == 10 && ti.new_num == 11 &&
              ti.ack_num == 20 && ti.diff_len == 7,
          "client input packet fields");

    check(make_test_transport_packet(key, 8, 20, 20, 10, 11, NULL, 0,
                                     0x3888, packet, sizeof(packet), &n) == 0,
          "client test throwaway packet");
    memset(&ti, 0, sizeof(ti));
    check(cmosh_client_recv_packet(&client, packet, n, &ti, diff,
                                   sizeof(diff), &timestamp, &seq) ==
              CMOSH_CLIENT_RECV_OK &&
              client.input.acked == 11 && client.input.nrecords == 0,
          "client receive throwaway trims input");

    check(cmosh_client_make_resize(&client, 132, 43, 200, 0x3999, packet,
                                   sizeof(packet), &n) == 0 &&
              client.input.current == 12 && client.input.nrecords == 1,
          "client make resize tracks input state");
    memset(&ti, 0, sizeof(ti));
    check(cmosh_transport_decode_packet(key, packet, n, &ti, diff,
                                        sizeof(diff), &timestamp, &timestamp,
                                        &seq) == 0 &&
              ti.old_num == 11 && ti.new_num == 12 && ti.diff_len != 0,
          "client resize packet fields");
    {
        struct cmosh_client_idle_event idle_event;
        size_t resize_diff_len = ti.diff_len;
        unsigned char resize_diff[64];

        memcpy(resize_diff, diff, resize_diff_len);
        check(cmosh_client_make_idle_event(
                  &client, 200 + CMOSH_INPUT_RETRY_FIRST_MS, 0x3aaa, packet,
                  sizeof(packet), &n, &idle_event) == 0 &&
                  idle_event.retransmitted &&
                  idle_event.retransmit_state == 12 &&
                  idle_event.input_acked == 11 &&
                  idle_event.input_current == 12 &&
                  idle_event.input_records == 1 &&
                  idle_event.input_bytes == resize_diff_len,
              "client resize retransmit due");
        memset(&ti, 0, sizeof(ti));
        check(cmosh_transport_decode_packet(key, packet, n, &ti, diff,
                                            sizeof(diff), &timestamp,
                                            &timestamp, &seq) == 0 &&
                  ti.old_num == 11 && ti.new_num == 12 &&
                  ti.diff_len == resize_diff_len &&
                  memcmp(diff, resize_diff, resize_diff_len) == 0,
              "client resize retransmits same diff");
    }
    check(make_test_transport_packet(key, 9, 20, 20, 10, 12, NULL, 0,
                                     0x3bbb, packet, sizeof(packet), &n) == 0,
          "client test resize ack packet");
    memset(&ti, 0, sizeof(ti));
    check(cmosh_client_recv_packet(&client, packet, n, &ti, diff,
                                   sizeof(diff), &timestamp, &seq) ==
              CMOSH_CLIENT_RECV_OK &&
              client.input.acked == 12 && client.input.nrecords == 0,
          "client receive ack trims resize");

    check(cmosh_transport_make_packet(key, CMOSH_SERVER_NONCE_BASE | 10,
                                      20, 21, 12,
                                      (const unsigned char *)"h", 1, 0x4444,
                                      0, packet, sizeof(packet), &n) == 0,
          "client test server packet");
    memset(&ti, 0, sizeof(ti));
    check(cmosh_client_recv_packet(&client, packet, n, &ti, diff,
                                   sizeof(diff), &timestamp, &seq) ==
              CMOSH_CLIENT_RECV_OK &&
              seq == (CMOSH_SERVER_NONCE_BASE | 10) &&
              client.input.acked == 12 &&
              client.echo_timestamp == 0x4444 && ti.diff_len == 1 &&
              diff[0] == 'h',
          "client receive packet");
    check(cmosh_client_recv_packet(&client, packet, n, &ti, diff,
                                   sizeof(diff), &timestamp, &seq) ==
              CMOSH_CLIENT_RECV_DUPLICATE,
          "client receive duplicate");
    check(cmosh_client_queue_server_diff(&client, &ti) == 0 &&
              cmosh_server_queue_pop_next(&client.server_queue,
                                          client.server_state) != NULL,
          "client queue server diff");

    memset(&sink, 0, sizeof(sink));
    previous_state = 0;
    queued_future = 0;
    check(cmosh_client_note_server_instruction(&client, &ti, &queued_future,
                                               &previous_state) == 0 &&
              previous_state == 20 && !queued_future,
          "client note server instruction");
    check(cmosh_client_apply_server_diffs(&client, test_output_callback,
                                          &sink) == 0 &&
              client.server_state == 21 && sink.len == 1 &&
              sink.bytes[0] == 'h',
          "client apply server diffs");
    check(cmosh_client_should_ack(&client, &ti, previous_state),
          "client ack after state advance");
    check(!cmosh_client_waiting_for_gap(&client, NULL, NULL),
          "client no gap after apply");

    memset(&ti, 0, sizeof(ti));
    ti.old_num = 30;
    ti.new_num = 31;
    ti.diff = (const unsigned char *)"z";
    ti.diff_len = 1;
    queued_future = 0;
    check(cmosh_client_note_server_instruction(&client, &ti, &queued_future,
                                               &previous_state) == 0 &&
              previous_state == 21 && queued_future &&
              cmosh_client_waiting_for_gap(&client, NULL, NULL),
          "client queues future gap");
    check(cmosh_client_missing_state_diag_due(
              &client, CMOSH_CLIENT_MISSING_STATE_DIAG_MS, NULL, NULL),
          "client missing state diag due");
    check(!cmosh_client_missing_state_diag_due(
              &client, CMOSH_CLIENT_MISSING_STATE_DIAG_MS + 1, NULL, NULL),
          "client missing state diag throttled");
    check(!cmosh_client_missing_state_diag_due(
              &client, CMOSH_CLIENT_MISSING_STATE_DIAG_MS - 1, NULL, NULL),
          "client missing state diag handles backwards time");
    cmosh_client_note_recv_time(&client, 100);
    check(!cmosh_client_udp_timeout_due(&client, 99),
          "client udp timeout handles backwards recv time");
    check(!cmosh_client_udp_timeout_due(
              &client, 100 + CMOSH_CLIENT_UDP_TIMEOUT_DIAG_MS - 1),
          "client udp timeout not early");
    check(cmosh_client_udp_timeout_due(
              &client, 100 + CMOSH_CLIENT_UDP_TIMEOUT_DIAG_MS),
          "client udp timeout due");
    check(!cmosh_client_udp_timeout_due(
              &client, 100 + CMOSH_CLIENT_UDP_TIMEOUT_DIAG_MS + 1),
          "client udp timeout throttled");

    cmosh_client_init(&client, key, 1, 5, CMOSH_SERVER_NONCE_BASE | 10,
                      0, 6);
    check(cmosh_client_make_input(&client, (const unsigned char *)"x", 1,
                                  100, 0x5100, packet, sizeof(packet),
                                  &n) == 0 &&
              cmosh_client_make_input(&client, (const unsigned char *)"y",
                                      1, 101, 0x5101, packet,
                                      sizeof(packet), &n) == 0,
          "client process ack setup");
    check(make_test_transport_packet(key, 11, 5, 5, 2, 3, NULL, 0,
                                     0x5111, packet, sizeof(packet), &n) == 0,
          "client process ack packet source");
    {
        struct cmosh_client_recv_event event;
        check(cmosh_client_process_packet(
                  &client, packet, n, diff, sizeof(diff),
                  test_output_callback, &sink, &event) ==
                  CMOSH_CLIENT_RECV_OK &&
              event.ack_num == 2 && event.throwaway_num == 3 &&
              event.input_acked_before == 1 &&
              event.input_acked_after == 3 &&
              event.input_current == 3 &&
              event.input_records_before == 2 &&
              event.input_records_after == 0 &&
              event.input_bytes_before == 2 &&
              event.input_bytes_after == 0 &&
              client.input.acked == 3 && client.input.nrecords == 0,
              "client process reports input ack transition");
    }

    cmosh_client_init(&client, key, 1, 5, CMOSH_SERVER_NONCE_BASE | 10,
                      0, 6);
    {
        struct cmosh_client_idle_event idle_event;

        check(cmosh_client_make_idle_event(&client, 100, 0x5100, packet,
                                           sizeof(packet), &n,
                                           &idle_event) == 0 &&
                  n != 0 && !idle_event.retransmitted,
              "client idle packet setup");
        check(cmosh_client_make_idle_event(&client, 100, 0x5101, packet,
                                           sizeof(packet), &n,
                                           &idle_event) == 0 &&
                  n == 0,
              "client idle packet is rate limited");
        cmosh_client_note_idle_send_failed(&client);
        check(cmosh_client_make_idle_event(&client, 100, 0x5102, packet,
                                           sizeof(packet), &n,
                                           &idle_event) == 0 &&
                  n != 0 && !idle_event.retransmitted,
              "client idle send failure makes keepalive retry due");
    }

    cmosh_client_init(&client, key, 1, 5, CMOSH_SERVER_NONCE_BASE | 10,
                      0, 6);
    check(cmosh_client_make_input(&client, (const unsigned char *)"x", 1,
                                  100, 0x5200, packet, sizeof(packet),
                                  &n) == 0,
          "client send failure setup");
    cmosh_client_note_input_send_failed(&client, 2, 100);
    {
        struct cmosh_client_idle_event idle_event;
        check(cmosh_client_make_idle_event(&client, 100, 0x5201, packet,
                                           sizeof(packet), &n,
                                           &idle_event) == 0 &&
                  n != 0 && idle_event.retransmitted &&
                  idle_event.retransmit_state == 2 &&
                  client.input.records[0].send_count == 1,
              "client transient send failure makes input retry due");
    }

    cmosh_client_init(&client, key, 1, 5, CMOSH_SERVER_NONCE_BASE | 10,
                      0, 6);
    memset(&sink, 0, sizeof(sink));
    check(cmosh_transport_make_packet(key, CMOSH_SERVER_NONCE_BASE | 11,
                                      5, 6, 1,
                                      (const unsigned char *)"q", 1, 0x5555,
                                      0, packet, sizeof(packet), &n) == 0,
          "client process packet source");
    {
        struct cmosh_client_recv_event event;
        check(cmosh_client_process_packet(
                  &client, packet, n, diff, sizeof(diff),
                  test_output_callback, &sink, &event) ==
                  CMOSH_CLIENT_RECV_OK &&
              event.seq == (CMOSH_SERVER_NONCE_BASE | 11) &&
              event.old_num == 5 && event.new_num == 6 &&
              event.diff_len == 1 && event.should_ack &&
              !event.queued_future && !event.server_shutdown &&
              client.server_state == 6 && sink.len == 1 &&
              sink.bytes[0] == 'q',
              "client process packet applies diff");
        check(cmosh_client_process_packet(
                  &client, packet, n, diff, sizeof(diff),
                  test_output_callback, &sink, &event) ==
                  CMOSH_CLIENT_RECV_DUPLICATE &&
              event.result == CMOSH_CLIENT_RECV_DUPLICATE,
              "client process duplicate");
    }

    cmosh_client_init(&client, key, 1, 5, CMOSH_SERVER_NONCE_BASE | 20,
                      0, 6);
    memset(&ti, 0, sizeof(ti));
    ti.old_num = 8;
    ti.new_num = 9;
    ti.diff = (const unsigned char *)"g";
    ti.diff_len = 1;
    check(cmosh_client_note_server_instruction(&client, &ti, &queued_future,
                                               &previous_state) == 0 &&
              queued_future,
          "client idle setup gap");
    {
        struct cmosh_client_idle_event idle_event;
        check(cmosh_client_make_idle_event(
                  &client, CMOSH_CLIENT_UDP_TIMEOUT_DIAG_MS,
                  0x6666, packet, sizeof(packet), &n, &idle_event) == 0 &&
              idle_event.missing_state && idle_event.udp_timeout &&
              !idle_event.retransmitted && idle_event.gap_old_num == 8 &&
              idle_event.gap_new_num == 9,
              "client idle event diagnostics");
    }

    cmosh_client_init(&client, key, 1, 5, CMOSH_SERVER_NONCE_BASE | 30,
                      0, 7);
    {
        struct cmosh_client_idle_event idle_event;
        uint64_t send_seq;

        check(cmosh_client_make_idle_event(&client, 1000, 0x7000, packet,
                                           sizeof(packet), &n,
                                           &idle_event) == 0 &&
                  n != 0 && !idle_event.retransmitted,
              "client first idle keepalive sends");
        send_seq = client.send_seq;
        check(cmosh_client_make_idle_event(
                  &client, 1000 + CMOSH_CLIENT_IDLE_KEEPALIVE_MS - 1,
                  0x7001, packet, sizeof(packet), &n, &idle_event) == 0 &&
                  n == 0 && client.send_seq == send_seq &&
                  !idle_event.retransmitted,
              "client idle keepalive is rate limited");
        check(cmosh_client_make_idle_event(
                  &client, 999, 0x7001, packet, sizeof(packet), &n,
                  &idle_event) == 0 &&
                  n == 0 && client.send_seq == send_seq &&
                  !idle_event.retransmitted,
              "client idle keepalive handles backwards time");
        check(cmosh_client_make_idle_event(
                  &client, 1000 + CMOSH_CLIENT_IDLE_KEEPALIVE_MS,
                  0x7002, packet, sizeof(packet), &n, &idle_event) == 0 &&
                  n != 0 && client.send_seq == send_seq + 1,
              "client idle keepalive sends when due");
    }
}

int main(void)
{
    test_base64();
    test_aes_ocb();
    test_bootstrap();
    test_proto();
    test_fragment();
    test_transport();
    test_session();
    test_client();
    return failures ? 1 : 0;
}
