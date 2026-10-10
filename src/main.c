/*
 * main.c - Socket creation, signal traps and the non-blocking poll(2)
 * event loop for the Y11 display server daemon.
 *
 * The primary UNIX domain socket is bound at /tmp/.X11-unix/X0 and a
 * symbolic link /tmp/.y11-unix/Y0 -> /tmp/.X11-unix/X0 is maintained.
 * The daemon catches SIGINT/SIGTERM via sigaction and unlinks the
 * sockets after a clean shutdown.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "y11.h"
#include "y11_wire.h"
#include "y11_drm.h"

int y11_debug;                  /* set from $Y11_DEBUG in main() */

static volatile sig_atomic_t y11_g_running = 1;
static volatile sig_atomic_t y11_g_chld;

static struct y11_session y11_g_session;

/* Poll map sentinels: client slots are >= 0. */
#define Y11_MAP_SEAT  (-2)
#define Y11_MAP_DRM   (-3)
#define Y11_MAP_EVDEV (-4)

/* ---- signals --------------------------------------------------------------- */

static void y11_handle_signal(int signo)
{
    (void)signo;
    y11_g_running = 0;          /* poll(2) returns EINTR and the loop winds down */
}

static void y11_handle_chld(int signo)
{
    (void)signo;
    y11_g_chld = 1;
}

static int y11_install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = y11_handle_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: poll(2) must return EINTR so we re-check the flag. */
    if (sigaction(SIGINT, &sa, NULL) != 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = y11_handle_chld;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) != 0)
        return -1;

    /* Writes to vanished clients must not kill the daemon.
     * Background tty access must not stop the process. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) != 0)
        return -1;
    if (sigaction(SIGTTIN, &sa, NULL) != 0)
        return -1;
    if (sigaction(SIGTTOU, &sa, NULL) != 0)
        return -1;

    return 0;
}

/* ---- sockets ---------------------------------------------------------------- */

static int y11_prepare_paths(void)
{
    /* 0777 directory permissions so any user may reach the sockets. */
    if (mkdir(Y11_SOCKET_DIR, 0777) != 0 && errno != EEXIST) {
        perror("y11: mkdir " Y11_SOCKET_DIR);
        return -1;
    }
    if (mkdir(Y11_LINK_DIR, 0777) != 0 && errno != EEXIST) {
        perror("y11: mkdir " Y11_LINK_DIR);
        return -1;
    }
    if (chmod(Y11_SOCKET_DIR, 0777) != 0 && errno != EPERM)
        perror("y11: warning: chmod " Y11_SOCKET_DIR);
    if (chmod(Y11_LINK_DIR, 0777) != 0 && errno != EPERM)
        perror("y11: warning: chmod " Y11_LINK_DIR);
    return 0;
}

static int y11_setup_sockets(struct y11_server *srv)
{
    struct sockaddr_un addr;
    int fd;

    srv->listen_fd = -1;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("y11: socket");
        return -1;
    }

    /* Non-blocking I/O (fcntl keeps this portable to FreeBSD; SOCK_NONBLOCK
     * is not POSIX). */
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        perror("y11: fcntl");
        close(fd);
        return -1;
    }

    /* Unlink any stale socket before bind(2). */
    (void)unlink(srv->socket_path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(srv->socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "y11: socket path too long\n");
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, srv->socket_path, strlen(srv->socket_path) + 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("y11: bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 128) != 0) {
        perror("y11: listen");
        close(fd);
        return -1;
    }

    if (chmod(srv->socket_path, 0666) != 0)
        perror("y11: warning: chmod socket");

    /* Alias socket: /tmp/.y11-unix/Y<display> -> /tmp/.X11-unix/X<display> */
    (void)unlink(srv->link_path);
    if (symlink(srv->socket_path, srv->link_path) != 0)
        perror("y11: warning: symlink");

    srv->listen_fd = fd;
    return 0;
}

/* ---- event loop -------------------------------------------------------------- */

static void y11_server_accept(struct y11_server *srv)
{
    for (;;) {
        int fd = accept(srv->listen_fd, NULL, NULL);
        int i;

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            /* EAGAIN/EWOULDBLOCK: no more pending connections. */
            return;
        }

        if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
            perror("y11: fcntl");
            close(fd);
            continue;
        }

        for (i = 0; i < Y11_MAX_CLIENTS; i++) {
            if (srv->clients[i] == NULL)
                break;
        }
        if (i == Y11_MAX_CLIENTS) {
            fprintf(stderr, "y11: connection limit reached, refusing\n");
            close(fd);
            continue;
        }

        srv->clients[i] = y11_client_create(fd, i);
        if (srv->clients[i] == NULL) {
            fprintf(stderr, "y11: out of memory for client %d\n", i);
            close(fd);
        }
    }
}

void y11_server_run(struct y11_server *srv)
{
    struct pollfd fds[Y11_MAX_CLIENTS + 3 + Y11_MAX_EVDEV_DEVICES];
    int map[Y11_MAX_CLIENTS + 3 + Y11_MAX_EVDEV_DEVICES];   /* poll index -> client slot */

    while (y11_g_running) {
        nfds_t n = 1;
        int i, ready;

        if (srv->client_pid > 0) {
            int status = 0;
            pid_t p = waitpid(srv->client_pid, &status, WNOHANG);

            if (p > 0) {
                if (WIFEXITED(status))
                    fprintf(stderr, "y11: client exited with status %d\n",
                            WEXITSTATUS(status));
                else if (WIFSIGNALED(status))
                    fprintf(stderr, "y11: client terminated by signal %d\n",
                            WTERMSIG(status));
                srv->client_pid = -1;
                y11_g_running = 0;
                break;
            }
        }

        fds[0].fd = srv->listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        map[0] = -1;

        if (srv->seat_fd >= 0) {
            fds[n].fd = srv->seat_fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            map[n] = Y11_MAP_SEAT;
            n++;
        }
        if (srv->drm_fd >= 0) {
            fds[n].fd = srv->drm_fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            map[n] = Y11_MAP_DRM;
            n++;
        }

        for (i = 0; i < (int)y11_evdev_get_count(); i++) {
            int dev_fd = y11_evdev_get_fd((size_t)i);

            if (dev_fd >= 0) {
                fds[n].fd = dev_fd;
                fds[n].events = POLLIN;
                fds[n].revents = 0;
                map[n] = Y11_MAP_EVDEV;
                n++;
            }
        }

        for (i = 0; i < Y11_MAX_CLIENTS; i++) {
            struct y11_client *c = srv->clients[i];

            if (c == NULL)
                continue;
            fds[n].fd = c->fd;
            fds[n].events = (short)(POLLIN | (c->out_len > 0 ? POLLOUT : 0));
            fds[n].revents = 0;
            map[n] = i;
            n++;
        }

        ready = poll(fds, n, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                /* A signal (VT switch and friends) may only set flags
                 * inside libseat; drive the seat dispatch before
                 * repolling or the switch-back is never handled. */
                y11_session_dispatch(&y11_g_session);
                continue;       /* signal: re-check y11_g_running */
            }
            perror("y11: poll");
            break;
        }

        if ((fds[0].revents & POLLIN) != 0)
            y11_server_accept(srv);

        /* Drive VT events and session dispatch on every wakeup. */
        y11_session_dispatch(&y11_g_session);

        for (i = 1; i < (int)n; i++) {
            struct y11_client *c;
            short rev;

            if (fds[i].revents == 0)
                continue;

            /* Extra (non-client) poll entries. */
            if (map[i] == Y11_MAP_SEAT) {
                /* Seat events: enable/disable callbacks (VT switches). */
                y11_session_dispatch(&y11_g_session);
                continue;
            }
            if (map[i] == Y11_MAP_DRM) {
                /* Page flip completions arrive at VBlank. */
                y11_drm_handle_events(srv->drm_fd);
                continue;
            }
            if (map[i] == Y11_MAP_EVDEV) {
                /* Evdev input events (keyboard, mouse, touch). */
                y11_evdev_handle(&y11_g_session, fds[i].fd);
                continue;
            }

            c = srv->clients[map[i]];
            if (c == NULL)
                continue;
            rev = fds[i].revents;

            if ((rev & POLLIN) != 0) {
                if (y11_client_read(c) < 0)
                    c->dead = 1;
                else if (y11_client_process(c) < 0)
                    c->dead = 1;
            }
            if ((rev & POLLOUT) != 0) {
                if (y11_client_flush(c) < 0)
                    c->dead = 1;
            }
            if ((rev & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                c->dead = 1;
        }

        /* Deferred flush and reaping pass. */
        for (i = 0; i < Y11_MAX_CLIENTS; i++) {
            struct y11_client *c = srv->clients[i];

            if (c == NULL)
                continue;
            if (c->out_len > 0 && !c->dead) {
                if (y11_client_flush(c) < 0)
                    c->dead = 1;
            }
            if (c->dead || (c->wants_close && c->out_len == 0))
                y11_client_destroy(srv, c);
        }

        /* Present accumulated root damage at the next VBlank. */
        y11_scanout_flush();
    }
}

/* ---- lifecycle ---------------------------------------------------------------- */

void y11_server_shutdown(struct y11_server *srv)
{
    int i;

    if (srv->client_pid > 0) {
        int status = 0;

        if (waitpid(srv->client_pid, &status, WNOHANG) == 0) {
            int w;

            (void)kill(srv->client_pid, SIGTERM);
            for (w = 0; w < 10; w++) {
                struct timespec ts = { 0, 100000000L };

                if (waitpid(srv->client_pid, &status, WNOHANG) != 0)
                    break;
                (void)nanosleep(&ts, NULL);
            }
            if (waitpid(srv->client_pid, &status, WNOHANG) == 0)
                (void)kill(srv->client_pid, SIGKILL);
            (void)waitpid(srv->client_pid, &status, 0);
        }
        srv->client_pid = -1;
    }

    for (i = 0; i < Y11_MAX_CLIENTS; i++) {
        if (srv->clients[i] != NULL)
            y11_client_destroy(srv, srv->clients[i]);
    }
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    (void)unlink(srv->socket_path);
    (void)unlink(srv->link_path);
}

int y11_server_init(struct y11_server *srv, unsigned display)
{
    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = -1;
    srv->seat_fd = -1;
    srv->drm_fd = -1;
    srv->client_pid = -1;
    y11_client_bind_server(srv);

    if (snprintf(srv->socket_path, sizeof(srv->socket_path),
                 "%s/X%u", Y11_SOCKET_DIR, display) >=
        (int)sizeof(srv->socket_path))
        return -1;
    if (snprintf(srv->link_path, sizeof(srv->link_path),
                 "%s/Y%u", Y11_LINK_DIR, display) >=
        (int)sizeof(srv->link_path))
        return -1;

    if (y11_install_signals() != 0)
        return -1;
    if (y11_prepare_paths() != 0)
        return -1;
    if (y11_setup_sockets(srv) != 0)
        return -1;
    return 0;
}

/* Physical millimeters for a standard 96 DPI screen of px pixels. */
static uint16_t y11_dpi96_mm(uint32_t px)
{
    return (uint16_t)((px * 254u + 479u) / 960u);
}

/* mkdir -p equivalent so --log-dir works without setup. */
static void y11_mkdir_p(const char *path)
{
    char buf[512];
    size_t i, len;

    if (path == NULL || path[0] == '\0')
        return;
    len = strlen(path);
    if (len >= sizeof(buf))
        return;
    memcpy(buf, path, len + 1);
    for (i = 1; i < len; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        (void)mkdir(buf, 0755);
        buf[i] = '/';
    }
    (void)mkdir(buf, 0755);
}

int main(int argc, char **argv)
{
    struct y11_server srv;
    unsigned display = 0;
    int client_arg_idx = -1;
    int debug_flag = 0;
    const char *log_dir = NULL;
    int i;

    /*
     * Flags: -d/--debug enable verbose logging, --log-dir DIR captures
     * all server output (startup, errors, verbose debug) into
     * DIR/y11-<display>.log.  Flags are removed from argv so the
     * client command stays intact wherever they appear.
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            debug_flag = 1;
            argv[i] = NULL;
        } else if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            log_dir = argv[i + 1];
            argv[i] = NULL;
            argv[i + 1] = NULL;
            i++;
        }
    }
    {
        int w = 1;

        for (i = 1; i < argc; i++) {
            if (argv[i] != NULL)
                argv[w++] = argv[i];
        }
        for (i = w; i < argc; i++)
            argv[i] = NULL;         /* stale slots: keep exec argv sane */
        argc = w;
    }

    if (argc > 1) {
        int idx = 1;
        const char *arg = argv[idx];
        const char *num_str = arg;
        char *end = NULL;
        long val;

        if (num_str[0] == ':')
            num_str++;
        val = strtol(num_str, &end, 10);
        if (*num_str != '\0' && *end == '\0' && val >= 0 && val <= 255) {
            display = (unsigned)val;
            idx++;
        }
        if (idx < argc)
            client_arg_idx = idx;
    }

    y11_debug = debug_flag || getenv("Y11_DEBUG") != NULL;

    /*
     * --log-dir DIR: point stderr at DIR/y11-<display>.log so the whole
     * run (startup, errors, verbose debug, and the spawned client's
     * stderr) is captured for diagnosis after the fact.  Truncated per
     * run so repeated tests read cleanly.
     */
    if (log_dir != NULL) {
        char log_path[512];
        FILE *lf;

        y11_mkdir_p(log_dir);
        (void)snprintf(log_path, sizeof(log_path), "%s/y11-%u.log",
                       log_dir, display);
        lf = fopen(log_path, "w");
        if (lf != NULL) {
            (void)dup2(fileno(lf), STDERR_FILENO);
            (void)fclose(lf);
        } else {
            fprintf(stderr, "y11: cannot open log file %s: %s\n",
                    log_path, strerror(errno));
        }
    }

    if (y11_server_init(&srv, display) != 0)
        return EXIT_FAILURE;

    if (y11_resource_init() != 0) {
        fprintf(stderr, "y11: cannot initialize resource table\n");
        y11_server_shutdown(&srv);
        return EXIT_FAILURE;
    }

    if (y11_atom_init() != 0) {
        fprintf(stderr, "y11: cannot initialize atom table\n");
        y11_server_shutdown(&srv);
        return EXIT_FAILURE;
    }

    /*
     * Hardware path: acquire the seat through libseat, discover KMS
     * outputs, size the root window from the output mode, then put
     * scanout and the cursor on the CRTC.  Any failure along the way
     * (no seat manager, another display server holds DRM master)
     * falls back to the headless software screen.
     */
    if (y11_session_init(&y11_g_session) == 0) {
        srv.seat_fd = y11_g_session.seat_fd;
        if (y11_drm_init(&y11_g_session) == 0) {
            struct y11_output *out = y11_drm_outputs();

            y11_screen_width = (uint16_t)out->mode.hdisplay;
            y11_screen_height = (uint16_t)out->mode.vdisplay;
            srv.drm_fd = y11_g_session.drm_card_fd;
            /*
             * The connector carries the EDID-derived physical size;
             * clients use it (the connection setup's width_in_mm) to
             * compute the real DPI.  Virtual outputs often report 0,
             * in which case the 96 DPI fallback below fills it in.
             */
            if (out->mm_width > 0 && out->mm_width <= 0xffffu)
                y11_screen_width_mm = (uint16_t)out->mm_width;
            if (out->mm_height > 0 && out->mm_height <= 0xffffu)
                y11_screen_height_mm = (uint16_t)out->mm_height;
        }
    }

    /*
     * Without a connector size (headless screen, virtual output with
     * no EDID), report dimensions matching a standard 96 DPI display
     * so clients' DPI math lands on the usual 96 instead of nonsense.
     */
    if (y11_screen_width_mm == 0)
        y11_screen_width_mm = y11_dpi96_mm(y11_screen_width);
    if (y11_screen_height_mm == 0)
        y11_screen_height_mm = y11_dpi96_mm(y11_screen_height);

    if (y11_window_init() != 0) {
        fprintf(stderr, "y11: cannot create the root window\n");
        y11_server_shutdown(&srv);
        y11_atom_shutdown();
        return EXIT_FAILURE;
    }

    /* Publish the EWMH root properties on the freshly created root. */
    y11_atom_publish_root_properties(y11_window_get(Y11_SCREEN_ROOT));

    if (y11_input_init() != 0) {
        fprintf(stderr, "y11: cannot initialize the input subsystem\n");
        y11_server_shutdown(&srv);
        y11_window_shutdown();
        y11_atom_shutdown();
        return EXIT_FAILURE;
    }
    y11_evdev_init(&y11_g_session);

    /*
     * DRI3 render-node access: prefer the session card fd, fall back
     * to opening a render node directly (unprivileged).
     */
    (void)y11_dri3_init(srv.drm_fd);
    (void)y11_present_init();

    /*
     * Hardware scanout comes up after the root window exists: it needs
     * the root backbuffer as the composition source and DRM master
     * from the seat.  Failing here just means the software screen.
     */
    if (srv.drm_fd >= 0) {
        struct y11_window *root = y11_window_get(Y11_SCREEN_ROOT);

        if (root != NULL &&
            y11_scanout_init(&root->drawable) != 0) {
            fprintf(stderr, "y11: hardware scanout unavailable (%s),"
                    " running headless\n", strerror(errno));
            srv.drm_fd = -1;
            y11_drm_shutdown();
        }
    }

    fprintf(stderr, "y11: listening on %s (%s -> %s)\n",
            srv.socket_path, srv.link_path, srv.socket_path);

    if (client_arg_idx > 0) {
        pid_t pid = fork();

        if (pid < 0) {
            fprintf(stderr, "y11: failed to fork client: %s\n",
                    strerror(errno));
        } else if (pid == 0) {
            char disp_str[16];

            (void)snprintf(disp_str, sizeof(disp_str), ":%u", display);
            (void)setenv("DISPLAY", disp_str, 1);

            (void)signal(SIGINT, SIG_DFL);
            (void)signal(SIGTERM, SIG_DFL);
            (void)signal(SIGPIPE, SIG_DFL);

            execvp(argv[client_arg_idx], &argv[client_arg_idx]);
            if (errno == ENOENT && strchr(argv[client_arg_idx], '/') == NULL) {
                const char *home = getenv("HOME");
                if (home != NULL) {
                    char local_path[1024];
                    if (snprintf(local_path, sizeof(local_path), "%s/.local/bin/%s",
                                 home, argv[client_arg_idx]) < (int)sizeof(local_path) &&
                        access(local_path, X_OK) == 0) {
                        argv[client_arg_idx] = local_path;
                        execv(local_path, &argv[client_arg_idx]);
                    }
                }
            }
            fprintf(stderr, "y11: execvp \"%s\" failed: %s\n",
                    argv[client_arg_idx], strerror(errno));
            _exit(127);
        } else {
            fprintf(stderr, "y11: spawned client \"%s\" (pid %d)\n",
                    argv[client_arg_idx], (int)pid);
            srv.client_pid = pid;
        }
    }

    y11_server_run(&srv);

    y11_evdev_shutdown(&y11_g_session);
    y11_scanout_shutdown();
    y11_drm_shutdown();
    y11_session_shutdown(&y11_g_session);
    y11_present_shutdown();
    y11_dri3_shutdown();

    y11_server_shutdown(&srv);
    y11_input_shutdown();
    y11_window_shutdown();
    y11_atom_shutdown();
    y11_resource_shutdown();
    return EXIT_SUCCESS;
}
