/*
 * pixmap.c - Off-screen pixel storage for the Y11 display server.
 *
 * Pixmaps are linear 32-bit XRGB buffers owned by one client and
 * registered in the global XID resource table.  Depth 1 pixmaps are
 * stored in the same 32-bit rows (pixels 0 and 1); the depth only
 * matters at the wire boundary, where render.c converts formats.
 *
 * Copyright (c) 2026 The Y11 Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* Free a pixmap object and its pixel buffer (table entry stays).
 * Shared-memory pixmaps point into the client's segment and DRI3
 * pixmaps hold a GEM reference: neither is freed here, but the GEM
 * handle is released. */
void y11_pixmap_destroy(void *ptr)
{
    struct y11_pixmap *p = ptr;

    if (p == NULL)
        return;
    if (p->is_dri3) {
        y11_dri3_release_buffer(p->dri3);
        free(p->dri3);
    } else if (!p->is_shm) {
        free(p->base.pixels);
    }
    free(p);
}

static int y11_pixmap_belongs(void *ptr, void *arg)
{
    return ((const struct y11_pixmap *)ptr)->owner ==
           (struct y11_client *)arg;
}

/*
 * CreatePixmap (opcode 53):
 *   1     53        opcode
 *   1     depth
 *   2     request length
 *   4     pixmap id
 *   4     drawable (screen reference; must exist)
 *   2     width
 *   2     height
 */
int y11_pixmap_req_create(struct y11_client *c, const uint8_t *pkt,
                          size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    yid_t pid, drawable_id;
    uint16_t width, height;
    uint8_t depth;
    struct y11_pixmap *p;
    size_t stride, alloc_size;

    if (len - data_off != 12u)
        goto badlength;

    pid = y11_wire_get32(body + 0);
    drawable_id = y11_wire_get32(body + 4);
    width = y11_wire_get16(body + 8);
    height = y11_wire_get16(body + 10);
    depth = pkt[1];             /* header data byte, both request forms */

    /*
     * The depths a full server advertises (1/4/8/16/24/32); storage
     * stays 32bpp internally, the image requests convert at the wire
     * boundary.
     */
    if (width == 0 || height == 0 ||
        (depth != 1 && depth != 4 && depth != 8 && depth != 16 &&
         depth != 24 && depth != 32)) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, depth, pkt[0]);
        return 0;
    }

    /* The drawable argument selects the screen; it must resolve. */
    if (y11_window_get(drawable_id) == NULL &&
        y11_resource_get(drawable_id, Y11_RESOURCE_PIXMAP) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE, drawable_id,
                                pkt[0]);
        return 0;
    }

    if (pid < c->resource_id_base ||
        pid - c->resource_id_base > Y11_RID_MASK ||
        y11_resource_get(pid, Y11_RESOURCE_PIXMAP) != NULL ||
        y11_window_get(pid) != NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, pid, pkt[0]);
        return 0;
    }

    /* stride = width * 4, alloc_size = stride * height, overflow-checked */
    stride = (size_t)width * 4u;
    if (height != 0 && stride > (size_t)-1 / height) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    alloc_size = stride * (size_t)height;

    p = calloc(1, sizeof(*p));
    if (p == NULL)
        goto badalloc;
    p->base.pixels = calloc(1, alloc_size);
    if (p->base.pixels == NULL) {
        free(p);
        goto badalloc;
    }

    p->base.id = pid;
    p->base.type = Y11_DRAWABLE_PIXMAP;
    p->base.width = width;
    p->base.height = height;
    p->base.depth = depth;
    p->base.bpp = 32;
    p->base.stride = stride;
    p->owner = c;
    p->is_shm = false;

    if (y11_resource_add(pid, Y11_RESOURCE_PIXMAP, p) != 0) {
        y11_pixmap_destroy(p);
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
 * FreePixmap (opcode 54): xResourceReq.  Only the creating client may
 * free a pixmap.
 */
int y11_pixmap_req_free(struct y11_client *c, const uint8_t *pkt,
                        size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    yid_t pid;
    struct y11_pixmap *p;

    if (len - data_off != 4u) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
        return 0;
    }
    pid = y11_wire_get32(body + 0);
    p = y11_resource_get(pid, Y11_RESOURCE_PIXMAP);
    if (p == NULL || p->owner != c) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_PIXMAP, pid, pkt[0]);
        return 0;
    }

    y11_resource_remove(pid);
    /*
     * libXcursor frees the source pixmap before RenderCreateCursor, so
     * a pixmap pictures still reference only leaves the id namespace;
     * the memory dies with the last picture.
     */
    if (p->picture_refs > 0) {
        p->orphaned = true;
        return 0;
    }
    y11_pixmap_destroy(p);
    return 0;                   /* no reply */
}

/*
 * Release every pixmap owned by `client` (called from the disconnect
 * path).
 */
void y11_pixmap_purge_client(struct y11_client *c)
{
    y11_resource_purge_type_arg(Y11_RESOURCE_PIXMAP, c,
                                y11_pixmap_belongs,
                                y11_pixmap_unref);
}

/* Drop one reference from a RENDER picture; free orphans. */
void y11_pixmap_picture_released(y11_drawable_t *d)
{
    struct y11_pixmap *p = (struct y11_pixmap *)d;

    if (d == NULL || d->type != Y11_DRAWABLE_PIXMAP)
        return;
    if (p->picture_refs > 0)
        p->picture_refs--;
    if (p->picture_refs == 0 && p->orphaned)
        y11_pixmap_destroy(p);
}

/* Unref with orphaning: used for FreePixmap and the disconnect purge. */
void y11_pixmap_unref(void *ptr)
{
    struct y11_pixmap *p = ptr;

    if (p == NULL)
        return;
    if (p->picture_refs > 0) {
        p->orphaned = true;
        return;
    }
    y11_pixmap_destroy(p);
}
