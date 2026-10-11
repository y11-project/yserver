# y11 Wire-Protocol Audit & Rust Migration Specification

Audit of every site that touches raw client byte streams, and the plan for
moving the pure parsing layer to a `#![no_std]` Rust static library
(`libsafe.a`) while keeping socket I/O, DRM/KMS and rendering in C99.

Scope: `src/*.c`, `include/y11.h`, `include/y11_wire.h`.
All line references are as of the audit commit; hazards marked **fixed**
were repaired before this document landed.

Wire-shape conventions relied on throughout:

- The dispatcher hands each handler `(pkt, len, data_off)` where
  `data_off` is 4 (normal framing) or 8 (BIG-REQUESTS framing) and
  `len >= data_off` is guaranteed by `y11_client_requests()`. Every
  `len - data_off` expression in a handler is therefore underflow-free.
- The receive buffer is capped at `Y11_INBUF_MAX` (16,777,220 bytes);
  a request declaring more is fatal at framing time.
- The server is little-endian only: byte-order `'B'` is refused at the
  handshake (phase 1), so no byte swapping exists anywhere.

## 1. Vulnerability & Parser Site Inventory

### 1.1 Connection & framing — `src/client.c`

| Location | Stage | Hazard | Current C implementation | Severity |
|---|---|---|---|---|
| `y11_client_handshake:527` | SetupRequest | unaligned cast; byte-order; padding math | 12-byte header wait; struct overlay of `c->in_buf` (offset 0, naturally aligned — safe); `'l'` only, `'B'` refused; version 11 check; `skip = pad4(proto_len) + pad4(data_len)` ≤ 131078, computed in `uint32_t` (no wrap), availability checked before advancing | Low (compat: no big-endian) |
| `y11_client_handshake:527` | SetupRequest | resource exhaustion (slow loris) | an incomplete handshake can park a connection forever holding up to `Y11_INBUF_MAX` of buffer; bounded by `Y11_MAX_CLIENTS` (64 × 16 MiB ≈ 1 GiB), no per-connection timeout exists | Medium (DoS) |
| `y11_client_read:180` | ancillary data | fd exhaustion | SCM_RIGHTS fds harvested with `cmsg_len` arithmetic; `in_fds` capped at 8 slots, excess closed immediately; fds released at disconnect | Low (correct) |
| `y11_client_requests:573` | request framing | length × 4 wraps | `wire_len` (CARD16) × 4 ≤ 262,140 in `size_t` — no wrap on LP64; BIG-REQUESTS `true_len` (CARD32) × 4 checked `> Y11_INBUF_MAX` before use, `< 2` fatal; incomplete requests wait, never exceed the cap | Low on LP64; **documented LP64 assumption**: on a 32-bit `size_t` the `× 4` wraps — the codebase is LP64-only |
| `y11_client_requests:581` | BIG-REQUESTS | `length == 0` dual meaning | `wire_len == 0` with the extension disabled is treated as a 4-byte request (Xorg behavior); with it enabled, an 8-byte extended header whose `true_len` is validated as above | Low (correct) |
| `y11_client_send:326` | output path | unbounded growth (DoS) | `out_cap` doubles on demand with **no maximum**; a client that stops reading while triggering large replies (e.g. repeated `GetImage` of a huge area) grows server memory without bound | **Medium (DoS)** — fix: mirror `Y11_INBUF_MAX` cap + wants_close |
| `y11_conn_setup_req` overlay at `client.c:535` | SetupRequest | struct cast of raw buffer | one-time overlay of the malloc'd (aligned) buffer at offset 0; the only raw-cast parse site in the tree; all other parsing goes through `y11_wire_get*` | Low (safe as written) |

### 1.2 Dispatcher — `src/dispatch.c`

| Location | Request | Hazard | Current C implementation | Severity |
|---|---|---|---|---|
| `y11_dispatch_req:1336` | generic | slice-before-check class | computes `data_off` (4/8), guarantees `len >= data_off`; per-request handlers below do their own `len - data_off` checks | Low (invariant, see header of this doc) |
| `y11_dispatch_send_event` | SendEvent | event-length confusion | strict `len - data_off == 40`, event-mask `& ~ALL_VALID` rejected, 32-byte event copied with the send-event flag forced | Low (correct) |
| `y11_dispatch_xtest` | XTEST FakeInput | injection payload | strict `len - data_off == 32`, type/detail validated | Low (correct) |
| `y11_dispatch_accept_resource` | ~15 requests (PolyArc, FillPoly, PolyText8/16, ImageText8/16, StoreColors, FreeColors, AllowEvents, …) | none — payloads never read | no-op: accepts and discards; no read can overrun, but variable-length drawing requests are silently unimplemented | Low (conformance gap, no memory risk) |

### 1.3 Value-mask requests (the extraction source)

| Location | Request | Current C implementation | Severity |
|---|---|---|---|
| `y11_window_req_create:753` | CreateWindow | `avail < 28` fixed part; `value_mask & ~Y11_CWA_ALL_VALID` → BadValue; `popcount(mask) × 4 ≤ avail - 28`; per-bit walk `y11_window_parse_cwa` | Low (correct — canonical pattern) |
| `y11_window_req_change_attributes:963` | ChangeWindowAttributes | `avail < 8` + same mask/popcount discipline | Low (correct) |
| `y11_window_req_configure:1428` | ConfigureWindow | mask truncated to 16 bits (CWX is a CARD16 mask per protocol), `avail < 8`, popcount check | Low (correct) |
| `y11_gc_req_create` / `y11_gc_req_change` (`src/gc.c:202,283`) | CreateGC / ChangeGC | `avail < 12`/`< 8`; `& ~Y11_GC_MASK_ALL` → BadValue; `popcount_gc × 4 ≤ avail` | Low (correct) |

### 1.4 Variable-length payloads

| Location | Request | Current C implementation | Severity |
|---|---|---|---|
| `y11_atom_req_intern` (`src/atom.c`) | InternAtom | `name_len` + `pad4(name_len)` checked against `avail`; interning reads exactly `name_len` bytes | Low (correct) |
| `y11_property_req_change` (`src/property.c`) | ChangeProperty | mode in header byte 1; `format ∈ {8,16,32}` → BadValue; `nbytes = nunits × format / 8` in `size_t`; `pad4(nbytes)` checked against remaining | Low on LP64 (same 32-bit caveat) |
| `y11_property_req_get` | GetProperty | `long_offset` clamped to item count, `long_length` clamped; reply length `pad4(count × unit) / 4` fits CARD32 | Low (correct) |
| `y11_render_req_poly_point`, `poly_segment`, `poly_rectangle`, `poly_fill_rectangle` (`src/render.c`) | core lists | fixed part + strict `% 4` / `% 8` divisibility of the remainder, element count by division | Low (correct) |
| `y11_render_req_put_image` | PutImage | format < 3, depth/drawable match, `data_len` validated against `width × height × 4` (ZPixmap) and plane strides (XYBitmap) before any row loop | Low (correct) |
| `y11_shm_put_image` (`src/shm.c`) | MIT-SHM PutImage | stride and `need` computed in `uint64_t`; `need > seg->size`, segment ownership and geometry validated before the blit — the strongest pattern in the tree | Low (correct) |
| AddGlyphs (`src/xrender.c:729`) | RENDER glyph upload | `nglyphs ≤ (avail-8)/4`; id list + 12-byte headers + bitmaps all bounded against `avail` with 64-bit arithmetic | Low (correct) |
| `y11_present` notify lists (`src/present.c:207`) | Present | `≥ 68` fixed + `& 7` divisibility of the remainder | Low (correct) |
| dri3 requests (`src/dri3.c:278,379,565`) | DRI3 | fixed 20/4/12-byte bodies; fd lifecycle through the capped `in_fds` | Low (correct) |
| selection, saver, damage, cursor requests | misc | all fixed-size with exact `len - data_off` checks; `CreateCursor` allocates `width × height × 4` (≤ 17 GiB) with a NULL check | Low (allocation-failure handled; a giant alloc attempt is a malloc-NULL path) |

### 1.5 Defects found by this audit

All three are the same class — **payload reads before length validation**,
reachable by any connected client with a header-only request
(`wire_len == 1`):

| Location | Request | Hazard | Severity | Status |
|---|---|---|---|---|
| `y11_input_req_get_keyboard_mapping` (`src/input.c`) | GetKeyboardMapping | `(void)len;` then reads `pkt[data_off]`, `pkt[data_off+1]` — 2-byte read past a 4-byte request; a packet at the end of the reassembly buffer reads past the heap chunk | High (remote OOB read) | **fixed**: strict `len - data_off == 4` → BadLength |
| `y11_xkb_select_events` (`src/xkb.c`) | XKB SelectEvents | read `pkt+6..15` with no length guard (introduced with StateNotify support; caught by this audit within days) — up to 12 bytes past a 4-byte request; `pkt+18` was guarded but the fixed part was not | High (remote OOB read) | **fixed**: `len < 16` → BadLength |
| `y11_glx_req` CreateContext cases (`src/glx.c`) | GLX | `y11_wire_get32(pkt + data_off)` with no check — 4 bytes past; GLX is unadvertised but the opcode still dispatches, so a blind opcode probe reaches it | High (remote OOB read) | **fixed**: `< 4` → BadLength |

Regression evidence: a loopback fuzz probe (4-byte requests for each
opcode) now gets `BadLength` for all three and the server survives.
That probe is the seed of the Phase 0 harness in §4.

### 1.6 Conformance quirks (no memory hazard)

| Location | Quirk |
|---|---|
| `y11_input_req_change_keyboard_mapping` (`src/input.c`) | interprets header byte 1 as *count* and payload byte 0 as *first-keycode*; Xproto puts first-keycode in byte 1 and nKeycodes at payload byte 0 — swapped. The `(uint8_t)(first + i)` truncation keeps the override-table index in `[8, 255]` (mod-256 aliasing, no OOB write), but real `XChangeKeyboardMapping` clients corrupt unrelated rows. Fix together with the Rust rewrite of the length math |
| `y11_glx_query_version` / `get_visual_configs` (`src/glx.c`) | check whole-request `len` instead of `len - data_off`, so a BIG-REQUESTS-framed form is rejected/misread. Dormant path (GLX unadvertised) |
| `y11_client_send` | output buffering is uncapped (§1.1) — the only unbounded-memory path in the tree |

## 2. Separation of Concerns Matrix

### 2.1 Migrate to Rust — `libsafe.a` (`#![no_std]`, zero heap)

Pure functions over borrowed bytes. No allocator, no statics, no I/O;
every output is by caller-provided POD out-parameter.

| Component | Source today | Rust API (§3) |
|---|---|---|
| Handshake header validation | `y11_client_handshake` arithmetic (skip computation, order/version checks) | `y11_safe_handshake_parse_header` |
| Request framing decode (incl. BIG-REQUESTS) | `y11_client_requests` length math + `Y11_INBUF_MAX` policy | `y11_safe_frame_request` |
| Value-mask validation | the window.c/gc.c popcount discipline (§1.3) | `y11_validate_value_mask` |
| Property payload validation | `y11_property_req_change/get` format/stride math | `y11_validate_property_payload` |
| List payload validation | render.c/xrender.c/present.c divisibility checks (§1.4) | `y11_safe_validate_list` |
| SHM bounds math | the uint64 stride/need formulas in `y11_shm_put_image` | `y11_safe_shm_bounds` |
| Error frame formatting | `y11_dispatch_send_error` / `y11_dispatch_bad_length` bytes | `y11_format_error_frame` |
| Reply length sizing | the `pad4(...)/4` CARD32 computations scattered across reply builders | `y11_safe_reply_length_words` |

### 2.2 Retained in C99 — `y11` core

| Component | Why it stays in C |
|---|---|
| `client.c` socket loop, `recvmsg`/`sendmsg`, SCM_RIGHTS fd lifecycle, poll integration (`main.c`) | POSIX I/O and descriptor ownership; Rust `no_std` has no syscall layer here and the fd semantics (MSG_CMSG helpers, close-on-overflow) are intertwined with the event loop |
| Reassembly buffer state machine (`in_buf` growth, compaction, `wants_close`) | stateful, socket-coupled; calls the Rust framers per packet |
| Window tree, resource table, atom interning, property *storage* | long-lived heap state and cross-request invariants; parsers only validate bytes |
| MIT-SHM segments (mmap ownership, purge), DRI3 fd plumbing, Present fences, DRM/KMS/scanout, evdev, libseat session | kernel interfaces and process-global state; no parsing of client-controlled streams beyond what the validators already cover |
| All rendering (core + RENDER composite, glyph stores, cursor planes) | performance; byte-validation is upstream of these paths, not inside them |
| GLX bridge | dormant (unadvertised); keep the guards, delete or freeze the rest |

## 3. Proposed C-ABI Surface — `include/safe.h`

The contract header landed with Phase 0; `rust/safe` implements it
as a `#![no_std]` crate audited by thorn-rs against these
declarations. Rules: fixed-width fields only, no pointers inside
structs, slices cross as `(const uint8_t *buf, size_t len)` borrows,
every call returns `y11_safe_status`. Rust side: `#[no_mangle] pub
extern "C"`, `#[repr(C)]` structs.

```c
#ifndef SAFE_H
#define SAFE_H

#include <stddef.h>
#include <stdint.h>

/* Status codes; 0 is the only success. */
typedef enum {
    Y11_SAFE_OK = 0,
    Y11_SAFE_INSUFFICIENT_DATA,   /* well-formed prefix; buffer more */
    Y11_SAFE_INVALID_HEADER,      /* framing/opcode violation */
    Y11_SAFE_LENGTH_OVERFLOW,     /* declared size unrepresentable */
    Y11_SAFE_UNSUPPORTED_VER,      /* protocol major is not 11 */
    Y11_SAFE_INVALID_ENDIAN,      /* byte order is not little-endian */
    Y11_SAFE_PAYLOAD_MISMATCH     /* value/format/units vs bytes */
} y11_safe_status;

struct y11_handshake_req {   /* the 12-byte SetupRequest fixed part */
    uint8_t  byte_order;
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t auth_proto_len;
    uint16_t auth_data_len;
    uint32_t skip_bytes;     /* pad4(proto) + pad4(data) to buffer */
};

struct y11_request_frame {   /* one framed request */
    uint8_t  opcode;
    uint16_t wire_len;       /* 0 selects the BIG-REQUESTS form */
    uint8_t  is_bigreq;
    uint32_t true_len;       /* total bytes incl. the header */
};

/* Landed in Phase 0 (stub bodies; full parses are Phase 1). */
y11_safe_status y11_handshake_parse_header(const uint8_t *buf, size_t len,
                                           struct y11_handshake_req *out);
y11_safe_status y11_frame_request(const uint8_t *buf, size_t len,
                                  uint8_t big_req_enabled,
                                  struct y11_request_frame *out);
y11_safe_status y11_validate_property_payload(uint8_t format,
                                              uint32_t num_items,
                                              size_t actual_bytes);

#endif /* SAFE_H */
```

Landed with Phase 2: `y11_validate_value_mask(value_mask, payload_len)`
(pure popcount arithmetic; valid-mask policy stays with the caller)
and `y11_format_error(...)` (the canonical 32-byte frame, fields
encoded in the client byte order, sequence truncated to its CARD16
wire width).  `y11_validate_property_payload` moved to its own module
and tightened: format outside 8/16/32 is INVALID_HEADER and the padded
item count must match the request body exactly.

Remaining surface:

```c
/* Phase 3 */
y11_safe_status y11_safe_validate_list(const uint8_t *body, size_t avail,
                                       size_t fixed_part,
                                       uint32_t element_size,
                                       uint32_t *n_elements);
uint32_t y11_safe_reply_length_words(size_t payload_bytes,
                                     uint8_t *overflow);
```

Linkage notes for the thorn ecosystem:

- `libsafe.a` is a **plain C-ABI static library**, not a pith
  plugin: no `PITH_FFI_LONG` crossing, no `c_<author>_<module>_<fn>`
  mangling. Symbols keep their `y11_*` names exactly as declared here.
- Built as a single `#![no_std]` crate via `rust.crate()` in
  build.thorn (thorn-rs plans `rustc --crate-type=staticlib`), honoring
  the one-crate-per-C-link rule: all validators are one crate. `core`
  and `compiler_builtins` come along in the archive; nothing else may
  be linked. The audit edge (the `rust.crate` header argument) fails
  the build on any missing expected symbol.

## 4. Step-by-Step Migration Plan

Phases are ordered so the server stays shippable after each: validators
run *in front of* the existing C checks first (dual-verify), then the C
check is deleted only once the golden corpus proves byte-identical
behavior.

### Phase 0 — scaffolding and gates (no behavior change)

Landed: `include/safe.h` contract, the `rust/safe` `#![no_std]`
crate with stub bodies, build.thorn wiring through thorn-rs
(`rust.crate` + the `test_safe_stub` target), and the C smoke harness
(`tests/test_safe_stub.c`). Verified: `thorn build --engine ninja` →
`ninja` builds `libsafe.a`, the audit stamps
`.thorn-rs/y11_safe.verified` with zero missing symbols, the server
links with the archive and serves, and the stub test exits 0.

Still owed by later phases:

1. Land the loopback fuzz harness (C, `tests/wirefuzz.c`): a headless
   server + a client that replays (a) the per-opcode header-only probe
   that found §1.5, (b) a recorded corpus of well-formed requests from
   the existing raw-socket probes, (c) random truncations/bit flips.
   Gate: no crash, and byte-identical error frames per corpus entry.
2. Header changes after this point require a version bump of
   `include/safe.h` (frozen by the audit).
3. Track in CI: `make` (warnings must stay at zero) + wirefuzz green,
   alongside the existing dwm session smoke.

### Phase 1 — framers (handshake + request header)

1. Crate `y11-safe`: `lib.rs` with the handshake and frame parsers as
   pure functions; `#![no_std]`, `#![feature(...)]`-free, no `unsafe`
   needed (all slice access via `get()` on the borrowed `&[u8]`).
2. `build.thorn` for yserver: `rust.crate("y11-safe", path: "safe/")`
   + `engine.add_target` for `libsafe.a` +
   `engine.add_source` into the server link, keeping the unity build:
   the archive is a link object, not a translation unit — no
   rename-define interplay with `RENAME_DEFS`.
3. Wire `y11_client_handshake` / `y11_client_requests` to call the Rust
   framers and *keep* the C checks for one release (dual-verify).
   Differences abort the connection exactly as today.
4. Delete the duplicated C arithmetic; wirefuzz stays green.

### Phase 2 — value-mask and fixed-size table

Landed: `y11_validate_value_mask` and the tightened
`y11_validate_property_payload` (exact padded sizing) plus
`y11_format_error`, verified by tests/test_safe_phase2.c. The C
call-site adoption (CreateWindow/ChangeWindowAttributes/ConfigureWindow
/CreateGC/ChangeGC consulting the validator, and the error sender
collapsing onto the formatter) is the next step.

1. Move the §1.3 popcount discipline into
   `y11_validate_value_mask`; the three window.c sites and the two
   gc.c sites call it before `y11_window_parse_cwa` /
   `y11_gc_parse_values` walk bits.
2. Table-drive the ~40 fixed-size handlers: one `(opcode, body_len)`
   table consulted by the dispatcher before dispatch, so handlers
   with exact sizes (`== n` checks) no longer each re-derive it.
   `y11_dispatch_accept_resource` requests stay unparsed by design.
3. Fix the §1.6 ChangeKeyboardMapping semantics swap here — its
   length math (`4 + count × per_kc × 4`) moves into the validator,
   and the field interpretation is corrected to Xproto order with a
   corpus entry proving Xlib-shaped requests now land in the right
   rows.

### Phase 3 — variable payloads

1. `y11_safe_validate_list` replaces the `% 4` / `% 8` divisibility
   checks in render.c/xrender.c/present.c; `n_elements` feeds the
   existing draw loops unchanged.
2. `y11_validate_property_payload` takes over ChangeProperty
   format/nunits math and GetProperty slicing; `y11_safe_shm_bounds`
   absorbs the SHM stride/need formulas (still uint64 internally).
3. `y11_safe_reply_length_words` replaces the scattered CARD32 reply
   sizing; every overflow path returns BadLength instead of
   truncating.

### Phase 4 — error frames, output cap, and C retire

1. `y11_format_error_frame` becomes the single producer of error
   frames; `y11_dispatch_send_error` collapses to a thin call.
2. Fix the §1.1 output-buffer DoS in C (`y11_client_send` gets a
   `Y11_OUTBUF_MAX` mirror of `Y11_INBUF_MAX`, exceeded →
   `wants_close`): unbounded buffering is I/O policy, so it stays in
   C even though the audit class is wire-triggered.
3. Handshake-parking (§1.1): add a per-connection handshake deadline
   in the C event loop (policy, not parsing).
4. Delete the now-dead C check helpers; the corpus becomes the
   regression contract for both languages.

### Verification gates (every phase)

- `make` warning-free with `-std=c99 -pedantic -Wall -Wextra -Werror`.
- wirefuzz corpus: byte-identical replies/errors before and after.
- Session smoke: dwm + sakura + rofi on a headless display exercises
  every migrated path (focus, keys, grabs, StateNotify, RENDER text).

### Explicitly out of scope

- Big-endian client support (phase-1 server restriction stands).
- Parsing of `y11_dispatch_accept_resource` request bodies (unimplemented
  drawing requests, not parser gaps).
- The dormant GLX bridge beyond the guards already added.
