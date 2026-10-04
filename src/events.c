/*
 * events.c - 32-byte event packet crafting, mask matching and delivery
 * for the Y11 display server.
 *
 * Every X11 event is exactly 32 bytes on the wire: byte 0 carries the
 * event code, byte 1 an event-specific detail, and bytes 2-3 the target
 * client's current sequence number (low 16 bits).  Events are queued
 * non-blockingly into the target client's output ring buffer.
 */

#include <stdio.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* ---- low-level send ---------------------------------------------------------- */

/*
 * Stamp the target client's sequence number into bytes 2-3 and queue the
 * 32-byte event into its output ring buffer.
 */
void y11_event_dispatch32(struct y11_client *target, void *event, size_t len)
{
    uint8_t *b = (uint8_t *)event;

    b[2] = (uint8_t)(target->sequence_number & 0xFFu);
    b[3] = (uint8_t)((target->sequence_number >> 8) & 0xFFu);

    if (y11_debug)
        fprintf(stderr, "y11: client %d: event type %u\n",
                target->slot, (unsigned)b[0]);

    if (y11_client_send(target, event, len) != 0)
        target->dead = 1;       /* out of memory: drop the connection */
}

/* Set the event code and clear the detail byte (sequence stamped later). */
static void y11_event_set_type(void *event, uint8_t type)
{
    uint8_t *b = (uint8_t *)event;
    b[0] = type;
    b[1] = 0;
}

/*
 * Deliver a notification event about `win` to every subscriber of `win`
 * holding StructureNotifyMask (event field = win) and every subscriber of
 * `win->parent` holding SubstructureNotifyMask (event field = parent).
 * The `event` field sits at bytes 4-7 of every structure in the
 * notification family (DestroyNotify, UnmapNotify, MapNotify,
 * ReparentNotify, ConfigureNotify), so one helper patches it for all.
 */
static void y11_event_notify_family(struct y11_window *win, void *event)
{
    const struct y11_event_sub *sub;

    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if ((sub->mask & Y11_MASK_STRUCTURE_NOTIFY) != 0) {
            y11_wire_put32((uint8_t *)event + 4, win->id);
            y11_event_dispatch32(sub->client, event, 32);
        }
    }

    if (win->parent == NULL)
        return;
    for (sub = win->parent->event_subs; sub != NULL; sub = sub->next) {
        if ((sub->mask & Y11_MASK_SUBSTRUCTURE_NOTIFY) != 0) {
            y11_wire_put32((uint8_t *)event + 4, win->parent->id);
            y11_event_dispatch32(sub->client, event, 32);
        }
    }
}

/* Deliver to every subscriber of win matching mask_bit, unmodified. */
static void y11_event_deliver(struct y11_window *win, uint32_t mask_bit,
                              void *event)
{
    const struct y11_event_sub *sub;

    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if ((sub->mask & mask_bit) != 0)
            y11_event_dispatch32(sub->client, event, 32);
    }
}

/* ---- hit-testing -------------------------------------------------------------- */

/*
 * Return the deepest viewable InputOutput window under the root-relative
 * point, or NULL when there is no root.  Stacking runs bottom-to-top
 * from first_child to last_child, so scan each child list topmost
 * (last_child) first.  A window contains the point when it falls inside
 * the window including its border.
 */
struct y11_window *y11_window_at_point(int32_t x, int32_t y)
{
    struct y11_window *win = y11_window_get(Y11_SCREEN_ROOT);

    if (win == NULL)
        return NULL;

    for (;;) {
        struct y11_window *child, *found = NULL;

        for (child = win->last_child; child != NULL;
             child = child->prev_sibling) {
            if (child->window_class == Y11_WINDOW_CLASS_INPUT_ONLY)
                continue;       /* transparent to the pointer */
            if (child->map_state != Y11_MAP_STATE_VIEWABLE)
                continue;
            if (x >= child->abs_x &&
                x < child->abs_x + (int32_t)child->border_width * 2 +
                    (int32_t)child->width &&
                y >= child->abs_y &&
                y < child->abs_y + (int32_t)child->border_width * 2 +
                    (int32_t)child->height) {
                found = child;
                break;
            }
        }
        if (found == NULL)
            return win;
        win = found;
    }
}

/* ---- notification crafting ------------------------------------------------------ */

/* CreateNotify (16): broadcast to the parent's SubstructureNotify listeners. */
void y11_event_send_create(struct y11_window *win)
{
    y11_create_notify_event ev;
    const struct y11_event_sub *sub;

    if (win->parent == NULL)
        return;

    for (sub = win->parent->event_subs; sub != NULL; sub = sub->next) {
        if ((sub->mask & Y11_MASK_SUBSTRUCTURE_NOTIFY) == 0)
            continue;
        memset(&ev, 0, sizeof(ev));
        y11_event_set_type(&ev, Y11_EVT_CREATE_NOTIFY);
        y11_wire_put32(&ev.parent, win->parent->id);
        y11_wire_put32(&ev.window, win->id);
        y11_wire_put16(&ev.x, (uint16_t)win->x);
        y11_wire_put16(&ev.y, (uint16_t)win->y);
        y11_wire_put16(&ev.width, win->width);
        y11_wire_put16(&ev.height, win->height);
        y11_wire_put16(&ev.border_width, win->border_width);
        ev.override = win->override_redirect ? 1 : 0;
        y11_event_dispatch32(sub->client, &ev, sizeof(ev));
    }
}

/* DestroyNotify (17): the window's StructureNotify listeners and the parent's
 * SubstructureNotify listeners. */
void y11_event_send_destroy(struct y11_window *win)
{
    y11_destroy_notify_event ev;

    memset(&ev, 0, sizeof(ev));
    y11_event_set_type(&ev, Y11_EVT_DESTROY_NOTIFY);
    y11_wire_put32(&ev.window, win->id);
    y11_event_notify_family(win, &ev);
}

/* MapNotify (19). */
void y11_event_send_map(struct y11_window *win)
{
    y11_map_notify_event ev;

    memset(&ev, 0, sizeof(ev));
    y11_event_set_type(&ev, Y11_EVT_MAP_NOTIFY);
    y11_wire_put32(&ev.window, win->id);
    ev.override = win->override_redirect ? 1 : 0;
    y11_event_notify_family(win, &ev);
}

/* UnmapNotify (18). */
void y11_event_send_unmap(struct y11_window *win, int from_configure)
{
    y11_unmap_notify_event ev;

    memset(&ev, 0, sizeof(ev));
    y11_event_set_type(&ev, Y11_EVT_UNMAP_NOTIFY);
    y11_wire_put32(&ev.window, win->id);
    ev.from_configure = from_configure ? 1 : 0;
    y11_event_notify_family(win, &ev);
}

/* ReparentNotify (21): also delivered to the old parent's SubstructureNotify
 * listeners, with the new parent reported in the event's parent field. */
void y11_event_send_reparent(struct y11_window *win,
                             struct y11_window *old_parent)
{
    y11_reparent_notify_event ev;

    memset(&ev, 0, sizeof(ev));
    y11_event_set_type(&ev, Y11_EVT_REPARENT_NOTIFY);
    y11_wire_put32(&ev.window, win->id);
    y11_wire_put32(&ev.parent,
                   win->parent != NULL ? win->parent->id : 0u);
    y11_wire_put16(&ev.x, (uint16_t)win->x);
    y11_wire_put16(&ev.y, (uint16_t)win->y);
    ev.override = win->override_redirect ? 1 : 0;

    y11_event_notify_family(win, &ev);

    if (old_parent != NULL && old_parent != win->parent) {
        const struct y11_event_sub *sub;
        for (sub = old_parent->event_subs; sub != NULL; sub = sub->next) {
            if ((sub->mask & Y11_MASK_SUBSTRUCTURE_NOTIFY) == 0)
                continue;
            y11_wire_put32((uint8_t *)&ev + 4, old_parent->id);
            y11_event_dispatch32(sub->client, &ev, sizeof(ev));
        }
    }
}

/* ConfigureNotify (22). */
void y11_event_send_configure(struct y11_window *win)
{
    y11_configure_notify_event ev;

    memset(&ev, 0, sizeof(ev));
    y11_event_set_type(&ev, Y11_EVT_CONFIGURE_NOTIFY);
    y11_wire_put32(&ev.window, win->id);
    y11_wire_put32(&ev.above_sibling, 0u);      /* none */
    y11_wire_put16(&ev.x, (uint16_t)win->x);
    y11_wire_put16(&ev.y, (uint16_t)win->y);
    y11_wire_put16(&ev.width, win->width);
    y11_wire_put16(&ev.height, win->height);
    y11_wire_put16(&ev.border_width, win->border_width);
    ev.override = win->override_redirect ? 1 : 0;

    y11_event_notify_family(win, &ev);
}

/* Expose (12): covering (0, 0, width, height), to ExposureMask listeners. */
void y11_event_send_expose(struct y11_window *win)
{
    y11_expose_event ev;

    memset(&ev, 0, sizeof(ev));
    y11_event_set_type(&ev, Y11_EVT_EXPOSE);
    y11_wire_put32(&ev.window, win->id);
    y11_wire_put16(&ev.x, 0);
    y11_wire_put16(&ev.y, 0);
    y11_wire_put16(&ev.width, win->width);
    y11_wire_put16(&ev.height, win->height);
    y11_wire_put16(&ev.count, 0);       /* last expose for this window */
    y11_event_deliver(win, Y11_MASK_EXPOSURE, &ev);
}

/* MapRequest (20): dispatched ONLY to the parent's redirecting client (the
 * window manager); the window itself is not mapped. */
void y11_event_send_map_request(struct y11_window *win)
{
    y11_map_request_event ev;
    struct y11_client *wm;

    if (win->parent == NULL)
        return;
    wm = win->parent->substructure_redirect_client;
    if (wm == NULL)
        return;

    memset(&ev, 0, sizeof(ev));
    y11_event_set_type(&ev, Y11_EVT_MAP_REQUEST);
    y11_wire_put32(&ev.parent, win->parent->id);
    y11_wire_put32(&ev.window, win->id);
    y11_event_dispatch32(wm, &ev, sizeof(ev));
}

/* ConfigureRequest (23): dispatched ONLY to the parent's redirecting client,
 * carrying the requested geometry (unrequested fields report the window's
 * current values).  Byte 1 carries the stack mode. */
void y11_event_send_configure_request(struct y11_window *win,
                                      uint32_t value_mask,
                                      int32_t x, int32_t y,
                                      uint32_t width, uint32_t height,
                                      uint32_t border_width,
                                      uint32_t sibling, uint8_t stack_mode)
{
    y11_configure_request_event ev;
    struct y11_client *wm;

    if (win->parent == NULL)
        return;
    wm = win->parent->substructure_redirect_client;
    if (wm == NULL)
        return;

    memset(&ev, 0, sizeof(ev));
    y11_event_set_type(&ev, Y11_EVT_CONFIGURE_REQUEST);
    {
        uint8_t *b = (uint8_t *)&ev;
        b[1] = stack_mode;
    }
    y11_wire_put32(&ev.parent, win->parent->id);
    y11_wire_put32(&ev.window, win->id);
    y11_wire_put32(&ev.sibling, sibling);
    y11_wire_put16(&ev.x, (uint16_t)x);
    y11_wire_put16(&ev.y, (uint16_t)y);
    y11_wire_put16(&ev.width, (uint16_t)width);
    y11_wire_put16(&ev.height, (uint16_t)height);
    y11_wire_put16(&ev.border_width, (uint16_t)border_width);
    y11_wire_put16(&ev.value_mask, (uint16_t)value_mask);
    y11_event_dispatch32(wm, &ev, sizeof(ev));
}
