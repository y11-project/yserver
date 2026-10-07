/*
 * window.c - Window allocation, tree hierarchy, coordinate calculations
 * and attribute logic for the Y11 display server.
 *
 * The window tree is an N-ary tree built from intrusive doubly-linked
 * lists (parent, first_child, last_child, prev_sibling, next_sibling)
 * for O(1) reparenting and stacking reorders.  The stacking order runs
 * bottom-to-top from first_child to last_child, and newly created
 * windows enter at the bottom.
 *
 * Per the X11 wire standard, a window's (x, y) is the position of its
 * upper-left outer corner relative to the upper-left inner corner of
 * its parent, so abs_x = parent->abs_x + parent->border_width + x.
 * The absolute coordinate cache is invalidated recursively whenever a
 * window moves or is reparented.
 *
 * Substructure redirection contract: only one client at a time may
 * select SubstructureRedirectMask on a window.  While a redirecting
 * client (the window manager) owns a parent, MapWindow and
 * ConfigureWindow requests on its non-override-redirect children are
 * forwarded as MapRequest and ConfigureRequest events instead of being
 * applied directly.
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

static struct y11_window *y11_root;     /* the screen's root window */

static void y11_window_destroy_tree(struct y11_window *win);

/* Screen geometry: defaults, overridden by the hardware output mode. */
uint16_t y11_screen_width = (uint16_t)Y11_SCREEN_WIDTH;
uint16_t y11_screen_height = (uint16_t)Y11_SCREEN_HEIGHT;

/* ---- small helpers --------------------------------------------------------- */

unsigned y11_popcount32(uint32_t v)
{
    unsigned n = 0;
    while (v != 0) {
        v &= v - 1;
        n++;
    }
    return n;
}

struct y11_window *y11_window_get(yid_t id)
{
    return y11_resource_get(id, Y11_RESOURCE_WINDOW);
}

/*
 * (Re)allocate a window's backing pixel buffer for the current
 * geometry, preserving the top-left content on resize.  InputOnly
 * windows carry no buffer.  Returns -1 on allocation failure.
 */
int y11_window_sync_drawable(struct y11_window *win)
{
    size_t stride, size;
    uint32_t *pixels;

    win->drawable.id = win->id;
    win->drawable.type = Y11_DRAWABLE_WINDOW;
    win->drawable.width = win->width;
    win->drawable.height = win->height;
    win->drawable.depth = win->depth;
    win->drawable.bpp = 32;
    win->drawable.stride = 0;

    if (win->window_class == Y11_WINDOW_CLASS_INPUT_ONLY ||
        win->width == 0 || win->height == 0) {
        y11_window_free_drawable(win);
        return 0;
    }

    stride = (size_t)win->width * 4u;
    if (stride > (size_t)-1 / win->height)
        return -1;
    size = stride * (size_t)win->height;

    pixels = calloc(1, size);
    if (pixels == NULL)
        return -1;

    /* Preserve the overlapping content on resize. */
    if (win->drawable.pixels != NULL) {
        uint16_t old_w = (uint16_t)(win->drawable.stride / 4u);
        uint16_t copy_w = old_w < win->width ? old_w : win->width;
        uint16_t old_h = win->drawable.height;
        uint16_t copy_h = old_h < win->height ? old_h : win->height;
        uint16_t row;

        for (row = 0; row < copy_h; row++) {
            memcpy((uint8_t *)pixels + (size_t)row * stride,
                   (uint8_t *)win->drawable.pixels +
                       (size_t)row * win->drawable.stride,
                   (size_t)copy_w * 4u);
        }
        free(win->drawable.pixels);
    }

    win->drawable.pixels = pixels;
    win->drawable.stride = stride;
    return 0;
}

void y11_window_free_drawable(struct y11_window *win)
{
    free(win->drawable.pixels);
    win->drawable.pixels = NULL;
    win->drawable.stride = 0;
}

/* The exclusive event-mask bits: only one client may select each. */
static uint32_t y11_exclusive_mask_bits(void)
{
    return Y11_MASK_SUBSTRUCTURE_REDIRECT | Y11_MASK_RESIZE_REDIRECT |
           Y11_MASK_BUTTON_PRESS;
}

/* Is `win` an ancestor of `w` (or equal to it)? */
static int y11_window_is_ancestor(const struct y11_window *win,
                                  const struct y11_window *w)
{
    for (; w != NULL; w = w->parent) {
        if (w == win)
            return 1;
    }
    return 0;
}

/* ---- tree mechanics ------------------------------------------------------------ */

/* Unlink from the parent's child list (O(1)). */
static void y11_window_detach(struct y11_window *win)
{
    struct y11_window *parent = win->parent;

    if (parent == NULL)
        return;
    if (win->prev_sibling != NULL)
        win->prev_sibling->next_sibling = win->next_sibling;
    else
        parent->first_child = win->next_sibling;
    if (win->next_sibling != NULL)
        win->next_sibling->prev_sibling = win->prev_sibling;
    else
        parent->last_child = win->prev_sibling;
    win->parent = NULL;
    win->prev_sibling = NULL;
    win->next_sibling = NULL;
}

/*
 * Insert at the top of the parent's stacking order.  The X protocol
 * places newly created and newly reparented windows above their
 * siblings, and the stacking list runs bottom (first_child) to top
 * (last_child).
 */
static void y11_window_attach_top(struct y11_window *parent,
                                  struct y11_window *child)
{
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    child->next_sibling = NULL;
    if (parent->last_child != NULL)
        parent->last_child->next_sibling = child;
    else
        parent->first_child = child;
    parent->last_child = child;
}

/* Insert directly above `sibling` (higher in the bottom-to-top list).
 * `sibling` must already be a sibling of `win`. */
static void y11_window_attach_above(struct y11_window *win,
                                    struct y11_window *sibling)
{
    struct y11_window *parent = sibling->parent;

    if (sibling == win || parent == NULL || win->parent != parent)
        return;
    y11_window_detach(win);
    win->parent = parent;
    win->prev_sibling = sibling;
    win->next_sibling = sibling->next_sibling;
    if (sibling->next_sibling != NULL)
        sibling->next_sibling->prev_sibling = win;
    else
        parent->last_child = win;
    sibling->next_sibling = win;
}

/* Insert directly below `sibling` (lower in the bottom-to-top list). */
static void y11_window_attach_below(struct y11_window *win,
                                    struct y11_window *sibling)
{
    struct y11_window *parent = sibling->parent;

    if (sibling == win || parent == NULL || win->parent != parent)
        return;
    y11_window_detach(win);
    win->parent = parent;
    win->next_sibling = sibling;
    win->prev_sibling = sibling->prev_sibling;
    if (sibling->prev_sibling != NULL)
        sibling->prev_sibling->next_sibling = win;
    else
        parent->first_child = win;
    sibling->prev_sibling = win;
}

/* Move to the very top or very bottom of the parent's stacking order. */
static void y11_window_attach_extreme(struct y11_window *win, int topmost)
{
    struct y11_window *parent = win->parent;

    if (parent == NULL)
        return;
    if (topmost && win == parent->last_child)
        return;
    if (!topmost && win == parent->first_child)
        return;
    y11_window_detach(win);
    if (topmost) {
        win->parent = parent;
        win->prev_sibling = parent->last_child;
        win->next_sibling = NULL;
        parent->last_child->next_sibling = win;
        parent->last_child = win;
    } else {
        win->parent = parent;
        win->prev_sibling = NULL;
        win->next_sibling = parent->first_child;
        parent->first_child->prev_sibling = win;
        parent->first_child = win;
    }
}

/* ---- absolute coordinates ------------------------------------------------------- */

/*
 * Recompute the absolute coordinate cache for `win` and all descendants
 * (the parent's origin is inside its border).
 */
static void y11_window_recompute_abs(struct y11_window *win)
{
    struct y11_window *child;

    if (win->parent != NULL) {
        win->abs_x = win->parent->abs_x + (int32_t)win->parent->border_width +
                     win->x;
        win->abs_y = win->parent->abs_y + (int32_t)win->parent->border_width +
                     win->y;
    } else {
        win->abs_x = win->x;
        win->abs_y = win->y;
    }
    for (child = win->first_child; child != NULL; child = child->next_sibling)
        y11_window_recompute_abs(child);
}

/* ---- map state --------------------------------------------------------------------- */

static int y11_window_parent_viewable(const struct y11_window *win)
{
    return win->parent != NULL &&
           win->parent->map_state == Y11_MAP_STATE_VIEWABLE;
}

/*
 * Propagate a viewability change down the tree: children that are mapped
 * but unviewable flip to viewable when the parent becomes viewable, and
 * viewable children flip to unviewable when it stops being viewable.
 * These transitions are silent; only explicit map/unmap requests
 * generate events.
 */
static void y11_window_propagate_map_state(struct y11_window *win,
                                          int viewable)
{
    struct y11_window *child;

    for (child = win->first_child; child != NULL; child = child->next_sibling) {
        if (viewable && child->map_state == Y11_MAP_STATE_UNVIEWABLE)
            child->map_state = Y11_MAP_STATE_VIEWABLE;
        else if (!viewable && child->map_state == Y11_MAP_STATE_VIEWABLE)
            child->map_state = Y11_MAP_STATE_UNVIEWABLE;
        y11_window_propagate_map_state(child, viewable);
    }
}

/* ---- event subscriptions ------------------------------------------------------------ */

static void y11_window_recompute_masks(struct y11_window *win)
{
    uint32_t masks = 0;
    const struct y11_event_sub *sub;

    for (sub = win->event_subs; sub != NULL; sub = sub->next)
        masks |= sub->mask;
    win->all_event_masks = masks;
}

/* Remove the calling client's subscription from `win`, if any. */
static void y11_window_remove_sub(struct y11_window *win, struct y11_client *c)
{
    struct y11_event_sub **link = &win->event_subs;

    while (*link != NULL) {
        struct y11_event_sub *sub = *link;
        if (sub->client == c) {
            *link = sub->next;
            free(sub);
            y11_window_recompute_masks(win);
            return;
        }
        link = &sub->next;
    }
}

/*
 * Install `mask` as client `c`'s event subscription on `win`.  Returns 0
 * on success, Y11_ERR_BAD_VALUE for undefined bits, or Y11_ERR_BAD_ACCESS
 * when another client already holds one of the exclusive bits
 * (SubstructureRedirectMask, ResizeRedirectMask, ButtonPressMask).
 */
static uint8_t y11_window_apply_event_mask(struct y11_window *win,
                                           struct y11_client *c,
                                           uint32_t mask)
{
    struct y11_event_sub *sub, *mine = NULL;

    if ((mask & ~Y11_MASK_ALL_VALID) != 0)
        return Y11_ERR_BAD_VALUE;

    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if (sub->client == c) {
            mine = sub;
        } else if ((sub->mask & mask & y11_exclusive_mask_bits()) != 0) {
            return Y11_ERR_BAD_ACCESS;   /* exclusive bit already held */
        }
    }

    if (mine == NULL && mask != 0) {
        mine = malloc(sizeof(*mine));
        if (mine == NULL)
            return Y11_ERR_BAD_ALLOC;
        mine->client = c;
        mine->mask = mask;
        mine->next = win->event_subs;
        win->event_subs = mine;
    } else if (mine != NULL) {
        if (mask == 0) {
            y11_window_remove_sub(win, c);
            mine = NULL;
        } else {
            mine->mask = mask;
        }
    }
    y11_window_recompute_masks(win);

    /* Track substructure-redirect ownership for the WM contract. */
    if ((mask & Y11_MASK_SUBSTRUCTURE_REDIRECT) != 0)
        win->substructure_redirect_client = c;
    else if (win->substructure_redirect_client == c)
        win->substructure_redirect_client = NULL;

    return 0;
}

static void y11_window_purge_tree(struct y11_window *win, struct y11_client *c)
{
    struct y11_window *child;

    if (win->substructure_redirect_client == c)
        win->substructure_redirect_client = NULL;
    y11_window_remove_sub(win, c);

    for (child = win->first_child; child != NULL; child = child->next_sibling)
        y11_window_purge_tree(child, c);
}

/*
 * Drop every subscription `c` holds (called when the client disconnects):
 * its per-window event masks and any substructure-redirect ownership.
 */
void y11_events_purge_client(struct y11_client *c)
{
    struct y11_window *root = y11_window_get(Y11_SCREEN_ROOT);

    if (root != NULL)
        y11_window_purge_tree(root, c);
}

/*
 * Recursively destroy windows owned by client `c` (called on
 * disconnect, after its subscriptions are purged).  Destroying a
 * window takes its whole subtree, whatever the owners.
 */
static void y11_window_destroy_owned_tree(struct y11_window *win,
                                          struct y11_client *c)
{
    struct y11_window *child = win->first_child;

    while (child != NULL) {
        struct y11_window *next = child->next_sibling;

        if (child->owner == c)
            y11_window_destroy_tree(child);
        else
            y11_window_destroy_owned_tree(child, c);
        child = next;
    }
}

void y11_window_destroy_owned(struct y11_client *c)
{
    struct y11_window *root = y11_window_get(Y11_SCREEN_ROOT);

    if (root != NULL)
        y11_window_destroy_owned_tree(root, c);
}

/* ---- lifecycle ------------------------------------------------------------------------ */

static void y11_window_free_subs(struct y11_window *win)
{
    struct y11_event_sub *sub = win->event_subs;

    while (sub != NULL) {
        struct y11_event_sub *next = sub->next;
        free(sub);
        sub = next;
    }
    win->event_subs = NULL;
    win->all_event_masks = 0;
}

/* Free a window and all descendants without dispatching events (shutdown). */
static void y11_window_free_tree(struct y11_window *win)
{
    struct y11_window *child = win->first_child;

    while (child != NULL) {
        struct y11_window *next = child->next_sibling;
        y11_window_free_tree(child);
        child = next;
    }
    y11_window_detach(win);
    y11_window_free_drawable(win);
    y11_property_destroy_all(win);
    y11_window_free_subs(win);
    y11_resource_remove(win->id);
    free(win);
}

/* Destroy a window and its subtree, dispatching DestroyNotify events. */
static void y11_window_destroy_tree(struct y11_window *win)
{
    while (win->first_child != NULL)
        y11_window_destroy_tree(win->first_child);

    y11_event_send_destroy(win);
    y11_window_detach(win);
    y11_window_free_drawable(win);
    y11_property_destroy_all(win);
    y11_window_free_subs(win);
    y11_resource_remove(win->id);
    free(win);
}

int y11_window_init(void)
{
    struct y11_window *root = calloc(1, sizeof(*root));

    if (root == NULL)
        return -1;
    root->id = Y11_SCREEN_ROOT;
    root->owner = NULL;                 /* server-owned */
    root->x = 0;
    root->y = 0;
    root->width = y11_screen_width;
    root->height = y11_screen_height;
    root->border_width = 0;
    root->abs_x = 0;
    root->abs_y = 0;
    root->depth = 24;
    root->visual_id = Y11_SCREEN_VISUAL;
    root->window_class = Y11_WINDOW_CLASS_INPUT_OUTPUT;
    root->map_state = Y11_MAP_STATE_VIEWABLE;
    root->override_redirect = false;
    root->background_pixel = 0;

    if (y11_resource_add(root->id, Y11_RESOURCE_WINDOW, root) != 0) {
        free(root);
        return -1;
    }
    if (y11_window_sync_drawable(root) != 0) {
        y11_resource_remove(root->id);
        free(root);
        return -1;
    }
    y11_root = root;
    return 0;
}

void y11_window_shutdown(void)
{
    if (y11_root != NULL) {
        y11_window_free_tree(y11_root);
        y11_root = NULL;
    }
}

/* ---- ChangeWindowAttributes value lists --------------------------------------------
 *
 * Value lists hold exactly 4 bytes per set mask bit, packed in increasing
 * bit order (the X11 LISTofVALUE encoding).
 */

struct y11_cwa_values {
    uint32_t background_pixel;
    uint32_t border_pixel;
    uint32_t event_mask;
    int have_background;        /* a background was explicitly specified */
    int have_border;
    int have_event_mask;
    int override_redirect;
    int have_override;
};

/*
 * Parse a ChangeWindowAttributes-style value list.  `vals` points at the
 * first 4-byte slot; `nvals` slots must be available (the caller checks
 * the request length first).
 */
static void y11_window_parse_cwa(uint32_t mask, const uint8_t *vals,
                                 struct y11_cwa_values *out)
{
    unsigned i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < 15; i++) {
        if ((mask & (1u << i)) == 0)
            continue;
        {
            uint32_t v = y11_wire_get32(vals);
            vals += 4;
            switch (i) {
            case 0:             /* background-pixmap: None or ParentRelative */
                out->have_background = (v == 1u);  /* ParentRelative counts */
                break;
            case 1:             /* background-pixel */
                out->background_pixel = v;
                out->have_background = 1;
                break;
            case 2:             /* border-pixmap */
                out->have_border = (v == 1u);
                break;
            case 3:             /* border-pixel */
                out->border_pixel = v;
                out->have_border = 1;
                break;
            case 9:             /* override-redirect */
                out->override_redirect = (v != 0);
                out->have_override = 1;
                break;
            case 11:            /* event-mask */
                out->event_mask = v;
                out->have_event_mask = 1;
                break;
            default:            /* accepted and ignored (headless) */
                break;
            }
        }
    }
}

/* ---- map/unmap engine ----------------------------------------------------------------- */

/*
 * Apply the MapWindow semantics to one window: honor substructure
 * redirection, transition the map state, and notify listeners.
 */
static int y11_window_do_map(struct y11_window *win)
{
    if (win->map_state != Y11_MAP_STATE_UNMAPPED)
        return 0;               /* already mapped (viewable or unviewable) */

    /* Substructure redirection: the window manager decides. */
    if (win->parent != NULL &&
        win->parent->substructure_redirect_client != NULL &&
        !win->override_redirect) {
        y11_event_send_map_request(win);
        return 0;
    }

    win->map_state = y11_window_parent_viewable(win)
                         ? Y11_MAP_STATE_VIEWABLE
                         : Y11_MAP_STATE_UNVIEWABLE;
    y11_event_send_map(win);
    y11_window_propagate_map_state(win,
                                   win->map_state == Y11_MAP_STATE_VIEWABLE);
    if (win->map_state == Y11_MAP_STATE_VIEWABLE)
        y11_damage_mapped(win, 0, 0, win->width, win->height);
    return 0;
}

static int y11_window_do_unmap(struct y11_window *win)
{
    int was_viewable;

    if (win->map_state == Y11_MAP_STATE_UNMAPPED)
        return 0;               /* already unmapped: no event */

    was_viewable = (win->map_state == Y11_MAP_STATE_VIEWABLE);
    win->map_state = Y11_MAP_STATE_UNMAPPED;
    y11_event_send_unmap(win, 0);
    if (was_viewable)
        y11_window_propagate_map_state(win, 0);
    return 0;
}

/* ---- request handlers ------------------------------------------------------------------- */

/* BadLength helper for request handlers. */
static int y11_window_bad_length(struct y11_client *c, uint8_t opcode)
{
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, opcode);
    return 0;
}

/*
 * CreateWindow (opcode 1), 32-byte fixed part after the request header:
 * wid, parent, x, y, width, height, border-width, class, visual,
 * value-mask, then the value list.  The depth rides in the header's
 * second byte.
 */
int y11_window_req_create(struct y11_client *c, const uint8_t *pkt,
                          size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail = len - data_off;
    uint32_t value_mask, wid, parent_id, visual;
    uint16_t width, height, border_width, wclass;
    int16_t x, y;
    uint8_t depth;
    unsigned nvalues;
    struct y11_cwa_values vals;
    struct y11_window *parent, *win;
    uint8_t err;

    if (avail < 28u)
        return y11_window_bad_length(c, pkt[0]);
    value_mask = y11_wire_get32(body + 24);
    if ((value_mask & ~Y11_CWA_ALL_VALID) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }
    nvalues = y11_popcount32(value_mask);
    if (avail - 28u < (size_t)nvalues * 4u)
        return y11_window_bad_length(c, pkt[0]);

    wid = y11_wire_get32(body + 0);
    parent_id = y11_wire_get32(body + 4);
    x = (int16_t)y11_wire_get16(body + 8);
    y = (int16_t)y11_wire_get16(body + 10);
    width = y11_wire_get16(body + 12);
    height = y11_wire_get16(body + 14);
    border_width = y11_wire_get16(body + 16);
    wclass = y11_wire_get16(body + 18);
    visual = y11_wire_get32(body + 20);
    depth = pkt[1];            /* header data byte, both request forms */

    y11_window_parse_cwa(value_mask, body + 28, &vals);
    if (vals.have_event_mask &&
        (vals.event_mask & ~Y11_MASK_ALL_VALID) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }

    parent = y11_window_get(parent_id);
    if (parent == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, parent_id, pkt[0]);
        return 0;
    }
    if (width == 0 || height == 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }
    if (wclass != Y11_WINDOW_CLASS_COPY_FROM_PARENT &&
        wclass != Y11_WINDOW_CLASS_INPUT_OUTPUT &&
        wclass != Y11_WINDOW_CLASS_INPUT_ONLY) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }
    if (wclass == Y11_WINDOW_CLASS_COPY_FROM_PARENT)
        wclass = (uint16_t)parent->window_class;

    /* InputOnly windows: no depth, no visuals, no background or border. */
    if (wclass == Y11_WINDOW_CLASS_INPUT_ONLY &&
        (depth != 0 || vals.have_background || vals.have_border ||
         (visual != 0 && visual != parent->visual_id))) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, 0, pkt[0]);
        return 0;
    }
    /* Children of InputOnly windows must themselves be InputOnly. */
    if (parent->window_class == Y11_WINDOW_CLASS_INPUT_ONLY &&
        wclass != Y11_WINDOW_CLASS_INPUT_ONLY) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, 0, pkt[0]);
        return 0;
    }
    if (wclass == Y11_WINDOW_CLASS_INPUT_OUTPUT) {
        if (visual == 0)        /* CopyFromParent */
            visual = parent->visual_id;
        else if (visual != parent->visual_id) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, visual, pkt[0]);
            return 0;
        }
        if (depth == 0)         /* CopyFromParent */
            depth = parent->depth;
        else if (depth != parent->depth) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, depth, pkt[0]);
            return 0;
        }
    } else {
        depth = 0;
        visual = parent->visual_id;
    }

    /* The id must be unused and fall in this client's resource range. */
    if (wid < c->resource_id_base ||
        wid - c->resource_id_base > Y11_RID_MASK ||
        y11_window_get(wid) != NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, wid, pkt[0]);
        return 0;
    }

    win = calloc(1, sizeof(*win));
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    win->id = wid;
    win->owner = c;
    win->x = x;
    win->y = y;
    win->width = width;
    win->height = height;
    win->border_width = border_width;
    win->depth = depth;
    win->visual_id = visual;
    win->window_class = (y11_window_class_t)wclass;
    win->map_state = Y11_MAP_STATE_UNMAPPED;
    win->override_redirect = vals.have_override
                                 ? (vals.override_redirect != 0)
                                 : false;
    win->background_pixel = vals.background_pixel;
    win->border_pixel = vals.border_pixel;

    if (y11_resource_add(win->id, Y11_RESOURCE_WINDOW, win) != 0) {
        free(win);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    if (y11_window_sync_drawable(win) != 0) {
        y11_resource_remove(win->id);
        free(win);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    y11_window_attach_top(parent, win);
    y11_window_recompute_abs(win);

    if (vals.have_event_mask) {
        err = y11_window_apply_event_mask(win, c, vals.event_mask);
        if (err != 0) {
            /* Cannot happen for a fresh window, but stay defensive. */
            y11_window_detach(win);
            y11_resource_remove(win->id);
            y11_window_free_drawable(win);
            y11_window_free_subs(win);
            free(win);
            y11_dispatch_send_error(c, err, 0, pkt[0]);
            return 0;
        }
    }

    /* Broadcast CreateNotify to the parent's SubstructureNotify listeners. */
    y11_event_send_create(win);
    return 0;
}

/*
 * ChangeWindowAttributes (opcode 2): window id, value mask, value list.
 * Enforces the single-client exclusive rule for SubstructureRedirectMask,
 * ResizeRedirectMask and ButtonPressMask (BadAccess on conflict).
 */
int y11_window_req_change_attributes(struct y11_client *c, const uint8_t *pkt,
                                     size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail = len - data_off;
    uint32_t value_mask, window_id;
    unsigned nvalues;
    struct y11_cwa_values vals;
    struct y11_window *win;
    uint8_t err;

    if (avail < 8u)
        return y11_window_bad_length(c, pkt[0]);
    value_mask = y11_wire_get32(body + 4);
    if ((value_mask & ~Y11_CWA_ALL_VALID) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }
    nvalues = y11_popcount32(value_mask);
    if (avail - 8u < (size_t)nvalues * 4u)
        return y11_window_bad_length(c, pkt[0]);

    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    y11_window_parse_cwa(value_mask, body + 8, &vals);

    if (win->window_class == Y11_WINDOW_CLASS_INPUT_ONLY &&
        (vals.have_background || vals.have_border)) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, 0, pkt[0]);
        return 0;
    }

    if (vals.have_background)
        win->background_pixel = vals.background_pixel;
    if (vals.have_border)
        win->border_pixel = vals.border_pixel;
    if (vals.have_override)
        win->override_redirect = vals.override_redirect != 0;

    if (vals.have_event_mask) {
        err = y11_window_apply_event_mask(win, c, vals.event_mask);
        if (err != 0) {
            y11_dispatch_send_error(c, err, 0, pkt[0]);
            return 0;
        }
    }
    return 0;                   /* no reply */
}

/* GetWindowAttributes (opcode 3): a 44-byte extra-large reply. */
int y11_window_req_get_attributes(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    y11_get_window_attributes_reply rep;
    uint32_t window_id;
    struct y11_window *win;
    const struct y11_event_sub *sub;

    if (len - data_off != 4u)
        return y11_window_bad_length(c, pkt[0]);
    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 0;           /* backing-store: NotUseful */
    y11_wire_put32(&rep.hdr.length, (sizeof(rep) - 32u) / 4u);   /* 3 */
    y11_wire_put32(&rep.visual, win->visual_id);
    y11_wire_put16(&rep.class, (uint16_t)win->window_class);
    rep.bit_gravity = 0;       /* ForgetGravity */
    rep.win_gravity = 0;       /* ForgetGravity */
    y11_wire_put32(&rep.backing_bit_planes, 0);
    y11_wire_put32(&rep.backing_pixel, 0);
    rep.save_under = 0;
    rep.map_installed = 1;      /* the default colormap is always installed */
    rep.map_state = (uint8_t)win->map_state;
    rep.override = win->override_redirect ? 1 : 0;
    y11_wire_put32(&rep.colormap, Y11_SCREEN_COLORMAP);
    y11_wire_put32(&rep.all_event_masks, win->all_event_masks);
    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if (sub->client == c) {
            y11_wire_put32(&rep.your_event_mask, sub->mask);
            break;
        }
    }
    y11_wire_put16(&rep.do_not_propagate_mask, 0);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* DestroyWindow (opcode 4). */
int y11_window_req_destroy(struct y11_client *c, const uint8_t *pkt,
                           size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id;
    struct y11_window *win;

    if (len - data_off != 4u)
        return y11_window_bad_length(c, pkt[0]);
    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }
    if (win->parent == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, window_id, pkt[0]);
        return 0;               /* the root window cannot be destroyed */
    }

    y11_window_destroy_tree(win);
    return 0;                   /* no reply */
}

/* DestroySubwindows (opcode 5). */
int y11_window_req_destroy_subwindows(struct y11_client *c, const uint8_t *pkt,
                                      size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id;
    struct y11_window *win;

    if (len - data_off != 4u)
        return y11_window_bad_length(c, pkt[0]);
    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    while (win->first_child != NULL)
        y11_window_destroy_tree(win->first_child);
    return 0;                   /* no reply */
}

/*
 * ClearArea (opcode 61): paint the region with the window's background
 * pixel.  A zero width or height means "to the window edge"; when
 * exposures is set, the window's ExposureMask subscribers receive
 * Expose for the cleared region.
 */
int y11_window_req_clear_area(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id;
    struct y11_window *win;
    int16_t x, y;
    uint32_t w, h;
    uint8_t exposures;

    (void)c;
    if (len - data_off != 12u)  /* window, x, y, width, height */
        goto badlength;
    window_id = y11_wire_get32(body + 0);
    x = (int16_t)y11_wire_get16(body + 4);
    y = (int16_t)y11_wire_get16(body + 6);
    w = y11_wire_get16(body + 8);
    h = y11_wire_get16(body + 10);
    exposures = pkt[1];

    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }
    if (win->drawable.pixels == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, window_id, pkt[0]);
        return 0;
    }

    if (w == 0)
        w = (uint32_t)(win->drawable.width - x < 0
                           ? 0 : win->drawable.width - x);
    if (h == 0)
        h = (uint32_t)(win->drawable.height - y < 0
                           ? 0 : win->drawable.height - y);
    if (x >= (int16_t)win->drawable.width || y >= (int16_t)win->drawable.height)
        return 0;               /* nothing to clear */

    {
        int32_t col, row;

        if (x < 0) {
            if ((uint32_t)(-x) >= w)
                return 0;
            w -= (uint32_t)(-x);
            x = 0;
        }
        if (y < 0) {
            if ((uint32_t)(-y) >= h)
                return 0;
            h -= (uint32_t)(-y);
            y = 0;
        }
        if (x + (int32_t)w > (int32_t)win->drawable.width)
            w = (uint32_t)((int32_t)win->drawable.width - x);
        if (y + (int32_t)h > (int32_t)win->drawable.height)
            h = (uint32_t)((int32_t)win->drawable.height - y);

        for (row = y; row < y + (int32_t)h; row++) {
            for (col = x; col < x + (int32_t)w; col++)
                win->drawable.pixels[(size_t)row * (win->drawable.stride / 4u) +
                                     (size_t)col] = win->background_pixel;
        }
    }

    if (exposures != 0)
        y11_damage_mapped(win, x, y, w, h);
    else
        y11_damage_drawn(&win->drawable, x, y, w, h);

    /* Clearing the root window repaints the hardware scanout. */
    if (win->id == Y11_SCREEN_ROOT)
        y11_scanout_mark_dirty(x, y, w, h);
    return 0;                   /* no reply */

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/*
 * ReparentWindow (opcode 7): window, parent, x, y.  Vital for window
 * managers building decoration frames.
 */
int y11_window_req_reparent(struct y11_client *c, const uint8_t *pkt,
                            size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id, new_parent_id;
    struct y11_window *win, *new_parent, *old_parent;

    if (len - data_off != 12u)
        return y11_window_bad_length(c, pkt[0]);
    window_id = y11_wire_get32(body + 0);
    new_parent_id = y11_wire_get32(body + 4);
    win = y11_window_get(window_id);
    new_parent = y11_window_get(new_parent_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }
    if (new_parent == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, new_parent_id, pkt[0]);
        return 0;
    }
    if (win->parent == NULL ||
        new_parent == win || y11_window_is_ancestor(win, new_parent)) {
        /* The root cannot be reparented, and no cycles are allowed. */
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, window_id, pkt[0]);
        return 0;
    }

    old_parent = win->parent;
    y11_window_detach(win);
    win->x = (int16_t)y11_wire_get16(body + 8);
    win->y = (int16_t)y11_wire_get16(body + 10);
    y11_window_attach_top(new_parent, win);
    y11_window_recompute_abs(win);

    /* Maintain map states across the reparent. */
    if (win->map_state != Y11_MAP_STATE_UNMAPPED) {
        win->map_state = (new_parent->map_state == Y11_MAP_STATE_VIEWABLE)
                             ? Y11_MAP_STATE_VIEWABLE
                             : Y11_MAP_STATE_UNVIEWABLE;
        y11_window_propagate_map_state(win,
                win->map_state == Y11_MAP_STATE_VIEWABLE);
    }

    y11_event_send_reparent(win, old_parent);
    return 0;                   /* no reply */
}

/* MapWindow (opcode 8). */
int y11_window_req_map(struct y11_client *c, const uint8_t *pkt,
                       size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id;
    struct y11_window *win;

    (void)c;
    if (len - data_off != 4u)
        return y11_window_bad_length(c, pkt[0]);
    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }
    return y11_window_do_map(win);
}

/* MapSubwindows (opcode 9): map every unmapped child, bottom to top. */
int y11_window_req_map_subwindows(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id;
    struct y11_window *win, *child;

    (void)c;
    if (len - data_off != 4u)
        return y11_window_bad_length(c, pkt[0]);
    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }
    for (child = win->first_child; child != NULL; child = child->next_sibling)
        y11_window_do_map(child);
    return 0;
}

/* UnmapWindow (opcode 10). */
int y11_window_req_unmap(struct y11_client *c, const uint8_t *pkt,
                        size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t window_id;
    struct y11_window *win;

    (void)c;
    if (len - data_off != 4u)
        return y11_window_bad_length(c, pkt[0]);
    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }
    return y11_window_do_unmap(win);
}

/* ConfigureWindow (opcode 12): window, 16-bit value mask, value list. */
int y11_window_req_configure(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail = len - data_off;
    uint32_t value_mask, window_id;
    unsigned nvalues, i;
    const uint8_t *vals;
    int32_t x, y;
    uint32_t width, height, border_width, sibling, stack_mode;
    struct y11_window *win;

    if (avail < 8u)
        return y11_window_bad_length(c, pkt[0]);
    value_mask = y11_wire_get32(body + 4) & 0xFFFFu;
    if ((value_mask & ~Y11_CW_ALL_VALID) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }
    nvalues = y11_popcount32(value_mask);
    if (avail - 8u < (size_t)nvalues * 4u)
        return y11_window_bad_length(c, pkt[0]);

    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    /* Parse the requested values (defaults = current geometry). */
    x = win->x;
    y = win->y;
    width = win->width;
    height = win->height;
    border_width = win->border_width;
    sibling = 0;
    stack_mode = Y11_STACK_ABOVE;
    vals = body + 8;
    for (i = 0; i < 7; i++) {
        if ((value_mask & (1u << i)) == 0)
            continue;
        {
            uint32_t v = y11_wire_get32(vals);
            vals += 4;
            switch (i) {
            case 0: x = (int32_t)(int16_t)(uint16_t)v; break;   /* CWX */
            case 1: y = (int32_t)(int16_t)(uint16_t)v; break;   /* CWY */
            case 2: width = v; break;                           /* CWWidth */
            case 3: height = v; break;                           /* CWHeight */
            case 4: border_width = v; break;                     /* CWBorderWidth */
            case 5: sibling = v; break;                           /* CWSibling */
            case 6: stack_mode = v; break;                        /* CWStackMode */
            default: break;
            }
        }
    }

    /*
     * Substructure redirection: do not apply the changes.  Forward the
     * requested geometry to the window manager as a ConfigureRequest.
     */
    if (win->parent != NULL &&
        win->parent->substructure_redirect_client != NULL &&
        !win->override_redirect) {
        y11_event_send_configure_request(win, value_mask, x, y, width,
                                         height, border_width, sibling,
                                         (uint8_t)stack_mode);
        return 0;
    }

    if (((value_mask & Y11_CW_WIDTH) != 0 && width == 0) ||
        ((value_mask & Y11_CW_HEIGHT) != 0 && height == 0)) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }

    /* Apply the geometry update and invalidate the absolute caches. */
    if ((value_mask & Y11_CW_X) != 0)
        win->x = (int16_t)x;
    if ((value_mask & Y11_CW_Y) != 0)
        win->y = (int16_t)y;
    if ((value_mask & Y11_CW_WIDTH) != 0)
        win->width = (uint16_t)width;
    if ((value_mask & Y11_CW_HEIGHT) != 0)
        win->height = (uint16_t)height;
    if ((value_mask & Y11_CW_BORDER_WIDTH) != 0)
        win->border_width = (uint16_t)border_width;
    if ((value_mask & (Y11_CW_X | Y11_CW_Y | Y11_CW_BORDER_WIDTH)) != 0)
        y11_window_recompute_abs(win);
    if ((value_mask & (Y11_CW_WIDTH | Y11_CW_HEIGHT)) != 0) {
        if (y11_window_sync_drawable(win) != 0) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
            return 0;
        }
    }

    /* Restack. */
    if ((value_mask & Y11_CW_STACK_MODE) != 0) {
        if (stack_mode == Y11_STACK_ABOVE || stack_mode == Y11_STACK_BELOW) {
            struct y11_window *sib = y11_window_get(sibling);

            if ((value_mask & Y11_CW_SIBLING) == 0 || sib == NULL ||
                sib == win || sib->parent != win->parent) {
                y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, sibling, pkt[0]);
                return 0;
            }
            if (stack_mode == Y11_STACK_ABOVE)
                y11_window_attach_above(win, sib);
            else
                y11_window_attach_below(win, sib);
        } else if (stack_mode == Y11_STACK_TOP_IF) {
            y11_window_attach_extreme(win, 1);
        } else if (stack_mode == Y11_STACK_BOTTOM_IF) {
            y11_window_attach_extreme(win, 0);
        }
        /* Y11_STACK_OPPOSITE is not applied (no occlusion model yet). */
    }

    y11_event_send_configure(win);
    return 0;                   /* no reply */
}

/* GetGeometry (opcode 14): a 32-byte reply about a drawable. */
int y11_window_req_get_geometry(struct y11_client *c, const uint8_t *pkt,
                                size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    y11_get_geometry_reply rep;
    uint32_t window_id;
    struct y11_window *win;

    if (len - data_off != 4u)
        return y11_window_bad_length(c, pkt[0]);
    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE, window_id, pkt[0]);
        return 0;
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = win->depth;
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put32(&rep.root, Y11_SCREEN_ROOT);
    y11_wire_put16(&rep.x, (uint16_t)win->x);
    y11_wire_put16(&rep.y, (uint16_t)win->y);
    y11_wire_put16(&rep.width, win->width);
    y11_wire_put16(&rep.height, win->height);
    y11_wire_put16(&rep.border_width, win->border_width);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* QueryTree (opcode 15): 32-byte reply plus the child list. */
int y11_window_req_query_tree(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    y11_query_tree_reply rep;
    uint32_t window_id;
    struct y11_window *win, *child;
    uint32_t count = 0;
    uint8_t *tail;

    if (len - data_off != 4u)
        return y11_window_bad_length(c, pkt[0]);
    window_id = y11_wire_get32(body + 0);
    win = y11_window_get(window_id);
    if (win == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_WINDOW, window_id, pkt[0]);
        return 0;
    }

    for (child = win->first_child; child != NULL; child = child->next_sibling)
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
            for (child = win->first_child; child != NULL;
                 child = child->next_sibling)
                y11_wire_put32(tail + (size_t)i++ * 4u, child->id);
        }
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put32(&rep.hdr.length, count);   /* one 4-byte unit per child */
    y11_wire_put32(&rep.root, Y11_SCREEN_ROOT);
    y11_wire_put32(&rep.parent,
                   win->parent != NULL ? win->parent->id : 0u);
    y11_wire_put16(&rep.n_children, (uint16_t)count);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    if (tail != NULL) {
        y11_client_send(c, tail, (size_t)count * 4u);
        free(tail);
    }
    return 0;
}
