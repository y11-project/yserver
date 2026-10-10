/*
 * scanout.c - Hardware scanout for the Y11 display server.
 *
 * The root window's 32bpp software backbuffer is the source of truth
 * for the screen.  Dirty regions are blitted into the back scanout
 * buffer and presented with a VBlank-synchronized page flip; the
 * completion event on the drm fd clears the pending flag and any
 * damage accumulated meanwhile schedules the next frame.  A hardware
 * cursor follows the pointer without touching the scanout buffers.
 *
 * Without a seat (another display server owns DRM master) every entry
 * point is a no-op and y11 stays headless.
 *
 * Copyright (c) 2026 The Y11 Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "y11_drm.h"

static y11_drawable_t *y11_scanout_root;
struct y11_scanout_damage {
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
    bool dirty;
};

static struct y11_scanout_damage y11_buf_dirty[2];

/* Hardware cursor (legacy KMS API, ARGB).  The BO is always the
 * driver's advertised cursor plane size (i915 only accepts 64x64 or
 * larger power-of-two squares, so smaller images are padded into the
 * plane's top-left corner, exactly like Xorg's modesetting driver).
 * The BO is recreated when a client cursor of a different size is
 * uploaded. */
static uint32_t y11_cursor_handle;
static uint32_t y11_cursor_map_size;
static uint32_t y11_cursor_map_pitch;
static uint32_t *y11_cursor_map;
static uint16_t y11_cursor_w, y11_cursor_h;    /* image size */
static uint16_t y11_cursor_bo_w, y11_cursor_bo_h;       /* BO size */
static uint16_t y11_cursor_plane_w, y11_cursor_plane_h;  /* plane caps */
static int16_t  y11_cursor_hot_x, y11_cursor_hot_y;

union y11_cursor_pixel {
    uint32_t argb;
    struct { uint8_t b, g, r, a; } c;
};

/* Classic left-leaning arrow: 1 = outline, 2 = fill. */
static const char *const y11_cursor_shape[32] = {
    "X...............................",
    "XX..............................",
    "XOX.............................",
    "XOOX............................",
    "XOOOX...........................",
    "XOOOOX..........................",
    "XOOOOOX.........................",
    "XOOOOOOX........................",
    "XOOOOOOOX.......................",
    "XOOOOOOOOX......................",
    "XOOOOOOOOOX.....................",
    "XOOOOOOOOOOX....................",
    "XOOOOOOOOOOOX...................",
    "XOOOOOOOOXXXX...................",
    "XOOOOXOOX.......................",
    "XOOOXXOOX.......................",
    "XOOX..XOOX......................",
    "XOX...XOOX......................",
    "XX.....XOOX.....................",
    "X......XOOX.....................",
    "........XOOX....................",
    "........XOOX....................",
    ".........XX.....................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................"
};

/* Rasterize the built-in arrow into X pixel-order uint32s. */
static uint32_t *y11_cursor_arrow_pixels(uint16_t *w, uint16_t *h)
{
    static uint32_t px[32 * 32];
    int y, x;

    memset(px, 0, sizeof(px));
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            char c = y11_cursor_shape[y][x];
            union y11_cursor_pixel p;

            p.argb = 0;
            if (c == 'X') {              /* black outline */
                p.c.r = 0; p.c.g = 0; p.c.b = 0; p.c.a = 255;
            } else if (c == 'O') {       /* white fill */
                p.c.r = 255; p.c.g = 255; p.c.b = 255; p.c.a = 255;
            }
            px[y * 32 + x] = p.argb;
        }
    }
    *w = 32;
    *h = 32;
    return px;
}

static void y11_scanout_cursor_bo_destroy(void)
{
    if (y11_cursor_handle != 0 && y11_drm_outputs() != NULL) {
        struct drm_mode_destroy_dumb d = { .handle = y11_cursor_handle };
        struct y11_output *out;

        for (out = y11_drm_outputs(); out != NULL; out = out->next)
            (void)drmModeSetCursor(out->drm_fd, out->crtc_id, 0, 0, 0);
        (void)drmIoctl(y11_drm_outputs()->drm_fd,
                       DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    if (y11_cursor_map != NULL && y11_cursor_map != MAP_FAILED)
        munmap(y11_cursor_map, y11_cursor_map_size);
    y11_cursor_map = NULL;
    y11_cursor_handle = 0;
    y11_cursor_bo_w = y11_cursor_bo_h = 0;
}

static int y11_scanout_cursor_bo_create(uint16_t width, uint16_t height)
{
    struct drm_mode_create_dumb create;
    struct drm_mode_map_dumb map;
    struct y11_output *out;
    int fd;

    out = y11_drm_outputs();
    if (out == NULL)
        return -1;
    fd = out->drm_fd;

    memset(&create, 0, sizeof(create));
    create.width = width;
    create.height = height;
    create.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
        return -1;
    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0)
        goto fail;
    y11_cursor_map = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, map.offset);
    if (y11_cursor_map == MAP_FAILED) {
        y11_cursor_map = NULL;
        goto fail;
    }
    y11_cursor_map_size = create.size;
    y11_cursor_map_pitch = create.pitch;
    y11_cursor_handle = create.handle;
    y11_cursor_bo_w = width;
    y11_cursor_bo_h = height;
    return 0;

fail:
    {
        struct drm_mode_destroy_dumb d = { .handle = create.handle };

        (void)drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    return -1;
}

/*
 * Upload a client cursor image to the cursor plane.  Pixels arrive in
 * X pixel order (A<<24|R<<16|G<<8|B), which is the KMS ARGB8888
 * layout, so rows copy verbatim.  The image is padded into the
 * driver's fixed-size cursor BO; if KMS rejects it anyway, the
 * recovery guard keeps the failure from recursing through the
 * default-restore path into a stack overflow.
 */
int y11_scanout_set_cursor(const uint32_t *argb, uint16_t width,
                           uint16_t height, int16_t hot_x, int16_t hot_y)
{
    static int recovering;
    struct y11_output *out;
    uint16_t y;

    if (y11_drm_outputs() == NULL)
        return -1;
    if (width == 0 || height == 0 || width > 256u || height > 256u)
        return -1;
    if (hot_x < 0 || (uint16_t)hot_x >= width ||
        hot_y < 0 || (uint16_t)hot_y >= height)
        return -1;
    if (y11_cursor_plane_w == 0 || y11_cursor_plane_h == 0)
        return -1;              /* caps not queried yet */
    if (width > y11_cursor_plane_w || height > y11_cursor_plane_h) {
        if (y11_debug)
            fprintf(stderr, "y11: cursor %ux%u exceeds the plane's "
                    "%ux%u, keeping the previous cursor\n",
                    width, height, y11_cursor_plane_w,
                    y11_cursor_plane_h);
        return -1;
    }

    if (y11_cursor_bo_w != y11_cursor_plane_w ||
        y11_cursor_bo_h != y11_cursor_plane_h) {
        y11_scanout_cursor_bo_destroy();
        if (y11_scanout_cursor_bo_create(y11_cursor_plane_w,
                                         y11_cursor_plane_h) != 0)
            return -1;
    }

    /* Zero the padding, then lay the image into the top-left. */
    memset(y11_cursor_map, 0, y11_cursor_map_size);
    for (y = 0; y < height; y++) {
        memcpy(&y11_cursor_map[y * (y11_cursor_map_pitch / 4u)],
               &argb[(size_t)y * width], (size_t)width * 4u);
    }

    y11_cursor_w = width;
    y11_cursor_h = height;
    y11_cursor_hot_x = hot_x;
    y11_cursor_hot_y = hot_y;

    for (out = y11_drm_outputs(); out != NULL; out = out->next) {
        if (drmModeSetCursor(out->drm_fd, out->crtc_id,
                             y11_cursor_handle, y11_cursor_plane_w,
                             y11_cursor_plane_h) != 0) {
            if (y11_debug)
                fprintf(stderr, "y11: drmModeSetCursor %ux%u failed: %s"
                        "\n", y11_cursor_plane_w, y11_cursor_plane_h,
                        strerror(errno));
            if (!recovering) {
                recovering = 1;
                y11_scanout_set_cursor_default();
                recovering = 0;
            }
            return -1;
        }
    }
    y11_scanout_move_cursor(y11_input_pointer()->root_x,
                            y11_input_pointer()->root_y);
    return 0;
}

/* Restore the built-in arrow (hot spot 0,0). */
void y11_scanout_set_cursor_default(void)
{
    uint16_t w = 32, h = 32;

    (void)y11_scanout_set_cursor(y11_cursor_arrow_pixels(&w, &h),
                                 w, h, 0, 0);
}

static void y11_scanout_cursor_init(int fd)
{
    struct drm_get_cap cap;
    uint64_t cw = 64, ch = 64;

    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CAP_CURSOR_WIDTH;
    if (drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value > 0)
        cw = cap.value;
    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CAP_CURSOR_HEIGHT;
    if (drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value > 0)
        ch = cap.value;
    /*
     * i915 answers these caps with its maximum plane size (256x256),
     * but the legacy drmModeSetCursor rejects anything above 64x64
     * with EPERM on hardware like the T2's Ice Lake.  64x64 is the
     * size the legacy API takes everywhere, and every cursor y11
     * serves (the 32px arrow, 48px themed cursors) fits inside.
     */
    if (cw > 64)
        cw = 64;
    if (ch > 64)
        ch = 64;
    y11_cursor_plane_w = (uint16_t)cw;
    y11_cursor_plane_h = (uint16_t)ch;
    if (y11_debug)
        fprintf(stderr, "y11: cursor plane %ux%u\n",
                y11_cursor_plane_w, y11_cursor_plane_h);

    y11_scanout_set_cursor_default();
}

/* ---- dirty tracking ---------------------------------------------------------- */

void y11_scanout_mark_dirty(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    int32_t x2 = x + (int32_t)w;
    int32_t y2 = y + (int32_t)h;
    int b;

    if (y11_scanout_root == NULL)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x2 > (int32_t)y11_scanout_root->width)
        x2 = y11_scanout_root->width;
    if (y2 > (int32_t)y11_scanout_root->height)
        y2 = y11_scanout_root->height;
    if (x >= x2 || y >= y2)
        return;

    for (b = 0; b < 2; b++) {
        if (!y11_buf_dirty[b].dirty) {
            y11_buf_dirty[b].x = x;
            y11_buf_dirty[b].y = y;
            y11_buf_dirty[b].w = (uint32_t)(x2 - x);
            y11_buf_dirty[b].h = (uint32_t)(y2 - y);
            y11_buf_dirty[b].dirty = true;
        } else {
            int32_t ox2 = y11_buf_dirty[b].x + (int32_t)y11_buf_dirty[b].w;
            int32_t oy2 = y11_buf_dirty[b].y + (int32_t)y11_buf_dirty[b].h;

            if (x < y11_buf_dirty[b].x)
                y11_buf_dirty[b].x = x;
            if (y < y11_buf_dirty[b].y)
                y11_buf_dirty[b].y = y;
            if (x2 > ox2)
                ox2 = x2;
            if (y2 > oy2)
                oy2 = y2;
            y11_buf_dirty[b].w = (uint32_t)(ox2 - y11_buf_dirty[b].x);
            y11_buf_dirty[b].h = (uint32_t)(oy2 - y11_buf_dirty[b].y);
        }
    }
}

/* ---- page flip events ---------------------------------------------------------- */

/*
 * The flip completed at VBlank: the presented buffer is now the front,
 * so the pipeline accepts the next frame.  Damage that arrived while
 * the flip was pending is flushed immediately.
 */
static void y11_scanout_flip_done(int fd, unsigned int sequence,
                                  unsigned int tv_sec, unsigned int tv_usec,
                                  void *user_data)
{
    struct y11_output *out = user_data;

    (void)fd;
    (void)sequence;
    (void)tv_sec;
    (void)tv_usec;
    out->pflip_pending = false;
    y11_scanout_flush();
}

static drmEventContext y11_drm_event_ctx = {
    .version = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = y11_scanout_flip_done
};

void y11_drm_handle_events(int fd)
{
    (void)drmHandleEvent(fd, &y11_drm_event_ctx);
}

/* ---- composition ---------------------------------------------------------------- */

/* Composite viewable windows onto the scanout buffer in stacking order. */
static void y11_scanout_composite_window(y11_drm_fb_t *fb,
                                         const struct y11_window *win,
                                         int32_t dx, int32_t dy,
                                         uint32_t dw, uint32_t dh)
{
    const struct y11_window *child;

    for (child = win->first_child; child != NULL; child = child->next_sibling) {
        if (child->map_state != Y11_MAP_STATE_VIEWABLE)
            continue;

        if (child->window_class != Y11_WINDOW_CLASS_INPUT_ONLY &&
            child->drawable.pixels != NULL) {
            int32_t cx1 = child->abs_x;
            int32_t cy1 = child->abs_y;
            int32_t cx2 = cx1 + (int32_t)child->width;
            int32_t cy2 = cy1 + (int32_t)child->height;

            int32_t ix1 = cx1 > dx ? cx1 : dx;
            int32_t iy1 = cy1 > dy ? cy1 : dy;
            int32_t ix2 = cx2 < (dx + (int32_t)dw) ? cx2 : (dx + (int32_t)dw);
            int32_t iy2 = cy2 < (dy + (int32_t)dh) ? cy2 : (dy + (int32_t)dh);

            if (ix1 < ix2 && iy1 < iy2) {
                int32_t row;
                int32_t cols = ix2 - ix1;

                for (row = iy1; row < iy2; row++) {
                    int32_t child_row = row - cy1;
                    int32_t child_col = ix1 - cx1;

                    memcpy((uint8_t *)fb->map + (size_t)row * fb->stride +
                               (size_t)ix1 * 4,
                           (uint8_t *)child->drawable.pixels +
                               (size_t)child_row * child->drawable.stride +
                               (size_t)child_col * 4,
                           (size_t)cols * 4);
                }
            }
        }

        y11_scanout_composite_window(fb, child, dx, dy, dw, dh);
    }
}

/* Blit one dirty rect from the root backbuffer into a scanout buffer. */
static void y11_scanout_blit(struct y11_output *out, int32_t x, int32_t y,
                             uint32_t w, uint32_t h)
{
    y11_drm_fb_t *fb = &out->buffers[out->back_buffer];
    int32_t rows = (int32_t)h;
    int32_t cols = (int32_t)w;
    int32_t row;
    struct y11_window *root;

    if (x + cols > (int32_t)fb->width)
        cols = (int32_t)fb->width - x;
    if (y + rows > (int32_t)fb->height)
        rows = (int32_t)fb->height - y;
    if (cols <= 0 || rows <= 0)
        return;

    for (row = 0; row < rows; row++) {
        memcpy((uint8_t *)fb->map + (size_t)(y + row) * fb->stride +
                   (size_t)x * 4,
               (uint8_t *)y11_scanout_root->pixels +
                   (size_t)(y + row) * y11_scanout_root->stride +
                   (size_t)x * 4,
               (size_t)cols * 4);
    }

    root = y11_window_get(Y11_SCREEN_ROOT);
    if (root != NULL)
        y11_scanout_composite_window(fb, root, x, y, (uint32_t)cols,
                                     (uint32_t)rows);
}

/*
 * Blit the accumulated dirty region and queue a page flip.  Skipped
 * while a flip is pending (the dirty region keeps accumulating) or
 * while the session is switched away from the VT.
 */
void y11_scanout_flush(void)
{
    struct y11_output *out;

    if (y11_scanout_root == NULL)
        return;
    if (!y11_buf_dirty[0].dirty && !y11_buf_dirty[1].dirty)
        return;
    if (!y11_session_is_active())
        return;
    if (y11_drm_outputs() == NULL) {
        y11_buf_dirty[0].dirty = false;
        y11_buf_dirty[1].dirty = false;
        return;
    }
    for (out = y11_drm_outputs(); out != NULL; out = out->next) {
        if (out->pflip_pending)
            return;             /* one frame in flight; keep the damage */
    }
    for (out = y11_drm_outputs(); out != NULL; out = out->next) {
        uint8_t bb = out->back_buffer;
        if (y11_buf_dirty[bb].dirty) {
            y11_scanout_blit(out, y11_buf_dirty[bb].x, y11_buf_dirty[bb].y,
                             y11_buf_dirty[bb].w, y11_buf_dirty[bb].h);
        }
        if (y11_drm_page_flip(out) == 0) {
            y11_buf_dirty[bb].dirty = false;
        } else {
            return;
        }
    }
}

/* ---- lifecycle -------------------------------------------------------------------- */

/*
 * Wire scanout to the root window: enable the hardware cursor on every
 * CRTC, present the initial frame and mark the whole screen dirty so
 * the first flush paints the background.
 */
int y11_scanout_init(y11_drawable_t *root)
{
    struct y11_output *out;

    if (root == NULL)
        return -1;
    y11_scanout_root = root;

    out = y11_drm_outputs();
    if (out == NULL)
        return -1;              /* headless: no outputs, no scanout */

    /* Initial modeset: point every CRTC at its front buffer.  This is
     * the DRM master gate - without master the ioctl fails with
     * EPERM and y11 falls back to the software screen. */
    if (y11_drm_mode_set_all() != 0)
        return -1;

    y11_scanout_cursor_init(out->drm_fd);
    if (y11_cursor_handle != 0) {
        y11_scanout_move_cursor(y11_input_pointer()->root_x,
                                y11_input_pointer()->root_y);
    }

    y11_scanout_mark_dirty(0, 0, root->width, root->height);
    y11_scanout_flush();
    return 0;
}

/*
 * VT switch-back (seat enabled): re-program every CRTC, re-show the
 * cursor and repaint the whole screen.
 */
void y11_scanout_restore(void)
{
    struct y11_output *out;

    if (y11_drm_outputs() == NULL)
        return;
    if (y11_drm_mode_set_all() != 0)
        return;
    for (out = y11_drm_outputs(); out != NULL; out = out->next) {
        out->pflip_pending = false;
        out->back_buffer = 1;
        if (y11_cursor_handle != 0) {
            (void)drmModeSetCursor(out->drm_fd, out->crtc_id,
                                   y11_cursor_handle,
                                   y11_cursor_plane_w,
                                   y11_cursor_plane_h);
        }
    }
    y11_scanout_mark_dirty(0, 0, y11_scanout_root->width,
                           y11_scanout_root->height);
    y11_scanout_flush();
}

void y11_scanout_move_cursor(int32_t x, int32_t y)
{
    struct y11_output *out;

    if (y11_cursor_handle == 0)
        return;
    /* KMS positions the image's top-left corner; X11 semantics put the
     * cursor's hot spot on the pointer, so offset by the hot spot. */
    x -= y11_cursor_hot_x;
    y -= y11_cursor_hot_y;
    for (out = y11_drm_outputs(); out != NULL; out = out->next)
        (void)drmModeMoveCursor(out->drm_fd, out->crtc_id, x, y);
}

void y11_scanout_shutdown(void)
{
    y11_scanout_cursor_bo_destroy();
    /* Hand the CRTCs back to the console before the buffers die. */
    y11_drm_restore_console();
    y11_scanout_root = NULL;
}
