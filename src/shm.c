/*
 * shm.c - MIT-SHM shared memory transport for the Y11 display server.
 *
 * Clients register SysV shared memory segments (ShmAttach) and then
 * reference them directly: ShmPutImage blits pixel rectangles from
 * the client's segment into drawables, ShmGetImage reads pixels back
 * into the segment, and ShmCreatePixmap backs a pixmap with client
 * memory.  Nothing crosses the UNIX socket except the small request
 * itself, and an optional ShmCompletion event reports when the
 * operation is done.
 *
 * Segment ids are validated with shmctl(IPC_STAT) before shmat(2), and
 * every path bounds-checks against the segment size so a hostile
 * client cannot make y11 read or write outside the mapping.  On
 * disconnect every segment is detached with shmdt(2).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#include "y11.h"
#include "y11_wire.h"

/* Sub-opcodes (shmproto.h). */
enum {
    Y11_SHM_QUERY_VERSION = 0,
    Y11_SHM_ATTACH        = 1,
    Y11_SHM_DETACH        = 2,
    Y11_SHM_PUT_IMAGE     = 3,
    Y11_SHM_GET_IMAGE     = 4,
    Y11_SHM_CREATE_PIXMAP = 5
};

static struct y11_shm_seg *y11_shm_get(yid_t id)
{
    return y11_resource_get(id, Y11_RESOURCE_SHMSEG);
}

/* A pixmap whose backing memory lies inside this segment. */
static int y11_shm_pixmap_in_seg(void *ptr, void *arg)
{
    struct y11_pixmap *p = ptr;
    struct y11_shm_seg *seg = arg;
    const uint8_t *mem = (const uint8_t *)p->base.pixels;

    return p->is_shm && mem >= (const uint8_t *)seg->addr &&
           mem < (const uint8_t *)seg->addr + seg->size;
}

/*
 * Destroy every shared pixmap backed by `seg`.  The memory belongs to
 * the client, so destruction only frees the metadata (is_shm skips the
 * pixel free in y11_pixmap_destroy).
 */
void y11_shm_purge_pixmaps(struct y11_shm_seg *seg)
{
    y11_resource_purge_type_arg(Y11_RESOURCE_PIXMAP, seg,
                                y11_shm_pixmap_in_seg, y11_pixmap_destroy);
}

/* ---- helpers ------------------------------------------------------------------ */

/* Validate a client-chosen resource id: in range and unused. */
static int y11_shm_check_id(struct y11_client *c, yid_t id)
{
    if (id < c->resource_id_base || id - c->resource_id_base > Y11_RID_MASK)
        return -1;
    if (y11_shm_get(id) != NULL || y11_window_get(id) != NULL ||
        y11_resource_get(id, Y11_RESOURCE_PIXMAP) != NULL ||
        y11_resource_get(id, Y11_RESOURCE_GC) != NULL)
        return -1;
    return 0;
}

/* ---- ShmQueryVersion (0) --------------------------------------------------------- */

static int y11_shm_query_version(struct y11_client *c, const uint8_t *pkt,
                                 size_t len, size_t data_off)
{
    y11_shm_query_version_reply rep;

    (void)pkt;
    (void)data_off;
    if (len != sizeof(y11_req))
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 1;           /* sharedPixmaps: supported */
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put16(&rep.major_version, 1);
    y11_wire_put16(&rep.minor_version, 2);
    y11_wire_put16(&rep.uid, (uint16_t)getuid());
    y11_wire_put16(&rep.gid, (uint16_t)getgid());
    rep.pixmap_format = 2;      /* ZPixmap */

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* ---- ShmAttach (1) / ShmDetach (2) -------------------------------------------------- */

static int y11_shm_attach(struct y11_client *c, const uint8_t *pkt,
                          size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    yid_t seg_id;
    uint32_t shmid;
    struct shmid_ds ds;
    struct y11_shm_seg *seg;
    void *addr;

    if (len - data_off != 12u)  /* shmseg, shmid, read-only, pad */
        return y11_dispatch_bad_length(c, pkt[0]);

    seg_id = y11_wire_get32(body + 0);
    shmid = y11_wire_get32(body + 4);
    if (y11_shm_check_id(c, seg_id) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, seg_id, pkt[0]);
        return 0;
    }

    /* Validate the segment exists and read its size. */
    if (shmctl((int)shmid, IPC_STAT, &ds) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ACCESS, seg_id, pkt[0]);
        return 0;
    }

    addr = shmat((int)shmid, NULL, body[8] != 0 ? SHM_RDONLY : 0);
    if (addr == (void *)-1) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ACCESS, seg_id, pkt[0]);
        return 0;
    }

    seg = calloc(1, sizeof(*seg));
    if (seg == NULL) {
        (void)shmdt(addr);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    seg->id = seg_id;
    seg->owner = c;
    seg->shmid = (int)shmid;
    seg->addr = addr;
    seg->size = (size_t)ds.shm_segsz;
    seg->read_only = body[8] != 0;

    if (y11_resource_add(seg_id, Y11_RESOURCE_SHMSEG, seg) != 0) {
        (void)shmdt(addr);
        free(seg);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    return 0;                   /* no reply */
}

static int y11_shm_detach(struct y11_client *c, const uint8_t *pkt,
                           size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    yid_t seg_id;
    struct y11_shm_seg *seg;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    seg_id = y11_wire_get32(body + 0);
    seg = y11_shm_get(seg_id);
    if (seg == NULL || seg->owner != c) {
        y11_dispatch_send_error(c, (uint8_t)Y11_SHM_FIRST_ERROR, seg_id,
                                pkt[0]);       /* BadShmSeg */
        return 0;
    }

    /*
     * Shared pixmaps point straight into this mapping: destroy them
     * before it goes away so no drawable keeps a dangling pointer.
     */
    y11_shm_purge_pixmaps(seg);

    y11_resource_remove(seg_id);
    (void)shmdt(seg->addr);
    free(seg);
    return 0;                   /* no reply */
}

/* ---- ShmPutImage (3) --------------------------------------------------------------- */

/*
 * Blit a sub-rectangle of the client's shared image into a drawable.
 * Bounds are verified against the segment size before any memory is
 * touched; the GXcopy fast path is a row memcpy, other raster ops go
 * through the GC's function and plane mask pixel by pixel.
 */
static int y11_shm_put_image(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t drawable_id, gc_id, shmseg_id, offset;
    uint16_t total_width, src_x, src_y, src_width, src_height;
    int16_t dst_x, dst_y;
    uint8_t depth, format, send_event;
    struct y11_shm_seg *seg;
    struct y11_gc *gc;
    y11_drawable_t *d;
    uint64_t stride, need;
    uint8_t *src;
    int32_t row;

    if (len - data_off != 36u)
        return y11_dispatch_bad_length(c, pkt[0]);

    drawable_id = y11_wire_get32(body + 0);
    gc_id = y11_wire_get32(body + 4);
    total_width = y11_wire_get16(body + 8);
    src_x = y11_wire_get16(body + 12);
    src_y = y11_wire_get16(body + 14);
    src_width = y11_wire_get16(body + 16);
    src_height = y11_wire_get16(body + 18);
    dst_x = (int16_t)y11_wire_get16(body + 20);
    dst_y = (int16_t)y11_wire_get16(body + 22);
    depth = body[24];
    format = body[25];
    send_event = body[26];
    shmseg_id = y11_wire_get32(body + 28);
    offset = y11_wire_get32(body + 32);

    if (src_width == 0 || src_height == 0 || total_width == 0)
        return 0;               /* nothing to blit */

    seg = y11_shm_get(shmseg_id);
    if (seg == NULL || seg->owner != c) {
        y11_dispatch_send_error(c, (uint8_t)Y11_SHM_FIRST_ERROR, shmseg_id,
                                pkt[0]);       /* BadShmSeg */
        return 0;
    }
    gc = y11_resource_get(gc_id, Y11_RESOURCE_GC);
    d = y11_drawable_lookup(drawable_id);
    if (d == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE, drawable_id,
                                pkt[0]);
        return 0;
    }
    if (gc == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_GCONTEXT, gc_id, pkt[0]);
        return 0;
    }
    if (d->pixels == NULL || depth != d->depth || format != 2) {
        /* only ZPixmap at the drawable's depth is supported */
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, drawable_id, pkt[0]);
        return 0;
    }
    if (seg->read_only) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ACCESS, shmseg_id, pkt[0]);
        return 0;
    }

    /* Bounds check: the last blitted byte must be inside the segment. */
    stride = (uint64_t)total_width * 4u;
    if (src_x + src_width > total_width || src_y + src_height >
        (uint16_t)(seg->size / (stride > 0 ? stride : 1))) {
        y11_dispatch_send_error(c, (uint8_t)Y11_SHM_FIRST_ERROR, shmseg_id,
                                pkt[0]);       /* BadShmSeg */
        return 0;
    }
    need = offset + (uint64_t)(src_y + src_height - 1u) * stride +
            (uint64_t)(src_x + src_width) * 4u;
    if (need > (uint64_t)seg->size) {
        y11_dispatch_send_error(c, (uint8_t)Y11_SHM_FIRST_ERROR, shmseg_id,
                                pkt[0]);       /* BadShmSeg */
        return 0;
    }

    src = (uint8_t *)seg->addr + offset;

    /* Clip the destination rectangle to the drawable bounds. */
    {
        int32_t sx = dst_x;
        int32_t sy = dst_y;
        uint32_t skip_left = 0, skip_top = 0;
        uint32_t w = src_width, h = src_height;

        if (sx < 0) {
            skip_left = (uint32_t)(-sx);
            sx = 0;
        }
        if (sy < 0) {
            skip_top = (uint32_t)(-sy);
            sy = 0;
        }
        if (w > skip_left && sx + (int32_t)(w - skip_left) >
            (int32_t)d->width)
            w = (uint32_t)((int32_t)d->width - sx) + skip_left;
        if (h > skip_top && sy + (int32_t)(h - skip_top) >
            (int32_t)d->height)
            h = (uint32_t)((int32_t)d->height - sy) + skip_top;
        if (w <= skip_left || h <= skip_top) {
            if (send_event != 0)
                goto complete;
            return 0;
        }

        /* GXcopy with a full plane mask is a row memcpy. */
        if (gc->function == 3 && gc->plane_mask == 0xFFFFFFFFu) {
            for (row = 0; row < (int32_t)(h - skip_top); row++) {
                const uint8_t *s = src +
                    (size_t)(src_y + skip_top + row) * stride +
                    (size_t)(src_x + skip_left) * 4u;
                uint8_t *dp = (uint8_t *)d->pixels +
                    ((size_t)(sy + row) * (d->stride / 4u) +
                     (size_t)sx) * 4u;

                memcpy(dp, s, (size_t)(w - skip_left) * 4u);
            }
        } else {
            uint32_t col;

            for (row = 0; row < (int32_t)(h - skip_top); row++) {
                for (col = 0; col < w - skip_left; col++) {
                    uint32_t pix = y11_wire_get32(
                        src + (size_t)(src_y + skip_top + row) * stride +
                        (size_t)(src_x + skip_left + col) * 4u);

                    y11_render_pixel_ex(d, gc, (size_t)(sx + col),
                                        (size_t)(sy + row), pix);
                }
            }
        }

        if (d->type == Y11_DRAWABLE_WINDOW)
            y11_damage_drawn(d, dst_x, dst_y, src_width, src_height);
    }

complete:
    if (send_event != 0) {
        y11_shm_completion_event ev;

        memset(&ev, 0, sizeof(ev));
        {
            uint8_t *b = (uint8_t *)&ev;
            b[0] = (uint8_t)Y11_SHM_FIRST_EVENT;
        }
        y11_wire_put32(&ev.drawable, drawable_id);
        y11_wire_put16(&ev.minor_event, Y11_SHM_PUT_IMAGE);
        {
            uint8_t *b = (uint8_t *)&ev;
            b[10] = (uint8_t)Y11_SHM_EXT_OPCODE;
        }
        y11_wire_put32(&ev.shmseg, shmseg_id);
        y11_wire_put32(&ev.offset, offset);
        y11_event_dispatch32(c, &ev, sizeof(ev));
    }
    return 0;                   /* no reply */
}

/* ---- ShmGetImage (4) ------------------------------------------------------------------ */

/*
 * Read a rectangle from a drawable into the client's shared segment.
 * The reply carries no pixel data: it only reports completion and the
 * drawable's visual; the pixels land in shared memory at `offset`.
 */
static int y11_shm_get_image(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t drawable_id, shmseg_id, offset, plane_mask;
    int16_t x, y;
    uint16_t width, height;
    uint8_t format;
    struct y11_shm_seg *seg;
    y11_drawable_t *d;
    struct y11_window *win;
    y11_shm_get_image_reply rep;
    uint64_t need;
    int32_t row;

    if (len - data_off != 28u)
        return y11_dispatch_bad_length(c, pkt[0]);

    drawable_id = y11_wire_get32(body + 0);
    x = (int16_t)y11_wire_get16(body + 4);
    y = (int16_t)y11_wire_get16(body + 6);
    width = y11_wire_get16(body + 8);
    height = y11_wire_get16(body + 10);
    plane_mask = y11_wire_get32(body + 12);
    format = body[16];
    shmseg_id = y11_wire_get32(body + 20);
    offset = y11_wire_get32(body + 24);

    seg = y11_shm_get(shmseg_id);
    if (seg == NULL || seg->owner != c) {
        y11_dispatch_send_error(c, (uint8_t)Y11_SHM_FIRST_ERROR, shmseg_id,
                                pkt[0]);       /* BadShmSeg */
        return 0;
    }
    d = y11_drawable_lookup(drawable_id);
    if (d == NULL || d->pixels == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE, drawable_id,
                                pkt[0]);
        return 0;
    }
    if (width == 0 || height == 0 || format != 2) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, format, pkt[0]);
        return 0;
    }

    need = offset + (uint64_t)width * (uint64_t)height * 4u;
    if (need > (uint64_t)seg->size) {
        y11_dispatch_send_error(c, (uint8_t)Y11_SHM_FIRST_ERROR, shmseg_id,
                                pkt[0]);       /* BadShmSeg */
        return 0;
    }

    for (row = 0; row < height; row++) {
        int32_t sy_ = (int32_t)y + row;
        uint32_t *dst = (uint32_t *)((uint8_t *)seg->addr + offset) +
                        (size_t)row * width;
        int32_t col;

        if (sy_ < 0 || sy_ >= (int32_t)d->height)
            memset(dst, 0, (size_t)width * 4u);   /* out of bounds: zero */
        else {
            for (col = 0; col < (int32_t)width; col++) {
                int32_t sx_ = (int32_t)x + col;

                if (sx_ < 0 || sx_ >= (int32_t)d->width)
                    dst[col] = 0;
                else {
                    dst[col] = d->pixels[(size_t)sy_ * (d->stride / 4u) +
                                         (size_t)sx_] & plane_mask;
                }
            }
        }
    }

    win = y11_window_get(drawable_id);
    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = d->depth;
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put32(&rep.visual, win != NULL ? win->visual_id : 0u);
    y11_wire_put32(&rep.size, (uint32_t)((uint64_t)width * height * 4u));

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* ---- ShmCreatePixmap (5) ---------------------------------------------------------------- */

/*
 * Create a pixmap backed directly by the client's shared memory at
 * `offset`: no internal buffer is allocated and FreePixmap never
 * frees the client's memory.
 */
static int y11_shm_create_pixmap(struct y11_client *c, const uint8_t *pkt,
                                 size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    yid_t pid, shmseg_id;
    uint32_t drawable_id, offset;
    uint16_t width, height;
    uint8_t depth;
    struct y11_shm_seg *seg;
    struct y11_pixmap *p;
    uint64_t need;

    if (len - data_off != 24u)
        return y11_dispatch_bad_length(c, pkt[0]);

    pid = y11_wire_get32(body + 0);
    drawable_id = y11_wire_get32(body + 4);
    width = y11_wire_get16(body + 8);
    height = y11_wire_get16(body + 10);
    depth = body[12];
    shmseg_id = y11_wire_get32(body + 16);
    offset = y11_wire_get32(body + 20);

    /* The drawable argument selects the screen; it must resolve. */
    if (y11_window_get(drawable_id) == NULL &&
        y11_resource_get(drawable_id, Y11_RESOURCE_PIXMAP) == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE, drawable_id,
                                pkt[0]);
        return 0;
    }

    if (width == 0 || height == 0 || (depth != 1 && depth != 24)) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_VALUE, depth, pkt[0]);
        return 0;
    }
    seg = y11_shm_get(shmseg_id);
    if (seg == NULL || seg->owner != c) {
        y11_dispatch_send_error(c, (uint8_t)Y11_SHM_FIRST_ERROR, shmseg_id,
                                pkt[0]);       /* BadShmSeg */
        return 0;
    }
    if (seg->read_only) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ACCESS, shmseg_id, pkt[0]);
        return 0;
    }
    if (y11_shm_check_id(c, pid) != 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, pid, pkt[0]);
        return 0;
    }

    need = offset + (uint64_t)width * (uint64_t)height * 4u;
    if (need > (uint64_t)seg->size) {
        y11_dispatch_send_error(c, (uint8_t)Y11_SHM_FIRST_ERROR, shmseg_id,
                                pkt[0]);       /* BadShmSeg */
        return 0;
    }

    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    p->base.id = pid;
    p->base.type = Y11_DRAWABLE_PIXMAP;
    p->base.width = width;
    p->base.height = height;
    p->base.depth = depth;
    p->base.bpp = 32;
    p->base.stride = (size_t)width * 4u;
    p->base.pixels = (uint32_t *)((uint8_t *)seg->addr + offset);
    p->owner = c;
    p->is_shm = true;

    if (y11_resource_add(pid, Y11_RESOURCE_PIXMAP, p) != 0) {
        free(p);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    return 0;                   /* no reply */
}

/* ---- dispatcher --------------------------------------------------------------------------- */

int y11_shm_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                size_t data_off)
{
    switch (pkt[1]) {
    case Y11_SHM_QUERY_VERSION:
        return y11_shm_query_version(c, pkt, len, data_off);
    case Y11_SHM_ATTACH:
        return y11_shm_attach(c, pkt, len, data_off);
    case Y11_SHM_DETACH:
        return y11_shm_detach(c, pkt, len, data_off);
    case Y11_SHM_PUT_IMAGE:
        return y11_shm_put_image(c, pkt, len, data_off);
    case Y11_SHM_GET_IMAGE:
        return y11_shm_get_image(c, pkt, len, data_off);
    case Y11_SHM_CREATE_PIXMAP:
        return y11_shm_create_pixmap(c, pkt, len, data_off);
    default:
        y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, pkt[1], pkt[0]);
        return 0;
    }
}

/* ---- cleanup -------------------------------------------------------------------------------- */

static void y11_shm_destroy(void *ptr)
{
    struct y11_shm_seg *seg = ptr;

    if (seg == NULL)
        return;
    (void)shmdt(seg->addr);
    free(seg);
}

static int y11_shm_belongs(void *ptr, struct y11_client *client)
{
    return ((const struct y11_shm_seg *)ptr)->owner == client;
}

/* Detach every segment owned by `client` at disconnect. */
void y11_shm_purge_client(struct y11_client *c)
{
    y11_resource_purge_type(Y11_RESOURCE_SHMSEG, c, y11_shm_belongs,
                            y11_shm_destroy);
}
