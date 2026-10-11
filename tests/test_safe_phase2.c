/*
 * test_safe_phase2.c - Phase 2 unit tests: value-mask bounds, exact
 * property padding and the 32-byte error frame in both byte orders.
 * Exit 0 is the pass condition.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "safe.h"

static int checks;

#define EXPECT(cond)                                                  \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            return 1;                                                 \
        }                                                             \
    } while (0)

static int test_value_mask(void)
{
    y11_safe_status st;

    /* an empty mask takes no value bytes */
    st = y11_validate_value_mask(0x00000000u, 0);
    EXPECT(st == Y11_SAFE_OK);

    /* three set bits: exactly 12 bytes */
    st = y11_validate_value_mask(0x00000007u, 12);
    EXPECT(st == Y11_SAFE_OK);
    st = y11_validate_value_mask(0x00000007u, 8);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    st = y11_validate_value_mask(0x00000007u, 16);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);

    /* a full mask: exactly 128 bytes */
    st = y11_validate_value_mask(0xffffffffu, 128);
    EXPECT(st == Y11_SAFE_OK);
    st = y11_validate_value_mask(0xffffffffu, 127);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);

    /* sparse high bit */
    st = y11_validate_value_mask(0x80000000u, 4);
    EXPECT(st == Y11_SAFE_OK);

    /* count, not magnitude: bit 0 alone is one value */
    st = y11_validate_value_mask(0x00000001u, 4);
    EXPECT(st == Y11_SAFE_OK);
    return 0;
}

static int test_property(void)
{
    y11_safe_status st;

    /* 8-bit: 5 items = 5 bytes + 3 pad */
    st = y11_validate_property_payload(8, 5, 8);
    EXPECT(st == Y11_SAFE_OK);
    st = y11_validate_property_payload(8, 5, 5);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);

    /* 8-bit exact multiple: no padding */
    st = y11_validate_property_payload(8, 4, 4);
    EXPECT(st == Y11_SAFE_OK);

    /* 16-bit: 3 items = 6 bytes + 2 pad */
    st = y11_validate_property_payload(16, 3, 8);
    EXPECT(st == Y11_SAFE_OK);

    /* 32-bit: 4 items = 16 bytes, no padding */
    st = y11_validate_property_payload(32, 4, 16);
    EXPECT(st == Y11_SAFE_OK);

    /* zero items is a valid empty payload */
    st = y11_validate_property_payload(32, 0, 0);
    EXPECT(st == Y11_SAFE_OK);

    /* format outside the protocol's 8/16/32 */
    st = y11_validate_property_payload(24, 1, 8);
    EXPECT(st == Y11_SAFE_INVALID_HEADER);

    /* trailing slack is a mismatch: the wire body must be exact */
    st = y11_validate_property_payload(16, 3, 9);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);

    /* missing pad bytes */
    st = y11_validate_property_payload(16, 3, 6);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    return 0;
}

static int test_error_frame(void)
{
    uint8_t out[40];
    y11_safe_status st;

    /* little-endian: code 2, major 43, minor 0, seq 0x1234,
     * bad value 0xdeadbeef */
    for (size_t i = 0; i < sizeof(out); i++)
        out[i] = 0xaa;             /* canary fill */
    st = y11_format_error(2, 43, 0, 0x1234, 0xdeadbeefu, 'l',
                          out, sizeof(out));
    EXPECT(st == Y11_SAFE_OK);
    EXPECT(out[0] == 0);           /* error frame */
    EXPECT(out[1] == 2);
    EXPECT(out[2] == 0x34 && out[3] == 0x12);       /* seq LSB first */
    EXPECT(out[4] == 0xef && out[5] == 0xbe);
    EXPECT(out[6] == 0xad && out[7] == 0xde);       /* bad value */
    EXPECT(out[8] == 0 && out[9] == 0);             /* minor */
    EXPECT(out[10] == 43);                          /* major */
    for (size_t i = 11; i < 32; i++)
        EXPECT(out[i] == 0);                       /* zero tail */
    EXPECT(out[32] == 0xaa);                       /* beyond: untouched */

    /* big-endian: same fields, MSB first */
    st = y11_format_error(2, 43, 0, 0x1234, 0xdeadbeefu, 'B',
                          out, sizeof(out));
    EXPECT(st == Y11_SAFE_OK);
    EXPECT(out[2] == 0x12 && out[3] == 0x34);
    EXPECT(out[4] == 0xde && out[5] == 0xad);
    EXPECT(out[6] == 0xbe && out[7] == 0xef);

    /* big-endian minor opcode lands MSB first at bytes 8..10 */
    st = y11_format_error(16, 146, 0x0102, 0xffff, 7, 'B',
                          out, sizeof(out));
    EXPECT(st == Y11_SAFE_OK);
    EXPECT(out[8] == 0x01 && out[9] == 0x02 && out[10] == 146);
    EXPECT(out[1] == 16);
    EXPECT(out[2] == 0xff && out[3] == 0xff);      /* seq truncates */

    /* a short buffer is refused without touching anything */
    for (size_t i = 0; i < sizeof(out); i++)
        out[i] = 0xbb;
    st = y11_format_error(1, 2, 3, 4, 5, 'l', out, 31);
    EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA);
    EXPECT(out[0] == 0xbb);                        /* untouched */

    /* unknown byte order is refused before any write */
    st = y11_format_error(1, 2, 3, 4, 5, 'x', out, sizeof(out));
    EXPECT(st == Y11_SAFE_INVALID_ENDIAN);
    EXPECT(out[0] == 0xbb);

    /* the all-zero frame is all zeros */
    st = y11_format_error(0, 0, 0, 0, 0, 'l', out, sizeof(out));
    EXPECT(st == Y11_SAFE_OK);
    for (size_t i = 0; i < 32; i++)
        EXPECT(out[i] == 0);
    return 0;
}

int main(void)
{
    if (test_value_mask() != 0)
        return 1;
    if (test_property() != 0)
        return 1;
    if (test_error_frame() != 0)
        return 1;
    printf("y11_safe phase 2: %d checks ok\n", checks);
    return 0;
}
