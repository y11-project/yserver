/*
 * selection.c - Selection ownership and clipboard transfer for the
 * Y11 display server.
 *
 * SetSelectionOwner records who owns each selection atom (PRIMARY,
 * CLIPBOARD, ...), notifying the previous owner with SelectionClear.
 * ConvertSelection forwards a request to the owner as a
 * SelectionRequest event; the owner answers by sending a
 * SelectionNotify event to the requestor through SendEvent.  When no
 * owner exists the server answers the requestor itself with an empty
 * SelectionNotify, exactly like a full X server.
 *
 * All three selection events are addressed, not masked: they go to
 * the specific client that owns the selection or made the request.
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

struct y11_selection {
    struct y11_selection *next;
    struct y11_client   *client;    /* connection that owns the selection */
    yid_t               selection;  /* the selection atom */
    yid_t               owner;      /* owner window id */
    uint32_t            time;       /* ownership timestamp */
};

static struct y11_selection *y11_selections;

static struct y11_selection *y11_selection_find(yid_t selection)
{
    struct y11_selection *s;

    for (s = y11_selections; s != NULL; s = s->next) {
        if (s->selection == selection)
            return s;
    }
    return NULL;
}

/* Drop the ownership of every selection held by a client's window. */
void y11_selection_drop_window(yid_t window)
{
    struct y11_selection **link = &y11_selections;

    while (*link != NULL) {
        struct y11_selection *s = *link;

        if (s->owner == window) {
            *link = s->next;
            free(s);
        } else {
            link = &s->next;
        }
    }
}

/* Drop every selection a client owns (disconnect cleanup). */
void y11_selection_purge_client(struct y11_client *c)
{
    struct y11_selection **link = &y11_selections;

    while (*link != NULL) {
        struct y11_selection *s = *link;

        if (s->client == c) {
            *link = s->next;
            free(s);
        } else {
            link = &s->next;
        }
    }
}

/*
 * SetSelectionOwner (opcode 22): owner, selection, time.
 * Owner None releases; an earlier timestamp than the current
 * ownership loses the race silently.
 */
int y11_selection_req_set_owner(struct y11_client *c, const uint8_t *pkt,
                                size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t owner_id, selection, time;
    struct y11_selection *s, *prev;
    uint32_t now;

    if (len - data_off != 12u)
        return y11_dispatch_bad_length(c, pkt[0]);

    owner_id = y11_wire_get32(body + 0);
    selection = y11_wire_get32(body + 4);
    time = y11_wire_get32(body + 8);
    now = time != 0 ? time : y11_input_event_time();

    s = y11_selection_find(selection);
    if (owner_id == 0) {
        /* Releasing: only the current owner may release it. */
        if (s != NULL && s->client == c) {
            struct y11_selection **link = &y11_selections;

            while (*link != s)
                link = &(*link)->next;
            *link = s->next;
            free(s);
        }
        return 0;               /* no reply */
    }

    if (y11_window_get(owner_id) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, owner_id, pkt[0]);
        return 0;
    }

    /* A later timestamp already holds the selection: the request loses. */
    if (s != NULL && s->time > now)
        return 0;

    prev = s;
    if (s == NULL) {
        s = calloc(1, sizeof(*s));
        if (s == NULL) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
            return 0;
        }
        s->selection = selection;
        s->next = y11_selections;
        y11_selections = s;
    }

    /* Notify the previous owner when another client takes over. */
    if (prev != NULL && prev->client != c) {
        uint8_t ev[32];

        memset(ev, 0, sizeof(ev));
        ev[0] = 29;             /* SelectionClear */
        y11_wire_put32(ev + 4, now);
        y11_wire_put32(ev + 8, prev->owner);
        y11_wire_put32(ev + 12, selection);
        y11_event_dispatch32(prev->client, ev, sizeof(ev));
    }

    s->client = c;
    s->owner = owner_id;
    s->time = now;
    return 0;                   /* no reply */
}

/*
 * GetSelectionOwner (opcode 23): the owner window id, or None.
 */
int y11_selection_req_get_owner(struct y11_client *c, const uint8_t *pkt,
                                size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t selection;
    struct y11_selection *s;
    uint8_t rep[32];

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    selection = y11_wire_get32(body + 0);
    s = y11_selection_find(selection);

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    y11_wire_put32(rep + 8, s != NULL ? s->owner : 0);
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/*
 * ConvertSelection (opcode 24): forward the request to the selection
 * owner as SelectionRequest; with no owner, answer the requestor
 * directly with an empty SelectionNotify (property None).
 */
int y11_selection_req_convert(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t requestor, selection, target, property, time;
    struct y11_selection *s;
    struct y11_window *win;
    uint8_t ev[32];

    if (len - data_off != 20u)
        return y11_dispatch_bad_length(c, pkt[0]);

    requestor = y11_wire_get32(body + 0);
    selection = y11_wire_get32(body + 4);
    target = y11_wire_get32(body + 8);
    property = y11_wire_get32(body + 12);
    time = y11_wire_get32(body + 16);

    win = y11_window_get(requestor);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, requestor, pkt[0]);
        return 0;
    }
    if (y11_atom_name(selection) == NULL ||
        y11_atom_name(target) == NULL ||
        (property != 0 && y11_atom_name(property) == NULL)) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ATOM, 0, pkt[0]);
        return 0;
    }

    if (time == 0)
        time = y11_input_event_time();

    s = y11_selection_find(selection);
    memset(ev, 0, sizeof(ev));
    if (s == NULL) {
        /* Nobody owns it: the empty notify says so. */
        ev[0] = 31;             /* SelectionNotify */
        y11_wire_put32(ev + 4, time);
        y11_wire_put32(ev + 8, requestor);
        y11_wire_put32(ev + 12, selection);
        y11_wire_put32(ev + 16, target);
        y11_wire_put32(ev + 20, 0);   /* property None */
        y11_event_dispatch32(c, ev, sizeof(ev));
        return 0;
    }

    ev[0] = 30;                 /* SelectionRequest */
    y11_wire_put32(ev + 4, time);
    y11_wire_put32(ev + 8, s->owner);
    y11_wire_put32(ev + 12, requestor);
    y11_wire_put32(ev + 16, selection);
    y11_wire_put32(ev + 20, target);
    y11_wire_put32(ev + 24, property);
    y11_event_dispatch32(s->client, ev, sizeof(ev));
    return 0;                   /* no reply */
}
