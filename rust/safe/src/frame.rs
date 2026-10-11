//! Request framing with BIG-REQUESTS and the opcode length gate.
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    get16, get32, Y11_SAFE_INSUFFICIENT_DATA, Y11_SAFE_INVALID_HEADER,
    Y11_SAFE_LENGTH_OVERFLOW, Y11_SAFE_OK, Y11SafeStatus,
};
use crate::opcode::OPCODE_MIN_LEN;

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
    /* Opcode gate runs only on fully buffered packets: a truncated
     * prefix is INSUFFICIENT_DATA either way, and the C dispatch loop
     * waits on the same condition. */
    if true_len < OPCODE_MIN_LEN[opcode as usize] as u32 {
        return Y11_SAFE_INVALID_HEADER;
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

