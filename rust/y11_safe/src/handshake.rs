//! SetupRequest header floor checks (stub: Phase 0).
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    get16, Y11_SAFE_INSUFFICIENT_DATA, Y11_SAFE_OK, Y11SafeStatus,
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
 * Stub: the 12-byte fixed part only.  Byte-order and version clauses
 * (INVALID_ENDIAN, UNSUPPORTED_VER) and the full skip validation are
 * Phase 1; until then a short buffer reports INSUFFICIENT_DATA and
 * anything else parses with the raw fields copied out.
 */
#[no_mangle]
pub extern "C" fn y11_handshake_parse_header(
    buf: *const u8,
    len: usize,
    out: *mut Y11HandshakeReq,
) -> Y11SafeStatus {
    if len < 12 || buf.is_null() || out.is_null() {
        return Y11_SAFE_INSUFFICIENT_DATA;
    }
    let b = unsafe { core::slice::from_raw_parts(buf, 12) };
    let req = Y11HandshakeReq {
        byte_order: b[0],
        major_version: get16(b, 2),
        minor_version: get16(b, 4),
        auth_proto_len: get16(b, 6),
        auth_data_len: get16(b, 8),
        skip_bytes: 0,
    };
    unsafe { *out = req };
    Y11_SAFE_OK
}
