/*
 * test_safe_phase1.c - Phase 1 unit tests: endian-aware handshake
 * parsing and checked request framing, driven through the frozen
 * C-ABI against the real archive.  Exit 0 is the pass condition.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "safe.h"

static int checks;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            return 1;                                                     \
        }                                                                 \
    } while (0)

/* A little-endian setup header: 'l', 11.0, 4+16 auth bytes. */
static const uint8_t hs_le[12] = {
    'l', 0x00, 11, 0, 0, 0, 4, 0, 16, 0, 0, 0
};

/* Big-endian: 'B', 11.0, 2+1 auth bytes. */
static const uint8_t hs_be[12] = {
    'B', 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0, 0
};

static int test_handshake(void)
{
    struct y11_handshake_req hs;
    y11_safe_status st;

    st = y11_handshake_parse_header(hs_le, sizeof(hs_le), &hs);
    EXPECT(st == Y11_SAFE_OK);
    EXPECT(hs.byte_order == 'l');
    EXPECT(hs.major_version == 11 && hs.minor_version == 0);
    EXPECT(hs.auth_proto_len == 4 && hs.auth_data_len == 16);
    /* pad4(4) + pad4(16) = 4 + 16 */
    EXPECT(hs.skip_bytes == 20);

    st = y11_handshake_parse_header(hs_be, sizeof(hs_be), &hs);
    EXPECT(st == Y11_SAFE_OK);
    EXPECT(hs.byte_order == 'B');
    EXPECT(hs.major_version == 11);
    /* fields decoded MSB-first, not raw */
    EXPECT(hs.auth_proto_len == 2 && hs.auth_data_len == 1);
    /* pad4(2) + pad4(1) = 4 + 4 */
    EXPECT(hs.skip_bytes == 8);

    /* boundary: one byte short of the fixed part */
    st = y11_handshake_parse_header(hs_le, 11, &hs);
    EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA);

    /* neither byte order */
    {
        uint8_t bad[12];
        for (size_t i = 0; i < sizeof(bad); i++)
            bad[i] = hs_le[i];
        bad[0] = 'x';
        st = y11_handshake_parse_header(bad, sizeof(bad), &hs);
        EXPECT(st == Y11_SAFE_INVALID_ENDIAN);
    }

    /* wrong protocol major */
    {
        uint8_t v10[12];
        for (size_t i = 0; i < sizeof(v10); i++)
            v10[i] = hs_le[i];
        v10[2] = 10;
        st = y11_handshake_parse_header(v10, sizeof(v10), &hs);
        EXPECT(st == Y11_SAFE_UNSUPPORTED_VER);
    }

    /* maximal authorization lengths must not wrap anywhere */
    {
        uint8_t big[12] = {
            'l', 0, 11, 0, 0, 0,
            0xff, 0xff,             /* proto 65535 */
            0xff, 0xff,             /* data 65535 */
            0, 0
        };
        st = y11_handshake_parse_header(big, sizeof(big), &hs);
        EXPECT(st == Y11_SAFE_OK);
        EXPECT(hs.skip_bytes == 131072); /* 2 * pad4(65535) */
    }
    return 0;
}

static int test_framing(void)
{
    struct y11_request_frame fr;
    y11_safe_status st;

    /* standard request: opcode 42, 5 units = 20 bytes, fully buffered */
    {
        uint8_t buf[20] = {42, 0, 5, 0};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_OK);
        EXPECT(fr.opcode == 42 && fr.wire_len == 5);
        EXPECT(fr.is_bigreq == 0 && fr.true_len == 20);
    }

    /* same request, one byte missing */
    {
        uint8_t buf[19] = {42, 0, 5, 0};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA);
    }

    /* boundary: 3 bytes is not a header */
    {
        uint8_t buf[3] = {42, 0, 5};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA);
    }

    /* length16 0xffff: 262140 bytes, no 16-bit wrap */
    {
        uint8_t buf[4] = {9, 0, 0xff, 0xff};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA); /* needs 262140 */
    }

    /* extended form without the extension is a framing violation */
    {
        uint8_t buf[8] = {9, 0, 0, 0, 0, 0, 0, 4};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_INVALID_HEADER);
    }

    /* extended header still incomplete at 7 bytes */
    {
        uint8_t buf[7] = {9, 0, 0, 0, 0, 0, 0};
        st = y11_frame_request(buf, sizeof(buf), 1, &fr);
        EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA);
    }

    /* extended: 6 units = 24 bytes, header present, body missing */
    {
        uint8_t buf[8] = {9, 0, 0, 0, 6, 0, 0, 0};
        st = y11_frame_request(buf, sizeof(buf), 1, &fr);
        EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA);
    }

    /* extended: fully buffered */
    {
        uint8_t buf[24] = {9, 0, 0, 0, 6, 0, 0, 0};
        st = y11_frame_request(buf, sizeof(buf), 1, &fr);
        EXPECT(st == Y11_SAFE_OK);
        EXPECT(fr.opcode == 9 && fr.wire_len == 0);
        EXPECT(fr.is_bigreq == 1 && fr.true_len == 24);
    }

    /* extended length below one 8-byte header unit */
    {
        uint8_t buf[8] = {9, 0, 0, 0, 1, 0, 0, 0};
        st = y11_frame_request(buf, sizeof(buf), 1, &fr);
        EXPECT(st == Y11_SAFE_LENGTH_OVERFLOW);
    }

    /* 32-bit length wrap: 0xffffffff * 4 overflows */
    {
        uint8_t buf[8] = {9, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
        st = y11_frame_request(buf, sizeof(buf), 1, &fr);
        EXPECT(st == Y11_SAFE_LENGTH_OVERFLOW);
    }

    /* a trailing request must not leak into the first frame */
    {
        uint8_t buf[12] = {42, 0, 2, 0, 0, 0, 0, 0, 7, 0, 1, 0};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_OK);
        EXPECT(fr.true_len == 8); /* first request only */
    }
    return 0;
}

int main(void)
{
    if (test_handshake() != 0)
        return 1;
    if (test_framing() != 0)
        return 1;
    printf("y11_safe phase 1: %d checks ok\n", checks);
    return 0;
}
