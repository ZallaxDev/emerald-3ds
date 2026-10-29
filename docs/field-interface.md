# Optional field interface

All three switches are OFF by default, on the second ENHANCEMENTS page.

- **AREA WILD LIST**: tap the map name plate to switch between the Hoenn map
  and the current map's encounters. Tap the list to cycle available methods:
  grass, Surf, Rock Smash, Old Rod, Good Rod and Super Rod. Pages with no table
  are omitted. Species not yet seen in the Pokédex appear as `???`.
- **REPEL STEPS**: the name plate shows the game's remaining Repel steps while
  a Repel is active. This reads `VAR_REPEL_STEP_COUNT` and never changes it.
- **CLOCK NOTICE**: a dismissible banner reports when an existing game's saved
  clock is missing, invalid or belongs to another save, so the 3DS clock is used.
  A valid temporary/backup clock recovered by the existing loader suppresses
  the warning. Starting a new game clears the previous game's warning. A tap
  dismisses the banner for that load; a later affected load can show it again.

The list shows the current map, including when a different region-map cell was
previously selected. It reads the same time-of-day grass resolver as encounters
and follows Altering Cave's selected set. Duplicate slots combine their chance
and level range. Percentages are table-slot weights conditional on the method,
not the chance of an encounter on the next step. They do not model ability/type
bias, Repel eligibility, fishing success, Feebas tiles, swarms, roaming Pokémon,
scripted encounters, or other special encounter substitutions. Unknown species
names are hidden, but their level ranges and table weights remain visible.

`CtrEncounters_DisplaySpecies(uint16_t)` has a weak identity implementation.
An optional randomizer can provide a strong, side-effect-free implementation
that projects its actual wild-species mapping. The projection occurs before
aggregation and the Pokédex visibility check. This branch alone does not
randomize encounters. Combined randomizer behaviour requires integration checks
against the randomizer's live mapping.

Verification uses production functions extracted into SDK-free mock harnesses:
`make -C 3ds_port verify-bottom-map verify-clock-store HOSTCC=cc PYTHON=python3`.
The map harness checks timed/method tables, aggregation,
seen-name masking, mapped-species masking, page/input transitions, hit targets,
Repel read-only snapshots, clock dismissal and partial/full redraw equivalence.
The clock harness checks save binding, corruption, backups, serial/status
transitions and new-game reset. The new-game hook lives in
`patches/pokeemerald/0078-field-clock-notice.patch` and must be applied by the
normal bootstrap when preparing a full game tree.

These host tests do not establish text readability on a physical screen or
hardware performance. Full ROM/3DSX builds and emulator/device testing remain
separate checks.
