# Voxel generation experiment: Python 3.15.2

Fork experiment, November 19, 2026. Source base: `6699c19a2a36bcf90be62859f43f70f405a2e777`.
Python 3.15.2 and Pillow 13.0.0 were selected explicitly for these measurements.
Release version pins follow `upstream.lock`; this experiment does not edit them.

## Complete building generation

| Mode | Wall time |
| --- | ---: |
| Original generator | 452.338 seconds (7:32) |
| Indexed floors and preselected shell pieces | 345.336 seconds (5:45) |
| Saved | 107.002 seconds (23.7% less time) |

This is one sequential local A/B comparison on Linux x86-64, using the same
prepared source tree and graphics inputs. Both ran without a profiler and with
the JIT disabled. Source data and file caches were warm. These times cover the
complete `gen_voxel_buildings.py --output` invocation, without `--verify`.
They measure generator work, rather than an entire CI job or 3DS compilation.
No console frame-rate measurement was made. Only English source assets were
used for the complete-file comparison.

Two repeated calculations changed:

- Index the allowed RGBA floor colours at each of the 256 repeating tile
  positions once per room. Each room pixel then uses a membership lookup.
  Alpha remains part of the comparison; the index is local to the room.
- Select the room's shell/wall grids once. The pixel loop no longer scans
  every furniture piece, checks its metadata, and splits its name again.

Model ordering, floating-point geometry calculations, output formats, release
pins and CI configuration retain their existing behavior. The implementation
uses Python features already available to the project's Python 3.11 builders.

## Output identity

Both complete outputs compared byte for byte, including the window-light sidecar.
No generated game data or binaries are included in this branch.

| File | Bytes | Matching SHA-256 |
| --- | ---: | --- |
| `buildings.bin` | 42,485,196 | `9cec5168e808c0299a0bbff5301800aafc01677267999578cc8bf4dca9f4b2ad` |
| `windowlights.bin` | 2,984 | `4640b06c85d305c6fa63ff3d2afbf529812e1442eabf85f58084be789f89f8af` |

## Floor-mask checks and JIT experiment

The real-room benchmark covers **193 interiors and 11,899,392 pixels**.
All masks matched in every one of three alternating paired passes. Image
preparation is outside the measured floor-classification work.

| Floor classification | Original median | Candidate median |
| --- | ---: | ---: |
| All authored rooms, JIT disabled | 6.506106 s | 1.965339 s |
| Synthetic 640x480 room, four floors, JIT disabled | 0.342223 s | 0.053182 s |
| Same synthetic workload, JIT enabled | 0.351151 s | 0.053088 s |

The synthetic comparisons use six alternating pairs per interpreter mode.
They do not demonstrate a meaningful JIT improvement for floor classification.
Full geometry generation with the JIT enabled was not benchmarked. JIT
activation remains an experiment, rather than a release setting.

## Profiling scope and next targets

Python 3.15's [sampling profiler](https://docs.python.org/3.15/library/profiling.sampling.html)
was unavailable here because process-memory access was blocked. The harness
supports it on hosts that permit it. Bounded 45-second tracing passes saved
profiles for buildings and relief. These are **prefix profiles**, not a full
CPU-time breakdown; cumulative timings overlap.

The building prefix spends substantial time in `voxel_bridge.block_specs`,
`Bridge._read`, quad construction and merging. It also captures the repeated
room-shell filtering changed here. The terrain prefix points to `world_levels`,
`drawn_prepare` and `drawn_canvas`, followed by height solving. Bridge geometry
and terrain preparation are candidates for a separate measured experiment.

## Validation

- Builder suite on Python 3.15.2/Pillow 13.0.0: **264 passed, 2 skipped**.
- Targeted floor tests on Python 3.11/Pillow 12.3.0: **2 passed**.
- Tests cover repeated and partial tiles, empty/duplicate floor lists, alpha,
  coordinate-dependent colours and unchanged input images.
- Release dependency discovery includes the new helper automatically.
- Publication audit: clean. Profiling harness smoke checks cover native and
  tracing runs, sidecar hashes, partial outputs and skipping a dependent step.

## Repeat the experiment

Use a prepared `build/upstream` tree with its graphics inputs generated through
normal source-build rules. The measurements used the pinned pokeemerald commit
`76463dac15cad36aca5e2b3f6366abf9e53f814f` and its upstream tileset-specific conversion rules.
Run the baseline before restoring the candidate scripts into that prepared tree:

```sh
git show 6699c19a2a36bcf90be62859f43f70f405a2e777:3ds_port/scripts/gen_voxel_buildings.py > build/upstream/3ds_port/scripts/gen_voxel_buildings.py
uv run --python 3.15.2 --with Pillow==13.0.0 python tools/profile_voxel.py --tree build/upstream --out build/profile-baseline --steps buildings --profiler none
cp 3ds_port/scripts/gen_voxel_buildings.py 3ds_port/scripts/voxel_floor.py build/upstream/3ds_port/scripts/
uv run --python 3.15.2 --with Pillow==13.0.0 python tools/profile_voxel.py --tree build/upstream --out build/profile-candidate --steps buildings --profiler none
cmp build/profile-baseline/buildings.bin build/profile-candidate/buildings.bin
cmp build/profile-baseline/windowlights.bin build/profile-candidate/windowlights.bin
uv run --python 3.15.2 --with Pillow==13.0.0 python tools/benchmark_voxel_floor.py --tree build/upstream --pairs 3
uv run --python 3.15.2 --with Pillow==13.0.0 python tools/profile_voxel.py --tree build/upstream --out build/profile-prefix --steps buildings --seconds 45
```

For a terrain prefix, first place a completed `buildings.bin` in the prepared
tree's `3ds_port/romfs/voxel/`, then use `--steps relief --seconds 45`.
For a full low-overhead sampling run on a supported host, select
`--profiler sampling` without a tracing time limit. Compare ordinary wall times
with `--profiler none`. The harness records versions, elapsed time, completion
status, file sizes, hashes and readable hotspot summaries.

Raw measurements: [voxel-python315.json](benchmarks/voxel-python315.json).

## Terrain generation follow-up

One complete local comparison took **370.235 seconds before and 368.321
seconds after**. The 1.914-second difference is about 0.5% on this pair.
The complete `relief.bin` matched byte for byte (3,325,150 bytes, SHA-256
`4d8fc4962c12b3ab603dcbff42f8aaab4bbe533203377c1017d6cce98ffe5c70`).
Both runs used the same prepared English source tree and building input,
with the JIT and profiling disabled. The timing includes terrace and map
corrections. The project's host C relief test passed against the candidate
file, and all eight terrace-mark and ledge tests passed.

Terrain classifies rock pixels by counting nearby top/rim and face pixels.
The candidate keeps one summed-area table of the difference between the
two counts. Positive values still select the top and ties still select the
face. The height solver also precomputes each active node's total weight
before iterating, with the same update order and floating-point operations.

| Synthetic workload | Original median | Candidate median |
| --- | ---: | ---: |
| Terrain voting on a 640×480 canvas | 148 ms | 83 ms |
| Height solve on a 100-node graph | 72 ms | 44 ms |

Each median comes from six alternating before/after pairs. Separate checks
matched the original results on 200 randomly generated canvases and 200
height-solver inputs. The solver comparisons checked exact floating-point
values.

A separate `tracemalloc` pass on the same synthetic canvas measured peak
temporary Python allocations of 23.1 MiB before and 14.0 MiB after. This
measurement covers the voting calculation.

To repeat the full comparison, keep the same generated `buildings.bin` at
`build/upstream/3ds_port/romfs/voxel/buildings.bin` for both runs. Use the
same Python 3.15.2 / Pillow 13.0.0 environment and start with the source
graphics already prepared. The earlier building experiment commit contains
the original terrain generator.

```sh
git show 2783f57:3ds_port/scripts/gen_voxel_relief.py > build/upstream/3ds_port/scripts/gen_voxel_relief.py
PYTHONHASHSEED=0 uv run --python 3.15.2 --with Pillow==13.0.0 python tools/profile_voxel.py --tree build/upstream --out build/terrain-baseline --steps relief --profiler none
cp 3ds_port/scripts/gen_voxel_relief.py build/upstream/3ds_port/scripts/
PYTHONHASHSEED=0 uv run --python 3.15.2 --with Pillow==13.0.0 python tools/profile_voxel.py --tree build/upstream --out build/terrain-candidate --steps relief --profiler none
cmp build/terrain-baseline/relief.bin build/terrain-candidate/relief.bin
```

## Route 110 bridge follow-up

The bridge generator now decodes each metatile's visible pixels once
per layout, constructs the neighborhoods around deck cells from the deck
outward, and scans for curb runs from actual curb tiles. The darkest
texel used for underside and girder faces is cached per cell. Removing the
selected texel invalidates its entry, and an empty cell still uses the
original fallback.

Three paired runs measured `voxel_bridge.block_specs` in fresh Python
3.15.2 processes with Pillow 13.0.0, JIT disabled and hash seed 0. Pairs
alternated run order. Timings exclude importing Python modules and checking
the generated data.

| Bridge generation | Run 1 | Run 2 | Run 3 | Median |
| --- | ---: | ---: | ---: | ---: |
| Original | 8.665 s | 7.932 s | 7.844 s | 7.932 s |
| Candidate | 6.604 s | 6.762 s | 7.470 s | 6.762 s |

The median fell by **1.171 seconds (14.8%)**. All six runs produced 292
models and 2,028 bridge faces. A fingerprint covering the textures, faces,
UVs, placement and shadow data, ground patches and deck table matched
across all runs.

The deck-neighborhood selection matched both the contents and iteration
order of the original on 100 randomized maps. Focused texel checks
covered tied colours, deletion of the selected texel, and the empty-cell
fallback.

A fresh full building export produced `buildings.bin` and
`windowlights.bin` byte for byte equal to the files from the earlier room
optimization experiment. Their hashes are listed under Output identity.
The paired bridge timings above quantify this change's performance.

To reproduce the timing, run this from a checkout with a prepared
`build/upstream` tree. Stage the previous or current copy of
`voxel_bridge.py` in that tree before each timed invocation. Commit
`79135bb` contains the previous copy.

```sh
PYTHONPATH=build/upstream/3ds_port/scripts PYTHONHASHSEED=0 PYTHON_JIT=0 \
  uv run --python 3.15.2 --with Pillow==13.0.0 python -c \
  'import time, voxel_bridge; start = time.perf_counter(); models = voxel_bridge.block_specs({"name": "cycling_road"}); print(time.perf_counter() - start, len(models))'
```

## Round furniture fitting follow-up

The furniture fitter estimates how much of each image row a trial profile
covers. Each search trial changes one ring, so only the neighboring bands
need new widths. The candidate caches each band's row widths by its two
radii and clears the cache when the axis moves. It uses the same radius
interpolation, ellipse arithmetic, pixel thresholds and trial order.

During a full building export on Python 3.15.2 and Pillow 13.0.0, each
object fit ran through both implementations on the same input. Run order
alternated between objects. The test checked the returned axis and
radius profiles before passing the candidate result to the exporter.
Four additional synthetic silhouettes matched, including fractional
heights and a case with more bands than samples.

| Fitting time across 152 authored object fits | Time |
| --- | ---: |
| Original | 26.589 s |
| Candidate | 7.667 s |
| Saved | 18.921 s (71.2% less time) |

These are sums of timed function calls from one export with JIT disabled.
They exclude the rest of model generation and the export itself.
All 152 fits matched. The full export also produced `buildings.bin` and
`windowlights.bin` byte for byte equal to the bridge follow-up's export.
The hashes are listed under Output identity.
