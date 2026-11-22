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
