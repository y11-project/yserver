//! SetupRequest header parser (Phase 1).
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    get16, get16be, pad_delta, Y11_SAFE_INSUFFICIENT_DATA,
    Y11_SAFE_INVALID_ENDIAN, Y11_SAFE_LENGTH_OVERFLOW, Y11_SAFE_OK,
    Y11_SAFE_UNSUPPORTED_VER, Y11SafeStatus,
};

/// struct y11_handshake_req (include/y11_safe.h).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Y11HandshakeReq {
    pub byte_order: u8,
    pub major_version: u16,
    pub minor_version: u16,
    pub auth_proto_len: u16,
    pub auth_data_len: u16,
    pub skip_bytes: u32,
}

/*
 * Parse the 12-byte fixed part of the SetupRequest, decoding the
 * integer fields in the byte order byte 0 names: 'B' (0x42) reads
 * multi-byte fields MSB-first, 'l' (0x6c) LSB-first.  skip_bytes
 * carries the padded authorization byte count (not the fixed part),
 * so the caller waits for 12 + skip_bytes before the handshake ends.
 */
#[no_mangle]
pub extern "C" fn y11_handshake_parse_header(
    buf: *const u8,
    len: usize,
    out: *mut Y11HandshakeReq,
) -> Y11SafeStatus {
    if buf.is_null() || out.is_null() || len < 12 {
        return Y11_SAFE_INSUFFICIENT_DATA;
    }
    let b = unsafe { core::slice::from_raw_parts(buf, 12) };

    let byte_order = b[0];
    if byte_order != b'l' && byte_order != b'B' {
        return Y11_SAFE_INVALID_ENDIAN;
    }
    let be = byte_order == b'B';
    let read16 = if be { get16be } else { get16 };

    let major_version = read16(b, 2);
    if major_version != 11 {
        return Y11_SAFE_UNSUPPORTED_VER;
    }
    let minor_version = read16(b, 4);
    let auth_proto_len = read16(b, 6);
    let auth_data_len = read16(b, 8);

    /* total = 12 + n + pad(n) + d + pad(d), all checked; u16 bounds
       keep the sum far below u32::MAX, so LENGTH_OVERFLOW is purely
       defensive. */
    let total = 12u32
        .checked_add(u32::from(auth_proto_len))
        .and_then(|t| t.checked_add(pad_delta(u32::from(auth_proto_len))))
        .and_then(|t| t.checked_add(u32::from(auth_data_len)))
        .and_then(|t| t.checked_add(pad_delta(u32::from(auth_data_len))));
    let total = match total {
        Some(t) => t,
        None => return Y11_SAFE_LENGTH_OVERFLOW,
    };

    unsafe {
        *out = Y11HandshakeReq {
            byte_order,
            major_version,
            minor_version,
            auth_proto_len,
            auth_data_len,
            skip_bytes: total - 12,
        };
    }
    Y11_SAFE_OK
}
