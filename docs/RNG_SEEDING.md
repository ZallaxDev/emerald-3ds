# RNG seeding on 3DS

Vanilla Emerald initializes its primary RNG state to zero, so repeating the
initial stream at boot is original game behavior, not a defect introduced by
this port. The optional upstream `SeedRngWithRtc()` change is guarded by
`BUGFIX`; this port enables the separate `UBFIX` flag.

This proposal makes two distinct changes for maintainer consideration:

- Normal 3DS boots and soft resets start from console timing instead of zero.
  This deliberately changes vanilla behavior and compatibility with zero-seed
  RNG manipulation. It is not required merely to make the port match Emerald.
- New-game generation reseeds at the reachable portable trainer-ID path. The
  port already generates both halves of the trainer ID from `Random()` there;
  its GBA naming-screen timer reseed is excluded by `PORTABLE`. This hook
  provides fresh timing when a new game begins, including after the RNG has
  advanced since boot. The portable path already avoids reading the GBA timer;
  the missing portable reseed is the issue addressed here.

The 3DS port now seeds the existing RNG at every `AgbMain` entry, including a
soft reset, using a mix of the console's high-resolution system tick and wall
clock. The reachable portable `InitPlayerTrainerId` path also samples the
same source immediately before its two `Random()` calls. The GBA naming
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
trainer-ID propagation and guarded call sites. It compiles the actual applied
`main.c` startup block and `new_game.c` trainer-ID function in eight macro
configurations, including `PORTABLE`, and checks that scenario new games do not
reseed or truncate the previous RNG state. From an overlay checkout, provide
an applied source tree explicitly:

```
python3 3ds_port/tests/rng_seeding_test.py --source-tree /path/to/bootstrapped/tree
```

Full `make verify` in a bootstrapped tree includes this test. Physical-console
reset behavior still requires hardware acceptance.
