/*
 * test_safe_phase3.c - Phase 3 unit tests: the opcode minimum-length
 * gate and the C handler error-propagation contract.
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
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            return 1;                                                 \
        }                                                             \
    } while (0)

static int test_min_len_table(void)
{
    EXPECT(y11_opcode_min_len(1) == 32);    /* CreateWindow */
    EXPECT(y11_opcode_min_len(2) == 12);    /* ChangeWindowAttributes */
    EXPECT(y11_opcode_min_len(12) == 12);   /* ConfigureWindow */
    EXPECT(y11_opcode_min_len(18) == 24);   /* ChangeProperty */
    EXPECT(y11_opcode_min_len(20) == 24);   /* GetProperty */
    EXPECT(y11_opcode_min_len(25) == 44);   /* SendEvent */
    EXPECT(y11_opcode_min_len(41) == 24);   /* WarpPointer */
    EXPECT(y11_opcode_min_len(53) == 16);   /* CreatePixmap */
    EXPECT(y11_opcode_min_len(55) == 16);   /* CreateGC */
    EXPECT(y11_opcode_min_len(56) == 12);   /* ChangeGC */
    EXPECT(y11_opcode_min_len(62) == 28);   /* CopyArea */
    EXPECT(y11_opcode_min_len(63) == 32);   /* CopyPlane */
    EXPECT(y11_opcode_min_len(70) == 12);   /* PolyFillRectangle */
    EXPECT(y11_opcode_min_len(36) == 4);    /* GrabServer */
    EXPECT(y11_opcode_min_len(43) == 4);    /* GetInputFocus */
    EXPECT(y11_opcode_min_len(127) == 4);   /* NoOperation */
    EXPECT(y11_opcode_min_len(128) == 4);   /* extension floor */
    EXPECT(y11_opcode_min_len(255) == 4);
    return 0;
}

static int test_gate(void)
{
    struct y11_request_frame fr;
    y11_safe_status st;

    /* opcode 1 (min 32) fully buffered at exactly 32: OK */
    {
        uint8_t buf[32] = {1, 0, 8, 0};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_OK);
        EXPECT(fr.true_len == 32);
    }

    /* opcode 1 at 8 bytes: framed, buffered, below minimum */
    {
        uint8_t buf[8] = {1, 0, 2, 0};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_INVALID_HEADER);
    }

    /* a short prefix of a legal 32-byte request is INSUFFICIENT_DATA,
     * not a gate rejection: the framer waits before judging */
    {
        uint8_t buf[4] = {1, 0, 8, 0};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA);
    }

    /* opcode 18 (min 24) at 12 buffered bytes: gate rejection */
    {
        uint8_t buf[12] = {18, 0, 3, 0};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_INVALID_HEADER);
    }

    /* opcode 18 at exactly 24: OK */
    {
        uint8_t buf[24] = {18, 0, 6, 0};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_OK);
    }

    /* opcode 55 (min 16) at 8: rejection; at 16: OK */
    {
        uint8_t buf[16] = {55, 0, 4, 0};
        st = y11_frame_request(buf, 8, 0, &fr);
        EXPECT(st == Y11_SAFE_INSUFFICIENT_DATA);
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_OK);
        /* truncation below the table minimum once buffered */
        buf[2] = 2;                 /* length16 = 2 -> 8 bytes */
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_INVALID_HEADER);
    }

    /* extension opcodes keep the 4-byte floor */
    {
        uint8_t buf[4] = {129, 2, 1, 0};
        st = y11_frame_request(buf, sizeof(buf), 0, &fr);
        EXPECT(st == Y11_SAFE_OK);
        EXPECT(fr.true_len == 4);
    }

    /* bigreq form obeys the gate too: opcode 1 declaring 8 bytes */
    {
        uint8_t buf[8] = {1, 0, 0, 0, 2, 0, 0, 0};
        st = y11_frame_request(buf, sizeof(buf), 1, &fr);
        EXPECT(st == Y11_SAFE_INVALID_HEADER);
    }
    return 0;
}

int main(void)
{
    if (test_min_len_table() != 0)
        return 1;
    if (test_gate() != 0)
        return 1;
    printf("y11_safe phase 3: %d checks ok\n", checks);
    return 0;
}
