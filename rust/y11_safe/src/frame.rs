//! Request framing (stub: Phase 0) and property payload arithmetic.
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    get16, Y11_SAFE_INSUFFICIENT_DATA, Y11_SAFE_OK, Y11_SAFE_PAYLOAD_MISMATCH,
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
 * Stub: single-word framing only.  wire_len == 0 without
 * BIG-REQUESTS frames as the 4-byte request the X protocol allows;
 * with the extension enabled the 8-byte extended header applies and
 * the stub defers its parse to Phase 1.  The 32-bit extended length
 * (the LENGTH_OVERFLOW clause) also lands in Phase 1.
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
    let wire_len = get16(b, 2);
    if wire_len == 0 && big_req_enabled != 0 {
        return Y11_SAFE_INSUFFICIENT_DATA; /* extended header: Phase 1 */
    }
    let frame = Y11RequestFrame {
        opcode: b[0],
        wire_len,
        is_bigreq: 0,
        true_len: if wire_len == 0 { 4 } else { wire_len as u32 * 4 },
    };
    unsafe { *out = frame };
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
