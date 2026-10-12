//! Outbound reply sizing and pad auditing (Phase 5).
//!
//! SPDX-License-Identifier: BSD-2-Clause

use crate::error::{
    get16, get32, Y11_SAFE_INSUFFICIENT_DATA, Y11_SAFE_INVALID_HEADER,
    Y11_SAFE_LENGTH_OVERFLOW, Y11_SAFE_OK, Y11_SAFE_PAYLOAD_MISMATCH,
    Y11SafeStatus,
};

/*
 * Core opcodes whose reply shape is fully fixed: the wire length is a
 * constant, so anything else is a construction bug.  GetWindowAttributes
 * is 44 bytes (3 words) and QueryKeymap 40 (2 words); the rest are
 * bare 32-byte frames.  Their unused tail is zero by construction —
 * y11 memsets every reply struct — so the count check doubles as the
 * leak guard for those replies.
 */
fn fixed_words(opcode: u8) -> Option<u32> {
    match opcode {
        3 => Some(3),    /* GetWindowAttributes */
        14 => Some(0),    /* GetGeometry */
        23 => Some(0),    /* GetSelectionOwner */
        38 => Some(0),    /* QueryPointer */
        40 => Some(0),    /* TranslateCoords */
        43 => Some(0),    /* GetInputFocus */
        44 => Some(2),    /* QueryKeymap */
        _ => None,
    }
}

/*
 * Wire padding a variable payload needs: pad(x) = (4 - x % 4) % 4 on
 * the content byte count.
 */
fn pad_delta(x: u64) -> u64 {
    (4 - (x % 4)) % 4
}

/*
 * Audit one complete reply: the CARD32 length must account for every
 * buffered byte (32 + words * 4, checked), fixed-shape opcodes must
 * carry their exact count, and for the two string/item replies the
 * embedded count plus padding must reconcile and the pad bytes must
 * read zero — trailing uninitialized heap is a leak.  out_buf may be
 * NULL to skip the pad inspection (split-reply accounting in C has no
 * contiguous buffer to hand over).
 */
#[no_mangle]
pub extern "C" fn y11_validate_reply_size(
    major_opcode: u8,
    reply_len_words: u32,
    actual_total_bytes: usize,
    out_buf: *const u8,
) -> Y11SafeStatus {
    if actual_total_bytes < 32 {
        return Y11_SAFE_INSUFFICIENT_DATA;
    }
    let trailing = match reply_len_words.checked_mul(4) {
        Some(t) => t,
        None => return Y11_SAFE_LENGTH_OVERFLOW,
    };
    let expected = match trailing.checked_add(32) {
        Some(t) => t as usize,
        None => return Y11_SAFE_LENGTH_OVERFLOW,
    };
    if actual_total_bytes != expected {
        return Y11_SAFE_PAYLOAD_MISMATCH;
    }
    if let Some(w) = fixed_words(major_opcode) {
        if reply_len_words != w {
            return Y11_SAFE_INVALID_HEADER;
        }
    }

    if out_buf.is_null() {
        return Y11_SAFE_OK;
    }
    let buf = unsafe { core::slice::from_raw_parts(out_buf, expected) };

    /*
     * Grammar-backed pad audits: GetProperty (20) and GetAtomName (17)
     * are the replies whose payloads can end mid-word.  The embedded
     * count fixes the content length; the declared words must cover
     * content plus padding exactly, and the padding bytes must be zero.
     */
    match major_opcode {
        20 => {
            let format = buf[1];
            if format != 0 {
                if format != 8 && format != 16 && format != 32 {
                    return Y11_SAFE_INVALID_HEADER;
                }
                let items = u64::from(get32(buf, 16));
                let content = match items.checked_mul((format / 8) as u64) {
                    Some(c) => c,
                    None => return Y11_SAFE_LENGTH_OVERFLOW,
                };
                let pad = pad_delta(content);
                let content = match content.checked_add(pad) {
                    Some(c) => c,
                    None => return Y11_SAFE_LENGTH_OVERFLOW,
                };
                if content != trailing as u64 {
                    return Y11_SAFE_PAYLOAD_MISMATCH;
                }
                for b in &buf[(32 + content - pad) as usize..expected] {
                    if *b != 0 {
                        return Y11_SAFE_PAYLOAD_MISMATCH;
                    }
                }
            }
        }
        17 => {
            let nlen = u64::from(get16(buf, 8));
            let pad = pad_delta(nlen);
            let content = match nlen.checked_add(pad) {
                Some(c) => c,
                None => return Y11_SAFE_LENGTH_OVERFLOW,
            };
            if content != trailing as u64 {
                return Y11_SAFE_PAYLOAD_MISMATCH;
            }
            for b in &buf[(32 + content - pad) as usize..expected] {
                if *b != 0 {
                    return Y11_SAFE_PAYLOAD_MISMATCH;
                }
            }
        }
        _ => {}
    }
    Y11_SAFE_OK
}
