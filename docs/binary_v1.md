# Raw binary v1: common envelope

This documents the **implemented common envelope**, not the scan/dark/energy
payload contracts. Payload field tables and acquisition lifecycle rules are
specified and implemented in later phases. Integers are explicitly
little-endian; floating-point payload values are IEEE-754 binary64, finite
only. A C++ struct is never written directly to disk.

## File header (80 bytes)

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | ASCII `ZSCNBIN\0` |
| 8 | 2 | Major version, `1` |
| 10 | 2 | Minor version, `0` |
| 12 | 2 | Kind: scan `1`, dark `2`, energy burst `3`, derived arrays `4` |
| 14 | 2 | Header flags, zero |
| 16 | 16 | Record UUID v4 as canonical/network-order UUID bytes |
| 32 | 16 | Session UUID v4 as canonical/network-order UUID bytes |
| 48 | 8 | Signed Unix nanoseconds UTC (`int64`) |
| 56 | 20 | Reserved, zero |
| 76 | 4 | CRC32C over header bytes 0–75 |

This is the file's **only** timestamp. Binary nanoseconds restrict supported
dates to the representable signed `int64` range; encoding uses the slightly
narrower safe range of ±9,223,372,035 seconds from the Unix epoch. Decimal
timestamps exposed by the schema types may represent a wider year range, but
cannot all be encoded in this raw binary version. The bytes at offset 48 are
an absolute UTC instant, not a pulse timestamp.

## Frame envelope

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 2 | Frame type (`uint16`) |
| 2 | 2 | Flags (`uint16`): bit 0 is `OPTIONAL`; other bits zero |
| 4 | 4 | Payload size (`uint32`) |
| 8 | 8 | Sequence (`uint64`), starting at zero, no gaps |
| 16 | N | Payload (no padding) |
| 16+N | 4 | CRC32C over frame bytes 0 through 15+N |

No known core frame may carry `OPTIONAL`. Unknown required types are rejected;
unknown optional extension types `0x8000`–`0xfffe` have their size, sequence,
and CRC validated before being skipped. `0xffff` is reserved. Unknown flags
are refused. Core types currently reserved for v1: scan `0x1001`–`0x1006`,
`0x10ff`; dark `0x2001`–`0x2003`, `0x20ff`; energy `0x3001`–`0x3002`, `0x30ff`.

The default reader accepts up to 16 MiB per frame and one million frames;
both are configurable per reader. It checks lengths before allocating or
accessing payload bytes. CRC32C uses the reflected Castagnoli polynomial
`0x82f63b78`, initial value `0xffffffff`, final XOR `0xffffffff`; the checksum
of ASCII `123456789` is `0xe3069283`. Wire checksums are little-endian.
Whole-file SHA-256 (stored in TOML when sealed) is separate from frame CRCs.

The common reader rejects a truncated/CRC-invalid frame. A future recovery
layer may use its last successful byte offset as the valid prefix of an
**interrupted**, audit-only file; no incomplete file is silently repaired.

Golden byte sequences for a complete header and energy-configuration frame
are tested in `tests/schema/binary_tests.cpp`. They were calculated with a
separate Python `struct` encoder and a bit-at-a-time CRC32C reference, not
with this library's encoder.

## Scan frames (kind `1`)

All offsets below are **within each frame payload**. No scan payload has a
timestamp. Scan frame order is `SCAN_CONFIG`, then sequential `POSITION_BEGIN`,
one or two `MOVE_CHECK` frames, zero or more accepted/rejected shot frames,
`POSITION_END` for each started position, then exactly one `SCAN_END`. Optional
extension frames can appear before `SCAN_END`, but not after it. A scan missing
`SCAN_END` is not a sealed scan; interrupted-prefix recovery belongs to Phase 5.

| Type | Name | Payload size | Fields (offset: type, unit) |
| ---: | --- | ---: | --- |
| `0x1001` | `SCAN_CONFIG` | `104 + 8*N` | 0: kind `u8` (1 blank/2 sample); 1: max moves `u8` (=2); 2: peak rule `u16` (=1, maximum post-trigger); 4: rule version `u16`; 6: reserved `u16` (=0); 8: position count `u32` (N); 12: accepted target `u32`; 16: rejected limit `u32`; 20: pretrigger samples `u32`; 24: post-trigger samples `u32`; 28: reserved `u32` (=0); 32: sample rate `f64` Hz; 40: start z `f64` m; 48: end z `f64` m; 56: spacing `f64` m; 64: readback acceptance limit `f64` m; 72/80: AI0 min/max `f64` V; 88/96: AI1 min/max `f64` V; 104: N commanded positions, `f64` m each |
| `0x1002` | `POSITION_BEGIN` | 16 | 0: position `u32`; 4: reserved `u32` (=0); 8: commanded z `f64` m |
| `0x1003` | `MOVE_CHECK` | 24 | 0: position `u32`; 4: move index `u16` (0 or 1); 6: result `u8` (0 in tolerance/1 outside); 7: reserved `u8` (=0); 8: commanded z `f64` m; 16: readback z `f64` m |
| `0x1004` | `ACCEPTED_SHOT` | 24 | 0: position `u32`; 4: trigger-attempt index `u32`; 8: AI0 peak `f64` V; 16: AI1 peak `f64` V |
| `0x1005` | `REJECTED_ATTEMPT` | 16 | 0: position `u32`; 4: trigger-attempt index `u32`; 8: reason `u16`; 10: reserved `u16` (=0); 12: signed vendor code `i32` (0 if unavailable). No peak values. |
| `0x1006` | `POSITION_END` | 16 | 0: position `u32`; 4: accepted count `u32`; 8: rejected count `u32`; 12: move-check count `u16`; 14: result `u16` |
| `0x10ff` | `SCAN_END` | 32 | 0: result `u16` (0 complete/1 incomplete); 2: reason `u16`; 4: started positions `u32`; 8: complete positions `u32`; 12: reserved `u32` (=0); 16: total accepted `u64`; 24: total rejected `u64` |

Rejection codes: `1` trigger timeout, `2` DAQ error, `3` missing AI0,
`4` missing AI1, `5` nonfinite AI0, `6` nonfinite AI1, `7` AI0 out of range,
`8` AI1 out of range, `9` multiple channel failures, `255` other.
Position results: `0` complete, `1` stage error, `2` shot retry limit,
`3` operator cancelled, `4` acquisition error. Scan-end reasons: `0` normal,
`1` operator cancelled, `2` stage error, `3` shot retry limit, `4` DAQ error,
`5` controller error, `6` application error, `255` other.

The explicit z array must be strictly monotonic and consistent with the linear
start/end/count/spacing definition. Its first and last values must bit-match
start and end; each position's command bit-matches the array entry. Actual
readbacks are *never* substituted into the grid used for fitting. Accepted
shots require a successful in-tolerance move, finite channel peaks within
their inclusive voltage ranges, and contiguous attempt indexes shared with
rejections. A complete position has the exact target accepted count; a failed
stage retry has two out-of-tolerance move checks and **no shots**. Footers
reconstruct counts from the preceding frames. Incomplete scans remain sealed
audit records but cannot enter production workup.

## Dark frames (kind `2`)

| Type | Name | Payload size | Fields (payload offsets, little-endian) |
| ---: | --- | ---: | --- |
| `0x2001` | `DARK_CONFIG` | 64 | 0: peak rule `u16` (=1); 2: rule version `u16`; 4: accepted target `u32`; 8: rejected limit `u32`; 12: pretrigger samples `u32`; 16: post-trigger samples `u32`; 20: reserved `u32` (=0); 24: sample rate `f64` Hz; 32/40: AI0 min/max `f64` V; 48/56: AI1 min/max `f64` V |
| `0x2002` | `DARK_ACCEPTED_SHOT` | 24 | 0: attempt index `u32`; 4: reserved `u32` (=0); 8/16: paired AI0/AI1 `f64` peaks, V |
| `0x2003` | `DARK_REJECTED_ATTEMPT` | 16 | 0: attempt index `u32`; 4: reason `u16` (same codes as scans); 6: reserved `u16` (=0); 8: signed vendor code `i32`; 12: reserved `u32` (=0). No peaks. |
| `0x20ff` | `DARK_END` | 16 | 0: result `u16` (0 complete/1 incomplete); 2: reason `u16` (0 normal, 1 cancelled, 2 shot limit, 3 DAQ error, 4 application error, 255 other); 4: accepted `u32`; 8: rejected `u32`; 12: reserved `u32` (=0) |

Frames occur in order: config, contiguous-index attempts, end. A completed
dark has exactly its accepted target (at least two). Incomplete darks remain
audit-only. The dark header's UTC time is the one timestamp; **no optical, OD,
cuvette, stage, or scan-grid data occurs in a dark file.** Dark conditions and
DAQ configuration identity belong in the session manifest.

## Manual energy-burst frames (kind `3`)

| Type | Name | Payload size | Fields |
| ---: | --- | ---: | --- |
| `0x3001` | `ENERGY_CONFIG` | 8 | 0: reading count N `u32`; 4: reserved `u32` (=0) |
| `0x3002` | `ENERGY_READINGS` | `8 + 8*N` | 0: matching count `u32`; 4: reserved `u32` (=0); 8: ordered N `f64` joule readings |
| `0x30ff` | `ENERGY_END` | 8 | 0: matching count `u32`; 4: reserved `u32` (=0) |

Exactly these three frames are required; N ≥ 2, and every unscaled meter
reading is finite and positive. The binary header's single timestamp is the
GUI **entry** time, not independently established physical measurement time.
Revision relationships and meter calibration IDs belong in TOML, not the
binary observation array. Independent dark/energy frame byte checks live in
`tests/schema/dark_energy_tests.cpp`.

## Derived numerical arrays (kind `4`)

Derived files use `ARRAY_BLOCK` (`0x4001`) followed by `ARRAY_END` (`0x40ff`).
Each block payload starts with role `u16`, reserved `u16`, curve index `u32`
(`0xffffffff` means global), rows `u64`, columns `u64`, then finite row-major
`f64` values. The footer stores block count `u32` and reserved zero `u32`.
Roles distinguish commanded positions, absolute transmission, point SEs, dark
sensitivities, sample/blank retained attempt indexes, meter mean/SEM, κ and its
uncertainty, joint covariance, predictions, conditional/total parameter
covariance, and the eight calibration variance groups. `(role, curve index)`
is unique. Immutable publication records byte size and SHA-256 and refuses
overwrite; replay checks shape, CRCs, digest, and exact values.
