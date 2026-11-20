# Voxel generation experiment: Python 3.15.2

Fork experiment, November 19, 2026. Source base: `6699c19a2a36bcf90be62859f43f70f405a2e777`.
Python 3.15.2 and Pillow 13.0.0 were selected explicitly for these measurements.
Release version pins follow `upstream.lock`; this experiment does not edit them.

## End-to-end generator benchmark

Compare the original project at `6699c19` with the complete optimization
stack at `a32152e`. Both runs use the same prepared English source inputs
from pokeemerald `76463dac`. The staged scripts match their respective
commits and 10,725 shared input files were checked for byte equality.
Both use Python 3.15.2 and Pillow 13.0.0 on Linux x86-64, with the JIT disabled,
hash seed 0, and no profiling.

The benchmark runs regions, signposts, buildings, terrain relief, and
intro generation in sequence. Each step is timed from process launch to
exit. The building step also generates the window-light file, and relief
reads the buildings just generated. The reported totals sum the measured
wall time of the five steps. Source preparation, 3DS compilation,
the building generator's optional geometry verification, and other
CI work are outside this measurement.

| Generator | Original | Optimized | Seconds saved |
| --- | ---: | ---: | ---: |
| Map regions | 6.667 s | 5.173 s | 1.494 |
| Signposts | 2.751 s | 2.907 s | -0.156 |
| Buildings and window lights | 268.854 s | 194.762 s | 74.092 |
| Terrain relief | 392.074 s | 269.450 s | 122.624 |
| Intro leaves | 0.290 s | 0.245 s | 0.045 |
| **Total** | **670.636 s (11:11)** | **472.538 s (7:53)** | **198.099 (29.5%)** |

All six generated files matched byte for byte. The timings come from
one local pair, with the baseline followed by the optimized run. The
earlier interrupted attempt is excluded. The individual experiments
below ran at different times and sum to an estimate of 255.5 seconds.
Use **198.1 seconds** as the measured end-to-end generator savings on
this machine. A separate CI comparison would measure the effect on the
complete `build-3ds` job.

Raw timings, sizes and SHA-256 hashes:
[voxel-end-to-end-python315.json](benchmarks/voxel-end-to-end-python315.json).

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

## Texture page loops follow-up

The atlas packer keeps a queue of column heights for the current image
width. Advancing by one column updates the queue and reads its maximum
instead of slicing and scanning the whole width. The scan stops at the
lowest possible placement height. A left-to-right scan preserves the
existing leftmost tie break.

The texture writer splits each GPU texture address into a row offset and a
column offset. Each page calculates those offsets once, and each pixel
adds the two values. Colour and optional window glow use the same address.
The colour still uses little-endian RGBA5551 with the same alpha cutoff.

The candidate matched the original on 1,000 randomized atlas packing
inputs and six texture widths, including offsets on either side of tile
boundaries. The existing building crop tests passed.

| Timed work | Calls | Original | Candidate | Saved |
| --- | ---: | ---: | ---: | ---: |
| Atlas placement search | 5,420 | 11.758 s | 3.582 s | 8.176 s |
| Texture and glow image writes | 3,878 | 5.703 s | 1.995 s | 3.709 s |
| Combined | | 17.462 s | 5.577 s | 11.885 s |

These are summed paired call times from a complete export using the same
Python 3.15.2 and Pillow 13.0.0 environment and JIT disabled. Each pair
used the same inputs, and the order alternated. The write timing covers
7,482,550 pixels, including 827,499 with a glow lookup. The timing excludes
preparing the page offset tables and the rest of the export.

Every packing result and each page's colour and glow buffers matched the
original. The complete `buildings.bin` and `windowlights.bin` also matched
the previous export byte for byte.

## Texture compaction loops follow-up

When testing how a triangle overlaps a row of texels, the candidate
computes the row's shared term once and skips bisection if the whole
remaining interval already passes. It preserves the edge tolerance and
the arithmetic of the overlap test. When marking the one-pixel safety
margin used by texture compaction, it gathers the distinct sampled
texels first so adjoining faces share the work.

The sampler matched the original on 3,000 generated triangles, including
partly clipped and degenerate triangles. The existing building crop tests
passed.

The paired comparison on Python 3.15.2 and Pillow 13.0.0 covered 2,858
models and 175,492 triangles. Summed compaction time fell from **12.895
seconds to 10.782 seconds**, a saving of **2.113 seconds (16.4%)**. Run
order alternated between models, and the JIT was disabled. This measures
compaction with the updated sampler. Sampling gains in other callers are
outside this timing.

Every compared texture and triangle list matched. The complete
`buildings.bin` and `windowlights.bin` matched the previous export byte
for byte.

## Cumulative timing estimate

| Change | Measured seconds saved |
| --- | ---: |
| Room floor and shell classification | 107.0 |
| Terrain voting and height solving | 1.9 |
| Route 110 bridge generation | 1.2 |
| Round furniture fitting | 18.9 |
| Texture page packing and writing | 11.9 |
| Texture compaction | 2.1 |
| Terrace edge scoring | 86.6 |
| Terrain region labeling | 8.3 |
| Terrain canvas scans | 6.2 |
| Terrain neighborhood lookups | 11.4 |
| Approximate sum of separate measurements | **255.5** |

The sum combines individual experiments with different baselines and
workloads. The paired end-to-end benchmark above measured 198.1 seconds
of wall-clock savings across the full generator sequence.

## Terrace edge scoring follow-up

The terrace solver tries several shapes and heights for each editable tile.
Only the four borders are used in those comparisons. The candidate caches
the border heights for repeated pieces and the step penalty for identical
border pairs. Both caches are bounded and local to one group of maps.

Each tile's trial pieces share a snapshot of its neighbors. The solver
still applies the chosen piece immediately, so the next tile sees the
updated border. The direction order, floating-point sums, and the rules
for held tiles, fins and pits stay the same.

The comparison matched 100 randomized solves with different heights, tile
types, and held edges. The eight terrace and ledge fixtures passed.

Across **76 map groups and 188,300 tile decisions**, the original block
solver took **107.692 seconds** and the candidate took **21.074 seconds**,
a difference of **86.617 seconds (80.4%)**. The timings sum paired calls
from one complete terrain export under Python 3.15.2 and Pillow 13.0.0
with JIT disabled. Run order alternated by group, and both versions used
the same inputs. The timings cover the block solver within terrain export.

The two solvers returned identical ordered tile choices for every group.
The final `relief.bin` matched the previous terrain export byte for byte,
with the same SHA-256 in the terrain follow-up. The host C relief reader
and its terrain assertions passed against the new file.

## Terrain record generation follow-up

The record writer turns solved heights into walls, decks, cuts and ground
under raised tiles. The trial reused sorted tile order, cached the minimum
and maximum of each tile's 5×5 lattice, and cached wall and floor
classifications. The bridge discovery pass cleared the classification
caches when it added bridge cells.

| Paired record writer calls | Seconds |
| --- | ---: |
| Existing writer | 42.291 |
| Cached trial | 41.926 |
| Difference | 0.365 (0.9%) |

The timed calls covered **76 map groups**, **188,300 input tiles** and
**39,680 written records** in one terrain export on Python 3.15.2 and
Pillow 13.0.0 with JIT disabled. Order alternated by group and both
implementations started with independent copies of the bridge-cell set.
The ordered records and discovered bridge cells matched. The resulting
`relief.bin` matched the terrace scoring follow-up byte for byte. The
eight terrace and ledge fixtures and the host C relief reader passed.

The branch retains the existing writer because the observed difference
was only 0.365 seconds in one comparison and the caches add state for
each map group. The cumulative estimate counts the accepted changes.

## Terrain region labeling follow-up

The flood fill queues horizontal runs of ground or top pixels and writes
their region labels together. Each run looks for connected pixels in the
rows above and below. Neighboring map tiles still use the original seam
rules, and the first pixel in row order still determines the region number.

The old and new algorithms produced identical region grids and sizes on
300 randomized canvases with a mix of open and blocked seams. The eight
terrace and ledge fixtures also passed.

| Paired region labeling calls | Seconds |
| --- | ---: |
| Pixel-by-pixel fill | 17.826 |
| Horizontal runs | 9.544 |
| Saved | 8.282 (46.5%) |

The timed calls covered 33 map groups, 59,703,808 canvas pixels and
16,129 regions in one terrain export on Python 3.15.2 and Pillow 13.0.0
with JIT disabled. The run order alternated by group and each version
started with an empty seam cache. The original pixel loop ran as a
function with local variables, like the original generator. Each pair
produced the same region numbers, label grids and sizes.

The complete `relief.bin` matched the previous terrain export byte for
byte, with the SHA-256 recorded in the terrain generation follow-up.
The host C relief reader and its terrain assertions passed.

## Terrain canvas scan follow-up

The region-width pass groups neighboring labels and marks their columns
in an integer bit mask. Counting set bits gives each region's distinct
width. The drop pass uses byte patterns to skip pixels outside face and
rim runs. It processes columns and rows in the same order and applies
the existing rules for orientation, covered ground and drop height.

Both scans matched their original versions on 350 randomized canvases,
including adjacent faces and rims, blocked ground, and west- and
east-facing terrain. The eight terrace and ledge fixtures passed.

| Paired scan calls | Original | Candidate | Saved |
| --- | ---: | ---: | ---: |
| Distinct columns per region | 2.822 s | 0.937 s | 1.884 s |
| Row and column drop runs | 8.363 s | 4.058 s | 4.305 s |
| Combined | 11.185 s | 4.995 s | 6.189 s |

Each comparison covered 33 map groups and 59,703,808 canvas pixels on
Python 3.15.2 and Pillow 13.0.0 with JIT disabled. The paired call order
alternated by group. Region widths and ordered drop samples matched for
every group.

The complete `relief.bin` matched the previous export byte for byte,
with the SHA-256 listed in the terrain generation follow-up. The host C
relief reader and its terrain assertions passed.

## Terrain neighborhood lookup follow-up

The plain-ground search builds an index of each visited row by dominant
colour. A lookup scans matching entries in the original 49×49 search
window and filters the caller's excluded cells at query time. The
colour quality, distance and row-order tie break still select the tile.
For rock cut masks, the generator reuses each neighboring metatile's
ground-colour palette before feeding the original silhouette flood fill.

Both changes matched the original on 360 randomized queries apiece,
including changing exclusions, forced colours and map edges. All eight
terrace and ledge fixtures passed.

| Paired lookup calls | Calls | Original | Candidate | Saved |
| --- | ---: | ---: | ---: | ---: |
| Plain-ground search | 18,559 | 28.166 s | 21.268 s | 6.898 s |
| Rock cut masks | 97,578 | 9.948 s | 5.415 s | 4.533 s |
| Combined | 116,137 | 38.114 s | 26.682 s | 11.431 s |

Times sum paired calls from one complete terrain export on Python 3.15.2
and Pillow 13.0.0 with the JIT disabled. The order alternated between
calls, and the original and candidate used separate per-art lookup caches.
Every tile choice and mask matched. The complete `relief.bin` matched the
previous export byte for byte, and the host C relief reader passed.

## Terrain search and mask follow-up

This round reduces three kinds of repeated work:

- Plain-ground lookup groups each row's matching tiles by quality, then
  searches outward from the nearest column until it finds an eligible tile.
  It checks current exclusions and retains the original row and column
  tie breaks.
- The cutout flood fill advances a whole bit mask at once using four-way
  connectivity. Terrain cuts cache the pixels for each metatile colour and
  combine only the colours present on the surrounding ground.
- Terrain voting visits runs of pixels whose kind can change. Rim pixels
  still contribute to the same vote totals.

The complete terrain generator, including terrace and map corrections,
took **293.130 seconds before** and **274.032 seconds after**.
This saved **19.098 seconds (6.5%)** in one sequential local
pair against the already optimized `a6f06d5` checkout. Both runs used the
same prepared English source inputs and building file, Python 3.15.2,
Pillow 13.0.0, hash seed 0, and JIT and profiling disabled. This measures
the terrain generator alone. It is a separate comparison from the earlier
five-generator benchmark.

The new `relief.bin` and `signposts.bin` both matched the original
generator's files byte for byte. The host C relief reader passed. The
signpost fixture and all eight terrace and ledge checks passed on Python
3.15.2 and 3.12.14. Focused comparisons matched 1,112 sign-mask cases
(including every 3×3 pattern), 1,800 ground searches with changing
exclusions, and 1,800 terrain cut masks. The builder suite passed
264 tests and skipped two with optional follower downloads disabled.

Raw measurements and hashes:
[voxel-terrain-algorithms-python315.json](benchmarks/voxel-terrain-algorithms-python315.json).

## Terrain colour and tile matching follow-up

The colour-frequency lookup now returns a sole winner directly. On a tie,
the original 64-bit CPython set ordering still decides, with one insertion
per distinct value in first-seen order. Integer hashes use the same modulus
and skip the modular inverse when the denominator is one. The portable
ordering is also used by the building generator.

Terrace matching now indexes rendered reference tiles by RGB bytes and
uses two 256-bit masks per tile to compare top and face pixels. The
reference order, exact match precedence, overlap thresholds, and RGB
distance tie breaks are preserved. Each layout's rendering owns its index
so alternate readings can keep their own palettes.

The complete terrain generator took **286.991 seconds before** and
**231.173 seconds after**, a saving of **55.818 seconds
(19.4%)**. This is one local pair against `36bc44b` on the same
prepared English source tree with Python 3.15.2, Pillow 13.0.0, hash seed 0,
and JIT and profiling disabled. A separate building verification also ran
on the same host. The timing covers terrain generation through the final
corrections.

The intermediate terrain export and the final `relief.bin` matched byte
for byte. A fresh building generation produced identical `buildings.bin`
and `windowlights.bin`. Focused comparisons also matched 797 real metatile
lookups across ten layouts and 800 cases each for colour frequency and
portable set order, including ties and equal numeric values of different
types. The host relief reader, eight terrace and ledge checks, and builder
suite passed. The builder suite recorded 264 passes and two skips with
optional follower downloads disabled.

Raw measurements and hashes:
[voxel-terrain-matching-python315.json](benchmarks/voxel-terrain-matching-python315.json).

## Terrace solver follow-up

The terrace piece solver skips trials for a stable tile until the tile,
one of its neighbours, or the current pit penalty changes. Tiles still
run in the original order, so a change reaches later neighbours in the
same sweep and earlier neighbours in the next sweep.

The region joiner tracks which current regions must stay apart. When two
regions join, it transfers their restrictions to the surviving region.
This avoids looking up the original face endpoints at every candidate
join and keeps the original region representatives and join order.

| Paired authored calls | Calls | Before | After |
| --- | ---: | ---: | ---: |
| Region joins | 76 | 7.169 s | 3.815 s |
| Piece solver | 76 | 26.077 s | 9.511 s |

The focused timing sums paired function calls on the same inputs.
Call order alternated by group. Every ordered region assignment, tree
ground assignment, and chosen piece matched. The generated checks also
matched all 180 piece grids and 300 region grids.

The complete terrain generator took **232.797 seconds before** and
**215.966 seconds after**, a saving of **16.831 seconds
(7.2%)**. This is one sequential local pair against `cd6a48b` on the
same prepared English source tree with Python 3.15.2, Pillow 13.0.0,
hash seed 0, and JIT and profiling disabled. A separate targeted check
also ran on the host. The timing includes the final terrain corrections.

The intermediate terrain export and final `relief.bin` matched byte for
byte. The host relief reader and all eight terrace and ledge checks
passed. The builder suite recorded 264 passes and two skips with optional
follower downloads disabled.

Raw measurements and hashes:
[voxel-terrace-sweeps-python315.json](benchmarks/voxel-terrace-sweeps-python315.json).

## Terrain topology and corners follow-up

The terrace batch reads outdoor connections and alternate layouts once
from the map metadata and shares them across groups. A standalone solve
reads a fresh snapshot. All 150 ordered connections and
13 alternate layout mappings matched the
previous implementation. A temporary-map check verified that a later
standalone read picks up a change to a map's outdoor status.

Corner scoring now intersects the heights of neighbouring tiles once
per visit and reuses those heights for the three candidate levels. The
height trials, tie break and update order are preserved. All
76 authored solves and
350 generated corner grids
returned identical heights.

| Paired calls | Calls | Before | After |
| --- | ---: | ---: | ---: |
| Full topology read | 12 | 1.294 s | 0.326 s |
| Authored corner solving | 76 | 2.007 s | 0.804 s |

Call order alternated within each focused comparison. The topology row
measures the cost of reading the same connections and alternate layouts
once. In the full terrace batch, the previous implementation rebuilt
the connections twice per group. The new batch shares one read for those two steps. The relief fixes
reader makes its own separate read.

The first complete terrain generator pair took
206.683 seconds before and
218.958 seconds after, with targeted
checks also running on the host. A sequential repeat ran the candidate
first and pinned both runs to the same logical CPU without overlapping
benchmarks. It took **195.975 seconds before** and
**188.337 seconds after**, a saving of **7.638 seconds
(3.9%)** in that pair. Both pairs compared against `298ea00` on
the same prepared English source tree with Python 3.15.2, Pillow 13.0.0,
hash seed 0, and JIT and profiling disabled. These timings include final
terrain corrections.

The intermediate terrain export and final `relief.bin` matched byte for
byte. The host relief reader, signpost check, and eight terrace and ledge
checks passed. The builder suite recorded 264 passes and two skips with
optional follower downloads disabled.

Raw measurements and hashes:
[voxel-terrain-topology-python315.json](benchmarks/voxel-terrain-topology-python315.json).

## Building verification follow-up

Room and bridge verification now indexes model placements once for the
pass and looks up the placements for each layout. Alternate room states
reuse the index and still evaluate their placement conditions. The
normal build checks every model, room and bridge, and writes proof images
for failures. Running the verifier directly still writes all images by
default. The focused diagnostic test also checks that a failed model
writes the same image as the full-diagnostics mode.

| Local building generator measurement | Before | After |
| --- | ---: | ---: |
| Complete generation, verification and export | 908.89 s | 253.21 s |
| Verification | 714.80 s | 49.11 s |
| Model pixel checks | 44.97 s | 7.44 s |
| Room checks, including alternate states | 665.41 s | 26.03 s |
| Placement searches, including export | 634.49 s | 14.77 s |
| Image writes across the generator | 29.98 s | 1.95 s |

The complete generator saved **655.69 seconds** in this local
pair. The rows overlap and describe where time was spent. Both versions
ran on the same prepared English source inputs with Python 3.15.2 and
Pillow 13.0.0, pinned to the same logical CPU with the JIT disabled.
The baseline wrote all diagnostic images. The candidate used the normal
build's new failure-only setting.

The results matched for 3,074 model checks, 188 rooms, 5
alternate room states, the Route 110 bridge, and interior lint.
`buildings.bin` and `windowlights.bin` matched the baseline and original
exports byte for byte. The timings cover the building generator through
export. The hosted `build-3ds` job was outside this comparison.

[Raw timing, output sizes, and hashes](benchmarks/voxel-building-verification-python315.json).

## Furniture search and placement reuse follow-up

Build an index of cell drawings once per room. Exact furniture matches
start at positions with the right first tile. Loose matches test each
distinct tile drawing for the anchor and visit matching positions in
the original row order. Both passes keep the same priority and bounds,
so overlapping placements claim pixels in the same order.

When a build verifies and exports, the exporter reuses the model
placements that verification already found. An export by itself still
finds its own placements.

| Local building generator measurement | Before | After |
| --- | ---: | ---: |
| Complete generation, verification and export | 231.81 s | 221.95 s |
| Model generation | 143.46 s | 133.85 s |
| Furniture reuse searches | 11.45 s | 3.81 s |
| Placement lookup, including export | 12.88 s | 10.43 s |
| Export | 41.47 s | 39.23 s |

This pair saved **9.86 seconds (4.3%)** against
`1809f55`. Both versions ran on the same prepared English inputs with
Python 3.15.2 and Pillow 13.0.0, pinned to the same logical CPU.
Both ran full verification and wrote proof images for failures only.
The focused timings overlap with the enclosing steps.

All 500 generated room cases returned the same ordered furniture
placements, including overlaps and edge positions. Every full-build
verification result matched. Both `buildings.bin` and `windowlights.bin`
matched the baseline and original exports byte for byte.

[Raw timing and output hashes](benchmarks/voxel-building-reuse-python315.json).
