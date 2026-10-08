/*
 * property.c - Window property storage for the Y11 display server.
 *
 * Properties are (name, type, format, data) tuples attached to
 * windows.  ChangeProperty / GetProperty / ListProperties /
 * DeleteProperty run against a per-window linked list, and every
 * change notifies PropertyChangeMask subscribers with a 32-byte
 * PropertyNotify event.  EWMH root properties (_NET_SUPPORTED and
 * friends) live on the root window and are readable by window
 * managers, exactly like on a full X server.
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* ---- storage ------------------------------------------------------------------ */

/* Append a property to a window, replacing any earlier same-name one. */
static int y11_property_set(struct y11_window *win, yid_t name,
                            yid_t type, uint8_t format,
                            const uint8_t *data, size_t size, int mode)
{
    struct y11_property **link = &win->props;
    struct y11_property *p;
    uint8_t *copy = NULL;

    if (data != NULL && size > 0) {
        copy = malloc(size);
        if (copy == NULL)
            return -1;
        memcpy(copy, data, size);
    }

    /* Locate any existing property of the same name. */
    while (*link != NULL) {
        if ((*link)->name == name)
            break;
        link = &(*link)->next;
    }

    if (mode == 0 /* Replace */ || *link == NULL) {
        if (*link != NULL) {
            p = *link;
            free(p->data);
        } else {
            p = calloc(1, sizeof(*p));
            if (p == NULL) {
                free(copy);
                return -1;
            }
            p->name = name;
            *link = p;
        }
        p->type = type;
        p->format = format;
        p->data = copy;
        p->size = size;
        return 0;
    }

    /* Append / Prepend: concatenate onto the existing value. */
    p = *link;
    {
        struct y11_property *newp;
        uint8_t *merged = malloc(p->size + size);

        if (merged == NULL) {
            free(copy);
            return -1;
        }
        if (mode == 1 /* Append */) {
            memcpy(merged, p->data, p->size);
            memcpy(merged + p->size, copy, size);
        } else { /* Prepend */
            memcpy(merged, copy, size);
            memcpy(merged + size, p->data, p->size);
        }
        free(copy);
        free(p->data);
        p->data = merged;
        p->size += size;
        newp = p;
        (void)newp;
    }
    return 0;
}

struct y11_property *y11_property_find(struct y11_window *win, yid_t name)
{
    struct y11_property *p;

    for (p = win->props; p != NULL; p = p->next) {
        if (p->name == name)
            return p;
    }
    return NULL;
}

void y11_property_destroy_all(struct y11_window *win)
{
    struct y11_property *p = win->props;

    while (p != NULL) {
        struct y11_property *next = p->next;

        free(p->data);
        free(p);
        p = next;
    }
    win->props = NULL;
}

/* ---- PropertyNotify (event 28) --------------------------------------------------- */

/* Notify PropertyChangeMask subscribers: state 0 = NewValue, 1 = Deleted. */
static void y11_property_notify(struct y11_window *win, yid_t name,
                               uint8_t state)
{
    uint8_t ev[32];
    const struct y11_event_sub *sub;

    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if ((sub->mask & Y11_MASK_PROPERTY_CHANGE) == 0)
            continue;
        memset(ev, 0, sizeof(ev));
        ev[0] = 28;                     /* PropertyNotify */
        y11_wire_put32(ev + 4, win->id);
        y11_wire_put32(ev + 8, name);
        y11_wire_put32(ev + 12, y11_input_event_time());
        ev[16] = state;
        y11_event_dispatch32(sub->client, ev, sizeof(ev));
    }
}

/* Store a property on a window (Replace semantics). */
int y11_property_store(struct y11_window *win, yid_t name, yid_t type,
                      uint8_t format, const uint8_t *data, size_t size)
{
    return y11_property_set(win, name, type, format, data, size, 0);
}

/* Bytes per element for a property format (8, 16 or 32). */
size_t format_bytes(uint8_t format)
{
    return format == 32 ? 4 : (format == 16 ? 2 : 1);
}

/* ---- request handlers --------------------------------------------------------------- */

/*
 * ChangeProperty (opcode 18): window, property, type, format (8/16/32),
 * mode (Replace/Append/Prepend), nelements, then the data.
 */
int y11_property_req_change(struct y11_client *c, const uint8_t *pkt,
                           size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id, name, type;
    uint8_t format, mode;
    uint32_t nunits;
    struct y11_window *win;
    size_t nbytes;

    if (len - data_off < 20u)
        return y11_dispatch_bad_length(c, pkt[0]);

    window_id = y11_wire_get32(body + 0);
    name = y11_wire_get32(body + 4);
    type = y11_wire_get32(body + 8);
    format = body[12];
    mode = pkt[1];              /* Replace/Append/Prepend live in byte 1 */
    nunits = y11_wire_get32(body + 16);

    if (format != 8 && format != 16 && format != 32) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, format, pkt[0]);
        return 0;
    }
    if (mode > 2) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, mode, pkt[0]);
        return 0;
    }

    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    nbytes = (size_t)nunits * (size_t)format / 8u;
    if (len - data_off - 20u < y11_wire_pad4((uint32_t)nbytes)) {
        /* the wire data is padded to a 4-byte boundary */
        return y11_dispatch_bad_length(c, pkt[0]);
    }

    if (y11_property_set(win, name, type, format, body + 20, nbytes,
                          (int)mode) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, name, pkt[0]);
        return 0;
    }

    y11_property_notify(win, name, 0);       /* NewValue */
    return 0;                   /* no reply */
}

/*
 * GetProperty (opcode 20): window, property, type, long-offset,
 * long-length.  The reply returns the stored value; type None with
 * format 0 when the property does not exist.
 */
int y11_property_req_get(struct y11_client *c, const uint8_t *pkt,
                        size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id, name, type, long_offset, long_length;
    struct y11_window *win;
    struct y11_property *p;
    y11_get_property_reply rep;

    if (len - data_off != 20u)
        return y11_dispatch_bad_length(c, pkt[0]);

    window_id = y11_wire_get32(body + 0);
    name = y11_wire_get32(body + 4);
    type = y11_wire_get32(body + 8);
    long_offset = y11_wire_get32(body + 12);
    long_length = y11_wire_get32(body + 16);

    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    p = y11_property_find(win, name);
    if (p == NULL || (type != 0 && p->type != type)) {
        /* Not found: type None, no data. */
        memset(&rep, 0, sizeof(rep));
        rep.hdr.type = 1;       /* X_Reply */
        rep.hdr.pad0 = 0;       /* format 0 */
        y11_wire_put32(&rep.hdr.length, 0);
        y11_dispatch_send_reply(c, &rep, sizeof(rep));
        return 0;
    }

    /* Slice out [long_offset, long_offset + long_length) in 32-bit
     * units, clamped to the stored size. */
    {
        size_t unit = (size_t)format_bytes(p->format);
        size_t nitems = p->size / unit;
        size_t off = (size_t)long_offset;
        size_t count;

        if (off > nitems)
            off = nitems;
        count = nitems - off;
        if (long_length < count)
            count = long_length;
        {
            size_t payload = count * unit;
            size_t padded = y11_wire_pad4((uint32_t)payload);

            memset(&rep, 0, sizeof(rep));
            rep.hdr.type = 1;   /* X_Reply */
            rep.hdr.pad0 = p->format;
            y11_wire_put32(&rep.hdr.length, padded / 4u);
            y11_wire_put32(&rep.property_type, p->type);
            y11_wire_put32(&rep.bytes_after,
                           (uint32_t)((nitems - off - count) * unit));
            y11_wire_put32(&rep.n_items, (uint32_t)count);

            y11_dispatch_send_reply(c, &rep, sizeof(rep));
            if (payload > 0) {
                uint8_t *zeros = calloc(1, padded);

                if (zeros == NULL) {
                    y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0,
                                            pkt[0]);
                    return 0;
                }
                memcpy(zeros, (const uint8_t *)p->data + off * unit,
                       payload);
                y11_client_send(c, zeros, padded);
                free(zeros);
            }
        }
    }
    return 0;
}

/*
 * DeleteProperty (opcode 19): window, property.
 */
int y11_property_req_delete(struct y11_client *c, const uint8_t *pkt,
                           size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id, name;
    struct y11_window *win;
    struct y11_property **link;

    if (len - data_off != 8u)
        return y11_dispatch_bad_length(c, pkt[0]);

    window_id = y11_wire_get32(body + 0);
    name = y11_wire_get32(body + 4);

    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    link = &win->props;
    while (*link != NULL) {
        struct y11_property *p = *link;

        if (p->name == name) {
            *link = p->next;
            free(p->data);
            free(p);
            y11_property_notify(win, name, 1);   /* Deleted */
            return 0;           /* no reply */
        }
        link = &p->next;
    }
    return 0;                   /* unknown property: no-op, no reply */
}

/*
 * ListProperties (opcode 21): one atom per stored property.
 */
int y11_property_req_list(struct y11_client *c, const uint8_t *pkt,
                         size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id;
    struct y11_window *win;
    struct y11_property *p;
    uint32_t count = 0;
    uint8_t *tail;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    for (p = win->props; p != NULL; p = p->next)
        count++;

    tail = NULL;
    if (count > 0) {
        tail = malloc((size_t)count * 4u);
        if (tail == NULL) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
            return 0;
        }
        {
            uint32_t i = 0;

            for (p = win->props; p != NULL; p = p->next)
                y11_wire_put32(tail + (size_t)i++ * 4u, p->name);
        }
    }

    {
        y11_list_properties_reply rep;

        memset(&rep, 0, sizeof(rep));
        rep.hdr.type = 1;
        y11_wire_put16(&rep.n_properties, (uint16_t)count);
        y11_wire_put32(&rep.hdr.length, count);
        y11_dispatch_send_reply(c, &rep, sizeof(rep));
        if (tail != NULL) {
            y11_client_send(c, tail, (size_t)count * 4u);
            free(tail);
        }
    }
    return 0;
}
