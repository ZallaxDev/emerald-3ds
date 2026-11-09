# Week 3 verification and execution record

Date: 2026-11-09. Scope: portable host prototype only, Nintendo3DS intended
runtime. Base d82f425; approved plan/spec committed as981824a. Work performed
inline in an isolated native worktree, without touching existing main-checkout
changes. No merge, push, release, emulator deployment or platform save I/O.

## Task commits

| Task | Commit | Delivered |
|---|---|---|
| 1 | 7e6ed74 | Canonical chronicle |
| 2 | 18dfd03 | World/runtime ownership and validation |
| 3 | 700a35c | Hourly needs/stress/Snap |
| 4 | 766c935 | Cause-aware affect |
| 5 | 35e88ec | Typed picker/effects and12 representative events |
| 6 | 0c879e5 | Aging and tracked goals |
| 7 | f9359ed | Bounded absence summaries |
| 8 | 13134fb | Detailed/offline calendar advancement |
| 9 | d78aeea | Complete-state snapshots |
| 10 | 610ca62 | Gazette query/text and demonstrations |

Each task had a failing new-API test run before implementation, a green
full-suite run, and a post-commit task-done full-suite run recorded by the
executing-plans tooling. Tasks used the same shared-state interfaces from the
plan; Task5's local goal reconciliation was replaced by Task6's shared helper.
Test-only valid-world fixtures are shared; production dependencies are not mocked.

## Verification

MSYS2 GCC16.2, `-std=c99 -O2 -Wall -Wextra -Werror`; standard build command:
`make -C living-rival-prototype test`. Task10 also performed a clean rebuild;
simulation and snapshot executables were run twice with identical output.
The final suite, including the post-review retirement-gate fix, reran all17
binaries successfully: **54,716 assertions**.

| Binary | Final assertions PASS |
|---|---:|
| absence | 124 |
| affect | 71 |
| chronicle | 1,050 |
| daily | 47 |
| gazette | 619 |
| goals | 66 |
| life_events | 55 |
| memory | 102 |
| mind | 45,482 |
| needs | 40 |
| rng | 1,110 |
| runtime | 45 |
| save | 1,698 |
| simulation | 2,950 |
| snapshot | 553 |
| tables | 694 |
| types | 10 |

The Gazette demonstration reproduces a ten-trainer30-day state after a day15
snapshot. It prints a readable sample and a two-profile six-month offline
summary. It is not proof of lifetime narrative balance; random outcomes can
give a collector more training than a competitor on an individual seed.

## Fresh review and fix

A fresh read-only reviewer examined d82f425..610ca62 and independently rebuilt
all17 binaries in approved temporary storage (54,677 assertions before the fix).
All five named Review Focus cases were verified. The reviewer also exercised
the actual large-weight picker reaching candidate3 with seed73291.

## Post-review behavior test

A throwaway host harness (kept outside the branch) simulated ten varied
profiles for365 days, printed journeys and Gazette news, round-tripped a
snapshot, and performed a six-month offline jump. This found a content gate
defect: every rookie retired within30 days because fixture12 allowed retirement
from any phase at stress>=90. The phase model says retirement follows the
mentor phase; stress should not make a rookie retire immediately. A regression
test now proves a ROOKIE at stress100 cannot retire while a MENTOR can, and the
suite passes. The same harness then showed all10 trainers still active after a
year, with badges spread0–8,14 active goals, and a500-entry chronicle mixing
381 life and118 clash events. Its temporary buffers reused a participant name;
that was a harness display bug, not core state.

This test also exposes a model limitation, not silently fixed here: detailed
hourly simulation raises needs but has no routine satisfaction, so long-term
needs/stress saturate and Snap clashes crowd the Gazette. Absence summaries do
recover needs/stress. Defining offscreen detailed-time satisfaction is a future
design decision (player actions, resources, activity), not authority granted
by Week3's approved plan.


One Important issue: Chronicle append accepted out-of-range faction IDs, while
world/snapshot validation rejected them. Regression tests reproduced both the
insertion and resulting blocked advancement/persistence. The fix rejects bad
faction values in all four positions before any ID/log/output mutation and
accepts sentinel255. Both tests went RED→GREEN and the complete suite passed.
No second reviewer was dispatched; the fix was verified by regression tests.

## Rulings made, in decision order

1. Internal validators use a private header, with Makefile dependencies;
   initialization rejects overlapping world/input arrays rather than destroying
   source data. Cost if wrong: move declarations; callers with aliased inputs
   must first copy them to separate storage.
2. `LrCategoryWeight` returns intrinsic preferences; summary selection separately
   zeros unavailable categories because the approved signature has no catalog.
   Cost if wrong: consumers must not mistake preference for eligibility.
3. Platform RTC/file I/O, crash recovery, rendering, specials and story/encounter
   integration stay outside Week3. Cost if wrong: those integrations remain
   unavailable until later phases.
4. Real parties/species legality, factions/territory, all270 events and full DSL
   remain deferred. Cost if wrong: pending intentions/fixtures cannot be used
   as complete battle/world content.
5. Summary/hourly and arbitrary-partition equivalence are not promised. Cost if
   wrong: different advancement requests can yield different rival trajectories.
6. Internal tick primitives retain their validated-caller precondition. Cost if
   wrong: bypassing outer validation can corrupt state.
7. Legacy affect arithmetic outside the new bounded event domain remains
   unchanged. Cost if wrong: extreme legacy deltas can overflow.
8. Big-endian codecs remain unsupported. Cost if wrong: a big-endian port needs
   explicit conversion rather than reading these bytes directly.
9. Lifetime balance and real ARM11 performance remain unproven. Cost if wrong:
   later balance/performance calibration may require adjustments.

## Deferred minor

`SAVE_SERIALIZATION_DESIGN.md` section5 still describes obsolete48h/seven-daily
catch-up. Use its section9, the approved Week3 design and current README instead:
complete-world snapshots, at most seven summary opportunities per trainer,
then at most48 hourly and two daily detailed ticks. Documentation consolidation
is deferred, not silently treated as implemented behavior.

## Remaining limits

Existing v1 trainer layouts/codec are unchanged and insufficient to persist
the complete world alone. Snapshots are in-memory replay bytes, not a working
dual-slot save system. Content is representative, actual party generation is
Week4, and encounters/story losses are future integration policy. No full
1000-by365 fuzz campaign, ARM11 profiling or real3DS validation was performed.
