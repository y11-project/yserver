/*
 * xkb.c - Minimal XKEYBOARD (XKB) extension: the request set
 * libxkbcommon-x11 and libX11's XKB parser need (UseExtension,
 * GetState, GetMap, GetNames, GetControls, GetIndicatorMap,
 * GetDeviceInfo) to build a keymap from y11's core
 * keycode/keysym/modifier tables.  Wire layouts follow XKBproto.h
 * and xcb's xkb.h byte for byte.
 *
 * Parser requirements (all verified byte-level): GetMap must report
 * every required component bit (0xdf) with 0-based entry levels;
 * GetMap/GetNames virtualMods, GetNames indicators/groupNames and
 * GetIndicatorMap which must be nonzero (msb_pos(0) is undefined);
 * GetNames nTypes must match GetMap and cover the whole keycode
 * range; GetControls needs numGroups > 0 and the full 92-byte reply.
 *
 * Copyright (c) 2026 The Y11 Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

/* XKB request opcodes carried in body byte 1 (xcb_xkb.h). */
#define Y11_XKB_REQ_USE_EXTENSION     0u
#define Y11_XKB_REQ_SELECT_EVENTS     1u
#define Y11_XKB_REQ_BELL              3u
#define Y11_XKB_REQ_GET_STATE         4u
#define Y11_XKB_REQ_LATCH_LOCK_STATE  5u
#define Y11_XKB_REQ_GET_CONTROLS      6u
#define Y11_XKB_REQ_SET_CONTROLS      7u
#define Y11_XKB_REQ_GET_MAP           8u
#define Y11_XKB_REQ_GET_COMPAT_MAP    10u
#define Y11_XKB_REQ_GET_INDICATOR_MAP 13u
#define Y11_XKB_REQ_SET_INDICATOR_MAP 14u
#define Y11_XKB_REQ_GET_NAMES         17u
#define Y11_XKB_REQ_GET_DEVICE_INFO   24u

/* GetMap reply "present" bits (XCB_XKB_MAP_PART_*). */
#define Y11_XKB_MAP_KEY_TYPES        0x0001u
#define Y11_XKB_MAP_KEY_SYMS         0x0002u
#define Y11_XKB_MAP_MODIFIER_MAP     0x0004u
#define Y11_XKB_MAP_EXPLICIT         0x0008u
#define Y11_XKB_MAP_KEY_ACTIONS      0x0010u
#define Y11_XKB_MAP_VIRTUAL_MODS     0x0040u
#define Y11_XKB_MAP_VIRTUAL_MOD_MAP  0x0080u
/* libxkbcommon refuses the map unless every one of these is present. */
#define Y11_XKB_MAP_PRESENT          (Y11_XKB_MAP_KEY_TYPES | \
                                      Y11_XKB_MAP_KEY_SYMS | \
                                      Y11_XKB_MAP_MODIFIER_MAP | \
                                      Y11_XKB_MAP_EXPLICIT | \
                                      Y11_XKB_MAP_KEY_ACTIONS | \
                                      Y11_XKB_MAP_VIRTUAL_MODS | \
                                      Y11_XKB_MAP_VIRTUAL_MOD_MAP)

/*
 * y11 serves keycodes 8..255 with one keyboard group and two levels
 * (base + shifted) per key, exactly like its core GetKeyboardMapping.
 * The key types are the four libX11 requires (XkbNumRequiredTypes = 4):
 * a smaller type table makes XkbAllocClientMap reject the whole map.
 */
#define Y11_XKB_MIN_KEYCODE   8u
#define Y11_XKB_MAX_KEYCODE   255u
#define Y11_XKB_NUM_KEYS      ((size_t)(Y11_XKB_MAX_KEYCODE - \
                                        Y11_XKB_MIN_KEYCODE + 1u))
#define Y11_XKB_NUM_GROUPS    1u      /* one keyboard group */
#define Y11_XKB_NUM_LEVELS    2u      /* TWO_LEVEL: base + shifted */
#define Y11_KEYSYM_LEVELS_PER_KEY 2   /* matches input.c's table layout */

/*
 * The four standard key types every client expects, in canonical order:
 *   0 ONE_LEVEL, 1 TWO_LEVEL, 2 ALPHABETIC, 3 KEYPAD.
 * Entry levels are 0-based (level 1 = the shifted level).
 */
static const uint8_t y11_xkb_std_types[] = {
    /* ONE_LEVEL: mask 0, real 0, vmods 0, levels 1, 0 entries */
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    /* TWO_LEVEL: mask Shift, real Shift, levels 2, 1 entry */
    0x01, 0x01, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00,
    /* entry: active, mask Shift, level 1, real Shift, vmods 0, pad */
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    /* ALPHABETIC: mask Shift|Lock, real Shift|Lock, levels 2, 2 entries */
    0x03, 0x03, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00,
    /* entry: active, mask Lock, level 1, real Lock */
    0x01, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
    /* entry: active, mask Shift, level 1, real Shift */
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    /* KEYPAD: mask Shift, real Shift, levels 2, 1 entry */
    0x01, 0x01, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00,
    /* entry: active, mask Shift, level 1, real Shift */
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00
};
#define Y11_XKB_STD_TYPES_BYTES  (sizeof(y11_xkb_std_types))
#define Y11_XKB_NUM_TYPES         4u

/*
 * Four virtual modifiers, all unnamed (atom None) and unmapped, exist
 * only so libxkbcommon's msb_pos(reply->virtualMods) never sees zero.
 */
#define Y11_XKB_VMODS_MASK    0x000fu
#define Y11_XKB_VMODS_COUNT   4u

/* GetNames "which" bits (XCB_XKB_NAME_DETAIL_*). */
#define Y11_XKB_NAME_KEYCODES        0x0001u
#define Y11_XKB_NAME_SYMBOLS         0x0004u
#define Y11_XKB_NAME_TYPES           0x0010u
#define Y11_XKB_NAME_COMPAT          0x0020u
#define Y11_XKB_NAME_KEY_TYPE_NAMES  0x0040u
#define Y11_XKB_NAME_KT_LEVEL_NAMES  0x0080u
#define Y11_XKB_NAME_INDICATOR_NAMES 0x0100u
#define Y11_XKB_NAME_KEY_NAMES       0x0200u
#define Y11_XKB_NAME_VIRTUAL_MODS    0x0800u
#define Y11_XKB_NAME_GROUP_NAMES     0x1000u
#define Y11_XKB_NAME_PRESENT         (Y11_XKB_NAME_KEYCODES | \
                                      Y11_XKB_NAME_SYMBOLS | \
                                      Y11_XKB_NAME_TYPES | \
                                      Y11_XKB_NAME_COMPAT | \
                                      Y11_XKB_NAME_KEY_TYPE_NAMES | \
                                      Y11_XKB_NAME_KT_LEVEL_NAMES | \
                                      Y11_XKB_NAME_INDICATOR_NAMES | \
                                      Y11_XKB_NAME_KEY_NAMES | \
                                      Y11_XKB_NAME_VIRTUAL_MODS | \
                                      Y11_XKB_NAME_GROUP_NAMES)

/* Empty 32-byte reply for the XKB requests y11 does not implement. */
static void y11_xkb_empty_reply(struct y11_client *c)
{
    uint8_t rep[32];

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    y11_dispatch_send_reply(c, rep, sizeof(rep));
}

/* UseExtension (minor 0): report XKB 1.0 as supported. */
static int y11_xkb_use_extension(struct y11_client *c)
{
    uint8_t rep[32];

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    rep[1] = 1;                 /* supported */
    y11_wire_put16(rep + 8, 1); /* serverMajor */
    y11_wire_put16(rep + 10, 0);        /* serverMinor */
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/* GetState (minor 4): mirror y11's core modifier state. */
static int y11_xkb_get_state(struct y11_client *c)
{
    uint8_t rep[32];
    uint8_t mods = (uint8_t)(y11_input_keyboard()->modifier_mask & 0xFFu);

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    rep[8] = mods;              /* mods: effective */
    rep[9] = mods;              /* baseMods: keys currently held */
    rep[12] = 0;                /* group */
    rep[18] = mods;             /* compatState */
    rep[19] = mods;             /* grabMods */
    rep[21] = mods;             /* lookupMods */
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/*
 * GetMap (minor 8): the keymap.  One TWO_LEVEL key type whose single
 * map entry maps Shift to level 1 (0-based: the shifted level), y11's
 * keysym table as the per-key syms, an all-zero action count per key,
 * four inert virtual-modifier mappings and the modifier map covering
 * every keycode like the reference servers do.  Sections land in the
 * reply in xcb's accessor order: types, syms, action counts, actions,
 * behaviors, virtual mods, explicit components, modifier map, vmodmap.
 */
static int y11_xkb_get_map(struct y11_client *c)
{
    uint8_t rep[40];
    uint8_t *buf;
    size_t off = 0;
    size_t total_syms = 0;
    size_t i;
    uint32_t data_words;

    /* Pass 1: count the keys that carry syms. */
    for (i = Y11_XKB_MIN_KEYCODE; i <= Y11_XKB_MAX_KEYCODE; i++) {
        uint32_t ks[2];

        y11_input_keysyms_for((uint8_t)i, ks);
        if (ks[0] != 0 || ks[1] != 0)
            total_syms += Y11_KEYSYM_LEVELS_PER_KEY;
    }

    buf = malloc(Y11_XKB_STD_TYPES_BYTES +     /* the four std types */
                 Y11_XKB_NUM_KEYS * 8u +  /* sym maps */
                 total_syms * 4u +        /* keysyms */
                 Y11_XKB_NUM_KEYS +       /* action counts */
                 Y11_XKB_VMODS_COUNT +    /* vmod mappings */
                 Y11_XKB_NUM_KEYS * 2u);  /* modifier map */
    if (buf == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0,
                                (uint8_t)Y11_XKB_EXT_OPCODE);
        return 0;
    }

    /* Section 1: the four standard key types (xkbKeyTypeWireDesc +
     * xkbKTMapEntryWireDesc each, entry levels are 0-based). */
    memcpy(buf, y11_xkb_std_types, Y11_XKB_STD_TYPES_BYTES);
    off += Y11_XKB_STD_TYPES_BYTES;

    /* Section 2: key syms.  Each entry is a VARIABLE-SIZE struct: the
     * 8-byte map (kt_index[4], groupInfo, width, nSyms) followed by
     * that key's nSyms keysyms inline (xcb_xkb_key_sym_map_syms()
     * reads them right after the 8-byte header and the iterator
     * advances by 8 + nSyms*4). */
    for (i = Y11_XKB_MIN_KEYCODE; i <= Y11_XKB_MAX_KEYCODE; i++) {
        uint32_t ks[2];
        uint8_t *sm = buf + off;

        y11_input_keysyms_for((uint8_t)i, ks);
        memset(sm, 0, 8);
        off += 8;
        if (ks[0] != 0 || ks[1] != 0) {
            /* kt_index[0] = 1: group 1 uses key type 1 (TWO_LEVEL).
             * The low bits of groupInfo are the number of groups. */
            sm[0] = 1;
            sm[4] = (uint8_t)Y11_XKB_NUM_GROUPS;
            sm[5] = (uint8_t)Y11_XKB_NUM_LEVELS;  /* width */
            y11_wire_put16(sm + 6, Y11_KEYSYM_LEVELS_PER_KEY);
            y11_wire_put32(buf + off, ks[0]);
            y11_wire_put32(buf + off + 4, ks[1]);
            off += Y11_KEYSYM_LEVELS_PER_KEY * 4u;
        }
    }

    /* Section 3: action counts.  nKeyActions bytes, all zero: no key
     * carries actions (wire_count == 0 is explicitly allowed). */
    memset(buf + off, 0, Y11_XKB_NUM_KEYS);
    off += Y11_XKB_NUM_KEYS;

    /* Section 4: behaviors - absent (bit not in present). */

    /* Section 5: virtual mod mappings, one byte per set vmod bit;
     * all zero: the vmods map to nothing. */
    memset(buf + off, 0, Y11_XKB_VMODS_COUNT);
    off += Y11_XKB_VMODS_COUNT;

    /* Section 6: explicit components - none (bit set, zero entries). */

    /* Section 7: modifier map.  One 2-byte (keycode, mods) pair per
     * keycode in the range, like the reference servers. */
    for (i = Y11_XKB_MIN_KEYCODE; i <= Y11_XKB_MAX_KEYCODE; i++) {
        buf[off] = (uint8_t)i;
        buf[off + 1] = (uint8_t)y11_input_modifier_mask_for((uint8_t)i);
        off += 2;
    }

    /* Section 8: virtual mod map - none (bit set, zero entries). */

    /* 40-byte reply header (sz_xkbGetMapReply == 40). */
    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    rep[10] = (uint8_t)Y11_XKB_MIN_KEYCODE;
    rep[11] = (uint8_t)Y11_XKB_MAX_KEYCODE;
    y11_wire_put16(rep + 12, Y11_XKB_MAP_PRESENT);
    rep[14] = 0;                /* firstType */
    rep[15] = (uint8_t)Y11_XKB_NUM_TYPES;      /* nTypes */
    rep[16] = (uint8_t)Y11_XKB_NUM_TYPES;      /* totalTypes */
    rep[17] = (uint8_t)Y11_XKB_MIN_KEYCODE;    /* firstKeySym */
    y11_wire_put16(rep + 18, (uint16_t)total_syms);
    rep[20] = (uint8_t)Y11_XKB_NUM_KEYS;       /* nKeySyms */
    rep[21] = (uint8_t)Y11_XKB_MIN_KEYCODE;    /* firstKeyAction */
    y11_wire_put16(rep + 22, 0);               /* totalActs */
    rep[24] = (uint8_t)Y11_XKB_NUM_KEYS;       /* nKeyActions */
    rep[31] = (uint8_t)Y11_XKB_MIN_KEYCODE;    /* firstModMapKey */
    rep[32] = (uint8_t)Y11_XKB_NUM_KEYS;       /* nModMapKeys */
    rep[33] = (uint8_t)Y11_XKB_NUM_KEYS;       /* totalModMapKeys */
    y11_wire_put16(rep + 38, Y11_XKB_VMODS_MASK);
    data_words = (uint32_t)((sizeof(rep) - 32u + off) / 4u);
    y11_wire_put32(rep + 4, data_words);

    y11_dispatch_send_reply(c, rep, sizeof(rep));
    y11_client_send(c, buf, off);
    free(buf);
    return 0;
}

/*
 * GetIndicatorMap (minor 14): one inert LED.  libxkbcommon derives the
 * LED count with msb_pos(which), so which must be nonzero, and it
 * dereferences the map list for every bit set in it.
 */
static int y11_xkb_get_indicator_map(struct y11_client *c)
{
    uint8_t rep[32 + 12];
    uint32_t data_words = 12u / 4u;

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    y11_wire_put32(rep + 8, 0x00000001u);      /* which: LED 1 */
    y11_wire_put32(rep + 12, 0x00000000u);     /* realIndicators */
    rep[16] = 1;                /* nIndicators */
    /* One zeroed xkbIndicatorMapWireDesc (12 bytes). */
    y11_wire_put32(rep + 4, data_words);
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/*
 * GetNames (minor 17): section names (None), one key-type name (None),
 * no kt level names, one inert indicator name, four vmod names (None),
 * one group name (None) and 248 empty 4-byte key names.  nTypes must
 * match GetMap's nTypes and the key range must cover every keycode.
 */
static int y11_xkb_get_names(struct y11_client *c)
{
    uint8_t rep[32];
    uint8_t *buf;
    size_t off = 0;
    size_t i;

    /*
     * Value list, in wire order:
     *   keycodesName, symbolsName, typesName, compatName:  4 atoms
     *   typeNames[nTypes = 4]:                             4 atoms
     *   nLevelsPerType[4] + alignment pad:                 4 bytes
     *   indicatorNames[1], vmodNames[4], groupNames[1]:    6 atoms
     *   keyNames[248]:                                    992 bytes
     */
    buf = malloc((4u + Y11_XKB_NUM_TYPES + 1u + Y11_XKB_VMODS_COUNT + 1u) * 4u +
                 4u + Y11_XKB_NUM_KEYS * 4u);
    if (buf == NULL) {
        y11_dispatch_send_error(c, Y11_ERR_BAD_ALLOC, 0,
                                (uint8_t)Y11_XKB_EXT_OPCODE);
        return 0;
    }
    for (i = 0; i < (4u + Y11_XKB_NUM_TYPES); i++) {
        y11_wire_put32(buf + off, 0);  /* atom None */
        off += 4;
    }
    /* nLevelsPerType[4], all zero (no level names), then alignment. */
    memset(buf + off, 0, Y11_XKB_NUM_TYPES);
    off += Y11_XKB_NUM_TYPES;
    while ((off & 3u) != 0)
        buf[off++] = 0;
    /* One indicator name, four vmod names, one group name: None. */
    for (i = 0; i < (size_t)(1 + Y11_XKB_VMODS_COUNT + 1); i++) {
        y11_wire_put32(buf + off, 0);
        off += 4;
    }
    /* 248 key names, all empty: name[0] == '\0' means unnamed. */
    memset(buf + off, 0, Y11_XKB_NUM_KEYS * 4u);
    off += Y11_XKB_NUM_KEYS * 4u;

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    y11_wire_put32(rep + 8, Y11_XKB_NAME_PRESENT);
    rep[12] = (uint8_t)Y11_XKB_MIN_KEYCODE;
    rep[13] = (uint8_t)Y11_XKB_MAX_KEYCODE;
    rep[14] = (uint8_t)Y11_XKB_NUM_TYPES;      /* nTypes: matches GetMap */
    rep[15] = 1;                 /* groupNames mask: group 1 */
    y11_wire_put16(rep + 16, Y11_XKB_VMODS_MASK);
    rep[18] = (uint8_t)Y11_XKB_MIN_KEYCODE;    /* firstKey */
    rep[19] = (uint8_t)Y11_XKB_NUM_KEYS;       /* nKeys */
    y11_wire_put32(rep + 20, 0x00000001u);     /* indicators: LED 1 */
    rep[24] = 0;                /* nRadioGroups */
    rep[25] = 0;                /* nKeyAliases */
    y11_wire_put16(rep + 26, 0);               /* nKTLevels */
    y11_wire_put32(rep + 4, (uint32_t)(off / 4u));

    y11_dispatch_send_reply(c, rep, sizeof(rep));
    y11_client_send(c, buf, off);
    free(buf);
    return 0;
}

/*
 * GetControls (minor 6): the full 92-byte reply.  numGroups must be
 * nonzero and the parser reads the 32-byte perKeyRepeat array, so the
 * length field must say 15 words.
 */
static int y11_xkb_get_controls(struct y11_client *c)
{
    uint8_t rep[92];

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    rep[8] = 1;                 /* mkDfltBtn */
    rep[9] = 1;                 /* numGroups */
    y11_wire_put16(rep + 20, 660);     /* repeatDelay */
    y11_wire_put16(rep + 22, 40);      /* repeatInterval */
    y11_wire_put32(rep + 56, 0x00000001u);     /* enabledCtrls: RepeatKeys */
    memset(rep + 60, 0xff, 32);         /* perKeyRepeat: all keys repeat */
    y11_wire_put32(rep + 4, 15u);       /* length: 60 bytes of data */

    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/* GetDeviceInfo (minor 24): deviceID must be nonzero. */
static int y11_xkb_get_device_info(struct y11_client *c)
{
    uint8_t rep[32];

    memset(rep, 0, sizeof(rep));
    rep[0] = 1;                 /* X_Reply */
    rep[1] = 1;                 /* deviceID: the core keyboard */
    /* present, supported, name length: all zero. */
    y11_dispatch_send_reply(c, rep, sizeof(rep));
    return 0;
}

/*
 * XKB request dispatcher.  The minor opcode rides in body byte 1 of
 * every XKB request.
 */
int y11_xkb_req(struct y11_client *c, const uint8_t *pkt, size_t len,
                size_t data_off)
{
    uint8_t minor = pkt[1];

    (void)len;
    (void)data_off;
    switch (minor) {
    case Y11_XKB_REQ_USE_EXTENSION:
        return y11_xkb_use_extension(c);
    case Y11_XKB_REQ_SELECT_EVENTS:
    case Y11_XKB_REQ_BELL:
    case Y11_XKB_REQ_LATCH_LOCK_STATE:
    case Y11_XKB_REQ_SET_CONTROLS:
    case Y11_XKB_REQ_SET_INDICATOR_MAP:
        return 0;               /* request-only: the server sends no reply */
    case Y11_XKB_REQ_GET_STATE:
        return y11_xkb_get_state(c);
    case Y11_XKB_REQ_GET_CONTROLS:
        return y11_xkb_get_controls(c);
    case Y11_XKB_REQ_GET_MAP:
        return y11_xkb_get_map(c);
    case Y11_XKB_REQ_GET_INDICATOR_MAP:
        return y11_xkb_get_indicator_map(c);
    case Y11_XKB_REQ_GET_NAMES:
        return y11_xkb_get_names(c);
    case Y11_XKB_REQ_GET_DEVICE_INFO:
        return y11_xkb_get_device_info(c);
    case Y11_XKB_REQ_GET_COMPAT_MAP:
        /* No symbol interpretations: the zeroed reply's nSIRtrn == 0 ==
         * nTotalSI and firstSIRtrn == 0 satisfy the parser. */
        return y11_xkb_empty_reply(c), 0;
    default:
        /* GetIndicatorState, GetNamedIndicator, GetKbdByName,
         * SetDebuggingFlags, ...: an empty reply keeps the client's
         * request/reply pairing intact. */
        y11_xkb_empty_reply(c);
        return 0;
    }
}
