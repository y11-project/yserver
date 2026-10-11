//! Request framing with BIG-REQUESTS (Phase 1).
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    get16, get32, Y11_SAFE_INSUFFICIENT_DATA, Y11_SAFE_INVALID_HEADER,
    Y11_SAFE_LENGTH_OVERFLOW, Y11_SAFE_OK, Y11_SAFE_PAYLOAD_MISMATCH,
    Y11SafeStatus,
};

/// struct y11_request_frame (include/y11_safe.h).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Y11RequestFrame {
    pub opcode: u8,
    pub wire_len: u16,
    pub is_bigreq: u8,
    pub true_len: u32,
}

/*
 * Frame one request off the stream.  length16 == 0 is the
 * BIG-REQUESTS escape: valid only once the client enabled the
 * extension, else the packet violates the framing (the C server
 * additionally reads such a packet leniently as a bare 4-byte
 * request; the validator stays strict).  Frame fields are always
 * little-endian — only byte-order 'l' connections are admitted.
 * No receive cap is applied here: oversize policy stays with the
 * caller, which rejects true_len values it cannot buffer.
 */
#[no_mangle]
pub extern "C" fn y11_frame_request(
    buf: *const u8,
    len: usize,
    big_req_enabled: u8,
    out: *mut Y11RequestFrame,
) -> Y11SafeStatus {
    if buf.is_null() || out.is_null() || len < 4 {
        return Y11_SAFE_INSUFFICIENT_DATA;
    }
    let b = unsafe { core::slice::from_raw_parts(buf, 4) };

    let opcode = b[0];            /* b[1] carries the per-request detail */
    let wire_len = get16(b, 2);

    let (is_bigreq, true_len) = if wire_len > 0 {
        /* Standard 4-byte header: the length counts 4-byte units. */
        let total = match u32::from(wire_len).checked_mul(4) {
            Some(t) => t,
            None => return Y11_SAFE_LENGTH_OVERFLOW,
        };
        if total < 4 {
            return Y11_SAFE_LENGTH_OVERFLOW;
        }
        (0u8, total)
    } else {
        /* Extended 8-byte header; the 32-bit length also counts units. */
        if big_req_enabled == 0 {
            return Y11_SAFE_INVALID_HEADER;
        }
        if len < 8 {
            return Y11_SAFE_INSUFFICIENT_DATA;
        }
        let b8 = unsafe { core::slice::from_raw_parts(buf, 8) };
        let length32 = get32(b8, 4);
        let total = match length32.checked_mul(4) {
            Some(t) => t,
            None => return Y11_SAFE_LENGTH_OVERFLOW,
        };
        if total < 8 {
            return Y11_SAFE_LENGTH_OVERFLOW;
        }
        (1u8, total)
    };

    if len < true_len as usize {
        return Y11_SAFE_INSUFFICIENT_DATA;
    }

    unsafe {
        *out = Y11RequestFrame {
            opcode,
            wire_len,
            is_bigreq,
            true_len,
        };
    }
    Y11_SAFE_OK
}

/*
 * ChangeProperty payload arithmetic (complete: the Phase 0 pure-math
 * validator).  format outside 8/16/32, or a padded item count that
 * outgrows the request body, is PAYLOAD_MISMATCH.  64-bit math: the
 * u32 pad4 wrap only occurs for item counts the receive cap forbids.
 */
#[no_mangle]
pub extern "C" fn y11_validate_property_payload(
    format: u8,
    num_items: u32,
    actual_bytes: usize,
) -> Y11SafeStatus {
    if format != 8 && format != 16 && format != 32 {
        return Y11_SAFE_PAYLOAD_MISMATCH;
    }
    let nbytes = (num_items as u64) * (format as u64) / 8;
    let padded = (nbytes + 3) & !3u64;
    if padded > actual_bytes as u64 {
        return Y11_SAFE_PAYLOAD_MISMATCH;
    }
    Y11_SAFE_OK
}
