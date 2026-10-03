/*
 * y11_wire.h - Binary wire structures for the X11 wire protocol.
 *
 * Every structure below is laid out exactly as specified by the X11 wire
 * protocol.  All members are naturally aligned, so the structures need no
 * compiler packing extensions (strict ISO C99) and may be used directly
 * over 4-byte-aligned buffers.  Multi-byte fields are always accessed
 * through the little-endian helpers at the bottom of this file, making
 * the server independent of host byte order.
 */

#ifndef Y11_WIRE_H
#define Y11_WIRE_H

#include <stdint.h>

/* ---- generic request / reply framing ----------------------------------- */

typedef struct {
    uint8_t  opcode;            /* major opcode of the request */
    uint8_t  pad0;              /* opcode-specific (Bool, minor opcode, ...) */
    uint16_t length;            /* whole request in 4-byte units, incl. header */
} y11_req;                      /* 4 bytes */

typedef struct {
    uint8_t  type;              /* 1 = reply, 0 = error */
    uint8_t  pad0;              /* opcode-specific */
    uint16_t sequence;          /* sequence number, low 16 bits (bytes 2-3) */
    uint32_t length;            /* additional 4-byte units beyond 32 bytes */
} y11_reply_hdr;                /* 8 bytes */

/* BIG-REQUESTS extended header (bigreqsproto.h: xBigReq) */
typedef struct {
    uint8_t  opcode;
    uint8_t  pad0;
    uint16_t zero;              /* 0 in the length field marks a big request */
    uint32_t length;            /* true length in 4-byte units, incl. header */
} y11_big_req;                  /* 8 bytes */

/* ---- connection setup --------------------------------------------------- */

typedef struct {
    uint8_t  byte_order;        /* 'l' (0x6C) = LE, 'B' (0x42) = BE */
    uint8_t  pad0;
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t auth_proto_len;    /* authorization protocol name, in bytes */
    uint16_t auth_data_len;     /* authorization protocol data, in bytes */
    uint16_t pad1;
} y11_conn_setup_req;           /* 12 bytes */

typedef struct {
    uint8_t  success;           /* 1 = success, 0 = failure */
    uint8_t  pad0;
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t length;            /* additional setup info in 4-byte units */
} y11_conn_setup_prefix;        /* 8 bytes */

typedef struct {
    uint32_t release_number;
    uint32_t resource_id_base;
    uint32_t resource_id_mask;
    uint32_t motion_buffer_size;
    uint16_t vendor_len;        /* bytes in the vendor string */
    uint16_t max_request_size;  /* 4-byte units */
    uint8_t  num_screens;
    uint8_t  num_formats;
    uint8_t  image_byte_order;  /* 0 = LSBFirst */
    uint8_t  bitmap_bit_order;  /* 0 = LeastSignificant */
    uint8_t  bitmap_scanline_unit;   /* 32 */
    uint8_t  bitmap_scanline_pad;    /* 32 */
    uint8_t  min_keycode;       /* 8 */
    uint8_t  max_keycode;       /* 255 */
    uint32_t pad0;
} y11_conn_setup_info;          /* 32 bytes */

typedef struct {
    uint8_t  depth;
    uint8_t  bits_per_pixel;
    uint8_t  scanline_pad;
    uint8_t  pad0[5];
} y11_pixmap_format;            /* 8 bytes */

typedef struct {
    uint32_t root_window;
    uint32_t default_colormap;
    uint32_t white_pixel;
    uint32_t black_pixel;
    uint32_t current_input_mask;
    uint16_t width_in_pixels;
    uint16_t height_in_pixels;
    uint16_t width_in_mm;
    uint16_t height_in_mm;
    uint16_t min_installed_maps;
    uint16_t max_installed_maps;
    uint32_t root_visual;
    uint8_t  backing_stores;
    uint8_t  save_unders;
    uint8_t  root_depth;
    uint8_t  allowed_depths;
} y11_screen_info;              /* 40 bytes */

typedef struct {
    uint8_t  depth;
    uint8_t  pad0;
    uint16_t visuals_count;
    uint32_t pad1;
} y11_depth_info;               /* 8 bytes */

typedef struct {
    uint32_t visual_id;
    uint8_t  class;             /* 4 = TrueColor */
    uint8_t  bits_per_rgb;
    uint16_t colormap_entries;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t pad0;
} y11_visual_type;              /* 24 bytes */

/* ---- replies (all carry the sequence number at bytes 2-3) --------------- */

typedef struct {
    y11_reply_hdr hdr;
    uint32_t atom;              /* bytes 8-11 */
    uint32_t pad0[5];
} y11_intern_atom_reply;        /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint16_t name_len;          /* bytes 8-9 */
    uint16_t pad0;              /* bytes 10-11 */
    uint32_t pad1[5];
} y11_get_atom_name_reply;      /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;          /* hdr.pad0 = format (0 when not found) */
    uint32_t property_type;     /* bytes 8-11: type ATOM (None when absent) */
    uint32_t bytes_after;       /* bytes 12-15 */
    uint32_t n_items;           /* bytes 16-19 */
    uint32_t pad0[3];
} y11_get_property_reply;       /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint16_t n_properties;      /* bytes 8-9 */
    uint16_t pad0;              /* bytes 10-11 */
    uint32_t pad1[5];
} y11_list_properties_reply;    /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;          /* hdr.pad0 = revert-to */
    uint32_t focus;             /* bytes 8-11 */
    uint32_t pad0[5];
} y11_get_input_focus_reply;    /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint16_t n_paths;           /* bytes 8-9 */
    uint16_t pad0;              /* bytes 10-11 */
    uint32_t pad1[5];
} y11_get_font_path_reply;      /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;          /* hdr.pad0 = global-auto-repeat */
    uint32_t led_mask;          /* bytes 8-11 */
    uint8_t  key_click_percent; /* byte 12 */
    uint8_t  bell_percent;      /* byte 13 */
    uint16_t bell_pitch;        /* bytes 14-15 */
    uint16_t bell_duration;     /* bytes 16-17 */
    uint16_t pad0;              /* bytes 18-19 */
    uint8_t  map[32];           /* auto-repeat bitmap, bytes 20-51 */
} y11_get_keyboard_control_reply;   /* 52 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint16_t accel_numerator;   /* bytes 8-9 */
    uint16_t accel_denominator; /* bytes 10-11 */
    uint16_t threshold;         /* bytes 12-13 */
    uint16_t pad0;              /* bytes 14-15 */
    uint32_t pad1[4];
} y11_get_pointer_control_reply;    /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint16_t timeout;           /* bytes 8-9 */
    uint16_t interval;          /* bytes 10-11 */
    uint8_t  prefer_blanking;   /* byte 12 */
    uint8_t  allow_exposures;   /* byte 13 */
    uint16_t pad0;              /* bytes 14-15 */
    uint32_t pad1[4];
} y11_get_screen_saver_reply;   /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint8_t  present;           /* byte 8 */
    uint8_t  major_opcode;      /* byte 9 */
    uint8_t  first_event;       /* byte 10 */
    uint8_t  first_error;       /* byte 11 */
    uint32_t pad0;              /* bytes 12-15 */
    uint16_t name_len;          /* bytes 16-17 (0: no name is echoed) */
    uint16_t pad1;              /* bytes 18-19 */
    uint32_t pad2[3];
} y11_query_extension_reply;    /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint32_t max_request_size;  /* bytes 8-11 */
    uint32_t pad0[5];
} y11_big_req_enable_reply;     /* 32 bytes */

typedef struct {
    uint8_t  type;              /* 0 = error */
    uint8_t  error_code;        /* byte 1 */
    uint16_t sequence;          /* bytes 2-3 */
    uint32_t resource_id;       /* bytes 4-7 */
    uint16_t minor_opcode;      /* bytes 8-9 */
    uint8_t  major_opcode;      /* byte 10 */
    uint8_t  pad0;              /* byte 11 */
    uint32_t pad1[5];
} y11_error;                    /* 32 bytes */

/* ---- little-endian wire accessors (host byte order independent) --------- */

static inline uint16_t y11_wire_get16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static inline uint32_t y11_wire_get32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline void y11_wire_put16(void *p, uint16_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void y11_wire_put32(void *p, uint32_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Round a byte count up to a 4-byte boundary (X11 "pad(p)"). */
static inline uint32_t y11_wire_pad4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

/* ---- compile-time layout checks (ISO C99) -------------------------------- */

typedef char y11_wire_chk_req[(sizeof(y11_req) == 4) ? 1 : -1];
typedef char y11_wire_chk_reply_hdr[(sizeof(y11_reply_hdr) == 8) ? 1 : -1];
typedef char y11_wire_chk_big_req[(sizeof(y11_big_req) == 8) ? 1 : -1];
typedef char y11_wire_chk_conn_setup_req[(sizeof(y11_conn_setup_req) == 12) ? 1 : -1];
typedef char y11_wire_chk_conn_setup_prefix[(sizeof(y11_conn_setup_prefix) == 8) ? 1 : -1];
typedef char y11_wire_chk_conn_setup_info[(sizeof(y11_conn_setup_info) == 32) ? 1 : -1];
typedef char y11_wire_chk_pixmap_format[(sizeof(y11_pixmap_format) == 8) ? 1 : -1];
typedef char y11_wire_chk_screen_info[(sizeof(y11_screen_info) == 40) ? 1 : -1];
typedef char y11_wire_chk_depth_info[(sizeof(y11_depth_info) == 8) ? 1 : -1];
typedef char y11_wire_chk_visual_type[(sizeof(y11_visual_type) == 24) ? 1 : -1];
typedef char y11_wire_chk_intern_atom_reply[(sizeof(y11_intern_atom_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_get_atom_name_reply[(sizeof(y11_get_atom_name_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_get_property_reply[(sizeof(y11_get_property_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_list_properties_reply[(sizeof(y11_list_properties_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_get_input_focus_reply[(sizeof(y11_get_input_focus_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_get_font_path_reply[(sizeof(y11_get_font_path_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_get_keyboard_control_reply[(sizeof(y11_get_keyboard_control_reply) == 52) ? 1 : -1];
typedef char y11_wire_chk_get_pointer_control_reply[(sizeof(y11_get_pointer_control_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_get_screen_saver_reply[(sizeof(y11_get_screen_saver_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_query_extension_reply[(sizeof(y11_query_extension_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_big_req_enable_reply[(sizeof(y11_big_req_enable_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_error[(sizeof(y11_error) == 32) ? 1 : -1];

#endif /* Y11_WIRE_H */
