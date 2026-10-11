//! 32-byte wire error frame formatting (Phase 2).
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    put16, put32, Y11_SAFE_INSUFFICIENT_DATA, Y11_SAFE_INVALID_ENDIAN,
    Y11_SAFE_OK, Y11SafeStatus,
};

/*
 * Canonical core error frame, byte-identical to the C sender's
 * y11_error layout: 0 | error_code, CARD16 sequence, CARD32 bad
 * value, CARD16 minor opcode, major opcode, zero tail.  Multi-byte
 * fields go out in the client's byte order; sequence numbers are
 * CARD16 on the wire, so the caller's wider counter truncates.
 */
#[no_mangle]
pub extern "C" fn y11_format_error(
    error_code: u8,
    major_opcode: u8,
    minor_opcode: u16,
    sequence_number: u32,
    bad_value: u32,
    byte_order: u8,
    out_buf: *mut u8,
    out_len: usize,
) -> Y11SafeStatus {
    if out_buf.is_null() || out_len < 32 {
        return Y11_SAFE_INSUFFICIENT_DATA;
    }
    let be = match byte_order {
        b'B' => true,
        b'l' => false,
        _ => return Y11_SAFE_INVALID_ENDIAN,
    };
    let out = unsafe { core::slice::from_raw_parts_mut(out_buf, 32) };
    out.fill(0);
    out[0] = 0;                  /* error frame */
    out[1] = error_code;
    put16(out, 2, sequence_number as u16, be);
    put32(out, 4, bad_value, be);
    put16(out, 8, minor_opcode, be);
    out[10] = major_opcode;
    Y11_SAFE_OK
}
