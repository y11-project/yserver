//! safe - wire-protocol validators for the y11 display server.
//!
//! #![no_std] static library linked into the C server as libsafe.a.
//! The C-ABI contract is include/safe.h; thorn-rs audits the
//! exported symbols against that header.  Phase 0 carries stub
//! implementations; the full parsers land per docs/Y11_SAFE_AUDIT.md.
//!
//! SPDX-License-Identifier: BSD-2-Clause

#![no_std]
#![crate_name = "safe"]

mod error;
mod error_frame;
mod frame;
mod list;
mod handshake;
mod mask;
mod opcode;
mod property;
mod reply;
mod shm;

pub use error::Y11SafeStatus;
pub use error_frame::y11_format_error;
pub use frame::{y11_frame_request, Y11RequestFrame};
pub use handshake::{y11_handshake_parse_header, Y11HandshakeReq};
pub use mask::y11_validate_value_mask;
pub use list::y11_validate_list;
pub use opcode::y11_opcode_min_len;
pub use property::y11_validate_property_payload;
pub use reply::y11_validate_reply_size;
pub use shm::y11_shm_bounds;

use core::panic::PanicInfo;

/// No unwinding in the server process: a panic parks the core.  The
/// panic strategy is -C panic=abort, so this exists only to satisfy
/// the language item; validators are total functions and must not
/// reach it.
#[panic_handler]
fn panic(_: &PanicInfo<'_>) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

/*
 * Sysroot core objects carry DW.ref.rust_eh_personality metadata even
 * when the crate links with -C panic=abort (stable toolchains compile
 * core once, unwind-capable); anything pulling those objects into a
 * C link then misses the symbol.  With abort semantics the
 * personality is never called, so a stub definition satisfies the
 * linker without enabling unwinding.
 */
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}
