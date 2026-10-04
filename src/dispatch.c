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
static int y11_dispatch_bad_length(struct y11_client *c, uint8_t opcode)
{
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, opcode);
    return 0;
}

/*
 * QueryPointer (38): a headless server has no pointer, so the reply
 * parks it at the center of the screen with no buttons pressed.
 * Clients that poll the pointer (xeyes and friends) stay happy.
 */
static int y11_dispatch_query_pointer(struct y11_client *c, const uint8_t *pkt,
                                      size_t len, size_t data_off)
{
    y11_query_pointer_reply rep;
    struct y11_window *win;

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
    y11_wire_put32(&rep.child, 0);     /* None */
    y11_wire_put16(&rep.root_x, (uint16_t)(Y11_SCREEN_WIDTH / 2));
    y11_wire_put16(&rep.root_y, (uint16_t)(Y11_SCREEN_HEIGHT / 2));
    y11_wire_put16(&rep.win_x,
                   (int16_t)(Y11_SCREEN_WIDTH / 2 - win->abs_x));
    y11_wire_put16(&rep.win_y,
                   (int16_t)(Y11_SCREEN_HEIGHT / 2 - win->abs_y));
    /* state (button mask) stays zero. */

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
 * ListProperties: y11 exposes no properties yet, so the reply reports an
 * empty atom list.  Valid for any window id.
 */
static int y11_dispatch_list_properties(struct y11_client *c,
                                        const uint8_t *pkt, size_t len,
                                        size_t data_off)
{
    y11_list_properties_reply rep;

    if (len - data_off != 4u)   /* window id */
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put16(&rep.n_properties, 0);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * GetProperty: no properties exist on any y11 window, so the reply
 * reports type None with format 0 and no data (the standard
 * "property not present" reply).
 */
static int y11_dispatch_get_property(struct y11_client *c,
                                     const uint8_t *pkt, size_t len,
                                     size_t data_off)
{
    y11_get_property_reply rep;

    if (len - data_off != 20u)  /* window, property, type, offset, length */
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 0;           /* format: 0 (not present) */
    y11_wire_put32(&rep.hdr.length, 0);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

static int y11_dispatch_get_input_focus(struct y11_client *c)
{
    y11_get_input_focus_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 0;           /* revert-to: RevertToNone */
    y11_wire_put32(&rep.focus, 1u);     /* PointerRoot */

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
    return -1;
}

/* Pack a 16-bit RGB triple into the screen's 24-bit pixel value. */
static uint32_t y11_color_pixel(uint16_t red, uint16_t green, uint16_t blue)
{
    return ((uint32_t)(red >> 8) << 16) |
           ((uint32_t)(green >> 8) << 8) |
           (uint32_t)(blue >> 8);
}

/* AllocColor (84): 16-byte request (colormap, red, green, blue, pad). */
static int y11_dispatch_alloc_color(struct y11_client *c, const uint8_t *pkt,
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

/* AllocNamedColor (85): colormap, pixel, name length, name. */
static int y11_dispatch_alloc_named_color(struct y11_client *c,
                                          const uint8_t *pkt, size_t len,
                                          size_t data_off)
{
    y11_alloc_named_color_reply rep;
    uint16_t name_len, red, green, blue;

    if (len - data_off < 12u)   /* colormap, pixel, name length, pad */
        return y11_dispatch_bad_length(c, pkt[0]);
    name_len = y11_wire_get16(pkt + data_off + 8);
    if (len - data_off - 12u < y11_wire_pad4(name_len))
        return y11_dispatch_bad_length(c, pkt[0]);

    if (y11_parse_color((const char *)pkt + data_off + 12, name_len,
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
    case Y11_REQ_REPARENT_WINDOW:
        return y11_window_req_reparent(c, pkt, len, data_off);
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
    case Y11_REQ_ALLOC_COLOR:
        return y11_dispatch_alloc_color(c, pkt, len, data_off);
    case Y11_REQ_ALLOC_NAMED_COLOR:
        return y11_dispatch_alloc_named_color(c, pkt, len, data_off);
    case Y11_REQ_QUERY_COLORS:
        return y11_dispatch_query_colors(c, pkt, len, data_off);
    case Y11_REQ_LOOKUP_COLOR:
        return y11_dispatch_lookup_color(c, pkt, len, data_off);
    case Y11_REQ_LIST_PROPERTIES:
        return y11_dispatch_list_properties(c, pkt, len, data_off);
    case Y11_REQ_GET_PROPERTY:
        return y11_dispatch_get_property(c, pkt, len, data_off);
    case Y11_REQ_GET_INPUT_FOCUS:
        return y11_dispatch_get_input_focus(c);
    case Y11_REQ_QUERY_POINTER:
        return y11_dispatch_query_pointer(c, pkt, len, data_off);
    case Y11_REQ_TRANSLATE_COORDS:
        return y11_dispatch_translate_coords(c, pkt, len, data_off);
    case Y11_REQ_GET_FONT_PATH:
        return y11_dispatch_get_font_path(c);
    case Y11_REQ_GET_KEYBOARD_CONTROL:
        return y11_dispatch_get_keyboard_control(c);
    case Y11_REQ_GET_POINTER_CONTROL:
        return y11_dispatch_get_pointer_control(c);
    case Y11_REQ_GET_SCREEN_SAVER:
        return y11_dispatch_get_screen_saver(c);
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
    case Y11_REQ_FREE_COLORS:
    case Y11_REQ_STORE_COLORS:
    case Y11_REQ_STORE_NAMED_COLOR:
    case Y11_REQ_CHANGE_PROPERTY:
    case Y11_REQ_DELETE_PROPERTY:
    case Y11_REQ_COPY_PLANE:
    case Y11_REQ_POLY_POINT:
    case Y11_REQ_POLY_LINE:
    case Y11_REQ_POLY_SEGMENT:
    case Y11_REQ_POLY_RECTANGLE:
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
    case Y11_REQ_POLY_FILL_RECTANGLE:
        return y11_render_req_poly_fill_rectangle(c, pkt, len, data_off);
    case Y11_REQ_PUT_IMAGE:
        return y11_render_req_put_image(c, pkt, len, data_off);
    case Y11_REQ_GET_IMAGE:
        return y11_render_req_get_image(c, pkt, len, data_off);
    case Y11_REQ_NO_OPERATION:
        return 0;               /* no reply */
    default:
        /* BIG-REQUESTS Enable: 4-byte request with minor opcode 0. */
        if (opcode == (uint8_t)Y11_BIGREQ_EXT_OPCODE && pkt[1] == 0 &&
            len == sizeof(y11_req))
            return y11_dispatch_bigreq_enable(c);
        y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, 0, opcode);
        return 0;
    }
}
