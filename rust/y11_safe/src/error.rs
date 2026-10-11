//! Status codes mirroring y11_safe_status (include/y11_safe.h) and the
//! wire readers shared by the validators.
//!
//! SPDX-License-Identifier: BSD-2-Clause

pub type Y11SafeStatus = u32;

pub const Y11_SAFE_OK: Y11SafeStatus = 0;
pub const Y11_SAFE_INSUFFICIENT_DATA: Y11SafeStatus = 1;
pub const Y11_SAFE_INVALID_HEADER: Y11SafeStatus = 2;
pub const Y11_SAFE_LENGTH_OVERFLOW: Y11SafeStatus = 3;
pub const Y11_SAFE_UNSUPPORTED_VER: Y11SafeStatus = 4;
pub const Y11_SAFE_INVALID_ENDIAN: Y11SafeStatus = 5;
pub const Y11_SAFE_PAYLOAD_MISMATCH: Y11SafeStatus = 6;

/// Little-endian CARD16 at a byte offset inside a borrowed slice.
/// The request stream is always little-endian: only byte-order 'l'
/// connections are admitted, so the framer needs no per-client order.
pub(crate) fn get16(buf: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([buf[off], buf[off + 1]])
}

/// Big-endian CARD16 at a byte offset (big-endian handshakes only).
pub(crate) fn get16be(buf: &[u8], off: usize) -> u16 {
    u16::from_be_bytes([buf[off], buf[off + 1]])
}

/// Little-endian CARD32 at a byte offset inside a borrowed slice.
pub(crate) fn get32(buf: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

/// Padding delta to the next 4-byte boundary: the wire's pad(x) =
/// (4 - (x % 4)) % 4.  Request fields add their own length plus this.
pub(crate) fn pad_delta(x: u32) -> u32 {
    (4 - (x % 4)) % 4
}
