/*
 * Native Mosh backend scaffold.
 *
 * This backend is deliberately not functional yet. It exists so PuTTY can
 * advertise and persist the protocol identity while the reusable cmosh client,
 * SSH bootstrap wrapper, and UDP event adapter are built up underneath it.
 */

#include "putty.h"

#include <string.h>

typedef struct Mosh Mosh;
struct Mosh {
    Backend backend;
    Seat *seat;
    char *description;
};

static char *mosh_init(const BackendVtable *vt, Seat *seat,
                       Backend **backend_out, LogContext *logctx, Conf *conf,
                       const char *host, int port, char **realhost,
                       bool nodelay, bool keepalive)
{
    Mosh *mosh;

    if (port < 0)
        port = 22;

    mosh = snew(Mosh);
    memset(mosh, 0, sizeof(*mosh));
    mosh->backend.vt = vt;
    mosh->seat = seat;
    mosh->description = default_description(vt, host, port);
    *backend_out = &mosh->backend;
    *realhost = dupstr(host);

    seat_set_trust_status(seat, false);
    return dupstr("Native Mosh support is not implemented yet");
}

static void mosh_free(Backend *be)
{
    Mosh *mosh = container_of(be, Mosh, backend);

    sfree(mosh->description);
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
    return false;
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
