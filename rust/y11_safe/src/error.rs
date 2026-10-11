//! Status codes mirroring y11_safe_status (include/y11_safe.h) and the
//! little-endian wire readers shared by the validators.
//!
//! SPDX-License-Identifier: BSD-2-Clause

// The INVALID_HEADER / LENGTH_OVERFLOW / UNSUPPORTED_VER /
// INVALID_ENDIAN clauses go live with the Phase 1 parsers.
#![allow(dead_code)]

pub type Y11SafeStatus = u32;

pub const Y11_SAFE_OK: Y11SafeStatus = 0;
pub const Y11_SAFE_INSUFFICIENT_DATA: Y11SafeStatus = 1;
pub const Y11_SAFE_INVALID_HEADER: Y11SafeStatus = 2;
pub const Y11_SAFE_LENGTH_OVERFLOW: Y11SafeStatus = 3;
pub const Y11_SAFE_UNSUPPORTED_VER: Y11SafeStatus = 4;
pub const Y11_SAFE_INVALID_ENDIAN: Y11SafeStatus = 5;
pub const Y11_SAFE_PAYLOAD_MISMATCH: Y11SafeStatus = 6;

/// Little-endian CARD16 at a byte offset inside a borrowed slice.
/// All wire fields are little-endian; there is no byte swapping.
pub(crate) fn get16(buf: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([buf[off], buf[off + 1]])
}
