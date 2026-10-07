/*
 * dri3.c - DRI3 extension for the Y11 display server: direct rendering
 * infrastructure buffer passing.
 *
 * DRI3Open hands the client an authenticated DRM render node file
 * descriptor through SCM_RIGHTS, letting Mesa open the GPU without
 * root.  DRI3PixmapFromBuffer imports a client DMA-BUF through DRM
 * Prime into a server GEM handle wrapped as a pixmap; DRI3Buffer-
 * FromPixmap exports the reverse.  Fences arrive as shared xshmfence
 * pages that the server triggers when a presented buffer becomes
 * idle again.
 *
 * GEM and Prime ioctls are unprivileged operations on the render
 * node; no DRM master is needed for any of this.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
/* syscall(2) is a BSD extension not declared under strict POSIX. */
extern long syscall(long number, ...);
#elif defined(__FreeBSD__)
#include <sys/umtx.h>
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "y11.h"
#include "y11_wire.h"

/* Sub-opcodes (dri3proto.h). */
enum {
    Y11_DRI3_QUERY_VERSION       = 0,
    Y11_DRI3_OPEN                 = 1,
    Y11_DRI3_PIXMAP_FROM_BUFFER  = 2,
    Y11_DRI3_BUFFER_FROM_PIXMAP  = 3,
    Y11_DRI3_FENCE_FROM_FD       = 4
};

static int y11_dri3_fd = -1;    /* render (or card) node fd */
static bool y11_dri3_fd_owned;  /* we opened it (vs borrowed) */

/*
 * Acquire a DRM device fd for GEM/Prime operations: prefer the
 * session's card fd, then probe render and card nodes directly,
 * keeping the first that actually supports dumb buffers (some
 * drivers, e.g. i915 render nodes, refuse CREATE_DUMB with EPERM).
 */
static int y11_dri3_probe(int fd)
{
    struct drm_mode_create_dumb create;
    int ok;

    memset(&create, 0, sizeof(create));
    create.width = 1;
    create.height = 1;
    create.bpp = 32;
    ok = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0;
    if (ok) {
        struct drm_mode_destroy_dumb d = { .handle = create.handle };

        (void)drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    return ok;
}

int y11_dri3_init(int preferred_fd)
{
    static const char *const paths[] = {
        "/dev/dri/renderD128", "/dev/dri/renderD129",
        "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2", NULL
    };
    int i;

    if (preferred_fd >= 0 && y11_dri3_probe(preferred_fd)) {
        y11_dri3_fd = preferred_fd;
        y11_dri3_fd_owned = false;
        fprintf(stderr, "y11: dri3 using session device fd\n");
        return 0;
    }
    for (i = 0; paths[i] != NULL; i++) {
        int fd = open(paths[i], O_RDWR | O_CLOEXEC);

        if (fd >= 0) {
            if (y11_dri3_probe(fd)) {
                y11_dri3_fd = fd;
                y11_dri3_fd_owned = true;
                fprintf(stderr, "y11: dri3 device %s\n", paths[i]);
                return 0;
            }
            close(fd);
        }
    }
    fprintf(stderr, "y11: dri3: no usable DRM device, extension"
            " partially disabled\n");
    /* Keep the render node if we at least managed to open one; DRI3Open
     * still hands the client a working device fd. */
    {
        int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);

        if (fd >= 0) {
            y11_dri3_fd = fd;
            y11_dri3_fd_owned = true;
            return 0;
        }
    }
    return -1;
}

void y11_dri3_shutdown(void)
{
    if (y11_dri3_fd_owned && y11_dri3_fd >= 0)
        close(y11_dri3_fd);
    y11_dri3_fd = -1;
    y11_dri3_fd_owned = false;
}

/* Release a DRI3 buffer's kernel references. */
void y11_dri3_release_buffer(struct y11_dri3_buffer *buf)
{
    if (buf == NULL || y11_dri3_fd < 0)
        return;
    if (buf->map != NULL && buf->map != MAP_FAILED) {
        munmap(buf->map, buf->map_size);
        buf->map = NULL;
    }
    if (buf->fb_id != 0) {
        drmModeRmFB(y11_dri3_fd, buf->fb_id);
        buf->fb_id = 0;
    }
    if (buf->gem_handle != 0) {
        struct drm_gem_close close_req = { .handle = buf->gem_handle };

        (void)drmIoctl(y11_dri3_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
        buf->gem_handle = 0;
    }
    if (buf->prime_fd >= 0) {
        close(buf->prime_fd);
        buf->prime_fd = -1;
    }
}

/*
 * Map an imported GEM handle for CPU access (the Present blit path).
 * Only works for dumb-backed buffers; graceful NULL otherwise.
 */
int y11_dri3_pixmap_cpu_map(struct y11_pixmap *p)
{
    struct drm_mode_map_dumb map_req;
    void *addr;

    if (p == NULL || !p->is_dri3 || p->dri3 == NULL ||
        p->dri3->map != NULL || p->dri3->gem_handle == 0)
        return -1;

    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = p->dri3->gem_handle;
    if (drmIoctl(y11_dri3_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) != 0)
        return -1;

    addr = mmap(NULL, p->dri3->map_size > 0 ? p->dri3->map_size :
                      (size_t)p->dri3->stride * p->base.height,
                PROT_READ, MAP_SHARED, y11_dri3_fd, map_req.offset);
    if (addr == MAP_FAILED)
        return -1;
    p->dri3->map = addr;
    return 0;
}

/* ---- ShmQueryVersion (0) -------------------------------------------------- */

static int y11_dri3_query_version(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off)
{
    y11_version_reply rep;

    (void)pkt;
    (void)data_off;
    if (len != 12u)
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put32(&rep.major, 1);
    y11_wire_put32(&rep.minor, 2);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* ---- DRI3Open (1) ----------------------------------------------------------- */

/*
 * Clients receive a render node fd, like on a real server: Mesa's
 * driver allocates real GEM buffers through it (the i915 render node
 * refuses dumb buffers with EPERM, but direct rendering never needs
 * them).  The server keeps its own card fd for Prime imports and the
 * dumb-buffer CPU maps used by the Present blit path.
 */
static int y11_dri3_client_fd(void)
{
    static const char *const render_paths[] = {
        "/dev/dri/renderD128", "/dev/dri/renderD129", NULL
    };
    int i;

    for (i = 0; render_paths[i] != NULL; i++) {
        int fd = open(render_paths[i], O_RDWR | O_CLOEXEC);

        if (fd >= 0)
            return fd;
    }
    /* No render node: fall back to the server's own device fd. */
    if (y11_dri3_fd >= 0)
        return fcntl(y11_dri3_fd, F_DUPFD_CLOEXEC, 0);
    return -1;
}

static int y11_dri3_open(struct y11_client *c, const uint8_t *pkt,
                         size_t len, size_t data_off)
{
    y11_version_reply rep;     /* same 32-byte shape, all pads */
    int fd;

    (void)data_off;
    if (len != 12u)             /* drawable, provider */
        return y11_dispatch_bad_length(c, pkt[0]);

    if (y11_dri3_fd < 0) {
        /* No device available: report the failure, no fd attached. */
        memset(&rep, 0, sizeof(rep));
        rep.hdr.type = 1;
        rep.hdr.pad0 = 0;       /* nfd = 0 */
        y11_wire_put32(&rep.hdr.length, 0);
        y11_dispatch_send_reply(c, &rep, sizeof(rep));
        return 0;
    }

    fd = y11_dri3_client_fd();
    if (fd < 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 1;           /* nfd: one descriptor attached */
    y11_wire_put32(&rep.hdr.length, 0);

    y11_dispatch_send_reply_fd(c, &rep, sizeof(rep), fd);
    return 0;
}

static int y11_dri3_pixmap_from_buffer(struct y11_client *c,
                                       const uint8_t *pkt, size_t len,
                                       size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    yid_t pid;
    uint32_t size, width, height, stride;
    uint8_t depth, bpp;
    int prime_fd;
    struct drm_prime_handle prime_req;
    struct y11_pixmap *p;

    if (len - data_off != 20u)
        return y11_dispatch_bad_length(c, pkt[0]);

    prime_fd = y11_client_pop_fd(c);
    if (prime_fd < 0 || y11_dri3_fd < 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, 0, pkt[0]);
        return 0;
    }

    pid = y11_wire_get32(body + 0);
    /* drawable (screen reference) at +4 is accepted, not used */
    size = y11_wire_get32(body + 8);
    width = y11_wire_get16(body + 12);
    height = y11_wire_get16(body + 14);
    stride = y11_wire_get16(body + 16);
    depth = body[18];
    bpp = body[19];

    if (width == 0 || height == 0 || stride == 0 || depth != 24 ||
        bpp != 32 || size < (uint64_t)stride * height ||
        pid < c->resource_id_base ||
        pid - c->resource_id_base > Y11_RID_MASK ||
        y11_window_get(pid) != NULL ||
        y11_resource_get(pid, Y11_RESOURCE_PIXMAP) != NULL) {
        close(prime_fd);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ID_CHOICE, pid, pkt[0]);
        return 0;
    }

    /* Import the client's DMA-BUF into our GEM namespace. */
    memset(&prime_req, 0, sizeof(prime_req));
    prime_req.fd = prime_fd;
    prime_req.flags = 0;
    if (drmIoctl(y11_dri3_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_req)
        != 0) {
        fprintf(stderr, "y11: dri3: prime import failed: %s\n",
                strerror(errno));
        close(prime_fd);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, pid, pkt[0]);
        return 0;
    }
    close(prime_fd);            /* the GEM handle keeps the memory alive */

    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        struct drm_gem_close close_req = { .handle = prime_req.handle };

        (void)drmIoctl(y11_dri3_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    p->dri3 = calloc(1, sizeof(*p->dri3));
    if (p->dri3 == NULL) {
        struct drm_gem_close close_req = { .handle = prime_req.handle };

        free(p);
        (void)drmIoctl(y11_dri3_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    p->base.id = pid;
    p->base.type = Y11_DRAWABLE_PIXMAP;
    p->base.width = (uint16_t)width;
    p->base.height = (uint16_t)height;
    p->base.depth = depth;
    p->base.bpp = (uint8_t)bpp;
    p->base.stride = stride;
    p->base.pixels = NULL;      /* GPU memory: CPU map on demand */
    p->dri3->prime_fd = -1;
    p->dri3->gem_handle = prime_req.handle;
    p->dri3->stride = stride;
    p->dri3->size = size;
    p->dri3->format = 0x32313858;   /* DRM_FORMAT_XRGB8888 */
    p->dri3->modifier = 0;          /* linear */
    p->dri3->map_size = (size_t)stride * height;
    p->owner = c;
    p->is_dri3 = true;

    if (y11_resource_add(pid, Y11_RESOURCE_PIXMAP, p) != 0) {
        y11_dri3_release_buffer(p->dri3);
        free(p->dri3);
        free(p);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    return 0;                   /* no reply */
}

/* ---- DRI3BufferFromPixmap (3) ---------------------------------------------------- */

static int y11_dri3_buffer_from_pixmap(struct y11_client *c,
                                       const uint8_t *pkt, size_t len,
                                       size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    yid_t pid;
    struct y11_pixmap *p;
    struct drm_prime_handle prime_req;
    y11_dri3_buffer_reply rep;
    int fd;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    pid = y11_wire_get32(body + 0);
    p = y11_resource_get(pid, Y11_RESOURCE_PIXMAP);
    if (p == NULL || p->owner != c || y11_dri3_fd < 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_PIXMAP, pid, pkt[0]);
        return 0;
    }

    /*
     * DRI3 pixmaps already hold a GEM handle: export it.  Software
     * pixmaps get staged into a fresh dumb buffer first (the export
     * needs kernel memory to point at).
     */
    if (p->is_dri3 && p->dri3 != NULL && p->dri3->gem_handle != 0) {
        /* nothing to stage */
    } else {
        struct drm_mode_create_dumb create;
        struct drm_mode_map_dumb map_req;
        void *addr;
        uint32_t row;

        memset(&create, 0, sizeof(create));
        create.width = p->base.width;
        create.height = p->base.height;
        create.bpp = 32;
        if (drmIoctl(y11_dri3_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
            y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, pid, pkt[0]);
            return 0;
        }
        memset(&map_req, 0, sizeof(map_req));
        map_req.handle = create.handle;
        if (drmIoctl(y11_dri3_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) != 0) {
            struct drm_mode_destroy_dumb d = { .handle = create.handle };

            (void)drmIoctl(y11_dri3_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
            y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, pid, pkt[0]);
            return 0;
        }
        addr = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    y11_dri3_fd, map_req.offset);
        if (addr == MAP_FAILED) {
            struct drm_mode_destroy_dumb d = { .handle = create.handle };

            (void)drmIoctl(y11_dri3_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
            y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, pid, pkt[0]);
            return 0;
        }
        for (row = 0; row < p->base.height; row++) {
            memcpy((uint8_t *)addr + row * create.pitch,
                   (uint8_t *)p->base.pixels + row * p->base.stride,
                   (size_t)p->base.width * 4u);
        }
        munmap(addr, create.size);

        if (!p->is_dri3) {
            p->dri3 = calloc(1, sizeof(*p->dri3));
            if (p->dri3 == NULL) {
                struct drm_mode_destroy_dumb d = { .handle = create.handle };

                (void)drmIoctl(y11_dri3_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
                y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, pid, pkt[0]);
                return 0;
            }
            p->is_dri3 = true;
            p->dri3->prime_fd = -1;
        }
        p->dri3->gem_handle = create.handle;
        p->dri3->stride = create.pitch;
        p->dri3->size = create.size;
        p->dri3->format = 0x32313858;
        p->dri3->modifier = 0;
        p->dri3->map_size = create.size;
    }

    memset(&prime_req, 0, sizeof(prime_req));
    prime_req.handle = p->dri3->gem_handle;
    prime_req.flags = DRM_CLOEXEC | DRM_RDWR;
    if (drmIoctl(y11_dri3_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_req)
        != 0) {
        fprintf(stderr, "y11: dri3: prime export failed: %s\n",
                strerror(errno));
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, pid, pkt[0]);
        return 0;
    }
    fd = prime_req.fd;

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    rep.hdr.pad0 = 1;           /* nfd: one descriptor attached */
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put32(&rep.size, p->dri3->size);
    y11_wire_put16(&rep.width, p->base.width);
    y11_wire_put16(&rep.height, p->base.height);
    y11_wire_put16(&rep.stride, (uint16_t)p->dri3->stride);
    rep.depth = p->base.depth;
    rep.bpp = p->base.bpp;

    y11_dispatch_send_reply_fd(c, &rep, sizeof(rep), fd);
    return 0;
}

/* ---- DRI3FenceFromFD (4) ------------------------------------------------------------- */

/*
 * DRI3 fences are shared-memory xshmfence pages: the client mmaps a
 * page, hands the fd over, and then waits on the page's 32-bit value
 * with futexes.  Triggering the fence means storing a non-zero value
 * and waking the waiters, exactly like libxshmfence does.  Mesa wraps
 * every DRI3 swapchain buffer in one and passes its XID as the idle
 * fence of PresentPixmap: the server triggers it once the buffer is
 * no longer needed for scanout.
 */
struct y11_dri3_fence {
    struct y11_dri3_fence *next;
    struct y11_client    *client;
    yid_t                fence;      /* XSyncFence XID chosen by the client */
    yid_t                pixmap;     /* pixmap the fence was created on */
    volatile int32_t     *page;      /* mapped fence page (int32 at offset 0) */
};

static struct y11_dri3_fence *y11_dri3_fences;

/* Wake every xshmfence waiter sleeping on this page. */
static void y11_dri3_fence_wake(volatile int32_t *page)
{
#if defined(__linux__)
    syscall(SYS_futex, (int *)(uintptr_t)page, 1 /* FUTEX_WAKE */,
            0x7fffffff);
#elif defined(__FreeBSD__)
    _umtx_op((void *)(uintptr_t)page, UMTX_OP_WAKE,
             (void *)(uintptr_t)0x7fffffff, NULL, NULL);
#endif
}

/* Store the triggered value like libxshmfence's xshmfence_trigger. */
static void y11_dri3_fence_signal(struct y11_dri3_fence *f)
{
    if (f->page != NULL && *f->page == 0) {
        *f->page = 1;
        y11_dri3_fence_wake(f->page);
    }
}

/* Trigger the fence registered under this XID, if any. */
void y11_dri3_fence_trigger(yid_t fence)
{
    struct y11_dri3_fence *f;

    for (f = y11_dri3_fences; f != NULL; f = f->next) {
        if (f->fence == fence) {
            y11_dri3_fence_signal(f);
            return;
        }
    }
}

/* Release every fence a client created (disconnect cleanup). */
void y11_dri3_fence_purge_client(struct y11_client *c)
{
    struct y11_dri3_fence **link = &y11_dri3_fences;

    while (*link != NULL) {
        struct y11_dri3_fence *f = *link;

        if (f->client == c) {
            *link = f->next;
            munmap((void *)(uintptr_t)f->page, 4096);
            free(f);
        } else {
            link = &f->next;
        }
    }
}

static int y11_dri3_fence_from_fd(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t pixmap_id, fence_id;
    uint8_t initially_triggered;
    struct y11_dri3_fence *f;
    volatile int32_t *page;
    int fd;

    if (len - data_off != 12u)  /* pixmap, fence, initially-triggered */
        return y11_dispatch_bad_length(c, pkt[0]);

    pixmap_id = y11_wire_get32(body + 0);
    fence_id = y11_wire_get32(body + 4);
    initially_triggered = body[8];

    fd = y11_client_pop_fd(c);
    if (fd < 0) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_MATCH, 0, pkt[0]);
        return 0;
    }

    page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (page == MAP_FAILED) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }

    f = calloc(1, sizeof(*f));
    if (f == NULL) {
        munmap((void *)page, 4096);
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    f->client = c;
    f->fence = fence_id;
    f->pixmap = pixmap_id;
    f->page = page;
    f->next = y11_dri3_fences;
    y11_dri3_fences = f;

    if (initially_triggered)
        y11_dri3_fence_signal(f);
    return 0;                   /* no reply */
}

/* ---- dispatcher ----------------------------------------------------------------------- */

int y11_dri3_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                 size_t data_off)
{
    switch (pkt[1]) {
    case Y11_DRI3_QUERY_VERSION:
        return y11_dri3_query_version(c, pkt, len, data_off);
    case Y11_DRI3_OPEN:
        return y11_dri3_open(c, pkt, len, data_off);
    case Y11_DRI3_PIXMAP_FROM_BUFFER:
        return y11_dri3_pixmap_from_buffer(c, pkt, len, data_off);
    case Y11_DRI3_BUFFER_FROM_PIXMAP:
        return y11_dri3_buffer_from_pixmap(c, pkt, len, data_off);
    case Y11_DRI3_FENCE_FROM_FD:
        return y11_dri3_fence_from_fd(c, pkt, len, data_off);
    default:
        y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, pkt[1], pkt[0]);
        return 0;
    }
}
