# Stereoscopic battle sprites — #68 forward-port (investigation)

This draft is a **staged compatibility port** of [original PR #68](https://github.com/ZallaxDev/emerald-3ds/pull/68) by **darkmattericecoffee**, built against upstream **`integration`**. It is **not yet an enabled gameplay feature**.

## Credit and source of truth

The full renderer, design, four-plane depth-file format, front/back sprite generators, stereo sprite compositor and original test fixtures originate in the original author's PR and remain attributed to them. The published [mons.bin and backs.bin packs](https://github.com/darkmattericecoffee/pokeemerald-3Ds-dualscreen/releases/tag/stereo-packs-2026-10-15) are maintained by the author. No sprite art or binary depth packs are checked into this branch. **Do not merge this partial stage as a feature.**

## Initial port boundary

This initial commit imports the existing pure depth-pack / file-decoder code and source-derived Python tools, preserving its design. It connects deterministic **synthetic tests** to normal CI in Python 3.11 / the release Python pin and in host C tests. It updates Pillow's deprecated `getdata()` call to `get_flattened_data()` (supported by the pinned Pillow 13.0.0) and resolves test import paths.

The actual runtime stereoscopic compositor, game-side battler detection, option controls and build-time asset wiring **have not been ported yet**, so shipping behavior is unchanged. Bringing over the original rendering code without adapting it to the newest renderer risks regressions; the next stage should adapt it incrementally.

## Required follow-up work

1. Rebase/forward-port the original renderer against current `integration` instead of overwriting newer `3ds_video.c`, `3ds_extras.c`, or gameplay code. Preserve #67 / #69 / #89 and all subsequent fixes.
2. Fix the original PR review issues: move the clip test inside the stereoscopic branch, prevent double counting and preserve wrapped rain/snow; run the full anatomy/asset tests with current Pillow; ensure the original format's absence-of-outline semantics.
3. Port settings with currently supported languages; hide options when packs are empty. Choose a currently unused, safe patch number for the optional wild-only enhancement instead of reusing 0057.
4. Verify deterministic asset generation, correctly package optional depth packs, and leave them absent without errors.
5. Test New 3DS and Old 3DS hardware for 2D/split/voxel battles (front/back/intro/Transform/Substitute/faint/doubles), compare the 3D-disabled path against baseline and inspect frame pacing, memory, VRAM, and weather. Hardware testing is outstanding.

**Review gate:** keep this PR draft until the renderer is integrated, the full build and visual checks pass, and the original author and maintainer have a fair opportunity to review the credited recovery.

## Phase 2 staging: compiled battle modules, without runtime activation

The original author's native plane-file loader, asynchronous battle-plane compositor,
and game-side battler identification source have been imported unchanged except that
the battle opt-in default is **OFF** (no automatic changes to current battles).

`STEREO_BATTLE=1` is a *temporary developer-only compile switch* during the port.
The draft PR's 3DS CI build uses it so all three new sources must compile/link;
an ordinary build omits them entirely (`STEREO_BATTLE=0`, default).
There are deliberately **no** battle-frame, sprite-filter, or drawing hooks
in the current engine yet: do not claim this phase produces visible stereo.

The next stage is a safe adapter for the split `video/*.inc` compositor, first
with disabled-path equivalence and weather regression tests. Only then should
runtime opt-in and optional pack setup be wired. The original `0057` wild-only
patch and UI changes are explicitly out of scope for this stage.

Original source: https://github.com/ZallaxDev/emerald-3ds/pull/68

## Phase 2B — experimental, guarded native video adapter

Current `video/*.inc` hooks run only in developer builds made with
`STEREO_BATTLE=1`; their runtime `battle_mon_3d` preference defaults
**OFF**. The original game-side battler snapshot is taken only when enabled.
Within actual stereo eyes, the native plane compositor can replace only an
OAM sprite that the original decoder validates as a matching 64×64 battler;
the normal drawing, particle wrapping, OBJ windows and 2D flat path remain.
A depth battler uses the existing **whole-per-eye** battle compositor to
avoid doubling its flat slot sprite. This extra work may cost performance,
particularly on Old 3DS; benchmarking and visual/hardware validation are
outstanding. No unrelated renderer code was copied from the old monolith.

Depth-file packaging, UI exposure, completeness, and New/Old 3DS hardware
validation are still outstanding. This is **not** a public-ready feature.


## Optional author's binary packs for developer testing

Only a `STEREO_BATTLE=1` build adds
`romfs/stereo/mons.bin` and `romfs/stereo/backs.bin`.
By default each is a valid **header-only empty file**, so no stereoscopic
picture is activated. Supplying `STEREO_MONS_PACK=/path/to/mons.bin` and
`STEREO_BACKS_PACK=/path/to/backs.bin` at build time copies the author's
ready-made depth-only files through the original idempotent fill-only
normalization. The builder does **not** download copyrighted assets or
bundle any Pokémon artwork.

Source of the finished packs: https://github.com/darkmattericecoffee/pokeemerald-3Ds-dualscreen/releases/tag/stereo-packs-2026-10-15

The hidden developer runtime setting `battle_mon_3d=1` is **also**
required to attempt stereoscopic sprite rendering; the default remains 0.
Never enable it by default before visual/Old/New 3DS testing.

## Phase 3A — translated Battle/STEREO settings

The existing OPTIONS architecture is unchanged. When a developer build
(`STEREO_BATTLE=1`) contains at least one valid nonempty plane pack,
the Battle category gains a **STEREO** group with three localized controls:
3D Pokémon (OFF by default), plane spread (1/2/3/4/6/8 pixels), and
plane position (behind/around/in front). Labels and pivot values are
translated in all seven currently supported locales.

The entire group is **hidden** if either the feature is not compiled or
both packs are absent/empty/invalid. Spread and position are greyed out
until 3D Pokémon is enabled. Even a persisted manually edited
`battle_mon_3d=1` is ineffective without a valid pack. Toggling ON
also attempts the original early texture reservation; failure still
falls back to original flat sprites. No other options, cheating modes,
or gameplay defaults are changed.

## Phase 3B — fail-closed pack validation

`collapse_stereo_planes.py --fill-only` now validates the complete
`EM3DSTM1` index and payload before normalizing any third-party pack:
version/magic, dimensions, plane/frame limits, max 520 entries, strictly
ordered unique identifiers, contiguous bounded frames, no trailing data,
and valid pixel plane numbers. Its output is deterministic and idempotent.
The native reader also validates each index and payload range before
declaring an asset available to the settings UI. A bad or truncated pack
therefore cannot advertise an active feature; the original sprite
renderer is the fallback. New synthetic tests cover malformed indexes,
short/trailing payloads, invalid planes and valid no-outline copies.

## Phase 3C — deterministic staging and stale-pack protection

The feature-on build now tracks `STEREO_MONS_PACK` and
`STEREO_BACKS_PACK` selections using a separate lightweight make
configuration stamp, so switching from one source pack to another,
or from a pack to a header-only empty container, regenerates RomFS
assets without forcing recompilation of the engine.

Switching back to `STEREO_BATTLE=0` prunes any previously generated
`romfs/stereo/*.bin` from the normal 3DSX and release stage. The
files classify as optional-feature **engine RomFS** for a developer
build, not game-derived content: they cannot affect the game-data
ABI or the `emerald3ds.pak` output. A synthetic staging test
guards these boundaries.

Example opt-in developer build (using the original author's own
separately obtained depth-only release files):

```sh
make -C 3ds_port release STEREO_BATTLE=1 \
  STEREO_MONS_PACK=/path/to/mons.bin \
  STEREO_BACKS_PACK=/path/to/backs.bin
```

Without the source paths a feature-on build generates two valid empty
containers, the settings stay hidden, and all Pokémon remain flat.
This draft has no automatic network downloads or redistributed artwork.

## Phase 4A — guardrails for the visual and hardware test matrix

**Source-level corrections:** The experimental whole-per-eye route now
honors the current battle clear color, rather than always painting its flat
eye black. It also explicitly releases the existing compositor's OBJ-window
slot and edge-store flags on its early return, preventing stale state from
leaking into subsequent battle frames. The existing branch for games without
the stereoscopic opt-in is unchanged.

**Host regression gate:** `stereo_draw_gate_test.c` exhaustively checks all
128 synthetic OAM entries under left/right/flat eyes, empty/missing masks,
the normal disabled state, all three weather categories, and all 128-bit
mask boundaries. Only a validated battler in an actual stereoscopic eye
can reach the new plane-rendering branch. Every other sprite continues to
use the existing engine. This tests the eligibility logic, **not** actual
pixel output or framerate.

### Manual Azahar / hardware verification (not yet performed)

Use an original compatible ROM via the user-owned builder and the original
author's separately obtained `mons.bin` and `backs.bin` files. Record the
base `integration` build and this experimental branch using identical
saves, scenes, and identical settings (first with 3D Pokémon OFF, then ON).

| Scenario | OFF-path parity | ON appearance | Old/New 3DS timing |
| --- | --- | --- | --- |
| Classic single battle, front/back Pokémon | Pending | Pending | Pending |
| Double battle, four Pokémon and priority | Pending | Pending | Pending |
| Trainer entry/throw, player back/intro | Pending | Pending | Pending |
| Fainting, recall, send-out, Substitute | Pending | Pending | Pending |
| Transform, palette tint, shiny and affine moves | Pending | Pending | Pending |
| Rain/snow/sand/bubbles and OAM wrapping | Pending | Pending | Pending |
| Voxel battle scenery and stereo slider sweep | Pending | Pending | Pending |
| Battle text/HUD, fades and OBJ windows | Pending | Pending | Pending |
| Battles with no/partial/corrupt depth packs | Pending | Pending | Pending |
| Outside battle: field, follower, menus, Pokédex | Pending | Pending | Pending |

For each case, capture **left and right eye images**, OAM sprite alignment,
any missing pixels, baseline-vs-OFF screenshots, GPU/CPU frame and VBlank
statistics, VRAM/linear memory before/after, and any error logs. Profile
at full slider on both New 3DS and Old 3DS. Do not call this review-ready
until the baseline OFF comparisons and ON-path visuals have been inspected.

## Phase 5A — battle sprite cache invalidation (implemented, verification pending)

The native experimental compositor stores the depth-pack ID as well as its
mutable plane-storage pointer when caching a composed texture. Switching
Pokémon/trainer IDs with the same OAM slot, animation frame and palette
must not reuse the previous battler's card stack. Updates to the copied
sprite buffer or player-back sheet, and clearing a battle, also invalidate
the cache even if the depth-pack identity has not changed.

An SDK-free host test verifies pointer/ID combinations, including player
back-sheet IDs. CI still builds the full feature-on and ordinary feature-off
engines. This is a source-level correctness fix, **not** proof of native GPU
pixel parity or physical hardware timing.

**Still required in Phase 5:** Azahar left/right eye screenshots with the
author's original packs; baseline vs feature-OFF visual comparisons; battle
animations (fainting, Substitute, Transform), weather and HUD review;
and Old/New 3DS VRAM/linear-memory and frame-pacing measurements.
No emulator or hardware results are claimed.

## Phase 6 — diagnostic profiling and conservative renderer fallbacks

### 6A: opt-in CPU profile

`STEREO_PROFILE=1` requires `STEREO_BATTLE=1` and enables CPU diagnostic
sampling without adding counters or timer calls to a normal release build.
Build the feature with the author's separately acquired packs, for example:

```sh
make -C 3ds_port release STEREO_BATTLE=1 STEREO_PROFILE=1 \
  STEREO_MONS_PACK=/path/to/mons.bin \
  STEREO_BACKS_PACK=/path/to/backs.bin
```

The console log reports each 600-frame active sample (and partial samples at
battle cleanup): mean CPU time in the battle's OAM matching/texture preparation
path (inclusive of composition), number of matched sprites, cache hits,
texture rebuilds and their mean CPU cost, draw calls/cards and mean CPU draw
submission cost, unmatched frames, ambiguous OAM rejections, texture
allocation fallbacks, and available linear/VRAM memory at each report.
A separate per-eye summary reports CPU-side left/right whole-eye draw costs
when stereoscopic Pokémon are present. All times are CPU wall-clock samples,
not GPU draw duration. The existing `perf.bin` and 3DS profiling tools
remain the sources for GPU, VBlank, framerate and command-queue behavior.
These counters do not silently change the profiler binary record format.

### 6C: ambiguous OAM matching fails flat

When multiple battler positions both match the **same OAM entry**, neither
position can safely claim that sprite. Those matches are removed before
texture composition and the original flat OAM renderer owns it; unrelated
battlers remain eligible for stereoscopic rendering. An SDK-free host test
covers distinct entries, two-slot collisions, three-slot collisions, all
invalid/unassigned entries, and boundary indices. An additional native payload check rejects malformed plane nibbles in
manually changed RomFS packs *before* the compositor suppresses a sprite,
preserving the legacy transparent sentinel `15`. SDK-free tests verify
valid planes, legacy transparency, invalid nibbles and plane-count bounds.
Missing packs, invalid entries, disabled settings and failed texture
allocations retain the ordinary flat-sprite fallback.

CI compiles the optional diagnostic mode within its non-shipping feature-on
build and separately compiles and audits the ordinary `STEREO_BATTLE=0` build.
This is **not** a performance win claim; review measured evidence before
changing the whole-per-eye compositor or reducing fidelity.

### 6B/6D: acceptance checks still pending

- [ ] Compare baseline vs feature-OFF real-ROM left/right captures and battle
      HUD, weather, OBJ windows, animations, palettes, fades and double battles.
- [ ] Test feature-ON using the original author's actual depth-only packs.
- [ ] Gather comparable 600-frame logs from Azahar and New/Old 3DS, with both
      stereo ON and OFF, alongside perf.bin/vblank measurements.
- [ ] Inspect GPU/CPU frame pacing, VRAM/linear-memory pressure, stalls and
      allocation failure behavior; optimize only measured bottlenecks.
- [ ] Original author and maintainer review before marking ready.

**No emulator capture or physical Old/New 3DS hardware validation is claimed.**
Keep this branch Draft and the new option OFF by default.


### 6B — share quantized depth groups across a sprite's cards

The feature-ON sprite compositor now decodes and groups each pixel's plane
**once per picture/frame**, then reuses that 4096-byte lookup for all 2–8
cards and their skirt lookups. The previous independent compositor is retained
as the reference implementation for a deterministic test that compares
**all bytes** of the PICA texture atlas, including untouched regions.
Synthetic fixtures exercise 2/4/7/15 planes, both frames, requested 2/3/4/8
cards, transparent pixels, palette values and skirt radii 0–2. The old
nearest-colour tie order and texel layout are unchanged.

The new map is a **4 KiB static buffer** in the opt-in battle module and
has no effect in the default-OFF binary. This removes repeated depth nibble
decodes and plane-to-group calculations from inner card/skirt loops. CI
establishes pixel equivalence for the synthetic fixtures; real-world speed
and GPU timing remain unmeasured, so no FPS improvement is claimed.


### 6D — reproducible visual-parity evidence (awaiting actual captures)

There is now a strict, image-based comparison tool:
`3ds_port/scripts/stereo_capture_compare.py`. It consumes synchronized
**native-resolution 400x240 top-screen PNGs** for both left and right eyes.
The CLI requires `scene.left.png` and `scene.right.png` pairs for
every scenario, exactly matching capture names and count between builds.
It compares **every RGBA pixel without tolerance or ignored rectangles**,
writes normalized-image SHA-256 hashes and mismatch bounding boxes to JSON,
and can generate red pixel-difference masks for any failures. Missing eyes,
different scene coverage, an empty folder, bad names and wrong resolutions
cannot pass. A fully synthetic suite runs in both Python CI audits.

Prepare three builds (same legal ROM/game data, save, emulator version and
settings): upstream `integration`, this PR with
`STEREO_BATTLE=0`, and the feature-enabled experimental binary
(with the original author's separately distributed depth-only packs).
Capture synchronized left/right top-eye PNG pairs for the scenarios below.
Optionally repeat with `STEREO_BATTLE=1` and 3D BATTLE MONS OFF.
Run the exact parity gate on *upstream vs feature OFF*:

```sh
python 3ds_port/scripts/stereo_capture_compare.py \
  --baseline captures/integration --candidate captures/stereo-off \
  --report captures/parity.json --diff-dir captures/differences \
  --require-scenes single,double,trainer,faint,transform,weather,hud,menus
```

Scene names above are examples, but the actual evidence should cover at least
the manual Phase 4 matrix, including both voxel and 2D battles, trainer
introductions, player backs, moves, weather, windows, fades and overworld.
The feature-enabled build must be **visually inspected**, including both eyes,
the slider at zero and full separation, and real frame-to-frame animation;
a successful OFF comparison cannot certify its new 3D appearance. Track
failed captures and approved intentional differences explicitly, never by
silently relaxing byte comparison thresholds.

Capture and retain independent `perf.bin` logs and experimental
`stereo perf` diagnostics for fixed battle scenes in Azahar,
Old 3DS and New 3DS. Use the existing `perf_report.py`
`compare` and `battle` subcommands to calculate latency,
render-route and dropped-VBlank differences; pin device, game data, save,
slider, settings, 600-frame windows and warmup. No real device or emulator
measurements/captures were available during this change, so neither FPS
improvements nor true visual parity are asserted here.


### 6B host CPU microbenchmark (reproducible, not hardware data)

`3ds_port/tests/stereo_compose_bench.c` now runs a fixed synthetic
four-card sprite with 2-pixel skirts 24 times through each algorithm.
It rejects any byte differences in the completed 128x512 texture atlas,
reports host CPU milliseconds per sprite and a relative throughput factor,
and does **not** enforce a fragile speed-ratio threshold. CI exercises this
diagnostic on its Linux runner; actual Old/New 3DS performance and GPU
latency remain unmeasured, requiring the opt-in Phase 6 profiler and
physical-device runs.

## Phase 7C — static regression audit and asynchronous publication

**Fix: collision-free sprite silhouette identity.** Previously the trainer
ID's kind bit `0x4000` overlapped with the `trainer.ok` bit of the 32-bit
cache key, so an asynchronous trainer-depth load could become ready without
invalidating the silhouette cache. The key now uses a 64-bit value with both
16-bit IDs and separate, non-overlapping ready flags. A host regression
test verifies transitions of either ready flag and changes to trainer and
Pokémon IDs.

**Fix: cross-thread readiness handoff.** The background file reader now uses
release stores to publish its `ok`, `failed`, and `pending` flags after
storing frames. The game/rendering thread uses acquire loads for these flags
rather than racing ordinary C reads against the writer. The data buffer is
only consumed after the acquire of `ok`, or after the pending flag is
observed false. Host tests check the primitive state transitions and the
cross-thread code paths are exercised by feature-on 3DS compilation.

These address source-level correctness issues, not a claim that real 3DS
timing, animation or frame pacing has been observed. Hardware acceptance
remains required.

**Additional bounds guard:** An eligible 64×64 4bpp battler needs 64
consecutive 32-byte OBJ tiles. Before accepting a new slot, the optional
renderer now rejects a base tile above 960 (the last base that fits in
the GBA's 32 KiB OBJ region). This prevents its 2048-byte VRAM comparison
from crossing the 32 KiB boundary and leaves any wrapping or unsupported
sprite layout to the unchanged flat renderer. Host tests cover bases 0,
959, 960, 961, 1023 and an invalid extreme. Invalid `STEREO_BATTLE`
build values now fail at Makefile parsing rather than silently skipping
feature isolation/pruning rules.
