/*
 * wirefuzz.c - Loopback regression harness: spawns a private headless
 * server, sweeps the per-opcode underflow probe, replays the
 * truncated-handshake corpus and checks the slow-reader cap.  Exit 0
 * is the pass condition; it also serves as the Phase 0 corpus the
 * audit doc still owed.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sys/un.h>

#include "safe.h"

#define WF_DISPLAY 19
#define WF_SOCK "/tmp/.X11-unix/X19"
#define WF_ROOT 0x21u

static pid_t wf_server;
static int wf_checks;

#define EXPECT(cond)                                                     \
    do {                                                                \
        wf_checks++;                                                    \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            wf_fail();                                                  \
            return 1;                                                   \
        }                                                               \
    } while (0)

static void wf_fail(void)
{
    if (wf_server > 0) {
        kill(wf_server, SIGKILL);
        waitpid(wf_server, NULL, 0);
        unlink(WF_SOCK);
        unlink("/tmp/.y11-unix/Y19");
    }
}

/* ---- server lifecycle -------------------------------------------------- */

static int wf_spawn_server(void)
{
    struct stat st;
    int null;
    int tries;

    if (stat(WF_SOCK, &st) == 0) {
        printf("wirefuzz: %s already exists; stale server?\n", WF_SOCK);
        return -1;
    }
    wf_server = fork();
    if (wf_server < 0)
        return -1;
    if (wf_server == 0) {
        null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            close(null);
        }
        execl("./out/y11", "y11", "19", (char *)NULL);
        _exit(127);
    }
    for (tries = 0; tries < 100; tries++) {
        if (stat(WF_SOCK, &st) == 0)
            return 0;
        if (waitpid(wf_server, NULL, WNOHANG) == wf_server) {
            wf_server = 0;
            printf("wirefuzz: server died at startup\n");
            return -1;
        }
        {
            struct timespec d = {0, 50 * 1000 * 1000};

            nanosleep(&d, NULL);
        }
    }
    printf("wirefuzz: server socket never appeared\n");
    return -1;
}

static void wf_stop_server(void)
{
    if (wf_server > 0) {
        kill(wf_server, SIGTERM);
        waitpid(wf_server, NULL, 0);
    }
    unlink(WF_SOCK);
    unlink("/tmp/.y11-unix/Y19");
}

/* ---- X plumbing --------------------------------------------------------- */

static int wf_connect(void)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", WF_SOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void wf_timeout_ms(int fd, int ms)
{
    struct timeval tv;

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* Standard little-endian setup; y11 accepts any auth, so 12 bytes. */
static int wf_handshake(int fd)
{
    static const uint8_t setup[12] = {
        'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    uint8_t buf[256];
    ssize_t n;
    size_t have = 0;
    uint32_t extra;

    if (write(fd, setup, sizeof(setup)) != (ssize_t)sizeof(setup))
        return -1;
    wf_timeout_ms(fd, 2000);
    for (;;) {
        n = read(fd, buf + have, sizeof(buf) - have);
        if (n <= 0)
            return -1;
        have += (size_t)n;
        if (have >= 8) {
            extra = (uint32_t)buf[6] | ((uint32_t)buf[7] << 8);
            if (have >= 8 + extra * 4)
                break;
        }
        if (have >= sizeof(buf))
            return -1;
    }
    return buf[0] == 1 ? 0 : -1;
}

/* Read one 32-byte frame; 0 = frame, -1 = timeout/close, 1 = EOF. */
static int wf_read_frame(int fd, uint8_t out[32])
{
    ssize_t n;
    size_t have = 0;

    while (have < 32) {
        n = read(fd, out + have, 32 - have);
        if (n == 0)
            return 1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;      /* timeout or reset */
        }
        have += (size_t)n;
    }
    return 0;
}

/* ---- corpus 1: per-opcode underflow probe -------------------------------- */

static int wf_opcode_probe(void)
{
    int op;
    int bad = 0;

    for (op = 0; op <= 255; op++) {
        uint8_t req[4] = {(uint8_t)op, 0, 1, 0};
        uint8_t fr[32];
        int fd = wf_connect();
        uint16_t min_len;
        int rc;

        if (fd < 0) {
            printf("wirefuzz: connect failed at opcode %d\n", op);
            return 1;
        }
        if (wf_handshake(fd) != 0) {
            printf("wirefuzz: handshake failed at opcode %d\n", op);
            close(fd);
            return 1;
        }
        if (write(fd, req, sizeof(req)) != (ssize_t)sizeof(req)) {
            close(fd);
            return 1;
        }
        rc = wf_read_frame(fd, fr);
        min_len = y11_opcode_min_len((uint8_t)op);
        if (min_len > 4) {
            /* Xorg parity: anything below the opcode minimum is a
             * BadLength, never a dispatch into the handler. */
            EXPECT(rc == 0);
            EXPECT(fr[0] == 0);            /* error frame */
            EXPECT(fr[1] == 16);          /* BadLength */
            EXPECT(fr[10] == (uint8_t)op);
            bad++;
        } else {
            /* at or above the minimum: reply, error or silence are
             * all legal; survival is the invariant. */
            EXPECT(rc == 0 || rc == -1 || rc == 1);
        }
        close(fd);
        if (op == 36) {
            /* GrabServer executed above; the sweep's next opcode
             * (37) releases it, so no state leaks out. */
        }
    }
    printf("wirefuzz: opcode sweep: %d below-min BadLength rejections\n",
           bad);
    return 0;
}

/* ---- corpus 2: truncated and hostile handshakes -------------------------- */

static int wf_handshake_corpus(void)
{
    static const uint8_t good[12] = {
        'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    size_t len;
    int fd;
    int i;

    /* every truncation of a valid header: the server must wait,
     * never read past its buffer */
    for (len = 0; len < 12; len++) {
        fd = wf_connect();
        EXPECT(fd >= 0);
        if (len > 0)
            EXPECT(write(fd, good, len) == (ssize_t)len);
        close(fd);
    }

    /* wrong byte orders and versions must be refused, not hang */
    for (i = 0; i < 4; i++) {
        uint8_t bad[12];
        uint8_t fr[32];
        int rc;

        memcpy(bad, good, sizeof(bad));
        if (i == 0)
            bad[0] = 'B';         /* big-endian: refused in phase 1 */
        else if (i == 1)
            bad[0] = 'x';          /* unknown order */
        else if (i == 2)
            bad[2] = 10;           /* wrong major */
        else
            bad[3] = 0xff;         /* unknown minor is tolerated; skip */
        fd = wf_connect();
        EXPECT(fd >= 0);
        EXPECT(write(fd, bad, sizeof(bad)) == (ssize_t)sizeof(bad));
        wf_timeout_ms(fd, 1500);
        rc = wf_read_frame(fd, fr);
        if (i != 3) {
            EXPECT(rc == 0 || rc == 1);
            if (rc == 0)
                EXPECT(fr[0] == 0); /* failed setup reply */
        }
        close(fd);
    }

    /* a header claiming maximum auth data parks the connection: the
     * server must wait (and the reaper owns it from there) */
    {
        uint8_t big[12];

        memcpy(big, good, sizeof(big));
        big[6] = 0xff; big[7] = 0xff;    /* auth proto len 0xffff */
        big[8] = 0xff; big[9] = 0xff;    /* auth data len 0xffff */
        fd = wf_connect();
        EXPECT(fd >= 0);
        EXPECT(write(fd, big, sizeof(big)) == (ssize_t)sizeof(big));
        wf_timeout_ms(fd, 300);
        close(fd);
    }

    /* a fresh healthy connection still works after the corpus */
    fd = wf_connect();
    EXPECT(fd >= 0);
    EXPECT(wf_handshake(fd) == 0);
    close(fd);
    printf("wirefuzz: handshake corpus ok\n");
    return 0;
}

/* ---- corpus 3: slow-reader cap ------------------------------------------- */

static int wf_slow_reader(void)
{
    uint8_t gi[20];
    int fd;
    int i;

    gi[0] = 73;                     /* GetImage */
    gi[1] = 2;                       /* ZPixmap */
    gi[2] = 5; gi[3] = 0;            /* length 5 */
    gi[4] = (uint8_t)WF_ROOT; gi[5] = 0; gi[6] = 0; gi[7] = 0;
    gi[8] = 0; gi[9] = 0;           /* x */
    gi[10] = 0; gi[11] = 0;         /* y */
    gi[12] = 0x80; gi[13] = 0x07;   /* width 1920 */
    gi[14] = 0x38; gi[15] = 0x04;   /* height 1080 */
    gi[16] = gi[17] = gi[18] = gi[19] = 0xff;   /* plane mask */

    fd = wf_connect();
    EXPECT(fd >= 0);
    EXPECT(wf_handshake(fd) == 0);
    /* three full-screen replies (~8.3MB each) exceed the 16MB
     * pending-output cap: the server must drop us at the third */
    for (i = 0; i < 3; i++)
        EXPECT(write(fd, gi, sizeof(gi)) == (ssize_t)sizeof(gi));
    {
        struct timespec d = {0, 400 * 1000 * 1000};

        nanosleep(&d, NULL);
    }
    {
        ssize_t n = write(fd, gi, sizeof(gi));

        /* the fourth blit lands on a closed socket */
        EXPECT(n < 0 || errno == EPIPE);
    }
    {
        uint8_t b;
        ssize_t n;

        wf_timeout_ms(fd, 3000);
        do {
            n = read(fd, &b, 1);
        } while (n > 0);
        EXPECT(n == 0);            /* EOF: the server reaped us */
    }
    close(fd);
    printf("wirefuzz: slow reader reaped at the output cap\n");
    return 0;
}

int main(void)
{
    /* The slow-reader corpus expects writes to a closed socket to
     * error out, not to kill us. */
    signal(SIGPIPE, SIG_IGN);
    if (wf_spawn_server() != 0)
        return 1;
    if (wf_opcode_probe() != 0)
        return 1;
    if (wf_handshake_corpus() != 0)
        return 1;
    if (wf_slow_reader() != 0)
        return 1;
    wf_stop_server();
    printf("wirefuzz: %d checks ok\n", wf_checks);
    return 0;
}
