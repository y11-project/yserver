/*
 * test_safe_phase5.c - Phase 5 unit tests: reply sizing, fixed-shape
 * limits and the pad-zero audit against the real reply grammars.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "safe.h"

static int checks;

#define EXPECT(cond)                                                  \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            return 1;                                                 \
        }                                                             \
    } while (0)

static void put32le(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

/* A bare zeroed 32-byte reply frame. */
static void frame32(uint8_t *b, uint32_t words)
{
    memset(b, 0, 32);
    b[0] = 1;
    put32le(b + 4, words);
}

static int test_lengths(void)
{
    uint8_t buf[64];
    y11_safe_status st;

    memset(buf, 0, sizeof(buf));
    frame32(buf, 0);
    st = y11_validate_reply_size(43, 0, 32, buf);
    EXPECT(st == Y11_SAFE_OK);

    /* below the 32-byte reply floor */
    st = y11_validate_reply_size(43, 0, 31, buf);
    EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA);

    /* declared words without the bytes to match */
    st = y11_validate_reply_size(43, 1, 32, buf);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    st = y11_validate_reply_size(43, 0, 36, buf);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);

    /* words * 4 wraps the 32-bit length arithmetic */
    st = y11_validate_reply_size(43, 0x40000000u, 32, buf);
    EXPECT(st == Y11_SAFE_LENGTH_OVERFLOW);
    st = y11_validate_reply_size(43, 0xffffffffu, 32, buf);
    EXPECT(st == Y11_SAFE_LENGTH_OVERFLOW);

    /* a NULL buffer only skips the pad inspection */
    st = y11_validate_reply_size(43, 0, 32, NULL);
    EXPECT(st == Y11_SAFE_OK);
    return 0;
}

static int test_fixed_shapes(void)
{
    uint8_t buf[64];
    y11_safe_status st;

    /* GetGeometry is a bare frame; extra words are a construction bug */
    memset(buf, 0, sizeof(buf));
    frame32(buf, 2);
    st = y11_validate_reply_size(14, 2, 40, buf);
    EXPECT(st == Y11_SAFE_INVALID_HEADER);

    /* GetWindowAttributes is exactly 44 bytes / 3 words */
    frame32(buf, 0);
    st = y11_validate_reply_size(3, 0, 32, buf);
    EXPECT(st == Y11_SAFE_INVALID_HEADER);
    frame32(buf, 3);
    st = y11_validate_reply_size(3, 3, 44, buf);
    EXPECT(st == Y11_SAFE_OK);

    /* QueryKeymap is 40 bytes / 2 words */
    frame32(buf, 2);
    st = y11_validate_reply_size(44, 2, 40, buf);
    EXPECT(st == Y11_SAFE_OK);
    frame32(buf, 0);
    st = y11_validate_reply_size(44, 0, 32, buf);
    EXPECT(st == Y11_SAFE_INVALID_HEADER);

    /* QueryPointer / GetInputFocus / TranslateCoords / GetSelectionOwner */
    frame32(buf, 0);
    st = y11_validate_reply_size(38, 0, 32, buf);
    EXPECT(st == Y11_SAFE_OK);
    st = y11_validate_reply_size(43, 0, 32, buf);
    EXPECT(st == Y11_SAFE_OK);
    st = y11_validate_reply_size(40, 0, 32, buf);
    EXPECT(st == Y11_SAFE_OK);
    st = y11_validate_reply_size(23, 0, 32, buf);
    EXPECT(st == Y11_SAFE_OK);
    return 0;
}

static int test_pad_audit(void)
{
    uint8_t buf[64];
    y11_safe_status st;

    /* GetProperty: format 8, 5 items = 5 bytes + 3 pad = 2 words */
    memset(buf, 0, sizeof(buf));
    buf[0] = 1;
    buf[1] = 8;                       /* format */
    put32le(buf + 4, 2);             /* words */
    put32le(buf + 16, 5);            /* n_items */
    memset(buf + 32, 0x41, 5);       /* payload */
    st = y11_validate_reply_size(20, 2, 40, buf);
    EXPECT(st == Y11_SAFE_OK);

    /* dirty padding: a leak of uninitialized heap */
    buf[39] = 0xAA;
    st = y11_validate_reply_size(20, 2, 40, buf);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    buf[39] = 0;

    /* items that outgrow the declared words: 9 bytes + 3 pad != 8 */
    put32le(buf + 16, 9);
    st = y11_validate_reply_size(20, 2, 40, buf);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    put32le(buf + 16, 5);

    /* format outside 8/16/32 on a found property */
    buf[1] = 24;
    st = y11_validate_reply_size(20, 2, 40, buf);
    EXPECT(st == Y11_SAFE_INVALID_HEADER);
    buf[1] = 8;

    /* GetAtomName: 14 bytes + 2 pad = 4 words */
    memset(buf, 0, sizeof(buf));
    buf[0] = 1;
    buf[8] = 14; buf[9] = 0;        /* name_len */
    put32le(buf + 4, 4);            /* words */
    memset(buf + 32, 'y', 14);
    st = y11_validate_reply_size(17, 4, 48, buf);
    EXPECT(st == Y11_SAFE_OK);

    buf[47] = 0x42;                  /* dirty pad */
    st = y11_validate_reply_size(17, 4, 48, buf);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    buf[47] = 0;

    /* name_len inconsistent with the declared words */
    buf[9] = 1;                      /* name_len 270: overflows the words */
    st = y11_validate_reply_size(17, 4, 48, buf);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    return 0;
}

int main(void)
{
    if (test_lengths() != 0)
        return 1;
    if (test_fixed_shapes() != 0)
        return 1;
    if (test_pad_audit() != 0)
        return 1;
    printf("y11_safe phase 5: %d checks ok\n", checks);
    return 0;
}
