/*
 * input.c - Pointer and keyboard state, device event routing and the
 * single ingestion point for input events in the Y11 display server.
 *
 * Hardware backends (libinput over libseat) feed the y11_input_*
 * functions; on hosts without the input stack the same entry points
 * are driven by the XTEST extension's FakeInput request, which is how
 * tools like xdotool inject keystrokes and pointer motion.
 *
 * Routing follows the X11 model: an active grab takes strict priority,
 * passive grabs activate implicitly on matching button/key presses, and
 * otherwise events walk up the window hierarchy from the window under
 * the cursor (pointer) or the focus window (keyboard) until a client
 * has selected the right event mask.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "y11.h"
#include "y11_wire.h"

static y11_pointer_t y11_pointer_state;
static y11_keyboard_t y11_keyboard_state;
static struct timeval y11_input_start;

/* keycode -> X11 modifier, standard evdev keycodes (scancode + 8) */
/* Static modifier-key table: keycode -> X modifier mask.  Used by the
 * state tracking and exposed to XKB's modifier-map section. */
static const struct {
    uint8_t keycode;
    uint16_t mask;
} y11_modifier_keys[] = {
    { 50,  0x0001 },            /* Shift_L   -> ShiftMask */
    { 62,  0x0001 },            /* Shift_R   -> ShiftMask */
    { 66,  0x0002 },            /* Caps_Lock -> LockMask */
    { 37,  0x0004 },            /* Control_L -> ControlMask */
    { 105, 0x0004 },            /* Control_R -> ControlMask */
    { 64,  0x0008 },            /* Alt_L     -> Mod1Mask */
    { 108, 0x0008 },            /* Alt_R     -> Mod1Mask */
    { 77,  0x0010 },            /* Num_Lock  -> Mod2Mask */
    { 133, 0x0040 },            /* Super_L   -> Mod4Mask */
    { 134, 0x0040 }             /* Super_R   -> Mod4Mask */
};

/*
 * Minimal standard keyboard mapping (2 keysyms per keycode) so clients
 * can resolve keysyms.  Unlisted keycodes map to NoSymbol.
 */
#define Y11_KEYSYM_PER_KEYCODE 2

static const struct {
    uint8_t keycode;
    uint32_t keysyms[Y11_KEYSYM_PER_KEYCODE];
} y11_keysym_table[] = {
    { 9,   { 0xff1b, 0xff1b } },            /* Escape */
    { 10,  { 0x31, 0x21 } },                /* 1 / ! */
    { 11,  { 0x32, 0x40 } },                /* 2 / @ */
    { 12,  { 0x33, 0x23 } },                /* 3 / # */
    { 13,  { 0x34, 0x24 } },                /* 4 / $ */
    { 14,  { 0x35, 0x25 } },                /* 5 / % */
    { 15,  { 0x36, 0x5e } },                /* 6 / ^ */
    { 16,  { 0x37, 0x26 } },                /* 7 / & */
    { 17,  { 0x38, 0x2a } },                /* 8 / * */
    { 18,  { 0x39, 0x28 } },                /* 9 / ( */
    { 19,  { 0x30, 0x29 } },                /* 0 / ) */
    { 20,  { 0x2d, 0x5f } },                /* - / _ */
    { 21,  { 0x3d, 0x2b } },                /* = / + */
    { 22,  { 0xff08, 0xff08 } },            /* BackSpace */
    { 23,  { 0xff09, 0xff09 } },            /* Tab */
    { 24,  { 0x71, 0x51 } },                /* q / Q */
    { 25,  { 0x77, 0x57 } },                /* w / W */
    { 26,  { 0x65, 0x45 } },                /* e / E */
    { 27,  { 0x72, 0x52 } },                /* r / R */
    { 28,  { 0x74, 0x54 } },                /* t / T */
    { 29,  { 0x79, 0x59 } },                /* y / Y */
    { 30,  { 0x75, 0x55 } },                /* u / U */
    { 31,  { 0x69, 0x49 } },                /* i / I */
    { 32,  { 0x6f, 0x4f } },                /* o / O */
    { 33,  { 0x70, 0x50 } },                /* p / P */
    { 36,  { 0xff0d, 0xff0d } },            /* Return */
    { 37,  { 0xffe3, 0xffe3 } },            /* Control_L */
    { 38,  { 0x61, 0x41 } },                /* a / A */
    { 39,  { 0x73, 0x53 } },                /* s / S */
    { 40,  { 0x64, 0x44 } },                /* d / D */
    { 41,  { 0x66, 0x46 } },                /* f / F */
    { 42,  { 0x67, 0x47 } },                /* g / G */
    { 43,  { 0x68, 0x48 } },                /* h / H */
    { 44,  { 0x6a, 0x4a } },                /* j / J */
    { 45,  { 0x6b, 0x4b } },                /* k / K */
    { 46,  { 0x6c, 0x4c } },                /* l / L */
    { 50,  { 0xffe1, 0xffe1 } },            /* Shift_L */
    { 52,  { 0x7a, 0x5a } },                /* z / Z */
    { 53,  { 0x78, 0x58 } },                /* x / X */
    { 54,  { 0x63, 0x43 } },                /* c / C */
    { 55,  { 0x76, 0x56 } },                /* v / V */
    { 56,  { 0x62, 0x42 } },                /* b / B */
    { 57,  { 0x6e, 0x4e } },                /* n / N */
    { 58,  { 0x6d, 0x4d } },                /* m / M */
    { 61,  { 0x2f, 0x3f } },                /* / / ? */
    { 62,  { 0xffe2, 0xffe2 } },            /* Shift_R */
    { 64,  { 0xffe9, 0xffe9 } },            /* Alt_L */
    { 65,  { 0x20, 0x20 } },                /* space */
    { 66,  { 0xffe5, 0xffe5 } },            /* Caps_Lock */
    { 67,  { 0xffbe, 0xffbe } },            /* F1 */
    { 68,  { 0xffbf, 0xffbf } },            /* F2 */
    { 69,  { 0xffc0, 0xffc0 } },            /* F3 */
    { 70,  { 0xffc1, 0xffc1 } },            /* F4 */
    { 71,  { 0xffc2, 0xffc2 } },            /* F5 */
    { 72,  { 0xffc3, 0xffc3 } },            /* F6 */
    { 73,  { 0xffc4, 0xffc4 } },            /* F7 */
    { 74,  { 0xffc5, 0xffc5 } },            /* F8 */
    { 75,  { 0xffc6, 0xffc6 } },            /* F9 */
    { 76,  { 0xffc7, 0xffc7 } },            /* F10 */
    { 95,  { 0xffc8, 0xffc8 } },            /* F11 */
    { 96,  { 0xffc9, 0xffc9 } },            /* F12 */
    { 105, { 0xffe4, 0xffe4 } },            /* Control_R */
    { 108, { 0xffea, 0xffea } },            /* Alt_R */
    { 111, { 0xff52, 0xff52 } },            /* Up */
    { 113, { 0xff51, 0xff51 } },            /* Left */
    { 114, { 0xff53, 0xff53 } },            /* Right */
    { 116, { 0xff54, 0xff54 } },            /* Down */
    { 133, { 0xffeb, 0xffeb } },            /* Super_L */
    { 134, { 0xffec, 0xffec } }            /* Super_R */
};

/* ---- timestamps ---------------------------------------------------------- */

uint32_t y11_input_event_time(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint32_t)((tv.tv_sec - y11_input_start.tv_sec) * 1000 +
                      (tv.tv_usec - y11_input_start.tv_usec) / 1000);
}

/* ---- state accessors -------------------------------------------------------- */

const y11_pointer_t *y11_input_pointer(void)
{
    return &y11_pointer_state;
}

const y11_keyboard_t *y11_input_keyboard(void)
{
    return &y11_keyboard_state;
}

int y11_input_init(void)
{
    memset(&y11_pointer_state, 0, sizeof(y11_pointer_state));
    memset(&y11_keyboard_state, 0, sizeof(y11_keyboard_state));
    gettimeofday(&y11_input_start, NULL);

    y11_pointer_state.root_x = (int16_t)(y11_screen_width / 2);
    y11_pointer_state.root_y = (int16_t)(y11_screen_height / 2);
    y11_pointer_state.focus_window = Y11_SCREEN_ROOT;
    y11_keyboard_state.focus_window = 1;   /* PointerRoot */
    y11_keyboard_state.revert_to = 1;      /* RevertToPointerRoot */
    return 0;
}

void y11_input_shutdown(void)
{
}

/* ---- event crafting helpers ---------------------------------------------------- */

/*
 * Fill a key/button/motion event: coordinates relative to the event
 * window's interior origin, child = the immediate child of the event
 * window on the path toward the hit window (None at the hit itself).
 */
static void y11_input_fill_device(y11_key_button_event *ev, uint8_t type,
                                  uint8_t detail,
                                  const struct y11_window *event_win,
                                  const struct y11_window *hit)
{
    const struct y11_window *child;

    memset(ev, 0, sizeof(*ev));
    {
        uint8_t *b = (uint8_t *)ev;
        b[0] = type;
        b[1] = detail;
    }
    y11_wire_put32(&ev->time, y11_input_event_time());
    y11_wire_put32(&ev->root, Y11_SCREEN_ROOT);
    y11_wire_put32(&ev->event, event_win->id);
    child = NULL;
    if (hit != NULL && hit != event_win) {
        const struct y11_window *w;

        for (w = hit; w != NULL; w = w->parent) {
            if (w->parent == event_win) {
                child = w;
                break;
            }
        }
    }
    y11_wire_put32(&ev->child, child != NULL ? child->id : 0u);
    y11_wire_put16(&ev->root_x, (uint16_t)y11_pointer_state.root_x);
    y11_wire_put16(&ev->root_y, (uint16_t)y11_pointer_state.root_y);
    y11_wire_put16(&ev->event_x,
                   (int16_t)(y11_pointer_state.root_x -
                             event_win->abs_x -
                             (int32_t)event_win->border_width));
    y11_wire_put16(&ev->event_y,
                   (int16_t)(y11_pointer_state.root_y -
                             event_win->abs_y -
                             (int32_t)event_win->border_width));
    y11_wire_put16(&ev->state,
                   y11_pointer_state.button_mask |
                   y11_keyboard_state.modifier_mask);
    ev->same_screen = 1;
}

/* Send one device event to every matching subscriber of `win`. */
static void y11_input_deliver(uint8_t type, uint8_t detail, uint32_t mask_bit,
                              const struct y11_window *win,
                              const struct y11_window *hit)
{
    y11_key_button_event ev;
    const struct y11_event_sub *sub;

    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if ((sub->mask & mask_bit) == 0)
            continue;
        y11_input_fill_device(&ev, type, detail, win, hit);
        y11_event_dispatch32(sub->client, &ev, sizeof(ev));
    }
}

/*
 * Walk up from `start` and deliver the event to the first window with
 * a subscriber for `mask_bit`.  Returns the window used, or NULL.
 */
static struct y11_window *y11_input_route(uint8_t type, uint8_t detail,
                                         uint32_t mask_bit,
                                         struct y11_window *start,
                                         const struct y11_window *hit)
{
    struct y11_window *win;

    for (win = start; win != NULL; win = win->parent) {
        const struct y11_event_sub *sub;

        for (sub = win->event_subs; sub != NULL; sub = sub->next) {
            if ((sub->mask & mask_bit) != 0) {
                y11_input_deliver(type, detail, mask_bit, win, hit);
                return win;
            }
        }
    }
    return NULL;
}

/* ---- crossing events ------------------------------------------------------------ */

/* Relation codes for EnterNotify/LeaveNotify detail. */
enum {
    Y11_NOTIFY_ANCESTOR          = 0,
    Y11_NOTIFY_VIRTUAL           = 1,
    Y11_NOTIFY_INFERIOR          = 2,
    Y11_NOTIFY_NONLINEAR         = 3,
    Y11_NOTIFY_NONLINEAR_VIRTUAL = 4
};

static int y11_window_is_ancestor(const struct y11_window *anc,
                                  const struct y11_window *win)
{
    for (; win != NULL; win = win->parent) {
        if (win == anc)
            return 1;
    }
    return 0;
}

static void y11_input_crossing(uint8_t type, uint8_t detail,
                               struct y11_window *win, uint32_t mask_bit)
{
    y11_crossing_event ev;
    const struct y11_event_sub *sub;

    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if ((sub->mask & mask_bit) == 0)
            continue;
        memset(&ev, 0, sizeof(ev));
        {
            uint8_t *b = (uint8_t *)&ev;
            b[0] = type;
            b[1] = detail;
        }
        y11_wire_put32(&ev.time, y11_input_event_time());
        y11_wire_put32(&ev.root, Y11_SCREEN_ROOT);
        y11_wire_put32(&ev.event, win->id);
        y11_wire_put32(&ev.child, 0);     /* pointer is in this window */
        y11_wire_put16(&ev.root_x, (uint16_t)y11_pointer_state.root_x);
        y11_wire_put16(&ev.root_y, (uint16_t)y11_pointer_state.root_y);
        y11_wire_put16(&ev.event_x,
                       (int16_t)(y11_pointer_state.root_x - win->abs_x -
                                 (int32_t)win->border_width));
        y11_wire_put16(&ev.event_y,
                       (int16_t)(y11_pointer_state.root_y - win->abs_y -
                                 (int32_t)win->border_width));
        y11_wire_put16(&ev.state,
                       y11_pointer_state.button_mask |
                       y11_keyboard_state.modifier_mask);
        ev.mode = 0;             /* NotifyNormal */
        ev.flags = 0x02;          /* same-screen */
        y11_event_dispatch32(sub->client, &ev, sizeof(ev));
    }
}

/* ---- pointer motion --------------------------------------------------------------- */

static void y11_input_update_focus_window(struct y11_window *hit)
{
    struct y11_window *old = y11_window_get(y11_pointer_state.focus_window);

    y11_pointer_state.focus_window = hit != NULL ? hit->id : Y11_SCREEN_ROOT;

    if (old == NULL || old == hit)
        return;

    /* LeaveNotify for the vacated window, EnterNotify for the new one. */
    if (y11_window_is_ancestor(old, hit)) {
        /* pointer moved down into a descendant */
        y11_input_crossing(Y11_EVT_LEAVE_NOTIFY, Y11_NOTIFY_INFERIOR, old,
                           Y11_MASK_LEAVE_WINDOW);
        y11_input_crossing(Y11_EVT_ENTER_NOTIFY, Y11_NOTIFY_ANCESTOR, hit,
                           Y11_MASK_ENTER_WINDOW);
    } else if (y11_window_is_ancestor(hit, old)) {
        /* pointer moved up out of a descendant */
        y11_input_crossing(Y11_EVT_LEAVE_NOTIFY, Y11_NOTIFY_ANCESTOR, old,
                           Y11_MASK_LEAVE_WINDOW);
        y11_input_crossing(Y11_EVT_ENTER_NOTIFY, Y11_NOTIFY_INFERIOR, hit,
                           Y11_MASK_ENTER_WINDOW);
    } else {
        y11_input_crossing(Y11_EVT_LEAVE_NOTIFY, Y11_NOTIFY_NONLINEAR, old,
                           Y11_MASK_LEAVE_WINDOW);
        y11_input_crossing(Y11_EVT_ENTER_NOTIFY, Y11_NOTIFY_NONLINEAR, hit,
                           Y11_MASK_ENTER_WINDOW);
    }
}

static void y11_input_do_motion(int16_t x, int16_t y)
{
    struct y11_window *hit;
    const y11_grab_t *grab = y11_grab_pointer_active();

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= (int16_t)y11_screen_width)
        x = (int16_t)(y11_screen_width - 1);
    if (y >= (int16_t)y11_screen_height)
        y = (int16_t)(y11_screen_height - 1);
    if (grab != NULL)
        y11_grab_confine(&x, &y);

    y11_pointer_state.root_x = x;
    y11_pointer_state.root_y = y;

    /* The hardware cursor follows without touching the scanout. */
    y11_scanout_move_cursor(x, y);
    /* A different window under the pointer may own a cursor. */
    y11_cursor_refresh();

    if (grab != NULL) {
        /* An active grab takes strict priority: no crossing events. */
        y11_grab_deliver(grab, Y11_EVT_MOTION_NOTIFY, 0,
                         Y11_MASK_POINTER_MOTION);
        return;
    }

    hit = y11_window_at_point(x, y);
    y11_input_update_focus_window(hit);
    if (hit != NULL)
        y11_input_route(Y11_EVT_MOTION_NOTIFY, 0, Y11_MASK_POINTER_MOTION,
                        hit, hit);
}

void y11_input_motion(int16_t dx, int16_t dy)
{
    y11_input_do_motion((int16_t)(y11_pointer_state.root_x + dx),
                        (int16_t)(y11_pointer_state.root_y + dy));
}

void y11_input_motion_abs(int16_t x, int16_t y)
{
    y11_input_do_motion(x, y);
}

/* ---- buttons ------------------------------------------------------------------------ */

void y11_input_button(int press, uint8_t button)
{
    uint16_t bit;
    struct y11_window *hit;
    const y11_grab_t *grab = y11_grab_pointer_active();

    if (button < 1 || button > 5)
        return;
    bit = (uint16_t)(1u << (7 + button));   /* Button1Mask .. Button5Mask */

    if (press)
        y11_pointer_state.button_mask |= bit;
    else
        y11_pointer_state.button_mask &= (uint16_t)~bit;

    if (grab != NULL) {
        /* An active grab owns all button events. */
        if (press)
            y11_grab_deliver(grab, Y11_EVT_BUTTON_PRESS, button,
                             Y11_MASK_BUTTON_PRESS);
        else
            y11_grab_deliver(grab, Y11_EVT_BUTTON_RELEASE, button,
                             Y11_MASK_BUTTON_RELEASE);
        y11_grab_button_release_check();
        return;
    }

    if (press) {
        /*
         * A passive grab (GrabButton) matching the button and modifier
         * state activates implicitly and receives the press.
         */
        if (y11_grab_match_button(button,
                                  y11_keyboard_state.modifier_mask) != NULL) {
            y11_grab_deliver(y11_grab_pointer_active(),
                             Y11_EVT_BUTTON_PRESS, button,
                             Y11_MASK_BUTTON_PRESS);
            return;
        }
    }

    hit = y11_window_get(y11_pointer_state.focus_window);
    if (hit == NULL)
        hit = y11_window_at_point(y11_pointer_state.root_x,
                                   y11_pointer_state.root_y);
    if (hit == NULL)
        return;

    if (press) {
        y11_input_route(Y11_EVT_BUTTON_PRESS, button,
                        Y11_MASK_BUTTON_PRESS, hit, hit);
    } else {
        y11_input_route(Y11_EVT_BUTTON_RELEASE, button,
                        Y11_MASK_BUTTON_RELEASE, hit, hit);
    }
}

/* ---- keys ----------------------------------------------------------------------------- */

static void y11_input_update_modifiers(uint8_t keycode, int press)
{
    size_t i;

    for (i = 0; i < sizeof(y11_modifier_keys) / sizeof(y11_modifier_keys[0]);
         i++) {
        if (y11_modifier_keys[i].keycode != keycode)
            continue;
        if (press)
            y11_keyboard_state.modifier_mask |= y11_modifier_keys[i].mask;
        else
            y11_keyboard_state.modifier_mask &=
                (uint16_t)~y11_modifier_keys[i].mask;
    }
}

/* Modifier mask a keycode produces (0 when not a modifier key). */
uint16_t y11_input_modifier_mask_for(uint8_t keycode)
{
    size_t i;

    for (i = 0; i < sizeof(y11_modifier_keys) / sizeof(y11_modifier_keys[0]);
         i++) {
        if (y11_modifier_keys[i].keycode == keycode)
            return y11_modifier_keys[i].mask;
    }
    return 0;
}

void y11_input_key(int press, uint8_t keycode)
{
    uint8_t byte = keycode / 8;
    uint8_t bit = (uint8_t)(1u << (keycode % 8));
    struct y11_window *focus;

    if (keycode < 8)
        return;                 /* X11 keycodes start at 8 */

    if (y11_debug)
        fprintf(stderr, "y11: input key %s kc=%u mod=0x%x\n",
                press ? "press" : "release", keycode,
                y11_keyboard_state.modifier_mask);

    if (press)
        y11_keyboard_state.key_state[byte] |= bit;
    else
        y11_keyboard_state.key_state[byte] &= (uint8_t)~bit;
    y11_input_update_modifiers(keycode, press);

    if (press && (y11_keyboard_state.modifier_mask & 0x0004) != 0 &&
        (y11_keyboard_state.modifier_mask & 0x0008) != 0) {
        int vt = 0;

        if (keycode >= 67 && keycode <= 76)
            vt = keycode - 66; /* F1=67 -> 1, ..., F10=76 -> 10 */
        else if (keycode == 95)
            vt = 11;           /* F11 */
        else if (keycode == 96)
            vt = 12;           /* F12 */

        if (vt > 0 && y11_session_request_vt_switch(vt) == 0)
            return;
    }

    {
        const y11_grab_t *grab = y11_grab_keyboard_active();

        if (grab != NULL) {
            /* An active keyboard grab owns all keystrokes. */
            if (press) {
                y11_grab_deliver(grab, Y11_EVT_KEY_PRESS, keycode,
                                 Y11_MASK_KEY_PRESS);
            } else {
                y11_grab_deliver(grab, Y11_EVT_KEY_RELEASE, keycode,
                                 Y11_MASK_KEY_RELEASE);
                y11_grab_key_release_check();
            }
            return;
        }
    }

    if (press) {
        /*
         * A passive key grab (GrabKey) matching the keycode and modifier
         * state activates implicitly and receives the press.
         */
        if (y11_grab_match_key(keycode,
                               y11_keyboard_state.modifier_mask) != NULL) {
            y11_grab_deliver(y11_grab_keyboard_active(),
                             Y11_EVT_KEY_PRESS, keycode,
                             Y11_MASK_KEY_PRESS);
            return;
        }
    }

    if (y11_keyboard_state.focus_window == 0)
        return;                   /* focus None: discard */
    if (y11_keyboard_state.focus_window == 1) {
        focus = y11_window_at_point(y11_pointer_state.root_x,
                                    y11_pointer_state.root_y);
    } else {
        focus = y11_window_get(y11_keyboard_state.focus_window);
        if (focus == NULL) {
            /* stale focus: revert */
            y11_keyboard_state.focus_window = 1;
            focus = y11_window_at_point(y11_pointer_state.root_x,
                                        y11_pointer_state.root_y);
        }
    }
    if (focus == NULL)
        return;

    if (press)
        y11_input_route(Y11_EVT_KEY_PRESS, keycode, Y11_MASK_KEY_PRESS,
                        focus, NULL);
    else
        y11_input_route(Y11_EVT_KEY_RELEASE, keycode, Y11_MASK_KEY_RELEASE,
                        focus, NULL);
}

/* ---- focus events ---------------------------------------------------------------------- */

static void y11_input_focus_event(uint8_t type, struct y11_window *win)
{
    y11_focus_event ev;
    const struct y11_event_sub *sub;

    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if ((sub->mask & Y11_MASK_FOCUS_CHANGE) == 0)
            continue;
        memset(&ev, 0, sizeof(ev));
        {
            uint8_t *b = (uint8_t *)&ev;
            b[0] = type;
        }
        y11_wire_put32(&ev.window, win->id);
        ev.mode = 0;             /* NotifyNormal */
        y11_event_dispatch32(sub->client, &ev, sizeof(ev));
    }
}

/* SetInputFocus (opcode 42): focus window, revert-to, time. */
int y11_input_req_set_input_focus(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t focus_id;
    uint8_t revert_to = pkt[1];
    struct y11_window *win, *old;

    if (len - data_off != 8u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    if (revert_to > 2) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, revert_to, pkt[0]);
        return 0;
    }
    focus_id = y11_wire_get32(body + 0);
    if (focus_id > 1) {
        win = y11_window_get(focus_id);
        if (win == NULL) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, focus_id, pkt[0]);
            return 0;
        }
        if (win->map_state != Y11_MAP_STATE_VIEWABLE) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, focus_id, pkt[0]);
            return 0;
        }
    } else {
        win = NULL;
    }

    old = (y11_keyboard_state.focus_window > 1)
              ? y11_window_get(y11_keyboard_state.focus_window)
              : NULL;
    if (old != NULL && old != win)
        y11_input_focus_event(Y11_EVT_FOCUS_OUT, old);

    y11_keyboard_state.focus_window = win != NULL ? win->id : focus_id;
    y11_keyboard_state.revert_to = revert_to;
    if (win != NULL)
        y11_input_focus_event(Y11_EVT_FOCUS_IN, win);

    return 0;                   /* no reply */
}

/* Client-supplied keycode -> keysym overrides (ChangeKeyboardMapping). */
#define Y11_KEYSYM_OVERRIDE_MAX 248   /* keycodes 8 .. 255 */

static uint32_t y11_keysym_override[Y11_KEYSYM_OVERRIDE_MAX][Y11_KEYSYM_PER_KEYCODE];
static uint8_t y11_keysym_override_set[Y11_KEYSYM_OVERRIDE_MAX];

/* Get the keysyms for one keycode, honoring client overrides. */
void y11_input_keysyms_for(uint8_t keycode, uint32_t out[2])
{
    size_t entry;

    out[0] = 0;
    out[1] = 0;
    if (keycode - 8 < Y11_KEYSYM_OVERRIDE_MAX &&
        y11_keysym_override_set[keycode - 8]) {
        out[0] = y11_keysym_override[keycode - 8][0];
        out[1] = y11_keysym_override[keycode - 8][1];
        return;
    }
    for (entry = 0;
         entry < sizeof(y11_keysym_table) / sizeof(y11_keysym_table[0]);
         entry++) {
        if (y11_keysym_table[entry].keycode == keycode) {
            out[0] = y11_keysym_table[entry].keysyms[0];
            out[1] = y11_keysym_table[entry].keysyms[1];
            return;
        }
    }
}

/*
 * ChangeKeyboardMapping (opcode 100): keyCodes@byte 1, first-keycode
 * and keysyms-per-keycode in the first two data bytes, then the keysym
 * list.  Stores a client-supplied keycode -> keysym overlay used by
 * GetKeyboardMapping, exactly what xdotool's runtime key binding
 * exercises.
 */
int y11_input_req_change_keyboard_mapping(struct y11_client *c,
                                          const uint8_t *pkt, size_t len,
                                          size_t data_off)
{
    uint8_t count = pkt[1];
    uint8_t first = pkt[data_off];
    uint8_t per_kc = pkt[data_off + 1];
    const uint8_t *data = pkt + data_off + 4;
    int i, j;

    if (len - data_off != 4u + (size_t)count * per_kc * 4u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    if (per_kc == 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }

    for (i = 0; i < count; i++) {
        uint8_t keycode = (uint8_t)(first + i);

        if (keycode < 8)
            continue;
        y11_keysym_override_set[keycode - 8] = 1;
        for (j = 0; j < Y11_KEYSYM_PER_KEYCODE; j++) {
            uint32_t ks = j < per_kc
                              ? y11_wire_get32(data +
                                    ((size_t)i * per_kc + (size_t)j) * 4u)
                              : 0;
            y11_keysym_override[keycode - 8][j] = ks;
        }
    }
    return 0;                   /* no reply */
}

/* ---- keyboard mapping ------------------------------------------------------------------- */

/*
 * GetKeyboardMapping (opcode 101): first-keycode and count ride in the
 * header bytes; the reply carries count * 2 keysyms.
 */
int y11_input_req_get_keyboard_mapping(struct y11_client *c,
                                       const uint8_t *pkt, size_t len,
                                       size_t data_off)
{
    y11_get_keyboard_mapping_reply rep;
    uint8_t first = pkt[data_off];
    uint8_t count = pkt[data_off + 1];
    uint8_t *data;
    int i, j;

    (void)len;
    if (count == 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }
    if (first < 8) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, first, pkt[0]);
        return 0;
    }

    data = calloc(count, Y11_KEYSYM_PER_KEYCODE * 4u);
    if (data == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    for (i = 0; i < count; i++) {
        uint8_t keycode = (uint8_t)(first + i);
        uint32_t ks[2];

        y11_input_keysyms_for(keycode, ks);
        for (j = 0; j < Y11_KEYSYM_PER_KEYCODE; j++)
            y11_wire_put32(data +
                               ((size_t)i * Y11_KEYSYM_PER_KEYCODE +
                                (size_t)j) * 4u,
                           ks[j]);
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = Y11_KEYSYM_PER_KEYCODE;
    y11_wire_put32(&rep.hdr.length,
                   (uint32_t)count * Y11_KEYSYM_PER_KEYCODE);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    y11_client_send(c, data, (size_t)count * Y11_KEYSYM_PER_KEYCODE * 4u);
    free(data);
    return 0;
}

/* ---- cleanup ------------------------------------------------------------------------------ */

void y11_input_purge_client(struct y11_client *c)
{
    (void)c;                    /* grabs are released by src/grab.c */
}
