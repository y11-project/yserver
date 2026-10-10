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
 *
 * Copyright (c) 2026 The Y11 Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/vt.h>
#include <drm/drm.h>
#include <drm_mode.h>

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

static struct y11_session *y11_active_session;

static void y11_session_enable(struct libseat *seat, void *userdata)
{
    struct y11_session *s = userdata;

    (void)seat;
    /*
     * Keys held across the switch never see their releases.
     */
    y11_input_reset_keys();
    y11_grab_reset_keyboard();
    s->active = true;
    if (y11_debug)
        fprintf(stderr, "y11: seat enabled (session active)\n");

    /* Re-acquire DRM master if it was dropped, repaint and rescan. */
    if (s->drm_card_fd >= 0 && !drmIsMaster(s->drm_card_fd))
        (void)drmSetMaster(s->drm_card_fd);
    y11_scanout_restore();
    y11_evdev_rescan(s);
}

static void y11_session_disable(struct libseat *seat, void *userdata)
{
    struct y11_session *s = userdata;

    (void)seat;
    /* Keys held across the switch never see their releases. */
    y11_input_reset_keys();
    y11_grab_reset_keyboard();
    s->active = false;
    if (y11_debug)
        fprintf(stderr, "y11: seat disabled (VT switch away)\n");

    /* Hand the display back to the console so the switch shows it. */
    y11_drm_restore_console();

    /* Drop master so the next VT can program the hardware. */
    if (s->drm_card_fd >= 0 && drmIsMaster(s->drm_card_fd))
        (void)drmDropMaster(s->drm_card_fd);

    /*
     * The disable ack must go out AFTER the dispatch returns: calling
     * back into libseat from inside its own callback nests reads on
     * the seat connection, the ack never completes, and the seat is
     * then unable to re-enable the client on switch-back.
     */
    s->pending_ack = true;
}

static const struct libseat_seat_listener y11_seat_listener = {
    .enable_seat = y11_session_enable,
    .disable_seat = y11_session_disable
};

/* Score a card's connected outputs to pick the primary display. */
static int y11_card_score(int fd)
{
    drmModeRes *res;
    int best_score = -1;
    int i;

    res = drmModeGetResources(fd);
    if (res == NULL)
        return -1;

    for (i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);

        if (conn == NULL)
            continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            int score = 1000;

            switch (conn->connector_type) {
            case DRM_MODE_CONNECTOR_eDP:
            case DRM_MODE_CONNECTOR_LVDS:
            case DRM_MODE_CONNECTOR_DSI:
                score += 100000;
                break;
            case DRM_MODE_CONNECTOR_DisplayPort:
            case DRM_MODE_CONNECTOR_HDMIA:
            case DRM_MODE_CONNECTOR_HDMIB:
            case DRM_MODE_CONNECTOR_DVII:
            case DRM_MODE_CONNECTOR_DVID:
            case DRM_MODE_CONNECTOR_VGA:
                score += 50000;
                break;
            case DRM_MODE_CONNECTOR_VIRTUAL:
                score += 10000;
                break;
            case DRM_MODE_CONNECTOR_USB:
            case DRM_MODE_CONNECTOR_SPI:
                score += 100;
                break;
            default:
                score += 500;
                break;
            }

            if (conn->modes[0].hdisplay > 0 && conn->modes[0].vdisplay > 0) {
                /* Strip/touchbar screens (< 200px) are heavily penalized */
                if (conn->modes[0].hdisplay < 200 || conn->modes[0].vdisplay < 200)
                    score -= 5000;
                score += (int)((uint32_t)conn->modes[0].hdisplay *
                               (uint32_t)conn->modes[0].vdisplay / 10000u);
            }

            if (score > best_score)
                best_score = score;
        }
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    return best_score;
}

/* Detect current VT number across environment, tty and sysfs. */
static int y11_get_vt_num(int tty_fd)
{
    const char *vtnr = getenv("XDG_VTNR");
    const char *name;
    struct stat st;
    int fd;

    if (vtnr != NULL && *vtnr != '\0') {
        int v = atoi(vtnr);
        if (v > 0)
            return v;
    }
    name = ttyname(tty_fd);
    if (name == NULL)
        name = ttyname(STDIN_FILENO);
    if (name != NULL && strncmp(name, "/dev/tty", 8) == 0 &&
        name[8] >= '0' && name[8] <= '9') {
        int v = atoi(name + 8);
        if (v > 0)
            return v;
    }
    if (fstat(tty_fd, &st) == 0 && major(st.st_rdev) == 4) {
        int v = (int)minor(st.st_rdev);
        if (v > 0)
            return v;
    }
    fd = open("/sys/class/tty/tty0/active", O_RDONLY);
    if (fd >= 0) {
        char buf[32];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 3 && strncmp(buf, "tty", 3) == 0 &&
            buf[3] >= '0' && buf[3] <= '9') {
            int v = atoi(buf + 3);
            if (v > 0)
                return v;
        }
    }
    return -1;
}

/* Probe card nodes and pick the best one with connected outputs. */
static int y11_session_open_card(struct y11_session *s, int *dev_id)
{
    static const char *const paths[] = {
        "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2",
        "/dev/dri/card3", "/dev/dri/card4", NULL
    };
    int best_fd = -1;
    int best_id = -1;
    int best_score = -1;
    int i;

    for (i = 0; paths[i] != NULL; i++) {
        int card_fd = -1;
        int id = libseat_open_device(s->seat, paths[i], &card_fd);

        if (id < 0 || card_fd < 0) {
            if (y11_debug)
                fprintf(stderr, "y11: seat open %s: %s\n", paths[i],
                        strerror(errno));
            continue;
        }

        int score = y11_card_score(card_fd);
        if (y11_debug)
            fprintf(stderr, "y11: evaluated %s (score %d)\n", paths[i], score);

        if (score > best_score) {
            if (best_id >= 0)
                (void)libseat_close_device(s->seat, best_id);
            best_fd = card_fd;
            best_id = id;
            best_score = score;
        } else {
            (void)libseat_close_device(s->seat, id);
        }
    }

    if (best_fd >= 0) {
        *dev_id = best_id;
        s->drm_card_fd = best_fd;
        return best_fd;
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
        "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2",
        "/dev/dri/card3", "/dev/dri/card4", NULL
    };
    struct vt_mode vtm;
    int best_fd = -1;
    int best_score = -1;
    int i, vt;

    for (i = 0; paths[i] != NULL; i++) {
        int fd = open(paths[i], O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) != 0) {
            if (y11_debug)
                fprintf(stderr, "y11: direct %s: %s\n", paths[i],
                        strerror(errno));
            close(fd);
            continue;
        }
        int score = y11_card_score(fd);
        if (y11_debug)
            fprintf(stderr, "y11: direct evaluated %s (score %d)\n",
                    paths[i], score);

        if (score > best_score) {
            if (best_fd >= 0) {
                (void)ioctl(best_fd, DRM_IOCTL_DROP_MASTER, 0);
                close(best_fd);
            }
            best_fd = fd;
            best_score = score;
        } else {
            (void)ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
            close(fd);
        }
    }
    if (best_fd < 0)
        return -1;

    s->drm_card_fd = best_fd;
    s->drm_device_id = -1;      /* no libseat device */
    s->active = true;
    y11_drm_set_tty(s->tty_fd);

    /* Own the VT switches; the kernel signals us to release/accept. */
    memset(&vtm, 0, sizeof(vtm));
    vtm.mode = VT_PROCESS;
    vtm.relsig = SIGUSR1;
    vtm.acqsig = SIGUSR2;
    (void)ioctl(s->tty_fd, VT_SETMODE, &vtm);
    {
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = y11_vt_signal;
        sigemptyset(&sa.sa_mask);
        /* No SA_RESTART: poll(2) must return EINTR to handle VT switches. */
        sa.sa_flags = 0;
        (void)sigaction(SIGUSR1, &sa, NULL);
        (void)sigaction(SIGUSR2, &sa, NULL);
    }

    /* Claim the VT: activate it so the display and the keyboard land
     * here instead of leaving the user typing into a blind shell. */
    vt = y11_get_vt_num(s->tty_fd);
    if (vt > 0 && ioctl(s->tty_fd, VT_ACTIVATE, vt) == 0)
        (void)ioctl(s->tty_fd, VT_WAITACTIVE, vt);

    return best_fd;
}

int y11_session_init(struct y11_session *s)
{
    int vt;

    memset(s, 0, sizeof(*s));
    s->drm_card_fd = -1;
    s->drm_device_id = -1;
    s->tty_fd = open("/dev/tty", O_RDWR);

    /*
     * A display needs VT coordination: without a controlling terminal
     * (daemons, remote shells) there is no way to hand the screen
     * back on a VT switch, which would trap whoever sits in front of
     * it.  Refuse instead.
     */
    if (s->tty_fd < 0) {
        s->seat = libseat_open_seat(&y11_seat_listener, s);
        if (s->seat != NULL) {
            libseat_close_seat(s->seat);
            s->seat = NULL;
        }
        fprintf(stderr, "y11: no controlling terminal, refusing the"
                " display (run from a TTY session)\n");
        return -1;
    }

    /*
     * Activate the controlling VT BEFORE the seat binds: the seat
     * session is assigned for the VT that is active right now, and it
     * has to be the tty y11 owns or the seat immediately disables the
     * session again (active VT elsewhere) and the display never takes.
     */
    vt = y11_get_vt_num(s->tty_fd);
    if (vt > 0) {
        (void)ioctl(s->tty_fd, VT_ACTIVATE, vt);
        (void)ioctl(s->tty_fd, VT_WAITACTIVE, vt);
    }

    s->seat = libseat_open_seat(&y11_seat_listener, s);
    if (s->seat == NULL) {
        if (y11_session_direct(s) >= 0) {
            fprintf(stderr, "y11: no seat manager; took the console"
                    " with VT_PROCESS switching\n");
            y11_active_session = s;
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
    s->active = true;
    y11_active_session = s;
    return 0;
}

void y11_session_dispatch(struct y11_session *s)
{
    if (s->seat != NULL) {
        (void)libseat_dispatch(s->seat, 0);
        /* Acknowledge a switch-away once the dispatch is done. */
        if (s->pending_ack) {
            s->pending_ack = false;
            (void)libseat_disable_seat(s->seat);
        }
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
            y11_scanout_restore();
            s->active = true;
            if (y11_debug)
                fprintf(stderr, "y11: VT switch back (master re-taken)\n");
        }
        (void)ioctl(s->tty_fd, VT_RELDISP, VT_ACKACQ);
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
        (void)signal(SIGUSR1, SIG_DFL);
        (void)signal(SIGUSR2, SIG_DFL);
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
    if (y11_active_session == s)
        y11_active_session = NULL;
}

int y11_session_switch_vt(struct y11_session *s, int vt)
{
    if (s == NULL || vt <= 0)
        return -1;
    if (y11_debug)
        fprintf(stderr, "y11: requesting switch to VT %d\n", vt);
    if (s->seat != NULL)
        return libseat_switch_session(s->seat, vt);
    if (s->tty_fd >= 0)
        return ioctl(s->tty_fd, VT_ACTIVATE, vt);
    return -1;
}

int y11_session_request_vt_switch(int vt)
{
    if (y11_active_session != NULL)
        return y11_session_switch_vt(y11_active_session, vt);
    return -1;
}

bool y11_session_is_active(void)
{
    return y11_active_session == NULL || y11_active_session->active;
}
