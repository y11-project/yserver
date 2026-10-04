/*
 * session.c - Seat session management for the Y11 display server.
 *
 * libseat authenticates with seatd or systemd-logind so an
 * unprivileged process started from a TTY session receives a usable
 * fd for the DRM card.  The enable/disable callbacks implement VT
 * switching: on switch-away the seat is acknowledged and hardware
 * scanout stops; on switch-back DRM master is re-acquired, every
 * output is re-modeset and the screen is redrawn.
 *
 * When the seat is unavailable - no seat manager, or another display
 * server already owns the session - this fails cleanly and y11 runs
 * headless over its UNIX sockets.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "y11_drm.h"

static void y11_session_enable(struct libseat *seat, void *userdata)
{
    struct y11_session *s = userdata;

    (void)seat;
    s->active = true;
    if (y11_debug)
        fprintf(stderr, "y11: seat enabled (session active)\n");

    /* Re-acquire DRM master if it was dropped, repaint and rescan. */
    if (s->drm_card_fd >= 0 && !drmIsMaster(s->drm_card_fd))
        (void)drmSetMaster(s->drm_card_fd);
    y11_scanout_restore();
}

static void y11_session_disable(struct libseat *seat, void *userdata)
{
    struct y11_session *s = userdata;

    s->active = false;
    if (y11_debug)
        fprintf(stderr, "y11: seat disabled (VT switch away)\n");

    /* Drop master so the next VT can program the hardware. */
    if (s->drm_card_fd >= 0 && drmIsMaster(s->drm_card_fd))
        (void)drmDropMaster(s->drm_card_fd);

    /* Acknowledge the switch; failure to do so loses the devices. */
    (void)libseat_disable_seat(seat);
}

static const struct libseat_seat_listener y11_seat_listener = {
    .enable_seat = y11_session_enable,
    .disable_seat = y11_session_disable
};

/* Probe card nodes in order; returns the fd or -1. */
static int y11_session_open_card(struct y11_session *s, int *dev_id)
{
    static const char *const paths[] = {
        "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2", NULL
    };
    int i;

    for (i = 0; paths[i] != NULL; i++) {
        int id = libseat_open_device(s->seat, paths[i], &s->drm_card_fd);

        if (id >= 0) {
            *dev_id = id;
            return s->drm_card_fd;
        }
        if (y11_debug)
            fprintf(stderr, "y11: seat open %s: %s\n", paths[i],
                    strerror(errno));
    }
    return -1;
}

int y11_session_init(struct y11_session *s)
{
    memset(s, 0, sizeof(*s));
    s->drm_card_fd = -1;
    s->drm_device_id = -1;

    s->seat = libseat_open_seat(&y11_seat_listener, s);
    if (s->seat == NULL) {
        fprintf(stderr, "y11: no seat available (%s), running headless\n",
                strerror(errno));
        return -1;
    }
    s->seat_fd = libseat_get_fd(s->seat);
    if (s->seat_fd < 0) {
        fprintf(stderr, "y11: seat fd unavailable, running headless\n");
        libseat_close_seat(s->seat);
        s->seat = NULL;
        return -1;
    }

    /* Drain any initial events; the enable callback arms activation. */
    (void)libseat_dispatch(s->seat, 0);
    if (y11_session_open_card(s, &s->drm_device_id) < 0) {
        fprintf(stderr, "y11: seat granted no DRM card, running headless\n");
        libseat_close_seat(s->seat);
        s->seat = NULL;
        s->seat_fd = -1;
        return -1;
    }
    return 0;
}

void y11_session_dispatch(struct y11_session *s)
{
    if (s->seat != NULL)
        (void)libseat_dispatch(s->seat, 0);
}

void y11_session_shutdown(struct y11_session *s)
{
    if (s->drm_device_id >= 0 && s->seat != NULL)
        (void)libseat_close_device(s->seat, s->drm_device_id);
    if (s->drm_card_fd >= 0) {
        close(s->drm_card_fd);
        s->drm_card_fd = -1;
    }
    if (s->seat != NULL) {
        (void)libseat_close_seat(s->seat);
        s->seat = NULL;
    }
    s->seat_fd = -1;
}
