/*
 * grab.c - Pointer and keyboard grabs for the Y11 display server.
 *
 * An active grab diverts all pointer or keyboard events exclusively to
 * the grabbing client's window, optionally confined to a rectangle.
 * Passive grabs (GrabButton / GrabKey) are registered on a window and
 * activate implicitly when a matching button or key press arrives
 * while the pointer or focus is inside the grab window's hierarchy; a
 * passive pointer grab ends automatically when all buttons release.
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* Grab reply status codes (X11 wire standard). */
enum {
    Y11_GRAB_SUCCESS      = 0,
    Y11_GRAB_ALREADY      = 1,
    Y11_GRAB_INVALID_TIME = 2,
    Y11_GRAB_NOT_VIEWABLE = 3,
    Y11_GRAB_FROZEN       = 4
};

struct y11_passive_grab {
    struct y11_passive_grab *next;
    struct y11_client       *client;
    yid_t                   window;
    uint16_t                modifiers;
    uint8_t                 button;   /* 0 = any; pointer grabs */
    uint8_t                 key;     /* 0 = any; key grabs */
    uint16_t                event_mask;
    bool                    owner_events;
    yid_t                   confine_to;
    uint8_t                 pointer_mode;
    uint8_t                 keyboard_mode;
    bool                    is_key;
};

static y11_grab_t y11_pointer_grab;
static y11_grab_t y11_keyboard_grab;
static struct y11_passive_grab *y11_passive_grabs;
static uint32_t y11_last_grab_time;

/* ---- helpers --------------------------------------------------------------- */

static void y11_grab_send_reply(struct y11_client *c, uint8_t status)
{
    y11_grab_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = status;
    y11_dispatch_send_reply(c, &rep, sizeof(rep));
}

static int y11_grab_time_ok(uint32_t time)
{
    if (time == 0)
        return 1;               /* CurrentTime */
    if (y11_last_grab_time == 0)
        return 1;
    return time >= y11_last_grab_time ? 1 : 0;
}

/* Is the pointer (or keyboard focus) inside the grab window's hierarchy? */
static int y11_grab_hierarchy_hit(yid_t window, int keyboard)
{
    const y11_pointer_t *ptr = y11_input_pointer();
    const y11_keyboard_t *kbd = y11_input_keyboard();
    struct y11_window *target, *w;

    if (keyboard) {
        if (kbd->focus_window <= 1)
            return 1;           /* None/PointerRoot: any window may grab */
        target = y11_window_get(kbd->focus_window);
    } else {
        target = y11_window_get(ptr->focus_window);
    }
    for (w = target; w != NULL; w = w->parent) {
        if (w->id == window)
            return 1;
    }
    return 0;
}

/* Activate an implicit grab from a passive registration. */
static void y11_grab_activate_passive(y11_grab_t *grab,
                                      const struct y11_passive_grab *p)
{
    grab->active = true;
    grab->client = p->client;
    grab->grab_window = p->window;
    grab->confine_to = p->confine_to;
    grab->event_mask = p->event_mask;
    grab->owner_events = p->owner_events;
    grab->pointer_mode = p->pointer_mode;
    grab->keyboard_mode = p->keyboard_mode;
    grab->passive = true;
    grab->time = y11_input_event_time();
    y11_last_grab_time = grab->time;
}

/* ---- public routing API (input.c) ---------------------------------------------- */

const y11_grab_t *y11_grab_pointer_active(void)
{
    return y11_pointer_grab.active ? &y11_pointer_grab : NULL;
}

const y11_grab_t *y11_grab_keyboard_active(void)
{
    return y11_keyboard_grab.active ? &y11_keyboard_grab : NULL;
}

/*
 * On a button press: if a passive grab matches (the pointer is inside
 * the grab window's hierarchy and button/modifiers agree), activate it
 * and return the grabbing client.
 */
struct y11_client *y11_grab_match_button(uint8_t button, uint16_t state)
{
    struct y11_passive_grab *p;

    for (p = y11_passive_grabs; p != NULL; p = p->next) {
        if (p->is_key)
            continue;
        if (p->button != 0 && p->button != button)
            continue;
        if (p->modifiers != (uint16_t)0x8000 &&
            p->modifiers != state)
            continue;
        if (!y11_grab_hierarchy_hit(p->window, 0))
            continue;
        y11_grab_activate_passive(&y11_pointer_grab, p);
        return p->client;
    }
    return NULL;
}

/*
 * On a key press: if a passive key grab matches (the keyboard focus is
 * inside the grab window's hierarchy and key/modifiers agree), activate
 * it and return the grabbing client.
 */
struct y11_client *y11_grab_match_key(uint8_t key, uint16_t state)
{
    struct y11_passive_grab *p;

    for (p = y11_passive_grabs; p != NULL; p = p->next) {
        if (!p->is_key)
            continue;
        if (p->key != 0 && p->key != key)
            continue;
        if (p->modifiers != (uint16_t)0x8000 &&
            p->modifiers != state)
            continue;
        if (!y11_grab_hierarchy_hit(p->window, 1))
            continue;
        y11_grab_activate_passive(&y11_keyboard_grab, p);
        return p->client;
    }
    return NULL;
}

/* All buttons released: a passive pointer grab ends automatically. */
void y11_grab_button_release_check(void)
{
    if (y11_pointer_grab.active && y11_pointer_grab.passive &&
        y11_input_pointer()->button_mask == 0) {
        memset(&y11_pointer_grab, 0, sizeof(y11_pointer_grab));
        y11_last_grab_time = y11_input_event_time();
    }
}

/*
 * Deliver a device event through an active grab: the event goes to the
 * grab window's client when it either selected the mask bit on the
 * grab window or the grab itself selected it.
 */
void y11_grab_deliver(const y11_grab_t *grab, uint8_t type, uint8_t detail,
                      uint32_t mask_bit)
{
    struct y11_window *win = y11_window_get(grab->grab_window);
    y11_key_button_event ev;
    const struct y11_event_sub *sub;
    const y11_pointer_t *ptr = y11_input_pointer();

    if (win == NULL)
        return;

    memset(&ev, 0, sizeof(ev));
    {
        uint8_t *b = (uint8_t *)&ev;
        b[0] = type;
        b[1] = detail;
    }
    y11_wire_put32(&ev.time, y11_input_event_time());
    y11_wire_put32(&ev.root, Y11_SCREEN_ROOT);
    y11_wire_put32(&ev.event, win->id);
    y11_wire_put32(&ev.child, 0);
    y11_wire_put16(&ev.root_x, (uint16_t)ptr->root_x);
    y11_wire_put16(&ev.root_y, (uint16_t)ptr->root_y);
    y11_wire_put16(&ev.event_x,
                   (int16_t)(ptr->root_x - win->abs_x -
                             (int32_t)win->border_width));
    y11_wire_put16(&ev.event_y,
                   (int16_t)(ptr->root_y - win->abs_y -
                             (int32_t)win->border_width));
    y11_wire_put16(&ev.state,
                   ptr->button_mask | y11_input_keyboard()->modifier_mask);
    ev.same_screen = 1;

    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if (sub->client != grab->client)
            continue;
        if ((sub->mask & mask_bit) == 0 && (grab->event_mask & mask_bit) == 0)
            continue;
        y11_event_dispatch32(sub->client, &ev, sizeof(ev));
        return;
    }
}

/* Clamp the pointer inside the confine rectangle of an active grab. */
void y11_grab_confine(int16_t *x, int16_t *y)
{
    struct y11_window *win;

    if (!y11_pointer_grab.active || y11_pointer_grab.confine_to == 0)
        return;
    win = y11_window_get(y11_pointer_grab.confine_to);
    if (win == NULL)
        return;
    if (*x < win->abs_x)
        *x = win->abs_x;
    if (*x > win->abs_x + (int32_t)win->width)
        *x = win->abs_x + (int32_t)win->width;
    if (*y < win->abs_y)
        *y = win->abs_y;
    if (*y > win->abs_y + (int32_t)win->height)
        *y = win->abs_y + (int32_t)win->height;
}

/* ---- request handlers -------------------------------------------------------------- */

static uint8_t y11_grab_check_window(yid_t id, int need_viewable)
{
    struct y11_window *win = y11_window_get(id);

    if (win == NULL)
        return Y11_GRAB_NOT_VIEWABLE;
    if (need_viewable && win->map_state != Y11_MAP_STATE_VIEWABLE)
        return Y11_GRAB_NOT_VIEWABLE;
    return Y11_GRAB_SUCCESS;
}

/*
 * GrabPointer (opcode 26): owner-events@byte 1, grab window, event
 * mask, modes, confine-to, cursor, time.  Replies with a status.
 */
int y11_grab_req_pointer(struct y11_client *c, const uint8_t *pkt,
                         size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t grab_window, confine_to, time;
    uint16_t event_mask;
    uint8_t status;

    if (len - data_off != 20u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    grab_window = y11_wire_get32(body + 0);
    event_mask = y11_wire_get16(body + 4);
    confine_to = y11_wire_get32(body + 8);
    time = y11_wire_get32(body + 16);

    if (body[2] > 1 || body[3] > 1) {   /* pointer/keyboard mode */
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }

    status = y11_grab_check_window(grab_window, 1);
    if (status != Y11_GRAB_SUCCESS)
        goto reply;
    if (confine_to != 0 &&
        y11_grab_check_window(confine_to, 1) != Y11_GRAB_SUCCESS) {
        status = Y11_GRAB_NOT_VIEWABLE;
        goto reply;
    }
    if (y11_pointer_grab.active) {
        status = Y11_GRAB_ALREADY;
        goto reply;
    }
    if (!y11_grab_time_ok(time)) {
        status = Y11_GRAB_INVALID_TIME;
        goto reply;
    }

    y11_pointer_grab.active = true;
    y11_pointer_grab.client = c;
    y11_pointer_grab.grab_window = grab_window;
    y11_pointer_grab.confine_to = confine_to;
    y11_pointer_grab.cursor_id = y11_wire_get32(body + 12);
    y11_pointer_grab.event_mask = event_mask;
    y11_pointer_grab.owner_events = pkt[1] != 0;
    y11_pointer_grab.pointer_mode = body[2];
    y11_pointer_grab.keyboard_mode = body[3];
    y11_pointer_grab.passive = false;
    y11_pointer_grab.time = y11_input_event_time();
    y11_last_grab_time = y11_pointer_grab.time;
    status = Y11_GRAB_SUCCESS;

reply:
    y11_grab_send_reply(c, status);
    return 0;
}

/* UngrabPointer (opcode 27): time. */
int y11_grab_req_ungrab_pointer(struct y11_client *c, const uint8_t *pkt,
                                size_t len, size_t data_off)
{
    uint32_t time;

    (void)c;
    if (len - data_off != 4u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    time = y11_wire_get32(pkt + data_off);
    if (!y11_pointer_grab.active)
        return 0;
    if (time != 0 && time < y11_pointer_grab.time)
        return 0;               /* GrabInvalidTime: silently ignored */
    memset(&y11_pointer_grab, 0, sizeof(y11_pointer_grab));
    y11_last_grab_time = y11_input_event_time();
    return 0;                   /* no reply */
}

/* Shared passive-grab registration (pointer buttons and keys). */
static int y11_grab_register_passive(struct y11_client *c,
                                     const uint8_t *pkt, size_t data_off,
                                     int is_key)
{
    const uint8_t *body = pkt + data_off;
    struct y11_passive_grab *p;
    yid_t window = y11_wire_get32(body + 0);
    uint16_t modifiers;
    uint8_t button = 0, key = 0;
    struct y11_passive_grab *q;

    if (y11_window_get(window) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window, pkt[0]);
        return 0;
    }
    if (is_key) {
        modifiers = y11_wire_get16(body + 4);
        key = pkt[1];
        if (body[3] > 1 || body[4] > 1) {       /* modes */
            y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
            return 0;
        }
    } else {
        modifiers = y11_wire_get16(body + 18);
        button = pkt[1];
        if (button > 5) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, button, pkt[0]);
            return 0;
        }
    }

    /* Duplicate registrations by the same client are BadAccess. */
    for (q = y11_passive_grabs; q != NULL; q = q->next) {
        if (q->client == c && q->window == window &&
            q->modifiers == modifiers && q->button == button &&
            q->key == key && q->is_key == (is_key != 0)) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_ACCESS, window, pkt[0]);
            return 0;
        }
    }

    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    p->client = c;
    p->window = window;
    p->modifiers = modifiers;
    p->button = button;
    p->key = key;
    p->owner_events = pkt[1] != 0;
    p->is_key = is_key != 0;
    if (is_key) {
        p->pointer_mode = body[3];
        p->keyboard_mode = body[4];
    } else {
        p->event_mask = y11_wire_get16(body + 4);
        p->confine_to = y11_wire_get32(body + 8);
        p->pointer_mode = body[2];
        p->keyboard_mode = body[3];
    }
    p->next = y11_passive_grabs;
    y11_passive_grabs = p;
    return 0;                   /* no reply */
}

/* GrabButton (opcode 28). */
int y11_grab_req_button(struct y11_client *c, const uint8_t *pkt,
                        size_t len, size_t data_off)
{
    if (len - data_off != 20u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    return y11_grab_register_passive(c, pkt, data_off, 0);
}

/* GrabKey (opcode 33). */
int y11_grab_req_key(struct y11_client *c, const uint8_t *pkt,
                     size_t len, size_t data_off)
{
    if (len - data_off != 12u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    return y11_grab_register_passive(c, pkt, data_off, 1);
}

static int y11_grab_unregister_passive(struct y11_client *c,
                                       const uint8_t *pkt, size_t data_off,
                                       int is_key)
{
    const uint8_t *body = pkt + data_off;
    yid_t window = y11_wire_get32(body + 0);
    uint16_t modifiers = y11_wire_get16(body + 4);
    uint8_t button = is_key ? 0 : pkt[1];
    uint8_t key = is_key ? pkt[1] : 0;
    struct y11_passive_grab **link = &y11_passive_grabs;

    while (*link != NULL) {
        struct y11_passive_grab *p = *link;

        if (p->client == c && p->window == window &&
            p->modifiers == modifiers && p->button == button &&
            p->key == key && p->is_key == (is_key != 0)) {
            *link = p->next;
            free(p);
            return 0;
        }
        link = &p->next;
    }
    return 0;                   /* unmatched ungrabs are no-ops */
}

/* UngrabButton (opcode 29). */
int y11_grab_req_ungrab_button(struct y11_client *c, const uint8_t *pkt,
                               size_t len, size_t data_off)
{
    if (len - data_off != 8u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    return y11_grab_unregister_passive(c, pkt, data_off, 0);
}

/* UngrabKey (opcode 34). */
int y11_grab_req_ungrab_key(struct y11_client *c, const uint8_t *pkt,
                            size_t len, size_t data_off)
{
    if (len - data_off != 8u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    return y11_grab_unregister_passive(c, pkt, data_off, 1);
}

/*
 * GrabKeyboard (opcode 31): owner-events@byte 1, grab window, time,
 * modes.  Replies with a status.
 */
int y11_grab_req_keyboard(struct y11_client *c, const uint8_t *pkt,
                          size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t grab_window, time;
    uint8_t status;

    if (len - data_off != 12u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    grab_window = y11_wire_get32(body + 0);
    time = y11_wire_get32(body + 4);

    if (body[4] > 1 || body[5] > 1) {   /* pointer/keyboard mode */
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }

    status = y11_grab_check_window(grab_window, 1);
    if (status != Y11_GRAB_SUCCESS)
        goto reply;
    if (y11_keyboard_grab.active) {
        status = Y11_GRAB_ALREADY;
        goto reply;
    }
    if (!y11_grab_time_ok(time)) {
        status = Y11_GRAB_INVALID_TIME;
        goto reply;
    }

    y11_keyboard_grab.active = true;
    y11_keyboard_grab.client = c;
    y11_keyboard_grab.grab_window = grab_window;
    y11_keyboard_grab.event_mask = Y11_MASK_KEY_PRESS |
                                    Y11_MASK_KEY_RELEASE;
    y11_keyboard_grab.owner_events = pkt[1] != 0;
    y11_keyboard_grab.pointer_mode = body[4];
    y11_keyboard_grab.keyboard_mode = body[5];
    y11_keyboard_grab.passive = false;
    y11_keyboard_grab.time = y11_input_event_time();
    y11_last_grab_time = y11_keyboard_grab.time;
    status = Y11_GRAB_SUCCESS;

reply:
    y11_grab_send_reply(c, status);
    return 0;
}

/* UngrabKeyboard (opcode 32): time. */
int y11_grab_req_ungrab_keyboard(struct y11_client *c, const uint8_t *pkt,
                                 size_t len, size_t data_off)
{
    uint32_t time;

    (void)c;
    if (len - data_off != 4u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    time = y11_wire_get32(pkt + data_off);
    if (!y11_keyboard_grab.active)
        return 0;
    if (time != 0 && time < y11_keyboard_grab.time)
        return 0;
    memset(&y11_keyboard_grab, 0, sizeof(y11_keyboard_grab));
    y11_last_grab_time = y11_input_event_time();
    return 0;                   /* no reply */
}

/* ---- lifecycle -------------------------------------------------------------------- */

/* Release a client's grabs (active and passive) at disconnect. */
void y11_grab_purge_client(struct y11_client *c)
{
    struct y11_passive_grab **link = &y11_passive_grabs;

    if (y11_pointer_grab.active && y11_pointer_grab.client == c)
        memset(&y11_pointer_grab, 0, sizeof(y11_pointer_grab));
    if (y11_keyboard_grab.active && y11_keyboard_grab.client == c)
        memset(&y11_keyboard_grab, 0, sizeof(y11_keyboard_grab));

    while (*link != NULL) {
        struct y11_passive_grab *p = *link;

        if (p->client == c) {
            *link = p->next;
            free(p);
        } else {
            link = &p->next;
        }
    }
}
