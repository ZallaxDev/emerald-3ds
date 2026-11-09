# Save Serialization Design — Living Rival Core

**Phase:** 2.5 (Week 1)
**Scope:** Nintendo 3DS (the shipping target), with possible PC/mobile ports. **GBA `SaveBlock2`/`SaveBlock3` mapping is explicitly out of scope** — this is a 3DS title and the portable core is not bound to the GBA save layout.

## 1. Measured sizes

| Structure | Size |
|---|---|
| `TrainerJourneyCore` | **87 B** |
| `TrainerJourneyExtended` | **221 B** |
| One record (core + extended) | **308 B** |
| 80 marked trainers | **24,640 B (24.1 KiB)** |
| Blob (16 B header + 80 records) | **24,656 B (24.1 KiB)** |
| Dual-slot total | **49,312 B (48.2 KiB)** |

`LIVING_RIVAL_PROTOTYPE.md` estimated 72 B / 56 B, but that does not match its own field list. The real layout is the field list, and the sizes above are enforced at compile time by `LR_STATIC_ASSERT` in `include/lr_types.h`. `milestoneFlags` was widened from `u16` to `u32` because the milestone list needs about 20 bits.

## 2. Blob layout

All integers are little-endian. The blob is a 16-byte header followed by `count` fixed-size records.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x314A524C` (`"LRJ1"` bytes) |
| 4 | 2 | version (`LR_SAVE_VERSION`, currently 1) |
| 6 | 2 | record count |
| 8 | 4 | payload bytes (`count * 308`) |
| 12 | 4 | CRC-32 of the payload |
| 16 | `count * 308` | records: `core` (87 B) then `extended` (221 B) |

The CRC is the standard reflected CRC-32 (polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`); the known vector `"123456789"` → `0xCBF43926` is pinned in `test/test_save.c`.

Records can be `memcpy`-ed because both structs are `LR_PACKED` and the save contract is little-endian. A big-endian port must add a byte-swapping layer before using this codec.

## 3. Versioning

- Version 1 is the baseline.
- **Unknown or older version → discard the blob and re-initialize every journey at `LR_PHASE_ROOKIE`.** Never crash, never partially apply a blob you do not understand.
- New fields append to the structs and bump the version.
- Per-version migration functions live in `src/lr_save.c`; the decode path dispatches on the header version before copying records.
- Never renumber enumerators in `lr_enums.h`: their numeric values are part of the on-disk format.

## 4. Dual-slot write protocol

Two independent blobs are kept (slot A and slot B) plus a small active-slot marker.

1. Write the **inactive** slot.
2. Compute and store the CRC over its payload.
3. Flush the media.
4. Toggle the active-slot marker.
5. Flush again.

On load, validate magic, version, count, payload length and CRC on the active slot. If it fails, try the inactive slot. If both fail, re-initialize and overwrite the worse slot on the next save. A power loss at any step leaves at least one intact slot.

## 5. Load and bounded catch-up

1. Decode the active slot into the in-memory arrays.
2. Compute elapsed game minutes since `lastSeenMinute`.
3. Call the simulation with the bounded catch-up rules before the first frame: at most 48 hourly ticks and 7 daily ticks per load (`LrSimulateTo`, Week 3).
4. Persist the new `lastSeenMinute` with the next save.

The cap prevents a months-long absence from stalling the first frame, while still advancing the world enough to look alive.

## 6. Determinism

- Persist `personalSeed`, `teamVersion` and the full derived state. A load must never re-derive a journey differently.
- Same seed plus same saved state produces bitwise-identical arrays; this is asserted by `test_mind` (same seed → `memcmp == 0`).
- Never store pointers. Never store host-specific padding: `LR_PACKED` plus the size assertions make the record layout portable.

## 7. Corruption recovery

- Validate magic, version, count, payload length and CRC before touching any destination buffer. `LrSaveDecode` returns `false` and leaves outputs untouched on any failure (pinned by `test_save`).
- If both slots are corrupt, re-initialize journeys at `LR_PHASE_ROOKIE` rather than failing the boot.
- CRC covers the payload only; the header is validated field by field.

## 8. Portability notes

- C99, fixed-width integers, no platform headers, no dynamic allocation.
- Little-endian only; PC (x86-64/ARM64), 3DS (ARM11) and mobile (ARM64) are all little-endian.
- `LR_PACKED` is GCC/Clang-specific; a new compiler must define it (the header raises a compile-time error otherwise).
- The sidecar blob can be embedded in a larger save container later without changing the record format; only the header/offset layer moves.
