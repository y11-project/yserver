/*
 * safe.h - C-ABI contract for libsafe.a.
 *
 * Copyright (c) 2026 The Y11 Project
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

/* One framed request; true_len counts the whole packet in bytes. */
struct y11_request_frame {
    uint8_t opcode;
    uint16_t wire_len;      /* raw CARD16; 0 selects the extended form */
    uint8_t is_bigreq;      /* 1 when the 8-byte extended header applies */
    uint32_t true_len;      /* validated total request length in bytes */
};

/* Parse the fixed SetupRequest header; the caller buffers
 * skip_bytes more to complete the handshake. */
y11_safe_status y11_handshake_parse_header(const uint8_t *buf, size_t len,
                                           struct y11_handshake_req *out);

/* Frame one request; OK means true_len bytes are buffered. */
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

/* One 4-byte value per set mask bit; valid-bit policy is the
 * caller's. */
y11_safe_status y11_validate_value_mask(uint32_t value_mask,
                                        size_t payload_len);

/* Total request bytes the core protocol requires per opcode; 4 for
 * extensions and unlisted opcodes. */
uint16_t y11_opcode_min_len(uint8_t opcode);

/*
 * Validate a fixed-part + element list body: the remainder after
 * fixed_part must divide exactly by element_size.  On OK,
 * *n_elements receives the quotient (n_elements may be NULL).
 */
y11_safe_status y11_validate_list(size_t avail,
                                  size_t fixed_part,
                                  uint32_t element_size,
                                  uint32_t *n_elements);

/*
 * Audit one complete outbound reply: the CARD32 length word must
 * account for every transmitted byte (32 + words * 4, checked),
 * fixed-shape core opcodes carry their exact count, and the
 * string/item replies (GetProperty, GetAtomName) must have zeroed
 * wire padding.  out_buf may be NULL to skip the pad inspection.
 */
y11_safe_status y11_validate_reply_size(uint8_t major_opcode,
                                        uint32_t reply_len_words,
                                        size_t actual_total_bytes,
                                        const uint8_t *out_buf);

/*
 * Byte extent of a ShmPutImage blit inside the client's segment:
 * depth 1 packs rows to 4-byte strides, everything else is 32bpp.
 * Oversized source extents are PAYLOAD_MISMATCH, checked-arithmetic
 * overflow is LENGTH_OVERFLOW; a zero extent is trivially OK.
 */
y11_safe_status y11_shm_bounds(uint32_t total_width,
                               uint32_t src_x, uint32_t src_y,
                               uint32_t src_w, uint32_t src_h,
                               uint8_t depth, uint32_t offset,
                               uint64_t seg_size);

/*
 * Format a canonical 32-byte X11 wire error frame into out_buf
 * (out_len must be at least 32): byte 0 reads 0, byte 1 carries
 * error_code, the CARD16 sequence number and CARD32 bad value are
 * encoded in byte_order ('B' big-endian, 'l' little-endian), the
 * CARD16 minor opcode precedes the major opcode at bytes 8..10, and
 * the tail stays zero.  The sequence counter truncates to its low
 * 16 bits, as the wire field is a CARD16.
 */
y11_safe_status y11_format_error(uint8_t error_code,
                                 uint8_t major_opcode,
                                 uint16_t minor_opcode,
                                 uint32_t sequence_number,
                                 uint32_t bad_value,
                                 uint8_t byte_order,
                                 uint8_t *out_buf,
                                 size_t out_len);

#endif /* Y11_SAFE_H */
