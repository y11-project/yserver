/*
 * present.c - Present extension for the Y11 display server:
 * VBlank-synchronized pixmap presentation.
 *
 * PresentPixmap composites a client pixmap to a window.  When the
 * window is a fullscreen direct-scanout target and the pixmap is a
 * DRI3 buffer matching the output mode, it flips the CRTC through the
 * page-flip machinery at the next vertical blank; otherwise it blits
 * through the CPU mapping.  Either way the client receives
 * PresentCompleteNotify with the achieved MSC/UST, and PresentIdle-
 * Notify when the previous buffer is reusable.
 *
 * MSC is a per-server counter advanced once per presented frame; UST
 * is the microsecond timestamp of the presentation.  PresentNotifyMSC
 * lets clients wait for a specific MSC (vsync throttling).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "y11.h"
#include "y11_drm.h"
#include "y11_wire.h"

/* Sub-opcodes (presentproto.h). */
enum {
    Y11_PRESENT_QUERY_VERSION     = 0,
    Y11_PRESENT_PIXMAP             = 1,
    Y11_PRESENT_NOTIFY_MSC         = 2,
    Y11_PRESENT_SELECT_INPUT      = 3,
    Y11_PRESENT_QUERY_CAPABILITIES = 4
};

/* PresentCompleteNotify kind values (presenttokens.h). */
#define Y11_PRESENT_KIND_PIXMAP 0
#define Y11_PRESENT_KIND_MSC    1

/* PresentCompleteNotify mode values. */
#define Y11_PRESENT_MODE_COPY  1
#define Y11_PRESENT_MODE_FLIP  0

/* PresentOptionAsync = 1 (presenttokens.h). */
#define Y11_PRESENT_OPTION_ASYNC 1

static uint64_t y11_present_msc;    /* media stream counter */
static uint32_t y11_present_event_id;

static uint64_t y11_present_ust(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

/* Per-window present event selections (PresentSelectInput). */
struct y11_present_select {
    struct y11_window  *window;
    struct y11_client *client;
    uint32_t           mask;
    struct y11_present_select *next;
};

static struct y11_present_select *y11_present_selects;

int y11_present_init(void)
{
    y11_present_msc = 0;
    y11_present_event_id = 1;
    return 0;
}

void y11_present_shutdown(void)
{
    while (y11_present_selects != NULL) {
        struct y11_present_select *s = y11_present_selects;

        y11_present_selects = s->next;
        free(s);
    }
}

void y11_present_purge_client(struct y11_client *c)
{
    struct y11_present_select **link = &y11_present_selects;

    while (*link != NULL) {
        struct y11_present_select *s = *link;

        if (s->client == c) {
            *link = s->next;
            free(s);
        } else {
            link = &s->next;
        }
    }
}

/* ---- event delivery ---------------------------------------------------------- */

/* Deliver a PresentCompleteNotify (40 bytes) to the window's
 * subscribed clients. */
static void y11_present_complete(struct y11_window *win, uint8_t kind,
                                 uint8_t mode, uint32_t event_id,
                                 uint32_t serial, uint64_t ust,
                                 uint64_t msc)
{
    uint8_t ev[40];
    struct y11_present_select *s;

    memset(ev, 0, sizeof(ev));
    ev[0] = (uint8_t)Y11_PRESENT_FIRST_EVENT + 1;  /* CompleteNotify */
    ev[1] = (uint8_t)Y11_PRESENT_EXT_OPCODE;
    y11_wire_put16(ev + 2, 0);                    /* sequence stamp at send */
    y11_wire_put32(ev + 4, 2);                    /* length: 8 extra bytes */
    y11_wire_put16(ev + 8, 1);                    /* evtype */
    ev[10] = kind;
    ev[11] = mode;
    y11_wire_put32(ev + 12, event_id);
    y11_wire_put32(ev + 16, win != NULL ? win->id : 0);
    y11_wire_put32(ev + 20, serial);
    y11_wire_put64(ev + 24, ust);
    y11_wire_put64(ev + 32, msc);

    for (s = y11_present_selects; s != NULL; s = s->next) {
        if (s->window == win && (s->mask & 2) != 0)  /* CompleteNotifyMask */
            y11_event_dispatch32(s->client, ev, sizeof(ev));
    }
}

/* Deliver a PresentIdleNotify (32 bytes). */
static void y11_present_idle(struct y11_window *win, uint32_t event_id,
                             uint32_t serial, yid_t pixmap_id)
{
    uint8_t ev[32];
    struct y11_present_select *s;

    memset(ev, 0, sizeof(ev));
    ev[0] = (uint8_t)Y11_PRESENT_FIRST_EVENT + 2;  /* IdleNotify */
    ev[1] = (uint8_t)Y11_PRESENT_EXT_OPCODE;
    y11_wire_put32(ev + 4, 0);
    y11_wire_put16(ev + 8, 2);                    /* evtype */
    y11_wire_put32(ev + 12, event_id);
    y11_wire_put32(ev + 16, win != NULL ? win->id : 0);
    y11_wire_put32(ev + 20, serial);
    y11_wire_put32(ev + 24, pixmap_id);

    for (s = y11_present_selects; s != NULL; s = s->next) {
        if (s->window == win && (s->mask & 4) != 0)  /* IdleNotifyMask */
            y11_event_dispatch32(s->client, ev, sizeof(ev));
    }
}

/* ---- PresentQueryVersion (0) ---------------------------------------------------- */

static int y11_present_query_version(struct y11_client *c,
                                    const uint8_t *pkt, size_t len,
                                    size_t data_off)
{
    y11_version_reply rep;

    (void)pkt;
    (void)data_off;
    if (len != 12u)
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put32(&rep.major, 1);
    y11_wire_put32(&rep.minor, 2);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* ---- PresentPixmap (1) ------------------------------------------------------------ */

/*
 * Composite the pixmap to the window.  Fullscreen DRI3 pixmaps that
 * match an output mode try the direct page flip (VBlank locked);
 * everything else blits through CPU mappings.  The 72-byte request is
 * followed by `notifies` LISTofPRESENTNOTIFY entries (8 bytes each).
 */
static int y11_present_pixmap(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id, pixmap_id, serial, options;
    int16_t x_off, y_off;
    struct y11_window *win;
    struct y11_pixmap *p;
    struct y11_output *out;
    int flipped = 0;

    if (len - data_off != 68u)
        return y11_dispatch_bad_length(c, pkt[0]);

    window_id = y11_wire_get32(body + 0);
    pixmap_id = y11_wire_get32(body + 4);
    serial = y11_wire_get32(body + 8);
    x_off = (int16_t)y11_wire_get16(body + 20);
    y_off = (int16_t)y11_wire_get16(body + 22);
    options = y11_wire_get32(body + 36);

    win = y11_window_get(window_id);
    p = y11_resource_get(pixmap_id, Y11_RESOURCE_PIXMAP);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }
    if (p == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_PIXMAP, pixmap_id, pkt[0]);
        return 0;
    }

    /*
     * Fullscreen direct scanout: the window covers the root, the
     * pixmap is a DRI3 buffer matching the output geometry, and the
     * server holds DRM master.  On hosts without master this falls
     * through to the blit path.
     */
    out = y11_drm_outputs();
    if (out != NULL && win->parent != NULL && win->parent->id ==
        Y11_SCREEN_ROOT &&
        win->drawable.width == out->mode.hdisplay &&
        win->drawable.height == out->mode.vdisplay &&
        p->is_dri3 && p->dri3 != NULL && p->dri3->fb_id == 0) {
        int fd = out->drm_fd;
        uint32_t handles[4] = { p->dri3->gem_handle, 0, 0, 0 };
        uint32_t pitches[4] = { p->dri3->stride, 0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };

        if (drmIsMaster(fd)) {
            if (drmModeAddFB2(fd, p->base.width, p->base.height,
                              p->dri3->format, handles, pitches, offsets,
                              &p->dri3->fb_id, 0) == 0) {
                int r = drmModeSetCrtc(fd, out->crtc_id, p->dri3->fb_id,
                                       0, 0, &out->connector_id, 1,
                                       &out->mode);

                if (r == 0)
                    flipped = 1;
            }
        }
    }

    /*
     * Blit fallback: copy the pixmap rows into the window buffer
     * through the CPU mapping (dri3 maps on demand, software pixmaps
     * read directly).
     */
    if (!flipped) {
        const uint8_t *src = (const uint8_t *)p->base.pixels;
        int32_t row;

        if (src == NULL && p->is_dri3) {
            if (y11_dri3_pixmap_cpu_map(p) == 0)
                src = (const uint8_t *)p->dri3->map;
        }
        if (src != NULL) {
            int32_t dx = x_off;
            int32_t dy = y_off;
            uint32_t w = p->base.width, h = p->base.height;

            if (dx < 0) {
                if ((uint32_t)(-dx) >= w)
                    goto presented;
                w -= (uint32_t)(-dx);
                src += (size_t)(-dx) * 4u;
                dx = 0;
            }
            if (dy < 0) {
                if ((uint32_t)(-dy) >= h)
                    goto presented;
                src += (size_t)(-dy) * p->base.stride;
                h -= (uint32_t)(-dy);
                dy = 0;
            }
            if (dx + (int32_t)w > (int32_t)win->drawable.width)
                w = (uint32_t)((int32_t)win->drawable.width - dx);
            if (dy + (int32_t)h > (int32_t)win->drawable.height)
                h = (uint32_t)((int32_t)win->drawable.height - dy);
            if (w == 0 || h == 0)
                goto presented;

            for (row = 0; row < (int32_t)h; row++) {
                memcpy((uint8_t *)win->drawable.pixels +
                           ((size_t)(dy + row) *
                                (win->drawable.stride / 4u) +
                            (size_t)dx) * 4u,
                       src + (size_t)row * p->base.stride,
                       (size_t)w * 4u);
            }
            y11_damage_drawn(&win->drawable, dx, dy, w, h);
        }
    }

presented:
    /* Frame is visible: advance the MSC and notify the client. */
    y11_present_msc++;
    {
        uint32_t event_id = y11_present_event_id++;

        y11_present_complete(win, Y11_PRESENT_KIND_PIXMAP,
                             flipped ? Y11_PRESENT_MODE_FLIP :
                                       Y11_PRESENT_MODE_COPY,
                             event_id, serial, y11_present_ust(),
                             y11_present_msc);
        /* The pixmap is immediately reusable (no flip reordering). */
        y11_present_idle(win, event_id, serial, pixmap_id);
    }
    (void)options;
    return 0;                   /* no reply */
}

/* ---- PresentNotifyMSC (2) ---------------------------------------------------------- */

static int y11_present_notify_msc(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id, serial;
    struct y11_window *win;

    if (len - data_off != 36u)
        return y11_dispatch_bad_length(c, pkt[0]);

    window_id = y11_wire_get32(body + 0);
    serial = y11_wire_get32(body + 4);

    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    /*
     * The counter advances once per presented frame; a notification
     * for a target we have already reached fires immediately, keyed
     * with the kind MSC.
     */
    y11_present_complete(win, Y11_PRESENT_KIND_MSC, Y11_PRESENT_MODE_COPY,
                         0, serial, y11_present_ust(), y11_present_msc);
    return 0;                   /* no reply */
}

/* ---- PresentSelectInput (3) ------------------------------------------------------------ */

static int y11_present_select_input(struct y11_client *c,
                                    const uint8_t *pkt, size_t len,
                                    size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id, mask;
    struct y11_window *win;
    struct y11_present_select *s;

    if (len - data_off != 12u) /* eid, window, event mask */
        return y11_dispatch_bad_length(c, pkt[0]);

    /* eid (body + 0) stays 0: no redirect notifications. */
    window_id = y11_wire_get32(body + 4);
    mask = y11_wire_get32(body + 8);

    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    /* Replace any previous selection for this client + window. */
    for (s = y11_present_selects; s != NULL; s = s->next) {
        if (s->window == win && s->client == c) {
            s->mask = mask;
            return 0;           /* no reply */
        }
    }
    if (mask == 0)
        return 0;               /* no reply */

    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    s->window = win;
    s->client = c;
    s->mask = mask;
    s->next = y11_present_selects;
    y11_present_selects = s;
    return 0;                   /* no reply */
}

/* ---- PresentQueryCapabilities (4) ---------------------------------------------------------- */

static int y11_present_query_capabilities(struct y11_client *c,
                                          const uint8_t *pkt, size_t len,
                                          size_t data_off)
{
    y11_grab_reply rep;         /* same 32-byte shape */

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put32(&rep.pad0[0], 0);    /* PresentCapabilityNone */

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* ---- dispatcher --------------------------------------------------------------------------- */

int y11_present_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                   size_t data_off)
{
    switch (pkt[1]) {
    case Y11_PRESENT_QUERY_VERSION:
        return y11_present_query_version(c, pkt, len, data_off);
    case Y11_PRESENT_PIXMAP:
        return y11_present_pixmap(c, pkt, len, data_off);
    case Y11_PRESENT_NOTIFY_MSC:
        return y11_present_notify_msc(c, pkt, len, data_off);
    case Y11_PRESENT_SELECT_INPUT:
        return y11_present_select_input(c, pkt, len, data_off);
    case Y11_PRESENT_QUERY_CAPABILITIES:
        return y11_present_query_capabilities(c, pkt, len, data_off);
    default:
        y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, pkt[1], pkt[0]);
        return 0;
    }
}
