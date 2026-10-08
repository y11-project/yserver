/*
 * render.c - Drawable resolution and core 2D rasterization for the
 * Y11 display server.
 *
 * Drawables resolve transparently to an on-screen window or an
 * off-screen pixmap: both store linear 32-bit XRGB rows (stride =
 * width * 4).  Solid rectangle fills, blits and image transfers apply
 * the GC's raster operation (GXclear .. GXset) and plane mask, and are
 * clipped to the drawable bounds and the GC clip rectangles.
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* ---- drawable resolution ------------------------------------------------------ */

static struct y11_gc *y11_render_gc_lookup(yid_t id)
{
    return y11_resource_get(id, Y11_RESOURCE_GC);
}

/*
 * Resolve any drawable id (window or pixmap) to its common
 * y11_drawable_t, or NULL when the id is unknown.
 */
y11_drawable_t *y11_drawable_lookup(yid_t id)
{
    struct y11_window *win = y11_window_get(id);

    if (win != NULL)
        return &win->drawable;
    {
        struct y11_pixmap *p = y11_resource_get(id, Y11_RESOURCE_PIXMAP);
        if (p != NULL)
            return &p->base;
    }
    return NULL;
}

/* ---- raster operations ----------------------------------------------------------- */

/* Apply a GX raster operation to one pixel (src over dst). */static uint32_t y11_render_gx(uint8_t function, uint32_t src, uint32_t dst)
{
    switch (function) {
    case 0:  return 0;                          /* GXclear */
    case 1:  return src & dst;                  /* GXand */
    case 2:  return src & ~dst;                 /* GXandReverse */
    case 3:  return src;                        /* GXcopy */
    case 4:  return ~src & dst;                 /* GXandInverted */
    case 5:  return dst;                        /* GXnoop */
    case 6:  return src ^ dst;                  /* GXxor */
    case 7:  return src | dst;                  /* GXor */
    case 8:  return ~(src | dst);               /* GXnor */
    case 9:  return ~src ^ dst;                 /* GXequiv */
    case 10: return ~dst;                       /* GXinvert */
    case 11: return src | ~dst;                 /* GXorReverse */
    case 12: return ~src;                       /* GXcopyInverted */
    case 13: return ~src | dst;                 /* GXorInverted */
    case 14: return ~(src & dst);               /* GXnand */
    case 15: return 0xFFFFFFFFu;                /* GXset */
    default: return src;
    }
}

/* Write one pixel through the GC's function and plane mask. */
static void y11_render_pixel(y11_drawable_t *d, const struct y11_gc *gc,
                             size_t col, size_t row, uint32_t src)
{
    uint32_t *px = &d->pixels[row * (d->stride / 4u) + col];
    uint32_t out = y11_render_gx(gc->function, src, *px);

    *px = (*px & ~gc->plane_mask) | (out & gc->plane_mask);
}

/* Public wrapper for other modules (MIT-SHM blits). */
void y11_render_pixel_ex(y11_drawable_t *d, const struct y11_gc *gc,
                         size_t col, size_t row, uint32_t src)
{
    y11_render_pixel(d, gc, col, row, src);
}

/*
 * Fill one rectangle (drawable coordinates), clipped to the drawable
 * bounds.
 */
static void y11_render_fill(y11_drawable_t *d, const struct y11_gc *gc,
                            int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t col, row;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > (int32_t)d->width)
        w = (int32_t)d->width - x;
    if (y + h > (int32_t)d->height)
        h = (int32_t)d->height - y;
    if (w <= 0 || h <= 0)
        return;

    for (row = y; row < y + h; row++) {
        for (col = x; col < x + w; col++)
            y11_render_pixel(d, gc, (size_t)col, (size_t)row, gc->foreground);
    }
}

/*
 * Fill a rectangle honoring the GC clip rectangles: each clip box
 * contributes the intersection with the fill rectangle.  Clip
 * rectangles are relative to the GC clip origin.
 */
static void y11_render_fill_clipped(y11_drawable_t *d,
                                    const struct y11_gc *gc,
                                    int32_t x, int32_t y,
                                    int32_t w, int32_t h)
{
    size_t i;

    if (gc->num_clip_rects == 0) {
        y11_render_fill(d, gc, x, y, w, h);
        return;
    }
    for (i = 0; i < gc->num_clip_rects; i++) {
        const y11_rect_t *clip = &gc->clip_rects[i];
        int32_t cx = (int32_t)gc->clip_x_origin + clip->x;
        int32_t cy = (int32_t)gc->clip_y_origin + clip->y;
        int32_t cw = clip->width;
        int32_t ch = clip->height;
        int32_t ix, iy, iw, ih;

        /* intersect the fill rectangle with the clip box */
        ix = x > cx ? x : cx;
        iy = y > cy ? y : cy;
        iw = (x + w < cx + cw ? x + w : cx + cw) - ix;
        ih = (y + h < cy + ch ? y + h : cy + ch) - iy;
        if (iw > 0 && ih > 0)
            y11_render_fill(d, gc, ix, iy, iw, ih);
    }
}

/* ---- shared request validation ---------------------------------------------------- */

/*
 * Validate a drawing request's target pair: the GC and drawable must
 * exist, the drawable must have a buffer (not InputOnly), and the
 * depths must agree.  Sends the proper error and returns NULL when
 * invalid; otherwise returns the drawable.
 */
static y11_drawable_t *y11_render_validate(struct y11_client *c,
                                           uint8_t opcode, uint32_t gc_id,
                                           uint32_t drawable_id)
{
    struct y11_gc *gc = y11_render_gc_lookup(gc_id);
    y11_drawable_t *d = y11_drawable_lookup(drawable_id);

    if (gc == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_GCONTEXT, gc_id, opcode);
        return NULL;
    }
    if (d == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE, drawable_id,
                                opcode);
        return NULL;
    }
    if (d->pixels == NULL || gc->depth != d->depth) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, drawable_id, opcode);
        return NULL;
    }
    return d;
}

/* ---- PolyFillRectangle (opcode 70) ---------------------------------------------------

 *   1     70        opcode
 *   1     unused
 *   2     request length
 *   4     drawable
 *   4     gc
 *   n     rectangles (8 bytes each: x, y, width, height)
 */
int y11_render_req_poly_fill_rectangle(struct y11_client *c,
                                       const uint8_t *pkt, size_t len,
                                       size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail, nrects, i;
    struct y11_gc *gc;
    y11_drawable_t *d;

    if (len - data_off < 8u)
        goto badlength;
    avail = len - data_off - 8u;
    if (avail % 8u != 0)
        goto badlength;
    nrects = avail / 8u;

    gc = y11_render_gc_lookup(y11_wire_get32(body + 4));
    d = y11_render_validate(c, pkt[0], y11_wire_get32(body + 4),
                            y11_wire_get32(body + 0));
    if (d == NULL)
        return 0;

    for (i = 0; i < nrects; i++) {
        const uint8_t *r = body + 8 + i * 8u;

        y11_render_fill_clipped(d, gc,
                                (int16_t)y11_wire_get16(r + 0),
                                (int16_t)y11_wire_get16(r + 2),
                                (int16_t)y11_wire_get16(r + 4),
                                (int16_t)y11_wire_get16(r + 6));
    }

    if (d->type == Y11_DRAWABLE_WINDOW)
        y11_damage_drawn(d, 0, 0, d->width, d->height);
    return 0;                   /* no reply */

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/* ---- CopyArea (opcode 62) -------------------------------------------------------------

 *   1     62        opcode
 *   1     unused
 *   2     request length
 *   4     source drawable
 *   4     destination drawable
 *   4     gc
 *   2     src x, y
 *   2     dst x, y
 *   2     width, height
 */
int y11_render_req_copy_area(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    y11_drawable_t *src, *dst;
    struct y11_gc *gc;
    int32_t sx, sy, dx, dy, w, h;
    int32_t row, col;
    uint32_t *snap;
    size_t snap_stride;

    if (len - data_off != 24u)
        goto badlength;

    src = y11_drawable_lookup(y11_wire_get32(body + 0));
    dst = y11_render_validate(c, pkt[0], y11_wire_get32(body + 8),
                              y11_wire_get32(body + 4));
    gc = y11_render_gc_lookup(y11_wire_get32(body + 8));
    if (dst == NULL)
        return 0;
    if (src == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE,
                                y11_wire_get32(body + 0), pkt[0]);
        return 0;
    }
    if (src->pixels == NULL || src->depth != dst->depth) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH,
                                y11_wire_get32(body + 0), pkt[0]);
        return 0;
    }

    sx = (int16_t)y11_wire_get16(body + 12);
    sy = (int16_t)y11_wire_get16(body + 14);
    dx = (int16_t)y11_wire_get16(body + 16);
    dy = (int16_t)y11_wire_get16(body + 18);
    w = (int16_t)y11_wire_get16(body + 20);
    h = (int16_t)y11_wire_get16(body + 22);
    if (w <= 0 || h <= 0)
        return 0;               /* nothing to copy */

    /* Shrink the source rectangle to the source bounds, keeping the
     * blit position consistent. */
    if (sx < 0) {
        w += sx;
        dx -= sx;
        sx = 0;
    }
    if (sy < 0) {
        h += sy;
        dy -= sy;
        sy = 0;
    }
    if (sx + w > (int32_t)src->width)
        w = (int32_t)src->width - sx;
    if (sy + h > (int32_t)src->height)
        h = (int32_t)src->height - sy;
    if (w <= 0 || h <= 0)
        return 0;

    /* Snapshot the source region first: the blit must be safe when the
     * source and destination are the same drawable and overlap. */
    snap = NULL;
    snap_stride = (size_t)w;
    if (src == dst || src->pixels == dst->pixels) {
        snap = malloc((size_t)w * (size_t)h * 4u);
        if (snap == NULL) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
            return 0;
        }
        for (row = 0; row < h; row++) {
            memcpy((uint8_t *)snap + (size_t)row * snap_stride * 4u,
                   (uint8_t *)src->pixels +
                       ((size_t)(sy + row) * (src->stride / 4u) +
                        (size_t)sx) * 4u,
                   (size_t)w * 4u);
        }
    }

    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            int32_t dcx = dx + col;
            int32_t dcy = dy + row;
            uint32_t pix;

            if (dcx < 0 || dcy < 0 || dcx >= (int32_t)dst->width ||
                dcy >= (int32_t)dst->height)
                continue;       /* clipped by the destination bounds */
            if (snap != NULL)
                pix = snap[(size_t)row * snap_stride + (size_t)col];
            else
                pix = src->pixels[(size_t)(sy + row) * (src->stride / 4u) +
                                  (size_t)(sx + col)];
            y11_render_pixel(dst, gc, (size_t)dcx, (size_t)dcy, pix);
        }
    }
    free(snap);

    if (dst->type == Y11_DRAWABLE_WINDOW)
        y11_damage_drawn(dst, dx, dy, (uint32_t)w, (uint32_t)h);
    return 0;                   /* no reply */

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/* ---- PutImage (opcode 72) ---------------------------------------------------------------

 *   1     72        opcode
 *   1     format (0 Bitmap, 1 XYPixmap, 2 ZPixmap)
 *   2     request length
 *   4     drawable
 *   4     gc
 *   2     width, height
 *   2     dst x, y
 *   1     left pad
 *   1     depth
 *   2     unused
 *   n     image data
 *
 * ZPixmap depth 24/32: one 4-byte pixel per column, no padding.
 * ZPixmap depth 1 and the XY formats: bit-packed scanlines padded to
 * 32-bit units, least significant plane first.
 */
int y11_render_req_put_image(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail, data_len;
    uint8_t format, left_pad, depth;
    uint16_t width, height;
    int16_t dst_x, dst_y;
    struct y11_gc *gc;
    y11_drawable_t *d;

    if (len - data_off < 20u)
        goto badlength;
    avail = len - data_off - 20u;
    data_len = avail - avail % 4u;

    format = pkt[1];
    if (format > 2) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, format, pkt[0]);
        return 0;
    }
    d = y11_render_validate(c, pkt[0], y11_wire_get32(body + 4),
                            y11_wire_get32(body + 0));
    gc = y11_render_gc_lookup(y11_wire_get32(body + 4));
    if (d == NULL)
        return 0;

    width = y11_wire_get16(body + 8);
    height = y11_wire_get16(body + 10);
    dst_x = (int16_t)y11_wire_get16(body + 12);
    dst_y = (int16_t)y11_wire_get16(body + 14);
    left_pad = body[16];
    depth = body[17];

    if (width == 0 || height == 0)
        return 0;

    if (format == 0) {
        if (depth != 1) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, depth, pkt[0]);
            return 0;
        }
    } else {
        if (depth != d->depth) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, depth, pkt[0]);
            return 0;
        }
    }

    if (format == 2 && depth >= 24) {
        /* ZPixmap, 32bpp rows: unpack one 4-byte pixel per column. */
        size_t row, col;
        if (data_len < (size_t)width * (size_t)height * 4u)
            goto badlength;
        for (row = 0; row < height; row++) {
            int32_t dy_ = (int32_t)dst_y + (int32_t)row;
            const uint8_t *src = body + 20 + row * (size_t)width * 4u;

            if (dy_ < 0 || dy_ >= (int32_t)d->height)
                continue;
            for (col = 0; col < width; col++) {
                int32_t dx_ = (int32_t)dst_x + (int32_t)col;

                if (dx_ < 0 || dx_ >= (int32_t)d->width)
                    continue;
                y11_render_pixel(d, gc, (size_t)dx_, (size_t)dy_,
                                 y11_wire_get32(src + col * 4u) & 0xFFFFFFu);
            }
        }
    } else {
        /* Bit-packed data (ZPixmap depth 1, XYPixmap, Bitmap):
         * scanlines padded to 32-bit units, LSB plane first. */
        size_t plane_stride = ((size_t)width + 31u) / 32u * 4u;
        size_t planes = (format == 1) ? (size_t)depth : 1u;
        size_t row, col, p;
        uint32_t *acc;

        if (data_len < planes * (size_t)height * plane_stride)
            goto badlength;

        acc = calloc((size_t)width, 4u);
        if (acc == NULL)
            goto badalloc;

        for (row = 0; row < height; row++) {
            int32_t dy_ = (int32_t)dst_y + (int32_t)row;

            if (dy_ < 0 || dy_ >= (int32_t)d->height)
                continue;
            memset(acc, 0, (size_t)width * 4u);
            for (p = 0; p < planes; p++) {
                const uint8_t *plane = body + 20 +
                    (p * (size_t)height + row) * plane_stride;
                for (col = 0; col < width; col++) {
                    size_t bit = (size_t)left_pad + col;
                    size_t byte = bit / 8u;
                    unsigned shift = (unsigned)(bit % 8u);

                    if ((plane[byte] >> shift) & 1u)
                        acc[col] |= 1u << p;
                }
            }
            for (col = 0; col < width; col++) {
                int32_t dx_ = (int32_t)dst_x + (int32_t)col;
                uint32_t pix;

                if (dx_ < 0 || dx_ >= (int32_t)d->width)
                    continue;
                if (format == 0 && d->depth > 1) {
                    pix = (acc[col] & 1u) ? gc->foreground : gc->background;
                } else {
                    pix = acc[col];
                }
                y11_render_pixel(d, gc, (size_t)dx_, (size_t)dy_, pix);
            }
        }
        free(acc);
    }

    if (d->type == Y11_DRAWABLE_WINDOW)
        y11_damage_drawn(d, dst_x, dst_y, width, height);
    return 0;                   /* no reply */

badalloc:
    y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/* ---- GetImage (opcode 73) -----------------------------------------------------------------

 *   1     73        opcode
 *   1     format
 *   2     request length
 *   4     drawable
 *   2     x, y
 *   2     width, height
 *   4     plane mask
 *   =>
 *   reply (32 bytes: depth, visual) + linear pixel payload
 *
 * Only ZPixmap (format 2) is answered; out-of-bounds pixels read as
 * zero so Xlib's expected payload length always matches.
 */
int y11_render_req_get_image(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    y11_get_image_reply rep;
    struct y11_window *win;
    y11_drawable_t *d;
    uint8_t format;
    uint16_t width, height;
    int16_t x, y;
    uint32_t plane_mask;
    size_t data_bytes, row, col;
    uint8_t *out;

    if (len - data_off != 16u)  /* drawable, x, y, w, h, plane mask */
        goto badlength;

    format = pkt[1];
    if (format != 2) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, format, pkt[0]);
        return 0;
    }
    d = y11_drawable_lookup(y11_wire_get32(body + 0));
    if (d == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE,
                                y11_wire_get32(body + 0), pkt[0]);
        return 0;
    }
    if (d->pixels == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH,
                                y11_wire_get32(body + 0), pkt[0]);
        return 0;
    }

    width = y11_wire_get16(body + 8);
    height = y11_wire_get16(body + 10);
    x = (int16_t)y11_wire_get16(body + 4);
    y = (int16_t)y11_wire_get16(body + 6);
    plane_mask = y11_wire_get32(body + 12);
    if (width == 0 || height == 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, 0, pkt[0]);
        return 0;
    }

    if (d->depth >= 24) {
        data_bytes = (size_t)width * (size_t)height * 4u;
    } else {
        data_bytes = (size_t)height * ((size_t)width + 31u) / 32u * 4u;
    }

    out = calloc(1, data_bytes);
    if (out == NULL)
        goto badalloc;

    if (d->depth >= 24) {
        for (row = 0; row < height; row++) {
            int32_t sy_ = (int32_t)y + (int32_t)row;

            if (sy_ < 0 || sy_ >= (int32_t)d->height)
                continue;       /* out of bounds: stays zero */
            for (col = 0; col < width; col++) {
                int32_t sx_ = (int32_t)x + (int32_t)col;
                uint32_t pix;

                if (sx_ < 0 || sx_ >= (int32_t)d->width)
                    continue;
                pix = d->pixels[(size_t)sy_ * (d->stride / 4u) +
                                (size_t)sx_];
                y11_wire_put32(out + (row * (size_t)width + col) * 4u,
                               pix & plane_mask);
            }
        }
    } else {
        /* Pack bit 0 of each pixel, LSB-first, 32-bit scanline units. */
        size_t stride = ((size_t)width + 31u) / 32u * 4u;
        for (row = 0; row < height; row++) {
            int32_t sy_ = (int32_t)y + (int32_t)row;

            if (sy_ < 0 || sy_ >= (int32_t)d->height)
                continue;
            for (col = 0; col < width; col++) {
                int32_t sx_ = (int32_t)x + (int32_t)col;
                uint32_t pix;

                if (sx_ < 0 || sx_ >= (int32_t)d->width)
                    continue;
                pix = d->pixels[(size_t)sy_ * (d->stride / 4u) +
                                (size_t)sx_];
                if ((pix & plane_mask & 1u) != 0)
                    out[row * stride + col / 8u] |=
                        (uint8_t)(1u << (col % 8u));
            }
        }
    }

    /* The drawable's visual (windows) or zero (pixmaps). */
    win = y11_window_get(d->id);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = d->depth;
    y11_wire_put32(&rep.hdr.length, data_bytes / 4u);
    y11_wire_put32(&rep.visual, win != NULL ? win->visual_id : 0u);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    y11_client_send(c, out, data_bytes);
    free(out);
    return 0;

badalloc:
    y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/* Plot one pixel honoring GC clip rectangles and drawable boundaries. */
static void y11_render_pixel_clipped(y11_drawable_t *d, const struct y11_gc *gc,
                                    int32_t x, int32_t y, uint32_t color)
{
    size_t i;

    if (x < 0 || y < 0 || x >= (int32_t)d->width || y >= (int32_t)d->height)
        return;

    if (gc->num_clip_rects > 0) {
        int in_clip = 0;
        for (i = 0; i < gc->num_clip_rects; i++) {
            const y11_rect_t *clip = &gc->clip_rects[i];
            int32_t cx = (int32_t)gc->clip_x_origin + clip->x;
            int32_t cy = (int32_t)gc->clip_y_origin + clip->y;
            int32_t cw = clip->width;
            int32_t ch = clip->height;

            if (x >= cx && y >= cy && x < cx + cw && y < cy + ch) {
                in_clip = 1;
                break;
            }
        }
        if (!in_clip)
            return;
    }

    y11_render_pixel(d, gc, (size_t)x, (size_t)y, color);
}

/* Bresenham line drawing between (x0, y0) and (x1, y1). */
static void y11_render_line_clipped(y11_drawable_t *d, const struct y11_gc *gc,
                                   int32_t x0, int32_t y0,
                                   int32_t x1, int32_t y1,
                                   uint32_t color)
{
    int32_t dx = abs(x1 - x0);
    int32_t dy = abs(y1 - y0);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx - dy;

    for (;;) {
        y11_render_pixel_clipped(d, gc, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        {
            int32_t e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

/* ---- PolyPoint (opcode 64) ------------------------------------------------ */
int y11_render_req_poly_point(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail, npoints, i;
    struct y11_gc *gc;
    y11_drawable_t *d;
    uint8_t coord_mode = pkt[1];
    int32_t cur_x = 0, cur_y = 0;

    if (len - data_off < 8u)
        goto badlength;
    avail = len - data_off - 8u;
    if (avail % 4u != 0)
        goto badlength;
    npoints = avail / 4u;

    gc = y11_render_gc_lookup(y11_wire_get32(body + 4));
    d = y11_render_validate(c, pkt[0], y11_wire_get32(body + 4),
                            y11_wire_get32(body + 0));
    if (d == NULL)
        return 0;

    for (i = 0; i < npoints; i++) {
        const uint8_t *p = body + 8 + i * 4u;
        int16_t px = (int16_t)y11_wire_get16(p + 0);
        int16_t py = (int16_t)y11_wire_get16(p + 2);

        if (coord_mode == 0) {
            cur_x = px;
            cur_y = py;
        } else {
            cur_x += px;
            cur_y += py;
        }
        y11_render_pixel_clipped(d, gc, cur_x, cur_y, gc->foreground);
    }

    if (d->type == Y11_DRAWABLE_WINDOW)
        y11_damage_drawn(d, 0, 0, d->width, d->height);
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/* ---- PolyLine (opcode 65) ------------------------------------------------- */
int y11_render_req_poly_line(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail, npoints, i;
    struct y11_gc *gc;
    y11_drawable_t *d;
    uint8_t coord_mode = pkt[1];
    int32_t prev_x = 0, prev_y = 0;

    if (len - data_off < 8u)
        goto badlength;
    avail = len - data_off - 8u;
    if (avail % 4u != 0)
        goto badlength;
    npoints = avail / 4u;

    gc = y11_render_gc_lookup(y11_wire_get32(body + 4));
    d = y11_render_validate(c, pkt[0], y11_wire_get32(body + 4),
                            y11_wire_get32(body + 0));
    if (d == NULL)
        return 0;

    for (i = 0; i < npoints; i++) {
        const uint8_t *p = body + 8 + i * 4u;
        int16_t px = (int16_t)y11_wire_get16(p + 0);
        int16_t py = (int16_t)y11_wire_get16(p + 2);
        int32_t cur_x, cur_y;

        if (coord_mode == 0 || i == 0) {
            cur_x = (coord_mode == 0) ? px : (prev_x + px);
            cur_y = (coord_mode == 0) ? py : (prev_y + py);
        } else {
            cur_x = prev_x + px;
            cur_y = prev_y + py;
        }

        if (i > 0)
            y11_render_line_clipped(d, gc, prev_x, prev_y, cur_x, cur_y, gc->foreground);
        else if (npoints == 1)
            y11_render_pixel_clipped(d, gc, cur_x, cur_y, gc->foreground);

        prev_x = cur_x;
        prev_y = cur_y;
    }

    if (d->type == Y11_DRAWABLE_WINDOW)
        y11_damage_drawn(d, 0, 0, d->width, d->height);
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/* ---- PolySegment (opcode 66) ---------------------------------------------- */
int y11_render_req_poly_segment(struct y11_client *c, const uint8_t *pkt,
                                size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail, nsegs, i;
    struct y11_gc *gc;
    y11_drawable_t *d;

    if (len - data_off < 8u)
        goto badlength;
    avail = len - data_off - 8u;
    if (avail % 8u != 0)
        goto badlength;
    nsegs = avail / 8u;

    gc = y11_render_gc_lookup(y11_wire_get32(body + 4));
    d = y11_render_validate(c, pkt[0], y11_wire_get32(body + 4),
                            y11_wire_get32(body + 0));
    if (d == NULL)
        return 0;

    for (i = 0; i < nsegs; i++) {
        const uint8_t *s = body + 8 + i * 8u;
        int32_t x1 = (int16_t)y11_wire_get16(s + 0);
        int32_t y1 = (int16_t)y11_wire_get16(s + 2);
        int32_t x2 = (int16_t)y11_wire_get16(s + 4);
        int32_t y2 = (int16_t)y11_wire_get16(s + 6);

        y11_render_line_clipped(d, gc, x1, y1, x2, y2, gc->foreground);
    }

    if (d->type == Y11_DRAWABLE_WINDOW)
        y11_damage_drawn(d, 0, 0, d->width, d->height);
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/* ---- PolyRectangle (opcode 67) -------------------------------------------- */
int y11_render_req_poly_rectangle(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    size_t avail, nrects, i;
    struct y11_gc *gc;
    y11_drawable_t *d;

    if (len - data_off < 8u)
        goto badlength;
    avail = len - data_off - 8u;
    if (avail % 8u != 0)
        goto badlength;
    nrects = avail / 8u;

    gc = y11_render_gc_lookup(y11_wire_get32(body + 4));
    d = y11_render_validate(c, pkt[0], y11_wire_get32(body + 4),
                            y11_wire_get32(body + 0));
    if (d == NULL)
        return 0;

    for (i = 0; i < nrects; i++) {
        const uint8_t *r = body + 8 + i * 8u;
        int32_t x = (int16_t)y11_wire_get16(r + 0);
        int32_t y = (int16_t)y11_wire_get16(r + 2);
        int32_t w = (uint16_t)y11_wire_get16(r + 4);
        int32_t h = (uint16_t)y11_wire_get16(r + 6);

        /* 4 outline segments: top, right, bottom, left */
        y11_render_line_clipped(d, gc, x, y, x + w, y, gc->foreground);
        y11_render_line_clipped(d, gc, x + w, y, x + w, y + h, gc->foreground);
        y11_render_line_clipped(d, gc, x + w, y + h, x, y + h, gc->foreground);
        y11_render_line_clipped(d, gc, x, y + h, x, y, gc->foreground);
    }

    if (d->type == Y11_DRAWABLE_WINDOW)
        y11_damage_drawn(d, 0, 0, d->width, d->height);
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/* ---- CopyPlane (opcode 63) ------------------------------------------------ */
int y11_render_req_copy_plane(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    y11_drawable_t *src, *dst;
    struct y11_gc *gc;
    int32_t sx, sy, dx, dy, w, h;
    int32_t row, col;
    uint32_t bit_plane;
    uint32_t *snap;
    size_t snap_stride;

    if (len - data_off != 28u)
        goto badlength;

    src = y11_drawable_lookup(y11_wire_get32(body + 0));
    dst = y11_render_validate(c, pkt[0], y11_wire_get32(body + 8),
                              y11_wire_get32(body + 4));
    gc = y11_render_gc_lookup(y11_wire_get32(body + 8));
    if (dst == NULL)
        return 0;
    if (src == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE,
                                y11_wire_get32(body + 0), pkt[0]);
        return 0;
    }
    if (src->pixels == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH,
                                y11_wire_get32(body + 0), pkt[0]);
        return 0;
    }

    bit_plane = y11_wire_get32(body + 24);
    /* bit_plane must have exactly one bit set */
    if (bit_plane == 0 || (bit_plane & (bit_plane - 1u)) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, bit_plane, pkt[0]);
        return 0;
    }

    sx = (int16_t)y11_wire_get16(body + 12);
    sy = (int16_t)y11_wire_get16(body + 14);
    dx = (int16_t)y11_wire_get16(body + 16);
    dy = (int16_t)y11_wire_get16(body + 18);
    w = (int16_t)y11_wire_get16(body + 20);
    h = (int16_t)y11_wire_get16(body + 22);
    if (w <= 0 || h <= 0)
        return 0;

    if (sx < 0) {
        w += sx;
        dx -= sx;
        sx = 0;
    }
    if (sy < 0) {
        h += sy;
        dy -= sy;
        sy = 0;
    }
    if (sx + w > (int32_t)src->width)
        w = (int32_t)src->width - sx;
    if (sy + h > (int32_t)src->height)
        h = (int32_t)src->height - sy;
    if (w <= 0 || h <= 0)
        return 0;

    snap = NULL;
    snap_stride = (size_t)w;
    if (src == dst || src->pixels == dst->pixels) {
        snap = malloc((size_t)w * (size_t)h * 4u);
        if (snap == NULL) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
            return 0;
        }
        for (row = 0; row < h; row++) {
            memcpy((uint8_t *)snap + (size_t)row * snap_stride * 4u,
                   (uint8_t *)src->pixels +
                       ((size_t)(sy + row) * (src->stride / 4u) +
                        (size_t)sx) * 4u,
                   (size_t)w * 4u);
        }
    }

    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            int32_t dcx = dx + col;
            int32_t dcy = dy + row;
            uint32_t pix;
            uint32_t out_color;

            if (dcx < 0 || dcy < 0 || dcx >= (int32_t)dst->width ||
                dcy >= (int32_t)dst->height)
                continue;
            if (snap != NULL)
                pix = snap[(size_t)row * snap_stride + (size_t)col];
            else
                pix = src->pixels[(size_t)(sy + row) * (src->stride / 4u) +
                                  (size_t)(sx + col)];

            out_color = (pix & bit_plane) ? gc->foreground : gc->background;
            y11_render_pixel_clipped(dst, gc, dcx, dcy, out_color);
        }
    }
    free(snap);

    if (dst->type == Y11_DRAWABLE_WINDOW)
        y11_damage_drawn(dst, dx, dy, (uint32_t)w, (uint32_t)h);
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

