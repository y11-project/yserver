//! y11_safe - wire-protocol validators for the y11 display server.
//!
//! #![no_std] static library linked into the C server as liby11_safe.a.
//! The C-ABI contract is include/y11_safe.h; thorn-rs audits the
//! exported symbols against that header.  Phase 0 carries stub
//! implementations; the full parsers land per docs/Y11_SAFE_AUDIT.md.
//!
//! SPDX-License-Identifier: BSD-2-Clause

#![no_std]
#![crate_name = "y11_safe"]

mod error;
mod frame;
mod handshake;

pub use error::Y11SafeStatus;
pub use frame::{y11_frame_request, Y11RequestFrame};
pub use handshake::{y11_handshake_parse_header, Y11HandshakeReq};

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
