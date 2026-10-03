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
#include <unistd.h>

#include "y11.h"
#include "y11_wire.h"

int y11_debug;                  /* set from $Y11_DEBUG in main() */

static volatile sig_atomic_t y11_g_running = 1;

/* ---- signals --------------------------------------------------------------- */

static void y11_handle_signal(int signo)
{
    (void)signo;
    y11_g_running = 0;          /* poll(2) returns EINTR and the loop winds down */
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

    /* Writes to vanished clients must not kill the daemon. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) != 0)
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
    if (chmod(Y11_SOCKET_DIR, 0777) != 0)
        perror("y11: warning: chmod " Y11_SOCKET_DIR);
    if (chmod(Y11_LINK_DIR, 0777) != 0)
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
    struct pollfd fds[Y11_MAX_CLIENTS + 1];
    int map[Y11_MAX_CLIENTS + 1];   /* poll index -> client slot */

    while (y11_g_running) {
        nfds_t n = 1;
        int i, ready;

        fds[0].fd = srv->listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        map[0] = -1;

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
            if (errno == EINTR)
                continue;       /* signal: re-check y11_g_running */
            perror("y11: poll");
            break;
        }

        if ((fds[0].revents & POLLIN) != 0)
            y11_server_accept(srv);

        for (i = 1; i < (int)n; i++) {
            struct y11_client *c;
            short rev;

            if (fds[i].revents == 0)
                continue;
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
    }
}

/* ---- lifecycle ---------------------------------------------------------------- */

void y11_server_shutdown(struct y11_server *srv)
{
    int i;

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

int main(int argc, char **argv)
{
    struct y11_server srv;
    unsigned display = 0;

    if (argc > 1) {
        char *end;
        long val = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || *end != '\0' || val < 0 || val > 255) {
            fprintf(stderr, "usage: y11 [display-number]\n");
            return EXIT_FAILURE;
        }
        display = (unsigned)val;
    }

    y11_debug = getenv("Y11_DEBUG") != NULL;

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

    fprintf(stderr, "y11: listening on %s (%s -> %s)\n",
            srv.socket_path, srv.link_path, srv.socket_path);

    y11_server_run(&srv);

    y11_server_shutdown(&srv);
    y11_atom_shutdown();
    y11_resource_shutdown();
    return EXIT_SUCCESS;
}
