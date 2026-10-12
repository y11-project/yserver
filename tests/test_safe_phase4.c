/*
 * test_safe_phase4.c - Phase 4 unit tests: list divisibility and the
 * SHM stride bounds against known-good and overflowing extents.
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

static int test_list(void)
{
    uint32_t n;
    y11_safe_status st;

    st = y11_validate_list(8, 8, 4, &n);     /* fixed part only */
    EXPECT(st == Y11_SAFE_OK && n == 0);
    st = y11_validate_list(16, 8, 4, &n);    /* two elements */
    EXPECT(st == Y11_SAFE_OK && n == 2);
    st = y11_validate_list(0, 0, 8, &n);
    EXPECT(st == Y11_SAFE_OK && n == 0);

    st = y11_validate_list(12, 8, 8, &n);   /* 4 bytes: half an element */
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    st = y11_validate_list(4, 8, 4, &n);     /* below the fixed part */
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    st = y11_validate_list(8, 8, 0, &n);    /* zero-size elements */
    EXPECT(st == Y11_SAFE_INVALID_HEADER);

    /* rectangles: 68 + 3x8 = 92 */
    st = y11_validate_list(92, 68, 8, &n);
    EXPECT(st == Y11_SAFE_OK && n == 3);
    st = y11_validate_list(93, 68, 8, &n);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);

    /* NULL out-param is legal when only the verdict matters */
    st = y11_validate_list(16, 8, 8, NULL);
    EXPECT(st == Y11_SAFE_OK);
    return 0;
}

static int test_shm_bounds(void)
{
    y11_safe_status st;

    /* 1920x1080 ZPixmap blit at (0,0): 1080 rows * 7680 stride */
    st = y11_shm_bounds(1920, 0, 0, 1920, 1080, 32, 0,
                        (uint64_t)1080 * 7680);
    EXPECT(st == Y11_SAFE_OK);

    /* one row short of the segment: mismatch */
    st = y11_shm_bounds(1920, 0, 0, 1920, 1080, 32, 0,
                        (uint64_t)1079 * 7680);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);

    /* source right edge past the image width: mismatch */
    st = y11_shm_bounds(100, 0, 0, 101, 1, 32, 0, 1000);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);

    /* depth 1 bitmap rows pack to 4-byte strides */
    st = y11_shm_bounds(32, 0, 0, 32, 1, 1, 0, 4);
    EXPECT(st == Y11_SAFE_OK);
    st = y11_shm_bounds(33, 0, 0, 33, 1, 1, 0, 4);   /* needs 8 */
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);

    /* the offset counts against the segment */
    st = y11_shm_bounds(100, 0, 0, 100, 1, 32, 396, 400);
    EXPECT(st == Y11_SAFE_PAYLOAD_MISMATCH);
    st = y11_shm_bounds(100, 0, 0, 100, 1, 32, 0, 400);
    EXPECT(st == Y11_SAFE_OK);

    /* zero extents blit nothing: trivially OK even in tiny segments */
    st = y11_shm_bounds(0, 0, 0, 0, 0, 32, 0, 0);
    EXPECT(st == Y11_SAFE_OK);
    st = y11_shm_bounds(100, 0, 0, 0, 100, 32, 0, 0);
    EXPECT(st == Y11_SAFE_OK);
    return 0;
}

int main(void)
{
    if (test_list() != 0)
        return 1;
    if (test_shm_bounds() != 0)
        return 1;
    printf("y11_safe phase 4: %d checks ok\n", checks);
    return 0;
}
