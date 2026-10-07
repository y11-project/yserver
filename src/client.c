/*
 * client.c - Client connection management for the Y11 display server.
 *
 * Handles client allocation, the per-client request reassembly ("ring")
 * buffer, and the X11 connection setup handshake.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "y11.h"
#include "y11_wire.h"

/* ---- allocation ---------------------------------------------------------- */

struct y11_client *y11_client_create(int fd, int slot)
{
    struct y11_client *c;

    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;

    c->fd = fd;
    c->slot = slot;
    c->state = Y11_CLIENT_HANDSHAKE;
    c->sequence_number = 0;
    c->resource_id_base = (yid_t)(slot + 1) * Y11_RID_BASE_STEP;   /* 0x00100000, 0x00200000, ... */

    c->in_cap = Y11_INBUF_MIN;
    c->in_buf = malloc(c->in_cap);
    if (c->in_buf == NULL) {
        free(c);
        return NULL;
    }

    c->out_cap = Y11_OUTBUF_MIN;
    c->out_buf = malloc(c->out_cap);
    if (c->out_buf == NULL) {
        free(c->in_buf);
        free(c);
        return NULL;
    }
    c->out_fd_pending = -1;
    c->out_fd_offset = 0;

    return c;
}

void y11_client_destroy(struct y11_server *srv, struct y11_client *c)
{
    size_t i;

    if (y11_debug)
        fprintf(stderr, "y11: client %d: disconnect\n", c->slot);
    /* Release window event subscriptions and redirect ownership. */
    y11_events_purge_client(c);
    /* Per the X11 protocol, a disconnect destroys the client's windows. */
    y11_window_destroy_owned(c);
    /* And its pixmaps and graphics contexts. */
    y11_pixmap_purge_client(c);
    y11_gc_purge_client(c);
    /* And its shared memory segments. */
    y11_shm_purge_client(c);
    /* And its colormaps. */
    y11_colormap_purge_client(c);
    /* And its fonts and cursors. */
    y11_font_purge_client(c);
    /* And its GLX contexts and input grabs. */
    y11_glx_purge_client(c);
    y11_grab_purge_client(c);
    /* And its DRI3 shared-memory fences. */
    y11_dri3_fence_purge_client(c);
    /* And its RENDER pictures and glyph sets. */
    y11_render_purge_client(c);
    /* And its Present event selections. */
    y11_present_purge_client(c);
    /* And its selection ownerships. */
    y11_selection_purge_client(c);
    /* And any undelivered ancillary descriptors. */
    for (i = 0; i < c->in_fd_count; i++)
        close(c->in_fds[i]);
    c->in_fd_count = 0;
    if (c->out_fd_pending >= 0) {
        close(c->out_fd_pending);
        c->out_fd_pending = -1;
    }
    if (c->fd >= 0)
        close(c->fd);
    free(c->in_buf);
    free(c->out_buf);
    srv->clients[c->slot] = NULL;
    free(c);
}

/* ---- input ("ring") buffer ------------------------------------------------ */

static int y11_client_grow_inbuf(struct y11_client *c)
{
    size_t cap = c->in_cap * 2;
    uint8_t *buf;

    if (cap > Y11_INBUF_MAX) {
        if (c->in_cap >= Y11_INBUF_MAX)
            return 0;           /* cannot grow any further */
        cap = Y11_INBUF_MAX;
    }

    buf = realloc(c->in_buf, cap);
    if (buf == NULL)
        return -1;

    c->in_buf = buf;
    c->in_cap = cap;
    return 1;
}

/*
 * Read everything currently available from the socket into the client's
 * reassembly buffer, harvesting any ancillary file descriptors passed
 * with SCM_RIGHTS.  Returns 0 on success (including clean EOF, which
 * marks the client dead), -1 on a fatal read error.
 */
int y11_client_read(struct y11_client *c)
{
    for (;;) {
        ssize_t n;
        char control[CMSG_SPACE(sizeof(int) * 8)];
        struct msghdr msg;
        struct iovec iov;
        struct cmsghdr *cmsg;

        if (c->in_len == c->in_cap) {
            int grown = y11_client_grow_inbuf(c);
            if (grown < 0)
                return -1;
            if (grown == 0)
                return 0;       /* buffer at maximum: parse what we have */
        }

        memset(&msg, 0, sizeof(msg));
        iov.iov_base = c->in_buf + c->in_len;
        iov.iov_len = c->in_cap - c->in_len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        n = recvmsg(c->fd, &msg, 0);
        if (n > 0) {
            c->in_len += (size_t)n;
            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
                 cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET &&
                    cmsg->cmsg_type == SCM_RIGHTS) {
                    size_t nfd = (cmsg->cmsg_len - CMSG_LEN(0)) /
                                 sizeof(int);
                    size_t i;

                    for (i = 0; i < nfd; i++) {
                        if (c->in_fd_count <
                            sizeof(c->in_fds) / sizeof(c->in_fds[0])) {
                            c->in_fds[c->in_fd_count++] =
                                ((int *)CMSG_DATA(cmsg))[i];
                        } else {
                            close(((int *)CMSG_DATA(cmsg))[i]);
                        }
                    }
                }
            }
            continue;
        }
        if (n == 0) {
            c->dead = 1;        /* orderly shutdown by peer */
            return 0;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

/*
 * Queue bytes for transmission, optionally carrying one ancillary file
 * descriptor (SCM_RIGHTS) attached at the start of this chunk.  The
 * event loop flushes pending output after each parse pass; POLLOUT
 * resumes when the client catches up.
 */
int y11_client_send_fd(struct y11_client *c, const void *data, size_t len,
                       int fd)
{
    /* A previous fd still pending: flush past it before queueing. */
    if (c->out_fd_pending >= 0) {
        if (y11_client_flush(c) < 0)
            return -1;
        if (c->out_fd_pending >= 0) {
            /* Would block with an fd still attached: fail hard. */
            return -1;
        }
    }

    if (fd >= 0) {
        c->out_fd_pending = fd;
        c->out_fd_offset = c->out_len;
    }
    return y11_client_send(c, data, len);
}

/*
 * Write all pending output, attaching the queued file descriptor with
 * sendmsg when the reply it belongs to goes out.  Returns 1 when fully
 * flushed, 0 when the client is not ready to accept more (POLLOUT will
 * resume), -1 on error.
 */
int y11_client_flush(struct y11_client *c)
{
    while (c->out_len > 0) {
        char control[CMSG_SPACE(sizeof(int))];
        struct msghdr msg;
        struct iovec iov;
        struct cmsghdr *cmsg;
        int attach_fd = -1;
        ssize_t n;

        if (c->out_fd_pending >= 0 &&
            c->out_fd_offset < c->out_len) {
            attach_fd = c->out_fd_pending;
        }

        memset(&msg, 0, sizeof(msg));
        iov.iov_base = c->out_buf;
        iov.iov_len = c->out_len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (attach_fd >= 0) {
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &attach_fd, sizeof(int));
        }

        n = sendmsg(c->fd, &msg, MSG_NOSIGNAL);
        if (n > 0) {
            size_t sent = (size_t)n;

            if (attach_fd >= 0) {
                close(c->out_fd_pending);
                c->out_fd_pending = -1;
                c->out_fd_offset = 0;
            } else if (c->out_fd_pending >= 0) {
                c->out_fd_offset -= sent;
            }
            memmove(c->out_buf, c->out_buf + sent, c->out_len - sent);
            c->out_len -= sent;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    }
    return 1;
}

/*
 * Pop the oldest received ancillary descriptor, or -1 when the client
 * sent none.  Used by DRI3 requests that carry a DMA-BUF.
 */
int y11_client_pop_fd(struct y11_client *c)
{
    int fd;
    size_t i;

    if (c->in_fd_count == 0)
        return -1;
    fd = c->in_fds[0];
    for (i = 1; i < c->in_fd_count; i++)
        c->in_fds[i - 1] = c->in_fds[i];
    c->in_fd_count--;
    return fd;
}

/* ---- output buffer --------------------------------------------------------- */

/*
 * Queue bytes for transmission.  The event loop flushes pending output
 * after each parse pass; POLLOUT resumes when the client catches up.
 */
int y11_client_send(struct y11_client *c, const void *data, size_t len)
{
    if (c->out_len + len > c->out_cap) {
        size_t cap = c->out_cap;
        uint8_t *buf;

        while (cap < c->out_len + len)
            cap *= 2;
        buf = realloc(c->out_buf, cap);
        if (buf == NULL)
            return -1;
        c->out_buf = buf;
        c->out_cap = cap;
    }

    memcpy(c->out_buf + c->out_len, data, len);
    c->out_len += len;
    return 0;
}

/* ---- connection setup handshake -------------------------------------------- */

/*
 * Send the failed connection setup reply and mark the client for closing:
 *
 *   1   0 (failed)
 *   1   unused
 *   2   length of reason in bytes
 *   2   unused
 *   n   reason string
 */
static int y11_client_refuse(struct y11_client *c, size_t *off,
                             const char *reason)
{
    uint8_t hdr[6];
    size_t rlen = strlen(reason);

    *off = c->in_len;           /* discard the rejected setup request */

    hdr[0] = 0;                 /* status: failed */
    hdr[1] = 0;
    y11_wire_put16(hdr + 2, (uint16_t)rlen);
    hdr[4] = 0;
    hdr[5] = 0;

    if (y11_client_send(c, hdr, sizeof(hdr)) != 0)
        return -1;
    if (rlen > 0 && y11_client_send(c, reason, rlen) != 0)
        return -1;

    c->wants_close = 1;
    return 0;
}

/*
 * Send the full successful connection setup reply:
 *
 *   prefix        8 bytes   success, protocol version, additional length
 *   fixed info   32 bytes   release, resource ids, vendor, formats, limits
 *   vendor       16 bytes   "The Y11 Project" padded to a 4-byte boundary
 *   formats      24 bytes   1/24/32-bit pixmap formats
 *   screen       40 bytes   root window, colormap, geometry, visuals
 *   depths       40 bytes   1, 4, 8, 24 (with the visual), 32
 *   visual       24 bytes   TrueColor visual
 *
 *   Total: 8 + 32 + 16 + 24 + 40 + 40 + 24 = 176 bytes.
 */
static int y11_client_send_setup_success(struct y11_client *c)
{
    y11_conn_setup_prefix prefix;
    y11_conn_setup_info info;
    y11_pixmap_format formats[3];
    y11_screen_info screen;
    y11_visual_type visual;
    static const char vendor[] = Y11_VENDOR_STRING;
    static const uint8_t vendor_pad[4] = { 0, 0, 0, 0 };
    size_t vlen = sizeof(vendor) - 1;               /* 15 */
    uint32_t vend_padded = y11_wire_pad4((uint32_t)vlen);   /* 16 */
    uint32_t additional;

    /* 8-byte setup prefix: five depth entries (1/4/8/24/32) with the
     * TrueColor visual under depth 24. */
    additional = 32u + vend_padded + (uint32_t)(sizeof(formats)) +
                 (uint32_t)(sizeof(screen)) +
                 5u * (uint32_t)(sizeof(y11_depth_info)) +
                 (uint32_t)(sizeof(visual));

    memset(&prefix, 0, sizeof(prefix));
    prefix.success = 1;
    y11_wire_put16(&prefix.major_version, Y11_PROTO_MAJOR);
    y11_wire_put16(&prefix.minor_version, Y11_PROTO_MINOR);
    y11_wire_put16(&prefix.length, (uint16_t)(additional / 4u));

    /* 32-byte fixed setup info */
    memset(&info, 0, sizeof(info));
    y11_wire_put32(&info.release_number, Y11_RELEASE_NUMBER);
    y11_wire_put32(&info.resource_id_base, c->resource_id_base);
    y11_wire_put32(&info.resource_id_mask, Y11_RID_MASK);
    y11_wire_put32(&info.motion_buffer_size, 256u);
    y11_wire_put16(&info.vendor_len, (uint16_t)vlen);
    y11_wire_put16(&info.max_request_size, (uint16_t)Y11_MAX_REQUEST_UNITS);
    info.num_screens = 1;
    info.num_formats = 3;
    info.image_byte_order = 0;          /* LSBFirst */
    info.bitmap_bit_order = 0;          /* Least Significant first */
    info.bitmap_scanline_unit = 32;
    info.bitmap_scanline_pad = 32;
    info.min_keycode = 8;
    info.max_keycode = 255;

    /* 16-byte format list */
    memset(formats, 0, sizeof(formats));
    formats[0].depth = 1;
    formats[0].bits_per_pixel = 1;
    formats[0].scanline_pad = 32;
    formats[1].depth = 24;
    formats[1].bits_per_pixel = 32;
    formats[1].scanline_pad = 32;
    formats[2].depth = 32;
    formats[2].bits_per_pixel = 32;
    formats[2].scanline_pad = 32;

    /* 40-byte screen information */
    memset(&screen, 0, sizeof(screen));
    y11_wire_put32(&screen.root_window, Y11_SCREEN_ROOT);
    y11_wire_put32(&screen.default_colormap, Y11_SCREEN_COLORMAP);
    y11_wire_put32(&screen.white_pixel, 0x00FFFFFFu);
    y11_wire_put32(&screen.black_pixel, 0x00000000u);
    y11_wire_put32(&screen.current_input_mask, 0u);
    y11_wire_put16(&screen.width_in_pixels, y11_screen_width);
    y11_wire_put16(&screen.height_in_pixels, y11_screen_height);
    y11_wire_put16(&screen.width_in_mm, (uint16_t)Y11_SCREEN_MM_WIDTH);
    y11_wire_put16(&screen.height_in_mm, (uint16_t)Y11_SCREEN_MM_HEIGHT);
    y11_wire_put16(&screen.min_installed_maps, 1);
    y11_wire_put16(&screen.max_installed_maps, 1);
    y11_wire_put32(&screen.root_visual, Y11_SCREEN_VISUAL);
    screen.backing_stores = 0;          /* Never */
    screen.save_unders = 0;
    screen.root_depth = 24;
    screen.allowed_depths = 5;

    /* 8-byte depth information: 1, 4, 8, 24 (with the visual), 32.
     * libXrender requires every one of these to be advertised before
     * it will even ask for the RENDER extension. */
    {
        uint8_t depths[5] = { 1, 4, 8, 24, 32 };
        int d;

        if (y11_client_send(c, &prefix, sizeof(prefix)) != 0)
            return -1;
        if (y11_client_send(c, &info, sizeof(info)) != 0)
            return -1;
        if (y11_client_send(c, vendor, vlen) != 0)
            return -1;
        if (y11_client_send(c, vendor_pad, vend_padded - vlen) != 0)
            return -1;
        if (y11_client_send(c, formats, sizeof(formats)) != 0)
            return -1;
        if (y11_client_send(c, &screen, sizeof(screen)) != 0)
            return -1;

        for (d = 0; d < 5; d++) {
            y11_depth_info di;

            memset(&di, 0, sizeof(di));
            di.depth = depths[d];
            if (depths[d] == 24) {
                y11_wire_put16(&di.visuals_count, 1);
                if (y11_client_send(c, &di, sizeof(di)) != 0)
                    return -1;
                /* 24-byte visual information */
                memset(&visual, 0, sizeof(visual));
                y11_wire_put32(&visual.visual_id, Y11_SCREEN_VISUAL);
                visual.class = 4;               /* TrueColor */
                visual.bits_per_rgb = 8;
                y11_wire_put16(&visual.colormap_entries, 256);
                y11_wire_put32(&visual.red_mask, 0x00FF0000u);
                y11_wire_put32(&visual.green_mask, 0x0000FF00u);
                y11_wire_put32(&visual.blue_mask, 0x000000FFu);
                if (y11_client_send(c, &visual, sizeof(visual)) != 0)
                    return -1;
                continue;
            }
            /* depths without visuals: legal (pixmap-only) */
            if (y11_client_send(c, &di, sizeof(di)) != 0)
                return -1;
        }
        return 0;
    }
}

/*
 * Parse the connection setup request.  *off is advanced past the bytes
 * consumed.  Returns 0 on success, -1 on a fatal error.
 */
static int y11_client_handshake(struct y11_client *c, size_t *off)
{
    const y11_conn_setup_req *req;
    uint32_t skip;

    if (c->in_len - *off < sizeof(*req))
        return 0;               /* wait for the 12-byte header */

    req = (const y11_conn_setup_req *)(c->in_buf + *off);

    if (req->byte_order != 'l') {
        /* Big-endian ('B', 0x42) and unknown byte orders are rejected in
         * phase 1; all wire data on this server is little-endian. */
        return y11_client_refuse(c, off,
            "big-endian byte order is not supported (y11 phase 1)");
    }

    if (y11_wire_get16(&req->major_version) != Y11_PROTO_MAJOR) {
        return y11_client_refuse(c, off,
            "unsupported protocol major version (y11 speaks 11)");
    }

    /* Skip the authorization protocol name and data, each padded to a
     * 4-byte boundary.  Phase 1 accepts any authorization data. */
    skip = y11_wire_pad4(y11_wire_get16(&req->auth_proto_len)) +
           y11_wire_pad4(y11_wire_get16(&req->auth_data_len));
    if (c->in_len - *off - sizeof(*req) < skip)
        return 0;               /* wait for the authorization data */

    *off += sizeof(*req) + skip;

    if (y11_client_send_setup_success(c) != 0)
        return -1;

    c->state = Y11_CLIENT_RUNNING;
    c->sequence_number = 0;
    return 0;
}

/* ---- request stream parsing ------------------------------------------------- */

/*
 * Parse every complete request packet buffered at *off and hand it to the
 * dispatcher.  Each packet increments the client's sequence number.
 * Returns 0 on success, -1 on a fatal error.
 */
static int y11_client_requests(struct y11_client *c, size_t *off)
{
    while (c->in_len - *off >= sizeof(y11_req)) {
        const uint8_t *pkt = c->in_buf + *off;
        uint16_t wire_len = y11_wire_get16(pkt + 2);
        size_t pkt_len;
        int rc;

        if (wire_len == 0) {
            if (!c->big_requests) {
                /* BIG-REQUESTS not enabled: the X server reads this as a
                 * 4-byte request (its opcode-specific fields decide). */
                pkt_len = sizeof(y11_req);
            } else {
                /* Extended 8-byte header carries the true length. */
                uint32_t true_len;

                if (c->in_len - *off < sizeof(y11_big_req))
                    break;      /* wait for the extended header */
                true_len = y11_wire_get32(pkt + 4);
                if (true_len < 2 || (size_t)true_len * 4u > Y11_INBUF_MAX) {
                    /* Cannot hold the extended header, or the request is
                     * larger than this server will ever buffer: fatal. */
                    c->wants_close = 1;
                    return -1;
                }
                pkt_len = (size_t)true_len * 4u;
            }
        } else {
            pkt_len = (size_t)wire_len * 4u;
            if (pkt_len > Y11_INBUF_MAX) {
                c->wants_close = 1;
                return -1;
            }
        }

        if (c->in_len - *off < pkt_len)
            break;              /* wait for the rest of the packet */

        rc = y11_dispatch_req(c, pkt, pkt_len);
        *off += pkt_len;
        if (rc < 0) {
            c->wants_close = 1;
            return -1;
        }
    }
    return 0;
}

/*
 * Advance the client's protocol state machine over its buffered input:
 * the connection setup handshake first, then the request stream.
 * Returns 0 on success, -1 on a fatal error.
 */
int y11_client_process(struct y11_client *c)
{
    size_t off = 0;
    int rc = 0;

    if (c->state == Y11_CLIENT_HANDSHAKE)
        rc = y11_client_handshake(c, &off);

    if (rc == 0 && c->state == Y11_CLIENT_RUNNING)
        rc = y11_client_requests(c, &off);

    if (off > 0) {
        /* Compact the reassembly buffer: drop consumed bytes. */
        memmove(c->in_buf, c->in_buf + off, c->in_len - off);
        c->in_len -= off;
    }
    return rc;
}
