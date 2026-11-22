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
