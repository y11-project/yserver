/*
 * y11.h - Internal definitions for the Y11 display server daemon.
 *
 * The Y11 Project -- Phase 1: Headless Protocol Engine & Handshake.
 *
 * y11 is an ISO C99, POSIX-portable X11-compatible display server.  This
 * header defines the resource identifier type, the per-client connection
 * state and the top-level server object shared between the socket event
 * loop (src/main.c), the connection layer (src/client.c), the request
 * dispatcher (src/dispatch.c) and the atom subsystem (src/atom.c).
 */

#ifndef Y11_H
#define Y11_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Resource identifiers ("XID"s).  Client N owns the range
 * [base, base + mask] with base = (N << 20) | 0x00100000.
 */
typedef uint32_t yid_t;

/* ---- protocol identity ------------------------------------------------ */

#define Y11_VENDOR_STRING   "The Y11 Project"
#define Y11_RELEASE_NUMBER  110000u
#define Y11_PROTO_MAJOR     11
#define Y11_PROTO_MINOR     0

/* ---- connection limits ------------------------------------------------ */

#define Y11_MAX_CLIENTS        64
#define Y11_MAX_REQUEST_UNITS  65535u   /* advertised max request length, 4-byte units */
#define Y11_BIGREQ_MAX_UNITS   4194303u /* max 4-byte units once BIG-REQUESTS is enabled */
#define Y11_INBUF_MIN          16384u   /* initial request reassembly buffer size */
#define Y11_INBUF_MAX          ((size_t)Y11_BIGREQ_MAX_UNITS * 4u + 8u)
#define Y11_OUTBUF_MIN         4096u    /* initial pending-reply buffer size */

/* ---- resource id space ------------------------------------------------ */

#define Y11_RID_BASE_STEP   0x00100000u
#define Y11_RID_MASK        0x000FFFFFu

/* ---- BIG-REQUESTS extension ------------------------------------------- */

#define Y11_BIGREQ_NAME       "BIG-REQUESTS"
#define Y11_BIGREQ_EXT_OPCODE 128u      /* major opcode handed out for the extension */
#define Y11_XTEST_NAME        "XTEST"
#define Y11_XTEST_EXT_OPCODE  129u      /* major opcode handed out for the extension */
#define Y11_SHM_NAME          "MIT-SHM"
#define Y11_SHM_EXT_OPCODE    130u      /* major opcode handed out for the extension */
#define Y11_SHM_FIRST_EVENT    64u       /* ShmCompletion lands here */
#define Y11_SHM_FIRST_ERROR    128u      /* BadShmSeg lands here */
#define Y11_DRI3_NAME         "DRI3"
#define Y11_DRI3_EXT_OPCODE   131u      /* major opcode handed out for the extension */
#define Y11_PRESENT_NAME      "Present"
#define Y11_PRESENT_EXT_OPCODE 132u      /* major opcode handed out for the extension */
#define Y11_PRESENT_FIRST_EVENT 65u       /* Configure/Complete/Idle notify */
#define Y11_PRESENT_FIRST_ERROR 129u      /* Present errors land here */
#define Y11_XFIXES_NAME       "XFIXES"
#define Y11_XFIXES_EXT_OPCODE 145u      /* major opcode handed out for the extension */
#define Y11_GLX_NAME          "GLX"
#define Y11_GLX_EXT_OPCODE    143u      /* major opcode handed out for the extension */

/* ---- event type codes (numeric values per the X11 wire standard) ------- */

enum y11_event_type {
    Y11_EVT_KEY_PRESS             = 2,
    Y11_EVT_KEY_RELEASE           = 3,
    Y11_EVT_BUTTON_PRESS          = 4,
    Y11_EVT_BUTTON_RELEASE        = 5,
    Y11_EVT_MOTION_NOTIFY         = 6,
    Y11_EVT_ENTER_NOTIFY          = 7,
    Y11_EVT_LEAVE_NOTIFY          = 8,
    Y11_EVT_FOCUS_IN              = 9,
    Y11_EVT_FOCUS_OUT             = 10,
    Y11_EVT_EXPOSE              = 12,
    Y11_EVT_CREATE_NOTIFY      = 16,
    Y11_EVT_DESTROY_NOTIFY     = 17,
    Y11_EVT_UNMAP_NOTIFY       = 18,
    Y11_EVT_MAP_NOTIFY         = 19,
    Y11_EVT_MAP_REQUEST        = 20,
    Y11_EVT_REPARENT_NOTIFY    = 21,
    Y11_EVT_CONFIGURE_NOTIFY   = 22,
    Y11_EVT_CONFIGURE_REQUEST  = 23,
    Y11_EVT_CIRCULATE_NOTIFY   = 26,
    Y11_EVT_CIRCULATE_REQUEST  = 27
};

/* ---- event mask bits (numeric values per the X11 wire standard) -------- */

enum y11_event_mask_bit {
    Y11_MASK_KEY_PRESS             = 1u << 0,
    Y11_MASK_KEY_RELEASE           = 1u << 1,
    Y11_MASK_BUTTON_PRESS          = 1u << 2,
    Y11_MASK_BUTTON_RELEASE        = 1u << 3,
    Y11_MASK_ENTER_WINDOW          = 1u << 4,
    Y11_MASK_LEAVE_WINDOW          = 1u << 5,
    Y11_MASK_POINTER_MOTION        = 1u << 6,
    Y11_MASK_POINTER_MOTION_HINT   = 1u << 7,
    Y11_MASK_BUTTON1_MOTION        = 1u << 8,
    Y11_MASK_BUTTON2_MOTION        = 1u << 9,
    Y11_MASK_BUTTON3_MOTION        = 1u << 10,
    Y11_MASK_BUTTON4_MOTION        = 1u << 11,
    Y11_MASK_BUTTON5_MOTION        = 1u << 12,
    Y11_MASK_BUTTON_MOTION         = 1u << 13,
    Y11_MASK_KEYMAP_STATE          = 1u << 14,
    Y11_MASK_EXPOSURE              = 1u << 15,
    Y11_MASK_VISIBILITY_CHANGE     = 1u << 16,
    Y11_MASK_STRUCTURE_NOTIFY      = 1u << 17,
    Y11_MASK_RESIZE_REDIRECT       = 1u << 18,
    Y11_MASK_SUBSTRUCTURE_NOTIFY   = 1u << 19,
    Y11_MASK_SUBSTRUCTURE_REDIRECT = 1u << 20,
    Y11_MASK_FOCUS_CHANGE          = 1u << 21,
    Y11_MASK_PROPERTY_CHANGE       = 1u << 22,
    Y11_MASK_COLORMAP_CHANGE       = 1u << 23,
    Y11_MASK_OWNER_GRAB_BUTTON     = 1u << 24,
    Y11_MASK_ALL_VALID             = 0x01FFFFFFu
};

/* ---- ChangeWindowAttributes value-mask bits (X11 wire values) ---------- */

enum y11_cwa_bit {
    Y11_CWA_BACK_PIXMAP       = 1u << 0,
    Y11_CWA_BACK_PIXEL        = 1u << 1,
    Y11_CWA_BORDER_PIXMAP     = 1u << 2,
    Y11_CWA_BORDER_PIXEL      = 1u << 3,
    Y11_CWA_BIT_GRAVITY       = 1u << 4,
    Y11_CWA_WIN_GRAVITY       = 1u << 5,
    Y11_CWA_BACKING_STORE     = 1u << 6,
    Y11_CWA_BACKING_PLANES    = 1u << 7,
    Y11_CWA_BACKING_PIXEL     = 1u << 8,
    Y11_CWA_OVERRIDE_REDIRECT = 1u << 9,
    Y11_CWA_SAVE_UNDER        = 1u << 10,
    Y11_CWA_EVENT_MASK        = 1u << 11,
    Y11_CWA_DONT_PROPAGATE    = 1u << 12,
    Y11_CWA_COLORMAP          = 1u << 13,
    Y11_CWA_CURSOR            = 1u << 14,
    Y11_CWA_ALL_VALID         = 0x00007FFFu
};

/* ---- ConfigureWindow value-mask bits (X11 wire values) ----------------- */

enum y11_cw_bit {
    Y11_CW_X            = 1u << 0,
    Y11_CW_Y            = 1u << 1,
    Y11_CW_WIDTH        = 1u << 2,
    Y11_CW_HEIGHT       = 1u << 3,
    Y11_CW_BORDER_WIDTH = 1u << 4,
    Y11_CW_SIBLING      = 1u << 5,
    Y11_CW_STACK_MODE   = 1u << 6,
    Y11_CW_ALL_VALID    = 0x0000007Fu
};

/* window restack modes (X11 wire values) */
enum y11_stack_mode {
    Y11_STACK_ABOVE     = 0,
    Y11_STACK_BELOW     = 1,
    Y11_STACK_TOP_IF    = 2,
    Y11_STACK_BOTTOM_IF = 3,
    Y11_STACK_OPPOSITE  = 4
};

/* ---- resources ---------------------------------------------------------- */

enum y11_resource_type {
    Y11_RESOURCE_WINDOW = 1,
    Y11_RESOURCE_PIXMAP = 2,
    Y11_RESOURCE_GC     = 3,
    Y11_RESOURCE_SHMSEG = 4,
    Y11_RESOURCE_COLORMAP = 5,
    Y11_RESOURCE_FONT = 6,
    Y11_RESOURCE_CURSOR = 7
};

/* ---- drawables ------------------------------------------------------------ */

typedef enum {
    Y11_DRAWABLE_WINDOW = 1,
    Y11_DRAWABLE_PIXMAP = 2
} y11_drawable_type_t;

typedef struct y11_rect {
    int16_t  x, y;
    uint16_t width, height;
} y11_rect_t;

/*
 * A drawable is any target with a linear 32-bit pixel buffer: an
 * on-screen window or an off-screen pixmap.  Buffers are always stored
 * as 32bpp rows (stride = width * 4); the drawable depth only selects
 * the wire-format conversion at the PutImage/GetImage boundary.
 */
typedef struct y11_drawable {
    yid_t                id;
    y11_drawable_type_t  type;
    uint16_t             width;
    uint16_t             height;
    uint8_t              depth;
    uint8_t              bpp;        /* always 32 internally */
    size_t               stride;
    uint32_t            *pixels;     /* 32-bit XRGB linear buffer */
} y11_drawable_t;

/* Pixmaps encapsulate an off-screen buffer owned by one client. */
struct y11_pixmap {
    y11_drawable_t    base;
    struct y11_client *owner;
    bool              is_shm;    /* MIT-SHM pixmaps share client memory */
    bool              is_dri3;   /* DRI3 pixmaps share a DMA-BUF */
    struct y11_dri3_buffer *dri3;
};

/*
 * DRI3 buffer: a DMA-BUF imported (or exported) through DRM Prime,
 * wrapped for scanout or CPU compositing.
 */
typedef struct y11_dri3_buffer {
    int      prime_fd;     /* DMA-BUF file descriptor (-1 once imported) */
    uint32_t gem_handle;   /* kernel GEM handle from Prime import */
    uint32_t fb_id;        /* optional DRM framebuffer for scanout */
    uint32_t stride;       /* pitch in bytes */
    uint32_t size;         /* total buffer size in bytes */
    uint32_t format;       /* DRM fourcc, e.g. DRM_FORMAT_XRGB8888 */
    uint64_t modifier;     /* DRM format modifier (linear or tiled) */
    void     *map;         /* CPU mapping when MAP_DUMB succeeds */
    size_t   map_size;
} y11_dri3_buffer_t;

/*
 * Present flip: one queued presentation of a pixmap to a window at a
 * target media stream counter.
 */
typedef struct y11_present_flip {
    uint32_t                event_id;
    yid_t                   window_id;
    yid_t                   pixmap_id;
    uint64_t                target_msc;
    uint32_t                options;    /* Copy or Async */
    struct y11_present_flip *next;
} y11_present_flip_t;

/*
 * MIT-SHM segment: a SysV shared memory segment attached into the
 * server address space.  Clients register one per shmget(2) segment
 * and reference it from ShmPutImage, ShmGetImage and shared pixmaps.
 */
struct y11_shm_seg {
    yid_t            id;         /* client-assigned ShmSeg resource ID */
    struct y11_client *owner;
    int              shmid;     /* SysV IPC shmid */
    void             *addr;      /* shmat(2) result */
    size_t           size;      /* segment size from shmctl(IPC_STAT) */
    bool             read_only;
};

/* Graphics contexts hold the mutable rasterization state. */
struct y11_gc {
    yid_t            id;
    struct y11_client *owner;
    uint8_t          function;         /* GXcopy (3), GXxor (6), ... */
    uint32_t         plane_mask;       /* plane write mask */
    uint32_t         foreground;       /* XRGB color */
    uint32_t         background;       /* XRGB color */
    uint16_t         line_width;
    int16_t          clip_x_origin;
    int16_t          clip_y_origin;
    size_t           num_clip_rects;
    y11_rect_t      *clip_rects;       /* optional clipping boxes */
    uint8_t          subwindow_mode;   /* ClipByChildren (0) */
    uint8_t          depth;            /* drawable depth at creation */
};

/* ---- screen geometry (1920x1080 at ~96 dpi) ---------------------------- */

#define Y11_SCREEN_WIDTH       1920u
#define Y11_SCREEN_HEIGHT      1080u
#define Y11_SCREEN_MM_WIDTH    508u
#define Y11_SCREEN_MM_HEIGHT   285u
#define Y11_SCREEN_ROOT        ((yid_t)0x00000021u)
#define Y11_SCREEN_COLORMAP    ((yid_t)0x00000022u)
#define Y11_SCREEN_VISUAL      ((yid_t)0x00000020u)

/* ---- socket paths ------------------------------------------------------ */

#define Y11_SOCKET_DIR      "/tmp/.X11-unix"
#define Y11_LINK_DIR        "/tmp/.y11-unix"
#define Y11_SOCK_PATH_MAX   104     /* POSIX sun_path length (FreeBSD's) */

/* ---- request opcodes (numeric values per the X11 wire standard) -------- */

enum y11_req_opcode {
    Y11_REQ_CREATE_WINDOW            = 1,
    Y11_REQ_CHANGE_WINDOW_ATTRIBUTES = 2,
    Y11_REQ_GET_WINDOW_ATTRIBUTES    = 3,
    Y11_REQ_DESTROY_WINDOW           = 4,
    Y11_REQ_DESTROY_SUBWINDOWS       = 5,
    Y11_REQ_REPARENT_WINDOW          = 7,
    Y11_REQ_MAP_WINDOW               = 8,
    Y11_REQ_MAP_SUBWINDOWS           = 9,
    Y11_REQ_UNMAP_WINDOW             = 10,
    Y11_REQ_CONFIGURE_WINDOW         = 12,
    Y11_REQ_CIRCULATE_WINDOW         = 13,
    Y11_REQ_GET_GEOMETRY             = 14,
    Y11_REQ_QUERY_TREE               = 15,
    Y11_REQ_CHANGE_PROPERTY          = 18,
    Y11_REQ_DELETE_PROPERTY          = 19,
    Y11_REQ_INTERN_ATOM              = 16,
    Y11_REQ_GET_ATOM_NAME            = 17,
    Y11_REQ_GET_PROPERTY             = 20,
    Y11_REQ_LIST_PROPERTIES          = 21,
    Y11_REQ_SET_SELECTION_OWNER      = 22,
    Y11_REQ_SEND_EVENT               = 25,
    Y11_REQ_GRAB_POINTER             = 26,
    Y11_REQ_UNGRAB_POINTER           = 27,
    Y11_REQ_GRAB_BUTTON              = 28,
    Y11_REQ_UNGRAB_BUTTON            = 29,
    Y11_REQ_GRAB_KEYBOARD            = 31,
    Y11_REQ_UNGRAB_KEYBOARD          = 32,
    Y11_REQ_GRAB_KEY                 = 33,
    Y11_REQ_UNGRAB_KEY               = 34,
    Y11_REQ_ALLOW_EVENTS             = 35,
    Y11_REQ_SET_INPUT_FOCUS          = 42,
    Y11_REQ_GET_INPUT_FOCUS          = 43,
    Y11_REQ_QUERY_POINTER           = 38,
    Y11_REQ_WARP_POINTER            = 41,
    Y11_REQ_TRANSLATE_COORDS       = 40,
    Y11_REQ_GET_FONT_PATH            = 52,
    Y11_REQ_OPEN_FONT                = 45,
    Y11_REQ_CLOSE_FONT               = 46,
    Y11_REQ_QUERY_FONT               = 47,
    Y11_REQ_CREATE_CURSOR            = 93,
    Y11_REQ_CREATE_GLYPH_CURSOR      = 94,
    Y11_REQ_FREE_CURSOR              = 95,
    Y11_REQ_CREATE_PIXMAP            = 53,
    Y11_REQ_FREE_PIXMAP              = 54,
    Y11_REQ_CREATE_GC                = 55,
    Y11_REQ_CHANGE_GC                = 56,
    Y11_REQ_COPY_GC                  = 57,
    Y11_REQ_SET_CLIP_RECTANGLES      = 59,
    Y11_REQ_FREE_GC                  = 60,
    Y11_REQ_CLEAR_AREA               = 61,
    Y11_REQ_COPY_AREA                = 62,
    Y11_REQ_POLY_FILL_RECTANGLE      = 70,
    Y11_REQ_PUT_IMAGE                = 72,
    Y11_REQ_GET_IMAGE                = 73,
    Y11_REQ_QUERY_EXTENSION          = 98,
    Y11_REQ_QUERY_BEST_SIZE          = 97,
    Y11_REQ_LIST_EXTENSIONS          = 99,
    Y11_REQ_ALLOC_COLOR              = 84,
    Y11_REQ_ALLOC_NAMED_COLOR        = 85,
    Y11_REQ_FREE_COLORS              = 88,
    Y11_REQ_STORE_COLORS             = 89,
    Y11_REQ_STORE_NAMED_COLOR        = 90,
    Y11_REQ_QUERY_COLORS             = 91,
    Y11_REQ_CREATE_COLORMAP           = 78,
    Y11_REQ_COPY_COLORMAP_AND_FREE    = 79,
    Y11_REQ_FREE_COLORMAP            = 80,
    Y11_REQ_INSTALL_COLORMAP          = 81,
    Y11_REQ_UNINSTALL_COLORMAP        = 82,
    Y11_REQ_LIST_INSTALLED_COLORMAPS  = 83,
    Y11_REQ_LOOKUP_COLOR             = 92,
    Y11_REQ_CHANGE_KEYBOARD_MAPPING    = 100,
    Y11_REQ_GET_KEYBOARD_CONTROL     = 103,
    Y11_REQ_GET_MODIFIER_MAPPING     = 119,
    Y11_REQ_GET_KEYBOARD_MAPPING     = 101,
    Y11_REQ_GET_POINTER_CONTROL      = 106,
    Y11_REQ_SET_SCREEN_SAVER         = 107,
    Y11_REQ_GET_SCREEN_SAVER         = 108,
    Y11_REQ_FORCE_SCREEN_SAVER       = 115,
    Y11_REQ_NO_OPERATION             = 127
};

/*
 * Drawing requests accepted as no-ops: y11 is headless in phases 1-2,
 * but real clients (which render into pixmaps before they ever create
 * a window) must be able to issue them without a BadRequest error.
 */
enum y11_req_draw_opcode {
    Y11_REQ_COPY_PLANE           = 63,
    Y11_REQ_POLY_POINT           = 64,
    Y11_REQ_POLY_LINE            = 65,
    Y11_REQ_POLY_SEGMENT         = 66,
    Y11_REQ_POLY_RECTANGLE       = 67,
    Y11_REQ_POLY_ARC             = 68,
    Y11_REQ_FILL_POLY            = 69,
    Y11_REQ_POLY_FILL_ARC        = 71,
    Y11_REQ_POLY_TEXT8           = 74,
    Y11_REQ_POLY_TEXT16          = 75,
    Y11_REQ_IMAGE_TEXT8          = 76,
    Y11_REQ_IMAGE_TEXT16         = 77
};

/* ---- error codes (numeric values per the X11 wire standard) ------------ */

enum y11_error_code {
    Y11_ERR_BAD_REQUEST        = 1,
    Y11_ERR_BAD_VALUE          = 2,
    Y11_ERR_BAD_WINDOW         = 3,
    Y11_ERR_BAD_PIXMAP         = 4,
    Y11_ERR_BAD_ATOM           = 5,
    Y11_ERR_BAD_CURSOR         = 6,
    Y11_ERR_BAD_FONT           = 7,
    Y11_ERR_BAD_MATCH          = 8,
    Y11_ERR_BAD_DRAWABLE       = 9,
    Y11_ERR_BAD_ACCESS         = 10,
    Y11_ERR_BAD_ALLOC          = 11,
    Y11_ERR_BAD_COLORMAP       = 12,
    Y11_ERR_BAD_GCONTEXT       = 13,
    Y11_ERR_BAD_ID_CHOICE      = 14,
    Y11_ERR_BAD_NAME           = 15,
    Y11_ERR_BAD_LENGTH         = 16,
    Y11_ERR_BAD_IMPLEMENTATION = 17
};

/* ---- client connection states ------------------------------------------ */

enum y11_client_state {
    Y11_CLIENT_HANDSHAKE = 0,   /* reading the connection setup request */
    Y11_CLIENT_RUNNING   = 1    /* exchanging requests */
};

/* ---- per-client connection state --------------------------------------- */

struct y11_client {
    int fd;                     /* connected socket */
    int slot;                   /* index into y11_server.clients */
    int state;                  /* enum y11_client_state */
    int dead;                   /* socket EOF or fatal I/O error */
    int wants_close;            /* protocol failure: close after flushing */
    int big_requests;           /* BIG-REQUESTS enabled for this client */
    uint32_t sequence_number;   /* last request sequence seen */
    yid_t resource_id_base;     /* base of this client's resource id range */

    uint8_t *in_buf;            /* request reassembly ring buffer */
    size_t in_len;              /* bytes currently buffered */
    size_t in_cap;              /* allocated size of in_buf */

    uint8_t *out_buf;           /* pending reply data */
    size_t out_len;             /* unsent bytes in out_buf */
    size_t out_cap;             /* allocated size of out_buf */

    /* Ancillary file descriptors (SCM_RIGHTS). */
    int    in_fds[8];            /* received, in arrival order */
    size_t in_fd_count;
    int    out_fd_pending;      /* fd to attach to the next flush, or -1 */
    size_t out_fd_offset;       /* out_buf offset the fd belongs to */
};

/* ---- window map states and classes (X11 wire values) ------------------- */

typedef enum {
    Y11_MAP_STATE_UNMAPPED   = 0,
    Y11_MAP_STATE_UNVIEWABLE = 1,
    Y11_MAP_STATE_VIEWABLE   = 2
} y11_map_state_t;

typedef enum {
    Y11_WINDOW_CLASS_COPY_FROM_PARENT = 0,
    Y11_WINDOW_CLASS_INPUT_OUTPUT     = 1,
    Y11_WINDOW_CLASS_INPUT_ONLY       = 2
} y11_window_class_t;

/* ---- per-client event subscription -------------------------------------- */

struct y11_event_sub {
    struct y11_client    *client;   /* subscribed client */
    uint32_t              mask;     /* selected event mask bits */
    struct y11_event_sub *next;
};

/* ---- windows ------------------------------------------------------------ */

/*
 * Property storage: (name, type, format, data) attached to a window.
 */
struct y11_property {
    struct y11_window  *window;
    struct y11_property *next;
    yid_t              name;      /* property atom */
    yid_t              type;      /* type atom (None for untyped) */
    uint8_t            format;    /* 8, 16 or 32 */
    uint8_t            *data;
    size_t             size;      /* bytes */
};

/*
 * Pointer and keyboard state: root coordinates, pressed buttons, and the
 * window directly under the cursor.
 */
typedef struct y11_pointer {
    int16_t  root_x;
    int16_t  root_y;
    uint16_t button_mask;        /* Button1Mask .. Button5Mask */
    yid_t    focus_window;       /* window directly under cursor */
} y11_pointer_t;

/*
 * Keyboard state: pressed-key bitfield, modifier mask, and the input
 * focus (None 0, PointerRoot 1, or a window).
 */
typedef struct y11_keyboard {
    uint8_t  key_state[32];      /* 256-bit bitfield of pressed keys */
    uint16_t modifier_mask;      /* Shift, Lock, Control, Mod1-Mod5 */
    yid_t    focus_window;
    uint8_t  revert_to;          /* RevertToNone/PointerRoot/Parent */
} y11_keyboard_t;

struct y11_window {
    yid_t               id;
    struct y11_client  *owner;
    struct y11_window  *parent;
    struct y11_window  *first_child;    /* bottom of stacking order */
    struct y11_window  *last_child;     /* top of stacking order */
    struct y11_window  *prev_sibling;
    struct y11_window  *next_sibling;

    /* Geometry relative to parent */
    int16_t             x, y;
    uint16_t            width, height;
    uint16_t            border_width;

    /* Absolute screen-space coordinates */
    int32_t             abs_x, abs_y;

    /* Attributes */
    uint8_t             depth;
    yid_t               visual_id;
    y11_window_class_t  window_class;
    y11_map_state_t     map_state;
    bool                override_redirect;
    uint32_t            background_pixel;
    uint32_t            border_pixel;

    /* Event subscriptions */
    uint32_t            all_event_masks;    /* bitwise OR of all client masks */
    struct y11_event_sub *event_subs;       /* per-client event mask list */
    struct y11_client  *substructure_redirect_client;   /* active WM client */

    /* Attached properties (ChangeProperty and friends). */
    struct y11_property *props;

    /* Backing pixel buffer (InputOnly windows have none) */
    y11_drawable_t      drawable;
};

/* ---- top-level server object ------------------------------------------- */

struct y11_server {
    int listen_fd;              /* listener on socket_path */
    struct y11_client *clients[Y11_MAX_CLIENTS];
    int seat_fd;                /* libseat connection, or -1 when headless */
    int drm_fd;                 /* DRM card node, or -1 when headless */
    char socket_path[Y11_SOCK_PATH_MAX];    /* primary socket, e.g. /tmp/.X11-unix/X0 */
    char link_path[Y11_SOCK_PATH_MAX];      /* alias symlink, e.g. /tmp/.y11-unix/Y0 */
};

/* ---- debug flag (set from $Y11_DEBUG in main.c) ------------------------ */

extern int y11_debug;

/* ---- src/main.c --------------------------------------------------------- */

int  y11_server_init(struct y11_server *srv, unsigned display);
void y11_server_run(struct y11_server *srv);
void y11_server_shutdown(struct y11_server *srv);

/* ---- src/client.c ------------------------------------------------------- */

struct y11_client *y11_client_create(int fd, int slot);
void y11_client_destroy(struct y11_server *srv, struct y11_client *c);
int  y11_client_read(struct y11_client *c);
int  y11_client_process(struct y11_client *c);
int  y11_client_flush(struct y11_client *c);
int  y11_client_send(struct y11_client *c, const void *data, size_t len);
int  y11_client_send_fd(struct y11_client *c, const void *data, size_t len,
                       int fd);
int  y11_client_pop_fd(struct y11_client *c);

/* ---- src/dispatch.c ----------------------------------------------------- */

int  y11_dispatch_req(struct y11_client *c, const uint8_t *pkt, size_t len);
void y11_dispatch_send_reply(struct y11_client *c, void *rep, size_t len);
void y11_dispatch_send_reply_fd(struct y11_client *c, void *rep, size_t len,
                                int fd);
void y11_dispatch_send_error(struct y11_client *c, uint8_t code,
                             uint32_t resource_id, uint8_t major_opcode);
int  y11_dispatch_bad_length(struct y11_client *c, uint8_t opcode);

/* ---- src/window.c -------------------------------------------------------- */

int  y11_window_init(void);
void y11_window_shutdown(void);
struct y11_window *y11_window_get(yid_t id);

/* Backing-buffer management (create, resize, free). */
int  y11_window_sync_drawable(struct y11_window *win);
void y11_window_free_drawable(struct y11_window *win);

int  y11_window_req_create(struct y11_client *c, const uint8_t *pkt,
                           size_t len, size_t data_off);
int  y11_window_req_change_attributes(struct y11_client *c, const uint8_t *pkt,
                                      size_t len, size_t data_off);
int  y11_window_req_get_attributes(struct y11_client *c, const uint8_t *pkt,
                                   size_t len, size_t data_off);
int  y11_window_req_destroy(struct y11_client *c, const uint8_t *pkt,
                            size_t len, size_t data_off);
int  y11_window_req_destroy_subwindows(struct y11_client *c, const uint8_t *pkt,
                                       size_t len, size_t data_off);
int  y11_window_req_reparent(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off);
int  y11_window_req_map(struct y11_client *c, const uint8_t *pkt,
                        size_t len, size_t data_off);
int  y11_window_req_map_subwindows(struct y11_client *c, const uint8_t *pkt,
                                   size_t len, size_t data_off);
int  y11_window_req_unmap(struct y11_client *c, const uint8_t *pkt,
                          size_t len, size_t data_off);
int  y11_window_req_configure(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off);
int  y11_window_req_get_geometry(struct y11_client *c, const uint8_t *pkt,
                                 size_t len, size_t data_off);
int  y11_window_req_clear_area(struct y11_client *c, const uint8_t *pkt,
                               size_t len, size_t data_off);
int  y11_window_req_circulate(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off);
int  y11_window_req_query_tree(struct y11_client *c, const uint8_t *pkt,
                               size_t len, size_t data_off);
void y11_window_destroy_owned(struct y11_client *c);

/* ---- src/events.c --------------------------------------------------------- */

/* Stamp the target's sequence into bytes 2-3 and queue the event. */
void y11_event_dispatch32(struct y11_client *target, void *event, size_t len);

void y11_event_send_create(struct y11_window *win);
void y11_event_send_destroy(struct y11_window *win);
void y11_event_send_map(struct y11_window *win);
void y11_event_send_unmap(struct y11_window *win, int from_configure);
void y11_event_send_reparent(struct y11_window *win,
                             struct y11_window *old_parent);
void y11_event_send_configure(struct y11_window *win);
void y11_event_send_expose(struct y11_window *win);
void y11_event_send_map_request(struct y11_window *win);
void y11_event_send_circulate_request(struct y11_window *parent,
                                      struct y11_window *child,
                                      uint8_t place);
void y11_event_send_circulate_notify(struct y11_window *win, uint8_t place);
void y11_event_send_configure_request(struct y11_window *win,
                                      uint32_t value_mask,
                                      int32_t x, int32_t y,
                                      uint32_t width, uint32_t height,
                                      uint32_t border_width,
                                      uint32_t sibling, uint8_t stack_mode);
void y11_events_purge_client(struct y11_client *c);

/* ---- src/resource.c -------------------------------------------------------- */

int   y11_resource_init(void);
void  y11_resource_shutdown(void);
int   y11_resource_add(yid_t id, int type, void *ptr);
void *y11_resource_get(yid_t id, int type);
void  y11_resource_remove(yid_t id);
void  y11_resource_purge_type(int type, struct y11_client *client,
                              int (*belongs)(void *ptr,
                                             struct y11_client *client),
                              void (*destroy)(void *ptr));
void  y11_resource_purge_type_arg(int type, void *arg,
                                  int (*belongs)(void *ptr, void *arg),
                                  void (*destroy)(void *ptr));

/* ---- src/pixmap.c ----------------------------------------------------------- */

int  y11_pixmap_req_create(struct y11_client *c, const uint8_t *pkt,
                           size_t len, size_t data_off);
int  y11_pixmap_req_free(struct y11_client *c, const uint8_t *pkt,
                         size_t len, size_t data_off);
void y11_pixmap_destroy(void *ptr);
void y11_pixmap_purge_client(struct y11_client *c);

/* ---- src/gc.c ------------------------------------------------------------------ */

int  y11_gc_req_create(struct y11_client *c, const uint8_t *pkt,
                       size_t len, size_t data_off);
int  y11_gc_req_change(struct y11_client *c, const uint8_t *pkt,
                       size_t len, size_t data_off);
int  y11_gc_req_copy(struct y11_client *c, const uint8_t *pkt,
                     size_t len, size_t data_off);
int  y11_gc_req_set_clip_rectangles(struct y11_client *c, const uint8_t *pkt,
                                    size_t len, size_t data_off);
int  y11_gc_req_free(struct y11_client *c, const uint8_t *pkt,
                     size_t len, size_t data_off);
void y11_gc_destroy(void *ptr);
void y11_gc_purge_client(struct y11_client *c);

/* ---- src/render.c ------------------------------------------------------------ */

y11_drawable_t *y11_drawable_lookup(yid_t id);
void y11_render_pixel_ex(y11_drawable_t *d, const struct y11_gc *gc,
                         size_t col, size_t row, uint32_t src);
int  y11_render_req_poly_fill_rectangle(struct y11_client *c,
                                        const uint8_t *pkt, size_t len,
                                        size_t data_off);
int  y11_render_req_copy_area(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off);
int  y11_render_req_put_image(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off);
int  y11_render_req_get_image(struct y11_client *c, const uint8_t *pkt,
                              size_t len, size_t data_off);

/* ---- src/damage.c ------------------------------------------------------------ */

void y11_damage_mapped(struct y11_window *win, int32_t x, int32_t y,
                       uint32_t w, uint32_t h);
void y11_damage_drawn(struct y11_drawable *d, int32_t x, int32_t y,
                      uint32_t w, uint32_t h);

/* ---- grabs ------------------------------------------------------------------ */

/*
 * Active grab state (pointer or keyboard): all device events are
 * diverted to the grabbing client's window until the grab ends.
 */
typedef struct y11_grab {
    bool              active;
    struct y11_client *client;
    yid_t             grab_window;
    yid_t             confine_to;     /* clamp the cursor inside, or 0 */
    yid_t             cursor_id;
    uint16_t          event_mask;
    bool              owner_events;
    uint8_t           pointer_mode;   /* Synchronous 0, Asynchronous 1 */
    uint8_t           keyboard_mode;
    bool              passive;       /* implicit grab from GrabButton/Key */
    uint32_t          time;
} y11_grab_t;

/* ---- src/grab.c ------------------------------------------------------------------ */

const y11_grab_t *y11_grab_pointer_active(void);
const y11_grab_t *y11_grab_keyboard_active(void);
struct y11_client *y11_grab_match_button(uint8_t button, uint16_t state);
struct y11_client *y11_grab_match_key(uint8_t key, uint16_t state);
void y11_grab_button_release_check(void);
void y11_grab_confine(int16_t *x, int16_t *y);
void y11_grab_deliver(const y11_grab_t *grab, uint8_t type, uint8_t detail,
                      uint32_t mask_bit);
int  y11_grab_req_pointer(struct y11_client *c, const uint8_t *pkt,
                          size_t len, size_t data_off);
int  y11_grab_req_ungrab_pointer(struct y11_client *c, const uint8_t *pkt,
                                 size_t len, size_t data_off);
int  y11_grab_req_button(struct y11_client *c, const uint8_t *pkt,
                         size_t len, size_t data_off);
int  y11_grab_req_ungrab_button(struct y11_client *c, const uint8_t *pkt,
                                size_t len, size_t data_off);
int  y11_grab_req_keyboard(struct y11_client *c, const uint8_t *pkt,
                           size_t len, size_t data_off);
int  y11_grab_req_ungrab_keyboard(struct y11_client *c, const uint8_t *pkt,
                                  size_t len, size_t data_off);
int  y11_grab_req_key(struct y11_client *c, const uint8_t *pkt,
                      size_t len, size_t data_off);
int  y11_grab_req_ungrab_key(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off);
void y11_grab_purge_client(struct y11_client *c);

/* ---- src/shm.c --------------------------------------------------------------------- */

int  y11_shm_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                 size_t data_off);
void y11_shm_purge_client(struct y11_client *c);
void y11_shm_purge_pixmaps(struct y11_shm_seg *seg);

/* ---- src/dri3.c --------------------------------------------------------------------- */

int  y11_dri3_init(int preferred_fd);
void y11_dri3_shutdown(void);
int  y11_dri3_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                  size_t data_off);
void y11_dri3_release_buffer(struct y11_dri3_buffer *buf);
int  y11_dri3_pixmap_cpu_map(struct y11_pixmap *p);

void y11_colormap_purge_client(struct y11_client *c);
void y11_font_purge_client(struct y11_client *c);
void y11_glx_purge_client(struct y11_client *c);
int  y11_glx_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                size_t data_off);

/* ---- src/property.c ---------------------------------------------------------------- */

int  y11_property_req_change(struct y11_client *c, const uint8_t *pkt,
                             size_t len, size_t data_off);
int  y11_property_req_get(struct y11_client *c, const uint8_t *pkt,
                         size_t len, size_t data_off);
int  y11_property_req_delete(struct y11_client *c, const uint8_t *pkt,
                            size_t len, size_t data_off);
int  y11_property_req_list(struct y11_client *c, const uint8_t *pkt,
                          size_t len, size_t data_off);
struct y11_property *y11_property_find(struct y11_window *win, yid_t name);
void y11_property_destroy_all(struct y11_window *win);
size_t format_bytes(uint8_t format);

/* Store a property on a window (used for the EWMH root properties). */
int y11_property_store(struct y11_window *win, yid_t name, yid_t type,
                      uint8_t format, const uint8_t *data, size_t size);

/* ---- src/present.c -------------------------------------------------------------------- */

int  y11_present_init(void);
void y11_present_shutdown(void);
int  y11_present_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                     size_t data_off);
void y11_present_purge_client(struct y11_client *c);

/* ---- misc ------------------------------------------------------------------------ */

unsigned y11_popcount32(uint32_t v);
uint32_t y11_input_event_time(void);

/* Screen geometry: 1920x1080 defaults, overridden by the hardware
 * output mode when DRM/KMS scanout is available. */
extern uint16_t y11_screen_width;
extern uint16_t y11_screen_height;

/* Scanout hooks (src/scanout.c): no-ops without hardware. */
void y11_scanout_mark_dirty(int32_t x, int32_t y, uint32_t w, uint32_t h);
void y11_scanout_move_cursor(int32_t x, int32_t y);

/* ---- src/input.c ----------------------------------------------------------------- */

int  y11_input_init(void);
void y11_input_shutdown(void);
void y11_input_purge_client(struct y11_client *c);
void y11_input_motion(int16_t dx, int16_t dy);
void y11_input_motion_abs(int16_t x, int16_t y);
void y11_input_button(int press, uint8_t button);
void y11_input_key(int press, uint8_t keycode);
const y11_pointer_t *y11_input_pointer(void);
const y11_keyboard_t *y11_input_keyboard(void);
int  y11_input_req_set_input_focus(struct y11_client *c, const uint8_t *pkt,
                                    size_t len, size_t data_off);
int  y11_input_req_get_keyboard_mapping(struct y11_client *c,
                                       const uint8_t *pkt, size_t len,
                                       size_t data_off);
int  y11_input_req_change_keyboard_mapping(struct y11_client *c,
                                           const uint8_t *pkt, size_t len,
                                           size_t data_off);

/* ---- src/events.c (hit-testing) --------------------------------------------------- */

struct y11_window *y11_window_at_point(int32_t x, int32_t y);

/* ---- src/atom.c --------------------------------------------------------- */

int  y11_atom_init(void);
void y11_atom_publish_root_properties(struct y11_window *root);
void y11_atom_shutdown(void);
yid_t y11_atom_intern(const char *name, size_t len, int only_if_exists);
const char *y11_atom_name(yid_t atom);
int  y11_atom_req_intern(struct y11_client *c, const uint8_t *pkt,
                         size_t len, size_t data_off);
int  y11_atom_req_get_name(struct y11_client *c, const uint8_t *pkt,
                           size_t len, size_t data_off);

#endif /* Y11_H */
