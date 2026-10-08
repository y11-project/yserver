/*
 * drm.c - KMS resource discovery, dumb buffer allocation and modeset
 * for the Y11 display server.
 *
 * Outputs are discovered from the connector list: every connected
 * connector with modes gets a preferred (or first) mode, a compatible
 * encoder/CRTC pair, and two mmap'ed dumb buffers wrapped as DRM
 * framebuffers for double-buffered scanout.
 *
 * All CRTC programming requires DRM master, which the session module
 * acquires through the seat.  Without master nothing here can program
 * hardware, and y11 stays headless.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "y11_drm.h"

static struct y11_output *y11_output_list;

/* ---- dumb buffers ---------------------------------------------------------- */

static void y11_drm_destroy_fb(int fd, y11_drm_fb_t *fb)
{
    if (fb->map != NULL && fb->map != MAP_FAILED) {
        munmap(fb->map, fb->size);
        fb->map = NULL;
    }
    if (fb->fb_id != 0) {
        drmModeRmFB(fd, fb->fb_id);
        fb->fb_id = 0;
    }
    if (fb->handle != 0) {
        struct drm_mode_destroy_dumb d = { .handle = fb->handle };

        (void)drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        fb->handle = 0;
    }
}

/*
 * Create one dumb buffer of width x height 32bpp, register it as a DRM
 * framebuffer and map it into userspace.
 */
static int y11_drm_create_fb(int fd, y11_drm_fb_t *fb, uint16_t width,
                             uint16_t height)
{
    struct drm_mode_create_dumb create;
    struct drm_mode_map_dumb map;
    uint64_t cap = 0;
    int r;

    memset(fb, 0, sizeof(*fb));
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) != 0 || cap == 0)
        return -1;

    memset(&create, 0, sizeof(create));
    create.width = width;
    create.height = height;
    create.bpp = 32;
    r = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    if (r != 0)
        return -1;

    r = drmModeAddFB(fd, width, height, 24, 32, create.pitch,
                     create.handle, &fb->fb_id);
    if (r != 0)
        goto fail;

    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    r = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    if (r != 0)
        goto fail_map;

    fb->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, map.offset);
    if (fb->map == MAP_FAILED) {
        fb->map = NULL;
        goto fail_map;
    }

    fb->handle = create.handle;
    fb->size = create.size;
    fb->stride = create.pitch;
    fb->width = width;
    fb->height = height;
    return 0;

fail_map:
    drmModeRmFB(fd, fb->fb_id);
    fb->fb_id = 0;
fail:
    {
        struct drm_mode_destroy_dumb d = { .handle = create.handle };

        (void)drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    return -1;
}

/* ---- output discovery -------------------------------------------------------- */

/* Pick the preferred mode, or the first listed mode. */
static const drmModeModeInfo *y11_drm_pick_mode(drmModeConnector *conn)
{
    int i;

    for (i = 0; i < conn->count_modes; i++) {
        if ((conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0)
            return &conn->modes[i];
    }
    return conn->count_modes > 0 ? &conn->modes[0] : NULL;
}

/* Find an encoder and CRTC that can drive this connector. */
static int y11_drm_pick_crtc(int fd, drmModeConnector *conn,
                             uint32_t *encoder_id, uint32_t *crtc_id,
                             drmModeRes *res)
{
    int e, c;
    drmModeEncoder *enc;

    /* Prefer the connector's currently attached encoder. */
    if (conn->encoder_id != 0) {
        enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc != NULL) {
            for (c = 0; c < res->count_crtcs; c++) {
                if ((enc->possible_crtcs & (1u << c)) != 0) {
                    *encoder_id = enc->encoder_id;
                    *crtc_id = res->crtcs[c];
                    drmModeFreeEncoder(enc);
                    return 0;
                }
            }
            drmModeFreeEncoder(enc);
        }
    }

    for (e = 0; e < conn->count_encoders; e++) {
        enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (enc == NULL)
            continue;
        for (c = 0; c < res->count_crtcs; c++) {
            if ((enc->possible_crtcs & (1u << c)) != 0) {
                *encoder_id = enc->encoder_id;
                *crtc_id = res->crtcs[c];
                drmModeFreeEncoder(enc);
                return 0;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return -1;
}

struct y11_output *y11_drm_outputs(void)
{
    return y11_output_list;
}

/*
 * Discover outputs and allocate their double buffers.  The session
 * must already hold the card fd.  Returns -1 when no output can be
 * driven, which keeps y11 headless.
 */
int y11_drm_init(struct y11_session *s)
{
    drmModeRes *res;
    int i;
    int found = 0;

    res = drmModeGetResources(s->drm_card_fd);
    if (res == NULL) {
        fprintf(stderr, "y11: drmModeGetResources failed: %s\n",
                strerror(errno));
        return -1;
    }

    for (i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn =
            drmModeGetConnector(s->drm_card_fd, res->connectors[i]);
        const drmModeModeInfo *mode;
        struct y11_output *out;
        uint32_t encoder_id, crtc_id;
        int b;

        if (conn == NULL)
            continue;
        if (conn->connection != DRM_MODE_CONNECTED ||
            conn->count_modes == 0) {
            drmModeFreeConnector(conn);
            continue;
        }
        mode = y11_drm_pick_mode(conn);
        if (mode == NULL) {
            drmModeFreeConnector(conn);
            continue;
        }
        if (y11_drm_pick_crtc(s->drm_card_fd, conn, &encoder_id, &crtc_id,
                              res) != 0) {
            fprintf(stderr, "y11: connector %u has no usable CRTC\n",
                   conn->connector_id);
            drmModeFreeConnector(conn);
            continue;
        }

        out = calloc(1, sizeof(*out));
        if (out == NULL) {
            drmModeFreeConnector(conn);
            continue;
        }
        out->connector_id = conn->connector_id;
        out->crtc_id = crtc_id;
        out->encoder_id = encoder_id;
        out->mode = *mode;
        out->drm_fd = s->drm_card_fd;
        out->back_buffer = 1;   /* so the first flush draws into 0 */

        for (b = 0; b < 2; b++) {
            if (y11_drm_create_fb(s->drm_card_fd, &out->buffers[b],
                                  mode->hdisplay, mode->vdisplay) != 0)
                break;
        }
        if (b != 2) {
            fprintf(stderr, "y11: dumb buffer allocation failed: %s\n",
                    strerror(errno));
            y11_drm_destroy_fb(s->drm_card_fd, &out->buffers[0]);
            free(out);
            drmModeFreeConnector(conn);
            continue;
        }

        out->next = y11_output_list;
        y11_output_list = out;
        found++;
        fprintf(stderr, "y11: output connector %u crtc %u %ux%u@%u\n",
                out->connector_id, out->crtc_id, mode->hdisplay,
                mode->vdisplay, mode->vrefresh);
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    return found > 0 ? 0 : -1;
}

/* Point every output's CRTC at its front buffer. */
int y11_drm_mode_set_all(void)
{
    struct y11_output *out;

    for (out = y11_output_list; out != NULL; out = out->next) {
        uint8_t front = (uint8_t)(out->back_buffer ^ 1);

        /*
         * Capture the console's CRTC state the first time this CRTC
         * is programmed: on shutdown the original framebuffer is set
         * back so the text console keeps working after y11 exits.
         */
        if (!out->console_saved) {
            drmModeCrtcPtr crtc = drmModeGetCrtc(out->drm_fd,
                                                 out->crtc_id);

            if (crtc != NULL) {
                if (crtc->buffer_id != 0 && crtc->mode_valid) {
                    out->console_saved = true;
                    out->console_fb = crtc->buffer_id;
                    out->console_x = crtc->x;
                    out->console_y = crtc->y;
                    out->console_mode = crtc->mode;
                }
                drmModeFreeCrtc(crtc);
            }
        }

        {
            int r = drmModeSetCrtc(out->drm_fd, out->crtc_id,
                                   out->buffers[front].fb_id, 0, 0,
                                   &out->connector_id, 1, &out->mode);

            if (r != 0) {
                fprintf(stderr, "y11: drmModeSetCrtc(%u): %s\n",
                        out->crtc_id, strerror(errno));
                return -1;
            }
        }
    }
    return 0;
}

/*
 * Hand every CRTC back to the console it displaced, restoring the
 * framebuffer and mode captured at modeset time.
 */
void y11_drm_restore_console(void)
{
    struct y11_output *out;

    for (out = y11_output_list; out != NULL; out = out->next) {
        if (!out->console_saved)
            continue;
        (void)drmModeSetCrtc(out->drm_fd, out->crtc_id, out->console_fb,
                             out->console_x, out->console_y,
                             &out->connector_id, 1, &out->console_mode);
        out->console_saved = false;
    }
}

/*
 * Queue a VBlank-synchronized flip of the back buffer: the CRTC
 * switches at the next vertical blank, the completion event arrives on
 * the drm fd and the buffer indices swap so drawing continues on the
 * freshly retired buffer.
 */
int y11_drm_page_flip(struct y11_output *out)
{
    uint8_t front = (uint8_t)(out->back_buffer ^ 1);
    int r;

    if (out->pflip_pending)
        return 0;               /* one flip in flight at a time */
    r = drmModePageFlip(out->drm_fd, out->crtc_id,
                        out->buffers[front].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, out);
    if (r != 0) {
        fprintf(stderr, "y11: drmModePageFlip(%u): %s\n", out->crtc_id,
                strerror(errno));
        return -1;
    }
    out->pflip_pending = true;
    out->back_buffer ^= 1;
    return 0;
}

void y11_drm_shutdown(void)
{
    struct y11_output *out = y11_output_list;

    while (out != NULL) {
        struct y11_output *next = out->next;
        int b;

        for (b = 0; b < 2; b++)
            y11_drm_destroy_fb(out->drm_fd, &out->buffers[b]);
        free(out);
        out = next;
    }
    y11_output_list = NULL;
}
