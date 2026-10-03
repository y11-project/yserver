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
    Y11_REQ_CREATE_WINDOW        = 1,
    Y11_REQ_CHANGE_WINDOW_ATTRIBUTES = 2,
    Y11_REQ_INTERN_ATOM          = 16,
    Y11_REQ_GET_ATOM_NAME        = 17,
    Y11_REQ_GET_PROPERTY         = 20,
    Y11_REQ_LIST_PROPERTIES      = 21,
    Y11_REQ_SET_SELECTION_OWNER  = 22,
    Y11_REQ_GET_INPUT_FOCUS      = 43,
    Y11_REQ_GET_FONT_PATH        = 52,
    Y11_REQ_CREATE_PIXMAP        = 53,
    Y11_REQ_FREE_PIXMAP          = 54,
    Y11_REQ_CREATE_GC            = 55,
    Y11_REQ_CHANGE_GC            = 56,
    Y11_REQ_FREE_GC              = 60,
    Y11_REQ_QUERY_EXTENSION      = 98,
    Y11_REQ_GET_KEYBOARD_CONTROL = 103,
    Y11_REQ_GET_POINTER_CONTROL  = 106,
    Y11_REQ_GET_SCREEN_SAVER     = 108,
    Y11_REQ_NO_OPERATION         = 127
};

/* ---- error codes (numeric values per the X11 wire standard) ------------ */

enum y11_error_code {
    Y11_ERR_BAD_REQUEST        = 1,
    Y11_ERR_BAD_VALUE          = 2,
    Y11_ERR_BAD_ATOM           = 5,
    Y11_ERR_BAD_ALLOC          = 11,
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
};

/* ---- top-level server object ------------------------------------------- */

struct y11_server {
    int listen_fd;              /* listener on socket_path */
    struct y11_client *clients[Y11_MAX_CLIENTS];
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

/* ---- src/dispatch.c ----------------------------------------------------- */

int  y11_dispatch_req(struct y11_client *c, const uint8_t *pkt, size_t len);
void y11_dispatch_send_reply(struct y11_client *c, void *rep, size_t len);
void y11_dispatch_send_error(struct y11_client *c, uint8_t code,
                             uint32_t resource_id, uint8_t major_opcode);

/* ---- src/atom.c --------------------------------------------------------- */

int  y11_atom_init(void);
void y11_atom_shutdown(void);
yid_t y11_atom_intern(const char *name, size_t len, int only_if_exists);
const char *y11_atom_name(yid_t atom);
int  y11_atom_req_intern(struct y11_client *c, const uint8_t *pkt,
                         size_t len, size_t data_off);
int  y11_atom_req_get_name(struct y11_client *c, const uint8_t *pkt,
                           size_t len, size_t data_off);

#endif /* Y11_H */
