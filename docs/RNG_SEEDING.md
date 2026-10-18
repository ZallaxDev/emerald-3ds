# RNG seeding on 3DS

Vanilla Emerald initializes its primary RNG state to zero, so repeating the
initial stream at boot is original game behavior, not a defect introduced by
this port. The optional upstream `SeedRngWithRtc()` change is guarded by
`BUGFIX`; this port enables the separate `UBFIX` flag.

The port makes two distinct changes:

- With OPTIONS' **RANDOM BOOT SEED** on, normal 3DS boots and soft resets
  start from console timing instead of zero. It is **off by default**: the
  boot seed is then Emerald's zero, so vanilla behavior and the zero-seed RNG
  manipulation built on it stay available. It is not required merely to make
  the port match Emerald.
- New-game generation reseeds at the reachable portable trainer-ID path. The
  port already generates both halves of the trainer ID from `Random()` there;
  its GBA naming-screen timer reseed is excluded by `PORTABLE`. This hook
  provides fresh timing when a new game begins, including after the RNG has
  advanced since boot. The portable path already avoids reading the GBA timer;
  the missing portable reseed is the issue addressed here.

The 3DS port seeds the existing RNG at every `AgbMain` entry, including a
soft reset, through `Port_RngSeed`. With RANDOM BOOT SEED off it returns the
zero fallback without reading the clock; on, a mix of the console's
high-resolution system tick and wall clock. The reachable portable
`InitPlayerTrainerId` path always samples that timing source immediately
before its two `Random()` calls, whatever the option says. The GBA naming
screen timer hook is disabled by `PORTABLE`, so it is not used for this fix.
Only these seed inputs change. The RNG algorithm, encounter generation, shiny
checks, secondary RNG, and existing saves/Pokémon/trainer IDs are unchanged.
New games can naturally receive different trainer IDs.

The seed remains 16-bit, matching `SeedRng`. Zero is a valid result, collisions
are possible, and a different seed on every reset is not guaranteed. This is
not a cryptographic generator or an increase to shiny odds. Frame-based RNG
manipulation that assumes a zero startup seed will behave differently in
normal play. Scripted/gameplay reseeding (including battle recordings) is not
removed.

## The RANDOM BOOT SEED option

The third line of OPTIONS' second screen (after 3D WORLD and 3D DEPTH, or
alone there in a `VOXEL=0` build), kept in `settings.txt` as `boot_seed=`
(`0` OFF, the default; `1` ON; a missing or unknown value is OFF). The key is
`CTR_BOOT_SEED_KEY` in `3ds_port/include/3ds_extras.h`. It is read when
`AgbMain` starts, so turning it on or off takes effect from the next soft reset
(A+B+START+SELECT) or boot. It changes only the boot/soft-reset seed: the
new-game trainer-ID reseed stays on either way, since without it the portable
path would take both trainer-ID halves from whatever state the RNG reached.

Measured scenarios retain the historical input at both seed points: recording
and playback use zero at startup and skip new-game reseeding entirely, so
the portable trainer-ID path continues its historical RNG state. These hooks
never read live ticks or wall time during scenarios. Existing recorded inputs
remain deterministic.
This preserves the historical scenario behavior rather than introducing new
scenario entropy/metadata. The separate seeded species randomizer's own seed
and permutation are unaffected.

`make -C 3ds_port verify-rng-seeding PYTHON=python3 HOSTCC=cc` tests the actual
backend function, source variation, integer boundaries, scenario fallbacks,
trainer-ID propagation and guarded call sites, and the boot seed option: its
OPTIONS row (two values, OFF by default), the zero seed it keeps at boot and
on a soft reset without reading the clock, and the new-game reseed that does
not depend on it. It compiles the actual applied
`main.c` startup block and `new_game.c` trainer-ID function in eight macro
configurations, including `PORTABLE`, and checks that scenario new games do not
reseed or truncate the previous RNG state. From an overlay checkout, provide
an applied source tree explicitly:

```
python3 3ds_port/tests/rng_seeding_test.py --source-tree /path/to/bootstrapped/tree
```

Full `make verify` in a bootstrapped tree includes this test. Physical-console
reset behavior still requires hardware acceptance.
