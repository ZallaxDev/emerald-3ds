# Living Rival Phase 2.5 — Week 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement deterministic needs, life events, meaningful bounded absence progression, snapshots, and a shared-chronicle Gazette prototype.

**Architecture:** Extend the existing C99 core with caller-owned world/runtime state. A chronological scheduler combines detailed ticks with bounded summaries; events and Snap write one canonical chronicle. All mutable replay inputs are captured by a pointer-free snapshot format.

**Tech Stack:** C99, GNU make, MSYS2 GCC 16.2; existing LCG and test harness; no new dependencies.

**Spec:** `docs/superpowers/specs/2026-11-09-living-rival-week3-design.md` (approved). The workspace-level Weeks 1–2 handoff is background, not authority over the approved changes to offline time and catch-up.

## Global Constraints

- Nintendo 3DS is the runtime target, with possible PC/mobile ports. No GBA runtime, SaveBlock integration, or obsolete GBA memory/cycle constraints apply.
- Preserve existing uncommitted changes; implementation belongs in an isolated feature worktree. Do not merge, push, publish, or deploy without permission.
- C99, fixed-size arrays, no allocation, no platform headers, no mutable globals. Core includes limited to stdint/stdbool/stddef/string and project headers; stdio is test-only.
- Existing v1 trainer blob and 87/233-byte trainer layouts remain unchanged. Never renumber existing enums.
- Public names use Lr/LR_. Integer arithmetic only, explicit overflow checks, per-mille where multipliers apply.
- No player-level input, hidden team weakening, story implementation, RTC API calls, file I/O, full party generation, or claim of all 270 events.
- Same initial state, content version and ordered requests reproduce identical results. Detailed-only partition equivalence is required; summary partition equivalence is explicitly not promised.
- Inline execution is preserved. Final fresh-context whole-change review, no per-task agent dispatch. This plan needs user review before execution.

## Review Focus

1. RTC rollback then recovery must not count the same offline interval twice (Task 8).
2. Large weighted pools must reach candidates beyond the existing RNG's 16-bit output range (Task 5).
3. Goal reordering and expiry must not transfer progress timestamps to a different goal (Task 6).
4. Snapshot corruption near the last field must leave the complete destination untouched (Task 9).
5. Chronicle overwrite must not let an old Gazette ID mark a different event heard (Tasks 1 and 10).

## Workspace and verification conventions

All paths are repo-relative (`emerald-3ds/` is the repository). HEAD at planning: d82f425. Main is dirty with unrelated Phase 2 and patch work. Read the approved spec and this plan; create an isolated feature worktree using using-git-worktrees at execution time. Copy only this plan and its approved spec into the worktree if they remain untracked, leaving their originals alone. Commit those two exact documents on the feature branch before Task 1. Never stage the entire docs directory or repository.

Initialize this plan's executing-plans workspace/ledger and record every ruling. Baseline command in the worktree, with `C:\msys64\usr\bin` prepended to PATH: `make -C living-rival-prototype test`. Expected: exit 0, eight test binaries PASS. Record baseline without altering the main checkout's build output.

In every task below RED means run the stated command and observe the new named assertions fail (or missing declared API at compile/link time), not an unrelated tool error. GREEN means the same command exits 0 with all binaries PASS. Use PowerShell `$env:PATH = 'C:\msys64\usr\bin;' + $env:PATH` before builds. No truncated live pipelines. Task-done runs the full `make -C living-rival-prototype test` after each task. Makefile wildcards already discover new sources/headers/tests.

## Files and responsibilities

Under `living-rival-prototype/`:

| Files | Responsibility |
|---|---|
| `include/lr_chronicle.h`, `src/lr_chronicle.c`, `test/test_chronicle.c` | Canonical event ring, stable IDs and heard state |
| `include/lr_runtime.h`, `src/lr_runtime.c`, `test/test_runtime.c` | World ownership, sidecars, initialization, semantic validation |
| `include/lr_needs.h`, `src/lr_needs.c`, `test/test_needs.c` | Need schedules/stress and deterministic Snap |
| existing `include/lr_affect.h`, `src/lr_affect.c`, `test/test_affect.c` | Cause-aware affect without changing legacy callers |
| `include/lr_life_events.h`, `src/lr_life_events.c`, `src/lr_event_catalog.c`, `test/test_life_events.c` | Typed event validation, picker/effects, small fixture catalog |
| `include/lr_daily.h`, `src/lr_daily.c`, `test/test_daily.c` | Memory aging, tracked goals, ordered daily work |
| `include/lr_absence.h`, `src/lr_absence.c`, `test/test_absence.c` | At most seven summary beats per trainer |
| `include/lr_simulation.h`, `src/lr_simulation.c`, `test/test_simulation.c` | Calendar boundaries, offline conversion, work accounting |
| `include/lr_snapshot.h`, `src/lr_snapshot.c`, `test/test_snapshot.c` | Versioned canonical in-memory snapshot bytes |
| `include/lr_gazette.h`, `src/lr_gazette.c`, `test/test_gazette.c` | Queries and bounded text, no rendering |
| existing `include/living_rival.h`, `README.md`, `integration_eval/SAVE_SERIALIZATION_DESIGN.md`; new `integration_eval/GAZETTE_UI_PROTOTYPE.md` | Public exports and truthful integration contracts |

Add each public header to `living_rival.h` in its owning task. Do not add all headers before they exist. Test functions are static named functions called from each test's main, using lr_test.h.

Each Task's Step 1 includes its executable assertion anchor below as well as the named cases described in the task. Extend the fixtures rather than substituting these anchors for the other required cases.

## Shared data contracts (defined by the owning task)

All counts/indices are explicitly bounded before indexing. Native runtime structs may have padding but are zero-initialized; canonical comparison/serialization is fieldwise, not compiler-layout dependent. Do not pack live world structs for convenience.

- Time: uint32_t absolute game minutes, range 0..94371839 (day 65535, minute 1439); this avoids silently wrapping existing uint16_t memory days. Offline clock samples are uint64_t UTC minutes, not seconds. Unsupported time ranges fail without mutation. No counter wraps silently.
- `LrWorldEvent`: uint32_t id/minute; uint16_t contentId/titleId/lineIds[4]/people[4]; uint8_t kind/region/place/factions[4]/outcomes[4]/heard. Absent people = LR_GOAL_TARGET_NONE, absent factions = 255. Event kinds LIFE=0, CLASH=1, BADGE=2, RETIREMENT=3, DEATH=4 (new core enum, not port encoding).
- `LrChronicle`: events[500], uint16_t count/head (next write), uint32_t nextId (starts 1). Reject append when nextId == UINT32_MAX; overwrite oldest when full; never reuse IDs. IDs 0 and UINT32_MAX are invalid.
- `LrTrainerRuntime`: uint8_t active/resources/skill/collector; uint32_t trainingPoints; uint16_t capturesPending/evolutionsPending/reorganizationsPending; uint16_t hostileTo (latest directed hostility, not a full relationship graph); trackedGoalTypes[2] uint8_t, trackedGoalTargets[2] uint16_t, goalProgressMinute[2] uint32_t; historyEventIds[60]/historyPhraseIds[60] uint16_t, historyMinutes[60] uint32_t, historyCount/historyHead uint8_t. Resources/skill/collector 0..100; pending counters saturate at 65535, trainingPoints at 1000000. `hostileTo` starts NONE. Snapshot all of it.
- `LrWorldState`: uint16_t schemaVersion/trainerCount; uint32_t contentVersion/nowMinute; uint64_t rtcHighWater; uint8_t rtcAnchored; LrRng rng; cores[80], exts[80], traits[80], runtime[80], LrChronicle chronicle. Only [0,trainerCount) slots are active storage, sorted ascending unique trainer IDs, never player/none sentinels. Logical active flag controls simulation and Snap eligibility. Initialize unused slots deterministically to zero. `currentRegion` is a core region, NOT a town; Week 3 event place defaults to 0 (unknown town) unless supplied by content. Do not invent town from region.
- Sidecar defaults: active unless RETIRED; resources = traitsRolled ? [poor=20, average=50, rich=80] : 50; skill = min(100,20 + rank*8); collector = archetype EXPERIMENTER ? 80 : 30; training/pending/history zero. Supplied sidecars may override defaults after validation. These are explicit prototype proxies, not species/team stats.
- `LrEventCatalog`: immutable events pointer, uint16_t count (0..270), uint32_t version. Pointers never enter world/snapshots. World contentVersion must match catalog version before any advance. Caller owns catalog lifetime.

---

### Task 1: Canonical chronicle

**Files:** create chronicle files from the map; modify umbrella header.

**Interfaces:** produces `void LrChronicleInit(LrChronicle *log)`; `bool LrChronicleAppend(LrChronicle *log, const LrWorldEvent *event, uint32_t *outId)` (input id ignored); `const LrWorldEvent *LrChronicleFind(const LrChronicle *log, uint32_t id)`; `bool LrChronicleMarkHeard(LrChronicle *log, uint32_t id)`.

- [ ] Step 1: write `test_ring_ids`: append 501 events -> count 500, first ID absent, IDs 2 and 501 present, nextId 502. `test_stale_heard`: marking ID 1 false and ID 501 remains unheard. `test_exhaustion`: nextId UINT32_MAX -> append false, log and outId unchanged. `test_full_width_people`: actor 1000/victim 2000 survive lookup.
- [ ] Step 2: run `make -C living-rival-prototype test`. Expected: RED, missing chronicle API.
- [ ] Step 3: implement contracts with bounded ring traversal, validated count/head, no static state. Reject invalid event kind/region/heard and future ID exhaustion before writing. NULL required pointers return false/NULL; outId may be NULL.
- [ ] Step 4: same command. Expected: GREEN.
- [ ] Step 5: stage only the four task files; commit `feat(living-rival): add canonical chronicle with stable event IDs`.

### Task 2: Owned world and runtime state

**Files:** runtime files, umbrella; consumes Task 1 types/API.

**Interfaces:** `bool LrWorldInit(LrWorldState *out, const TrainerJourneyCore *cores, const TrainerJourneyExtended *exts, const LrTrainerTraits *traits, uint16_t count, uint32_t seed, uint32_t minute, uint32_t contentVersion)`; `bool LrWorldValidate(const LrWorldState *world)`; `int32_t LrFindTrainer(const LrWorldState *world, uint16_t trainerId)` (-1 absent). Init requires sorted inputs rather than silently reordering external arrays. traits may be NULL -> zero/unrolled per trainer. Counts 0..80 legal; count>0 requires core/ext pointers. No reroll of existing trainers.

- [ ] Step 1: write `test_defaults`: records copied exactly, correct sidecar defaults and initial goal timestamps=minute; seed retained, chronicle initialized. `test_invalid_init`: duplicate/descending/sentinel IDs, count81, bad time/enums/counts -> false and destination unchanged. `test_owned_copy`: changing input arrays after init does not change world. `test_validation`: reject needs101, bad traits enum/ranges, history count61, invalid goal targets/types and future progress/history/event times. Accept valid trainer ID1000 without truncation.
- [ ] Step 2: full make test. Expected: RED, missing runtime API.
- [ ] Step 3: implement checked initialization and validation for every bounded semantic field the new modules read: old arrays/counts, enums, facets, values, memory intensities/emotions/causes/targets, booleans, runtime caps, goals/sentinels, time fields, ring head/count/IDs, unique chronicle IDs and nextId greater than stored IDs. Inactive trainers may retain goals/records. External trainer targets may be absent from roster; forbid player/none only as actual trainer IDs. Schema=1. Input invalidity is rejected, not repaired. Validate caller inputs before assigning out; avoid a full-world automatic stack copy (suitable for ARM11).
- [ ] Step 4: full make test. Expected: GREEN.
- [ ] Step 5: narrow stage; commit `feat(living-rival): own simulation state and persistent runtime inputs`.

### Task 3: Hourly needs and Snap

**Files:** needs files, umbrella; consumes world, chronicle and existing memory API.

**Interfaces:** `void LrTickNeeds(TrainerJourneyCore *core, const TrainerJourneyExtended *ext, uint32_t minute)`; `bool LrTrySnap(LrWorldState *world, uint16_t slot, uint32_t minute)`; `void LrHourlyTick(LrWorldState *world, uint32_t minute)`. These are internal scheduling primitives: validated world, minute at an elapsed boundary, not standalone clock advancement APIs.

- [ ] Step 1: write `test_need_schedule`: zero needs at minute60 -> [5,0,0,0,1], at180 Money+8, at360 Company+5, at720 Faith+10; Magma/Aqua Violence+3; all clamp100. `test_stress`: need69 rising to74 adds2, bad long-term memory adds1, joy does not, stress max100; memoryCount255 never reads beyond12 in standalone need function. `test_snap`: fixed seeds found via local RNG copies distinguish rolls1/2 -> success/fail at stress85; stress84 consumes no draw. `test_victims`: only same non-independent faction/region active OTHER slot eligible; full IDs stored; no victim -> no clash or RNG draw. Chronicle exhausted -> no Snap mutation or RNG consumption.
- [ ] Step 2: full make test. Expected: RED.
- [ ] Step 3: implement needs then stress. For eligible Snap: ensure chronicle append capacity/ID first; one `LrRandomNext%100` chance draw, success -> one `LrRandomNext%eligibleCount` draw (even one victim); ascending slots. Set actor hostileTo=victim; stress=max(0,stress-20); append CLASH with SNAPPED outcome=1 for actor, SHAKEN=2 for victim. Victim fear-of-player/hate, death, retirement, faction territory untouched. No extra memory here. No eligible victim -> zero draws. Only active trainers get hourly work.
- [ ] Step 4: full make test. Expected: GREEN.
- [ ] Step 5: narrow stage; commit `feat(living-rival): simulate needs stress and comrade clashes`.

### Task 4: Preserve affect cause

**Files:** existing affect header/source/test; no trainer layout changes.

**Interfaces:** add `void LrAddHateCaused(TrainerJourneyCore *core, TrainerJourneyExtended *ext, const LrTrainerTraits *traits, int32_t delta, uint16_t nowDay, uint8_t cause)` and equivalent `LrAddFearCaused`. Existing Add APIs delegate with LR_CAUSE_PLAYER. New APIs accept event deltas -100..100; invalid delta/cause is a no-op. Existing caller domain/behavior stays unchanged; do not silently clamp the legacy API to this new domain.

- [ ] Step 1: write `test_world_cause`: a high world-caused delta creates an ANGER memory with WORLD cause and never installs player Avenge. `test_other_cause`: Fear memory cause OTHER; `test_player_compatibility`: existing/new player calls match scalar and memory arrays. `test_negative_cause`: negative fear produces Joy with supplied cause. `test_invalid_event_delta`: ±101 and INT32 extremes leave new-API state unchanged.
- [ ] Step 2: full make test. Expected: RED.
- [ ] Step 3: share existing mutation bodies using private helpers with explicit cause; keep positive multiplication exactly once and post-multiplier memory magnitude. Retain all existing legacy behavior and tests. Do not broaden this into fixing all legacy arithmetic debt.
- [ ] Step 4: full make test. Expected: GREEN, existing affect assertions included.
- [ ] Step 5: narrow stage; commit `feat(living-rival): preserve world and trainer affect causes`.

### Task 5: Typed life-event engine and representative content

**Files:** event files, umbrella; consumes Tasks 1–4. Goal synchronization initially calls the metadata reconciliation contract in Task 6 after that task exists; here effect application updates identity timestamps directly using the same identity matching rule below, not a forward dependency on an undefined helper.

**Interfaces:** categories TRAINING=0, PERSONAL=1, ACTIVITY=2, TEAM=3, LIFE=4. `LrLifeEvent` contains uint16_t id/titleId/phraseId/odds; uint8_t category/pool/kind/place/conditionCount/effectCount; conditions[4], effects[4]. Pool=255 any or an existing faction. Condition `{uint8_t field, op; int32_t value;}`; fields BADGES, PHASE, AMBITION, SOCIAL, RESOURCES, SKILL, COLLECTOR, STRESS; ops GE/LE/EQ. All conditions AND; empty true. Effect `{uint8_t kind, index; int16_t amount;}`; kinds NEED, RESOURCE, TRAINING, CAPTURE_INTENT, EVOLVE_INTENT, REORGANIZE_INTENT, HATE, FEAR, GOAL, REGION, STRESS, BADGE, PHASE. Goal index=type, amount unused; target NONE. All effects world-caused. No string DSL.

`bool LrCatalogValidate(const LrEventCatalog *catalog)`; `bool LrEventFits(const LrWorldState *world, uint16_t slot, const LrLifeEvent *event)`; `const LrLifeEvent *LrPickLifeEvent(LrWorldState *world, uint16_t slot, const LrEventCatalog *catalog, uint32_t minute, uint8_t category)` (category255 any); `bool LrApplyLifeEvent(LrWorldState *world, uint16_t slot, const LrLifeEvent *event, uint32_t minute, uint8_t scaleDays)`; `LrEventCatalog LrPrototypeEventCatalog(void)` returns version1, 12 fixtures.

Picker never advances time; apply requires event minute in caller's current processed interval, catalog already validated, and scaleDays1..30. Scheduler controls time; world validation is for stable API boundaries, not mid-tick. Failed validation/apply leaves world unchanged. Apply rechecks fitting conditions. Preconditions on resources use conditions and are evaluated before effects; positive resource rewards do not bypass eligibility.

**Exact content/balance:** conditions are shown as inequalities; unmentioned pool ANY, kind LIFE, place0. Every row has unique nonzero id/titleId/phraseId equal to row number. Odds20 except row12 odds10.

| ID/category | Conditions | Ordered effects |
|---|---|---|
| 1 training | resources>=10, skill>=20 | resource -2, training +10, violence need -15 |
| 2 training | resources>=30, skill>=40 | resource -5, training +20, stress -5 |
| 3 personal | social>=55 | goal PROTECT, company need -20, stress -5 |
| 4 personal | ambition>=70 | goal RISE, stress -5 |
| 5 activity/BADGE | skill>=50, resources>=20, badges<=7, phase<=BADGE_HUNT | resource -10, badge +1, phase BADGE_HUNT |
| 6 activity | collector>=60, resources>=10 | resource -5, region JOHTO, company need -10 |
| 7 team | collector>=50, resources>=15 | resource -5, capture intent +1 |
| 8 team | skill>=60, resources>=5 | evolve intent +1, resource -5 |
| 9 team | resources>=10 | reorganize intent +1, resource -2 |
| 10 life | stress>=70 | goal SETTLE, stress -20, drink need -25 |
| 11 life | resources<=30 | resource +5, stress -10, money need -20 |
| 12 life/RETIREMENT | phase<=MENTOR, stress>=90 | phase RETIRED, stress -30 |

PHASE effect RETIRED sets runtime.active=0; it is permanent in this fixture subset, not a full temporary-retirement system. Explicitly document richer life events and relationships as future content. GOAL PROTECT with NONE is an aspiration, not an actual assigned student. No fabricated friendships or evolved species.

- [ ] Step 1: write named tests `test_conditions_and_pools`, `test_recent_and_phrase`, `test_fallback`, `test_zero_weights`, `test_effects_and_causes`, `test_pending_not_party`, `test_catalog_validation`. Assert recent same ID within <4320min suppressed, same phrase in last30 entries suppressed regardless age; fallback ignores only suppression, never fit/positive odds/category. Full-width history survives day256. Apply writes one chronicle event and one history entry; failed append changes neither effects nor RNG. Invalid opcode/condition/index, duplicate IDs, >270 rows, and effect delta>100 rejected.
- [ ] Step 2: full make test. Expected: RED.
- [ ] Step 3: implement picker and catalog. Sum odds into uint32_t; combine TWO LCG outputs `(first<<16)|second`, modulo total, exactly two draws if total>0; zero candidates consumes none. Expose pure `uint16_t LrWeightedIndex(const uint16_t *weights, uint16_t count, uint32_t roll)` returning count if total0; roll reduced modulo total, count<=270. Test weights[65535,65535,1], roll131070 selects index2; do not use existing 16-bit LrRandomRange for large totals. Catalog order is tie order.
- [ ] Step 4: implement effects and append/history transaction. Only TRAINING and positive RESOURCE amounts scale by scaleDays; resource costs, goals, badges, needs and team intents happen once per beat. Clamp supported fields; training cap1000000, pending65535. All regular daily events scale1. Keep latest60 history records; last30 based on insertion order, not wrapping legacy timestamps. Sync goal timestamps by identity(type,target), preserve retained identity, initialize new identity to event minute. No partial application on unsupported effects. Add tests for scale30, saturation and retired activity status.
- [ ] Step 5: full make test. Expected: GREEN.
- [ ] Step 6: narrow stage; commit `feat(living-rival): add typed life events and prototype catalog`.

### Task 6: Memory aging, tracked goals and daily ordering

**Files:** daily files, umbrella; update event source to use the shared reconciliation helper (removing its local version); consume Tasks 2,4,5.

**Interfaces:** `void LrAgeTrainer(LrWorldState *world, uint16_t slot, uint32_t elapsedDays, uint32_t minute)`; `void LrSyncGoals(LrWorldState *world, uint16_t slot, uint32_t minute)`; `bool LrSetTrackedGoal(LrWorldState *world, uint16_t slot, uint8_t type, uint16_t target, uint32_t minute)`; `bool LrRecordGoalProgress(LrWorldState *world, uint16_t slot, uint8_t type, uint16_t target, uint32_t minute)`; `void LrDailyTick(LrWorldState *world, const LrEventCatalog *catalog, uint32_t minute)`.

- [ ] Step 1: write `test_memory_aging`: short-term10 removed after1d; long-term31->30 and stays30; every parallel array follows retained record, trailing slots zero. elapsedDays0 does not age memories/scalars but still evaluates goal expiry at minute. `test_goal_identity`: reorder Avenge/Rich preserves their original timestamps, new goal starts now, explicit progress resets only matching identity. `test_expiry`: at30d stays, at30d+1min expires, survivor compacts with matching timestamp, empty target NONE. `test_daily_order`: memory fade/expiry/fear-1/notoriety-2 occur before event eligibility/effects; bad memory removed no longer contributes stress next hour. Event-created goal timestamp is current boundary.
- [ ] Step 2: full make test. Expected: RED.
- [ ] Step 3: implement saturating bulk aging with widened products; long-term floor30, short-term -10/day, fear -1/day, notoriety -2/day, stable compaction. Expire goals where minute-lastProgress>43200. Reconcile metadata by identity, not slot; external direct goal edits must explicitly sync before advancing or fail world validation. Tracked Set reuses LrSetGoal, preserving existing exclusions; reinstall same identity does not imply progress. Reject wrapper slot/type/target or future minute inputs before mutation. Successful BADGE effects record progress for existing RISE, positive RESOURCE effects for existing RICH; other fixture effects do not imply progress. Test both matching updates and an unrelated goal remaining unchanged. Daily active trainers: Age1, chance draw `%100<60`, pick category255, apply scale1. Inactive trainers still age/expire but get no life-event chance. No faction/world mutation; documented reserved phases run after trainer work with no draws.
- [ ] Step 4: full make test. Expected: GREEN.
- [ ] Step 5: narrow stage; commit `feat(living-rival): age memories and track goal progress deterministically`.

### Task 7: Meaningful bounded absence summaries

**Files:** absence files, umbrella; consumes Tasks 2,5,6.

**Interfaces:** `LrAbsenceStats {uint32_t beats, appliedEvents, agedDays;}`; `void LrSummarizeAbsence(LrWorldState *world, const LrEventCatalog *catalog, uint32_t fromMinute, uint32_t toMinute, LrAbsenceStats *out)` (validated interval and inputs, no direct public clock advance). `uint16_t LrCategoryWeight(const LrWorldState *world, uint16_t slot, uint8_t category)` pure query.

**Balance and ordering:** duration>0 -> K=min(7,ceil(duration/1440)), otherwise0. Beat j=1..K at `from + floor(duration*j/K)` using uint64 intermediate. At each beat, age ALL trainers by crossed calendar days since previous beat before active trainer opportunities; one chance draw%100<60 per active slot. At most K*trainerCount chances. Category weights: training=10+Ambition+Discipline+skill/2; personal=10+Social+Loyalty/2; activity=10+Ambition/2+collector; team=10+collector+resources/2; life=10+stress+(100-resources)/2. Zero category weight if no fitting positive-odds fixture, even after suppression fallback. Exactly two draws for weighted category if any exists, then exactly two for event picker. No category/checklist guarantees.

Before each opportunity, recovery is applied to active trainers as routine offscreen self-care: each need decreases by min(100, crossedDays*10), stress by min(100,crossedDays*5), resources costs not simulated hourly. Recovery occurs after aging, before chance/category selection. This is explicitly an approximation, not hourly equivalence. ScaleDays=clamp(ceil(gapMinutes/1440),1,30) for selected event. On zero crossed days no routine recovery. Do not drain resources for unselected activities. No player level influences weights or effects.

- [ ] Step 1: write `test_summary_bounds`: six-month interval -> at most7 beats/trainer and <=7 events, no hourly iteration, deterministic same state/request. `test_duration`: 1d/6d/180d ->1/6/7 beats; training fixture scale1 vs30 leads10 vs300 points for same successful beat. `test_personalized_weights`: competitive Ambition90/Discipline90/skill80 weight230; collector80/low ambition activity weight>competitive collector0 equivalent. `test_impossible_categories`: resources0 and filtered catalog -> team zero, no captures; no fitting categories -> chance only, no picker draws. `test_not_checklist`: one-category catalog creates no other categories. `test_bulk_aging`: many months remove short memories, long-term floor30, goals expire, no unsigned underflow. `test_no_player_ceiling`: world with player absent still can reach badges8 if eligible. `test_summary_timestamp_order`: multiple slots/retirement sorted minute then ID, retired gets no later opportunities.
- [ ] Step 2: full make test. Expected: RED.
- [ ] Step 3: implement capped beat scheduling and exact formulas; return counters, never store generated Pokémon/levels. Reuse LrAgeTrainer and typed effects; no duplicate hidden event log. Record that elapsed-time variation affects opportunity frequency and magnitude but is not complete lifetime modeling. Avoid an unbounded loop over days or minutes.
- [ ] Step 4: full make test. Expected: GREEN.
- [ ] Step 5: narrow stage; commit `feat(living-rival): summarize plausible rival lives during long absences`.

### Task 8: Chronological calendar and offline advancement

**Files:** simulation files, umbrella; consumes Tasks 2,3,6,7.

**Interfaces:** `LrAdvanceStats {uint32_t fromMinute,toMinute,summaryEndMinute,hourlyTicks,dailyTicks,summaryBeats,appliedSummaryEvents;}`; `bool LrSimulateTo(LrWorldState *world, const LrEventCatalog *catalog, uint32_t targetMinute, LrAdvanceStats *out)`; `bool LrResumeOffline(LrWorldState *world, const LrEventCatalog *catalog, uint64_t rtcMinute, LrAdvanceStats *out)`. Required out, world, catalog pointers; invalid input false and all outputs unchanged. Same time true zero work; backward game requests false. Invalid catalog/version/world must fail before mutation.

**Algorithm:** If target-now>2880, summarize [now,target-2880] first; then detail (summaryEnd,target]. Otherwise detail (now,target]. Hour boundary=floor(start/60)*60+60; iterate <=target; set world.nowMinute to boundary before tick. Hour then daily when boundary%1440==0. Finally world.nowMinute=target. Summary processing also sets world.nowMinute to each beat timestamp before applying effects; never leave the world clock in a future range while processing older events. World stable-state validation is applied at outer API boundaries, not halfway through multi-trainer work. At most48 hourly and2 daily ticks. Summary interval's day aging plus detailed boundary aging never overlap.

Offline: if unanchored, anchor sample, zero time advancement. Otherwise elapsed=max(0,rtcMinute-highWater), target=now+elapsed checked in uint64. Only after successful advancement update highWater=max(old,sample). Active game-minute advancement never changes rtcHighWater; platform must call Resume only once per closed interval and reanchor on close, via `bool LrAnchorOfflineClock(LrWorldState *world, uint64_t rtcMinute)` setting rtcAnchored and max prior/sample without advancing. Preserve high-water across rollback. The adapter owns close/open transitions; these pure APIs do not pretend to detect an open game or double invocation errors.

- [ ] Step 1: write `test_partial_minutes`: init0, advance59 ->0 ticks;60 ->1;61 ->0;120 ->1. `test_midnight_order`: needs at1440 contribute stress before daily fade/life effects. `test_incremental_equivalence`: single2880 vs every minute to2880 canonical fields and RNG equal (stats excluded). `test_caps`: jump180d ->48 hourly,2 daily, summary<=7*80; target reached. `test_summary_split_contract`: identical long request replay matches; different partitions are permitted to differ, never assert inequality is mandatory. `test_noop_backward_limits`: now same no RNG, backwards/94371840 false untouched. `test_versions`: wrong catalog false untouched.
- [ ] Step 2: full make test. Expected: RED.
- [ ] Step 3: implement scheduler and offline API with preflight validation. Treat exhausted chronicle as nonfatal loss of new events: effects requiring a chronicle entry fail atomically, time/aging still advance; Snap checks before drawing. Expose full chronology even if log overwrites. State-report counters reflect actual applied events.
- [ ] Step 4: add `test_offline_clock`: anchor1000, resume1060 ->+60; repeated1060 ->0; rollback900 then1000 then1060 ->0;1120 ->+60. Active play+120 then close anchor1240/resume1300 ->+60 not+180. `test_offline_equivalence`: freshly cloned anchored state with60 elapsed offline equals direct+60 except clock anchor metadata. UINT64_MAX sample overflows supported game range ->false without changes. Run full make test. Expected: GREEN.
- [ ] Step 5: narrow stage; commit `feat(living-rival): advance calendar online and offline with bounded work`.

### Task 9: Full-state snapshots and persistence contract

**Files:** snapshot files, umbrella; update SAVE_SERIALIZATION_DESIGN.md.

**Interfaces:** `size_t LrSnapshotBytes(const LrWorldState *world)` (0 invalid); `bool LrSnapshotWrite(const LrWorldState *world, uint8_t *buffer, size_t capacity, size_t *written)`; `bool LrSnapshotRestore(LrWorldState *world, const uint8_t *buffer, size_t length, uint32_t contentVersion)`.

**Encoding:** 20-byte LE header: magic bytes LRS1, u16 schema1, u16 trainerCount, u32 contentVersion, u32 payloadLength, u32 CRC32(payload). Payload encodes world clock/RTC fields/RNG, each used trainer core+ext (existing packed LE bytes), traits fieldwise (no padding), every runtime field, chronicle metadata and all500 event slots fieldwise (including unused zero slots). Field order follows declarations in Task2; no implicit padding, pointers, or callback state. Use the existing LrCrc32. Explicitly list every field width/order in the save design addendum. Assert checked exact sizes; write size tests from independently summed widths, not merely Bytes returning itself.

- [ ] Step 1: write `test_complete_roundtrip`: populated history, pending counters, goal progress, traits, RTC rollback high-water, ring wrap and heard -> byte-identical write/restore/write; continue both copies using same request ->identical snapshot. `test_rejected_restore`: truncation, extra bytes, magic/schema/content/CRC, and valid-CRC corruption of final event heard byte ->false and entire destination bytewise unchanged. `test_rejected_write`: short capacity and NULL ->false, buffer/written unchanged. `test_old_blob`: existing test_save still v1/320 bytes per trainer.
- [ ] Step 2: full make test. Expected: RED.
- [ ] Step 3: implement two-pass parsing: first bounds/CRC and all field semantics, then decode into destination only after guaranteed validity. Avoid full-world stack temporaries; fixed per-record scratch is allowed. Canonicalize unused slots as zero when writing; restore must reject noncanonical inactive tail data rather than allowing hidden inputs. SnapshotRestore need not preserve native padding from source, but zeroes destination padding deterministically. Document buffer must not overlap source/destination world; reject overlap using portable checked uintptr_t ranges. Commit only new snapshot contract, not a false claim of platform persistence or dual-slot recovery.
- [ ] Step 4: full make test. Expected: GREEN.
- [ ] Step 5: narrow stage; commit `feat(living-rival): snapshot complete replay state without host padding`.

### Task 10: Gazette projection, text and integrated demonstration

**Files:** gazette files, umbrella, README.md, GAZETTE_UI_PROTOTYPE.md; consumes canonical chronicle and complete scheduler/snapshot APIs.

**Interfaces:** `LrGazetteFilter {uint8_t region,place; uint16_t trainerId;}` (255 region/place and NONE trainer wildcard); `uint8_t LrGazetteQuery(const LrWorldState *world, const LrGazetteFilter *filter, uint8_t maxCount, LrWorldEvent *out)`; `LrTextLookup` typedef `const char *(*)(void *context,uint16_t id)`; `bool LrGazetteFormat(const LrWorldEvent *event, LrTextLookup title, LrTextLookup trainer, LrTextLookup place, void *context, char *out, size_t capacity)`. Text callbacks externally owned, never snapshotted.

**Rules:** future events excluded, age<=7200min inclusive, unheard only. Select min(maxCount,5) by score `kindWeight*(6-age/1440)+fameBonus`; weights death100,badge60,retirement50,clash30,life10. fameBonus=max known participant fameLevel/16 (0..15); absent participants zero. Rank score desc, timestamp desc, ID asc. Group selected records place asc, keeping within-place ranked order. Query does not mark heard or draw RNG. Mark heard only via canonical API. out must fit min(maxCount,5), NULL/invalid filter ->0 no writes.

Formatting uses title templates with only `{trainer}` and `{place}` replacement (first participant; place lookup receives uint16 place value), missing lookups -> `Unknown trainer`/`Unknown place`/`An event occurred.`. No printf-format evaluation, escape language or template recursion. Truncated output returns false but is NUL-terminated if capacity>0; cap0 does not write. The enum/text namespace is core-only, not pretending port GazetteEntry already agrees.

- [ ] Step 1: write `test_filters_age_score_group`: >5 events, death ranks above badge above life when equally recent, exact5d boundary, region/town/trainerID1000 filters, deterministic ties and grouping after selection. `test_heard_and_overwrite`: stale ID cannot mark replacement, query no mutation, valid heard removed. `test_format_bounds`: capacities0/1/exact/short, missing names/title, literal percent signs, unknown tokens copied literally, no recursion.
- [ ] Step 2: full make test. Expected: RED.
- [ ] Step 3: implement top-five bounded selection without copying/sorting500 events. Scan ring and keep five ranked candidates; lookup fame by actual ID. Document one shared store, field encoding differences vs port, encounter policy outside core, and Week4 ownership extension.
- [ ] Step 4: add `test_thirty_day_demo`: initialize10 distinct trainers, simulate30 daily advances (each detailed), query <=5; fixed fixture text/name callbacks print a readable sample. Snapshot/restore day15 and reproduce day30. Also demonstrate six-month offline advance of competitive and collector profiles; assert reproducibility and plausible eligibility/pending intents, not a fragile demand that every seed grants a particular category. Demonstration must label pending teams and representative content honestly. Update README with sample host command, daily vs absence determinism contract, no rubber-banding/story-loss policy for future integration.
- [ ] Step 5: run `make -C living-rival-prototype clean`, then `make -C living-rival-prototype test`. Expected: GREEN, both exit0; all eight original and all new test binaries PASS, sample Gazette text printed. Capture full output and exit status. Run snapshot and sim binaries again to verify repeatable output. Record assertion counts from actual results, not estimates. No 3DS/emulator/fuzz performance claim.
- [ ] Step 6: narrow stage; commit `feat(living-rival): project shared chronicle into trainer Gazette`.

## Step 1 assertion anchors

These fragments go inside the owning test's main or named static test function, with the setup stated here; all other cases retain the exact values specified in each task. Small fixtures can be automatic variables; world-sized fixtures must use test-only static storage to avoid consuming the ARM11 stack.

Task 1, after appending 501 valid events into initialized `log`:
```c
LR_CHECK_EQ_INT(log.count, 500);
LR_CHECK(LrChronicleFind(&log, 1) == NULL);
LR_CHECK(LrChronicleFind(&log, 501) != NULL);
LR_CHECK(!LrChronicleMarkHeard(&log, 1));
LR_CHECK_EQ_INT(LrChronicleFind(&log, 501)->heard, 0);
```

Task 2, before initialization, with destination `world` filled with 0xAA and its bytes copied into `before`:
```c
LR_CHECK(!LrWorldInit(&world, NULL, NULL, NULL, 81, 1, 0, 1));
LR_CHECK(memcmp(&world, &before, sizeof(world)) == 0);
```

Task 3, zeroed core/ext, Independent faction, stress0 and no memories:
```c
LrTickNeeds(&core, &ext, 60);
LR_CHECK_EQ_INT(core.needs[LR_NEED_DRINK], 5);
LR_CHECK_EQ_INT(core.needs[LR_NEED_COMPANY], 0);
LR_CHECK_EQ_INT(core.needs[LR_NEED_VIOLENCE], 1);
```

Task 4, rolled Steady core with Anger60, no goals and empty ext:
```c
LrAddHateCaused(&core, &ext, NULL, 100, 1, LR_CAUSE_WORLD);
LR_CHECK_EQ_INT(ext.memoryCauses[0], LR_CAUSE_WORLD);
LR_CHECK(!LrHasGoalAgainstPlayer(&core, LR_GOAL_AVENGE));
```

Task 5:
```c
const uint16_t weights[3] = {65535, 65535, 1};
LR_CHECK_EQ_INT(LrWeightedIndex(weights, 3, 131070), 2);
LR_CHECK_EQ_INT(LrPrototypeEventCatalog().count, 12);
```

Task 6, initialized one-trainer world with a tracked RICH goal installed at minute0, no progress; set world.nowMinute before calling the internal aging primitive:
```c
world.nowMinute = 43200;
LrAgeTrainer(&world, 0, 30, 43200);
LR_CHECK(LrHasGoal(&world.cores[0], LR_GOAL_RICH));
world.nowMinute = 43201;
LrAgeTrainer(&world, 0, 0, 43201);
LR_CHECK(!LrHasGoal(&world.cores[0], LR_GOAL_RICH));
```

Task 7, one active trainer, empty valid catalog, world initialized at0:
```c
LrAbsenceStats stats;
LrSummarizeAbsence(&world, &catalog, 0, 180u * 1440u, &stats);
LR_CHECK_EQ_INT(stats.beats, 7);
LR_CHECK_EQ_INT(stats.appliedEvents, 0);
```
Here `beats` counts active trainer opportunities before the chance draw, not only successful events; `agedDays` counts world calendar days aged, not trainer-days.

Task 8, fresh world at0 and matching catalog:
```c
LrAdvanceStats stats;
LR_CHECK(LrSimulateTo(&world, &catalog, 59, &stats));
LR_CHECK_EQ_INT(stats.hourlyTicks, 0);
LR_CHECK(LrSimulateTo(&world, &catalog, 60, &stats));
LR_CHECK_EQ_INT(stats.hourlyTicks, 1);
LR_CHECK(LrSimulateTo(&world, &catalog, 61, &stats));
LR_CHECK_EQ_INT(stats.hourlyTicks, 0);
```

Task 9, valid snapshot `bytes` of `length`, matching catalog; corrupt a final heard byte to2, then recompute CRC in header LE using the existing CRC function before invoking the decoder (so semantic validation, not CRC, must reject it):
```c
LR_CHECK(!LrSnapshotRestore(&destination, bytes, length, catalog.version));
LR_CHECK(memcmp(&destination, &before, sizeof(destination)) == 0);
```

Task 10, world with six equally recent unheard matching events, valid wildcard filter, five-record output:
```c
LR_CHECK_EQ_INT(LrGazetteQuery(&world, &filter, 255, entries), 5);
LR_CHECK(LrChronicleMarkHeard(&world.chronicle, entries[0].id));
LR_CHECK_EQ_INT(LrChronicleFind(&world.chronicle, entries[0].id)->heard, 1);
```

## Self-review record and execution handoff

- Spec coverage: needs/Snap3; typed events/fixture limits4–5; goals/memory6; personalized summary7; detailed/offline scheduler8; full authoritative state2/9; Gazette10. Encounter/loss handling and real party generation remain explicit integration/Week4 non-goals.
- Type/dependency scan: events use runtime/chronicle/affect; daily then adopts shared metadata helper; absence consumes daily/events; scheduler consumes absence/needs/daily; snapshots consume complete world types; Gazette consumes chronicle and does not own an event store. No call into future faction code or platform clock API.
- Review Focus has a named covering test for all five cases. Supported time range is explicit rather than hiding uint16 day wrap. New content/version IDs never reinterpret old port values.
- Balance formulas, clock ratio and capped approximation are prototype decisions. Do not report that seven developments constitute a full six-month Pokémon training simulation.
- This file and the approved spec are the only planning edits. No runtime implementation yet; no existing uncommitted files staged or overwritten.

After user plan review, execute inline with executing-plans, tests first. Before completion run verification-before-completion, create the review package and obtain one fresh whole-change review under requesting-code-review. Fix Important/Critical findings RED→GREEN and run the full suite; ledger minor deferrals and rulings. Follow finishing-a-development-branch; do not interpret approval of this plan as merge/push/deploy permission.
