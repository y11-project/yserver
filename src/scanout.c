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

/* Hardware cursor (legacy KMS API, ARGB 64x64). */
static uint32_t y11_cursor_handle;
static uint32_t y11_cursor_map_size;
static uint32_t *y11_cursor_map;

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

static void y11_scanout_cursor_init(int fd)
{
    struct drm_mode_create_dumb create;
    struct drm_mode_map_dumb map;
    int y, x;

    memset(&create, 0, sizeof(create));
    create.width = 64;
    create.height = 64;
    create.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
        return;
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
    y11_cursor_handle = create.handle;
    memset(y11_cursor_map, 0, create.size);

    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            char c = y11_cursor_shape[y][x];
            union y11_cursor_pixel px;

            px.argb = 0;
            if (c == 'X') {              /* black outline */
                px.c.r = 0; px.c.g = 0; px.c.b = 0; px.c.a = 255;
            } else if (c == 'O') {       /* white fill */
                px.c.r = 255; px.c.g = 255; px.c.b = 255; px.c.a = 255;
            }
            y11_cursor_map[y * (create.pitch / 4) + x] = px.argb;
        }
    }
    return;

fail:
    {
        struct drm_mode_destroy_dumb d = { .handle = create.handle };

        (void)drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
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
        for (; out != NULL; out = out->next) {
            (void)drmModeSetCursor(out->drm_fd, out->crtc_id,
                                  y11_cursor_handle, 64, 64);
        }
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
                                   y11_cursor_handle, 64, 64);
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
    for (out = y11_drm_outputs(); out != NULL; out = out->next)
        (void)drmModeMoveCursor(out->drm_fd, out->crtc_id, x, y);
}

void y11_scanout_shutdown(void)
{
    if (y11_cursor_handle != 0 && y11_drm_outputs() != NULL) {
        struct y11_output *out;
        int fd = y11_drm_outputs()->drm_fd;
        struct drm_mode_destroy_dumb d = { .handle = y11_cursor_handle };

        for (out = y11_drm_outputs(); out != NULL; out = out->next)
            (void)drmModeSetCursor(out->drm_fd, out->crtc_id, 0, 0, 0);
        if (y11_cursor_map != NULL)
            munmap(y11_cursor_map, y11_cursor_map_size);
        y11_cursor_map = NULL;
        (void)drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        y11_cursor_handle = 0;
    }
    /* Hand the CRTCs back to the console before the buffers die. */
    y11_drm_restore_console();
    y11_scanout_root = NULL;
}
