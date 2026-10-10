/*
 * damage.c - Dirty region tracking and Expose dispatch: drawing that
 * overwrites viewable descendants sends them Expose; one event per
 * window per dirty mark (count 0).
 *
 * Copyright (c) 2026 The Y11 Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/*
 * Deliver Expose for the region (in `win`'s drawable coordinates,
 * clipped to the drawable bounds) to `win`'s ExposureMask subscribers.
 */
static void y11_damage_expose(struct y11_window *win, int32_t x, int32_t y,
                              int32_t w, int32_t h)
{
    const struct y11_event_sub *sub;
    y11_expose_event ev;

    if (w <= 0 || h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > (int32_t)win->drawable.width)
        w = (int32_t)win->drawable.width - x;
    if (y + h > (int32_t)win->drawable.height)
        h = (int32_t)win->drawable.height - y;
    if (w <= 0 || h <= 0)
        return;

    for (sub = win->event_subs; sub != NULL; sub = sub->next) {
        if ((sub->mask & Y11_MASK_EXPOSURE) == 0)
            continue;
        memset(&ev, 0, sizeof(ev));
        {
            uint8_t *b = (uint8_t *)&ev;
            b[0] = Y11_EVT_EXPOSE;
        }
        y11_wire_put32(&ev.window, win->id);
        y11_wire_put16(&ev.x, (uint16_t)x);
        y11_wire_put16(&ev.y, (uint16_t)y);
        y11_wire_put16(&ev.width, (uint16_t)w);
        y11_wire_put16(&ev.height, (uint16_t)h);
        y11_wire_put16(&ev.count, 0);
        y11_event_dispatch32(sub->client, &ev, sizeof(ev));
    }
}

/*
 * Recurse into `win`'s subtree: viewable descendants intersecting the
 * region (in `win`'s drawable coordinates) receive Expose translated
 * into their own coordinate space.
 */
static void y11_damage_descendants(struct y11_window *win, int32_t x,
                                   int32_t y, int32_t w, int32_t h)
{
    struct y11_window *child;

    for (child = win->first_child; child != NULL;
         child = child->next_sibling) {
        int32_t ix, iy, iw, ih;

        ix = x > (int32_t)child->x ? x : child->x;
        iy = y > (int32_t)child->y ? y : child->y;
        iw = (x + w < (int32_t)child->x + child->width
                  ? x + w : (int32_t)child->x + child->width) - ix;
        ih = (y + h < (int32_t)child->y + child->height
                  ? y + h : (int32_t)child->y + child->height) - iy;

        if (iw > 0 && ih > 0 && child->map_state == Y11_MAP_STATE_VIEWABLE)
            y11_damage_expose(child, ix - child->x, iy - child->y, iw, ih);

        y11_damage_descendants(child, x, y, w, h);
    }
}

/*
 * A window's region became visible (mapping) or its content was
 * invalidated: the window's own ExposureMask subscribers and all
 * viewable descendants intersecting the region are notified.
 */
void y11_damage_mapped(struct y11_window *win, int32_t x, int32_t y,
                       uint32_t w, uint32_t h)
{
    if (win->map_state == Y11_MAP_STATE_VIEWABLE)
        y11_scanout_mark_dirty(win->abs_x + x, win->abs_y + y, w, h);
    y11_damage_expose(win, x, y, (int32_t)w, (int32_t)h);
    y11_damage_descendants(win, x, y, (int32_t)w, (int32_t)h);
}

/*
 * Drawing landed on drawable `d` (a window): viewable descendant
 * windows intersecting the region had their content overwritten and
 * must repaint.  The drawn window itself is not notified: its client
 * just drew there intentionally.
 */
void y11_damage_drawn(struct y11_drawable *d, int32_t x, int32_t y,
                      uint32_t w, uint32_t h)
{
    struct y11_window *win;

    if (d->type != Y11_DRAWABLE_WINDOW || d->pixels == NULL)
        return;
    win = y11_window_get(d->id);
    if (win == NULL)
        return;

    /* Viewable window pixels feed the hardware scanout. */
    if (win->map_state == Y11_MAP_STATE_VIEWABLE)
        y11_scanout_mark_dirty(win->abs_x + x, win->abs_y + y, w, h);

    y11_damage_descendants(win, x, y, (int32_t)w, (int32_t)h);
}
