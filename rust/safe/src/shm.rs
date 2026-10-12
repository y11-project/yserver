//! MIT-SHM bounds arithmetic (Phase 4).
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    Y11_SAFE_LENGTH_OVERFLOW, Y11_SAFE_OK, Y11_SAFE_PAYLOAD_MISMATCH,
    Y11SafeStatus,
};

/*
 * Byte extent a ShmPutImage blit reads from the client's segment:
 * depth 1 rows pack to 4-byte bitmap strides, everything else is
 * 32bpp.  All arithmetic is checked; a zero width, height or source
 * extent blits nothing (the caller's early exit).  Mirrors the C
 * check in y11_shm_put_image — divergence fails the shadow build.
 */
#[no_mangle]
pub extern "C" fn y11_shm_bounds(
    total_width: u32,
    src_x: u32,
    src_y: u32,
    src_w: u32,
    src_h: u32,
    depth: u8,
    offset: u32,
    seg_size: u64,
) -> Y11SafeStatus {
    if total_width == 0 || src_w == 0 || src_h == 0 {
        return Y11_SAFE_OK;
    }

    let Some(tw) = (total_width as u64).checked_add(31) else {
        return Y11_SAFE_LENGTH_OVERFLOW;
    };
    let stride = if depth == 1 {
        let Some(words) = tw.checked_div(32) else {
            return Y11_SAFE_LENGTH_OVERFLOW;
        };
        words.checked_mul(4)
    } else {
        (total_width as u64).checked_mul(4)
    };
    let Some(stride) = stride else {
        return Y11_SAFE_LENGTH_OVERFLOW;
    };

    let Some(bottom) = src_y.checked_add(src_h) else {
        return Y11_SAFE_LENGTH_OVERFLOW;
    };
    let Some(bottom) = bottom.checked_sub(1) else {
        return Y11_SAFE_LENGTH_OVERFLOW;
    };
    let Some(last_col) = src_x.checked_add(src_w) else {
        return Y11_SAFE_LENGTH_OVERFLOW;
    };
    let row_bytes = if depth == 1 {
        let Some(e) = (last_col as u64).checked_add(7) else {
            return Y11_SAFE_LENGTH_OVERFLOW;
        };
        e.checked_div(8)
    } else {
        (last_col as u64).checked_mul(4)
    };
    let Some(row_bytes) = row_bytes else {
        return Y11_SAFE_LENGTH_OVERFLOW;
    };

    let Some(rows_bytes) = (bottom as u64).checked_mul(stride) else {
        return Y11_SAFE_LENGTH_OVERFLOW;
    };
    let Some(head) = (offset as u64).checked_add(rows_bytes) else {
        return Y11_SAFE_LENGTH_OVERFLOW;
    };
    let Some(need) = head.checked_add(row_bytes) else {
        return Y11_SAFE_LENGTH_OVERFLOW;
    };

    if (last_col as u64) > total_width as u64 {
        return Y11_SAFE_PAYLOAD_MISMATCH;
    }
    if stride > 0 {
        let Some(rows_fit) = seg_size.checked_div(stride) else {
            return Y11_SAFE_LENGTH_OVERFLOW;
        };
        if (src_y as u64) + (src_h as u64) > rows_fit {
            return Y11_SAFE_PAYLOAD_MISMATCH;
        }
    }
    if need > seg_size {
        return Y11_SAFE_PAYLOAD_MISMATCH;
    }
    Y11_SAFE_OK
}
