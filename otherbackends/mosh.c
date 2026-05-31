/*
 * Native Mosh backend scaffold.
 *
 * This backend is deliberately not functional yet. It exists so PuTTY can
 * advertise and persist the protocol identity while the reusable cmosh client,
 * SSH bootstrap wrapper, and UDP event adapter are built up underneath it.
 */

#include "putty.h"
#include "cmosh_bootstrap.h"

#include <string.h>

typedef struct Mosh Mosh;

struct Mosh {
    Backend backend;
    Interactor interactor;
    Seat bootstrap_seat;
    Seat *seat;
    Conf *conf;
    Conf *ssh_conf;
    LogContext *logctx;
    Backend *ssh_backend;
    strbuf *bootstrap_output;
    struct cmosh_bootstrap bootstrap;
    char udp_host[256];
    char *description;
    char *bootstrap_command;
    char *ssh_host;
    bool ssh_disconnected;
    bool remote_exited;
    bool shutdown;
    bool udp_target_ready;
};

static void mosh_free(Backend *be);

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

static size_t mosh_bootstrap_output(Seat *seat, SeatOutputType type,
                                    const void *data, size_t len)
{
    Mosh *mosh = container_of(seat, Mosh, bootstrap_seat);

    if (!mosh->bootstrap.port) {
        memcpy(strbuf_append(mosh->bootstrap_output, len), data, len);
        if (mosh_parse_bootstrap_output(mosh))
            mosh_prepare_udp_target(mosh);
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
    mosh->shutdown = true;

    if (mosh->bootstrap.port && mosh->udp_target_ready) {
        seat_connection_fatal(mosh->seat,
                              "Native Mosh UDP session to %s:%u is not "
                              "implemented yet",
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
    mosh->seat = seat;
    mosh->conf = conf_copy(conf);
    mosh->logctx = logctx;
    mosh->description = default_description(vt, host, port);
    mosh->ssh_host = dupstr(host);
    mosh->bootstrap_output = strbuf_new();

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

    if (mosh->ssh_backend)
        backend_free(mosh->ssh_backend);
    if (mosh->bootstrap_output)
        strbuf_free(mosh->bootstrap_output);
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

static void mosh_send(Backend *be, const char *buf, size_t len)
{
}

static size_t mosh_sendbuffer(Backend *be)
{
    return 0;
}

static void mosh_size(Backend *be, int width, int height)
{
}

static void mosh_special(Backend *be, SessionSpecialCode code, int arg)
{
}

static const SessionSpecial *mosh_get_specials(Backend *be)
{
    return NULL;
}

static bool mosh_connected(Backend *be)
{
    Mosh *mosh = container_of(be, Mosh, backend);
    return !mosh->shutdown;
}

static int mosh_exitcode(Backend *be)
{
    return INT_MAX;
}

static bool mosh_sendok(Backend *be)
{
    return false;
}

static bool mosh_ldisc_option_state(Backend *be, int option)
{
    return false;
}

static void mosh_provide_ldisc(Backend *be, Ldisc *ldisc)
{
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
