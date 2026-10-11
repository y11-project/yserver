//! Opcode minimum-length table (Phase 3).
//!
//! SPDX-License-Identifier: BSD-2-Clause

/// Total request bytes (header included) the core protocol requires per
/// opcode; 4 for extensions and unlisted opcodes.  Value-mask opcodes
/// count only their fixed part — the value list varies with the mask.
pub const OPCODE_MIN_LEN: [u16; 256] = {
    let mut t = [4u16; 256];
    t[1] = 32;   // CreateWindow
    t[2] = 12;   // ChangeWindowAttributes
    t[3] = 8;    // GetWindowAttributes
    t[4] = 8;    // DestroyWindow
    t[5] = 8;    // DestroySubwindows
    t[6] = 8;    // ChangeSaveSet
    t[7] = 16;   // ReparentWindow
    t[8] = 8;    // MapWindow
    t[9] = 8;    // MapSubwindows
    t[10] = 8;   // UnmapWindow
    t[11] = 8;   // UnmapSubwindows
    t[12] = 12;  // ConfigureWindow
    t[13] = 8;   // CirculateWindow
    t[14] = 8;   // GetGeometry
    t[15] = 8;   // QueryTree
    t[16] = 8;   // InternAtom
    t[17] = 8;   // GetAtomName
    t[18] = 24;  // ChangeProperty
    t[19] = 12;  // DeleteProperty
    t[20] = 24;  // GetProperty
    t[21] = 8;   // ListProperties
    t[22] = 16;  // SetSelectionOwner
    t[23] = 8;   // GetSelectionOwner
    t[24] = 24;  // ConvertSelection
    t[25] = 44;  // SendEvent
    t[26] = 24;  // GrabPointer
    t[27] = 8;   // UngrabPointer
    t[28] = 24;  // GrabButton
    t[29] = 12;  // UngrabButton
    t[30] = 16;  // ChangeActivePointerGrab
    t[31] = 16;  // GrabKeyboard
    t[32] = 8;   // UngrabKeyboard
    t[33] = 16;  // GrabKey
    t[34] = 12;  // UngrabKey
    t[35] = 8;   // AllowEvents
    t[36] = 4;   // GrabServer
    t[37] = 4;   // UngrabServer
    t[38] = 8;   // QueryPointer
    t[39] = 16;  // GetMotionEvents
    t[40] = 16;  // TranslateCoords
    t[41] = 24;  // WarpPointer
    t[42] = 12;  // SetInputFocus
    t[43] = 4;   // GetInputFocus
    t[44] = 4;   // QueryKeymap
    t[45] = 12;  // OpenFont
    t[46] = 8;   // CloseFont
    t[47] = 8;   // QueryFont
    t[48] = 8;   // QueryTextExtents
    t[49] = 8;   // ListFonts
    t[50] = 8;   // ListFontsWithInfo
    t[51] = 8;   // SetFontPath
    t[52] = 4;   // GetFontPath
    t[53] = 16;  // CreatePixmap
    t[54] = 8;   // FreePixmap
    t[55] = 16;  // CreateGC
    t[56] = 12;  // ChangeGC
    t[57] = 16;  // CopyGC
    t[58] = 12;  // SetDashes
    t[59] = 12;  // SetClipRectangles
    t[60] = 8;   // FreeGC
    t[61] = 16;  // ClearArea
    t[62] = 28;  // CopyArea
    t[63] = 32;  // CopyPlane
    t[64] = 12;  // PolyPoint
    t[65] = 12;  // PolyLine
    t[66] = 12;  // PolySegment
    t[67] = 12;  // PolyRectangle
    t[68] = 12;  // PolyArc
    t[69] = 12;  // FillPoly
    t[70] = 12;  // PolyFillRectangle
    t[71] = 12;  // PolyFillArc
    t
};

/// O(1) minimum request length for an opcode, for C-side gating.
#[no_mangle]
pub extern "C" fn y11_opcode_min_len(opcode: u8) -> u16 {
    OPCODE_MIN_LEN[opcode as usize]
}
