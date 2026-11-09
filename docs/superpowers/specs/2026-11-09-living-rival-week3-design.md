# Living Rivals — Week 3 design

Status: approved by the user in conversation on November 9, 2026.
Implementation awaits review of the Week 3 implementation plan.

## Intent and authority

Continue the existing portable C99 prototype, not Weeks 1–2 again. Nintendo
3DS is the runtime target, with possible PC/mobile ports. No GBA runtime,
SaveBlock integration, or obsolete GBA memory/cycle constraints apply.

Authority: the November 9 Weeks 1–2 handoff, existing core, and the user
decisions below supersede conflicting sketches in the original documents.
Preserve existing uncommitted changes; implementation belongs in an isolated
feature worktree. Do not merge, push, publish, or deploy without permission.

## User decisions

- Elapsed time can produce training, personal development, independent
  activities, team development, and life events.
- These are possibilities, not five guaranteed rewards. Personality,
  resources, skill, and circumstances determine plausible developments.
- An ambitious competitor may train extensively; a casual collector may
  explore Johto and collect Pokémon instead.
- Separate actual world accomplishments, actual battle-team strength, and
  encounter accessibility. The player's progression is not an absolute
  simulation ceiling. No hidden rubber-banding or weakening real teams.
- Mandatory rival battles acknowledge genuine strength differences. Losing
  may advance the story, with the defeat acknowledged.
- Both in-game calendar advancement and elapsed time while the game is
  closed advance rival lives.

## Scope and non-goals

Week 3 delivers needs/stress/Snap, a life-event engine with representative
content, a deterministic scheduler and absence-summary path, snapshots, and
a Gazette data/text prototype. Long absences are lived time, not erased time.
This extends the old cap-and-discard interpretation of catch-up.

Full party generation, species/move legality, factions, territory, and the
content compiler remain Week 4. Week 3 records typed pending team-development
intents, not invented battle stats or a claim that catching/evolution already
works. Week 4 must resolve these into authentic teams before game integration.
No story scenes, encounter access implementation, rendering, platform clock
reads, file I/O, or connection to the 38 specials stubs in this work.
The event engine is not delivery of all 270 authored events or the full DSL.

## Proposed design calls (assistant recommendations, not user decisions)

### Time ownership

Use a caller-owned simulation/world container for the absolute simulation
minute, next tick boundaries, one simulation RNG, trainer runtime sidecars,
stable traits, and one chronicle. No hidden mutable globals. The platform
adapter supplies elapsed offline time; the portable core never reads an RTC.

Use one elapsed minute per real offline minute initially. Convert offline
time once on load and add it to the saved game calendar; do not also count
that interval as active play. Resume with a newly anchored clock sample.
Persist a wall-clock high-water mark in the future surrounding save container:
backward RTC movement contributes zero until the clock exceeds that mark.
Forward clock changes count as elapsed time, with bounded computational cost;
this is not tamper-proof clock enforcement. Platform clock plumbing remains
Phase 3. Use checked arithmetic and explicitly reject unsupported time ranges.

Snapshot all authoritative simulation state, including RNG, traits, runtime
metadata, journal/heard state, and pending development. Callbacks, text lookup
functions, and immutable content tables are external, identified by a content
version. Restore validates before modifying the destination and rejects a
content-version mismatch. Snapshot records are not raw pointers or host padding.
Keep the existing v1 trainer blob and 87/233-byte trainer layouts unchanged.
A new world-envelope persistence contract documents the added state; do not
claim platform save/load or a new disk codec is implemented.

### Detailed simulation and bounded absence

For jumps within 48 hours, execute every elapsed hourly/daily boundary in
chronological order; at midnight hourly work precedes daily work. No tick
occurs at initialization, and partial hours survive between calls.

For larger jumps, summarize the older interval, then replay the final 48 hours
in detail. Summary work has a fixed maximum number of meaningful development
beats per trainer (proposed maximum seven, not seven guaranteed events).
Allocate beats across the elapsed interval, in timestamp order. Each beat
selects from plausible category/event candidates and applies bounded outcomes.
Longer duration affects opportunity and magnitude, not unbounded iteration.
Summaries account for need satisfaction, recovery and memory/affect aging;
never multiply unattended hourly stress across six months or grant every
trainer automatic badges, levels, or retirement.

The implementation plan must pin the integer duration bands, development
weights, eligibility requirements, recovery rules, and progression bounds in
reviewable C tables. Those are builder choices, not implicit user-approved
balance. They must depend on persistent simulated inputs, not player level.
Representative fixtures must demonstrate distinct competitive/collector arcs,
resource limitations, setbacks, and a category with no eligible events.

Detailed hourly ticks are capped at 48 per jump. Detailed daily ticks occur
inside that window (at most two); the former seven-daily-tick allowance becomes
at most seven summary beats per trainer, not seven extra daily passes that
double-count the same interval. This intentionally replaces the old sketch.
Each call reports its detailed and summarized ranges and work counts.

Exact equivalence between one call and incremental calls is required when
neither summarizes. Summary trajectories can differ from hourly replay or
different jump partitions. Deterministic replay requires identical initial
state, content version, and ordered advancement/input requests. No claim that
six months summarized equals six months of hourly simulation.

### Deterministic ordering and missing state

Stable trainer slots ordered by trainer ID, with unique IDs validated at
initialization; maximum 80. Each boundary processes trainers in that order.
Hourly order: needs, stress, Snap. Daily order: memory fade/compaction,
goal expiry, fear decay, notoriety decay, event chance/picker/effects, then
reserved faction/world phases. Reserved phases have no Week 4 behavior yet.

One persisted LCG stream; document exact conditional draws for Snap, victim
selection, daily chance, picker, and summary opportunities in the plan. Queries
and formatting consume no RNG. Stable candidate/content order breaks ties.
Summary beats use timestamp then trainer ID order. This ordering also fixes
which earlier effects later participants observe.

Sidecars own goal identity/target and last-progress minute, full-width history
event IDs/keyphrase IDs/timestamps, resource/skill proxies, training progress,
and pending team-development intents. Define explicit progression inputs and
default profiles; do not infer missing traits from current facets on restore.
All simulation-owned goal mutations synchronize metadata; external mutations
must use a tracked wrapper or explicit resynchronization before advancement.
Expire goals after more than 30 days without recorded progress, preserving
existing asymmetric exclusions. Do not use wrapping u8 history timestamps for
long-absence suppression. Version and snapshot all added state.

### Needs and Snap

Prefer prototype test authority: Drink +5/hour, Company +5/6h, Faith +10/12h,
Money +8/3h, Violence +3/hour for Magma/Aqua and +1 otherwise, clamped 0–100.
Spirit scaling is deferred because no formula is agreed. Needs at least 70
add two stress each; each bad long-term memory adds one, bounded by 12 memories.

At stress at least 85, one chance draw per eligible trainer-hour, 2 in 100.
Choose uniformly from other active same-faction trainers in the same region;
Independent is not a shared comrade faction. No eligible victim means no
victim draw, no clash, and no invented faction effects. Record a clash with
actor/victim full trainer IDs and timestamp, and a directed hostility record.
No automatic death, retirement, territory change, or player-directed hate.
Detailed consequences and the summary recovery/reset rule are plan-level
balance choices to pin explicitly, with tests.

### Life events and content

Use fixed-capacity typed conditions/effects and const C tables, not a runtime
string parser. Support representative stat/phase/faction/goal predicates,
pool filtering, weighted selection, recent-event and last-30-keyphrase
suppression, then fallback to all fitting positive-weight events. No fitting
event returns none without invalid writes. Use sufficiently wide totals and
a weighted draw capable of reaching every supported candidate.

Effects cover clamped needs/resources/affect, memories, goals/progress,
training progress, locations, and pending team changes. Positive affect
effects call Add directly, never Calculate followed by Add. Their memories
must preserve world/other-trainer causation rather than the existing
player-only affect wrapper's semantics; add an explicit cause-aware route
without changing existing caller behavior. Unsupported effects fail validation
instead of silently pretending to evolve or capture a Pokémon.

The plan pins the supported vocabulary and fixture catalog. Category weighting
uses personality, resources, skills, and circumstance inputs; it is neither
uniform random rewards nor a five-category checklist.

### Chronicle and Gazette

Create the minimal reusable chronicle now; Week 4 extends this same store.
One fixed-capacity ring (proposed 500 entries) with monotonic stable IDs,
absolute timestamps, region/place, kind, full-width participant IDs, text IDs,
outcomes, and heard flag. Define overflow/ID exhaustion behavior and test it.
Gazette entries are projections, not a second authoritative event history.
Trainer history is suppression metadata referring to content events.

Query at most five unheard entries in the last five days, with region/town
and trainer filters. Select by explicit integer newsworthiness, then group
selected entries by town with stable timestamp/ID tie-breaks. Marking heard
mutates only the canonical event. Format text into caller-provided bounded
buffers with supplied name/place/text lookups; no allocation or rendering.
The design document for the Gazette explains future story/encounter consumers
without implementing them. Gazette display does not secretly adjust teams.

## Verification and delivery

- Needs: all schedules, saturation, memory count bounds, stress 84/85,
  deterministic Snap chance/victim eligibility, no-victim behavior.
- Events: filtering, zero weights, fallback, suppression, weighted boundaries,
  cause-aware affect, bounded effects and pending development.
- Simulation: boundary ordering, partial minutes, no-op/backward requests,
  caps, long durations, both elapsed-time sources, work bounds, repeatability,
  detailed partition equivalence and explicitly non-equivalent summary policy.
- Snapshot: full-state round-trip, resume determinism, malformed/version
  rejection without partial mutation. Include heard flags and pending intents.
- Gazette: 10 trainers over 30 days, five-entry bound, age/filter/sort/group,
  ring overflow, heard marking, bounded text and representative readable output.
- Run existing eight host binaries unchanged alongside new tests. No claim of
  full 3DS verification, full content, authentic generated parties, or full
  1000-by-365 fuzz coverage from Week 3 host results.

## Approval and next step

Obtain user approval of this full design before writing the Week 3
implementation plan. Then use writing-plans, review that plan, and execute
inline with tests first and a fresh whole-change review. Existing user work
must remain untouched throughout; local merging still requires permission.
