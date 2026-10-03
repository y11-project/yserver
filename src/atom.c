/*
 * atom.c - Atom subsystem for the Y11 display server.
 *
 * Atoms 1-68 are the predefined X11 core atoms (Xatom.h order) and are
 * held in a static array.  Atoms 69 and up are dynamic and live in a
 * simple chained hash table.  InternAtom (opcode 16) and GetAtomName
 * (opcode 17) are implemented here.
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* ---- predefined atoms 1-68 (Xatom.h order) -------------------------------- */

#define Y11_PREDEF_ATOM_COUNT 68

static const char *const y11_predef_atoms[Y11_PREDEF_ATOM_COUNT + 1] = {
    NULL,                       /* 0: None */
    "PRIMARY",                  /* 1 */
    "SECONDARY",                /* 2 */
    "ARC",                      /* 3 */
    "ATOM",                     /* 4 */
    "BITMAP",                   /* 5 */
    "CARDINAL",                 /* 6 */
    "COLORMAP",                 /* 7 */
    "CURSOR",                   /* 8 */
    "CUT_BUFFER0",              /* 9 */
    "CUT_BUFFER1",              /* 10 */
    "CUT_BUFFER2",              /* 11 */
    "CUT_BUFFER3",              /* 12 */
    "CUT_BUFFER4",              /* 13 */
    "CUT_BUFFER5",              /* 14 */
    "CUT_BUFFER6",              /* 15 */
    "CUT_BUFFER7",              /* 16 */
    "DRAWABLE",                 /* 17 */
    "FONT",                     /* 18 */
    "INTEGER",                  /* 19 */
    "PIXMAP",                   /* 20 */
    "POINT",                    /* 21 */
    "RECTANGLE",                /* 22 */
    "RESOURCE_MANAGER",         /* 23 */
    "RGB_COLOR_MAP",            /* 24 */
    "RGB_BEST_MAP",             /* 25 */
    "RGB_BLUE_MAP",             /* 26 */
    "RGB_DEFAULT_MAP",          /* 27 */
    "RGB_GRAY_MAP",             /* 28 */
    "RGB_GREEN_MAP",            /* 29 */
    "RGB_RED_MAP",              /* 30 */
    "STRING",                   /* 31 */
    "VISUALID",                 /* 32 */
    "WINDOW",                   /* 33 */
    "WM_COMMAND",               /* 34 */
    "WM_HINTS",                 /* 35 */
    "WM_CLIENT_MACHINE",        /* 36 */
    "WM_ICON_NAME",             /* 37 */
    "WM_ICON_SIZE",             /* 38 */
    "WM_NAME",                  /* 39 */
    "WM_NORMAL_HINTS",          /* 40 */
    "WM_SIZE_HINTS",            /* 41 */
    "WM_ZOOM_HINTS",            /* 42 */
    "MIN_SPACE",                /* 43 */
    "NORM_SPACE",               /* 44 */
    "MAX_SPACE",                /* 45 */
    "END_SPACE",                /* 46 */
    "SUPERSCRIPT_X",            /* 47 */
    "SUPERSCRIPT_Y",             /* 48 */
    "SUBSCRIPT_X",              /* 49 */
    "SUBSCRIPT_Y",              /* 50 */
    "UNDERLINE_POSITION",       /* 51 */
    "UNDERLINE_THICKNESS",      /* 52 */
    "STRIKEOUT_ASCENT",         /* 53 */
    "STRIKEOUT_DESCENT",        /* 54 */
    "ITALIC_ANGLE",             /* 55 */
    "X_HEIGHT",                 /* 56 */
    "QUAD_WIDTH",               /* 57 */
    "WEIGHT",                   /* 58 */
    "POINT_SIZE",               /* 59 */
    "RESOLUTION",               /* 60 */
    "COPYRIGHT",                /* 61 */
    "NOTICE",                   /* 62 */
    "FONT_NAME",                /* 63 */
    "FAMILY_NAME",              /* 64 */
    "FULL_NAME",                /* 65 */
    "CAP_HEIGHT",               /* 66 */
    "WM_CLASS",                 /* 67 */
    "WM_TRANSIENT_FOR"          /* 68 */
};

/* ---- dynamic atom hash table (IDs 69 and up) ------------------------------- */

#define Y11_ATOM_HASH_BUCKETS 128u

struct y11_atom_node {
    char *name;                 /* owned, NUL-terminated copy */
    yid_t id;
    struct y11_atom_node *next;
};

static struct y11_atom_node *y11_atom_buckets[Y11_ATOM_HASH_BUCKETS];
static yid_t y11_atom_next_id = Y11_PREDEF_ATOM_COUNT + 1;  /* 69 */

static uint32_t y11_atom_hash(const char *name, size_t len)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= (uint32_t)(uint8_t)name[i];
        h *= 16777619u;
    }
    return h;
}

static int y11_atom_name_eq(const char *a, size_t alen,
                            const char *b, size_t blen)
{
    return alen == blen && memcmp(a, b, alen) == 0;
}

int y11_atom_init(void)
{
    return 0;                   /* static tables need no setup */
}

void y11_atom_shutdown(void)
{
    unsigned i;

    for (i = 0; i < Y11_ATOM_HASH_BUCKETS; i++) {
        struct y11_atom_node *n = y11_atom_buckets[i];
        while (n != NULL) {
            struct y11_atom_node *next = n->next;
            free(n->name);
            free(n);
            n = next;
        }
        y11_atom_buckets[i] = NULL;
    }
    y11_atom_next_id = Y11_PREDEF_ATOM_COUNT + 1;
}

/*
 * Return the atom id for name, creating a dynamic atom (69+) unless
 * only_if_exists is true.  Returns None (0) when not found and
 * only_if_exists is set, or on allocation failure.
 */
yid_t y11_atom_intern(const char *name, size_t len, int only_if_exists)
{
    yid_t id;
    uint32_t bucket;
    struct y11_atom_node *n;
    char *copy;

    /* Predefined atoms 1-68. */
    for (id = 1; id <= Y11_PREDEF_ATOM_COUNT; id++) {
        if (y11_atom_name_eq(y11_predef_atoms[id],
                             strlen(y11_predef_atoms[id]), name, len))
            return id;
    }

    /* Dynamic atoms: hash table. */
    bucket = y11_atom_hash(name, len) % Y11_ATOM_HASH_BUCKETS;
    for (n = y11_atom_buckets[bucket]; n != NULL; n = n->next) {
        if (y11_atom_name_eq(n->name, strlen(n->name), name, len))
            return n->id;
    }

    if (only_if_exists)
        return 0;               /* None */

    copy = malloc(len + 1);
    if (copy == NULL)
        return 0;
    memcpy(copy, name, len);
    copy[len] = '\0';

    n = malloc(sizeof(*n));
    if (n == NULL) {
        free(copy);
        return 0;
    }
    n->name = copy;
    n->id = y11_atom_next_id++;
    n->next = y11_atom_buckets[bucket];
    y11_atom_buckets[bucket] = n;
    return n->id;
}

/* Return the name of atom, or NULL when the atom does not exist. */
const char *y11_atom_name(yid_t atom)
{
    unsigned i;

    if (atom == 0)
        return NULL;
    if (atom <= Y11_PREDEF_ATOM_COUNT)
        return y11_predef_atoms[atom];

    for (i = 0; i < Y11_ATOM_HASH_BUCKETS; i++) {
        struct y11_atom_node *n;
        for (n = y11_atom_buckets[i]; n != NULL; n = n->next) {
            if (n->id == atom)
                return n->name;
        }
    }
    return NULL;
}

/* ---- InternAtom (opcode 16) -------------------------------------------------

 *   1   16                opcode
 *   1   only-if-exists
 *   2   request length
 *   2   length of name (n)
 *   2   unused
 *   n   name
 *   p   unused, p = pad(n)
 */
int y11_atom_req_intern(struct y11_client *c, const uint8_t *pkt,
                        size_t len, size_t data_off)
{
    y11_intern_atom_reply rep;
    int only_if_exists = pkt[1];
    uint16_t name_len;
    const char *name;
    yid_t atom;

    if (len - data_off < 4u)    /* name length + unused */
        goto badlength;
    name_len = y11_wire_get16(pkt + data_off);
    if ((len - data_off) - 4u < y11_wire_pad4(name_len))
        goto badlength;
    name = (const char *)pkt + data_off + 4;

    atom = y11_atom_intern(name, name_len, only_if_exists);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put32(&rep.atom, atom);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}

/* ---- GetAtomName (opcode 17) -------------------------------------------------

 *   1   17                opcode
 *   1   unused
 *   2   request length
 *   4   atom
 */
int y11_atom_req_get_name(struct y11_client *c, const uint8_t *pkt,
                          size_t len, size_t data_off)
{
    y11_get_atom_name_reply rep;
    static const uint8_t pad[4] = { 0, 0, 0, 0 };
    const char *name;
    size_t nlen, padded;
    yid_t atom;

    (void)pkt;
    if (len - data_off != 4u)
        goto badlength;
    atom = y11_wire_get32(pkt + data_off);

    name = y11_atom_name(atom);
    if (name == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ATOM, atom, pkt[0]);
        return 0;
    }

    nlen = strlen(name);
    padded = y11_wire_pad4((uint32_t)nlen);

    memset(&rep, 0, sizeof(rep));
    rep.hdr.type = 1;           /* X_Reply */
    y11_wire_put32(&rep.hdr.length, padded / 4u);
    y11_wire_put16(&rep.name_len, (uint16_t)nlen);

    y11_dispatch_send_reply(c, &rep, sizeof(rep));
    y11_client_send(c, name, nlen);
    if (padded > nlen)
        y11_client_send(c, pad, padded - nlen);
    return 0;

badlength:
    y11_dispatch_send_error(c, Y11_ERR_BAD_LENGTH, 0, pkt[0]);
    return 0;
}
