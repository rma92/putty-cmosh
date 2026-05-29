#include "cmosh_base64.h"
#include "cmosh_bootstrap.h"
#include "cmosh_aes.h"
#include "cmosh_fragment.h"
#include "cmosh_ocb.h"
#include "cmosh_proto.h"
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
    unsigned char key[16], packet[128], plain[128], nonce[8];
    const unsigned char msg[] = "transport";
    size_t n;
    uint64_t seq;

    cmosh_transport_init(&st);
    check(cmosh_transport_note_recv(&st, 1) == 0, "transport accepts new seq");
    check(cmosh_transport_note_recv(&st, 1) != 0, "transport rejects replay");
    check(st.latest_ack == 1, "transport latest ack");
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
}

int main(void)
{
    test_base64();
    test_aes_ocb();
    test_bootstrap();
    test_proto();
    test_fragment();
    test_transport();
    return failures ? 1 : 0;
}
