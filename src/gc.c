/*
 * gc.c - Graphics contexts for the Y11 display server.
 *
 * A GC holds the mutable rasterization state used by the drawing
 * requests.  Attributes arrive through the 23-bit X11 GC value mask;
 * every set bit consumes one 4-byte slot in the wire stream, in
 * increasing bit order (GCFunction is bit 0, GCArcMode is bit 22).
 *
 * Attributes y11 does not act on yet (tiles, stipples, dashes, fonts)
 * are accepted and ignored so headless clients keep working.
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* Accepted GC value-mask bits (GCFunction .. GCArcMode). */
#define Y11_GC_MASK_ALL 0x007FFFFFu

/* ---- value mask bits --------------------------------------------------------- */

enum {
    Y11_GCV_FUNCTION          = 1u << 0,
    Y11_GCV_PLANE_MASK        = 1u << 1,
    Y11_GCV_FOREGROUND        = 1u << 2,
    Y11_GCV_BACKGROUND        = 1u << 3,
    Y11_GCV_LINE_WIDTH        = 1u << 4,
    Y11_GCV_LINE_STYLE        = 1u << 5,
    Y11_GCV_CAP_STYLE         = 1u << 6,
    Y11_GCV_JOIN_STYLE        = 1u << 7,
    Y11_GCV_FILL_STYLE        = 1u << 8,
    Y11_GCV_FILL_RULE         = 1u << 9,
    Y11_GCV_TILE              = 1u << 10,
    Y11_GCV_STIPPLE           = 1u << 11,
    Y11_GCV_TS_X_ORIGIN       = 1u << 12,
    Y11_GCV_TS_Y_ORIGIN       = 1u << 13,
    Y11_GCV_FONT              = 1u << 14,
    Y11_GCV_SUBWINDOW_MODE    = 1u << 15,
    Y11_GCV_GRAPHICS_EXPOSURE = 1u << 16,
    Y11_GCV_CLIP_X_ORIGIN     = 1u << 17,
    Y11_GCV_CLIP_Y_ORIGIN     = 1u << 18,
    Y11_GCV_CLIP_MASK         = 1u << 19,
    Y11_GCV_DASH_OFFSET       = 1u << 20,
    Y11_GCV_DASHES            = 1u << 21,
    Y11_GCV_ARC_MODE          = 1u << 22
};

static unsigned y11_popcount_gc(uint32_t v)
{
    return y11_popcount32(v);
}

/* ---- lifecycle ------------------------------------------------------------------ */

/* Free a graphics context object (table entry stays). */
void y11_gc_destroy(void *ptr)
{
    struct y11_gc *gc = ptr;

    if (gc == NULL)
        return;
    free(gc->clip_rects);
    free(gc);
}

static int y11_gc_belongs(void *ptr, struct y11_client *client)
{
    return ((const struct y11_gc *)ptr)->owner == client;
}

/* Release every graphics context owned by `client` at disconnect. */
void y11_gc_purge_client(struct y11_client *c)
{
    y11_resource_purge_type(Y11_RESOURCE_GC, c, y11_gc_belongs,
                            y11_gc_destroy);
}

/* ---- attribute updates -------------------------------------------------------------- */

/*
 * Parse the wire value mask: 4 bytes per set bit, in increasing bit
 * order.  Only the attributes y11 stores are consumed; the rest are
 * accepted and ignored.  Returns an error code (0 on success).
 */
static uint8_t y11_gc_parse_values(struct y11_gc *gc, uint32_t mask,
                                   const uint8_t *vals)
{
    unsigned i;

    for (i = 0; i < 23; i++) {
        uint32_t v;

        if ((mask & (1u << i)) == 0)
            continue;
        v = y11_wire_get32(vals);
        vals += 4;

        switch (i) {
        case 0:             /* GCFunction: GXclear .. GXset (0..15) */
            if (v > 15u)
                return Y11_ERR_BAD_VALUE;
            gc->function = (uint8_t)v;
            break;
        case 1:             /* plane mask */
            gc->plane_mask = v;
            break;
        case 2:             /* foreground */
            gc->foreground = v;
            break;
        case 3:             /* background */
            gc->background = v;
            break;
        case 4:             /* line width */
            gc->line_width = (uint16_t)v;
            break;
        case 15:            /* subwindow mode */
            if (v > 1u)
                return Y11_ERR_BAD_VALUE;
            gc->subwindow_mode = (uint8_t)v;
            break;
        case 17:            /* clip x origin */
            gc->clip_x_origin = (int16_t)v;
            break;
        case 18:            /* clip y origin */
            gc->clip_y_origin = (int16_t)v;
            break;
        case 5:             /* line style 0..2 */
            if (v > 2u)
                return Y11_ERR_BAD_VALUE;
            break;
        case 6:             /* cap style 0..3 */
            if (v > 3u)
                return Y11_ERR_BAD_VALUE;
            break;
        case 7:             /* join style 0..2 */
            if (v > 2u)
                return Y11_ERR_BAD_VALUE;
            break;
        case 8:             /* fill style 0..3 (only Solid renders) */
            if (v > 3u)
                return Y11_ERR_BAD_VALUE;
            break;
        case 9:             /* fill rule 0..1 */
            if (v > 1u)
                return Y11_ERR_BAD_VALUE;
            break;
        case 16:            /* graphics exposures */
        case 22:            /* arc mode */
            break;          /* accepted, not stored */
        default:            /* tile, stipple, origins, font, clip-mask,
                             * dash offset, dashes: accepted, ignored */
            break;
        }
    }
    return 0;
}

/* Set the X11 defaults on a fresh GC. */
static void y11_gc_set_defaults(struct y11_gc *gc)
{
    gc->function = 3;            /* GXcopy */
    gc->plane_mask = 0xFFFFFFFFu;
    gc->foreground = 0;
    gc->background = 1;
    gc->line_width = 0;
    gc->clip_x_origin = 0;
    gc->clip_y_origin = 0;
    gc->num_clip_rects = 0;
    gc->clip_rects = NULL;
    gc->subwindow_mode = 0;    /* ClipByChildren */
}

/* ---- request handlers --------------------------------------------------------------- */

/*
 * CreateGC (opcode 55):
 *   1     55        opcode
 *   1     unused
 *   2     request length
 *   4     gc id
 *   4     drawable
 *   4     value mask
 *   n     value list (4 bytes per set bit)
 */
int y11_gc_req_create(struct y11_client *c, const uint8_t *pkt,
                      size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail = len - data_off;
    uint32_t value_mask, cid, drawable_id;
    unsigned nvalues;
    struct y11_window *win;
    struct y11_pixmap *pix;
    struct y11_gc *gc;
    uint8_t depth, err;

    if (avail < 12u)
        goto badlength;
    value_mask = y11_wire_get32(body + 8);
    if ((value_mask & ~Y11_GC_MASK_ALL) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, value_mask, pkt[0]);
        return 0;
    }
    nvalues = y11_popcount_gc(value_mask);
    if (avail - 12u < (size_t)nvalues * 4u)
        goto badlength;

    cid = y11_wire_get32(body + 0);
    drawable_id = y11_wire_get32(body + 4);

    /* The drawable fixes the GC's depth; it must resolve. */
    win = y11_window_get(drawable_id);
    pix = y11_resource_get(drawable_id, Y11_RESOURCE_PIXMAP);
    if (win == NULL && pix == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE, drawable_id,
                                pkt[0]);
        return 0;
    }
    if (win != NULL)
        depth = win->depth;
    else
        depth = pix->base.depth;

    if (cid < c->resource_id_base ||
        cid - c->resource_id_base > Y11_RID_MASK ||
        y11_resource_get(cid, Y11_RESOURCE_GC) != NULL ||
        y11_window_get(cid) != NULL ||
        y11_resource_get(cid, Y11_RESOURCE_PIXMAP) != NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, cid, pkt[0]);
        return 0;
    }

    gc = calloc(1, sizeof(*gc));
    if (gc == NULL)
        goto badalloc;
    y11_gc_set_defaults(gc);
    gc->id = cid;
    gc->owner = c;
    gc->depth = depth;

    err = y11_gc_parse_values(gc, value_mask, body + 12);
    if (err != 0) {
        y11_gc_destroy(gc);
        y11_dispatch_send_error(c, err, 0, pkt[0]);
        return 0;
    }
    if (y11_resource_add(cid, Y11_RESOURCE_GC, gc) != 0) {
        y11_gc_destroy(gc);
        goto badalloc;
    }
    return 0;                   /* no reply */

badalloc:
    y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/*
 * ChangeGC (opcode 56): gc id, value mask, value list.  Only the
 * creating client may modify a GC.
 */
int y11_gc_req_change(struct y11_client *c, const uint8_t *pkt,
                      size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail = len - data_off;
    uint32_t value_mask, cid;
    unsigned nvalues;
    struct y11_gc *gc;
    uint8_t err;

    if (avail < 8u)
        goto badlength;
    value_mask = y11_wire_get32(body + 4);
    if ((value_mask & ~Y11_GC_MASK_ALL) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, value_mask, pkt[0]);
        return 0;
    }
    nvalues = y11_popcount_gc(value_mask);
    if (avail - 8u < (size_t)nvalues * 4u)
        goto badlength;

    cid = y11_wire_get32(body + 0);
    gc = y11_resource_get(cid, Y11_RESOURCE_GC);
    if (gc == NULL || gc->owner != c) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_GCONTEXT, cid, pkt[0]);
        return 0;
    }

    err = y11_gc_parse_values(gc, value_mask, body + 8);
    if (err != 0) {
        y11_dispatch_send_error(c, err, 0, pkt[0]);
        return 0;
    }
    return 0;                   /* no reply */

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/*
 * CopyGC (opcode 57): source gc, destination gc, value mask.  The mask
 * selects which attributes to copy; no value list follows on the wire.
 * The clip rectangle list is never copied.
 */
int y11_gc_req_copy(struct y11_client *c, const uint8_t *pkt,
                    size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t mask, src_id, dst_id;
    struct y11_gc *src, *dst;

    if (len - data_off != 12u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    src_id = y11_wire_get32(body + 0);
    dst_id = y11_wire_get32(body + 4);
    mask = y11_wire_get32(body + 8);

    if ((mask & ~Y11_GC_MASK_ALL) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, mask, pkt[0]);
        return 0;
    }
    src = y11_resource_get(src_id, Y11_RESOURCE_GC);
    dst = y11_resource_get(dst_id, Y11_RESOURCE_GC);
    if (src == NULL || src->owner != c) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_GCONTEXT, src_id, pkt[0]);
        return 0;
    }
    if (dst == NULL || dst->owner != c) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_GCONTEXT, dst_id, pkt[0]);
        return 0;
    }

    if ((mask & Y11_GCV_FUNCTION) != 0)
        dst->function = src->function;
    if ((mask & Y11_GCV_PLANE_MASK) != 0)
        dst->plane_mask = src->plane_mask;
    if ((mask & Y11_GCV_FOREGROUND) != 0)
        dst->foreground = src->foreground;
    if ((mask & Y11_GCV_BACKGROUND) != 0)
        dst->background = src->background;
    if ((mask & Y11_GCV_LINE_WIDTH) != 0)
        dst->line_width = src->line_width;
    if ((mask & Y11_GCV_SUBWINDOW_MODE) != 0)
        dst->subwindow_mode = src->subwindow_mode;
    if ((mask & Y11_GCV_CLIP_X_ORIGIN) != 0)
        dst->clip_x_origin = src->clip_x_origin;
    if ((mask & Y11_GCV_CLIP_Y_ORIGIN) != 0)
        dst->clip_y_origin = src->clip_y_origin;
    return 0;                   /* no reply */
}

/*
 * SetClipRectangles (opcode 59):
 *   1     59        opcode
 *   1     ordering (Unsorted 0, YXSorted 1, YXBanded 2)
 *   2     request length
 *   4     gc id
 *   2     clip x origin
 *   2     clip y origin
 *   n     rectangles (8 bytes each: x, y, width, height)
 */
int y11_gc_req_set_clip_rectangles(struct y11_client *c, const uint8_t *pkt,
                                   size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t cid;
    size_t avail, nrects, i;
    struct y11_gc *gc;

    if (pkt[1] > 2) {           /* ordering */
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, pkt[1], pkt[0]);
        return 0;
    }
    if (len - data_off < 8u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    avail = len - data_off - 8u;
    if (avail % 8u != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    nrects = avail / 8u;

    cid = y11_wire_get32(body + 0);
    gc = y11_resource_get(cid, Y11_RESOURCE_GC);
    if (gc == NULL || gc->owner != c) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_GCONTEXT, cid, pkt[0]);
        return 0;
    }

    /* The clip list replaces any previous one. */
    free(gc->clip_rects);
    gc->clip_rects = NULL;
    gc->num_clip_rects = 0;
    gc->clip_x_origin = (int16_t)y11_wire_get16(body + 4);
    gc->clip_y_origin = (int16_t)y11_wire_get16(body + 6);

    if (nrects == 0)
        return 0;               /* empty clip list */
    if (nrects > (size_t)-1 / sizeof(y11_rect_t)) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    gc->clip_rects = malloc(nrects * sizeof(y11_rect_t));
    if (gc->clip_rects == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    for (i = 0; i < nrects; i++) {
        const uint8_t *r = body + 8 + i * 8u;

        gc->clip_rects[i].x = (int16_t)y11_wire_get16(r + 0);
        gc->clip_rects[i].y = (int16_t)y11_wire_get16(r + 2);
        gc->clip_rects[i].width = y11_wire_get16(r + 4);
        gc->clip_rects[i].height = y11_wire_get16(r + 6);
    }
    gc->num_clip_rects = nrects;
    return 0;                   /* no reply */
}

/*
 * FreeGC (opcode 60): xResourceReq.  Only the creating client may free.
 */
int y11_gc_req_free(struct y11_client *c, const uint8_t *pkt,
                    size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t cid;
    struct y11_gc *gc;

    if (len - data_off != 4u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    cid = y11_wire_get32(body + 0);
    gc = y11_resource_get(cid, Y11_RESOURCE_GC);
    if (gc == NULL || gc->owner != c) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_GCONTEXT, cid, pkt[0]);
        return 0;
    }
    y11_resource_remove(cid);
    y11_gc_destroy(gc);
    return 0;                   /* no reply */
}
