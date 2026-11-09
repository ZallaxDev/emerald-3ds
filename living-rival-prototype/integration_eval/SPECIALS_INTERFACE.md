# Specials Interface — Host Mapping (Phase 2.5, Week 2)

**Status:** design + mapping, validated by the Week 5 `test_specials_interface.c`
**Companion:** `../../../docs/superpowers/specs/2026-11-09-SPECIALS_INTERFACE.md` is the approved script-facing catalog (38 specials, parameter encoding, registration). This document does not restate it; it maps each special to the portable core and records the gaps.

## Scope

The catalog is the contract between scripts and the engine. The portable core (`living-rival-prototype/`) is the simulation behind it. The port already implements all 38 specials as stubs (`emerald-3ds/3ds_port/src/living_rival_specials.c`); this mapping is what those stubs become when Phase 3 connects them to the core.

Calling convention is unchanged: specials are `void(void)`, read `VAR_0x8000+`, and return through `gSpecialVar_Result`. All core functions below are ordinary C calls; the specials layer does the var packing.

## Mapping

Status legend: **now** = core API exists; **helper** = small wrapper still needed; **W3/W4** = owned by a later week; **gap** = no source of truth yet.

### Journey queries (1–8)

| # | Special | Core source | Status |
|---|---|---|---|
| 1 | `LRS_GetJourneyPhase` | `core->journeyPhase` | now, but phase names differ (see Normalization) |
| 2 | `LRS_GetTeamVersion` | `core->teamVersion` | now; written by team gen (W4) |
| 3 | `LRS_GetMentorId` | `core->mentorId` (`0xFF` = none) | now |
| 4 | `LRS_GetRivalryLevel` | helper: `(2*hate + fear + notoriety) / 4`, clamped 0–100 | helper |
| 5 | `LRS_GetBadgesEarned` | `core->badgesEarned` | now |
| 6 | `LRS_GetCurrentRegion` | `core->currentRegion` | now |
| 7 | `LRS_GetJourneyArchetype` | `core->archetype` | now, but the two archetype axes differ (see Normalization) |
| 8 | `LRS_GetTemperament` | `core->temperament` | now, with a label translation (see Normalization) |

### Relationship queries (9–14)

| # | Special | Core source | Status |
|---|---|---|---|
| 9 | `LRS_GetHateTowardsPlayer` | `core->hate` | now |
| 10 | `LRS_GetFearOfPlayer` | `core->fear` (raw; `LrEffectiveFear` is for behaviour only) | now |
| 11 | `LRS_GetNotoriety` | `core->notoriety` | now |
| 12 | `LRS_GetFactionReputation` | no field | gap → W4 factions |
| 13 | `LRS_GetVisibilityFlags` | `core->visibilityFlags` | now; set by W3/W4 |
| 14 | `LRS_GetActivityLevel` | `core->activityLevel` | now; set by W3/W4 |

### Goal queries (15–18)

| # | Special | Core source | Status |
|---|---|---|---|
| 15 | `LRS_GetActiveGoals` | `core->goal1_type`, `goal2_type` through the goal translation | now |
| 16 | `LRS_GetGoalTargets` | `core->goal1_target`, `goal2_target` | now |
| 17 | `LRS_HasGoal` | `LrHasGoal` through the goal translation | now |
| 18 | `LRS_GetGoalProgress` | no progress metric | gap → W3/W4 |

### Mutations (19–23)

| # | Special | Core source | Status |
|---|---|---|---|
| 19 | `LRS_CreateMemory` | `LrRemember(core, ext, emotion, intensity, cause, nowDay, targetFacet)` | now |
| 20 | `LRS_ModifyHate` | `LrCalculateHateDelta` + `LrAddHate`, then return `core->hate` | now |
| 21 | `LRS_ModifyFear` | `LrCalculateFearDelta` + `LrAddFear`, then return `core->fear` | now |
| 22 | `LRS_ModifyNotoriety` | `LrAddNotoriety`, then return `core->notoriety` | now |
| 23 | `LRS_SetMentorStudent` | direct `mentorId` / `studentIds` writes | now |

Note: `LrAddHate`/`LrAddFear` auto-create a memory for `abs(delta) >= 10`, by design. A script calling `LRS_ModifyHate(40)` therefore also feeds the drift pipeline.

### Team / party (24–27)

| # | Special | Core source | Status |
|---|---|---|---|
| 24 | `LRS_GetPartySpecies` | generated party | W4 team gen |
| 25 | `LRS_GetPartyLevel` | generated party | W4 |
| 26 | `LRS_GetPartyCount` | generated party | W4 |
| 27 | `LRS_GetPreferredTypes` | `core->preferredTypes[3]` | now |

### Gazette / news (28–31)

All four depend on `WorldState` and the chronicle: **W4**. `GazetteEntry` already matches the catalog.

### Simulation control (32–33)

`LRS_ForceDailyTick` / `LRS_ForceHourlyTick` call `LrSimulateTo` / the tick entry points: **W3**.

### Factions / territory (34–38)

All five depend on `WorldState`: **W4**.

## Normalization the specials layer must perform

### Goals — the two numberings disagree

| Core value | Catalog value | Notes |
|---|---|---|
| `LR_GOAL_NONE` (0) | 0 | |
| `LR_GOAL_AVENGE` (1) | 1 | |
| `LR_GOAL_PROVE` (2) | 2 | |
| `LR_GOAL_RISE` (3) | 4 | |
| `LR_GOAL_RICH` (4) | **none** | catalog has no Rich; propose adding 7 |
| `LR_GOAL_SETTLE` (5) | 3 | |
| `LR_GOAL_PROTECT` (6) | 5 | |
| — | 6 = Hunt | an intent, not a stored goal; `LRS_GetActiveGoals` never emits it |

`LRS_HasGoal(goalType)` translates the catalog value first; `6` (Hunt) always returns false.

### Phases — same count, different names

Core: `ROOKIE, BADGE_HUNT, TOURNAMENT, ELITE_CHALLENGE, MENTOR, RETIRED`.
Catalog: `ROOKIE, TRAVELER, VETERAN, MASTER, LEGEND, RETIRED`.

Positional mapping is close but `MENTOR` is not `LEGEND`. **Decision for Phase 3:** either rename the catalog's slot 4 to `MENTOR` (recommended; the core model is the one the simulation actually drives) or add a translation with an explicit `MENTOR → 4` rule. Until then the specials layer maps 0–3 and 5 positionally and maps `MENTOR → 4`.

### Temperament — two different axes

Core (EnRage temperaments): `HOTHEAD, COWARD, PROUD, COLD, GREEDY, LOYAL, DRIFTER, STEADY`.
Catalog (Pokémon natures): `BOLD, TIMID, CALM, JOLLY, SERIOUS, RASH, ADAMANT, NAUGHTY`.

Recommended translation for the script label: `BOLD→PROUD`, `TIMID→COWARD`, `CALM→STEADY`, `JOLLY→HOTHEAD`, `SERIOUS→COLD`, `RASH→GREEDY`, `ADAMANT→LOYAL`, `NAUGHTY→DRIFTER`.

### Archetype — the two axes are unrelated

Core: `PRODIGY, GRINDER, SPECIALIST, EXPERIMENTER, LATE_BLOOMER, MENTOR, MERCENARY, LEGACY` (how a journey is run).
Catalog: `BALANCED, AGGRESSIVE, DEFENSIVE, SPEED, SPECIALIST, COLLECTOR, BREEDER, EXPLORER` (battle/collection style).

There is no honest 1:1 map. **Recommendation:** scripts should expose the core archetypes and the catalog's list should be replaced in Phase 3. Pretending a `GRINDER` is `BALANCED` would leak a wrong value into dialogue and team templates.

## Validation plan (Week 5)

`test/test_specials_interface.c` will:

1. Declare all 38 signatures exactly as the catalog lists them and compile them against host stubs.
2. Assert each parameter pack fits in 16 bytes (the current maximum is 6).
3. Assert every special has a mapping entry above, and that "now" entries call an existing core symbol.
4. Exercise the two translation tables: every core goal maps to a catalog value and back; `Hunt` is rejected by `LRS_HasGoal`; `MENTOR` maps to 4.
5. Exercise `LRS_CreateMemory` end-to-end against `LrRemember` and assert the long-term return value.

## Open items for Phase 3

- Add a `Rich` value to the catalog (or drop `LR_GOAL_RICH` from the specials surface).
- Resolve phase slot 4 (`MENTOR` vs `LEGEND`).
- Replace the catalog's archetype list or define an explicit translation.
- Define goal progress (slot-based) before `LRS_GetGoalProgress` leaves stub status.
- Define faction reputation storage in `WorldState` (W4).
