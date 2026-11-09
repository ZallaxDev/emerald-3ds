# Living Rival Prototype (Phase 2.5, Layer A)

Portable, deterministic simulation core for the Living Rival system.
Target: Nintendo 3DS (ARM11), with possible PC/mobile ports. Not a GBA target.

## Build and test

GCC lives at `C:\msys64\usr\bin\gcc.exe` and is not on PATH:

```powershell
$env:PATH = 'C:\msys64\usr\bin;' + $env:PATH
make test
```

Each `test/test_*.c` is a standalone executable. `make test` builds and runs all of them.

## Rules

- C99, no platform headers, no dynamic allocation, fixed-size arrays.
- All exported symbols are `Lr`-prefixed.
- Same inputs + same seed must produce bitwise-identical state.

## Week 3 simulation

The world owns its clock, stable traits, runtime sidecars, simulation RNG and
one shared chronicle. `LrSimulateTo` preserves partial hours and processes hour
then day boundaries chronologically. Jumps above48h summarize the older range
in at most seven personality-weighted opportunities per active trainer, then
replay the final48h (at most two detailed daily ticks). Not every opportunity
produces an event, and there is no five-category reward checklist.

Both in-game calendar changes and offline elapsed time advance rivals.
The platform adapter must anchor on close and call `LrResumeOffline` once on
open; no platform RTC or file I/O exists here. Offline conversion is1:1 and
preserves a high-water mark across clock rollback. Forward clock changes count
as elapsed time. Supported game time ends at minute94371839 (day65535).

Exact partition equivalence applies only to detailed simulation. Summaries
are deterministic approximations, not equivalent to every-hour replay or
arbitrary partitions. Replay the same ordered requests and content version.
`LrSnapshotWrite`/`Restore` include all mutable replay inputs, not pointers;
the older v1 trainer-only save blob is insufficient for complete-world replay.

Representative const content contains12 events, NOT the full270. Team captures,
evolutions and reorganizations are pending intentions; real party generation
is Week4. World growth has no player-level ceiling or rubber-banding. Future
encounter integration must acknowledge genuine rival strength and allow story
progress after a mandatory defeat, rather than secretly weakening teams.

Run the text/data demonstration after `make test`:

```powershell
.\build\test_gazette.exe
```

See `integration_eval/GAZETTE_UI_PROTOTYPE.md` for query/text contracts and
`integration_eval/SAVE_SERIALIZATION_DESIGN.md` for full snapshot field order.
Host results do not establish3DS timings, complete content, platform persistence
or readiness for battle/story deployment.
