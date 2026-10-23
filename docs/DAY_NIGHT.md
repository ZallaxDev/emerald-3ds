# Optional day/night, encounters and clock

## Scope

The lighting, timed land encounters and bottom-screen clock are independent,
opt-in features. They introduce no NPC schedules, music changes or new assets.
The experimental follow-up also accelerates berry growth using the shared
clock. It does not modify the RTC, daily-event calendar, GBA save layout or
data-pack schema.

In OPTION → ENHANCEMENTS:

| Setting | Values | Default |
| --- | --- | --- |
| DAY / NIGHT | OFF, AUTO | OFF |
| CLOCK SPEED | 1X, 6X, 12X, 24X, 60X, 120X | 1X |
| WILD POKEMON | ORIGINAL, TIMED | ORIGINAL |
| SHOW CLOCK | OFF, ON | OFF |
| SYNC CLOCK | action (shows SAVE TO KEEP) | — |

The port persists `day_night=0|1` and `clock_speed=0..5` in its existing
SD settings file, along with `timed_encounters=0|1` and `show_clock=0|1`.
Unknown enable values disable their feature; unknown speed values select 1X.
The shared clock runs whenever at least one consumer is enabled, a speed
above 1X is selected, or an accelerated clock offset has been retained. Lighting
remains unchanged when DAY / NIGHT is OFF, even with timed encounters enabled.

## Clock behaviour

Initially, 1X reads Emerald's effective local clock, including the saved local-time
offset. The bridge calculates a temporary time instead of rewriting
`gLocalTime` or the game's RTC. It samples at most once per second.

Accelerated rates use elapsed monotonic milliseconds, never game frames.
They start from local game time, retain their phase when changing accelerated
rates, and wrap at midnight. At 24X a day takes one hour; at 60X, 24 minutes;
at 120X, 12 minutes. After acceleration, selecting 1X keeps the accumulated
offset and advances normally instead of jumping back to the system hour.
Changing lighting, encounters or clock visibility does not discard that offset.
During sleep or while the app is closed, time advances at 1X, not the selected
accelerated rate. The original daily events continue to use the real RTC.

### SYNC CLOCK

Once acceleration has been used, the shifted phase is otherwise kept
indefinitely. SYNC CLOCK (Spanish SINCR. RELOJ, French SYNCHRO HORLOGE,
Italian SINCR. OROLOGIO; second ENHANCEMENTS screen) discards that phase.
It reads Emerald's effective local clock, then credits the time since the last
update at the speed in use. It clears the shift and sets CLOCK SPEED to 1X.
Lighting, the sun, the clock display and timed encounters follow the local
clock immediately, including across midnight or when it is earlier than the
accelerated time. Pending bonus berry minutes and fractions are kept and still
reach the trees. Time spent suspended is not credited at the old rate.
The RTC, the game's local-time offset, the daily-event calendar and
`lastBerryTreeUpdate` are not changed. The success sound means the clock was
synced; the failure sound means the local time could not be read, and nothing
changed.

The action does not write `emerald3ds.clock` immediately. Like other game
progress, the synced state is stored with the next successful in-game save
(the cell reads SAVE TO KEEP). Before that save, reloading or soft-resetting
restores the saved shifted phase. The CLOCK SPEED setting becomes 1X immediately; the settings service
persists that setting separately from the in-game save.

## Berry growth and persistence

Berry growth receives the original RTC minutes plus only the additional
`(rate - 1)` time generated while playing. This prevents double-counting the
real minutes, including across midnight. Sub-minute fractions are retained
when changing rates; the original watering, yield, regrowth and expiry rules
still apply. Trees whose original `stopGrowth` flag is set remain stopped.
The normal overworld callback checks for pending bonus minutes; at 120X this
requires at most about two tree scans per real second, not a scan per frame.
Menus and battles accumulate time for the next normal field update.

Successful normal/Hall-of-Fame and completed link saves also write a 40-byte
`SD:/3ds/emerald3ds/emerald3ds.clock` file with phase, pending bonus milliseconds,
wall-time anchor, a CRC of the save image and a record CRC. A temporary file
and `.clock.bak` recovery file support filesystems without rename-overwrite.
Loading or soft-resetting restores only state matching the loaded save image;
a new game resets it. Missing, corrupt or mismatched state safely falls back
to the original RTC without modifying the `.sav` or granting bonus growth.
Keep `.sav`, `.clock` and `.clock.bak` together when backing up/restoring a save.
If writing the auxiliary file fails, the game save remains usable, but clock
offset/fraction persistence is not guaranteed; the failure is logged.

## Bottom-screen indicator

SHOW CLOCK displays `HH:MM`, MORNING / DAY / EVENING / NIGHT and the selected rate
in a compact plate below the location name on the MAP screen. It does not
overlap the map, right-hand buttons, menus or battle UI. Spanish builds show
MAÑANA / DÍA / TARDE / NOCHE. The name plate uses the small font while the clock is
visible; disabling it restores the original layout. Redraws track minutes,
not frames or seconds, and clock-only changes repaint only the footer.
An unavailable clock displays `--:--` and timed encounters fall back to ORIGINAL.

## Timed land encounters

WILD POKEMON → TIMED (Spanish: POKéMON SALV. → POR HORARIO) switches
selected grass slots on eleven Hoenn routes using the same effective clock:
morning 04:00–09:59, day 10:00–19:59, night 20:00–03:59.
The clock display splits the day band at 16:00 into DAY / EVENING
(Spanish: MAÑANA / DÍA / TARDE / NOCHE). This is display-only;
encounter tables still use the original three bands.
Clock acceleration applies to this display, lighting, solar shadows and timed
encounters and berry growth, but not to the original daily events.
This is a custom, conservative Hoenn schedule, not HGSS encounter data.
Slots are zero-based; their weights remain Emerald's original weights.

| Route | Slot (weight) | Original | Morning | Day | Night |
| --- | --- | --- | --- | --- | --- |
| 101 | 2 (10%) | Wurmple | Taillow | Wurmple | Poochyena |
| 101 | 6 (5%) | Wurmple | Wurmple | Zigzagoon | Poochyena |
| 102 | 3 (10%) | Wurmple | Taillow | Wurmple | Poochyena |
| 102 | 5 (10%) | Lotad | Lotad | Lotad | Oddish |
| 103 | 2 (10%) | Poochyena | Wingull | Zigzagoon | Poochyena |
| 104 | 5 (10%) | Poochyena | Taillow | Wingull | Poochyena |
| 104 | 4 (10%) | Marill | Wurmple | Marill | Zubat |
| 110 | 3 (10%) | Electrike | Electrike | Minun | Oddish |
| 110 | 7 (5%) | Gulpin | Gulpin | Gulpin | Zubat |
| 116 | 4 (10%) | Nincada | Taillow | Nincada | Poochyena |
| 116 | 7 (5%) | Taillow | Taillow | Taillow | Zubat |
| 117 | 3 (10%) | Oddish | Marill | Oddish | Volbeat |
| 117 | 7 (5%) | Illumise | Illumise | Roselia | Illumise |
| 119 | 2 (10%) | Zigzagoon | Zigzagoon | Linoone | Oddish |
| 119 | 7 (5%) | Oddish | Tropius | Oddish | Gloom |
| 120 | 6 (5%) | Oddish | Marill | Oddish | Mightyena |
| 121, 123 | 3 (10%) | Shuppet | Oddish | Gloom | Shuppet |
| 121, 123 | 9 (4%) | Wingull | Wingull | Wingull | Duskull |

All other slots/maps, levels and encounter rates remain unchanged. Water,
fishing/Feebas, Rock Smash, caves, facilities, roamers and scripted encounters
remain original. ORIGINAL returns the exact original table without consuming
RNG. Repel, type-attracting abilities, level selection and shiny generation
continue through the existing engine. Ordinary encounters, Sweet Scent,
visible wild spawns and local land-species queries share the selected tables.
Already visible Pokémon keep their species/personality until they disappear
or are encountered; newly spawned ones follow the current phase. Each rule
checks the original species before applying, avoiding unexpected table edits.

## Lighting

The proposed schedule is inspired by, not an exact reproduction of, HGSS:

| Visual time | Lighting |
| --- | --- |
| 00:00–02:00 | Cool blue night |
| 02:00–04:00 | Eased transition to morning |
| 04:00–08:00 | Warm morning |
| 08:00–10:00 | Eased transition to daylight |
| 10:00–15:00 | Original daylight colours |
| 15:00–17:00 | Eased transition to sunset |
| 17:00–18:00 | Warm sunset |
| 18:00–20:00 | Eased transition to night |
| 20:00–24:00 | Cool blue night |

Only towns, cities, routes and ocean routes receive the effect. Interiors,
secret bases, caves and underwater maps retain their existing lighting.
Classic battle screens and menus are unchanged. Outdoor voxel battle
scenery uses the cycle; battlers and battle UI are not recoloured.

Classic 2D multiplies the world layers and non-screen-space sprites during
composition, before applying detected palette fades. BG0 text/windows and
tagged screen-space sprites are excluded. Source palettes and cached tile
textures remain unchanged. Voxel lighting modifies existing per-draw colour
grades, distance haze, sunlight rays, motes and dapples. The PR #57 follow-up
also updates cached voxel meshes for a moving sun as described below;
classic 2D artwork shadows remain fixed.
Encounter time bands keep their original boundaries; the clock display splits
DÍA at 16:00 to show TARDE. The eased visual transitions start earlier.

## Gentle moonlight

The user confirms the final preview on a physical 3DS on October 12, 2026.
This is user-reported acceptance, not measured GPU/FPS validation. At full night, the world
grade is RGB `(0.66, 0.74, 0.92)` rather than `(0.55, 0.62, 0.82)`. An ambient
blend towards RGB `(70, 92, 136)` adds a blue cast even to dark surfaces, capped
at 6%. Its strength is `0.06 * glow * glow`, using the existing eased clock
curve rather than abrupt time-band switches. Daytime and AUTO/OFF behaviour,
encounters, clock speeds, berries and saves are unchanged.

Classic 2D combines this blend with the existing final palette/brightness fade.
Voxel terrain uses multiply-add in its existing texture stage, with the same
grade applied to sprites, reflections and distance haze. Warm building emission
is applied afterwards, and lamp emission keeps its authored amber colour. There
are no new textures, geometry, render passes or draw calls. Palette-faded sprites
also suppress the added ambient component so full black fades remain black.
Interiors, screen-space UI and classic battles remain excluded.

Host regression tests cover all 32,768 GBA colours, every game minute, black and
white fade composition, the production 2D moonlight block, voxel texture stages,
sprite ambient suppression and unchanged warm-emission constants. These are
mathematical/state checks, not a substitute for inspecting the PICA200 result
and frame rate on a physical console.

For console review, compare AUTO and OFF in Petalburg and Rustboro at night,
including dark roofs, grass, water, NPCs, windows and street lamps. Use 60X or
120X to inspect sunset/dawn and walk through doors, open menus, enter a battle
and return outdoors. Check that dark/white fades complete, light stays subtle,
warm windows remain distinct and reflections do not retain a blue veil.

## Experimental follow-up: softer transitions and warm windows

This follow-up is included in PR #57 after the user's physical-3DS acceptance
on 2026-10-12. Its two-game-hour transitions use smoothstep interpolation, with
zero slope at either end, instead of the original linear 30-minute ramps.
At 120X a complete transition takes 60 real seconds rather than 15.

### Clock-driven voxel shadows

The experimental sun moves from east in the morning to west in the evening
between 04:00 and 20:00 on the same effective clock. Building, tree and prop
shadows fall in the opposite direction; character shadows and building-face
lighting share that direction. Shadows are longer near dawn/dusk and shorter
at midday. Direct sunlight fades near the horizon and is absent at night,
while contact shading remains. This is an artistic bounded solar path, not
an astronomical latitude/season simulation. Warm windows do not cast new
shadows or illuminate nearby ground.

For Old 3DS, the sun advances in 15-game-minute steps (7.5 real seconds at
120X), not a full shadow rebuild each frame. A changed solar key invalidates
the affected outdoor mesh signatures; the existing budgeted chunk scheduler
refreshes visible/stale chunks while retaining their previous meshes until
replacements are ready. In-progress old-sun jobs are discarded by the epoch
check, and offscreen chunks refresh on demand. Caster classification caches
are retained; only ray-result caches expire. There are no new shadow textures,
framebuffers, draw passes or shader instructions. Direction changes are
discrete and chunk updates are staggered: this is not continuous per-pixel
shadow motion, and its visual pacing still needs console testing.

Disabling day/night restores the legacy northwest sun even when the clock
or timed encounters remain enabled. Indoors and invalid RTC also retain
the legacy behaviour. Classic 2D uses its original artwork shadows and only
receives the global colour transitions; dynamic cast shadows are voxel-only.

In voxel mode, authored exterior glass panes gradually light up amber during
sunset and fade out towards morning. Coverage starts with Petalburg's shared
four- and five-cell houses, Oldale's houses, Littleroot's two houses and lab,
and Route 104's flower shop. Reused copies of those building models receive
the same effect. Frames, roofs, signs, doors and ground are excluded.
This does not claim coverage for every Hoenn building. The separate
[city-lighting extension](CITY_LIGHTS.md) extends supported glass coverage and
adds original street lamps with local ground pools. On 2026-10-12 the user
reported successful 3DS testing and authorized publication. Classic 2D window emission remains
unimplemented; classic 2D does receive the softer global transitions.

Window light follows `day_night` and the shared effective clock, without a
new setting. With lighting disabled, indoors, or without a valid RTC, its
strength is zero even if the clock or timed encounters remain enabled.
Existing daylight textures are never recoloured or rebuilt as time advances.
The mask is streamed with each building page and sampled in the existing
building pass before haze and palette fades; no extra geometry or draw pass
is added. Building draws with active emission omit sunlight dapples.

Generated building files use VXB8: page height bit 15 marks an L8 glass mask
immediately following that page's RGBA5551 pixels, in the same PICA order.
Unmarked pages have no mask; VXB7 files are no longer read. A drawing with
glass is compacted like any other: each mask texel moves with the faces that
read it, and a drawing whose faces would then read one texel both with and
without glass keeps its layout as drawn (none does today).

Masks have no memory of their own. Each is an L8 piece of the 1 MiB building
page arena in VRAM, taken only while windows glow (DAY / NIGHT on, outdoors,
from mid-afternoon to dawn) and only from room the pages leave free: a mask
never evicts a page, and masks give up their pieces before any page has to go.
A mask streams in after its page's texels, is drawn only once complete, and is
freed two frames after its last draw. A page whose mask finds no room shows
unlit windows and asks again a couple of seconds later; the video log says so
once per page. Nothing is reserved in linear memory, of which an Old 3DS has
under 300 KiB left once the world is loaded. Rebuild the engine and pack
together: the old pack does not contain these masks.

The user confirms lights and Dewford 3D in the local preview on physical
hardware, where masks had a linear pool reserved at start-up. The original
failure's cause was not independently reproduced. The video log reports
`windows loaded` for each mask, or that one found no room yet. If glass still
fails to light, retain that log with the map/time and a screenshot; host tests
do not prove the PICA output on a real console.

Preview hardware regression checklist (not individually reported):

- Back up the save, auxiliary clock files, settings and previous working executable/pack.
- In Petalburg, enable voxel mode, day/night AUTO, clock and 120X. Watch
  sunset and dawn for at least a full transition each.
- Check that only window panes glow and retain their crossbars; rotate the
  camera, walk past the houses and cross the Route 102/104 borders.
- Compare a house's ground shadow at morning, midday and evening. Confirm
  that its direction reverses, its length shrinks at midday, and direct
  shadows fade at night. Stay still across several 15-minute clock steps,
  then move and cross map borders to check cache refreshes and frame pacing.
- Open dialogue and menus, enter/exit a house, and trigger a palette fade;
  lights must not show through roofs, remain after a fade, or tint interiors.
- Disable day/night while leaving the clock or timed encounters on. Return
  to 1X, then sleep/resume the console. Verify continuity and 1X sleep progress.
- Repeat in Spanish and English, and check frame pacing on physical hardware.

Local preview validation (2026-10-12): English and Spanish embedded-data and
engine-only builds passed, with 34 required game symbols and no strong
undefined symbol. Clock, encounters, authored masks, VXB7/VXB8 page readers,
mocked emission-stage/texture-unit cleanup, input, video and pack-parser
tests passed. Solar tests cover the 120X update cadence, suspend/resume,
disabled/indoor/invalid-RTC fallback and 56,700 ray samples against a full
point-by-point oracle, including reversed directions and zero axes. They
also verify caster-cache reuse and mesh-signature invalidation. Lighting
ON/OFF tests pass on macOS and in both staged builds; clock and ray tests
pass ASan/UBSan. Mask/reader/render-state tests passed ASan/UBSan; both generated
building files contain 14 glowing pages and no glow on transparent pixels.
The existing map-footer raster and voxel scheduling harnesses also passed.
All delivered pack entries passed CRC checks, with matching engine ABI:
English `83aad755`, Spanish `a1ef398c`. Delivered SMDH files match the metadata
embedded in each 3DSX; the Spanish preview title/description are localized.
The berry-clock follow-up builds and links in both languages. Clock and storage
tests pass on macOS (also ASan/UBSan) and in both Linux staging trees. Native
berry-update tests pass in both trees for all rates, midnight, speed changes,
duplicate calls and original watering/stopped-tree/expiry behaviour. Storage
tests reject mismatched/corrupt records and cover backup recovery and write
failure without mutating the save image. On 2026-10-12 the user confirmed that
the latest supplied build works on their physical 3DS and authorized uploading
these follow-up changes to PR #57. No console model or itemized results were
provided; this records general hardware acceptance, not individual checklist passes.

The earlier broad terrain mesh harness could not link because its fixture
did not supply `gTileset_Dewford`, which the unchanged tree code references.
The CI repair now supplies that symbol and updates stale tree/settings fixtures;
the complete staged `make verify` passes. This is not a GPU performance
measurement; hardware acceptance is the user's report above.

## Automated validation

- `make -C 3ds_port verify-daynight`: rate validation, midnight rollover,
  smooth phase boundaries, elapsed-time/frame-count independence, invalid
  settings, outdoor exclusions, sleep/resume and RTC failure fallback.
  SYNC CLOCK: last-interval credit at the old rate, kept berry bonus, unchanged
  state and settings on an unreadable RTC, midnight and backwards local time
  afterwards, unshifted snapshot/restore and no credit for suspended time.
  The same scenarios also run with the production settings setter, including
  its old-rate clock update before the new speed is stored.
- `make -C 3ds_port verify-night-windows`: authored glass masks, unchanged
  source art, real VXB7/VXB8 readers, streamed slices, missing-mask fallback,
  bounds checks and opaque-only generated masks, with ASan/UBSan.
- `make -C 3ds_port verify-encounters`: phase boundaries and route/slot/species
  guards in the pure resolver.
- `make -C 3ds_port verify-clock-store`: paired-save CRC, fractional progress,
  corrupt/missing/mismatched records, replacement without overwrite, backup
  recovery, backwards RTC and failed writes without game-save mutation.
- `python3 3ds_port/tests/berry_clock_test.py --tree build/upstream`: the
  production minute-update and growth functions, all six rates across midnight,
  no double credit, speed changes, original watering/stopped-tree/expiry rules.
  Repeat with the Spanish tree. A synthetic four-hour stage takes two real
  minutes at 120X.
- `python3 3ds_port/tests/encounter_tables_test.py --tree build/upstream`:
  production rules against pinned map data, every phase and original fallback,
  with unchanged input tables, levels and rates under ASan/UBSan.
  Repeat with `--tree build/upstream-es` for the Spanish staging.
- `python3 3ds_port/tests/bottom_map_test.py`: partial/full redraw equivalence,
  clock toggles, invalid time, midnight, rate changes and moving between locations.
- Settings shutdown test: all four keys survive the asynchronous writer's latest
  snapshot, including stalled writes and write failures.
- `make -C build/upstream/3ds_port verify PYTHON=python3`: build/link audit
  and the project's host tests, including day/night.
- `python3 -m unittest discover -s builder/tests -v`.
- `python3 tools/release_audit.py --repo . --strict`.

## Hardware validation and acceptance checklist

### PR #57 baseline validation status (2026-10-12)

- Default 3DS development and engine-only release builds: passed in the
  existing devkitPro Docker image using Python 3.12.
- ARM11 hard-float/link and RomFS asset audit: passed, no strong undefined
  symbols; 6,424 assets verified.
- Spanish BPES embedded development build: passed using the user's clean ROM
  in a separate `build/upstream-es` tree. ARM11/link audit and all 6,393
  indexed assets passed; the day/night host tests also passed in this tree.
  Spanish options also include `POKéMON SALV.` and `VER RELOJ`.
- Original touch-screen menu localization: 60 Spanish port labels fit the
  small font's 110-pixel cell area (maximum measured advance: 85 pixels).
- Timed encounter core and all production rule/phase combinations passed,
  including original/invalid setting, indoor and unavailable-clock fallbacks,
  in both language trees. Production table tests passed ASan/UBSan.
- Clock-only/encounters-only consumers, suspension, invalid RTC recovery and
  phase continuity passed. The map raster harness passed partial/full redraw
  comparisons with clock toggles, midnight and location changes.
- Both updated desktop packs passed every entry CRC and matching
  language/engine ABI checks: English `a79c1bd3`, Spanish `b17b20c3`.
- Day/night, input, video decoding, pack and settings shutdown tests: passed.
  Day/night also passed Clang AddressSanitizer/UndefinedBehaviorSanitizer.
- Repository publication audit and `git diff --check`: passed.
- Native syntax checks with VOXEL_LIGHTING=0: passed after exempting
  existing unused-variable/parameter warnings.
- Full `make verify`: blocked by pre-existing incompatible one-argument
  `VoxelTree_Part`/`VoxelTree_GroundMetatile` stubs in
  `3ds_port/tests/voxel_world_hash_test.c`; production declarations take two
  arguments. Both forms are already present in the base revision.
- Builder suite: 42/43 passed; the existing
  `test_inprocess_matches_subprocess` fixture fails because its subprocess
  generator cannot find `romfs/voxel/regions.bin`. This change does not edit
  the builder or its tests.
- Console validation: the user confirmed the supplied build works on their
  physical 3DS on 2026-10-12 and authorized opening the PR. The console model
  and an itemized checklist were not supplied, so this records user acceptance,
  not a claim that every scenario below was individually verified.
- The user also confirmed the latest softer-lighting, window, moving-shadow
  and persistent berry-clock follow-up works on their physical 3DS on
  2026-10-12 and authorized updating the existing PR #57. No itemized hardware
  results or performance measurements were supplied.

### Procedure

Back up the console's game save and settings first. Use an engine/data pack
built together from this checkout, or a development build with embedded data.
Do not replace a release executable while keeping an incompatible data pack.

1. With DAY / NIGHT OFF, compare outdoors, weather, menus and battles against
   the previous build. Verify no tint, brightness or performance regression.
2. Enable AUTO at 1X. Confirm lighting follows the clock set inside Emerald,
   not just the system clock. Check both touch and button navigation.
3. Set 120X and observe a complete twelve-minute cycle in classic 2D and
   voxel mode. Verify smooth dawn, daylight, sunset and night in both eyes
   with the 3D slider enabled, and with the slider off.
4. Check player/NPC sprites, visible wild Pokémon, Surf, reflected sprites,
   field move banners, dialogue and location popups. Text/UI must remain
   legible and untinted. Check voxel fallback/warm-up when entering maps.
5. Enter a Pokémon Center, house, secret base, cave and underwater map at
   night. Each must retain its original lighting. Exit and check the cycle
   returns without flickering or a stale indoor/outdoor grade.
6. Check rain, fog, sandstorm, ash, sunny/drought weather, poison flashes,
   doors, warps, Fly and fade-to-black/fade-to-white transitions.
7. Test classic battles, voxel battle scenery, battle intro/exit and moves
   replacing the battle background. Battle text and Pokémon must not tint.
8. Switch accelerated rates and return to 1X: no phase reset.
   Disable visual clock consumers and re-enable: the offset stays intact.
   Toggle only the lighting while SHOW CLOCK is ON and confirm phase continuity.
   Use game fast-forward and
   verify the visual clock still advances at the selected clock rate.
9. Close/reopen the lid and use HOME/suspend. The effective clock and berries
   must receive only real elapsed time at 1X, not accelerated sleeping hours.
10. Save, restart and soft-reset. Options, saved phase and pending fractions
    persist; unsaved bonus progress rolls back with the save. Test a restored
    `.sav` without its clock files: original RTC fallback, no free bonus growth.
    Daily events stay on the real calendar. Check FPS on Old 3DS if available.
11. Enable SHOW CLOCK at 1X and 120X. Check the MAP footer, midnight, phase
    boundaries, location changes and tapping the map; switch to other screens
    and battles to verify there is no overlay. Disable it to restore the name plate.
12. Compare ORIGINAL and TIMED on routes 101/104/117/121 at each time band,
    with lighting OFF as well as AUTO. Test random encounters, Sweet Scent,
    visible wild spawns, Repel and attraction abilities. Existing visible mons
    must not reroll when the time band or encounter setting changes.
13. Plant and water berries at 120X. Compare stage duration against the species'
    original hours: one real minute equals two growth hours. Save midway, reload,
    switch to 6X and 1X, and confirm there is no restart or repeated growth credit.
    Check visible growth while standing on the route and after menus/battles.
13. Verify unaffected rare slots, caves, Surf, fishing, Feebas, Rock Smash,
    Battle Frontier, roamers and scripted encounters. ORIGINAL must restore
    the original species distribution immediately for new spawns/encounters.

Record console model, firmware/homebrew entry point, language, renderer,
options, tested areas and results. Visual fidelity and GPU behaviour require
hardware testing; passing host tests alone does not establish acceptance.
The current build has user-confirmed console acceptance. Keep this checklist
for future lighting refinements and regression testing.
