//! Value-list payload bounds (Phase 2).
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    Y11_SAFE_LENGTH_OVERFLOW, Y11_SAFE_OK, Y11_SAFE_PAYLOAD_MISMATCH,
    Y11SafeStatus,
};

/*
 * A value list carries exactly one 4-byte value per set mask bit, in
 * increasing bit order.  A u32 mask sets at most 32 bits (128
 * bytes), so the checked multiply guards form, not reachability.
 * Bits outside a request's valid-mask are caller policy; this check
 * is pure arithmetic.
 */
#[no_mangle]
pub extern "C" fn y11_validate_value_mask(
    value_mask: u32,
    payload_len: usize,
) -> Y11SafeStatus {
    let count = value_mask.count_ones() as usize;
    let expected = match count.checked_mul(4) {
        Some(n) => n,
        None => return Y11_SAFE_LENGTH_OVERFLOW,
    };
    if expected != payload_len {
        return Y11_SAFE_PAYLOAD_MISMATCH;
    }
    Y11_SAFE_OK
}
