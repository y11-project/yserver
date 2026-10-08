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
 * server already owns the session - this falls back to taking DRM
 * master directly on an idle console (the kernel allows it when no
 * other master exists); if that also fails, y11 runs headless over
 * its UNIX sockets and never disturbs a running session.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/vt.h>
#include <drm/drm.h>

#include "y11_drm.h"

/*
 * VT switch flags, set from the signal handlers and consumed in the
 * dispatch loop (async-signal-safe: handlers only set flags).
 */
static volatile sig_atomic_t y11_vt_switch_away;
static volatile sig_atomic_t y11_vt_switch_back;

static void y11_vt_signal(int sig)
{
    if (sig == SIGUSR1)
        y11_vt_switch_away = 1;
    else if (sig == SIGUSR2)
        y11_vt_switch_back = 1;
}

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

/*
 * Direct fallback: no seat manager is running (no seatd socket, no
 * logind).  This path REQUIRES a controlling terminal: the tty is
 * put in VT_PROCESS mode so y11 owns VT switching - master drops on
 * switch-away and comes back on switch-back, which keeps the console
 * usable at all times.  Without a controlling terminal (daemons,
 * remote shells) the path refuses: holding the display with no way
 * to release it would trap whoever is in front of the screen.
 */
static int y11_session_direct(struct y11_session *s)
{
    static const char *const paths[] = {
        "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2", NULL
    };
    struct vt_mode vtm;
    int i, fd = -1, tty_fd;

    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0)
        return -1;              /* no controlling terminal */

    for (i = 0; paths[i] != NULL; i++) {
        fd = open(paths[i], O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0)
            break;
        if (y11_debug)
            fprintf(stderr, "y11: direct %s: %s\n", paths[i],
                    strerror(errno));
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        close(tty_fd);
        return -1;
    }

    s->drm_card_fd = fd;
    s->drm_device_id = -1;      /* no libseat device */
    s->tty_fd = tty_fd;
    s->active = true;
    y11_drm_set_tty(tty_fd);

    /* Own the VT switches; the kernel signals us to release/accept. */
    memset(&vtm, 0, sizeof(vtm));
    vtm.mode = VT_PROCESS;
    vtm.relsig = SIGUSR1;
    vtm.acqsig = SIGUSR2;
    (void)ioctl(tty_fd, VT_SETMODE, &vtm);
    (void)signal(SIGUSR1, y11_vt_signal);
    (void)signal(SIGUSR2, y11_vt_signal);
    return fd;
}

int y11_session_init(struct y11_session *s)
{
    memset(s, 0, sizeof(*s));
    s->drm_card_fd = -1;
    s->drm_device_id = -1;
    s->tty_fd = -1;

    s->seat = libseat_open_seat(&y11_seat_listener, s);
    if (s->seat == NULL) {
        if (y11_session_direct(s) >= 0) {
            fprintf(stderr, "y11: no seat manager; took the console"
                    " with VT_PROCESS switching\n");
            return 0;
        }
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
    if (s->seat != NULL) {
        (void)libseat_dispatch(s->seat, 0);
        return;
    }

    /* Direct path: consume the VT switch flags. */
    if (y11_vt_switch_away) {
        y11_vt_switch_away = 0;
        if (s->active) {
            /* Hand the display back to the console, then allow the
             * switch to proceed. */
            y11_drm_restore_console();
            if (s->drm_card_fd >= 0)
                (void)ioctl(s->drm_card_fd, DRM_IOCTL_DROP_MASTER, 0);
            s->active = false;
            if (y11_debug)
                fprintf(stderr, "y11: VT switch away (master dropped)\n");
        }
        (void)ioctl(s->tty_fd, VT_RELDISP, 1);
    }
    if (y11_vt_switch_back) {
        y11_vt_switch_back = 0;
        if (!s->active && s->drm_card_fd >= 0) {
            (void)ioctl(s->drm_card_fd, DRM_IOCTL_SET_MASTER, 0);
            y11_drm_mode_set_all();
            s->active = true;
            if (y11_debug)
                fprintf(stderr, "y11: VT switch back (master re-taken)\n");
        }
    }
}

void y11_session_shutdown(struct y11_session *s)
{
    /* Give the VT back to the kernel before tearing anything down. */
    if (s->tty_fd >= 0) {
        struct vt_mode vtm;

        memset(&vtm, 0, sizeof(vtm));
        vtm.mode = VT_AUTO;
        (void)ioctl(s->tty_fd, VT_SETMODE, &vtm);
        close(s->tty_fd);
        s->tty_fd = -1;
    }
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
