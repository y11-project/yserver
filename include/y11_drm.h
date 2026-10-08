/*
 * y11_drm.h - DRM/KMS modesetting, session control and hardware
 * scanout for the Y11 display server.
 *
 * The session module (src/session.c) authenticates with seatd or
 * systemd-logind through libseat, acquiring the DRM card without root.
 * The DRM module (src/drm.c) discovers connectors, CRTCs and modes,
 * and allocates double-buffered dumb buffers for scanout.  The scanout
 * module (src/scanout.c) blits the root window backbuffer into the
 * scanout buffers and drives VBlank-synchronized page flips.
 *
 * When the seat cannot be acquired (another display server owns DRM
 * master, or no seat manager is running), all of this is inert and
 * y11 runs headless over its UNIX sockets exactly as before.
 */

#ifndef Y11_DRM_H
#define Y11_DRM_H

#include <stdbool.h>
#include <stdint.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "libseat.h"
#include "y11.h"

/* ---- dumb buffer ------------------------------------------------------------ */

/* A scanout-capable dumb buffer, mmap'ed into userspace. */
typedef struct y11_drm_fb {
    uint32_t fb_id;             /* DRM framebuffer ID (drmModeAddFB) */
    uint32_t handle;            /* GEM handle */
    uint32_t size;              /* total bytes */
    uint32_t stride;            /* pitch in bytes */
    uint16_t width;
    uint16_t height;
    uint32_t *map;              /* userspace mmap pointer */
} y11_drm_fb_t;

/* ---- outputs ----------------------------------------------------------------- */

/* One connected output: a connector driven by a CRTC at a fixed mode. */
typedef struct y11_output {
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t encoder_id;
    drmModeModeInfo mode;
    int drm_fd;
    y11_drm_fb_t buffers[2];    /* double buffer: 0 and 1 */
    uint8_t back_buffer;        /* index currently being drawn into */
    bool pflip_pending;         /* waiting for the VBlank flip event */
    bool console_saved;         /* console CRTC state captured */
    uint32_t console_fb;        /* framebuffer the console was scanning out */
    uint32_t console_x, console_y;
    drmModeModeInfo console_mode;
    struct y11_output *next;
} y11_output_t;

/* ---- session ------------------------------------------------------------------ */

/* Seat session state managed through libseat. */
typedef struct y11_session {
    struct libseat *seat;
    int seat_fd;                /* libseat pollable connection */
    int drm_card_fd;            /* the card node fd */
    int drm_device_id;          /* assigned by libseat_open_device */
    int tty_fd;                 /* console tty (direct path only) */
    bool active;                /* false while switched away from the VT */
    bool pending_ack;           /* disable ack deferred out of the callback */
} y11_session_t;

/* ---- src/session.c -------------------------------------------------------------- */

int  y11_session_init(struct y11_session *s);
void y11_session_shutdown(struct y11_session *s);
void y11_session_dispatch(struct y11_session *s);

/* ---- src/drm.c ------------------------------------------------------------------- */

int              y11_drm_init(struct y11_session *s);
void             y11_drm_shutdown(void);
struct y11_output *y11_drm_outputs(void);
void             y11_drm_restore_console(void);
void             y11_drm_set_tty(int fd);
int              y11_drm_mode_set_all(void);
int              y11_drm_page_flip(struct y11_output *out);

/* ---- src/scanout.c (events) ------------------------------------------------- */

void y11_drm_handle_events(int fd);

/* ---- src/scanout.c ----------------------------------------------------------------- */

int  y11_scanout_init(y11_drawable_t *root);
void y11_scanout_shutdown(void);
void y11_scanout_flush(void);
void y11_scanout_restore(void);
void y11_scanout_move_cursor(int32_t x, int32_t y);

#endif /* Y11_DRM_H */
