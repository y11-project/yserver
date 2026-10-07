/*
 * xrender.c - RENDER extension for the Y11 display server.
 *
 * Pictures wrap drawables with a pixel format; glyph sets hold A8
 * alpha masks uploaded by the client (Xft rasterizes glyphs through
 * freetype on the client side and ships the bitmaps here with
 * AddGlyphs).  CompositeGlyphs blends the glyph masks into the
 * destination picture's drawable with the source picture's color,
 * and FillRectangles blends solid colors - both straight into the
 * 32-bit framebuffer the core rasterizer already maintains.
 *
 * Supported ops: Clear (0), Src (1) and Over (3), the ones text
 * rendering and icon compositing actually use.
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* RENDER sub-opcodes (render.h). */
enum {
    Y11_RENDER_QUERY_VERSION    = 0,
    Y11_RENDER_QUERY_PICT_FORMATS = 1,
    Y11_RENDER_CREATE_PICTURE   = 4,
    Y11_RENDER_CHANGE_PICTURE   = 5,
    Y11_RENDER_SET_PICTURE_CLIP_RECTANGLES = 6,
    Y11_RENDER_FREE_PICTURE     = 7,
    Y11_RENDER_COMPOSITE        = 8,
    Y11_RENDER_CREATE_GLYPH_SET = 17,
    Y11_RENDER_REFERENCE_GLYPH_SET = 18,
    Y11_RENDER_FREE_GLYPH_SET   = 19,
    Y11_RENDER_ADD_GLYPHS       = 20,
    Y11_RENDER_FREE_GLYPHS      = 22,
    Y11_RENDER_COMPOSITE_GLYPHS8  = 23,
    Y11_RENDER_COMPOSITE_GLYPHS16 = 24,
    Y11_RENDER_COMPOSITE_GLYPHS32 = 25,
    Y11_RENDER_FILL_RECTANGLES  = 26,
    Y11_RENDER_CREATE_SOLID_FILL = 33
};

/* PictOp values (render.h). */
enum {
    Y11_PICTOP_CLEAR = 0,
    Y11_PICTOP_SRC   = 1,
    Y11_PICTOP_OVER  = 3
};

/* RENDER error codes (render.h), offset by the extension first error. */
enum {
    Y11_RERR_BAD_PICT_FORMAT = 0,
    Y11_RERR_BAD_PICTURE     = 1,
    Y11_RERR_BAD_PICT_OP     = 2,
    Y11_RERR_BAD_GLYPH_SET   = 3,
    Y11_RERR_BAD_GLYPH       = 4
};

#define Y11_RENDER_FIRST_ERROR 142u

/* ---- the advertised pixel formats ----------------------------------------------- */

struct y11_pictformat {
    yid_t    id;
    uint8_t  type;          /* 0 = indexed, 1 = direct */
    uint8_t  depth;
    uint16_t red, red_mask;
    uint16_t green, green_mask;
    uint16_t blue, blue_mask;
    uint16_t alpha, alpha_mask;
};

/*
 * The formats a modern server exposes: ARGB32 and RGB24 for the
 * window visual, A8 for glyph masks.  RGB24 is the picture format of
 * the root visual.
 */
enum {
    Y11_PFMT_ARGB32 = 0,
    Y11_PFMT_RGB24  = 1,
    Y11_PFMT_A8     = 2,
    Y11_PFMT_COUNT  = 3
};

static const struct y11_pictformat y11_pict_formats[Y11_PFMT_COUNT] = {
    { 0x00000101, 1, 32,
      16, 0xff, 8, 0xff, 0, 0xff, 24, 0xff },
    { 0x00000102, 1, 24,
      16, 0xff, 8, 0xff, 0, 0xff, 0, 0 },
    { 0x00000103, 1, 8,
      0, 0, 0, 0, 0, 0, 0, 0xff }
};

#define Y11_PFMT_VISUAL (y11_pict_formats[Y11_PFMT_RGB24].id)
#define Y11_PFMT_A8_ID  (y11_pict_formats[Y11_PFMT_A8].id)

/* ---- pictures -------------------------------------------------------------------- */

struct y11_render_picture {
    struct y11_render_picture *next;
    struct y11_client      *client;
    yid_t                  id;
    y11_drawable_t         *drawable;   /* NULL for solid fills */
    const struct y11_pictformat *format;
    uint32_t               color;       /* 0x00RRGGBB for solid fills */
    uint32_t               color_alpha; /* 0-255 */
    int32_t                clip_x, clip_y;
    uint32_t               num_clip_rects;
    y11_rect_t             *clip_rects;
};

static struct y11_render_picture *y11_render_pictures;

static struct y11_render_picture *y11_render_picture_find(yid_t id)
{
    struct y11_render_picture *p;

    for (p = y11_render_pictures; p != NULL; p = p->next) {
        if (p->id == id)
            return p;
    }
    return NULL;
}

static void y11_render_picture_free(struct y11_render_picture *p)
{
    free(p->clip_rects);
    free(p);
}

/* ---- glyph sets ------------------------------------------------------------------ */

struct y11_render_glyph {
    struct y11_render_glyph *next;
    yid_t    id;
    uint16_t width, height;
    int16_t  x, y;          /* origin-to-bitmap offset */
    int16_t  x_off, y_off;  /* pen advance */
    uint8_t  *bits;         /* A8, width * height */
};

struct y11_render_glyphset {
    struct y11_render_glyphset *next;
    struct y11_client   *client;
    yid_t               id;
    const struct y11_pictformat *format;
    struct y11_render_glyph *glyphs;
};

static struct y11_render_glyphset *y11_render_glyphsets;

static struct y11_render_glyphset *y11_render_glyphset_find(yid_t id)
{
    struct y11_render_glyphset *gs;

    for (gs = y11_render_glyphsets; gs != NULL; gs = gs->next) {
        if (gs->id == id)
            return gs;
    }
    return NULL;
}

static struct y11_render_glyph *y11_render_glyph_find(
    struct y11_render_glyphset *gs, yid_t id)
{
    struct y11_render_glyph *g;

    for (g = gs->glyphs; g != NULL; g = g->next) {
        if (g->id == id)
            return g;
    }
    return NULL;
}

void y11_render_purge_client(struct y11_client *c)
{
    struct y11_render_picture **plink = &y11_render_pictures;
    struct y11_render_glyphset **glink = &y11_render_glyphsets;

    while (*plink != NULL) {
        struct y11_render_picture *p = *plink;

        if (p->client == c) {
            *plink = p->next;
            y11_render_picture_free(p);
        } else {
            plink = &p->next;
        }
    }
    while (*glink != NULL) {
        struct y11_render_glyphset *gs = *glink;
        struct y11_render_glyph *g = gs->glyphs;

        if (gs->client == c) {
            *glink = gs->next;
            while (g != NULL) {
                struct y11_render_glyph *next = g->next;

                free(g->bits);
                free(g);
                g = next;
            }
            free(gs);
        } else {
            glink = &gs->next;
        }
    }
}

/* ---- compositing ----------------------------------------------------------------- */

/*
 * Blend a solid color over one 32bpp XRGB pixel with PictOpOver:
 * out = color * eff + dst * (1 - eff) where eff combines the color
 * and mask alphas (both 0-255).
 */
static uint32_t y11_render_blend(uint32_t dst, uint32_t color,
                                 uint32_t color_alpha, uint32_t mask_alpha)
{
    uint32_t eff, dr, dg, db, cr, cg, cb;
    uint32_t r, g, b;

    eff = color_alpha * mask_alpha / 255u;
    if (eff >= 255u)
        return color | 0xff000000u;
    if (eff == 0u)
        return dst;

    dr = (dst >> 16) & 0xffu;
    dg = (dst >> 8) & 0xffu;
    db = dst & 0xffu;
    cr = (color >> 16) & 0xffu;
    cg = (color >> 8) & 0xffu;
    cb = color & 0xffu;

    r = (cr * eff + dr * (255u - eff)) / 255u;
    g = (cg * eff + dg * (255u - eff)) / 255u;
    b = (cb * eff + db * (255u - eff)) / 255u;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/* Apply the clip rectangles of a picture to a region; 0 = clipped out. */
static int y11_render_clipped(const struct y11_render_picture *p,
                              int32_t x, int32_t y)
{
    const y11_rect_t *r;
    uint32_t i;

    if (p->num_clip_rects == 0)
        return 0;
    x -= p->clip_x;
    y -= p->clip_y;
    for (i = 0; i < p->num_clip_rects; i++) {
        r = &p->clip_rects[i];
        if (x >= r->x && y >= r->y &&
            x < r->x + (int32_t)r->width &&
            y < r->y + (int32_t)r->height)
            return 0;
    }
    return 1;
}

/*
 * Composite an A8 glyph mask onto the destination picture at (px, py)
 * (the bitmap's top-left corner).  The source color comes from the
 * source picture: solid fills carry it directly, drawables sample
 * their own pixel at the source origin.
 */
static void y11_render_glyph_composite(const struct y11_render_picture *src,
                                       struct y11_render_picture *dst,
                                       const struct y11_render_glyph *g,
                                       int32_t px, int32_t py, uint8_t op)
{
    y11_drawable_t *d = dst->drawable;
    uint32_t color;
    int32_t row, col;

    (void)src;
    if (d == NULL || d->pixels == NULL)
        return;

    /* Source color: 1x1 drawables are the usual solid source. */
    if (src->drawable != NULL && src->drawable->pixels != NULL &&
        src->drawable->width == 1 && src->drawable->height == 1)
        color = src->drawable->pixels[0] & 0xffffffu;
    else
        color = src->color & 0xffffffu;

    for (row = 0; row < (int32_t)g->height; row++) {
        int32_t y = py + row;

        if (y < 0 || y >= (int32_t)d->height)
            continue;
        for (col = 0; col < (int32_t)g->width; col++) {
            int32_t x = px + col;
            uint8_t alpha;
            size_t off;

            if (x < 0 || x >= (int32_t)d->width)
                continue;
            if (y11_render_clipped(dst, x, y))
                continue;
            alpha = g->bits[(size_t)row * g->width + (size_t)col];
            if (alpha == 0)
                continue;
            off = (size_t)y * (d->stride / 4u) + (size_t)x;
            if (op == Y11_PICTOP_CLEAR) {
                d->pixels[off] = 0;
            } else if (op == Y11_PICTOP_SRC) {
                d->pixels[off] = color | 0xff000000u;
            } else {
                d->pixels[off] = y11_render_blend(d->pixels[off], color,
                                                  255u, alpha);
            }
        }
    }
}

/* ---- request handlers ------------------------------------------------------------ */

static int y11_render_query_version(struct y11_client *c,
                                    const uint8_t *pkt, size_t len,
                                    size_t data_off)
{
    y11_version_reply rep;

    (void)data_off;
    if (len != 12u)             /* header + major + minor */
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put32(&rep.major, 0);
    y11_wire_put32(&rep.minor, 10);
    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/*
 * QueryPictFormats (1): the format list, then the screen tree
 * binding the root visual to its RGB24 format.
 */
static int y11_render_query_pict_formats(struct y11_client *c,
                                         const uint8_t *pkt, size_t len,
                                         size_t data_off)
{
    uint8_t rep[32];
    uint8_t *body;
    size_t formats_size = Y11_PFMT_COUNT * 28u;
    size_t screen_size = 8u + 8u + 8u;  /* screen + depth + one visual */
    size_t total = formats_size + screen_size;
    uint32_t f;
    size_t off;

    (void)data_off;
    if (len != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    body = calloc(1, total);
    if (body == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }

    /* LISTofPICTFORMINFO */
    for (f = 0; f < Y11_PFMT_COUNT; f++) {
        const struct y11_pictformat *pf = &y11_pict_formats[f];
        uint8_t *e = body + (size_t)f * 28u;

        y11_wire_put32(e + 0, pf->id);
        e[4] = pf->type;
        e[5] = pf->depth;
        y11_wire_put16(e + 8, pf->red);
        y11_wire_put16(e + 10, pf->red_mask);
        y11_wire_put16(e + 12, pf->green);
        y11_wire_put16(e + 14, pf->green_mask);
        y11_wire_put16(e + 16, pf->blue);
        y11_wire_put16(e + 18, pf->blue_mask);
        y11_wire_put16(e + 20, pf->alpha);
        y11_wire_put16(e + 22, pf->alpha_mask);
        /* colormap stays 0 */
    }

    /* LISTofPICTSCREEN: one screen, one depth, one visual. */
    off = formats_size;
    y11_wire_put32(body + off + 0, 1);          /* nDepth */
    y11_wire_put32(body + off + 4, Y11_PFMT_VISUAL);   /* fallback */
    off += 8;
    body[off + 0] = 24;                          /* depth */
    y11_wire_put16(body + off + 2, 1);           /* nPictVisuals */
    off += 8;
    y11_wire_put32(body + off + 0, Y11_SCREEN_VISUAL);
    y11_wire_put32(body + off + 4, Y11_PFMT_VISUAL);

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                  /* X_Reply */
    y11_wire_put32(rep + 4, total / 4u);
    y11_wire_put32(rep + 8, Y11_PFMT_COUNT);    /* numFormats */
    y11_wire_put32(rep + 12, 1);                /* numScreens */
    y11_wire_put32(rep + 16, 1);                /* numDepths */
    y11_wire_put32(rep + 20, 1);                /* numVisuals */
    y11_wire_put32(rep + 24, 0);                /* numSubpixel */

    y11_dispatch_send_reply(c, rep, sizeof(rep));
    y11_client_send(c, body, total);
    free(body);
    return 0;
}

/* CreatePicture (4): pid, drawable, format, value mask, values. */
static int y11_render_create_picture(struct y11_client *c,
                                     const uint8_t *pkt, size_t len,
                                     size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t pid, drawable_id, format_id, value_mask;
    struct y11_render_picture *p;
    y11_drawable_t *d;
    const struct y11_pictformat *pf = NULL;
    uint32_t i;

    if (len - data_off < 16u)
        return y11_dispatch_bad_length(c, pkt[0]);

    pid = y11_wire_get32(body + 0);
    drawable_id = y11_wire_get32(body + 4);
    format_id = y11_wire_get32(body + 8);
    value_mask = y11_wire_get32(body + 12);
    (void)value_mask;           /* values are accepted and ignored */

    for (i = 0; i < Y11_PFMT_COUNT; i++) {
        if (y11_pict_formats[i].id == format_id) {
            pf = &y11_pict_formats[i];
            break;
        }
    }
    if (pf == NULL) {
        y11_dispatch_send_error(c, (uint8_t)(Y11_RENDER_FIRST_ERROR +
                                             Y11_RERR_BAD_PICT_FORMAT),
                                format_id, pkt[0]);
        return 0;
    }

    d = y11_drawable_lookup(drawable_id);
    if (d == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_DRAWABLE, drawable_id,
                                pkt[0]);
        return 0;
    }

    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    p->client = c;
    p->id = pid;
    p->drawable = d;
    p->format = pf;
    p->next = y11_render_pictures;
    y11_render_pictures = p;
    return 0;                   /* no reply */
}

/* FreePicture (7). */
static int y11_render_free_picture(struct y11_client *c,
                                   const uint8_t *pkt, size_t len,
                                   size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t pid;
    struct y11_render_picture **link = &y11_render_pictures;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    pid = y11_wire_get32(body + 0);
    while (*link != NULL) {
        struct y11_render_picture *p = *link;

        if (p->id == pid) {
            *link = p->next;
            y11_render_picture_free(p);
            return 0;
        }
        link = &p->next;
    }
    y11_dispatch_send_error(c, (uint8_t)(Y11_RENDER_FIRST_ERROR +
                                         Y11_RERR_BAD_PICTURE),
                            pid, pkt[0]);
    return 0;
}

/* CreateSolidFill (33): pid, color. */
static int y11_render_create_solid_fill(struct y11_client *c,
                                        const uint8_t *pkt, size_t len,
                                        size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t pid;
    uint16_t red, green, blue, alpha;
    struct y11_render_picture *p;

    if (len - data_off != 12u)  /* pid, color */
        return y11_dispatch_bad_length(c, pkt[0]);

    pid = y11_wire_get32(body + 0);
    red = y11_wire_get16(body + 4);
    green = y11_wire_get16(body + 6);
    blue = y11_wire_get16(body + 8);
    alpha = y11_wire_get16(body + 10);

    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    /* Un-premultiply the color into 8-bit RGB. */
    if (alpha > 0) {
        p->color = 0xff000000u |
            (uint32_t)(red * 255u / alpha) << 16 |
            (uint32_t)(green * 255u / alpha) << 8 |
            (uint32_t)(blue * 255u / alpha);
        p->color_alpha = alpha > 255 ? 255 : alpha;
    } else {
        p->color = 0;
        p->color_alpha = 0;
    }
    p->format = &y11_pict_formats[Y11_PFMT_ARGB32];
    p->client = c;
    p->id = pid;
    p->next = y11_render_pictures;
    y11_render_pictures = p;
    return 0;                   /* no reply */
}

/* FillRectangles (26): op, dst, color, then rectangles. */
static int y11_render_fill_rectangles(struct y11_client *c,
                                      const uint8_t *pkt, size_t len,
                                      size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t pid;
    uint8_t op;
    uint16_t red, green, blue, alpha;
    struct y11_render_picture *p;
    y11_drawable_t *d;
    uint32_t color, color_alpha;
    size_t nrects, i;

    if (len - data_off < 16u || ((len - data_off - 16u) & 7u) != 0u)
        return y11_dispatch_bad_length(c, pkt[0]);

    op = body[0];
    pid = y11_wire_get32(body + 4);
    red = y11_wire_get16(body + 8);
    green = y11_wire_get16(body + 10);
    blue = y11_wire_get16(body + 12);
    alpha = y11_wire_get16(body + 14);
    nrects = (len - data_off - 16u) / 8u;

    p = y11_render_picture_find(pid);
    if (p == NULL) {
        y11_dispatch_send_error(c, (uint8_t)(Y11_RENDER_FIRST_ERROR +
                                             Y11_RERR_BAD_PICTURE),
                                pid, pkt[0]);
        return 0;
    }
    d = p->drawable;
    if (d == NULL || d->pixels == NULL)
        return 0;               /* solid fills cannot be painted */

    if (alpha > 0) {
        color = (uint32_t)(red * 255u / alpha) << 16 |
                (uint32_t)(green * 255u / alpha) << 8 |
                (uint32_t)(blue * 255u / alpha);
        color_alpha = alpha > 255 ? 255 : alpha;
    } else {
        color = 0;
        color_alpha = 0;
    }

    for (i = 0; i < nrects; i++) {
        const uint8_t *r = body + 16u + i * 8u;
        int32_t x = (int16_t)y11_wire_get16(r + 0);
        int32_t y = (int16_t)y11_wire_get16(r + 2);
        int32_t w = y11_wire_get16(r + 4);
        int32_t h = y11_wire_get16(r + 6);
        int32_t row, col;

        for (row = y; row < y + h; row++) {
            if (row < 0 || row >= (int32_t)d->height)
                continue;
            for (col = x; col < x + w; col++) {
                size_t po;

                if (col < 0 || col >= (int32_t)d->width)
                    continue;
                if (y11_render_clipped(p, col, row))
                    continue;
                po = (size_t)row * (d->stride / 4u) + (size_t)col;
                if (op == Y11_PICTOP_CLEAR) {
                    d->pixels[po] = 0;
                } else if (op == Y11_PICTOP_SRC) {
                    d->pixels[po] = color | 0xff000000u;
                } else {
                    d->pixels[po] = y11_render_blend(d->pixels[po], color,
                                                     color_alpha, 255u);
                }
            }
        }
    }
    y11_damage_drawn(d, 0, 0, (int32_t)d->width, (int32_t)d->height);
    return 0;                   /* no reply */
}

/* CreateGlyphSet (17) / ReferenceGlyphSet (18). */
static int y11_render_create_glyph_set(struct y11_client *c,
                                       const uint8_t *pkt, size_t len,
                                       size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t gsid, format_id;
    struct y11_render_glyphset *gs;
    const struct y11_pictformat *pf = NULL;
    uint32_t i;

    if (len - data_off != 8u)
        return y11_dispatch_bad_length(c, pkt[0]);

    gsid = y11_wire_get32(body + 0);
    format_id = y11_wire_get32(body + 4);
    for (i = 0; i < Y11_PFMT_COUNT; i++) {
        if (y11_pict_formats[i].id == format_id) {
            pf = &y11_pict_formats[i];
            break;
        }
    }
    if (pf == NULL) {
        y11_dispatch_send_error(c, (uint8_t)(Y11_RENDER_FIRST_ERROR +
                                             Y11_RERR_BAD_PICT_FORMAT),
                                format_id, pkt[0]);
        return 0;
    }

    gs = calloc(1, sizeof(*gs));
    if (gs == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0, pkt[0]);
        return 0;
    }
    gs->client = c;
    gs->id = gsid;
    gs->format = pf;
    gs->next = y11_render_glyphsets;
    y11_render_glyphsets = gs;
    return 0;                   /* no reply */
}

/* FreeGlyphSet (19). */
static int y11_render_free_glyph_set(struct y11_client *c,
                                     const uint8_t *pkt, size_t len,
                                     size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t gsid;
    struct y11_render_glyphset **link = &y11_render_glyphsets;

    if (len - data_off != 4u)
        return y11_dispatch_bad_length(c, pkt[0]);

    gsid = y11_wire_get32(body + 0);
    while (*link != NULL) {
        struct y11_render_glyphset *gs = *link;

        if (gs->id == gsid) {
            struct y11_render_glyph *g = gs->glyphs;

            *link = gs->next;
            while (g != NULL) {
                struct y11_render_glyph *next = g->next;

                free(g->bits);
                free(g);
                g = next;
            }
            free(gs);
            return 0;
        }
        link = &gs->next;
    }
    y11_dispatch_send_error(c, (uint8_t)(Y11_RENDER_FIRST_ERROR +
                                         Y11_RERR_BAD_GLYPH_SET),
                            gsid, pkt[0]);
    return 0;
}

/*
 * AddGlyphs (20): glyphset, nglyphs, glyph ids, per-glyph xGlyphInfo,
 * then the bitmap data (one byte per pixel for A8, padded to a
 * 4-byte boundary per glyph).
 */
static int y11_render_add_glyphs(struct y11_client *c, const uint8_t *pkt,
                                 size_t len, size_t data_off)
{
    const uint8_t *body = pkt + data_off;
    uint32_t gsid, nglyphs;
    struct y11_render_glyphset *gs;
    size_t off, i;

    if (len - data_off < 8u)
        return y11_dispatch_bad_length(c, pkt[0]);

    gsid = y11_wire_get32(body + 0);
    nglyphs = y11_wire_get32(body + 4);
    gs = y11_render_glyphset_find(gsid);
    if (gs == NULL) {
        y11_dispatch_send_error(c, (uint8_t)(Y11_RENDER_FIRST_ERROR +
                                             Y11_RERR_BAD_GLYPH_SET),
                                gsid, pkt[0]);
        return 0;
    }

    if ((size_t)nglyphs > (len - data_off - 8u) / 4u)
        return y11_dispatch_bad_length(c, pkt[0]);
    off = 8u + (size_t)nglyphs * 4u;    /* past the glyph id list */
    if (off + (size_t)nglyphs * 12u > len - data_off)
        return y11_dispatch_bad_length(c, pkt[0]);

    {
        size_t infos_off = off;
        size_t bits_off = off + (size_t)nglyphs * 12u;  /* bitmaps follow
                                                         * every info */

        for (i = 0; i < nglyphs; i++) {
            yid_t gid = y11_wire_get32(body + 8u + i * 4u);
            const uint8_t *info = body + infos_off + i * 12u;
            uint16_t width = y11_wire_get16(info + 0);
            uint16_t height = y11_wire_get16(info + 2);
            int16_t gx = (int16_t)y11_wire_get16(info + 4);
            int16_t gy = (int16_t)y11_wire_get16(info + 6);
            int16_t x_off = (int16_t)y11_wire_get16(info + 8);
            int16_t y_off = (int16_t)y11_wire_get16(info + 10);
            size_t bits_size = (size_t)width * (size_t)height;
            struct y11_render_glyph *g;
            uint8_t *bits = NULL;

            if (bits_size > 0) {
                size_t padded = y11_wire_pad4((uint32_t)bits_size);

                if (bits_off + bits_size > len - data_off)
                    return y11_dispatch_bad_length(c, pkt[0]);
                bits = malloc(bits_size);
                if (bits == NULL) {
                    y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0,
                                            pkt[0]);
                    return 0;
                }
                memcpy(bits, body + bits_off, bits_size);
                bits_off += padded;
            }

            /* Replace any earlier glyph with the same id. */
            g = y11_render_glyph_find(gs, gid);
            if (g == NULL) {
                g = calloc(1, sizeof(*g));
                if (g == NULL) {
                    free(bits);
                    y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0,
                                            pkt[0]);
                    return 0;
                }
                g->id = gid;
                g->next = gs->glyphs;
                gs->glyphs = g;
            } else {
                free(g->bits);
            }
            g->width = width;
            g->height = height;
            g->x = gx;
            g->y = gy;
            g->x_off = x_off;
            g->y_off = y_off;
            g->bits = bits;
        }
    }
    return 0;                   /* no reply */
}

/*
 * CompositeGlyphs8/16/32 (23/24/25): the glyph list is a sequence of
 * elements, each (len, deltax, deltay, chars); the first element's
 * deltas position the pen absolutely, later glyphs advance it by
 * their xOff/yOff.
 */
static int y11_render_composite_glyphs(struct y11_client *c,
                                       const uint8_t *pkt, size_t len,
                                       size_t data_off, int chars_per)
{
    const uint8_t *body = pkt + data_off;
    uint8_t op;
    uint32_t src_id, dst_id, gsid;
    struct y11_render_picture *src, *dst;
    struct y11_render_glyphset *gs;
    size_t off;
    int32_t pen_x = 0, pen_y = 0;
    int first_elt = 1;

    if (len - data_off < 24u)
        return y11_dispatch_bad_length(c, pkt[0]);

    op = body[0];
    src_id = y11_wire_get32(body + 4);
    dst_id = y11_wire_get32(body + 8);
    gsid = y11_wire_get32(body + 16);

    src = y11_render_picture_find(src_id);
    dst = y11_render_picture_find(dst_id);
    gs = y11_render_glyphset_find(gsid);
    if (src == NULL || dst == NULL) {
        y11_dispatch_send_error(c, (uint8_t)(Y11_RENDER_FIRST_ERROR +
                                             Y11_RERR_BAD_PICTURE),
                                src == NULL ? src_id : dst_id, pkt[0]);
        return 0;
    }
    if (gs == NULL) {
        y11_dispatch_send_error(c, (uint8_t)(Y11_RENDER_FIRST_ERROR +
                                             Y11_RERR_BAD_GLYPH_SET),
                                gsid, pkt[0]);
        return 0;
    }

    off = 24u;
    while (off < len - data_off) {
        uint8_t elt_len;
        int32_t deltax, deltay;
        uint32_t ch;

        if (len - data_off - off < 8u)
            break;
        elt_len = body[off];
        deltax = (int16_t)y11_wire_get16(body + off + 4);
        deltay = (int16_t)y11_wire_get16(body + off + 6);

        if (first_elt) {
            pen_x = deltax;     /* absolute start position */
            pen_y = deltay;
            first_elt = 0;
        }

        off += 8u;
        for (ch = 0; ch < elt_len; ch++) {
            yid_t gid;
            struct y11_render_glyph *g;
            int32_t px, py;

            if (len - data_off - off < (size_t)chars_per)
                break;
            if (chars_per == 1) {
                gid = body[off];
            } else if (chars_per == 2) {
                gid = y11_wire_get16(body + off);
            } else {
                gid = y11_wire_get32(body + off);
            }
            off += (size_t)chars_per;

            g = y11_render_glyph_find(gs, gid);
            if (g == NULL || g->bits == NULL)
                continue;
            px = pen_x - g->x;
            py = pen_y - g->y;
            y11_render_glyph_composite(src, dst, g, px, py, op);
            pen_x += g->x_off;
            pen_y += g->y_off;
        }
        /* Elements pad to a 4-byte boundary. */
        off = (off + 3u) & ~(size_t)3u;
    }
    if (dst->drawable != NULL)
        y11_damage_drawn(dst->drawable, 0, 0,
                         (int32_t)dst->drawable->width,
                         (int32_t)dst->drawable->height);
    return 0;                   /* no reply */
}

/* ---- dispatcher ------------------------------------------------------------------- */

int y11_render_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                   size_t data_off)
{
    switch (pkt[1]) {
    case Y11_RENDER_QUERY_VERSION:
        return y11_render_query_version(c, pkt, len, data_off);
    case Y11_RENDER_QUERY_PICT_FORMATS:
        return y11_render_query_pict_formats(c, pkt, len, data_off);
    case Y11_RENDER_CREATE_PICTURE:
        return y11_render_create_picture(c, pkt, len, data_off);
    case Y11_RENDER_FREE_PICTURE:
        return y11_render_free_picture(c, pkt, len, data_off);
    case Y11_RENDER_CREATE_SOLID_FILL:
        return y11_render_create_solid_fill(c, pkt, len, data_off);
    case Y11_RENDER_FILL_RECTANGLES:
        return y11_render_fill_rectangles(c, pkt, len, data_off);
    case Y11_RENDER_CREATE_GLYPH_SET:
    case Y11_RENDER_REFERENCE_GLYPH_SET:
        return y11_render_create_glyph_set(c, pkt, len, data_off);
    case Y11_RENDER_FREE_GLYPH_SET:
        return y11_render_free_glyph_set(c, pkt, len, data_off);
    case Y11_RENDER_ADD_GLYPHS:
        return y11_render_add_glyphs(c, pkt, len, data_off);
    case Y11_RENDER_COMPOSITE_GLYPHS8:
        return y11_render_composite_glyphs(c, pkt, len, data_off, 1);
    case Y11_RENDER_COMPOSITE_GLYPHS16:
        return y11_render_composite_glyphs(c, pkt, len, data_off, 2);
    case Y11_RENDER_COMPOSITE_GLYPHS32:
        return y11_render_composite_glyphs(c, pkt, len, data_off, 4);
    case Y11_RENDER_CHANGE_PICTURE:
    case Y11_RENDER_SET_PICTURE_CLIP_RECTANGLES:
    case Y11_RENDER_FREE_GLYPHS:
    case Y11_RENDER_COMPOSITE:
    case 27:                    /* RenderCreateCursor: decorative */
        return 0;               /* accepted, no reply */
    default:
        y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, pkt[1], pkt[0]);
        return 0;
    }
}
