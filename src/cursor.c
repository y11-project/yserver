/*
 * cursor.c - Client-defined cursors for the Y11 display server.
 *
 * Cursors carry an ARGB image (from RENDER's CreateCursor, what
 * libXcursor uploads for themed cursors like Borealis) plus a hot spot.
 * Windows reference a cursor through XDefineCursor or the CWCursor
 * attribute; the cursor actually shown is the one set on the deepest
 * window under the pointer, walking up to the root.  When the
 * resolved cursor has no image (core cursor-font stubs, None), the
 * classic arrow stays up.
 *
 * The image pixels are X pixel-order uint32s (A<<24|R<<16|G<<8|B),
 * which is exactly the KMS ARGB8888 cursor plane layout, so scanout
 * copies them verbatim (src/scanout.c).
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

struct y11_cursor_img {
    struct y11_cursor_img *next;
    yid_t                id;
    struct y11_client    *owner;
    uint16_t             width, height;
    int16_t              hot_x, hot_y;
    uint32_t             *argb;       /* NULL = image-less stub */
};

static struct y11_cursor_img *y11_cursor_list;
static yid_t y11_cursor_active_id;

/*
 * Register a cursor.  pixels may be NULL, in which case the cursor is
 * a stub: resolving it falls back to the built-in arrow (core
 * cursor-font cursors, which y11 does not rasterize).  pixel rows are
 * stride bytes apart.
 */
int y11_cursor_create(yid_t id, struct y11_client *owner,
                      uint16_t width, uint16_t height,
                      int16_t hot_x, int16_t hot_y,
                      const uint32_t *pixels, size_t stride)
{
    struct y11_cursor_img *img;

    if (y11_resource_get(id, Y11_RESOURCE_CURSOR) != NULL)
        return -1;
    if (pixels != NULL &&
        (width == 0 || height == 0 || width > 256u || height > 256u))
        return -1;

    img = calloc(1, sizeof(*img));
    if (img == NULL)
        return -1;
    img->id = id;
    img->owner = owner;
    img->width = width;
    img->height = height;
    img->hot_x = hot_x;
    img->hot_y = hot_y;
    if (pixels != NULL) {
        uint16_t y;

        img->argb = malloc((size_t)width * (size_t)height * 4u);
        if (img->argb == NULL) {
            free(img);
            return -1;
        }
        for (y = 0; y < height; y++) {
            memcpy(&img->argb[(size_t)y * width],
                   (const uint8_t *)pixels + (size_t)y * stride,
                   (size_t)width * 4u);
        }
    }

    if (y11_resource_add(id, Y11_RESOURCE_CURSOR, img) != 0) {
        free(img->argb);
        free(img);
        return -1;
    }
    img->next = y11_cursor_list;
    y11_cursor_list = img;
    return 0;
}

struct y11_cursor_img *y11_cursor_lookup(yid_t id)
{
    return y11_resource_get(id, Y11_RESOURCE_CURSOR);
}

/* FreeCursor (95).  Unknown ids are a no-op like in the reference. */
void y11_cursor_destroy(yid_t id)
{
    struct y11_cursor_img **link = &y11_cursor_list;

    for (; *link != NULL; link = &(*link)->next) {
        if ((*link)->id == id) {
            struct y11_cursor_img *img = *link;

            *link = img->next;
            free(img->argb);
            free(img);
            y11_resource_remove(id);
            if (y11_cursor_active_id == id) {
                y11_cursor_active_id = 0;
                y11_scanout_set_cursor_default();
            }
            return;
        }
    }
}

static int y11_cursor_belongs(void *ptr, void *arg)
{
    return ((struct y11_cursor_img *)ptr)->owner ==
           (struct y11_client *)arg;
}

static void y11_cursor_unlink_and_free(void *ptr)
{
    struct y11_cursor_img **link = &y11_cursor_list;

    for (; *link != NULL; link = &(*link)->next) {
        if (*link == ptr) {
            struct y11_cursor_img *img = *link;

            *link = img->next;
            free(img->argb);
            free(img);
            return;
        }
    }
}

/* Disconnect cleanup: drop every cursor the client created. */
void y11_cursor_purge_client(struct y11_client *c)
{
    y11_resource_purge_type_arg(Y11_RESOURCE_CURSOR, c,
                                y11_cursor_belongs,
                                y11_cursor_unlink_and_free);
}

/*
 * Attach a cursor to a window (XDefineCursor / CWCursor).  Zero means
 * "inherit from the parent".  Returns 0 on success, -1 for an unknown
 * cursor id.
 */
int y11_cursor_set_window(struct y11_window *win, yid_t cursor)
{
    if (cursor != 0 && y11_cursor_lookup(cursor) == NULL)
        return -1;
    win->cursor_id = cursor;
    y11_cursor_refresh();
    return 0;
}

/*
 * Resolve which cursor belongs at the pointer: the deepest window
 * under the pointer that carries one, root included.  Cheap enough to
 * run on every pointer motion (a short parent walk and an id compare).
 */
void y11_cursor_refresh(void)
{
    struct y11_window *win;
    yid_t id = 0;

    win = y11_window_at_point(y11_input_pointer()->root_x,
                              y11_input_pointer()->root_y);
    for (; win != NULL; win = win->parent) {
        if (win->cursor_id != 0) {
            id = win->cursor_id;
            break;
        }
    }

    if (id == y11_cursor_active_id)
        return;                 /* unchanged: the plane keeps its BO */
    y11_cursor_active_id = id;

    if (id != 0) {
        struct y11_cursor_img *img = y11_cursor_lookup(id);

        if (img != NULL && img->argb != NULL) {
            (void)y11_scanout_set_cursor(img->argb, img->width,
                                         img->height, img->hot_x,
                                         img->hot_y);
            return;
        }
    }
    y11_scanout_set_cursor_default();
}
