/*
 * y11_safe.h - C-ABI contract for liby11_safe.a, the no_std Rust wire
 * validators (docs/Y11_SAFE_AUDIT.md).  Structs are POD with fixed-width
 * fields; buffers cross as (buf, len) borrows; every call returns
 * y11_safe_status.  The thorn-rs audit compares this header's function
 * declarations against the archive's exported symbols, so every
 * prototype here must exist as #[no_mangle] pub extern "C" in Rust.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef Y11_SAFE_H
#define Y11_SAFE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Every validator result.  Ordered so 0 is the only success; each
 * failure names the protocol clause that was violated.
 */
typedef enum {
    Y11_SAFE_OK = 0,            /* parsed; out-params are valid */
    Y11_SAFE_INSUFFICIENT_DATA, /* well-formed prefix; buffer more */
    Y11_SAFE_INVALID_HEADER,    /* framing/opcode violation */
    Y11_SAFE_LENGTH_OVERFLOW,   /* declared size exceeds the cap or width */
    Y11_SAFE_UNSUPPORTED_VER,   /* protocol major version is not 11 */
    Y11_SAFE_INVALID_ENDIAN,    /* byte order is not little-endian */
    Y11_SAFE_PAYLOAD_MISMATCH   /* value/format/units disagree with bytes */
} y11_safe_status;

/*
 * Connection-setup header (the 12-byte fixed part of SetupRequest).
 * skip_bytes carries the padded authorization lengths so the caller can
 * advance past them once buffered.
 */
struct y11_handshake_req {
    uint8_t byte_order;     /* 'l' or 'B' as received */
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t auth_proto_len;
    uint16_t auth_data_len;
    uint32_t skip_bytes;    /* pad4(auth_proto_len) + pad4(auth_data_len) */
};

/*
 * One framed request: the 4-byte core header, or the 8-byte extended
 * header when BIG-REQUESTS is enabled and wire_len reads zero.
 * true_len is the total request size in bytes including the header.
 */
struct y11_request_frame {
    uint8_t opcode;
    uint16_t wire_len;      /* raw CARD16; 0 selects the extended form */
    uint8_t is_bigreq;      /* 1 when the 8-byte extended header applies */
    uint32_t true_len;      /* validated total request length in bytes */
};

/*
 * Parse the fixed SetupRequest header.  len < 12 reports
 * INSUFFICIENT_DATA; otherwise out is filled and OK returned.  The
 * caller still has to buffer skip_bytes more before the handshake
 * completes.
 */
y11_safe_status y11_handshake_parse_header(const uint8_t *buf, size_t len,
                                           struct y11_handshake_req *out);

/*
 * Frame one request off the stream.  len < 4 (or len < 8 for the
 * extended form selected by big_req_enabled and a zero wire_len)
 * reports INSUFFICIENT_DATA; a declared length that cannot be
 * represented reports LENGTH_OVERFLOW.  On OK, out holds the frame
 * and true_len is the byte count the caller must have buffered.
 */
y11_safe_status y11_frame_request(const uint8_t *buf, size_t len,
                                  uint8_t big_req_enabled,
                                  struct y11_request_frame *out);

/*
 * Validate ChangeProperty payload arithmetic: format must be 8, 16 or
 * 32 and the padded n-items byte count must fit in actual_bytes.
 * Pure math, no buffer access.
 */
y11_safe_status y11_validate_property_payload(uint8_t format,
                                              uint32_t num_items,
                                              size_t actual_bytes);

/*
 * Validate a value-list payload against its bitmask: the list carries
 * exactly one 4-byte value per set bit, so payload_len must equal
 * popcount(value_mask) * 4.  Bits outside a request's valid mask are
 * the caller's policy; this check is pure arithmetic.
 */
y11_safe_status y11_validate_value_mask(uint32_t value_mask,
                                        size_t payload_len);

#endif /* Y11_SAFE_H */
