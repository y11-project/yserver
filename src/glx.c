/*
 * glx.c - GLX visual bridge for the Y11 display server.
 *
 * Legacy OpenGL clients (glxgears, glxinfo) speak the GLX wire
 * extension before they ever touch DRI3: they negotiate a version,
 * discover visuals and fbconfigs, and create a context.  In direct
 * rendering none of that needs a server-side GL: Mesa builds the real
 * GPU context on the client side using the render node fd it receives
 * from DRI3Open, swaps through Present, and only uses these replies to
 * pick a pixel format.  This module answers exactly those questions
 * and hands everything else back to the DRI3/Present path.
 *
 * Wire layouts are from glxproto.h / the Mesa client parsers
 * (src/glx/glxext.c): GetVisualConfigs returns 18 fixed properties
 * per visual (__GLX_MIN_CONFIG_PROPS); GetFBConfigs returns tagged
 * attribute/value pairs.
 *
 * Copyright (c) 2026 The Y11 Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* GLX sub-opcodes (glxproto.h). */
enum {
    Y11_GLX_RENDER              = 1,
    Y11_GLX_CREATE_CONTEXT      = 3,
    Y11_GLX_DESTROY_CONTEXT    = 4,
    Y11_GLX_MAKE_CURRENT        = 5,
    Y11_GLX_IS_DIRECT           = 6,
    Y11_GLX_QUERY_VERSION       = 7,
    Y11_GLX_SWAP_BUFFERS        = 11,
    Y11_GLX_GET_VISUAL_CONFIGS  = 14,
    Y11_GLX_QUERY_SERVER_STRING = 19,
    Y11_GLX_CLIENT_INFO         = 20,
    Y11_GLX_GET_FB_CONFIGS      = 21,
    Y11_GLX_CREATE_WINDOW       = 22,
    Y11_GLX_DESTROY_WINDOW      = 23,
    Y11_GLX_CREATE_NEW_CONTEXT  = 24,
    Y11_GLX_QUERY_CONTEXT      = 25,
    Y11_GLX_MAKE_CONTEXT_CURRENT = 26,
    Y11_GLX_GET_DRAWABLE_ATTRIBUTES = 29,
    Y11_GLX_CHANGE_DRAWABLE_ATTRIBUTES = 30,
    Y11_GLX_SET_CLIENT_INFO_ARB = 33,
    Y11_GLX_CREATE_CONTEXT_ATTRIBS_ARB = 34,
    Y11_GLX_SET_CLIENT_INFO2_ARB = 35
};

/* GLX attribute tokens (glxtokens.h). */
enum {
    Y11_GLX_USE_GL          = 1,
    Y11_GLX_BUFFER_SIZE     = 2,
    Y11_GLX_LEVEL           = 3,
    Y11_GLX_RGBA            = 4,
    Y11_GLX_DOUBLEBUFFER    = 5,
    Y11_GLX_STEREO          = 6,
    Y11_GLX_AUX_BUFFERS     = 7,
    Y11_GLX_RED_SIZE        = 8,
    Y11_GLX_GREEN_SIZE      = 9,
    Y11_GLX_BLUE_SIZE       = 10,
    Y11_GLX_ALPHA_SIZE      = 11,
    Y11_GLX_DEPTH_SIZE      = 12,
    Y11_GLX_STENCIL_SIZE    = 13,
    Y11_GLX_ACCUM_RED_SIZE   = 14,
    Y11_GLX_ACCUM_GREEN_SIZE = 15,
    Y11_GLX_ACCUM_BLUE_SIZE  = 16,
    Y11_GLX_ACCUM_ALPHA_SIZE = 17,
    Y11_GLX_CONFIG_CAVEAT     = 0x20,
    Y11_GLX_X_VISUAL_TYPE     = 0x22,
    Y11_GLX_TRANSPARENT_TYPE  = 0x23,
    Y11_GLX_DRAWABLE_TYPE     = 0x8010,
    Y11_GLX_RENDER_TYPE       = 0x8011,
    Y11_GLX_X_RENDERABLE      = 0x8012,
    Y11_GLX_FBCONFIG_ID       = 0x8013,
    Y11_GLX_VISUAL_ID         = 0x800B,
    Y11_GLX_NONE              = 0x8000,
    Y11_GLX_TRUE_COLOR        = 0x8002,
    Y11_GLX_RGBA_BIT         = 0x00000001,
    Y11_GLX_WINDOW_BIT        = 0x00000001,
    Y11_GLX_PIXMAP_BIT        = 0x00000002,
    Y11_GLX_SWAP_EXCHANGE_OML = 0x8061,
    Y11_GLX_SWAP_UNDEFINED_OML = 0x8062
};

/* GLX context registry: dummy server-side handles for direct contexts. */
struct y11_glx_context {
    struct y11_glx_context *next;
    struct y11_client      *client;
    yid_t                  id;
};

static struct y11_glx_context *y11_glx_contexts;
static uint32_t y11_glx_next_tag = 1;

static struct y11_glx_context *y11_glx_ctx_find(yid_t id)
{
    struct y11_glx_context *ctx;

    for (ctx = y11_glx_contexts; ctx != NULL; ctx = ctx->next) {
        if (ctx->id == id)
            return ctx;
    }
    return NULL;
}

static void y11_glx_ctx_create(struct y11_client *c, yid_t id)
{
    struct y11_glx_context *ctx;

    if (y11_glx_ctx_find(id) != NULL)
        return;                 /* duplicate: keep the original */
    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
        return;
    ctx->client = c;
    ctx->id = id;
    ctx->next = y11_glx_contexts;
    y11_glx_contexts = ctx;
}

void y11_glx_purge_client(struct y11_client *c)
{
    struct y11_glx_context **link = &y11_glx_contexts;

    while (*link != NULL) {
        struct y11_glx_context *ctx = *link;

        if (ctx->client == c) {
            *link = ctx->next;
            free(ctx);
        } else {
            link = &ctx->next;
        }
    }
}

/* ---- the advertised configuration --------------------------------------------- */

/* Tag/value pairs for GetFBConfigs (Mesa parses these exactly). */
struct y11_glx_pair {
    uint32_t tag;
    uint32_t value;
};

/*
 * One TrueColor visual matching the root: 24-bit depth, 32bpp,
 * double buffered, 24/8 depth/stencil.  This describes exactly what
 * the DRI3/Present path can present.
 *
 * The 18 fixed GLX 1.0 properties are followed by tagged attribute
 * pairs.  Mesa matches the advertised config against the driver's
 * configs attribute by attribute (driConfigEqual), so everything the
 * driver reports must line up: no accumulation buffers, texture
 * binding enabled, Y inverted like the driver's own configs.
 */
#define Y11_GLX_VISUAL_COUNT 1
#define Y11_GLX_MIN_PROPS     18

/* Tagged pairs appended after the fixed properties. */
static const struct y11_glx_pair y11_glx_visual_tags[] = {
    { 0x20D0 /* GLX_BIND_TO_TEXTURE_RGB_EXT */, 1 },
    { 0x20D1 /* GLX_BIND_TO_TEXTURE_RGBA_EXT */, 1 },
    { 0x20D2 /* GLX_BIND_TO_MIPMAP_TEXTURE_EXT */, 0 },
    { 0x20D3 /* GLX_BIND_TO_TEXTURE_TARGETS_EXT */, 0x1 | 0x2 | 0x4 },
    { 0x20D4 /* GLX_Y_INVERTED_EXT */, 1 },
    { 0x20B2 /* GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT */, 0 },
    { 100000 /* GLX_SAMPLE_BUFFERS_SGIS */, 0 },
    { 100001 /* GLX_SAMPLES_SGIS */, 0 }
};

#define Y11_GLX_VISUAL_TAG_COUNT \
    ((uint32_t)(sizeof(y11_glx_visual_tags) / sizeof(y11_glx_visual_tags[0])))
#define Y11_GLX_VISUAL_PROPS (Y11_GLX_MIN_PROPS + 2 * Y11_GLX_VISUAL_TAG_COUNT)

static void y11_glx_fill_visual_props(uint32_t *p)
{
    size_t i;

    p[0] = Y11_SCREEN_VISUAL;          /* visualID */
    p[1] = 4;                           /* X TrueColor class */
    p[2] = 1;                           /* rgba */
    p[3] = 8;                           /* red bits */
    p[4] = 8;                           /* green bits */
    p[5] = 8;                           /* blue bits */
    p[6] = 8;                           /* alpha bits (RGBA8888 config) */
    p[7] = 0;                           /* accum red */
    p[8] = 0;                           /* accum green */
    p[9] = 0;                           /* accum blue */
    p[10] = 0;                          /* accum alpha */
    p[11] = 1;                          /* double buffer */
    p[12] = 0;                          /* stereo */
    p[13] = 32;                         /* buffer size (rgbBits) */
    p[14] = 24;                         /* depth bits */
    p[15] = 8;                          /* stencil bits */
    p[16] = 0;                          /* aux buffers */
    p[17] = 0;                          /* level */

    for (i = 0; i < Y11_GLX_VISUAL_TAG_COUNT; i++) {
        p[Y11_GLX_MIN_PROPS + i * 2] = y11_glx_visual_tags[i].tag;
        p[Y11_GLX_MIN_PROPS + i * 2 + 1] = y11_glx_visual_tags[i].value;
    }
}

/* Tag/value pairs for GetFBConfigs (Mesa parses these exactly). */
static const struct y11_glx_pair y11_glx_fbconfig_props[] = {
    { 0x8010 /* GLX_DRAWABLE_TYPE */,
      0x00000001 | 0x00000002 /* WINDOW | PIXMAP */ },
    { 0x8011 /* GLX_RENDER_TYPE */, 0x00000001 /* RGBA_BIT */ },
    { 0x8012 /* GLX_X_RENDERABLE */, 1 },
    { 0x8013 /* GLX_FBCONFIG_ID */, 0x00000101 },
    { 0x800B /* GLX_VISUAL_ID */, Y11_SCREEN_VISUAL },
    { 0x22   /* GLX_X_VISUAL_TYPE */, 0x8002 /* GLX_TRUE_COLOR */ },
    { 5      /* GLX_DOUBLEBUFFER */, 1 },
    { 4      /* GLX_RGBA */, 1 },
    { 2      /* GLX_BUFFER_SIZE */, 32 },
    { 8      /* GLX_RED_SIZE */, 8 },
    { 9      /* GLX_GREEN_SIZE */, 8 },
    { 10     /* GLX_BLUE_SIZE */, 8 },
    { 11     /* GLX_ALPHA_SIZE */, 8 },
    { 12     /* GLX_DEPTH_SIZE */, 24 },
    { 13     /* GLX_STENCIL_SIZE */, 8 },
    { 14     /* GLX_ACCUM_RED_SIZE */, 0 },
    { 15     /* GLX_ACCUM_GREEN_SIZE */, 0 },
    { 16     /* GLX_ACCUM_BLUE_SIZE */, 0 },
    { 17     /* GLX_ACCUM_ALPHA_SIZE */, 0 },
    { 3      /* GLX_LEVEL */, 0 },
    { 7      /* GLX_AUX_BUFFERS */, 0 },
    { 6      /* GLX_STEREO */, 0 },
    { 0x20   /* GLX_CONFIG_CAVEAT */, 0x8000 /* GLX_NONE */ },
    { 0x23   /* GLX_TRANSPARENT_TYPE */, 0x8000 /* GLX_NONE */ },
    { 0x20D0 /* GLX_BIND_TO_TEXTURE_RGB_EXT */, 1 },
    { 0x20D1 /* GLX_BIND_TO_TEXTURE_RGBA_EXT */, 1 },
    { 0x20D2 /* GLX_BIND_TO_MIPMAP_TEXTURE_EXT */, 0 },
    { 0x20D3 /* GLX_BIND_TO_TEXTURE_TARGETS_EXT */, 0x1 | 0x2 | 0x4 },
    { 0x20D4 /* GLX_Y_INVERTED_EXT */, 1 },
    { 0x20B2 /* GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT */, 0 },
    { 100000 /* GLX_SAMPLE_BUFFERS_SGIS */, 0 },
    { 100001 /* GLX_SAMPLES_SGIS */, 0 }
};

#define Y11_GLX_FBCONFIG_ATTRS \
    ((uint32_t)(sizeof(y11_glx_fbconfig_props) / sizeof(y11_glx_fbconfig_props[0])))

/* ---- replies -------------------------------------------------------------------- */

/* QueryVersion (7): negotiate 1.4. */
static int y11_glx_query_version(struct y11_client *c, const uint8_t *pkt,
                                 size_t len, size_t data_off)
{
    y11_version_reply rep;

    (void)data_off;
    if (len != 12u)
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put32(&rep.major, 1);
    y11_wire_put32(&rep.minor, 4);
    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;
}

/* GetVisualConfigs (14): 18 fixed properties plus tagged pairs per visual. */
static int y11_glx_get_visual_configs(struct y11_client *c,
                                     const uint8_t *pkt, size_t len,
                                     size_t data_off)
{
    y11_glx_configs_reply rep;
    uint32_t props[Y11_GLX_VISUAL_COUNT * Y11_GLX_VISUAL_PROPS];
    uint32_t payload_words;

    (void)data_off;
    if (len != 8u)              /* screen */
        return y11_dispatch_bad_length(c, pkt[0]);

    y11_glx_fill_visual_props(props);

    payload_words = Y11_GLX_VISUAL_COUNT * Y11_GLX_VISUAL_PROPS;
    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;
    y11_wire_put32(&rep.hdr.length, payload_words);
    y11_wire_put32(&rep.num_visuals, Y11_GLX_VISUAL_COUNT);
    y11_wire_put32(&rep.num_props, Y11_GLX_VISUAL_PROPS);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    y11_client_send(c, props, (size_t)payload_words * 4u);
    return 0;
}

/* GetFBConfigs (21): tagged attribute/value pairs. */
static int y11_glx_get_fb_configs(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off)
{
    y11_glx_configs_reply rep;
    uint32_t pairs[Y11_GLX_FBCONFIG_ATTRS * 2];
    size_t i;

    (void)data_off;
    if (len != 8u)              /* screen */
        return y11_dispatch_bad_length(c, pkt[0]);

    for (i = 0; i < Y11_GLX_FBCONFIG_ATTRS; i++) {
        pairs[i * 2] = y11_glx_fbconfig_props[i].tag;
        pairs[i * 2 + 1] = y11_glx_fbconfig_props[i].value;
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;
    y11_wire_put32(&rep.hdr.length, Y11_GLX_FBCONFIG_ATTRS * 2u);
    y11_wire_put32(&rep.num_visuals, 1);   /* one FBConfig */
    y11_wire_put32(&rep.num_props,
                    Y11_GLX_FBCONFIG_ATTRS);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    y11_client_send(c, pairs, sizeof(pairs));
    return 0;
}

/*
 * QueryServerString (19): name 1 = vendor, 2 = version, 3 = the
 * supported GLX extension list.  The reply is 32 bytes with the
 * string length at byte 12, then the string (NUL terminated, padded
 * to a 4-byte boundary).
 */
static int y11_glx_query_server_string(struct y11_client *c,
                                       const uint8_t *pkt, size_t len,
                                       size_t data_off)
{
    static const char vendor[] = "The Y11 Project";
    static const char version[] = "1.4 y11";
    static const char extensions[] =
        "GLX_ARB_create_context GLX_ARB_create_context_profile "
        "GLX_ARB_fbconfig_float GLX_ARB_framebuffer_sRGB "
        "GLX_EXT_framebuffer_sRGB GLX_EXT_import_context "
        "GLX_EXT_texture_from_pixmap GLX_EXT_visual_info "
        "GLX_EXT_visual_rating GLX_MESA_copy_sub_buffer "
        "GLX_MESA_multithread_makecurrent GLX_MESA_query_renderer "
        "GLX_OML_swap_method GLX_SGI_swap_control GLX_SGI_video_sync "
        "GLX_SGIX_fbconfig GLX_SGIX_pbuffer GLX_SGIX_visual_select_group";
    const char *str = "";
    uint8_t rep[32];
    size_t slen;
    uint32_t name;

    if (len - data_off != 8u)   /* screen, name */
        return y11_dispatch_bad_length(c, pkt[0]);

    name = y11_wire_get32(pkt + data_off + 4);
    switch (name) {
    case 1:
        str = vendor;
        break;
    case 2:
        str = version;
        break;
    case 3:
        str = extensions;
        break;
    default:
        str = "";
        break;
    }

    slen = strlen(str) + 1;     /* include the NUL, like the Xorg server */
    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                  /* X_Reply */
    y11_wire_put32(rep + 4, y11_wire_pad4((uint32_t)slen) / 4u);
    y11_wire_put32(rep + 12, (uint32_t)slen);
    y11_dispatch_send_reply(c, rep, sizeof(rep));

    {
        uint8_t buf[512];

        if (slen > sizeof(buf))
            return 0;
        memset(buf, 0, y11_wire_pad4((uint32_t)slen));
        memcpy(buf, str, slen);
        y11_client_send(c, buf, y11_wire_pad4((uint32_t)slen));
    }
    return 0;
}

/*
 * MakeCurrent (5) / MakeContextCurrent (26): reply with a context
 * tag.  Direct contexts never use the tag for rendering, but Mesa
 * records it; hand out a nonzero tag when a context is bound.
 */
static int y11_glx_make_current(struct y11_client *c, const uint8_t *pkt,
                                size_t len, size_t data_off)
{
    uint32_t context;
    y11_shm_query_version_reply rep;    /* contextTag at byte 8 */
    uint32_t tag = 0;

    /* MakeCurrent: drawable(4), context(4), oldTag(4); context at +4. */
    if (len - data_off == 12u)
        context = y11_wire_get32(pkt + data_off + 4);
    else if (len - data_off == 16u)  /* MakeContextCurrent: tag first */
        context = y11_wire_get32(pkt + data_off + 12);
    else
        return y11_dispatch_bad_length(c, pkt[0]);

    if (context != 0) {
        tag = y11_glx_next_tag++;
        if (y11_glx_next_tag == 0)
            y11_glx_next_tag = 1;
    }

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;
    y11_wire_put32(&rep.hdr.length, 0);
    y11_wire_put32(&rep.uid, tag);     /* bytes 12-15 hold the tag */
    /* byte 8 (majorVersion slot) stays 0: reply is all pads + tag. */
    {
        uint8_t r[32];

        memset(r, 0, sizeof(r));
        r[0] = 1;
        y11_wire_put32(r + 8, tag);
        y11_dispatch_send_reply(c, r, sizeof(r));
    }
    return 0;
}

/* IsDirect (6): direct rendering through DRI3, always true. */
/*
 * IsDirect (6): always direct - rendering happens client side on the
 * DRI3 render node.  xcb (what modern Mesa parses) reads is_direct
 * from byte 8, behind the length field; the legacy Xlib struct keeps
 * it at byte 1.  Fill both.
 */
static int y11_glx_is_direct(struct y11_client *c, const uint8_t *pkt,
                            size_t len, size_t data_off)
{
    uint8_t rep[32];

    (void)data_off;
    if (len != 8u)
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;
    rep[1] = 1;                  /* isDirect (xGLXIsDirectReply) */
    rep[8] = 1;                  /* is_direct (xcb_glx_is_direct_reply_t) */
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/* QueryContext (25): report no attributes. */
static int y11_glx_query_context(struct y11_client *c, const uint8_t *pkt,
                                 size_t len, size_t data_off)
{
    uint8_t rep[32];

    (void)data_off;
    if (len != 8u)
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/*
 * GetDrawableAttributes (29): plain windows carry no GLX attributes,
 * so the reply lists zero of them (num_attribs stays at byte 8).
 */
static int y11_glx_get_drawable_attributes(struct y11_client *c,
                                           const uint8_t *pkt, size_t len,
                                           size_t data_off)
{
    uint8_t rep[32];

    (void)data_off;
    if (len != 8u)              /* drawable */
        return y11_dispatch_bad_length(c, pkt[0]);

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/* ---- the dispatcher -------------------------------------------------------------- */

int y11_glx_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                size_t data_off)
{
    switch (pkt[1]) {
    case Y11_GLX_QUERY_VERSION:
        return y11_glx_query_version(c, pkt, len, data_off);
    case Y11_GLX_GET_VISUAL_CONFIGS:
        return y11_glx_get_visual_configs(c, pkt, len, data_off);
    case Y11_GLX_GET_FB_CONFIGS:
        return y11_glx_get_fb_configs(c, pkt, len, data_off);
    case Y11_GLX_QUERY_SERVER_STRING:
        return y11_glx_query_server_string(c, pkt, len, data_off);
    case Y11_GLX_MAKE_CURRENT:
    case Y11_GLX_MAKE_CONTEXT_CURRENT:
        return y11_glx_make_current(c, pkt, len, data_off);
    case Y11_GLX_IS_DIRECT:
        return y11_glx_is_direct(c, pkt, len, data_off);
    case Y11_GLX_QUERY_CONTEXT:
        return y11_glx_query_context(c, pkt, len, data_off);
    case Y11_GLX_GET_DRAWABLE_ATTRIBUTES:
        return y11_glx_get_drawable_attributes(c, pkt, len, data_off);
    case Y11_GLX_CREATE_CONTEXT:
    case Y11_GLX_CREATE_NEW_CONTEXT:
    case Y11_GLX_CREATE_CONTEXT_ATTRIBS_ARB:
        y11_glx_ctx_create(c, y11_wire_get32(pkt + data_off));
        return 0;               /* no reply */
    case Y11_GLX_DESTROY_CONTEXT:
        return 0;               /* no reply; purged at disconnect */
    case Y11_GLX_CLIENT_INFO:
    case Y11_GLX_SET_CLIENT_INFO_ARB:
    case Y11_GLX_SET_CLIENT_INFO2_ARB:
    case Y11_GLX_SWAP_BUFFERS:
    case Y11_GLX_CREATE_WINDOW:
    case Y11_GLX_DESTROY_WINDOW:
    case Y11_GLX_CHANGE_DRAWABLE_ATTRIBUTES:
    case Y11_GLX_RENDER:
        return 0;               /* no reply needed for the bridge */
    default:
        y11_dispatch_send_error(c, Y11_ERR_BAD_REQUEST, pkt[1], pkt[0]);
        return 0;
    }
}
