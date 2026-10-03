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

/* ---- window requests (opcodes 1, 2, 12) ----------------------------------
 *
 * Value lists hold exactly 4 bytes per set mask bit, packed in increasing
 * bit order (the X11 LISTofVALUE encoding).
 */

typedef struct {
    uint8_t  opcode;            /* 1 = CreateWindow */
    uint8_t  depth;             /* 0 = CopyFromParent */
    uint16_t length;
    uint32_t wid;               /* client-chosen resource id */
    uint32_t parent;
    int16_t  x, y;
    uint16_t width, height;
    uint16_t border_width;
    uint16_t class;             /* y11_window_class_t */
    uint32_t visual;
    uint32_t value_mask;
} y11_create_window_req;        /* 32 bytes + value list */

typedef struct {
    uint8_t  opcode;            /* 2 = ChangeWindowAttributes */
    uint8_t  pad0;
    uint16_t length;
    uint32_t window;
    uint32_t value_mask;
} y11_change_window_attributes_req;     /* 12 bytes + value list */

typedef struct {
    uint8_t  opcode;            /* 12 = ConfigureWindow */
    uint8_t  pad0;
    uint16_t length;
    uint32_t window;
    uint32_t value_mask;
    uint16_t pad0_[2];
} y11_configure_window_req;     /* 16 bytes + value list */

/* ---- replies --------------------------------------------------------------- */

typedef struct {
    y11_reply_hdr hdr;          /* hdr.pad0 = backing-store; length 3 */
    uint32_t visual;            /* bytes 8-11 */
    uint16_t class;             /* bytes 12-13 */
    uint8_t  bit_gravity;       /* byte 14 */
    uint8_t  win_gravity;       /* byte 15 */
    uint32_t backing_bit_planes;    /* bytes 16-19 */
    uint32_t backing_pixel;     /* bytes 20-23 */
    uint8_t  save_under;        /* byte 24 */
    uint8_t  map_installed;     /* byte 25 */
    uint8_t  map_state;         /* byte 26 */
    uint8_t  override;          /* byte 27 */
    uint32_t colormap;          /* bytes 28-31 */
    uint32_t all_event_masks;   /* bytes 32-35 */
    uint32_t your_event_mask;   /* bytes 36-39 */
    uint16_t do_not_propagate_mask;     /* bytes 40-41 */
    uint16_t pad0;              /* bytes 42-43 */
} y11_get_window_attributes_reply;      /* 44 bytes */

typedef struct {
    y11_reply_hdr hdr;          /* hdr.pad0 = depth */
    uint32_t root;              /* bytes 8-11 */
    int16_t  x, y;              /* bytes 12-15 */
    uint16_t width, height;     /* bytes 16-19 */
    uint16_t border_width;      /* bytes 20-21 */
    uint16_t pad0;              /* bytes 22-23 */
    uint32_t pad1[2];
} y11_get_geometry_reply;       /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint32_t root;              /* bytes 8-11 */
    uint32_t parent;            /* bytes 12-15 */
    uint16_t n_children;        /* bytes 16-17 */
    uint16_t pad0;              /* bytes 18-19 */
    uint32_t pad1[3];
} y11_query_tree_reply;         /* 32 bytes + child list */

typedef struct {
    y11_reply_hdr hdr;
    uint16_t red;                /* bytes 8-9 */
    uint16_t green;              /* bytes 10-11 */
    uint16_t blue;               /* bytes 12-13 */
    uint16_t pad0;               /* bytes 14-15 */
    uint32_t pixel;              /* bytes 16-19 */
    uint32_t pad1[3];
} y11_alloc_color_reply;        /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint32_t pixel;              /* bytes 8-11 */
    uint16_t exact_red;          /* bytes 12-13 */
    uint16_t exact_green;        /* bytes 14-15 */
    uint16_t exact_blue;         /* bytes 16-17 */
    uint16_t screen_red;        /* bytes 18-19 */
    uint16_t screen_green;      /* bytes 20-21 */
    uint16_t screen_blue;       /* bytes 22-23 */
    uint32_t pad0[2];
} y11_alloc_named_color_reply;  /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;
    uint16_t n_colors;           /* bytes 8-9 */
    uint16_t pad0;               /* bytes 10-11 */
    uint32_t pad1[5];
} y11_query_colors_reply;       /* 32 bytes + one 8-byte RGB item per color */

/* ---- rendering requests ------------------------------------------------- */

typedef struct {
    uint8_t  opcode;            /* 53 = CreatePixmap */
    uint8_t  depth;
    uint16_t length;
    uint32_t pid;
    uint32_t drawable;
    uint16_t width, height;
} y11_create_pixmap_req;        /* 16 bytes */

typedef struct {
    uint8_t  opcode;            /* 55 = CreateGC */
    uint8_t  pad0;
    uint16_t length;
    uint32_t gc;
    uint32_t drawable;
    uint32_t value_mask;
} y11_create_gc_req;            /* 16 bytes + value list */

typedef struct {
    uint8_t  opcode;            /* 56 = ChangeGC */
    uint8_t  pad0;
    uint16_t length;
    uint32_t gc;
    uint32_t value_mask;
} y11_change_gc_req;            /* 12 bytes + value list */

typedef struct {
    uint8_t  opcode;            /* 57 = CopyGC */
    uint8_t  pad0;
    uint16_t length;
    uint32_t src_gc;
    uint32_t dst_gc;
    uint32_t value_mask;
} y11_copy_gc_req;              /* 16 bytes */

typedef struct {
    uint8_t  opcode;            /* 59 = SetClipRectangles */
    uint8_t  ordering;          /* 0 Unsorted, 1 YXSorted, 2 YXBanded */
    uint16_t length;
    uint32_t gc;
    int16_t  clip_x_origin;
    int16_t  clip_y_origin;
} y11_set_clip_rectangles_req;  /* 12 bytes + 8-byte rectangles */

typedef struct {
    uint8_t  opcode;            /* 70 = PolyFillRectangle */
    uint8_t  pad0;
    uint16_t length;
    uint32_t gc;
    uint32_t drawable;
} y11_poly_fill_rectangle_req;  /* 12 bytes + 8-byte rectangles */

typedef struct {
    uint8_t  opcode;            /* 62 = CopyArea */
    uint8_t  pad0;
    uint16_t length;
    uint32_t src_drawable;
    uint32_t dst_drawable;
    uint32_t gc;
    int16_t  src_x, src_y;
    int16_t  dst_x, dst_y;
    uint16_t width, height;
} y11_copy_area_req;            /* 28 bytes */

typedef struct {
    uint8_t  opcode;            /* 72 = PutImage */
    uint8_t  format;            /* 0 Bitmap, 1 XYPixmap, 2 ZPixmap */
    uint16_t length;
    uint32_t drawable;
    uint32_t gc;
    uint16_t width, height;
    int16_t  dst_x, dst_y;
    uint8_t  left_pad;
    uint8_t  depth;
    uint16_t pad0;
} y11_put_image_req;            /* 24 bytes + image data */

typedef struct {
    uint8_t  opcode;            /* 73 = GetImage */
    uint8_t  format;
    uint16_t length;
    uint32_t drawable;
    int16_t  x, y;
    uint16_t width, height;
    uint32_t plane_mask;
} y11_get_image_req;            /* 20 bytes */

typedef struct {
    uint8_t  opcode;            /* 61 = ClearArea */
    uint8_t  exposures;         /* generate Expose when nonzero */
    uint16_t length;
    uint32_t window;
    int16_t  x, y;
    uint16_t width, height;     /* zero: to the window edge */
} y11_clear_area_req;           /* 16 bytes */
typedef struct {
    y11_reply_hdr hdr;          /* hdr.pad0 = depth */
    uint32_t visual;            /* bytes 8-11 */
    uint32_t pad0[5];
} y11_get_image_reply;          /* 32 bytes + image data */

typedef struct {
    y11_reply_hdr hdr;
    uint16_t exact_red;          /* bytes 8-9 */
    uint16_t exact_green;        /* bytes 10-11 */
    uint16_t exact_blue;         /* bytes 12-13 */
    uint16_t screen_red;         /* bytes 14-15 */
    uint16_t screen_green;      /* bytes 16-17 */
    uint16_t screen_blue;       /* bytes 18-19 */
    uint32_t pad0[3];
} y11_lookup_color_reply;       /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;          /* hdr.pad0 = same-screen */
    uint32_t child;             /* bytes 8-11 */
    int16_t  dst_x;             /* bytes 12-13 */
    int16_t  dst_y;             /* bytes 14-15 */
    uint32_t pad0[4];
} y11_translate_coords_reply;  /* 32 bytes */

typedef struct {
    y11_reply_hdr hdr;          /* hdr.pad0 = same-screen */
    uint32_t root;              /* bytes 8-11 */
    uint32_t child;             /* bytes 12-15 */
    int16_t  root_x;            /* bytes 16-17 */
    int16_t  root_y;            /* bytes 18-19 */
    int16_t  win_x;             /* bytes 20-21 */
    int16_t  win_y;             /* bytes 22-23 */
    uint16_t state;             /* bytes 24-25 */
    uint16_t pad0;              /* bytes 26-27 */
    uint32_t pad1;              /* bytes 28-31 */
} y11_query_pointer_reply;      /* 32 bytes */

/* ---- events (all exactly 32 bytes; type@0, detail@1, sequence@2-3) -------- */

typedef struct {
    uint32_t pad00;             /* type, detail, sequence */
    uint32_t window;            /* bytes 4-7 */
    int16_t  x, y;              /* bytes 8-11 */
    uint16_t width, height;     /* bytes 12-15 */
    uint16_t count;             /* bytes 16-17 */
    uint16_t pad0;              /* bytes 18-19 */
    uint32_t pad1[3];
} y11_expose_event;             /* 32 bytes */

typedef struct {
    uint32_t pad00;             /* type, detail, sequence */
    uint32_t parent;            /* bytes 4-7 */
    uint32_t window;            /* bytes 8-11 */
    int16_t  x, y;              /* bytes 12-15 */
    uint16_t width, height;     /* bytes 16-19 */
    uint16_t border_width;      /* bytes 20-21 */
    uint8_t  override;          /* byte 22 */
    uint8_t  pad0[1];
    uint32_t pad1[2];
} y11_create_notify_event;      /* 32 bytes */

typedef struct {
    uint32_t pad00;             /* type, detail, sequence */
    uint32_t event;             /* bytes 4-7 */
    uint32_t window;            /* bytes 8-11 */
    uint32_t pad0[5];
} y11_destroy_notify_event;     /* 32 bytes */

typedef struct {
    uint32_t pad00;             /* type, detail, sequence */
    uint32_t event;             /* bytes 4-7 */
    uint32_t window;            /* bytes 8-11 */
    uint8_t  from_configure;    /* byte 12 */
    uint8_t  pad0[3];
    uint32_t pad1[4];
} y11_unmap_notify_event;       /* 32 bytes */

typedef struct {
    uint32_t pad00;             /* type, detail, sequence */
    uint32_t event;             /* bytes 4-7 */
    uint32_t window;            /* bytes 8-11 */
    uint8_t  override;          /* byte 12 */
    uint8_t  pad0[3];
    uint32_t pad1[4];
} y11_map_notify_event;         /* 32 bytes */

typedef struct {
    uint32_t pad00;             /* type, detail, sequence */
    uint32_t parent;            /* bytes 4-7 */
    uint32_t window;            /* bytes 8-11 */
    uint32_t pad0[5];
} y11_map_request_event;        /* 32 bytes */

typedef struct {
    uint32_t pad00;             /* type, detail, sequence */
    uint32_t event;             /* bytes 4-7 */
    uint32_t window;            /* bytes 8-11 */
    uint32_t parent;            /* bytes 12-15 */
    int16_t  x, y;              /* bytes 16-19 */
    uint8_t  override;          /* byte 20 */
    uint8_t  pad0[3];
    uint32_t pad1[2];
} y11_reparent_notify_event;    /* 32 bytes */

typedef struct {
    uint32_t pad00;             /* type, detail, sequence */
    uint32_t event;             /* bytes 4-7 */
    uint32_t window;            /* bytes 8-11 */
    uint32_t above_sibling;     /* bytes 12-15 */
    int16_t  x, y;              /* bytes 16-19 */
    uint16_t width, height;     /* bytes 20-23 */
    uint16_t border_width;      /* bytes 24-25 */
    uint8_t  override;          /* byte 26 */
    uint8_t  pad0[1];
    uint32_t pad1;
} y11_configure_notify_event;   /* 32 bytes */

typedef struct {
    uint32_t pad00;             /* type, stack-mode@1, sequence@2-3 */
    uint32_t parent;            /* bytes 4-7 */
    uint32_t window;            /* bytes 8-11 */
    uint32_t sibling;           /* bytes 12-15 */
    int16_t  x, y;              /* bytes 16-19 */
    uint16_t width, height;     /* bytes 20-23 */
    uint16_t border_width;      /* bytes 24-25 */
    uint16_t value_mask;        /* bytes 26-27 */
    uint32_t pad0;              /* bytes 28-31 */
} y11_configure_request_event;  /* 32 bytes */

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
typedef char y11_wire_chk_create_window_req[(sizeof(y11_create_window_req) == 32) ? 1 : -1];
typedef char y11_wire_chk_change_window_attributes_req[(sizeof(y11_change_window_attributes_req) == 12) ? 1 : -1];
typedef char y11_wire_chk_configure_window_req[(sizeof(y11_configure_window_req) == 16) ? 1 : -1];
typedef char y11_wire_chk_get_window_attributes_reply[(sizeof(y11_get_window_attributes_reply) == 44) ? 1 : -1];
typedef char y11_wire_chk_get_geometry_reply[(sizeof(y11_get_geometry_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_query_tree_reply[(sizeof(y11_query_tree_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_expose_event[(sizeof(y11_expose_event) == 32) ? 1 : -1];
typedef char y11_wire_chk_create_notify_event[(sizeof(y11_create_notify_event) == 32) ? 1 : -1];
typedef char y11_wire_chk_destroy_notify_event[(sizeof(y11_destroy_notify_event) == 32) ? 1 : -1];
typedef char y11_wire_chk_unmap_notify_event[(sizeof(y11_unmap_notify_event) == 32) ? 1 : -1];
typedef char y11_wire_chk_map_notify_event[(sizeof(y11_map_notify_event) == 32) ? 1 : -1];
typedef char y11_wire_chk_map_request_event[(sizeof(y11_map_request_event) == 32) ? 1 : -1];
typedef char y11_wire_chk_reparent_notify_event[(sizeof(y11_reparent_notify_event) == 32) ? 1 : -1];
typedef char y11_wire_chk_configure_notify_event[(sizeof(y11_configure_notify_event) == 32) ? 1 : -1];
typedef char y11_wire_chk_configure_request_event[(sizeof(y11_configure_request_event) == 32) ? 1 : -1];
typedef char y11_wire_chk_alloc_color_reply[(sizeof(y11_alloc_color_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_alloc_named_color_reply[(sizeof(y11_alloc_named_color_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_lookup_color_reply[(sizeof(y11_lookup_color_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_query_colors_reply[(sizeof(y11_query_colors_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_query_pointer_reply[(sizeof(y11_query_pointer_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_translate_coords_reply[(sizeof(y11_translate_coords_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_create_pixmap_req[(sizeof(y11_create_pixmap_req) == 16) ? 1 : -1];
typedef char y11_wire_chk_create_gc_req[(sizeof(y11_create_gc_req) == 16) ? 1 : -1];
typedef char y11_wire_chk_change_gc_req[(sizeof(y11_change_gc_req) == 12) ? 1 : -1];
typedef char y11_wire_chk_copy_gc_req[(sizeof(y11_copy_gc_req) == 16) ? 1 : -1];
typedef char y11_wire_chk_set_clip_rectangles_req[(sizeof(y11_set_clip_rectangles_req) == 12) ? 1 : -1];
typedef char y11_wire_chk_poly_fill_rectangle_req[(sizeof(y11_poly_fill_rectangle_req) == 12) ? 1 : -1];
typedef char y11_wire_chk_copy_area_req[(sizeof(y11_copy_area_req) == 28) ? 1 : -1];
typedef char y11_wire_chk_put_image_req[(sizeof(y11_put_image_req) == 24) ? 1 : -1];
typedef char y11_wire_chk_get_image_req[(sizeof(y11_get_image_req) == 20) ? 1 : -1];
typedef char y11_wire_chk_get_image_reply[(sizeof(y11_get_image_reply) == 32) ? 1 : -1];
typedef char y11_wire_chk_clear_area_req[(sizeof(y11_clear_area_req) == 16) ? 1 : -1];

#endif /* Y11_WIRE_H */
