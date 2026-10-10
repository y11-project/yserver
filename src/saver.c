/*
 * saver.c - Minimal MIT-SCREEN-SAVER extension for the Y11 display
 * server.
 *
 * xidlehook (the session's idle daemon) dereferences the reply of
 * SaverQueryInfo without checking for the extension, so without this
 * it segfaults at its first idle poll.  y11 answers the two requests
 * idle clients use: QueryVersion and SaverQueryInfo.  The reported
 * idle value is the time since the last device event (keyboard,
 * button or motion), which is what feeds the lock-screen and DPMS
 * timers.  SelectInput and the attribute requests are accepted
 * without effect: y11 never blanks the screen itself.
 *
 * Wire layouts follow /usr/include/X11/extensions/saverproto.h.
 *
 * Copyright (c) 2026 The Y11 Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

#define Y11_SAVER_REQ_QUERY_VERSION  0u
#define Y11_SAVER_REQ_QUERY_INFO     1u
#define Y11_SAVER_REQ_SELECT_INPUT   2u

/* QueryVersion (minor 0): answer with the protocol's 1.1. */
static int y11_saver_query_version(struct y11_client *c)
{
    uint8_t rep[32];

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    y11_wire_put16(rep + 8, 1);  /* majorVersion */
    y11_wire_put16(rep + 10, 1); /* minorVersion */
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/*
 * QueryInfo (minor 1): drawable, then a reply with state, the saver
 * window, til-or-since, idle milliseconds and the event mask.
 * y11 keeps no saver window and never blanks: state Off, window None,
 * kind Blanked.  The idle value drives xidlehook's timers.
 */
static int y11_saver_query_info(struct y11_client *c, const uint8_t *pkt,
                                size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t drawable;
    uint8_t rep[32];

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);
    drawable = y11_wire_get32(body + 0);
    if (y11_window_get(drawable) == NULL &&
        y11_resource_get(drawable, Y11_RESOURCE_PIXMAP) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE, drawable,
                                pkt[0]);
        return 0;
    }

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    rep[1] = 1;                  /* state: Off */
    y11_wire_put32(rep + 8, 0);                 /* window: None */
    y11_wire_put32(rep + 12, 0);                /* tilOrSince */
    y11_wire_put32(rep + 16, y11_input_idle_ms());   /* idle */
    y11_wire_put32(rep + 20, 0);                /* eventMask */
    rep[24] = 0;                /* kind: Blanked */
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

int y11_saver_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                  size_t data_off)
{
    switch (pkt[1]) {
    case Y11_SAVER_REQ_QUERY_VERSION:
        return y11_saver_query_version(c);
    case Y11_SAVER_REQ_QUERY_INFO:
        return y11_saver_query_info(c, pkt, len, data_off);
    case Y11_SAVER_REQ_SELECT_INPUT:
    case 3:                     /* SetAttributes */
    case 4:                     /* UnsetAttributes */
    case 5:                     /* Suspend */
        return 0;               /* request-only: accepted, no effect */
    default:
        y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, pkt[1], pkt[0]);
        return 0;
    }
}
