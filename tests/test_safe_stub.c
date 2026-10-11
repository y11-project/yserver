/*
 * test_safe_stub.c - Phase 0 smoke: include the contract header, link
 * against liby11_safe.a and drive the stub C-ABI.  Exit 0 is the pass
 * condition in the thorn build (target test_safe_stub).
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "y11_safe.h"

int main(void)
{
    static const uint8_t req4[4] = {0x2a, 0x00, 0x01, 0x00}; /* len=1 */
    static const uint8_t hs12[12] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    struct y11_request_frame fr;
    struct y11_handshake_req hs;
    y11_safe_status st;

    /* a full 4-byte request frames */
    st = y11_frame_request(req4, sizeof(req4), 0, &fr);
    if (st != Y11_SAFE_OK)
        return 1;
    if (fr.opcode != 0x2a || fr.wire_len != 1 || fr.true_len != 4)
        return 2;

    /* a truncated header reports insufficient data, not a crash */
    st = y11_frame_request(req4, 2, 0, &fr);
    if (st != Y11_SAFE_INSUFFICIENT_DATA)
        return 3;

    /* the 12-byte setup header parses; the skip fields land in Phase 1 */
    st = y11_handshake_parse_header(hs12, sizeof(hs12), &hs);
    if (st != Y11_SAFE_OK)
        return 4;
    if (hs.byte_order != 'l' || hs.major_version != 11)
        return 5;

    /* short setup header reports insufficient data */
    st = y11_handshake_parse_header(hs12, 4, &hs);
    if (st != Y11_SAFE_INSUFFICIENT_DATA)
        return 6;

    /* property math: 4 x 32-bit units = 16 padded bytes */
    st = y11_validate_property_payload(32, 4, 16);
    if (st != Y11_SAFE_OK)
        return 7;

    /* format outside 8/16/32 is a header violation */
    st = y11_validate_property_payload(7, 1, 16);
    if (st != Y11_SAFE_INVALID_HEADER)
        return 8;
    st = y11_validate_property_payload(16, 3, 4);
    if (st != Y11_SAFE_PAYLOAD_MISMATCH)
        return 9;

    printf("y11_safe Phase 0 stub: ok\n");
    return 0;
}
