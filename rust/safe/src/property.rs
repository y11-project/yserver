//! Property payload stride validation (Phase 2).
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    Y11_SAFE_INVALID_HEADER, Y11_SAFE_LENGTH_OVERFLOW, Y11_SAFE_OK,
    Y11_SAFE_PAYLOAD_MISMATCH, Y11SafeStatus,
};

/*
 * ChangeProperty payload: num_items units of format width, padded to
 * the 4-byte wire boundary.  The body must carry exactly the padded
 * count — trailing slack is a mismatch too, so callers pass the
 * request's remaining bytes, not the client-declared size.  A u32
 * item count against a usize byte count cannot overflow on any
 * supported word size; the checked ops keep that true portably.
 */
#[no_mangle]
pub extern "C" fn y11_validate_property_payload(
    format: u8,
    num_items: u32,
    actual_bytes: usize,
) -> Y11SafeStatus {
    if format != 8 && format != 16 && format != 32 {
        return Y11_SAFE_INVALID_HEADER;
    }
    let unit_size = (format / 8) as usize;
    let unpadded = match (num_items as usize).checked_mul(unit_size) {
        Some(n) => n,
        None => return Y11_SAFE_LENGTH_OVERFLOW,
    };
    let pad = (4 - (unpadded % 4)) % 4;
    let expected = match unpadded.checked_add(pad) {
        Some(n) => n,
        None => return Y11_SAFE_LENGTH_OVERFLOW,
    };
    if expected != actual_bytes {
        return Y11_SAFE_PAYLOAD_MISMATCH;
    }
    Y11_SAFE_OK
}
