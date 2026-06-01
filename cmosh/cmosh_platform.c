#include "cmosh_platform.h"

#include "cmosh_client.h"
#include "cmosh_proto.h"
#include "cmosh_session.h"
#include "cmosh_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif
#define popen _popen
#define pclose _pclose
#define cmosh_close_socket closesocket
typedef SOCKET cmosh_socket_t;
static DWORD saved_output_mode;
static DWORD saved_input_mode;
static int have_saved_output_mode;
static int have_saved_input_mode;
#else
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define cmosh_close_socket close
typedef int cmosh_socket_t;
static struct termios saved_termios;
static int have_saved_termios;
#endif

static int cmosh_socket_valid(cmosh_socket_t s)
{
    return s != INVALID_SOCKET;
}

void cmosh_console_setup(void)
{
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;

    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        saved_output_mode = mode;
        have_saved_output_mode = 1;
        mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                DISABLE_NEWLINE_AUTO_RETURN;
        SetConsoleMode(out, mode);
    }
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        saved_input_mode = mode;
        have_saved_input_mode = 1;
        mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                  ENABLE_PROCESSED_INPUT);
        SetConsoleMode(in, mode);
    }
#else
    if (isatty(0) && tcgetattr(0, &saved_termios) == 0) {
        struct termios raw = saved_termios;

        have_saved_termios = 1;
        raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= (tcflag_t)~OPOST;
        raw.c_cflag |= CS8;
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSAFLUSH, &raw);
    }
#endif
}

static void cmosh_console_restore(void)
{
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);

    if (have_saved_output_mode && out != INVALID_HANDLE_VALUE)
        SetConsoleMode(out, saved_output_mode);
    if (have_saved_input_mode && in != INVALID_HANDLE_VALUE)
        SetConsoleMode(in, saved_input_mode);
#else
    if (have_saved_termios)
        tcsetattr(0, TCSAFLUSH, &saved_termios);
#endif
}

static void cmosh_terminal_soft_reset(void)
{
    static const char reset[] =
        "\033[?25h"     /* show cursor */
        "\033[?1000l"   /* mouse reporting off */
        "\033[?1002l"
        "\033[?1003l"
        "\033[?1006l"
        "\033[?2004l"   /* bracketed paste off */
        "\033[?1049l"   /* leave alternate screen */
        "\033[?1047l"
        "\033[?47l"
        "\033[0m";      /* reset attributes */

    fwrite(reset, 1, sizeof(reset) - 1, stdout);
    fflush(stdout);
}

static void cmosh_terminal_session_start(void)
{
    static const char init[] =
        "\033[0m"
        "\033[?25h"
        "\033[2J"
        "\033[H";

    fwrite(init, 1, sizeof(init) - 1, stdout);
    fflush(stdout);
}

static void cmosh_dump_hex(FILE *fp, const char *label,
                           const unsigned char *data, size_t len);

struct cmosh_render_ctx {
    int verbose;
    int wrote;
};

static int cmosh_render_host_output(void *vctx, const unsigned char *data,
                                    size_t len)
{
    struct cmosh_render_ctx *ctx = (struct cmosh_render_ctx *)vctx;

    if (ctx->verbose)
        cmosh_dump_hex(stderr, "decoded host bytes", data, len);
    if (len) {
        fwrite(data, 1, len, stdout);
        fflush(stdout);
        ctx->wrote = 1;
    }
    return 0;
}

static int cmosh_decode_and_render_host(const unsigned char *diff,
                                        size_t diff_len,
                                        unsigned char *host_output,
                                        size_t host_output_len, int verbose)
{
    struct cmosh_render_ctx ctx;

    (void)host_output;
    (void)host_output_len;

    if (!diff_len)
        return 0;
    if (verbose)
        cmosh_dump_hex(stderr, "loop host diff", diff, diff_len);
    ctx.verbose = verbose;
    ctx.wrote = 0;
    if (cmosh_decode_host_output_cb(diff, diff_len, cmosh_render_host_output,
                                    &ctx) != 0)
        return -1;
    if (!ctx.wrote && verbose) {
        cmosh_dump_hex(stderr, "undecoded loop host diff", diff, diff_len);
    }
    return 0;
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

static uint64_t cmosh_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000U) + (uint64_t)(tv.tv_usec / 1000U);
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
        if (c == 0 || c == 0xe0) {
            static const struct {
                int code;
                const char *seq;
            } keys[] = {
                { 0x47, "\033[1~" }, { 0x48, "\033[A" },
                { 0x49, "\033[5~" }, { 0x4b, "\033[D" },
                { 0x4d, "\033[C" },  { 0x4f, "\033[4~" },
                { 0x50, "\033[B" },  { 0x51, "\033[6~" },
                { 0x52, "\033[2~" }, { 0x53, "\033[3~" },
                { 0x3b, "\033OP" },  { 0x3c, "\033OQ" },
                { 0x3d, "\033OR" },  { 0x3e, "\033OS" },
                { 0x3f, "\033[15~" }, { 0x40, "\033[17~" },
                { 0x41, "\033[18~" }, { 0x42, "\033[19~" },
                { 0x43, "\033[20~" }, { 0x44, "\033[21~" },
                { 0x85, "\033[23~" }, { 0x86, "\033[24~" },
            };
            int ext = _getch();
            size_t i;

            for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                if (keys[i].code == ext) {
                    size_t n = strlen(keys[i].seq);
                    if (*len + n <= buflen) {
                        memcpy(buf + *len, keys[i].seq, n);
                        *len += n;
                    }
                    break;
                }
            }
            continue;
        }
        if (c == '\b')
            c = 0x7f;
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

static void cmosh_console_size(unsigned int *cols, unsigned int *rows)
{
    *cols = 80;
    *rows = 24;
#ifdef _WIN32
    {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;

        if (out != INVALID_HANDLE_VALUE &&
            GetConsoleScreenBufferInfo(out, &csbi)) {
            *cols = (unsigned int)(csbi.srWindow.Right -
                                   csbi.srWindow.Left + 1);
            *rows = (unsigned int)(csbi.srWindow.Bottom -
                                   csbi.srWindow.Top + 1);
        }
    }
#else
    {
        struct winsize ws;

        if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
            *cols = ws.ws_col;
            *rows = ws.ws_row;
        }
    }
#endif
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

struct cmosh_host_output_ctx {
    unsigned char *host_output;
    size_t host_output_len;
    int verbose;
};

static int cmosh_host_output_callback(void *vctx, const unsigned char *diff,
                                      size_t diff_len)
{
    struct cmosh_host_output_ctx *ctx =
        (struct cmosh_host_output_ctx *)vctx;

    return cmosh_decode_and_render_host(diff, diff_len, ctx->host_output,
                                        ctx->host_output_len, ctx->verbose);
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
    unsigned char packet[CMOSH_MAX_PACKET], initial_diff[CMOSH_SERVER_DIFF_MAX];
    unsigned char host_output[8192];
    size_t packet_len = 0;
    size_t diff_len = 0;
    uint64_t seq = 0;
    struct cmosh_transport_instruction ti;
    unsigned int initial_ts = 0, initial_echo_ts = 0;
    unsigned int cols = 80, rows = 24;
    int rc = -1;
    int console_started = 0;
    fd_set rfds;
    struct timeval tv;

#ifdef _WIN32
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
        return -1;
#endif

    if (cmosh_udp_open_connected(host, port, ipv4, ipv6, 0, &s) != 0)
        goto out_wsa;

    cmosh_console_setup();
    console_started = 1;

    cmosh_console_size(&cols, &rows);
    if (cmosh_client_make_initial_packet(key, cols, rows, cmosh_now16_ms(),
                                         packet, sizeof(packet), &packet_len,
                                         &diff_len) != 0)
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
                        "(%u bytes, %u byte diff)\n",
                        attempt + 1, (unsigned)packet_len,
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
    if (cmosh_transport_decode_packet(key, packet, packet_len, &ti,
                                      initial_diff, sizeof(initial_diff),
                                      &initial_ts, &initial_echo_ts,
                                      &seq) != 0) {
        if (verbose)
            fputs("cmosh: UDP response failed authentication or transport "
                  "decode\n",
                  stderr);
        goto out_socket;
    }

    if (verbose)
        fprintf(stderr,
                "cmosh: received authenticated UDP packet seq=%llu "
                "timestamp=%u echo=%u transport version=%u old=%llu new=%llu "
                "ack=%llu diff=%u bytes\n",
                (unsigned long long)seq, initial_ts, initial_echo_ts,
                ti.protocol_version, (unsigned long long)ti.old_num,
                (unsigned long long)ti.new_num, (unsigned long long)ti.ack_num,
                (unsigned)ti.diff_len);

    if (ti.new_num) {
        unsigned int remote_ts = initial_ts;
        unsigned char post_ack_diff[CMOSH_SERVER_DIFF_MAX];
        unsigned int post_ack_ts, post_ack_echo_ts;

        if (cmosh_client_make_start_ack(key, remote_ts, cmosh_now16_ms(),
                                        packet, sizeof(packet), &packet_len) !=
            0)
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
                cmosh_transport_decode_packet(
                    key, packet, packet_len, &ti, post_ack_diff,
                    sizeof(post_ack_diff), &post_ack_ts, &post_ack_echo_ts,
                    &seq) == 0) {
                    if (verbose)
                        fprintf(stderr,
                                "cmosh: received post-ACK transport packet "
                                "seq=%llu timestamp=%u echo=%u version=%u "
                                "old=%llu new=%llu ack=%llu diff=%u bytes\n",
                                (unsigned long long)seq,
                                post_ack_ts, post_ack_echo_ts,
                                ti.protocol_version,
                                (unsigned long long)ti.old_num,
                                (unsigned long long)ti.new_num,
                                (unsigned long long)ti.ack_num,
                                (unsigned)ti.diff_len);
                    if (verbose && ti.diff_len)
                        cmosh_dump_hex(stderr, "post-ACK diff", ti.diff,
                                       ti.diff_len);
                    if (!verbose)
                        cmosh_terminal_session_start();
                    if (cmosh_decode_and_render_host(
                            ti.diff, ti.diff_len, host_output,
                            sizeof(host_output), verbose) != 0)
                        goto out_socket;
                    {
                        struct cmosh_client client;
                        unsigned int last_cols = cols, last_rows = rows;
                        int idle = 0;

                        cmosh_client_init(&client, key, ti.ack_num,
                                          ti.new_num, seq, post_ack_ts, 3);
                        cmosh_client_note_recv_time(&client, cmosh_now_ms());
                        for (;;) {
                            unsigned char keys[256];
                            unsigned char incoming_diff[CMOSH_SERVER_DIFF_MAX];
                            size_t key_len = 0;
                            int sent = 0;
                            uint64_t now_ms = cmosh_now_ms();
                            unsigned int cur_cols, cur_rows;

                            cmosh_console_size(&cur_cols, &cur_rows);
                            if ((cur_cols != last_cols ||
                                 cur_rows != last_rows)) {
                                if (cmosh_client_make_resize(
                                        &client, cur_cols, cur_rows,
                                        now_ms, cmosh_now16_ms(), packet,
                                        sizeof(packet), &packet_len) != 0)
                                    goto out_socket;
                                if (send(s, (const char *)packet,
                                         (int)packet_len, 0) == SOCKET_ERROR)
                                    goto out_socket;
                                last_cols = cur_cols;
                                last_rows = cur_rows;
                                sent = 1;
                                if (verbose)
                                    fprintf(stderr,
                                            "cmosh: sent resize %ux%u "
                                            "client state=%llu\n",
                                            cur_cols, cur_rows,
                                            (unsigned long long)
                                                client.input.current);
                            }

                            cmosh_console_read(keys, sizeof(keys), &key_len);
                            if (key_len &&
                                cmosh_contains_exit_key(keys, key_len)) {
                                if (verbose)
                                    fputs("cmosh: local disconnect key\n",
                                          stderr);
                                rc = 0;
                                goto out_socket;
                            }
                            if (key_len) {
                                if (cmosh_client_make_input(
                                        &client, keys, key_len, now_ms,
                                        cmosh_now16_ms(), packet,
                                        sizeof(packet), &packet_len) != 0)
                                    goto out_socket;
                                if (send(s, (const char *)packet,
                                         (int)packet_len, 0) == SOCKET_ERROR)
                                    goto out_socket;
                                sent = 1;
                                if (verbose) {
                                    fprintf(stderr,
                                            "cmosh: sent %u input byte(s), "
                                            "client state=%llu acked=%llu\n",
                                            (unsigned)key_len,
                                            (unsigned long long)
                                                client.input.current,
                                            (unsigned long long)
                                                client.input.acked);
                                    cmosh_dump_hex(stderr, "input bytes", keys,
                                                   key_len);
                                }
                            }

                            for (;;) {
                                FD_ZERO(&rfds);
                                FD_SET(s, &rfds);
                                tv.tv_sec = 0;
                                tv.tv_usec = sent ? 0 : 100000;
                                if (select((int)(s + 1), &rfds, NULL, NULL,
                                           &tv) <= 0)
                                    break;
                                idle = 0;
                                packet_len = (size_t)recv(
                                    s, (char *)packet, sizeof(packet), 0);
                                if (packet_len != (size_t)SOCKET_ERROR) {
                                    struct cmosh_host_output_ctx output_ctx;
                                    struct cmosh_client_recv_event event;

                                    output_ctx.host_output = host_output;
                                    output_ctx.host_output_len =
                                        sizeof(host_output);
                                    output_ctx.verbose = verbose;

                                    if (cmosh_client_process_packet(
                                            &client, packet, packet_len,
                                            incoming_diff,
                                            sizeof(incoming_diff),
                                            cmosh_host_output_callback,
                                            &output_ctx, &event) ==
                                        CMOSH_CLIENT_RECV_BAD_PACKET)
                                        continue;
                                    seq = event.seq;
                                    if (event.result ==
                                        CMOSH_CLIENT_RECV_DUPLICATE) {
                                        cmosh_client_note_recv_time(
                                            &client, cmosh_now_ms());
                                        if (verbose)
                                            fprintf(stderr,
                                                    "cmosh: ignored duplicate "
                                                    "server packet seq=%llu\n",
                                                    (unsigned long long)seq);
                                        if (cmosh_client_make_ack(
                                                &client, cmosh_now16_ms(),
                                                packet, sizeof(packet),
                                                &packet_len) != 0)
                                            goto out_socket;
                                        if (send(s, (const char *)packet,
                                                 (int)packet_len, 0) ==
                                            SOCKET_ERROR)
                                            goto out_socket;
                                        continue;
                                    }
                                    if (event.result ==
                                        CMOSH_CLIENT_RECV_PENDING) {
                                        cmosh_client_note_recv_time(
                                            &client, cmosh_now_ms());
                                        if (verbose)
                                            fprintf(stderr,
                                                    "cmosh: received partial "
                                                    "fragment seq=%llu\n",
                                                    (unsigned long long)seq);
                                        continue;
                                    }
                                    cmosh_client_note_recv_time(
                                        &client, cmosh_now_ms());
                                    if (event.queued_future && verbose)
                                        fprintf(stderr,
                                                "cmosh: queued future "
                                                "server diff old=%llu "
                                                "new=%llu current=%llu\n",
                                                (unsigned long long)
                                                    event.old_num,
                                                (unsigned long long)
                                                    event.new_num,
                                                (unsigned long long)
                                                    event.previous_server_state);
                                    if (verbose)
                                        fprintf(stderr,
                                                "cmosh: loop recv seq=%llu "
                                                "old=%llu new=%llu ack=%llu "
                                                "diff=%u\n",
                                                (unsigned long long)seq,
                                                (unsigned long long)
                                                    event.old_num,
                                                (unsigned long long)
                                                    event.new_num,
                                                (unsigned long long)
                                                    event.ack_num,
                                                (unsigned)event.diff_len);
                                    if (event.server_shutdown) {
                                        if (verbose)
                                            fputs("cmosh: server shutdown\n",
                                                  stderr);
                                        rc = 0;
                                        goto out_socket;
                                    }
                                    if (event.should_ack) {
                                        if (cmosh_client_make_ack(
                                                &client, cmosh_now16_ms(),
                                                packet, sizeof(packet),
                                                &packet_len) != 0)
                                            goto out_socket;
                                        if (send(s, (const char *)packet,
                                                 (int)packet_len, 0) ==
                                            SOCKET_ERROR)
                                            goto out_socket;
                                    }
                                }
                            }
                            if (!sent && ++idle % 20 == 0) {
                                struct cmosh_client_idle_event idle_event;

                                now_ms = cmosh_now_ms();
                                if (cmosh_client_make_idle_event(
                                        &client, now_ms, cmosh_now16_ms(),
                                        packet, sizeof(packet), &packet_len,
                                        &idle_event) != 0)
                                    goto out_socket;
                                if (idle_event.missing_state) {
                                    if (verbose)
                                        fprintf(stderr,
                                                "cmosh: waiting for missing "
                                                "server state current=%llu "
                                                "needed_old=%llu queued_new="
                                                "%llu\n",
                                                (unsigned long long)
                                                    client.server_state,
                                                (unsigned long long)
                                                    idle_event.gap_old_num,
                                                (unsigned long long)
                                                    idle_event.gap_new_num);
                                }
                                if (idle_event.udp_timeout) {
                                    fprintf(stderr,
                                            "cmosh: UDP timeout; no server "
                                            "packet for %u seconds\n",
                                            (unsigned)
                                                (CMOSH_CLIENT_UDP_TIMEOUT_DIAG_MS /
                                                 1000U));
                                }
                                if (packet_len &&
                                    send(s, (const char *)packet,
                                         (int)packet_len, 0) == SOCKET_ERROR)
                                    goto out_socket;
                                if (packet_len && verbose) {
                                    fprintf(stderr,
                                            "cmosh: sent keepalive ack=%llu "
                                            "client state=%llu retry=%u "
                                            "retry_state=%llu queued=%u "
                                            "bytes=%u\n",
                                            (unsigned long long)
                                                client.server_state,
                                            (unsigned long long)
                                                client.input.current,
                                            idle_event.retransmitted,
                                            (unsigned long long)
                                                idle_event.retransmit_state,
                                            (unsigned)idle_event.input_records,
                                            (unsigned)idle_event.input_bytes);
                                }
                            }
                        }
                    }
            }
        } else if (verbose) {
            fputs("cmosh: no additional UDP response after ACK yet\n", stderr);
        }
    }
    rc = 0;

out_socket:
    if (console_started) {
        cmosh_terminal_soft_reset();
        cmosh_console_restore();
    }
    cmosh_close_socket(s);
out_wsa:
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}
