/*
 * Native Mosh backend scaffold.
 *
 * This backend is deliberately not functional yet. It exists so PuTTY can
 * advertise and persist the protocol identity while the reusable cmosh client,
 * SSH bootstrap wrapper, and UDP event adapter are built up underneath it.
 */

#include "putty.h"
#include "cmosh_bootstrap.h"
#include "cmosh_client.h"
#include "cmosh_proto.h"
#include "cmosh_terminal.h"
#include "cmosh_transport.h"

#include <stdio.h>
#include <string.h>

#define MOSH_ASSOC_RETRY_MS 1000U
#define MOSH_START_ACK_RETRY_MS 250U
#define MOSH_UDP_REOPEN_RETRY_MS 1000U
#define MOSH_UDP_SEND_FAIL_LOG_MS 5000U
#define MOSH_BOOTSTRAP_OUTPUT_MAX 65536U

typedef struct Mosh Mosh;

struct Mosh {
    Backend backend;
    Interactor interactor;
    Seat bootstrap_seat;
    Plug udp_plug;
    Seat *seat;
    Conf *conf;
    Conf *ssh_conf;
    LogContext *logctx;
    Backend *ssh_backend;
    Socket *udp_socket;
    struct cmosh_client client;
    struct cmosh_terminal *terminal;
    struct cmosh_transport_state bootstrap_transport;
    unsigned char initial_packet[CMOSH_MAX_PACKET];
    size_t initial_packet_len;
    unsigned char start_ack_packet[CMOSH_MAX_PACKET];
    size_t start_ack_packet_len;
    strbuf *bootstrap_output;
    strbuf *pending_input;
    struct cmosh_bootstrap bootstrap;
    Ldisc *ldisc;
    char udp_host[256];
    char *description;
    char *bootstrap_command;
    char *ssh_host;
    unsigned int cols, rows;
    size_t pending_resize_after_input_len;
    int exitcode;
    uint64_t last_assoc_probe_ms;
    uint64_t last_input_diag_ms;
    uint64_t last_recv_diag_ms;
    uint64_t last_start_ack_retry_ms;
    uint64_t last_udp_send_fail_log_ms;
    uint64_t last_udp_reopen_attempt_ms;
    bool udp_started;
    bool udp_start_queued;
    bool udp_reopen_queued;
    bool udp_ready;
    bool ssh_disconnected;
    bool remote_exited;
    bool shutdown;
    bool udp_target_ready;
    bool pending_start_ack;
    bool pending_resize;
    bool logged_state_only_output;
};

static void mosh_free(Backend *be);
static void mosh_timer(void *ctx, unsigned long now);
static bool mosh_open_udp_socket(Mosh *mosh, bool fatal_on_error);
static bool mosh_reopen_udp_socket(Mosh *mosh);
static void mosh_queue_udp_reopen(Mosh *mosh);
static bool mosh_start_udp(Mosh *mosh);
static void mosh_start_udp_callback(void *ctx);
static bool mosh_try_send_start_ack(Mosh *mosh, unsigned long now);
static bool mosh_try_send_resize(Mosh *mosh, uint64_t now);
static void mosh_try_send_pending(Mosh *mosh);
static bool mosh_sendok(Backend *be);

static bool mosh_interval_due(uint64_t last_ms, unsigned long now_ms,
                              unsigned interval_ms)
{
    uint64_t now64 = (uint64_t)now_ms;

    if (!last_ms)
        return true;
    if (now64 >= last_ms)
        return now64 - last_ms >= interval_ms;
    if (sizeof(unsigned long) == 4 && last_ms <= UINT32_MAX &&
        last_ms - now64 > UINT32_MAX / 2)
        return (uint64_t)(uint32_t)((uint32_t)now_ms -
                                    (uint32_t)last_ms) >= interval_ms;
    return false;
}

static char *mosh_description(Interactor *itr)
{
    Mosh *mosh = container_of(itr, Mosh, interactor);
    return dupstr(mosh->description);
}

static LogPolicy *mosh_logpolicy(Interactor *itr)
{
    Mosh *mosh = container_of(itr, Mosh, interactor);
    return log_get_policy(mosh->logctx);
}

static Seat *mosh_get_seat(Interactor *itr)
{
    Mosh *mosh = container_of(itr, Mosh, interactor);
    return mosh->seat;
}

static void mosh_set_seat(Interactor *itr, Seat *seat)
{
    Mosh *mosh = container_of(itr, Mosh, interactor);
    mosh->seat = seat;
}

static const InteractorVtable Mosh_interactorvt = {
    .description = mosh_description,
    .logpolicy = mosh_logpolicy,
    .get_seat = mosh_get_seat,
    .set_seat = mosh_set_seat,
};

static inline InteractionReadySeat mosh_wrap_real_seat(Mosh *mosh)
{
    InteractionReadySeat iseat;
    iseat.seat = mosh->seat;
    return iseat;
}

static char *mosh_build_remote_command(const char *server, const char *locale)
{
    char command[1024];

    if (cmosh_build_remote_command(server, NULL, locale, 0, 0, 0,
                                   command, sizeof(command)) != 0)
        return NULL;
    return dupstr(command);
}

static Conf *mosh_make_ssh_conf(Conf *base_conf, int port,
                                const char *remote_command)
{
    Conf *ssh_conf = conf_copy(base_conf);

    conf_set_int(ssh_conf, CONF_protocol, PROT_SSH);
    conf_set_int(ssh_conf, CONF_port, port);
    conf_set_str(ssh_conf, CONF_remote_cmd, remote_command);
    conf_set_str(ssh_conf, CONF_remote_cmd2, "");
    conf_set_bool(ssh_conf, CONF_nopty, true);
    conf_set_bool(ssh_conf, CONF_ssh_subsys, false);
    conf_set_bool(ssh_conf, CONF_ssh_no_shell, false);
    conf_set_bool(ssh_conf, CONF_ssh_simple, true);
    conf_set_bool(ssh_conf, CONF_x11_forward, false);
    conf_set_bool(ssh_conf, CONF_agentfwd, false);
    conf_set_bool(ssh_conf, CONF_ssh_connection_sharing_upstream, false);
    for (const char *subkey;
         (subkey = conf_get_str_nthstrkey(ssh_conf, CONF_portfwd, 0)) != NULL;)
        conf_del_str_str(ssh_conf, CONF_portfwd, subkey);
    return ssh_conf;
}

static bool mosh_parse_bootstrap_output(Mosh *mosh)
{
    return cmosh_parse_startup(mosh->bootstrap_output->s,
                               &mosh->bootstrap) == 0;
}

static bool mosh_prepare_udp_target(Mosh *mosh)
{
    if (mosh->bootstrap.ip[0]) {
        size_t len = strlen(mosh->bootstrap.ip);
        if (len >= sizeof(mosh->udp_host))
            return false;
        memcpy(mosh->udp_host, mosh->bootstrap.ip, len + 1);
    } else if (cmosh_extract_network_host(mosh->ssh_host, mosh->udp_host,
                                          sizeof(mosh->udp_host)) != 0) {
        return false;
    }

    mosh->udp_target_ready = true;
    return true;
}

static unsigned int mosh_now16(unsigned long now)
{
    return (unsigned int)(now & 0xffffU);
}

struct mosh_output_ctx {
    Mosh *mosh;
    bool wrote;
};

struct mosh_terminal_apply_ctx {
    struct cmosh_terminal *terminal;
    bool visible;
};

static int mosh_seat_output(void *vctx, const unsigned char *data, size_t len)
{
    struct mosh_output_ctx *ctx = (struct mosh_output_ctx *)vctx;

    if (len) {
        seat_stdout(ctx->mosh->seat, data, len);
        ctx->wrote = true;
    }
    return 0;
}

static int mosh_terminal_hostbytes(void *vctx, const unsigned char *data,
                                   size_t len)
{
    struct mosh_terminal_apply_ctx *ctx =
        (struct mosh_terminal_apply_ctx *)vctx;

    if (len)
        ctx->visible = true;
    return cmosh_terminal_apply_bytes(ctx->terminal, data, len);
}

static int mosh_terminal_resize(void *vctx, unsigned int cols,
                                unsigned int rows)
{
    struct mosh_terminal_apply_ctx *ctx =
        (struct mosh_terminal_apply_ctx *)vctx;

    ctx->visible = true;
    return cmosh_terminal_resize(ctx->terminal, cols, rows);
}

static int mosh_host_output(void *vctx, const unsigned char *diff,
                            size_t diff_len)
{
    Mosh *mosh = (Mosh *)vctx;
    struct mosh_output_ctx ctx;
    struct mosh_terminal_apply_ctx apply_ctx;
    struct cmosh_host_apply apply;
    int ret;

    if (!diff_len)
        return 0;
    ctx.mosh = mosh;
    ctx.wrote = false;
    apply_ctx.terminal = mosh->terminal;
    apply_ctx.visible = false;
    memset(&apply, 0, sizeof(apply));
    apply.hostbytes = mosh_terminal_hostbytes;
    apply.resize = mosh_terminal_resize;
    apply.ctx = &apply_ctx;
    ret = cmosh_decode_host_apply(diff, diff_len, &apply);
    if (ret == 0 && apply_ctx.visible)
        ret = cmosh_terminal_render_full(mosh->terminal, mosh_seat_output,
                                         &ctx);
    if (ret == 0 && !ctx.wrote && !mosh->logged_state_only_output) {
        logevent(mosh->logctx,
                 "Mosh server update contained no raw host-output bytes; "
                 "full terminal-state renderer is required for this screen");
        mosh->logged_state_only_output = true;
    }
    return ret;
}

static bool mosh_udp_send(Mosh *mosh, const unsigned char *packet,
                          size_t packet_len)
{
    size_t backlog;
    unsigned long now;

    if (!mosh->udp_socket || mosh->shutdown)
        return false;
    backlog = sk_write(mosh->udp_socket, packet, packet_len);
    if (backlog) {
        now = GETTICKCOUNT();
        if (mosh_interval_due(mosh->last_udp_send_fail_log_ms, now,
                              MOSH_UDP_SEND_FAIL_LOG_MS)) {
            logevent(mosh->logctx,
                     "Mosh UDP send failed locally; packet will be retried "
                     "if it carries retransmittable state");
            mosh->last_udp_send_fail_log_ms = (uint64_t)now;
        }
        return false;
    }
    return true;
}

static void mosh_send_idle(Mosh *mosh, unsigned long now)
{
    unsigned char packet[CMOSH_MAX_PACKET];
    size_t packet_len = 0;
    struct cmosh_client_idle_event event;

    if (!mosh->udp_ready || mosh->shutdown)
        return;
    if (cmosh_client_make_idle_event(&mosh->client, (uint64_t)now,
                                     mosh_now16(now), packet, sizeof(packet),
                                     &packet_len, &event) != 0)
        return;
    if (event.missing_state) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Mosh waiting for missing server state current=%llu "
                 "wanted=%llu",
                 (unsigned long long)mosh->client.server_state,
                 (unsigned long long)event.gap_new_num);
        logevent(mosh->logctx, msg);
    }
    if (event.udp_timeout) {
        logevent(mosh->logctx,
                 "Mosh UDP timeout; no server packet received recently");
        if (mosh_reopen_udp_socket(mosh))
            mosh->last_udp_reopen_attempt_ms = 0;
        else
            mosh->last_udp_reopen_attempt_ms = (uint64_t)now;
    }
    if (event.retransmitted &&
        mosh_interval_due(mosh->last_input_diag_ms, now, 5000U)) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "Mosh retransmitting input state=%llu acked=%llu "
                 "current=%llu queued=%u bytes=%u",
                 (unsigned long long)event.retransmit_state,
                 (unsigned long long)event.input_acked,
                 (unsigned long long)event.input_current,
                 (unsigned)event.input_records,
                 (unsigned)event.input_bytes);
        logevent(mosh->logctx, msg);
        mosh->last_input_diag_ms = (uint64_t)now;
    }
    if (packet_len && !mosh_udp_send(mosh, packet, packet_len)) {
        cmosh_client_note_idle_send_failed(&mosh->client);
        if (event.retransmitted)
            cmosh_client_note_input_send_failed(
                &mosh->client, event.retransmit_state, (uint64_t)now);
    }
    mosh_try_send_pending(mosh);
}

static void mosh_send_assoc_probe(Mosh *mosh, unsigned long now)
{
    if (!mosh->udp_started || mosh->udp_ready || mosh->shutdown ||
        !mosh->udp_socket || !mosh->initial_packet_len)
        return;
    if (mosh->last_assoc_probe_ms &&
        !mosh_interval_due(mosh->last_assoc_probe_ms, now,
                           MOSH_ASSOC_RETRY_MS))
        return;
    if (mosh_udp_send(mosh, mosh->initial_packet, mosh->initial_packet_len))
        mosh->last_assoc_probe_ms = (uint64_t)now;
    else
        mosh->last_assoc_probe_ms = 0;
}

static bool mosh_try_send_start_ack(Mosh *mosh, unsigned long now)
{
    if (!mosh->pending_start_ack)
        return true;
    if (!mosh->udp_socket || mosh->shutdown || !mosh->start_ack_packet_len)
        return false;
    if (mosh->last_start_ack_retry_ms &&
        !mosh_interval_due(mosh->last_start_ack_retry_ms, now,
                           MOSH_START_ACK_RETRY_MS))
        return false;
    if (!mosh_udp_send(mosh, mosh->start_ack_packet,
                       mosh->start_ack_packet_len)) {
        mosh->last_start_ack_retry_ms = (uint64_t)now;
        return false;
    }
    mosh->pending_start_ack = false;
    mosh->last_start_ack_retry_ms = (uint64_t)now;
    return true;
}

static void mosh_send_ack(Mosh *mosh, unsigned long now)
{
    unsigned char packet[CMOSH_MAX_PACKET];
    size_t packet_len = 0;

    if (!mosh->udp_ready || mosh->shutdown)
        return;
    mosh_try_send_start_ack(mosh, now);
    if (cmosh_client_make_ack(&mosh->client, mosh_now16(now), packet,
                              sizeof(packet), &packet_len) == 0 &&
        !mosh_udp_send(mosh, packet, packet_len))
        cmosh_client_note_idle_send_failed(&mosh->client);
}

static void mosh_timer(void *ctx, unsigned long now)
{
    Mosh *mosh = (Mosh *)ctx;

    if (mosh->shutdown || !mosh->udp_started)
        return;
    if (!mosh->udp_socket) {
        mosh_queue_udp_reopen(mosh);
        return;
    }

    if (mosh->last_udp_reopen_attempt_ms &&
        mosh_interval_due(mosh->last_udp_reopen_attempt_ms, now,
                          MOSH_UDP_REOPEN_RETRY_MS)) {
        if (mosh_reopen_udp_socket(mosh))
            mosh->last_udp_reopen_attempt_ms = 0;
        else
            mosh->last_udp_reopen_attempt_ms = (uint64_t)now;
    }

    mosh_send_assoc_probe(mosh, now);
    mosh_try_send_start_ack(mosh, now);
    mosh_send_idle(mosh, now);
    if (!mosh->shutdown)
        schedule_timer(100, mosh_timer, mosh);
}

static void mosh_udp_log(Plug *plug, Socket *s, PlugLogType type,
                         SockAddr *addr, int port, const char *error_msg,
                         int error_code)
{
    Mosh *mosh = container_of(plug, Mosh, udp_plug);
    backend_socket_log(mosh->seat, mosh->logctx, s, type, addr, port,
                       error_msg, error_code, mosh->conf, mosh->udp_started);
}

static void mosh_udp_closing(Plug *plug, PlugCloseType type,
                             const char *error_msg)
{
    Mosh *mosh = container_of(plug, Mosh, udp_plug);

    if (mosh->shutdown)
        return;
    if (type == PLUGCLOSE_USER_ABORT)
        return;
    if (mosh->udp_started && type != PLUGCLOSE_USER_ABORT) {
        char msg[256];
        Socket *old_socket = mosh->udp_socket;

        snprintf(msg, sizeof(msg),
                 "Mosh UDP socket closed%s%s; scheduling local reopen",
                 error_msg ? ": " : "", error_msg ? error_msg : "");
        logevent(mosh->logctx, msg);
        mosh->udp_socket = NULL;
        if (old_socket)
            sk_close(old_socket);
        mosh_queue_udp_reopen(mosh);
        return;
    }

    mosh->shutdown = true;
    if (type == PLUGCLOSE_NORMAL)
        seat_notify_remote_exit(mosh->seat);
    else if (error_msg)
        seat_connection_fatal(mosh->seat, "%s", error_msg);
    seat_notify_remote_disconnect(mosh->seat);
}

static void mosh_udp_receive(Plug *plug, int urgent, const char *data,
                             size_t len)
{
    Mosh *mosh = container_of(plug, Mosh, udp_plug);
    unsigned char diff[CMOSH_SERVER_DIFF_MAX];
    struct cmosh_transport_instruction ti;
    struct cmosh_client_recv_event event;
    unsigned int timestamp = 0, echo_timestamp = 0;
    uint64_t seq = 0;

    if (urgent || mosh->shutdown)
        return;

    if (!mosh->udp_ready) {
        size_t start_ack_packet_len = 0;
        int decode_result = cmosh_transport_decode_packet_state(
            &mosh->bootstrap_transport, mosh->bootstrap.key,
            (const unsigned char *)data, len, &ti, diff, sizeof(diff),
            &timestamp, &echo_timestamp, &seq);

        if (decode_result != 0)
            return;
        if (ti.new_num <= ti.old_num)
            return;

        if (cmosh_client_make_start_ack(
                mosh->bootstrap.key, timestamp, mosh_now16(GETTICKCOUNT()),
                mosh->start_ack_packet, sizeof(mosh->start_ack_packet),
                &start_ack_packet_len) != 0)
            return;
        mosh->start_ack_packet_len = start_ack_packet_len;
        mosh->pending_start_ack = true;
        mosh->last_start_ack_retry_ms = 0;
        mosh_try_send_start_ack(mosh, GETTICKCOUNT());
        cmosh_client_init(&mosh->client, mosh->bootstrap.key, ti.ack_num,
                          ti.new_num, seq, timestamp, 3);
        cmosh_client_note_recv_time(&mosh->client,
                                    (uint64_t)GETTICKCOUNT());
        if (ti.diff_len)
            mosh_host_output(mosh, ti.diff, ti.diff_len);
        mosh->udp_ready = true;
        seat_notify_session_started(mosh->seat);
        seat_update_specials_menu(mosh->seat);
        mosh_try_send_pending(mosh);
        if (mosh->ldisc)
            ldisc_check_sendok(mosh->ldisc);
        return;
    }

    if (cmosh_client_process_packet(&mosh->client,
                                    (const unsigned char *)data, len, diff,
                                    sizeof(diff), mosh_host_output, mosh,
                                    &event) == CMOSH_CLIENT_RECV_BAD_PACKET)
        return;
    if (event.result == CMOSH_CLIENT_RECV_PENDING) {
        cmosh_client_note_recv_time(&mosh->client, (uint64_t)GETTICKCOUNT());
        return;
    }
    if (event.result == CMOSH_CLIENT_RECV_DUPLICATE) {
        cmosh_client_note_recv_time(&mosh->client, (uint64_t)GETTICKCOUNT());
        if (event.server_shutdown) {
            mosh->shutdown = true;
            mosh->exitcode = 0;
            seat_notify_remote_exit(mosh->seat);
            seat_notify_remote_disconnect(mosh->seat);
            return;
        }
        mosh_send_ack(mosh, GETTICKCOUNT());
        mosh_try_send_pending(mosh);
        if (mosh->ldisc && mosh_sendok(&mosh->backend))
            ldisc_check_sendok(mosh->ldisc);
        return;
    }

    cmosh_client_note_recv_time(&mosh->client, (uint64_t)GETTICKCOUNT());

    if (event.server_shutdown) {
        mosh->shutdown = true;
        mosh->exitcode = 0;
        seat_notify_remote_exit(mosh->seat);
        seat_notify_remote_disconnect(mosh->seat);
        return;
    }

    if ((event.throwaway_num > event.ack_num ||
         (event.input_records_before &&
          event.input_acked_after > event.input_acked_before)) &&
        mosh_interval_due(mosh->last_recv_diag_ms, GETTICKCOUNT(), 5000U)) {
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "Mosh server input ACK ack=%llu throwaway=%llu "
                 "input_acked=%llu->%llu current=%llu queued=%u->%u "
                 "bytes=%u->%u",
                 (unsigned long long)event.ack_num,
                 (unsigned long long)event.throwaway_num,
                 (unsigned long long)event.input_acked_before,
                 (unsigned long long)event.input_acked_after,
                 (unsigned long long)event.input_current,
                 (unsigned)event.input_records_before,
                 (unsigned)event.input_records_after,
                 (unsigned)event.input_bytes_before,
                 (unsigned)event.input_bytes_after);
        logevent(mosh->logctx, msg);
        mosh->last_recv_diag_ms = (uint64_t)GETTICKCOUNT();
    }

    if (event.should_ack)
        mosh_send_ack(mosh, GETTICKCOUNT());
    mosh_try_send_pending(mosh);
    if (mosh->ldisc && mosh_sendok(&mosh->backend))
        ldisc_check_sendok(mosh->ldisc);
}

static const PlugVtable Mosh_udp_plugvt = {
    .log = mosh_udp_log,
    .closing = mosh_udp_closing,
    .receive = mosh_udp_receive,
    .sent = nullplug_sent,
    .accepting = NULL,
};

static bool mosh_open_udp_socket(Mosh *mosh, bool fatal_on_error)
{
    SockAddr *addr;
    char *realhost = NULL;
    const char *err;
    int addressfamily;

    if (!mosh->bootstrap.port || !mosh->udp_target_ready)
        return false;

    addressfamily = conf_get_int(mosh->conf, CONF_addressfamily);
    addr = name_lookup(mosh->udp_host, mosh->bootstrap.port, &realhost,
                       mosh->conf, addressfamily, mosh->logctx, "mosh UDP");
    if ((err = sk_addr_error(addr)) != NULL) {
        if (fatal_on_error)
            seat_connection_fatal(mosh->seat, "Mosh UDP lookup failed: %s",
                                  err);
        else {
            char msg[256];
            snprintf(msg, sizeof(msg), "Mosh UDP reopen lookup failed: %s",
                     err);
            logevent(mosh->logctx, msg);
        }
        sk_addr_free(addr);
        sfree(realhost);
        return false;
    }

    mosh->udp_socket = sk_new_udp(addr, mosh->bootstrap.port, &mosh->udp_plug);
    sfree(realhost);
    if ((err = sk_socket_error(mosh->udp_socket)) != NULL) {
        Socket *failed_socket = mosh->udp_socket;
        mosh->udp_socket = NULL;
        if (failed_socket)
            sk_close(failed_socket);
        if (fatal_on_error)
            seat_connection_fatal(mosh->seat, "Mosh UDP connect failed: %s",
                                  err);
        else {
            char msg[256];
            snprintf(msg, sizeof(msg), "Mosh UDP reopen failed: %s", err);
            logevent(mosh->logctx, msg);
        }
        return false;
    }

    return true;
}

static bool mosh_reopen_udp_socket(Mosh *mosh)
{
    Socket *old_socket;

    if (!mosh->udp_started || mosh->shutdown)
        return false;

    old_socket = mosh->udp_socket;
    mosh->udp_socket = NULL;

    if (!mosh_open_udp_socket(mosh, false)) {
        mosh->udp_socket = old_socket;
        return false;
    }

    if (old_socket)
        sk_close(old_socket);
    logevent(mosh->logctx, "Mosh UDP socket reopened after timeout");
    mosh->last_udp_reopen_attempt_ms = 0;
    if (mosh->udp_ready) {
        mosh_send_ack(mosh, GETTICKCOUNT());
        mosh_try_send_pending(mosh);
        if (mosh->ldisc && mosh_sendok(&mosh->backend))
            ldisc_check_sendok(mosh->ldisc);
    } else {
        mosh->last_assoc_probe_ms = 0;
        mosh_send_assoc_probe(mosh, GETTICKCOUNT());
    }
    return true;
}

static void mosh_udp_reopen_callback(void *ctx)
{
    Mosh *mosh = (Mosh *)ctx;

    mosh->udp_reopen_queued = false;
    if (!mosh->udp_started || mosh->shutdown || mosh->udp_socket)
        return;

    if (!mosh_reopen_udp_socket(mosh)) {
        logevent(mosh->logctx,
                 "Mosh UDP socket reopen failed; will retry");
        schedule_timer(MOSH_UDP_REOPEN_RETRY_MS, mosh_timer, mosh);
    } else {
        schedule_timer(100, mosh_timer, mosh);
    }
}

static void mosh_queue_udp_reopen(Mosh *mosh)
{
    if (!mosh->udp_started || mosh->shutdown || mosh->udp_socket ||
        mosh->udp_reopen_queued)
        return;

    mosh->udp_reopen_queued = true;
    queue_toplevel_callback(mosh_udp_reopen_callback, mosh);
}

static bool mosh_start_udp(Mosh *mosh)
{
    unsigned char packet[CMOSH_MAX_PACKET];
    size_t packet_len = 0, diff_len = 0;

    if (mosh->udp_started)
        return true;
    if (!mosh_open_udp_socket(mosh, true))
        return false;

    mosh->udp_started = true;
    if (cmosh_client_make_initial_packet(
            mosh->bootstrap.key, mosh->cols ? mosh->cols : 80,
            mosh->rows ? mosh->rows : 24, mosh_now16(GETTICKCOUNT()), packet,
            sizeof(packet), &packet_len, &diff_len) != 0) {
        seat_connection_fatal(mosh->seat,
                              "Mosh initial packet could not be encoded");
        return false;
    }

    memcpy(mosh->initial_packet, packet, packet_len);
    mosh->initial_packet_len = packet_len;
    if (mosh_udp_send(mosh, packet, packet_len))
        mosh->last_assoc_probe_ms = (uint64_t)GETTICKCOUNT();
    else
        mosh->last_assoc_probe_ms = 0;
    schedule_timer(100, mosh_timer, mosh);

    if (mosh->ssh_backend) {
        Backend *child = mosh->ssh_backend;
        mosh->ssh_backend = NULL;
        backend_free(child);
    }
    return true;
}

static void mosh_start_udp_callback(void *ctx)
{
    Mosh *mosh = (Mosh *)ctx;

    mosh->udp_start_queued = false;
    if (!mosh->shutdown)
        mosh_start_udp(mosh);
}

static size_t mosh_bootstrap_output(Seat *seat, SeatOutputType type,
                                    const void *data, size_t len)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);

    if (!mosh->bootstrap.port) {
        if (len > MOSH_BOOTSTRAP_OUTPUT_MAX ||
            mosh->bootstrap_output->len >
                MOSH_BOOTSTRAP_OUTPUT_MAX - len) {
            mosh->shutdown = true;
            seat_connection_fatal(
                mosh->seat,
                "Mosh SSH bootstrap output exceeded expected limit");
            seat_notify_remote_disconnect(mosh->seat);
            return 0;
        }
        memcpy(strbuf_append(mosh->bootstrap_output, len), data, len);
        if (mosh_parse_bootstrap_output(mosh) &&
            mosh_prepare_udp_target(mosh) && !mosh->udp_start_queued &&
            !mosh->udp_started) {
            mosh->udp_start_queued = true;
            queue_toplevel_callback(mosh_start_udp_callback, mosh);
        }
    }
    return 0;
}

static bool mosh_bootstrap_eof(Seat *seat)
{
    return true;
}

static size_t mosh_bootstrap_banner(Seat *seat, const void *data, size_t len)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_banner(mosh_wrap_real_seat(mosh), data, len);
}

static SeatPromptResult mosh_bootstrap_get_userpass_input(Seat *seat,
                                                          prompts_t *p)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_get_userpass_input(mosh_wrap_real_seat(mosh), p);
}

static void mosh_bootstrap_notify_remote_exit(Seat *seat)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    mosh->remote_exited = true;
}

static void mosh_bootstrap_notify_remote_disconnect(Seat *seat)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);

    mosh->ssh_disconnected = true;

    if (mosh->udp_started) {
        return;
    } else if (mosh->bootstrap.port && mosh->udp_target_ready) {
        seat_connection_fatal(mosh->seat,
                              "Mosh SSH bootstrap succeeded, but UDP did not "
                              "start for %s:%u",
                              mosh->udp_host, mosh->bootstrap.port);
    } else if (mosh->bootstrap.port) {
        seat_connection_fatal(mosh->seat,
                              "Mosh SSH bootstrap succeeded, but the UDP "
                              "target host could not be determined");
    } else if (mosh->bootstrap_output->len) {
        seat_connection_fatal(mosh->seat,
                              "Mosh SSH bootstrap ended without MOSH CONNECT");
    } else {
        seat_connection_fatal(mosh->seat,
                              "Mosh SSH bootstrap ended without output");
    }
    mosh->shutdown = true;
    seat_notify_remote_disconnect(mosh->seat);
}

static void mosh_bootstrap_connection_fatal(Seat *seat, const char *message)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    mosh->shutdown = true;
    seat_connection_fatal(mosh->seat, "%s", message);
}

static void mosh_bootstrap_nonfatal(Seat *seat, const char *message)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    seat_nonfatal(mosh->seat, "%s", message);
}

static char *mosh_bootstrap_get_ttymode(Seat *seat, const char *mode)
{
    return NULL;
}

static void mosh_bootstrap_set_busy_status(Seat *seat, BusyStatus status)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    seat_set_busy_status(mosh->seat, status);
}

static SeatPromptResult mosh_bootstrap_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_confirm_ssh_host_key(mosh_wrap_real_seat(mosh), host, port,
                                     keytype, keystr, text, helpctx, callback,
                                     ctx);
}

static SeatPromptResult mosh_bootstrap_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_confirm_weak_crypto_primitive(
        mosh_wrap_real_seat(mosh), text, callback, ctx);
}

static SeatPromptResult mosh_bootstrap_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_confirm_weak_cached_hostkey(
        mosh_wrap_real_seat(mosh), text, callback, ctx);
}

static const SeatDialogPromptDescriptions *mosh_bootstrap_prompt_descriptions(
    Seat *seat)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_prompt_descriptions(mosh->seat);
}

static bool mosh_bootstrap_is_utf8(Seat *seat)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_is_utf8(mosh->seat);
}

static const char *mosh_bootstrap_get_display(Seat *seat,
                                              SeatDisplayType dtype)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_get_display(mosh->seat, dtype);
}

static bool mosh_bootstrap_get_windowid(Seat *seat, long *id_out)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_get_windowid(mosh->seat, id_out);
}

static bool mosh_bootstrap_get_window_pixel_size(Seat *seat, int *width,
                                                 int *height)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_get_window_pixel_size(mosh->seat, width, height);
}

static StripCtrlChars *mosh_bootstrap_stripctrl_new(
    Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_stripctrl_new(mosh->seat, bs_out, sic);
}

static void mosh_bootstrap_set_trust_status(Seat *seat, bool trusted)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    seat_set_trust_status(mosh->seat, trusted);
}

static bool mosh_bootstrap_can_set_trust_status(Seat *seat)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_can_set_trust_status(mosh->seat);
}

static bool mosh_bootstrap_has_mixed_input_stream(Seat *seat)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_has_mixed_input_stream(mosh->seat);
}

static bool mosh_bootstrap_verbose(Seat *seat)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_verbose(mosh->seat);
}

static bool mosh_bootstrap_interactive(Seat *seat)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);
    return seat_interactive(mosh->seat);
}

static const SeatVtable Mosh_bootstrap_seat_vt = {
    .output = mosh_bootstrap_output,
    .eof = mosh_bootstrap_eof,
    .sent = nullseat_sent,
    .banner = mosh_bootstrap_banner,
    .get_userpass_input = mosh_bootstrap_get_userpass_input,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = mosh_bootstrap_notify_remote_exit,
    .notify_remote_disconnect = mosh_bootstrap_notify_remote_disconnect,
    .connection_fatal = mosh_bootstrap_connection_fatal,
    .nonfatal = mosh_bootstrap_nonfatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = mosh_bootstrap_get_ttymode,
    .set_busy_status = mosh_bootstrap_set_busy_status,
    .confirm_ssh_host_key = mosh_bootstrap_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = mosh_bootstrap_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = mosh_bootstrap_confirm_weak_cached_hostkey,
    .prompt_descriptions = mosh_bootstrap_prompt_descriptions,
    .is_utf8 = mosh_bootstrap_is_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_display = mosh_bootstrap_get_display,
    .get_windowid = mosh_bootstrap_get_windowid,
    .get_window_pixel_size = mosh_bootstrap_get_window_pixel_size,
    .stripctrl_new = mosh_bootstrap_stripctrl_new,
    .set_trust_status = mosh_bootstrap_set_trust_status,
    .can_set_trust_status = mosh_bootstrap_can_set_trust_status,
    .has_mixed_input_stream = mosh_bootstrap_has_mixed_input_stream,
    .verbose = mosh_bootstrap_verbose,
    .interactive = mosh_bootstrap_interactive,
    .get_cursor_position = nullseat_get_cursor_position,
};

static char *mosh_init(const BackendVtable *vt, Seat *seat,
                       Backend **backend_out, LogContext *logctx, Conf *conf,
                       const char *host, int port, char **realhost,
                       bool nodelay, bool keepalive)
{
    Mosh *mosh;
    char *error, *child_realhost = NULL;

    if (port < 0)
        port = 22;

    mosh = snew(Mosh);
    memset(mosh, 0, sizeof(*mosh));
    mosh->backend.vt = vt;
    mosh->backend.interactor = &mosh->interactor;
    mosh->interactor.vt = &Mosh_interactorvt;
    mosh->bootstrap_seat.vt = &Mosh_bootstrap_seat_vt;
    mosh->udp_plug.vt = &Mosh_udp_plugvt;
    mosh->seat = seat;
    mosh->conf = conf_copy(conf);
    mosh->logctx = logctx;
    mosh->description = default_description(vt, host, port);
    mosh->ssh_host = dupstr(host);
    mosh->bootstrap_output = strbuf_new();
    mosh->pending_input = strbuf_new();
    mosh->cols = 80;
    mosh->rows = 24;
    mosh->terminal = cmosh_terminal_new(mosh->cols, mosh->rows);
    if (!mosh->terminal) {
        mosh_free(&mosh->backend);
        return dupstr("Mosh terminal renderer could not be initialised");
    }
    mosh->exitcode = INT_MAX;

    mosh->bootstrap_command = mosh_build_remote_command("mosh-server",
                                                        "en_US.UTF-8");
    if (!mosh->bootstrap_command) {
        mosh_free(&mosh->backend);
        return dupstr("Mosh bootstrap command could not be constructed");
    }
    mosh->ssh_conf = mosh_make_ssh_conf(conf, port, mosh->bootstrap_command);
    conf_set_str(mosh->ssh_conf, CONF_host, host);

    error = backend_init(&ssh_backend, &mosh->bootstrap_seat,
                         &mosh->ssh_backend, logctx, mosh->ssh_conf, host,
                         port, &child_realhost, nodelay, keepalive);
    if (error) {
        mosh_free(&mosh->backend);
        return error;
    }

    if (mosh->ssh_backend && mosh->ssh_backend->interactor)
        interactor_set_child(&mosh->interactor, mosh->ssh_backend->interactor);

    *backend_out = &mosh->backend;
    *realhost = child_realhost ? child_realhost : dupstr(host);
    return NULL;
}

static void mosh_free(Backend *be)
{
    Mosh *mosh = container_of(be, Mosh, backend);

    delete_callbacks_for_context(mosh);
    expire_timer_context(mosh);
    if (mosh->udp_socket)
        sk_close(mosh->udp_socket);
    cmosh_transport_clear(&mosh->bootstrap_transport);
    cmosh_transport_clear(&mosh->client.recv_transport);
    cmosh_server_queue_clear(&mosh->client.server_queue);
    cmosh_terminal_free(mosh->terminal);
    if (mosh->ssh_backend)
        backend_free(mosh->ssh_backend);
    if (mosh->bootstrap_output)
        strbuf_free(mosh->bootstrap_output);
    if (mosh->pending_input)
        strbuf_free(mosh->pending_input);
    conf_free(mosh->conf);
    conf_free(mosh->ssh_conf);
    sfree(mosh->description);
    sfree(mosh->bootstrap_command);
    sfree(mosh->ssh_host);
    sfree(mosh);
}

static void mosh_reconfig(Backend *be, Conf *conf)
{
}

static void mosh_consume_pending_input(Mosh *mosh, size_t len)
{
    if (!len || !mosh->pending_input)
        return;
    if (mosh->pending_resize_after_input_len) {
        if (len >= mosh->pending_resize_after_input_len)
            mosh->pending_resize_after_input_len = 0;
        else
            mosh->pending_resize_after_input_len -= len;
    }
    if (len >= mosh->pending_input->len) {
        strbuf_clear(mosh->pending_input);
        return;
    }
    memmove(mosh->pending_input->u, mosh->pending_input->u + len,
            mosh->pending_input->len - len);
    strbuf_shrink_to(mosh->pending_input, mosh->pending_input->len - len);
}

static bool mosh_try_send_resize(Mosh *mosh, uint64_t now)
{
    unsigned char packet[CMOSH_MAX_PACKET];
    size_t packet_len = 0;

    if (!mosh->pending_resize)
        return true;
    if (mosh->pending_resize_after_input_len)
        return true;
    if (!mosh->udp_ready || mosh->shutdown || !mosh->udp_socket)
        return false;

    if (cmosh_client_make_resize(&mosh->client, mosh->cols, mosh->rows,
                                 now, mosh_now16((unsigned long)now),
                                 packet, sizeof(packet), &packet_len) != 0)
        return false;

    mosh->pending_resize = false;
    if (!mosh_udp_send(mosh, packet, packet_len)) {
        cmosh_client_note_input_send_failed(
            &mosh->client, mosh->client.input.current, now);
        return false;
    }
    return true;
}

static void mosh_try_send_pending(Mosh *mosh)
{
    unsigned char packet[CMOSH_MAX_PACKET];
    size_t packet_len = 0;
    uint64_t now;

    if (!mosh->udp_ready || mosh->shutdown || !mosh->pending_input)
        return;
    now = (uint64_t)GETTICKCOUNT();
    while (mosh->pending_input->len) {
        size_t chunk = mosh->pending_input->len;
        size_t room;

        if (mosh->pending_resize && !mosh->pending_resize_after_input_len &&
            !mosh_try_send_resize(mosh, now))
            return;
        if (mosh->client.input.nrecords >= CMOSH_INPUT_MAX_RECORDS)
            return;
        room = CMOSH_INPUT_MAX_BYTES - mosh->client.input.bytes_len;
        if (!room)
            return;
        if (mosh->pending_resize_after_input_len &&
            chunk > mosh->pending_resize_after_input_len)
            chunk = mosh->pending_resize_after_input_len;
        if (chunk > room)
            chunk = room;
        if (chunk > CMOSH_CLIENT_INPUT_CHUNK_MAX)
            chunk = CMOSH_CLIENT_INPUT_CHUNK_MAX;
        while (chunk &&
               cmosh_client_make_input(&mosh->client,
                                       mosh->pending_input->u,
                                       chunk, now,
                                       mosh_now16((unsigned long)now),
                                       packet, sizeof(packet),
                                       &packet_len) != 0)
            chunk /= 2;
        if (!chunk)
            return;
        if (!mosh_udp_send(mosh, packet, packet_len)) {
            cmosh_client_note_input_send_failed(
                &mosh->client, mosh->client.input.current, now);
            mosh_consume_pending_input(mosh, chunk);
            return;
        }
        mosh_consume_pending_input(mosh, chunk);
    }
    mosh_try_send_resize(mosh, now);
}

static void mosh_send(Backend *be, const char *buf, size_t len)
{
    Mosh *mosh = container_of(be, Mosh, backend);

    if (mosh->shutdown || !len)
        return;

    if (memchr(buf, 0x1d, len)) {
        mosh->shutdown = true;
        seat_notify_remote_disconnect(mosh->seat);
        return;
    }

    memcpy(strbuf_append(mosh->pending_input, len), buf, len);
    mosh_try_send_pending(mosh);
}

static size_t mosh_sendbuffer(Backend *be)
{
    Mosh *mosh = container_of(be, Mosh, backend);
    size_t control_pending = 0;

    if (mosh->shutdown)
        return 0;
    if (mosh->pending_start_ack)
        control_pending++;
    if (mosh->pending_resize)
        control_pending++;
    return mosh->client.input.bytes_len + mosh->pending_input->len +
           control_pending;
}

static void mosh_size(Backend *be, int width, int height)
{
    Mosh *mosh = container_of(be, Mosh, backend);

    mosh->cols = width > 0 ? (unsigned int)width : 80;
    mosh->rows = height > 0 ? (unsigned int)height : 24;
    if (mosh->terminal)
        cmosh_terminal_resize(mosh->terminal, mosh->cols, mosh->rows);

    if (!mosh->pending_resize) {
        mosh->pending_resize = true;
        mosh->pending_resize_after_input_len = mosh->pending_input->len;
    }

    if (!mosh->udp_ready || mosh->shutdown)
        return;

    mosh_try_send_pending(mosh);
}

static void mosh_special(Backend *be, SessionSpecialCode code, int arg)
{
    Mosh *mosh = container_of(be, Mosh, backend);

    if (code == SS_NOP && mosh->terminal) {
        struct mosh_output_ctx ctx;
        ctx.mosh = mosh;
        ctx.wrote = false;
        cmosh_terminal_render_full(mosh->terminal, mosh_seat_output, &ctx);
    }
    if (code == SS_NOP && mosh->udp_ready)
        mosh_size(be, (int)mosh->cols, (int)mosh->rows);
}

static const SessionSpecial *mosh_get_specials(Backend *be)
{
    static const SessionSpecial specials[] = {
        {"Redraw screen", SS_NOP},
        {NULL, SS_EXITMENU},
    };
    return specials;
}

static bool mosh_connected(Backend *be)
{
    Mosh *mosh = container_of(be, Mosh, backend);
    return !mosh->shutdown;
}

static int mosh_exitcode(Backend *be)
{
    Mosh *mosh = container_of(be, Mosh, backend);
    return mosh->exitcode;
}

static bool mosh_sendok(Backend *be)
{
    Mosh *mosh = container_of(be, Mosh, backend);

    return mosh->udp_ready && mosh->udp_socket && !mosh->shutdown &&
           !mosh->pending_start_ack && !mosh->pending_resize &&
           mosh->pending_input->len == 0 &&
           mosh->client.input.nrecords < CMOSH_INPUT_MAX_RECORDS &&
           mosh->client.input.bytes_len < CMOSH_INPUT_MAX_BYTES;
}

static bool mosh_ldisc_option_state(Backend *be, int option)
{
    return false;
}

static void mosh_provide_ldisc(Backend *be, Ldisc *ldisc)
{
    Mosh *mosh = container_of(be, Mosh, backend);
    mosh->ldisc = ldisc;
}

static void mosh_unthrottle(Backend *be, size_t bufsize)
{
}

static int mosh_cfg_info(Backend *be)
{
    return 0;
}

const BackendVtable mosh_backend = {
    .init = mosh_init,
    .free = mosh_free,
    .reconfig = mosh_reconfig,
    .send = mosh_send,
    .sendbuffer = mosh_sendbuffer,
    .size = mosh_size,
    .special = mosh_special,
    .get_specials = mosh_get_specials,
    .connected = mosh_connected,
    .exitcode = mosh_exitcode,
    .sendok = mosh_sendok,
    .ldisc_option_state = mosh_ldisc_option_state,
    .provide_ldisc = mosh_provide_ldisc,
    .unthrottle = mosh_unthrottle,
    .cfg_info = mosh_cfg_info,
    .id = "mosh",
    .displayname_tc = "Mosh",
    .displayname_lc = "mosh",
    .protocol = PROT_MOSH,
    .default_port = 22,
    .flags = BACKEND_NEEDS_TERMINAL,
};
