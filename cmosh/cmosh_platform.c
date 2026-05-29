#include "cmosh_platform.h"

#include "cmosh_fragment.h"
#include "cmosh_proto.h"
#include "cmosh_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#define cmosh_close_socket closesocket
typedef SOCKET cmosh_socket_t;
#else
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define cmosh_close_socket close
typedef int cmosh_socket_t;
#endif

static int cmosh_socket_valid(cmosh_socket_t s)
{
    return s != INVALID_SOCKET;
}

void cmosh_console_setup(void)
{
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;

    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(out, mode);
    }
#endif
}

static unsigned int cmosh_now16_ms(void)
{
#ifdef _WIN32
    return (unsigned int)(GetTickCount() & 0xffffU);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned int)(((tv.tv_sec * 1000U) + (tv.tv_usec / 1000U)) &
                          0xffffU);
#endif
}

static void cmosh_dump_hex(FILE *fp, const char *label,
                           const unsigned char *data, size_t len)
{
    size_t i, limit = len < 64 ? len : 64;

    fprintf(fp, "cmosh: %s (%u bytes):", label, (unsigned)len);
    for (i = 0; i < limit; i++)
        fprintf(fp, " %02x", data[i]);
    if (limit < len)
        fputs(" ...", fp);
    fputc('\n', fp);
}

static int cmosh_console_read(unsigned char *buf, size_t buflen, size_t *len)
{
    *len = 0;
#ifdef _WIN32
    if (!_kbhit())
        return 0;
    while (*len < buflen && _kbhit()) {
        int c = _getch();
        if (c == 0 || c == 0xe0)
            continue;
        if (c == '\r')
            c = '\n';
        buf[(*len)++] = (unsigned char)c;
    }
    return 0;
#else
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(1, &rfds, NULL, NULL, &tv) > 0) {
        int n = read(0, buf, buflen);
        if (n > 0)
            *len = (size_t)n;
    }
    return 0;
#endif
}

static int cmosh_make_packet(const unsigned char key[16], uint64_t seq,
                             uint64_t old_num, uint64_t new_num,
                             uint64_t ack_num, const unsigned char *diff,
                             size_t diff_len, unsigned int echo_ts,
                             unsigned char *packet, size_t packet_cap,
                             size_t *packet_len)
{
    unsigned char instruction[512], compressed[640], fragment[704];
    unsigned char plain[CMOSH_MAX_PACKET];
    size_t instruction_len, compressed_len, fragment_len, plain_len;
    struct cmosh_transport_instruction ti;
    unsigned int ts = cmosh_now16_ms();

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
    plain[0] = (unsigned char)(ts >> 8);
    plain[1] = (unsigned char)ts;
    plain[2] = (unsigned char)(echo_ts >> 8);
    plain[3] = (unsigned char)echo_ts;
    memcpy(plain + 4, fragment, fragment_len);
    plain_len = fragment_len + 4;
    return cmosh_transport_encrypt_packet(key, CMOSH_CLIENT_NONCE_BASE | seq,
                                          plain, plain_len, packet,
                                          packet_cap, packet_len);
}

static int cmosh_decode_packet(const unsigned char key[16],
                               const unsigned char *packet, size_t packet_len,
                               struct cmosh_transport_instruction *ti,
                               unsigned int *timestamp, uint64_t *seq)
{
    unsigned char plain[CMOSH_MAX_PACKET], decompressed[1024];
    size_t plain_len, decompressed_len;
    struct cmosh_fragment frag;

    if (cmosh_transport_decrypt_packet(key, packet, packet_len, plain,
                                       sizeof(plain), &plain_len, seq) != 0)
        return -1;
    if (plain_len < 4)
        return -1;
    *timestamp = ((unsigned)plain[0] << 8) | plain[1];
    if (cmosh_decode_fragment(plain + 4, plain_len - 4, &frag) != 0)
        return -1;
    if (cmosh_zlib_store_decompress(frag.payload, frag.payload_len,
                                    decompressed, sizeof(decompressed),
                                    &decompressed_len) != 0)
        return -1;
    return cmosh_decode_transport_instruction(decompressed, decompressed_len,
                                              ti, NULL, NULL);
}

static int append_quoted(char *buf, size_t buflen, size_t *pos, const char *s)
{
    int n;
    if (*pos >= buflen)
        return -1;
    n = snprintf(buf + *pos, buflen - *pos, "\"%s\"", s);
    if (n < 0 || (size_t)n >= buflen - *pos)
        return -1;
    *pos += (size_t)n;
    return 0;
}

static int cmosh_contains_exit_key(const unsigned char *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (buf[i] == 0x1d) /* Ctrl+] */
            return 1;
    }
    return 0;
}

static int append_raw(char *buf, size_t buflen, size_t *pos, const char *s)
{
    size_t len;

    if (!s)
        return -1;
    len = strlen(s);
    if (*pos + len >= buflen)
        return -1;
    memcpy(buf + *pos, s, len);
    *pos += len;
    return 0;
}

int cmosh_run_bootstrap_command(const char *ssh_command, const char *host,
                                const char *remote_command, char *output,
                                size_t output_len, int verbose)
{
    char cmd[2048];
    size_t pos = 0;
    FILE *fp;
    size_t used = 0;
    int rc;

    if (!ssh_command || !host || !remote_command || !output || output_len == 0)
        return -1;

    /*
     * ssh_command is a command prefix, not just an executable path, so that
     * --ssh="plink -i key.ppk" works. The host and remote command are still
     * quoted as individual arguments.
     */
#ifdef _WIN32
    cmd[pos++] = '"'; /* Outer cmd.exe /c quote pair. */
#endif
    if (append_raw(cmd, sizeof(cmd), &pos, ssh_command) ||
        pos + 2 >= sizeof(cmd))
        return -1;
    cmd[pos++] = ' ';
    if (append_quoted(cmd, sizeof(cmd), &pos, host) ||
        pos + 2 >= sizeof(cmd))
        return -1;
    cmd[pos++] = ' ';
    if (append_quoted(cmd, sizeof(cmd), &pos, remote_command))
        return -1;
#ifdef _WIN32
    if (pos + 1 >= sizeof(cmd))
        return -1;
    cmd[pos++] = '"';
#endif
    cmd[pos] = '\0';

    if (verbose)
        fprintf(stderr, "cmosh: bootstrap: %s\n", cmd);

    fp = popen(cmd, "r");
    if (!fp)
        return -1;

    while (used + 1 < output_len) {
        size_t n = fread(output + used, 1, output_len - used - 1, fp);
        used += n;
        if (n == 0)
            break;
    }
    output[used] = '\0';
    rc = pclose(fp);
    return rc == 0 ? 0 : -1;
}

static int cmosh_udp_open_connected(const char *host, unsigned short port,
                                    int ipv4, int ipv6, int verbose,
                                    cmosh_socket_t *out)
{
    struct addrinfo hints, *res = NULL, *ai;
    char service[16];
    int gai;

    if (!host || !out)
        return -1;
    *out = INVALID_SOCKET;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = ipv4 ? AF_INET : (ipv6 ? AF_INET6 : AF_UNSPEC);
    snprintf(service, sizeof(service), "%u", (unsigned)port);

    gai = getaddrinfo(host, service, &hints, &res);
    if (gai != 0) {
        if (verbose)
            fprintf(stderr, "cmosh: UDP resolve failed for %s:%s: %s\n", host,
#ifdef _WIN32
                    service, gai_strerrorA(gai));
#else
                    service, gai_strerror(gai));
#endif
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        cmosh_socket_t s = socket(ai->ai_family, ai->ai_socktype,
                                  ai->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) != SOCKET_ERROR) {
            *out = s;
            break;
        }
        cmosh_close_socket(s);
    }

    freeaddrinfo(res);

    if (!cmosh_socket_valid(*out))
        return -1;

    if (verbose)
        fprintf(stderr, "cmosh: UDP target resolved: %s:%s\n", host, service);
    return 0;
}

int cmosh_udp_check(const char *host, unsigned short port, int ipv4, int ipv6,
                    int verbose)
{
    cmosh_socket_t s;

#ifdef _WIN32
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
        return -1;
#endif

    if (cmosh_udp_open_connected(host, port, ipv4, ipv6, verbose, &s) != 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    cmosh_close_socket(s);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

int cmosh_udp_probe_encrypted(const char *host, unsigned short port, int ipv4,
                              int ipv6, const unsigned char key[16],
                              int verbose)
{
    cmosh_socket_t s;
    unsigned char packet[CMOSH_MAX_PACKET], plain[CMOSH_MAX_PACKET];
    unsigned char instruction[256], compressed[320], fragment[384], diff[64];
    unsigned char decompressed[512];
    unsigned char host_output[1024];
    size_t packet_len = 0, plain_len = 0;
    size_t instruction_len = 0, compressed_len = 0, fragment_len = 0;
    size_t diff_len = 0;
    uint64_t seq = 0;
    struct cmosh_transport_instruction ti;
    struct cmosh_fragment frag;
    int rc = -1;
    fd_set rfds;
    struct timeval tv;

#ifdef _WIN32
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
        return -1;
#endif

    if (cmosh_udp_open_connected(host, port, ipv4, ipv6, 0, &s) != 0)
        goto out_wsa;

    memset(&ti, 0, sizeof(ti));
    ti.protocol_version = CMOSH_PROTOCOL_VERSION;
    ti.new_num = 1;
    if (cmosh_encode_user_resize_message(80, 24, diff, sizeof(diff),
                                         &diff_len) != 0)
        goto out_socket;
    ti.diff = diff;
    ti.diff_len = diff_len;
    if (cmosh_encode_transport_instruction(&ti, instruction,
                                           sizeof(instruction),
                                           &instruction_len) != 0)
        goto out_socket;
    if (cmosh_zlib_store_compress(instruction, instruction_len, compressed,
                                  sizeof(compressed), &compressed_len) != 0)
        goto out_socket;
    if (cmosh_encode_fragment(1, 0, 1, compressed, compressed_len, fragment,
                              sizeof(fragment), &fragment_len) != 0)
        goto out_socket;

    if (fragment_len + 4 > sizeof(plain))
        goto out_socket;
    {
        unsigned int ts = cmosh_now16_ms();
        plain[0] = (unsigned char)(ts >> 8);
        plain[1] = (unsigned char)ts;
    }
    plain[2] = 0;
    plain[3] = 0;
    memcpy(plain + 4, fragment, fragment_len);
    plain_len = fragment_len + 4;

    if (cmosh_transport_encrypt_packet(key, CMOSH_CLIENT_NONCE_BASE | 1, plain,
                                       plain_len, packet, sizeof(packet),
                                       &packet_len) != 0)
        goto out_socket;

    {
        int attempt;
        for (attempt = 0; attempt < 5; attempt++) {
            if (send(s, (const char *)packet, (int)packet_len, 0) ==
                SOCKET_ERROR)
                goto out_socket;
            if (verbose)
                fprintf(stderr,
                        "cmosh: sent encrypted association probe #%d "
                        "(%u bytes, %u byte instruction, %u byte compressed "
                        "fragment, %u byte diff)\n",
                        attempt + 1, (unsigned)packet_len,
                        (unsigned)instruction_len, (unsigned)fragment_len,
                        (unsigned)diff_len);

            FD_ZERO(&rfds);
            FD_SET(s, &rfds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            if (select((int)(s + 1), &rfds, NULL, NULL, &tv) > 0)
                break;
        }
        if (attempt == 5) {
            if (verbose)
                fputs("cmosh: no authenticated UDP response to probe yet\n",
                      stderr);
            rc = 1;
            goto out_socket;
        }
    }

    packet_len = (size_t)recv(s, (char *)packet, sizeof(packet), 0);
    if (packet_len == (size_t)SOCKET_ERROR)
        goto out_socket;
    if (cmosh_transport_decrypt_packet(key, packet, packet_len, plain,
                                       sizeof(plain), &plain_len, &seq) != 0) {
        if (verbose)
            fputs("cmosh: UDP response failed authentication\n", stderr);
        goto out_socket;
    }

    if (plain_len < 4) {
        if (verbose)
            fputs("cmosh: UDP response was authenticated but too short\n",
                  stderr);
        goto out_socket;
    }

    if (cmosh_decode_fragment(plain + 4, plain_len - 4, &frag) != 0) {
        if (verbose)
            fputs("cmosh: UDP response was authenticated but not a fragment\n",
                  stderr);
        goto out_socket;
    }

    if (cmosh_zlib_store_decompress(frag.payload, frag.payload_len,
                                    decompressed, sizeof(decompressed),
                                    &plain_len) != 0) {
        if (verbose) {
            fputs("cmosh: authenticated fragment used unsupported zlib form\n",
                  stderr);
            cmosh_dump_hex(stderr, "authenticated compressed payload",
                           frag.payload, frag.payload_len);
        }
        goto out_socket;
    }

    if (cmosh_decode_transport_instruction(decompressed, plain_len, &ti, NULL,
                                           NULL) != 0) {
        if (verbose)
            fputs("cmosh: authenticated fragment did not decode as transport "
                  "instruction\n",
                  stderr);
        goto out_socket;
    }

    if (verbose)
        fprintf(stderr,
                "cmosh: received authenticated UDP packet seq=%llu "
                "timestamp=%u echo=%u fragment_id=%llu fragment=%u/%u "
                "payload=%u bytes; transport version=%u old=%llu new=%llu "
                "ack=%llu diff=%u bytes\n",
                (unsigned long long)seq,
                ((unsigned)plain[0] << 8) | plain[1],
                ((unsigned)plain[2] << 8) | plain[3],
                (unsigned long long)frag.id,
                frag.index, frag.final, (unsigned)frag.payload_len,
                ti.protocol_version, (unsigned long long)ti.old_num,
                (unsigned long long)ti.new_num, (unsigned long long)ti.ack_num,
                (unsigned)ti.diff_len);

    if (ti.new_num) {
        unsigned int remote_ts = ((unsigned)plain[0] << 8) | plain[1];
        unsigned int ts = cmosh_now16_ms();

        memset(&ti, 0, sizeof(ti));
        ti.protocol_version = CMOSH_PROTOCOL_VERSION;
        ti.old_num = 1;
        ti.new_num = 1;
        ti.ack_num = 1;
        if (cmosh_encode_transport_instruction(&ti, instruction,
                                               sizeof(instruction),
                                               &instruction_len) != 0)
            goto out_socket;
        if (cmosh_zlib_store_compress(instruction, instruction_len,
                                      compressed, sizeof(compressed),
                                      &compressed_len) != 0)
            goto out_socket;
        if (cmosh_encode_fragment(2, 0, 1, compressed, compressed_len,
                                  fragment, sizeof(fragment),
                                  &fragment_len) != 0)
            goto out_socket;
        if (fragment_len + 4 > sizeof(plain))
            goto out_socket;
        plain[0] = (unsigned char)(ts >> 8);
        plain[1] = (unsigned char)ts;
        plain[2] = (unsigned char)(remote_ts >> 8);
        plain[3] = (unsigned char)remote_ts;
        memcpy(plain + 4, fragment, fragment_len);
        plain_len = fragment_len + 4;
        if (cmosh_transport_encrypt_packet(key, CMOSH_CLIENT_NONCE_BASE | 2,
                                           plain, plain_len, packet,
                                           sizeof(packet), &packet_len) != 0)
            goto out_socket;
        if (send(s, (const char *)packet, (int)packet_len, 0) == SOCKET_ERROR)
            goto out_socket;
        if (verbose)
            fprintf(stderr,
                    "cmosh: sent authenticated ACK for server state %llu "
                    "(%u bytes)\n",
                    (unsigned long long)ti.ack_num, (unsigned)packet_len);

        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (select((int)(s + 1), &rfds, NULL, NULL, &tv) > 0) {
            packet_len = (size_t)recv(s, (char *)packet, sizeof(packet), 0);
            if (packet_len != (size_t)SOCKET_ERROR &&
                cmosh_transport_decrypt_packet(key, packet, packet_len, plain,
                                               sizeof(plain), &plain_len,
                                               &seq) == 0) {
                if (plain_len >= 4 &&
                    cmosh_decode_fragment(plain + 4, plain_len - 4, &frag) ==
                        0 &&
                    cmosh_zlib_store_decompress(frag.payload,
                                                frag.payload_len,
                                                decompressed,
                                                sizeof(decompressed),
                                                &plain_len) == 0 &&
                    cmosh_decode_transport_instruction(decompressed,
                                                       plain_len, &ti, NULL,
                                                       NULL) == 0) {
                    if (verbose)
                        fprintf(stderr,
                                "cmosh: received post-ACK transport packet "
                                "seq=%llu timestamp=%u echo=%u version=%u "
                                "old=%llu new=%llu ack=%llu diff=%u bytes\n",
                                (unsigned long long)seq,
                                ((unsigned)plain[0] << 8) | plain[1],
                                ((unsigned)plain[2] << 8) | plain[3],
                                ti.protocol_version,
                                (unsigned long long)ti.old_num,
                                (unsigned long long)ti.new_num,
                                (unsigned long long)ti.ack_num,
                                (unsigned)ti.diff_len);
                    if (verbose && ti.diff_len)
                        cmosh_dump_hex(stderr, "post-ACK diff", ti.diff,
                                       ti.diff_len);
                    if (ti.diff_len &&
                        cmosh_decode_host_output(ti.diff, ti.diff_len,
                                                 host_output,
                                                 sizeof(host_output),
                                                 &plain_len) == 0 &&
                        plain_len) {
                        fwrite(host_output, 1, plain_len, stdout);
                        fflush(stdout);
                    }
                    {
                        uint64_t client_state = 1, server_state = ti.new_num;
                        uint64_t send_seq = 3;
                        unsigned int echo_ts =
                            ((unsigned)plain[0] << 8) | plain[1];
                        int idle = 0;

                        for (;;) {
                            unsigned char keys[256], diffbuf[512];
                            size_t key_len = 0, loop_diff_len = 0;
                            int sent = 0;

                            cmosh_console_read(keys, sizeof(keys), &key_len);
                            if (key_len &&
                                cmosh_contains_exit_key(keys, key_len)) {
                                rc = 0;
                                goto out_socket;
                            }
                            if (key_len) {
                                client_state++;
                                if (cmosh_encode_user_keystroke_message(
                                        keys, key_len, diffbuf,
                                        sizeof(diffbuf), &loop_diff_len) !=
                                    0)
                                    goto out_socket;
                                if (cmosh_make_packet(
                                        key, send_seq++, client_state - 1,
                                        client_state, server_state, diffbuf,
                                        loop_diff_len, echo_ts, packet,
                                        sizeof(packet), &packet_len) != 0)
                                    goto out_socket;
                                if (send(s, (const char *)packet,
                                         (int)packet_len, 0) == SOCKET_ERROR)
                                    goto out_socket;
                                sent = 1;
                                if (verbose)
                                    fprintf(stderr,
                                            "cmosh: sent %u input byte(s), "
                                            "client state=%llu\n",
                                            (unsigned)key_len,
                                            (unsigned long long)client_state);
                            }

                            FD_ZERO(&rfds);
                            FD_SET(s, &rfds);
                            tv.tv_sec = 0;
                            tv.tv_usec = 100000;
                            if (select((int)(s + 1), &rfds, NULL, NULL,
                                       &tv) > 0) {
                                idle = 0;
                                packet_len = (size_t)recv(
                                    s, (char *)packet, sizeof(packet), 0);
                                if (packet_len != (size_t)SOCKET_ERROR &&
                                    cmosh_decode_packet(key, packet,
                                                        packet_len, &ti,
                                                        &echo_ts, &seq) == 0) {
                                    if (ti.new_num > server_state &&
                                        ti.diff_len &&
                                        cmosh_decode_host_output(
                                            ti.diff, ti.diff_len, host_output,
                                            sizeof(host_output), &plain_len) ==
                                            0 &&
                                        plain_len) {
                                        fwrite(host_output, 1, plain_len,
                                               stdout);
                                        fflush(stdout);
                                    }
                                    if (ti.new_num > server_state)
                                        server_state = ti.new_num;
                                    if (verbose)
                                        fprintf(stderr,
                                                "cmosh: loop recv seq=%llu "
                                                "old=%llu new=%llu ack=%llu "
                                                "diff=%u\n",
                                                (unsigned long long)seq,
                                                (unsigned long long)ti.old_num,
                                                (unsigned long long)ti.new_num,
                                                (unsigned long long)ti.ack_num,
                                                (unsigned)ti.diff_len);
                                    if (ti.new_num == server_state) {
                                        if (cmosh_make_packet(
                                                key, send_seq++, client_state,
                                                client_state, server_state,
                                                NULL, 0, echo_ts, packet,
                                                sizeof(packet), &packet_len) !=
                                            0)
                                            goto out_socket;
                                        if (send(s, (const char *)packet,
                                                 (int)packet_len, 0) ==
                                            SOCKET_ERROR)
                                            goto out_socket;
                                    }
                                }
                            } else if (!sent && ++idle % 20 == 0) {
                                if (cmosh_make_packet(
                                        key, send_seq++, client_state,
                                        client_state, server_state, NULL, 0,
                                        echo_ts, packet, sizeof(packet),
                                        &packet_len) != 0)
                                    goto out_socket;
                                if (send(s, (const char *)packet,
                                         (int)packet_len, 0) == SOCKET_ERROR)
                                    goto out_socket;
                                if (verbose)
                                    fprintf(stderr,
                                            "cmosh: sent keepalive ack=%llu\n",
                                            (unsigned long long)server_state);
                            }
                        }
                    }
                } else {
                    if (verbose)
                        fprintf(stderr,
                                "cmosh: received post-ACK authenticated "
                                "packet seq=%llu payload=%u bytes\n",
                                (unsigned long long)seq, (unsigned)plain_len);
                }
            }
        } else if (verbose) {
            fputs("cmosh: no additional UDP response after ACK yet\n", stderr);
        }
    }
    rc = 0;

out_socket:
    cmosh_close_socket(s);
out_wsa:
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}
