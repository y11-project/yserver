/*
 * dispatch.c - Opcode dispatcher, sequence tracking and reply encoding
 * for the Y11 display server.
 *
 * Strict sequence rule: every incoming request packet increments the
 * client's sequence number, and every reply or error packet carries that
 * number (low 16 bits) in bytes 2-3.
 *
 * BIG-REQUESTS: the extension is advertised through QueryExtension and
 * enabled by its 4-byte Enable request (minor opcode 0).  Once enabled,
 * a request whose 16-bit length field is 0 carries its true 4-byte-unit
 * length in the following 32-bit word (an 8-byte extended header whose
 * length includes the header itself).
 *
 * Copyright (c) 2026 The Y11 Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* ---- reply / error encoding ------------------------------------------------ */

/*
 * Send a reply packet.  Stamps the client's sequence number into bytes
 * 2-3 of the packet before queueing it.
 */
void y11_dispatch_send_reply(struct y11_client *c, void *rep, size_t len)
{
    uint8_t *b = (uint8_t *)rep;

    b[2] = (uint8_t)(c->sequence_number & 0xFFu);
    b[3] = (uint8_t)((c->sequence_number >> 8) & 0xFFu);

    if (y11_debug)
        fprintf(stderr, "y11: client %d: reply seq %u len %lu\n",
                c->slot, c->sequence_number, (unsigned long)len);

    if (y11_client_send(c, rep, len) != 0)
        c->dead = 1;            /* out of memory: drop the connection */
}

/*
 * Send a reply that carries one ancillary file descriptor (DRI3Open,
 * DRI3BufferFromPixmap): sequence-stamped, fd attached at flush time.
 */
void y11_dispatch_send_reply_fd(struct y11_client *c, void *rep, size_t len,
                                int fd)
{
    uint8_t *b = (uint8_t *)rep;

    b[2] = (uint8_t)(c->sequence_number & 0xFFu);
    b[3] = (uint8_t)((c->sequence_number >> 8) & 0xFFu);

    if (y11_client_send_fd(c, rep, len, fd) != 0)
        c->dead = 1;
}

/*
 * Send a 32-byte error packet:
 *
 *   1   0 (error)
 *   1   error code
 *   2   sequence number
 *   4   bad resource id / value
 *   2   minor opcode (0 for core requests)
 *   1   major opcode
 *   21  unused
 */
void y11_dispatch_send_error(struct y11_client *c, uint8_t code,
                             uint32_t resource_id, uint8_t major_opcode)
{
    y11_error err;

    if (y11_debug)
        fprintf(stderr, "y11: client %d: error code %u res 0x%lx major %u\n",
                c->slot, code, (unsigned long)resource_id, major_opcode);

    memset(&err, 0, sizeof(err));
    err.type = 0;
    err.error_code = code;
    y11_wire_put32(&err.resource_id, resource_id);
    err.major_opcode = major_opcode;

    y11_dispatch_send_reply(c, &err, sizeof(err));
}

/* BadLength: request size mismatch (the X11 "Length" error). */
int y11_dispatch_bad_length(struct y11_client *c, uint8_t opcode)
{
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, opcode);
    return 0;
}

/*
 * QueryPointer (38): reply with the live pointer position, pressed
 * buttons, modifiers, and the child of the queried window under the
 * cursor.
 */
static int y11_dispatch_query_pointer(struct y11_client *c, const uint8_t *pkt,
                                      size_t len, size_t data_off)
{
    y11_query_pointer_reply rep;
    const y11_pointer_t *ptr = y11_input_pointer();
    const y11_keyboard_t *kbd = y11_input_keyboard();
    struct y11_window *win, *hit;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);
    win = y11_window_get(y11_wire_get32(pkt + data_off));
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW,
                                y11_wire_get32(pkt + data_off), pkt[0]);
        return 0;
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 1;           /* same-screen */
    y11_wire_put32(&rep.root, Y11_SCREEN_ROOT);

    hit = y11_window_at_point(ptr->root_x, ptr->root_y);
    if (hit != NULL && hit != win) {
        struct y11_window *w;

        for (w = hit; w != NULL; w = w->parent) {
            if (w->parent == win) {
                y11_wire_put32(&rep.child, w->id);
                break;
            }
        }
    }

    y11_wire_put16(&rep.root_x, (uint16_t)ptr->root_x);
    y11_wire_put16(&rep.root_y, (uint16_t)ptr->root_y);
    y11_wire_put16(&rep.win_x,
                   (int16_t)(ptr->root_x - win->abs_x -
                             (int32_t)win->border_width));
    y11_wire_put16(&rep.win_y,
                   (int16_t)(ptr->root_y - win->abs_y -
                             (int32_t)win->border_width));
    y11_wire_put16(&rep.state, ptr->button_mask | kbd->modifier_mask);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * TranslateCoordinates (40): report a point's position relative to the
 * destination window's origin, using the cached absolute coordinates.
 */
static int y11_dispatch_translate_coords(struct y11_client *c,
                                         const uint8_t *pkt, size_t len,
                                         size_t data_off)
{
    y11_translate_coords_reply rep;
    struct y11_window *src, *dst;

    if (len - data_off != 12u)  /* src, dst, src-x, src-y */
        return y11_dispatch_bad_length(c, pkt[0]);
    src = y11_window_get(y11_wire_get32(pkt + data_off));
    dst = y11_window_get(y11_wire_get32(pkt + data_off + 4));
    if (src == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW,
                                y11_wire_get32(pkt + data_off), pkt[0]);
        return 0;
    }
    if (dst == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW,
                                y11_wire_get32(pkt + data_off + 4), pkt[0]);
        return 0;
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 1;           /* same-screen: one root for everyone */
    y11_wire_put32(&rep.child, 0);       /* None */
    /* Translate into the destination window's coordinate system, whose
     * origin is the interior top-left (inside the border). */
    y11_wire_put16(&rep.dst_x,
                   (int16_t)((int32_t)y11_wire_get16(pkt + data_off + 8) +
                             src->abs_x + (int32_t)src->border_width -
                             dst->abs_x - (int32_t)dst->border_width));
    y11_wire_put16(&rep.dst_y,
                   (int16_t)((int32_t)y11_wire_get16(pkt + data_off + 10) +
                             src->abs_y + (int32_t)src->border_width -
                             dst->abs_y - (int32_t)dst->border_width));

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * SendEvent (opcode 25): deliver a client-crafted 32-byte event.
 * Destination 0 is the window under the pointer, 1 the input focus;
 * when propagation is set and no client selected the mask on the
 * destination, the event walks up the parent chain with the event
 * window rewritten at each step.  The send-event flag (byte 0 bit 7)
 * marks the packet on the wire.
 */
static int y11_dispatch_send_event(struct y11_client *c, const uint8_t *pkt,
                                   size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t destination, event_mask;
    uint8_t ev[32];
    struct y11_window *win;

    if (len - data_off != 40u)  /* destination, mask, 32-byte event */
        return y11_dispatch_bad_length(c, pkt[0]);

    destination = y11_wire_get32(body + 0);
    event_mask = y11_wire_get32(body + 4);
    if ((event_mask & ~Y11_MASK_ALL_VALID) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, event_mask, pkt[0]);
        return 0;
    }
    memcpy(ev, body + 8, sizeof(ev));
    ev[0] |= 0x80;              /* send-event flag */

    if (destination == 0)       /* PointerWindow */
        win = y11_window_at_point(y11_input_pointer()->root_x,
                                  y11_input_pointer()->root_y);
    else if (destination == 1)  /* InputFocus */
        win = y11_window_get(y11_input_keyboard()->focus_window);
    else
        win = y11_window_get(destination);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, destination, pkt[0]);
        return 0;
    }

    /*
     * A zero event mask means "deliver to the client that created
     * the destination window" (the usual WM_DELETE_WINDOW shape).
     */
    if (event_mask == 0) {
        y11_wire_put32(ev + 4, win->id);
        y11_event_dispatch32(win->owner, ev, sizeof(ev));
        return 0;
    }

    for (; win != NULL; win = win->parent) {
        const struct y11_event_sub *sub;

        y11_wire_put32(ev + 4, win->id);   /* event window field */
        for (sub = win->event_subs; sub != NULL; sub = sub->next) {
            if ((sub->mask & event_mask) != 0) {
                y11_event_dispatch32(sub->client, ev, sizeof(ev));
                return 0;
            }
        }
        if (pkt[1] == 0)         /* no propagation */
            break;
    }
    return 0;                   /* no reply */
}

/*
 * GetModifierMapping (opcode 119): report two keycodes per modifier in
 * the standard order Shift, Lock, Control, Mod1..Mod5, drawn from the
 * evdev keycodes input.c uses for its modifier state.
 */
static int y11_dispatch_get_modifier_mapping(struct y11_client *c,
                                              const uint8_t *pkt, size_t len,
                                              size_t data_off)
{
    static const uint8_t keys[16] = {
        50, 62,                 /* Shift */
        66, 0,                  /* Lock */
        37, 105,                /* Control */
        64, 108,                /* Mod1 (Alt) */
        77, 0,                  /* Mod2 (NumLock) */
        0, 0,                   /* Mod3 */
        133, 134                /* Mod4 (Super) */
    };
    y11_get_keyboard_mapping_reply rep;

    (void)pkt;
    (void)len;
    (void)data_off;
    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 2;           /* keysyms per keycode */
    y11_wire_put32(&rep.hdr.length, sizeof(keys) / 4u);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    y11_client_send(c, keys, sizeof(keys));
    return 0;
}

/*
 * QueryBestSize (opcode 97): the largest cursor is 64x64; tile and
 * stipple sizes are echoed back (the headless screen has no hardware
 * limits).
 */
static int y11_dispatch_query_best_size(struct y11_client *c,
                                        const uint8_t *pkt, size_t len,
                                        size_t data_off)
{
    y11_get_geometry_reply rep;     /* same 32-byte shape: w/h at 8-11 */

    if (len - data_off != 8u)  /* drawable, width, height */
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    if (pkt[1] == 0) {          /* Cursor */
        y11_wire_put16(&rep.x, 64);
        y11_wire_put16(&rep.y, 64);
    } else {                    /* Tile / Stipple: echo */
        y11_wire_put16(&rep.x, y11_wire_get16(pkt + data_off + 4));
        y11_wire_put16(&rep.y, y11_wire_get16(pkt + data_off + 6));
    }

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * ListExtensions (opcode 99): reply with every extension name as a
 * counted string, the whole list padded to a 4-byte boundary.
 */
static int y11_dispatch_list_extensions(struct y11_client *c)
{
    static const char *const names[] = {
        Y11_BIGREQ_NAME, Y11_XTEST_NAME, Y11_SHM_NAME, Y11_DRI3_NAME,
        Y11_PRESENT_NAME, Y11_XFIXES_NAME, Y11_GLX_NAME, Y11_RENDER_NAME
    };
    y11_list_extensions_reply rep;
    uint8_t data[64];
    size_t off = 0;
    size_t total;
    unsigned i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t n = strlen(names[i]);

        data[off++] = (uint8_t)n;
        memcpy(data + off, names[i], n);
        off += n;
    }
    while (off % 4u != 0)
        data[off++] = 0;        /* pad the list to a 4-byte boundary */
    total = off;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = (uint8_t)(sizeof(names) / sizeof(names[0]));
    y11_wire_put32(&rep.hdr.length, total / 4u);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    y11_client_send(c, data, total);
    return 0;
}

/* ---- individual request handlers ------------------------------------------- */

/*
 * QueryExtension: report BIG-REQUESTS as present with the major opcode
 * y11 assigned to it.  Everything else is reported absent.  The reply is
 * exactly 32 bytes with length 0 and no name echo, matching the X server.
 */
static int y11_dispatch_query_extension(struct y11_client *c,
                                        const uint8_t *pkt, size_t len,
                                        size_t data_off)
{
    y11_query_extension_reply rep;
    uint16_t name_len;
    const uint8_t *name;

    if (len - data_off < 4u)
        return y11_dispatch_bad_length(c, pkt[0]);
    name_len = y11_wire_get16(pkt + data_off);
    if ((size_t)(len - data_off) - 4u < y11_wire_pad4(name_len))
        return y11_dispatch_bad_length(c, pkt[0]);
    name = pkt + data_off + 4;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */

    if (name_len == (uint16_t)(sizeof(Y11_BIGREQ_NAME) - 1) &&
        memcmp(name, Y11_BIGREQ_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_BIGREQ_EXT_OPCODE;
        /* first_event / first_error stay 0: the extension defines neither. */
    }
    if (name_len == (uint16_t)(sizeof(Y11_XTEST_NAME) - 1) &&
        memcmp(name, Y11_XTEST_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_XTEST_EXT_OPCODE;
    }
    if (name_len == (uint16_t)(sizeof(Y11_SHM_NAME) - 1) &&
        memcmp(name, Y11_SHM_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_SHM_EXT_OPCODE;
        rep.first_event = (uint8_t)Y11_SHM_FIRST_EVENT;
        rep.first_error = (uint8_t)Y11_SHM_FIRST_ERROR;
    }
    if (name_len == (uint16_t)(sizeof(Y11_DRI3_NAME) - 1) &&
        memcmp(name, Y11_DRI3_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_DRI3_EXT_OPCODE;
    }
    if (name_len == (uint16_t)(sizeof(Y11_PRESENT_NAME) - 1) &&
        memcmp(name, Y11_PRESENT_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_PRESENT_EXT_OPCODE;
        rep.first_event = (uint8_t)Y11_PRESENT_FIRST_EVENT;
        rep.first_error = (uint8_t)Y11_PRESENT_FIRST_ERROR;
    }
    if (name_len == (uint16_t)(sizeof(Y11_XFIXES_NAME) - 1) &&
        memcmp(name, Y11_XFIXES_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_XFIXES_EXT_OPCODE;
    }
    if (name_len == (uint16_t)(sizeof(Y11_GLX_NAME) - 1) &&
        memcmp(name, Y11_GLX_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_GLX_EXT_OPCODE;
    }
    if (name_len == (uint16_t)(sizeof(Y11_RENDER_NAME) - 1) &&
        memcmp(name, Y11_RENDER_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_RENDER_EXT_OPCODE;
    }
    if (name_len == (uint16_t)(sizeof(Y11_XKB_NAME) - 1) &&
        memcmp(name, Y11_XKB_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_XKB_EXT_OPCODE;
        rep.first_event = (uint8_t)Y11_XKB_FIRST_EVENT;
    }
    if (name_len == (uint16_t)(sizeof(Y11_SAVER_NAME) - 1) &&
        memcmp(name, Y11_SAVER_NAME, name_len) == 0) {
        rep.present = 1;
        rep.major_opcode = (uint8_t)Y11_SAVER_EXT_OPCODE;
    }

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * Enable (BIG-REQUESTS): the 4-byte request with minor opcode 0.  Reply
 * with the maximum request length y11 accepts and flag the client.
 */
static int y11_dispatch_bigreq_enable(struct y11_client *c)
{
    y11_big_req_enable_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put32(&rep.max_request_size, Y11_BIGREQ_MAX_UNITS);

    c->big_requests = 1;
    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * ListProperties (opcode 21): one atom per stored property.
 * Handled in src/property.c.
 */

static int y11_dispatch_get_input_focus(struct y11_client *c)
{
    y11_get_input_focus_reply rep;
    const y11_keyboard_t *kbd = y11_input_keyboard();

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = kbd->revert_to;
    y11_wire_put32(&rep.focus, kbd->focus_window);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* GetFontPath: y11 manages no font path; report an empty list. */
static int y11_dispatch_get_font_path(struct y11_client *c)
{
    y11_get_font_path_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put16(&rep.n_paths, 0);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * GetKeyboardControl: 52-byte reply (length 5) carrying the 32-byte
 * auto-repeat bitmap.  All controls report off/zero.
 */
static int y11_dispatch_get_keyboard_control(struct y11_client *c)
{
    y11_get_keyboard_control_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 0;           /* global-auto-repeat: off */
    y11_wire_put32(&rep.hdr.length, (sizeof(rep) - 32u) / 4u);
    /* led_mask, key_click_percent, bell_percent, bell_pitch,
     * bell_duration and map[] all stay zero. */

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

static int y11_dispatch_get_pointer_control(struct y11_client *c)
{
    y11_get_pointer_control_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put16(&rep.accel_numerator, 2);
    y11_wire_put16(&rep.accel_denominator, 1);
    y11_wire_put16(&rep.threshold, 4);
    /* hdr.length stays 0 */

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

static int y11_dispatch_get_screen_saver(struct y11_client *c)
{
    y11_get_screen_saver_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    /* timeout, interval, prefer_blanking (DontPreferBlanking) and
     * allow_exposures (DontAllowExposures) all stay zero. */

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * Color requests: resolve colors through the X11 color-name grammar
 * (#RGB/#RRGGBB/... hex forms plus a small table of common names) and
 * pack pixels through the screen's TrueColor visual (red 0xFF0000,
 * green 0xFF00, blue 0xFF).
 */

/* Scale an n-hex-digit component to the full 16-bit range. */
static uint16_t y11_color_scale(uint32_t v, unsigned digits)
{
    switch (digits) {
    case 1:  return (uint16_t)(v * 0x1111u);
    case 2:  return (uint16_t)(v * 0x101u);
    case 3:  return (uint16_t)((v << 4) | (v >> 8));
    default: return (uint16_t)v;                /* 4 digits */
    }
}

static int y11_color_hex(const char *name, size_t len, unsigned digits,
                         uint16_t *red, uint16_t *green, uint16_t *blue)
{
    uint32_t r = 0, g = 0, b = 0;
    size_t i;

    if (len != 1u + digits * 3u)
        return -1;
    for (i = 0; i < digits; i++) {
        unsigned k;
        for (k = 0; k < 3; k++) {
            char ch = name[1 + k * digits + i];
            uint32_t d;

            if (ch >= '0' && ch <= '9')
                d = (uint32_t)(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                d = (uint32_t)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                d = (uint32_t)(ch - 'A' + 10);
            else
                return -1;
            if (k == 0)
                r = r * 16u + d;
            else if (k == 1)
                g = g * 16u + d;
            else
                b = b * 16u + d;
        }
    }
    *red = y11_color_scale(r, digits);
    *green = y11_color_scale(g, digits);
    *blue = y11_color_scale(b, digits);
    return 0;
}

/*
 * Parse an X11 color name.  Returns 0 on success and fills the RGB
 * triple in the 16-bit range.
 */
static int y11_parse_color(const char *name, size_t len,
                           uint16_t *red, uint16_t *green, uint16_t *blue)
{
    static const struct {
        const char *name;
        uint16_t r, g, b;
    } table[] = {
        { "black",   0,     0,     0     },
        { "white",   65535, 65535, 65535 },
        { "red",     65535, 0,     0     },
        { "green",   0,     65535, 0     },
        { "blue",    0,     0,     65535 },
        { "cyan",    0,     65535, 65535 },
        { "magenta", 65535, 0,     65535 },
        { "yellow",  65535, 65535, 0     },
        { "gray",    0xb8b8, 0xb8b8, 0xb8b8 },
        { "grey",    0xb8b8, 0xb8b8, 0xb8b8 }
    };
    size_t i;
    unsigned digits;

    if (len > 0 && name[0] == '#') {
        for (digits = 1; digits <= 4; digits++) {
            if (len == 1u + digits * 3u &&
                y11_color_hex(name, len, digits, red, green, blue) == 0)
                return 0;
        }
        return -1;
    }
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strlen(table[i].name) == len &&
            memcmp(table[i].name, name, len) == 0) {
            *red = table[i].r;
            *green = table[i].g;
            *blue = table[i].b;
            return 0;
        }
    }
    if (len >= 5 && (memcmp(name, "gray", 4) == 0 || memcmp(name, "grey", 4) == 0)) {
        char *endp;
        long val = strtol(name + 4, &endp, 10);
        if (endp == name + len && val >= 0 && val <= 100) {
            uint16_t g = (uint16_t)((val * 65535L) / 100L);
            *red = g;
            *green = g;
            *blue = g;
            return 0;
        }
    }
    return -1;
}

/* Pack a 16-bit RGB triple into the screen's 24-bit pixel value. */
static uint32_t y11_color_pixel(uint16_t red, uint16_t green, uint16_t blue)
{
    return ((uint32_t)(red >> 8) << 16) |
           ((uint32_t)(green >> 8) << 8) |
           (uint32_t)(blue >> 8);
}

/* ---- colormap lifecycle ------------------------------------------------------ */

/*
 * TrueColor visuals carry their color maps in the pixel value, so a
 * colormap needs no palette storage: it is a registered XID that
 * CreateWindow can reference and clients can allocate colors from.
 */

static int y11_dispatch_create_colormap(struct y11_client *c,
                                        const uint8_t *pkt, size_t len,
                                        size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t mid;

    if (len - data_off != 12u)  /* mid, window, visual */
        return y11_dispatch_bad_length(c, pkt[0]);

    mid = y11_wire_get32(body + 0);
    if (mid == 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }
    if (y11_resource_add(mid, Y11_RESOURCE_COLORMAP, c) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, mid, pkt[0]);
        return 0;
    }
    return 0;                   /* no reply */
}

/* CopyColormapAndFree (79): register the copy, keep the source. */
static int y11_dispatch_copy_colormap(struct y11_client *c,
                                      const uint8_t *pkt, size_t len,
                                      size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t mid, src;

    if (len - data_off != 8u)   /* mid, src-cmap */
        return y11_dispatch_bad_length(c, pkt[0]);

    mid = y11_wire_get32(body + 0);
    src = y11_wire_get32(body + 4);
    if (y11_resource_get(src, Y11_RESOURCE_COLORMAP) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_COLORMAP, src, pkt[0]);
        return 0;
    }
    if (mid == 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }
    if (y11_resource_add(mid, Y11_RESOURCE_COLORMAP, c) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, mid, pkt[0]);
        return 0;
    }
    return 0;                   /* no reply */
}

/* FreeColormap (80). */
static int y11_dispatch_free_colormap(struct y11_client *c,
                                      const uint8_t *pkt, size_t len,
                                      size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t cmap;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    cmap = y11_wire_get32(body + 0);
    if (y11_resource_get(cmap, Y11_RESOURCE_COLORMAP) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_COLORMAP, cmap, pkt[0]);
        return 0;
    }
    y11_resource_remove(cmap);
    return 0;                   /* no reply */
}

/* InstallColormap (81) / UninstallColormap (82): no palette state. */
static int y11_dispatch_colormap_noop(struct y11_client *c,
                                     const uint8_t *pkt, size_t len,
                                     size_t data_off)
{
    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);
    return 0;                   /* no reply */
}

/* ListInstalledColormaps (83): none installed (CARD16 count at byte 8). */
static int y11_dispatch_list_colormaps(struct y11_client *c,
                                      const uint8_t *pkt, size_t len,
                                      size_t data_off)
{
    uint8_t rep[32];

    if (len - data_off != 4u)   /* window */
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/* Drop every colormap a client created (disconnect cleanup). */
static int y11_colormap_belongs(void *ptr, struct y11_client *c)
{
    return ptr == c;
}

void y11_colormap_purge_client(struct y11_client *c)
{
    y11_resource_purge_type(Y11_RESOURCE_COLORMAP, c,
                            y11_colormap_belongs, NULL);
}

/* ---- fonts and cursors ------------------------------------------------------- */

/*
 * Fonts are server-side stubs: y11 draws no text (the core text
 * requests are accepted as no-ops), so an OpenFont just registers an
 * XID and QueryFont reports an empty character cell that covers every
 * glyph index.  That is enough for Xlib's XCreateFontCursor, which
 * validates the requested shape against min/max char range before
 * asking for a glyph cursor - the shape number is what my hardware
 * cursor path keys on.
 */

/* OpenFont (45): fid, name length, name. */
static int y11_dispatch_open_font(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t fid;
    uint32_t nbytes;

    if (len - data_off < 8u)   /* fid, nbytes, pad(2) */
        return y11_dispatch_bad_length(c, pkt[0]);

    fid = y11_wire_get32(body + 0);
    nbytes = y11_wire_get16(body + 4);
    if ((size_t)nbytes > len - data_off - 8u)
        return y11_dispatch_bad_length(c, pkt[0]);
    if (fid == 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }
    if (y11_resource_add(fid, Y11_RESOURCE_FONT, c) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, fid, pkt[0]);
        return 0;
    }
    return 0;                   /* no reply */
}

/* CloseFont (46). */
static int y11_dispatch_close_font(struct y11_client *c,
                                  const uint8_t *pkt, size_t len,
                                  size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t fid;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    fid = y11_wire_get32(body + 0);
    if (y11_resource_get(fid, Y11_RESOURCE_FONT) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_FONT, fid, pkt[0]);
        return 0;
    }
    y11_resource_remove(fid);
    return 0;                   /* no reply */
}

/*
 * QueryFont (47): 60-byte fixed reply followed by per-character
 * metrics; the stub reports zero characters with a full 0..255 range
 * so every cursor-font glyph index validates on the client side.
 */
static int y11_dispatch_query_font(struct y11_client *c,
                                  const uint8_t *pkt, size_t len,
                                  size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint8_t rep[60];
    uint32_t fid;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    fid = y11_wire_get32(body + 0);
    if (y11_resource_get(fid, Y11_RESOURCE_FONT) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_FONT, fid, pkt[0]);
        return 0;
    }

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                  /* X_Reply */
    y11_wire_put32(rep + 4, 7);  /* 7 words follow the 32-byte prefix */
    y11_wire_put16(rep + 40, 0); /* minCharOrByte2 */
    y11_wire_put16(rep + 42, 255); /* maxCharOrByte2 */
    y11_wire_put16(rep + 46, 0); /* nFontProps */
    rep[51] = 1;                 /* allCharsExist */
    y11_wire_put16(rep + 52, 16); /* fontAscent */
    y11_wire_put16(rep + 54, 16); /* fontDescent */
    y11_wire_put32(rep + 56, 0); /* nCharInfos */

    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/*
 * CreateGlyphCursor (94): register the cursor XID and remember the
 * cursor-font glyph the client picked; the number matches the
 * standard cursor font shape table (XC_left_ptr, XC_xterm, ...).
 */
static int y11_dispatch_create_glyph_cursor(struct y11_client *c,
                                            const uint8_t *pkt, size_t len,
                                            size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t cid, source_font, mask_font, shape;

    if (len - data_off != 28u)  /* cid, fonts, chars, colors */
        return y11_dispatch_bad_length(c, pkt[0]);

    cid = y11_wire_get32(body + 0);
    source_font = y11_wire_get32(body + 4);
    mask_font = y11_wire_get32(body + 8);
    shape = y11_wire_get16(body + 12);

    (void)mask_font;
    if (y11_resource_get(source_font, Y11_RESOURCE_FONT) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_FONT, source_font, pkt[0]);
        return 0;
    }
    /*
     * Glyph cursors have no rasterized image in y11; register a stub
     * so DefineCursor/FreeCursor bookkeeping works and resolving
     * falls back to the built-in arrow.
     */
    if (y11_cursor_create(cid, c, 0, 0, 0, 0, NULL, 0) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, cid, pkt[0]);
        return 0;
    }
    (void)shape;                /* glyph shapes are decorative in y11 */
    return 0;                   /* no reply */
}

/* CreateCursor (93): pixmap-based cursors register the same way. */
static int y11_dispatch_create_cursor(struct y11_client *c,
                                     const uint8_t *pkt, size_t len,
                                     size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t cid, source, mask;

    if (len - data_off != 28u)  /* cid, pixmaps, colors, hotspot */
        return y11_dispatch_bad_length(c, pkt[0]);

    cid = y11_wire_get32(body + 0);
    source = y11_wire_get32(body + 4);
    mask = y11_wire_get32(body + 8);

    if (y11_resource_get(source, Y11_RESOURCE_PIXMAP) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_PIXMAP, source, pkt[0]);
        return 0;
    }
    if (mask != 0 && y11_resource_get(mask, Y11_RESOURCE_PIXMAP) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_PIXMAP, mask, pkt[0]);
        return 0;
    }
    /*
     * Core pixmap cursors have no rasterized image in y11; register a
     * stub so DefineCursor/FreeCursor bookkeeping works and resolving
     * falls back to the built-in arrow.
     */
    if (y11_cursor_create(cid, c, 0, 0, 0, 0, NULL, 0) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, cid, pkt[0]);
        return 0;
    }
    return 0;                   /* no reply */
}

/* FreeCursor (95). */
static int y11_dispatch_free_cursor(struct y11_client *c,
                                    const uint8_t *pkt, size_t len,
                                    size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t cid;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    cid = y11_wire_get32(body + 0);
    if (y11_resource_get(cid, Y11_RESOURCE_CURSOR) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_CURSOR, cid, pkt[0]);
        return 0;
    }
    y11_cursor_destroy(cid);
    return 0;                   /* no reply */
}

/* RecolorCursor (96). */
static int y11_dispatch_recolor_cursor(struct y11_client *c,
                                      const uint8_t *pkt, size_t len,
                                      size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t cid;

    if (len - data_off != 16u)
        return y11_dispatch_bad_length(c, pkt[0]);

    cid = y11_wire_get32(body + 0);
    if (y11_resource_get(cid, Y11_RESOURCE_CURSOR) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_CURSOR, cid, pkt[0]);
        return 0;
    }
    return 0;                   /* no reply */
}

/* Drop every font and cursor a client created (disconnect cleanup). */
void y11_font_purge_client(struct y11_client *c)
{
    y11_resource_purge_type(Y11_RESOURCE_FONT, c, y11_colormap_belongs,
                            NULL);
    y11_cursor_purge_client(c);
}

/* ---- XFIXES cursor names ------------------------------------------------------ */

struct y11_cursor_name {
    struct y11_cursor_name *next;
    yid_t    cursor;
    char     *name;
};

static struct y11_cursor_name *y11_cursor_names;

/* SetCursorName (23): store the name against the cursor XID. */
static void y11_xfixes_set_cursor_name(uint32_t cursor, uint32_t nbytes,
                                       const uint8_t *name)
{
    struct y11_cursor_name **link = &y11_cursor_names;
    char *copy;

    while (*link != NULL) {
        if ((*link)->cursor == cursor)
            break;
        link = &(*link)->next;
    }
    copy = malloc((size_t)nbytes + 1u);
    if (copy == NULL)
        return;
    memcpy(copy, name, nbytes);
    copy[nbytes] = '\0';

    if (*link == NULL) {
        *link = calloc(1, sizeof(**link));
        if (*link == NULL) {
            free(copy);
            return;
        }
        (*link)->cursor = cursor;
    } else {
        free((*link)->name);
    }
    (*link)->name = copy;
}

/* GetCursorName (24): the stored name, or NULL. */
static const char *y11_xfixes_get_cursor_name(uint32_t cursor)
{
    struct y11_cursor_name *n;

    for (n = y11_cursor_names; n != NULL; n = n->next) {
        if (n->cursor == cursor)
            return n->name;
    }
    return NULL;
}

/*
 * KillClient (113): disconnect the client that owns the resource.
 * The _XSETROOT_ID protocol (feh, xsetroot) kills the previous
 * background-setter through its window id; resource 0
 * (AllTemporary) disconnects every other client whose close-down
 * mode is DestroyAll.
 */
static int y11_dispatch_kill_client(struct y11_client *c,
                                    const uint8_t *pkt, size_t len,
                                    size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t resource;
    struct y11_window *win;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    resource = y11_wire_get32(body + 0);
    if (resource == 0) {
        y11_client_kill_others(c);
        return 0;
    }

    win = y11_window_get(resource);
    if (win != NULL && win->owner != NULL && win->owner != c)
        win->owner->dead = 1;
    return 0;                   /* unknown resource: no-op */
}

/*
 * SetCloseDownMode (112): DestroyAll(0) / RetainPermanent(1) /
 * RetainTemporary(2) — what survives this client's disconnect.  The
 * 4-byte request carries the mode in header byte 1.
 */
static int y11_dispatch_set_close_down_mode(struct y11_client *c,
                                            const uint8_t *pkt,
                                            size_t len, size_t data_off)
{
    (void)data_off;
    if (len != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);
    if (pkt[1] > 2) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, pkt[1], pkt[0]);
        return 0;
    }
    c->close_down_mode = pkt[1];
    return 0;                   /* no reply */
}

/* AllocColor (84): 16-byte request (colormap, red, green, blue, pad). */static int y11_dispatch_alloc_color(struct y11_client *c, const uint8_t *pkt,
                                    size_t len, size_t data_off)
{
    y11_alloc_color_reply rep;
    uint16_t red, green, blue;

    if (len - data_off != 12u)  /* colormap, red, green, blue, pad */
        return y11_dispatch_bad_length(c, pkt[0]);

    red = y11_wire_get16(pkt + data_off + 4);
    green = y11_wire_get16(pkt + data_off + 6);
    blue = y11_wire_get16(pkt + data_off + 8);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put16(&rep.red, red);
    y11_wire_put16(&rep.green, green);
    y11_wire_put16(&rep.blue, blue);
    y11_wire_put32(&rep.pixel, y11_color_pixel(red, green, blue));

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* AllocNamedColor (85): colormap, nbytes, pad(2), then the name. */
static int y11_dispatch_alloc_named_color(struct y11_client *c,
                                          const uint8_t *pkt, size_t len,
                                          size_t data_off)
{
    y11_alloc_named_color_reply rep;
    uint16_t name_len, red, green, blue;

    if (len - data_off < 12u)   /* colormap, nbytes, pad */
        return y11_dispatch_bad_length(c, pkt[0]);
    name_len = y11_wire_get16(pkt + data_off + 4);
    if (len - data_off - 8u < y11_wire_pad4(name_len))
        return y11_dispatch_bad_length(c, pkt[0]);

    if (y11_parse_color((const char *)pkt + data_off + 8, name_len,
                        &red, &green, &blue) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_NAME, 0, pkt[0]);
        return 0;
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put32(&rep.pixel, y11_color_pixel(red, green, blue));
    y11_wire_put16(&rep.exact_red, red);
    y11_wire_put16(&rep.exact_green, green);
    y11_wire_put16(&rep.exact_blue, blue);
    y11_wire_put16(&rep.screen_red, red);
    y11_wire_put16(&rep.screen_green, green);
    y11_wire_put16(&rep.screen_blue, blue);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * QueryColors (91): 8-byte fixed part (colormap), then one CARD32
 * pixel per unit of request length; there is no explicit count field.
 * The reply is followed by one 8-byte RGB item (red, green, blue,
 * pad) per pixel, all zero: Xlib reads exactly npixels * 8 bytes.
 */
static int y11_dispatch_query_colors(struct y11_client *c, const uint8_t *pkt,
                                     size_t len, size_t data_off)
{
    y11_query_colors_reply rep;
    size_t data_len, n_colors, i;

    if (len - data_off < 4u)    /* colormap */
        return y11_dispatch_bad_length(c, pkt[0]);
    data_len = len - data_off - 4u;
    if (data_len % 4u != 0)
        return y11_dispatch_bad_length(c, pkt[0]);
    n_colors = data_len / 4u;
    if (n_colors > 65535u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put32(&rep.hdr.length, (uint32_t)n_colors * 2u);  /* 8B items */
    y11_wire_put16(&rep.n_colors, (uint16_t)n_colors);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    for (i = 0; i < n_colors; i++) {
        static const uint8_t item[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        y11_client_send(c, item, sizeof(item));   /* zeroed RGB item */
    }
    return 0;
}

/* LookupColor (92): colormap, name length, name. */
static int y11_dispatch_lookup_color(struct y11_client *c, const uint8_t *pkt,
                                     size_t len, size_t data_off)
{
    y11_lookup_color_reply rep;
    uint16_t name_len;

    if (len - data_off < 8u)     /* colormap, name length, pad */
        return y11_dispatch_bad_length(c, pkt[0]);
    name_len = y11_wire_get16(pkt + data_off + 4);
    if (len - data_off - 8u < y11_wire_pad4(name_len))
        return y11_dispatch_bad_length(c, pkt[0]);

    {
        uint16_t red, green, blue;

        if (y11_parse_color((const char *)pkt + data_off + 8, name_len,
                            &red, &green, &blue) != 0) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_NAME, 0, pkt[0]);
            return 0;
        }

        memset(&rep, 0, sizeof(rep));
        rep.hdr.type = 1;       /* X_Reply */
        y11_wire_put16(&rep.exact_red, red);
        y11_wire_put16(&rep.exact_green, green);
        y11_wire_put16(&rep.exact_blue, blue);
        y11_wire_put16(&rep.screen_red, red);
        y11_wire_put16(&rep.screen_green, green);
        y11_wire_put16(&rep.screen_blue, blue);
    }

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * Resource lifecycle requests (CreateGC, ChangeGC, FreeGC, CreatePixmap,
 * FreePixmap, ChangeWindowAttributes): y11 renders nothing in phase 1,
 * so these are consumed without a reply; the resource ids they carry are
 * simply accepted.
 */
static int y11_dispatch_accept_resource(struct y11_client *c,
                                        const uint8_t *pkt, size_t len,
                                        size_t data_off)
{
    (void)c;
    (void)pkt;
    (void)data_off;
    (void)len;
    return 0;
}

/*
 * XTEST extension (major opcode 129): the input injection path.  Tools
 * like xdotool drive the keyboard and pointer through FakeInput, which
 * feeds the same y11_input_* entry points a libinput backend would.
 *
 * Requests (minor opcode = byte 1):
 *   0 GetVersion  - reply with the extension version (2.2)
 *   1 CompareCursor - reply same=1
 *   2 FakeInput   - type/detail ride in the request body
 *   3 GrabControl - no reply, ignored (y11 is never impervious)
 */
static int y11_dispatch_xtest(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;

    switch (pkt[1]) {
    case 0: {                   /* GetVersion */
        y11_get_input_focus_reply rep;  /* same 32-byte reply shape */

        if (len - data_off != 4u)
            return y11_dispatch_bad_length(c, pkt[0]);
        memset(&rep, 0, sizeof(rep));
        rep.hdr.type = 1;       /* X_Reply */
        rep.hdr.pad0 = 2;       /* major version */
        y11_wire_put32(&rep.hdr.length, 0);
        y11_wire_put16((uint8_t *)&rep + 8, 2);     /* minor version */
        y11_dispatch_send_reply(c, &rep, sizeof(rep));
        return 0;
    }
    case 1: {                   /* CompareCursor */
        y11_grab_reply rep;

        if (len - data_off != 8u)
            return y11_dispatch_bad_length(c, pkt[0]);
        memset(&rep, 0, sizeof(rep));
        rep.hdr.type = 1;       /* X_Reply */
        rep.hdr.pad0 = 1;       /* same */
        y11_dispatch_send_reply(c, &rep, sizeof(rep));
        return 0;
    }
    case 2: {                   /* FakeInput */
        uint8_t type, detail;
        int16_t root_x, root_y;

        if (len - data_off != 32u)
            return y11_dispatch_bad_length(c, pkt[0]);
        type = body[0];
        detail = body[1];
        root_x = (int16_t)y11_wire_get16(body + 20);
        root_y = (int16_t)y11_wire_get16(body + 22);

        switch (type) {
        case Y11_EVT_KEY_PRESS:
        case Y11_EVT_KEY_RELEASE:
            y11_input_key(type == Y11_EVT_KEY_PRESS, detail);
            break;
        case Y11_EVT_BUTTON_PRESS:
        case Y11_EVT_BUTTON_RELEASE:
            y11_input_button(type == Y11_EVT_BUTTON_PRESS, detail);
            break;
        case Y11_EVT_MOTION_NOTIFY:
            y11_input_motion_abs(root_x, root_y);
            break;
        default:
            y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, type, pkt[0]);
            break;
        }
        return 0;               /* no reply */
    }
    case 3:                     /* GrabControl: accepted, ignored */
        return 0;
    default:
        y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, pkt[1], pkt[0]);
        return 0;
    }
}

/* ---- the dispatcher --------------------------------------------------------- */

/*
 * Dispatch one complete request packet.  pkt points at the first byte of
 * the packet and len is its full size in bytes (a multiple of 4).
 * Returns 0 on success, -1 on a fatal error.
 */
int y11_dispatch_req(struct y11_client *c, const uint8_t *pkt, size_t len)
{
    uint8_t opcode = pkt[0];
    uint16_t wire_len = y11_wire_get16(pkt + 2);
    size_t data_off;

    /* Strict sequence rule: every request packet increments the counter. */
    c->sequence_number++;

    if (y11_debug) {
        size_t dump = len < 20 ? len : 20;
        size_t i;
        fprintf(stderr, "y11: client %d: request %u opcode %u len %lu bytes:",
                c->slot, c->sequence_number, (unsigned)opcode,
                (unsigned long)len);
        for (i = 0; i < dump; i++)
            fprintf(stderr, " %02x", pkt[i]);
        fprintf(stderr, "\n");
    }

    if (wire_len == 0) {
        /* BIG-REQUESTS framing: fields start after the 8-byte header. */
        data_off = sizeof(y11_big_req);
    } else {
        data_off = sizeof(y11_req);
    }

    switch (opcode) {
    case Y11_REQ_CREATE_WINDOW:
        return y11_window_req_create(c, pkt, len, data_off);
    case Y11_REQ_CHANGE_WINDOW_ATTRIBUTES:
        return y11_window_req_change_attributes(c, pkt, len, data_off);
    case Y11_REQ_GET_WINDOW_ATTRIBUTES:
        return y11_window_req_get_attributes(c, pkt, len, data_off);
    case Y11_REQ_DESTROY_WINDOW:
        return y11_window_req_destroy(c, pkt, len, data_off);
    case Y11_REQ_DESTROY_SUBWINDOWS:
        return y11_window_req_destroy_subwindows(c, pkt, len, data_off);
    case Y11_REQ_CHANGE_SAVE_SET: {
        uint32_t wid;

        if (len - data_off < 4u)
            return y11_dispatch_bad_length(c, pkt[0]);
        wid = y11_wire_get32(pkt + data_off);
        if (y11_window_get(wid) == NULL) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, wid, pkt[0]);
            return 0;
        }
        return 0;
    }
    case Y11_REQ_REPARENT_WINDOW:
        return y11_window_req_reparent(c, pkt, len, data_off);
    case Y11_REQ_CIRCULATE_WINDOW:
        return y11_window_req_circulate(c, pkt, len, data_off);
    case Y11_REQ_MAP_WINDOW:
        return y11_window_req_map(c, pkt, len, data_off);
    case Y11_REQ_MAP_SUBWINDOWS:
        return y11_window_req_map_subwindows(c, pkt, len, data_off);
    case Y11_REQ_UNMAP_WINDOW:
        return y11_window_req_unmap(c, pkt, len, data_off);
    case Y11_REQ_CONFIGURE_WINDOW:
        return y11_window_req_configure(c, pkt, len, data_off);
    case Y11_REQ_GET_GEOMETRY:
        return y11_window_req_get_geometry(c, pkt, len, data_off);
    case Y11_REQ_SEND_EVENT:
        return y11_dispatch_send_event(c, pkt, len, data_off);
    case Y11_REQ_CREATE_PIXMAP:
        return y11_pixmap_req_create(c, pkt, len, data_off);
    case Y11_REQ_FREE_PIXMAP:
        return y11_pixmap_req_free(c, pkt, len, data_off);
    case Y11_REQ_QUERY_TREE:
        return y11_window_req_query_tree(c, pkt, len, data_off);
    case Y11_REQ_INTERN_ATOM:
        return y11_atom_req_intern(c, pkt, len, data_off);
    case Y11_REQ_GET_ATOM_NAME:
        return y11_atom_req_get_name(c, pkt, len, data_off);
    case Y11_REQ_QUERY_EXTENSION:
        return y11_dispatch_query_extension(c, pkt, len, data_off);
    case Y11_REQ_LIST_EXTENSIONS:
        return y11_dispatch_list_extensions(c);
    case Y11_REQ_QUERY_BEST_SIZE:
        return y11_dispatch_query_best_size(c, pkt, len, data_off);
    case Y11_REQ_CREATE_COLORMAP:
        return y11_dispatch_create_colormap(c, pkt, len, data_off);
    case Y11_REQ_COPY_COLORMAP_AND_FREE:
        return y11_dispatch_copy_colormap(c, pkt, len, data_off);
    case Y11_REQ_FREE_COLORMAP:
        return y11_dispatch_free_colormap(c, pkt, len, data_off);
    case Y11_REQ_INSTALL_COLORMAP:
    case Y11_REQ_UNINSTALL_COLORMAP:
        return y11_dispatch_colormap_noop(c, pkt, len, data_off);
    case Y11_REQ_LIST_INSTALLED_COLORMAPS:
        return y11_dispatch_list_colormaps(c, pkt, len, data_off);
    case Y11_REQ_OPEN_FONT:
        return y11_dispatch_open_font(c, pkt, len, data_off);
    case Y11_REQ_CLOSE_FONT:
        return y11_dispatch_close_font(c, pkt, len, data_off);
    case Y11_REQ_QUERY_FONT:
        return y11_dispatch_query_font(c, pkt, len, data_off);
    case Y11_REQ_CREATE_CURSOR:
        return y11_dispatch_create_cursor(c, pkt, len, data_off);
    case Y11_REQ_CREATE_GLYPH_CURSOR:
        return y11_dispatch_create_glyph_cursor(c, pkt, len, data_off);
    case Y11_REQ_FREE_CURSOR:
        return y11_dispatch_free_cursor(c, pkt, len, data_off);
    case Y11_REQ_RECOLOR_CURSOR:
        return y11_dispatch_recolor_cursor(c, pkt, len, data_off);
    case Y11_REQ_ALLOC_COLOR:
        return y11_dispatch_alloc_color(c, pkt, len, data_off);
    case Y11_REQ_ALLOC_NAMED_COLOR:
        return y11_dispatch_alloc_named_color(c, pkt, len, data_off);
    case Y11_REQ_QUERY_COLORS:
        return y11_dispatch_query_colors(c, pkt, len, data_off);
    case Y11_REQ_LOOKUP_COLOR:
        return y11_dispatch_lookup_color(c, pkt, len, data_off);
    case Y11_REQ_SET_SELECTION_OWNER:
        return y11_selection_req_set_owner(c, pkt, len, data_off);
    case Y11_REQ_GET_SELECTION_OWNER:
        return y11_selection_req_get_owner(c, pkt, len, data_off);
    case Y11_REQ_CONVERT_SELECTION:
        return y11_selection_req_convert(c, pkt, len, data_off);
    case Y11_REQ_LIST_PROPERTIES:
        return y11_property_req_list(c, pkt, len, data_off);
    case Y11_REQ_GET_PROPERTY:
        return y11_property_req_get(c, pkt, len, data_off);
    case Y11_REQ_SET_INPUT_FOCUS:
        return y11_input_req_set_input_focus(c, pkt, len, data_off);
    case Y11_REQ_GRAB_POINTER:
        return y11_grab_req_pointer(c, pkt, len, data_off);
    case Y11_REQ_UNGRAB_POINTER:
        return y11_grab_req_ungrab_pointer(c, pkt, len, data_off);
    case Y11_REQ_GRAB_BUTTON:
        return y11_grab_req_button(c, pkt, len, data_off);
    case Y11_REQ_UNGRAB_BUTTON:
        return y11_grab_req_ungrab_button(c, pkt, len, data_off);
    case Y11_REQ_GRAB_KEYBOARD:
        return y11_grab_req_keyboard(c, pkt, len, data_off);
    case Y11_REQ_UNGRAB_KEYBOARD:
        return y11_grab_req_ungrab_keyboard(c, pkt, len, data_off);
    case Y11_REQ_GRAB_KEY:
        return y11_grab_req_key(c, pkt, len, data_off);
    case Y11_REQ_UNGRAB_KEY:
        return y11_grab_req_ungrab_key(c, pkt, len, data_off);
    case Y11_REQ_GRAB_SERVER:
    case Y11_REQ_UNGRAB_SERVER:
        return 0;
    case Y11_REQ_GET_INPUT_FOCUS:
        return y11_dispatch_get_input_focus(c);
    case Y11_REQ_GET_KEYBOARD_MAPPING:
        return y11_input_req_get_keyboard_mapping(c, pkt, len, data_off);
    case Y11_REQ_GET_MODIFIER_MAPPING:
        return y11_dispatch_get_modifier_mapping(c, pkt, len, data_off);
    case Y11_REQ_CHANGE_KEYBOARD_MAPPING:
        return y11_input_req_change_keyboard_mapping(c, pkt, len, data_off);
    case Y11_REQ_QUERY_POINTER:
        return y11_dispatch_query_pointer(c, pkt, len, data_off);
    case Y11_REQ_TRANSLATE_COORDS:
        return y11_dispatch_translate_coords(c, pkt, len, data_off);
    case Y11_REQ_WARP_POINTER:
        return y11_dispatch_accept_resource(c, pkt, len, data_off);
    case Y11_REQ_GET_FONT_PATH:
        return y11_dispatch_get_font_path(c);
    case Y11_REQ_GET_KEYBOARD_CONTROL:
        return y11_dispatch_get_keyboard_control(c);
    case Y11_REQ_GET_POINTER_CONTROL:
        return y11_dispatch_get_pointer_control(c);
    case Y11_REQ_GET_SCREEN_SAVER:
        return y11_dispatch_get_screen_saver(c);
    case Y11_REQ_SET_SCREEN_SAVER:
        return y11_dispatch_accept_resource(c, pkt, len, data_off);
    case Y11_REQ_CREATE_GC:
        return y11_gc_req_create(c, pkt, len, data_off);
    case Y11_REQ_CHANGE_GC:
        return y11_gc_req_change(c, pkt, len, data_off);
    case Y11_REQ_COPY_GC:
        return y11_gc_req_copy(c, pkt, len, data_off);
    case Y11_REQ_SET_CLIP_RECTANGLES:
        return y11_gc_req_set_clip_rectangles(c, pkt, len, data_off);
    case Y11_REQ_FREE_GC:
        return y11_gc_req_free(c, pkt, len, data_off);
    case Y11_REQ_CHANGE_PROPERTY:
        return y11_property_req_change(c, pkt, len, data_off);
    case Y11_REQ_DELETE_PROPERTY:
        return y11_property_req_delete(c, pkt, len, data_off);
    case Y11_REQ_FREE_COLORS:
    case Y11_REQ_STORE_COLORS:
    case Y11_REQ_STORE_NAMED_COLOR:
    case Y11_REQ_ALLOW_EVENTS:
    case Y11_REQ_FORCE_SCREEN_SAVER:
    case Y11_REQ_POLY_ARC:
    case Y11_REQ_FILL_POLY:
    case Y11_REQ_POLY_FILL_ARC:
    case Y11_REQ_POLY_TEXT8:
    case Y11_REQ_POLY_TEXT16:
    case Y11_REQ_IMAGE_TEXT8:
    case Y11_REQ_IMAGE_TEXT16:
        return y11_dispatch_accept_resource(c, pkt, len, data_off);
    case Y11_REQ_CLEAR_AREA:
        return y11_window_req_clear_area(c, pkt, len, data_off);
    case Y11_REQ_COPY_AREA:
        return y11_render_req_copy_area(c, pkt, len, data_off);
    case Y11_REQ_COPY_PLANE:
        return y11_render_req_copy_plane(c, pkt, len, data_off);
    case Y11_REQ_POLY_POINT:
        return y11_render_req_poly_point(c, pkt, len, data_off);
    case Y11_REQ_POLY_LINE:
        return y11_render_req_poly_line(c, pkt, len, data_off);
    case Y11_REQ_POLY_SEGMENT:
        return y11_render_req_poly_segment(c, pkt, len, data_off);
    case Y11_REQ_POLY_RECTANGLE:
        return y11_render_req_poly_rectangle(c, pkt, len, data_off);
    case Y11_REQ_POLY_FILL_RECTANGLE:
        return y11_render_req_poly_fill_rectangle(c, pkt, len, data_off);
    case Y11_REQ_PUT_IMAGE:
        return y11_render_req_put_image(c, pkt, len, data_off);
    case Y11_REQ_GET_IMAGE:
        return y11_render_req_get_image(c, pkt, len, data_off);
    case Y11_REQ_SET_CLOSE_DOWN_MODE:
        return y11_dispatch_set_close_down_mode(c, pkt, len, data_off);
    case Y11_REQ_KILL_CLIENT:
        return y11_dispatch_kill_client(c, pkt, len, data_off);
    case Y11_REQ_NO_OPERATION:
        return 0;               /* no reply */
    default:
        /* BIG-REQUESTS Enable: 4-byte request with minor opcode 0. */
        if (opcode == (uint8_t)Y11_BIGREQ_EXT_OPCODE && pkt[1] == 0 &&
            len == sizeof(y11_req))
            return y11_dispatch_bigreq_enable(c);
        /* XTEST extension requests carry the minor opcode in byte 1. */
        if (opcode == (uint8_t)Y11_XTEST_EXT_OPCODE)
            return y11_dispatch_xtest(c, pkt, len, data_off);
        /* MIT-SHM extension requests carry the sub-opcode in byte 1. */
        if (opcode == (uint8_t)Y11_SHM_EXT_OPCODE)
            return y11_shm_req(c, pkt, len, data_off);
        /* DRI3 buffer passing (sub-opcode in byte 1). */
        if (opcode == (uint8_t)Y11_DRI3_EXT_OPCODE)
            return y11_dri3_req(c, pkt, len, data_off);
        /* Present flips and vsync notifications (byte 1). */
        if (opcode == (uint8_t)Y11_PRESENT_EXT_OPCODE)
            return y11_present_req(c, pkt, len, data_off);
        /* GLX visual bridge (sub-opcode in byte 1). */
        if (opcode == (uint8_t)Y11_GLX_EXT_OPCODE)
            return y11_glx_req(c, pkt, len, data_off);
        /* RENDER pictures, glyph sets and compositing (byte 1). */
        if (opcode == (uint8_t)Y11_RENDER_EXT_OPCODE)
            return y11_render_req(c, pkt, len, data_off);
        /* XKEYBOARD (minor opcode in byte 1). */
        if (opcode == (uint8_t)Y11_XKB_EXT_OPCODE)
            return y11_xkb_req(c, pkt, len, data_off);
        /* MIT-SCREEN-SAVER (minor opcode in byte 1). */
        if (opcode == (uint8_t)Y11_SAVER_EXT_OPCODE)
            return y11_saver_req(c, pkt, len, data_off);
        /* XFIXES: Mesa's DRI3 loader rejects the render fd unless the
         * server reports XFIXES 2 or newer, so answer with 5.0 (the
         * version real servers expose; the sync-fence plumbing is
         * handled through DRI3FenceFromFD instead). */
        if (opcode == (uint8_t)Y11_XFIXES_EXT_OPCODE) {
            y11_version_reply vr;

            if (pkt[1] == 0 && len == 12u) {
                memset(&vr, 0, sizeof(vr));
                vr.hdr.type = 1;
                y11_wire_put32(&vr.hdr.length, 0);
                y11_wire_put32(&vr.major, 5);
                y11_wire_put32(&vr.minor, 0);
                y11_dispatch_send_reply(c, &vr, sizeof(vr));
                return 0;
            }
            /* SelectCursorInput (3): window, eventMask */
            if (pkt[1] == 3)
                return 0;
            /* GetCursorImage (4): return 32-byte reply with cursor coords, 0x0 size */
            if (pkt[1] == 4) {
                uint8_t rep[32];
                memset(rep, 0, sizeof(rep));
                rep[0] = 1;
                y11_wire_put16(rep + 8, (uint16_t)y11_input_pointer()->root_x);
                y11_wire_put16(rep + 10, (uint16_t)y11_input_pointer()->root_y);
                y11_dispatch_send_reply(c, rep, sizeof(rep));
                return 0;
            }
            /* SetCursorName (23): cursor, nbytes, name. */
            if (pkt[1] == 23 && len >= 12u) {
                uint32_t cursor = y11_wire_get32(pkt + data_off);
                uint32_t nbytes = y11_wire_get16(pkt + data_off + 4);

                if ((size_t)nbytes <= len - data_off - 8u)
                    y11_xfixes_set_cursor_name(cursor, nbytes,
                                               pkt + data_off + 8);
                return 0;
            }
            /* GetCursorName (24): atom, nbytes, then the name. */
            if (pkt[1] == 24 && len == 8u) {
                uint32_t cursor = y11_wire_get32(pkt + data_off);
                const char *name = y11_xfixes_get_cursor_name(cursor);
                uint8_t rep[32];

                memset(rep, 0, sizeof(rep));
                rep[0] = 1;
                if (name != NULL) {
                    size_t slen = strlen(name);
                    size_t padded = y11_wire_pad4((uint32_t)slen);

                    y11_wire_put32(rep + 4, (uint32_t)(padded / 4u));
                    y11_wire_put32(rep + 8, 0);  /* no atom: unnamed */
                    y11_wire_put16(rep + 12, (uint16_t)slen);
                    y11_dispatch_send_reply(c, rep, sizeof(rep));
                    y11_client_send(c, (const uint8_t *)name, slen);
                    if (padded > slen) {
                        static const uint8_t pad[4] = { 0 };
                        y11_client_send(c, pad, padded - slen);
                    }
                } else if (cursor == 0 || y11_resource_get(cursor, Y11_RESOURCE_CURSOR) != NULL) {
                    /* Valid cursor without a name: atom=None, nbytes=0 */
                    y11_dispatch_send_reply(c, rep, sizeof(rep));
                } else {
                    y11_dispatch_send_error(c, Y11_ERR_BAD_CURSOR,
                                            cursor, pkt[0]);
                }
                return 0;
            }
            /* ChangeCursor (26), ChangeCursorByName (27), HideCursor (29), ShowCursor (30) */
            if (pkt[1] == 26 || pkt[1] == 27 || pkt[1] == 29 || pkt[1] == 30)
                return 0;
            /* ChangeSaveSet (1), SelectSelectionInput (2) */
            if (pkt[1] == 1 || pkt[1] == 2)
                return 0;
            y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, pkt[1],
                                    pkt[0]);
            return 0;
        }
        y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, 0, opcode);
        return 0;
    }
}
