//! Fixed-part + element list validation (Phase 4).
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    Y11_SAFE_INVALID_HEADER, Y11_SAFE_LENGTH_OVERFLOW, Y11_SAFE_OK,
    Y11_SAFE_PAYLOAD_MISMATCH, Y11SafeStatus,
};

/*
 * A request made of a fixed part plus N same-size elements: the
 * remainder after the fixed part must divide exactly.  n_elements
 * receives the quotient when non-null.  Element shapes here are the
 * simple ones (points, rectangles, notify entries); glyph and text
 * lists have per-entry headers and stay in C.
 */
#[no_mangle]
pub extern "C" fn y11_validate_list(
    avail: usize,
    fixed_part: usize,
    element_size: u32,
    n_elements: *mut u32,
) -> Y11SafeStatus {
    if element_size == 0 {
        return Y11_SAFE_INVALID_HEADER;
    }
    if avail < fixed_part {
        return Y11_SAFE_PAYLOAD_MISMATCH;
    }
    let rest = avail - fixed_part;
    let rest = rest as u64;
    let elem = element_size as u64;
    if rest % elem != 0 {
        return Y11_SAFE_PAYLOAD_MISMATCH;
    }
    let n = rest / elem;
    if n > u32::MAX as u64 {
        return Y11_SAFE_LENGTH_OVERFLOW;
    }
    if !n_elements.is_null() {
        unsafe { *n_elements = n as u32 };
    }
    Y11_SAFE_OK
}
