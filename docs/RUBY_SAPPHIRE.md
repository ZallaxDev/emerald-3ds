# Experimental Ruby/Sapphire source setup

Emerald remains the priority for development, testing and support. Please help
improve the existing Emerald port first where you can. This optional setup is
for contributors who specifically want to explore Ruby/Sapphire support for
[issue #247](https://github.com/ZallaxDev/emerald-3ds/issues/247).
It opens a starting point for that work, without promising a playable release
or a timetable.

## Prepare the source

Only Git and Python 3.11+ are needed for these commands:

```sh
python tools/bootstrap.py --game ruby
python tools/bootstrap.py --game sapphire
```

The first command prepares `build/upstream-ruby`; the second prepares
`build/upstream-sapphire`. Both use the same pret/pokeruby repository, which
contains both games, at the exact commit recorded in the `[pokeruby]` section
of `upstream.lock`. The separate destinations keep each game's experiments
apart. This step does not build either version of the GBA game.

For a custom destination, choose a new, unused path:

```sh
python tools/bootstrap.py --game ruby --dir build/ruby-experiment
```

Preparation fetches only the pinned commit and checks it out with a detached
HEAD. Create a working branch in that checkout before starting development.
A repeat invocation accepts the same clean, pinned checkout without resetting
it. If you have edited files, created untracked files, changed the commit or
changed the remote, preparation stops and preserves your work. Use another
destination to prepare a fresh copy. Existing directories without this
setup's Git metadata are refused, even when empty. A failed download leaves
no partial destination.

## What is ready, and what is not

This is an upstream source checkout only. The command does not apply Emerald
patches, copy `3ds_port/`, generate assets or recipes, or create a 3DS executable.
`--make`, `--clean`, locale-ROM options and `--port-lang pt_br` are rejected
for Ruby/Sapphire before any checkout is changed. Emerald's normal setup
remains `python tools/bootstrap.py` (or explicitly `--game emerald`).
The setup refuses to reuse a Ruby/Sapphire checkout as an Emerald destination,
and refuses Emerald's default destination for Ruby/Sapphire.

The player-facing builder and release pipeline still do not support
Ruby/Sapphire. This developer option does not enable either ROM or change
the support messages for other games. FireRed/LeafGreen are outside this work.

## Next development work

A playable port still needs game-specific bridge hooks and patches, a 3DS build
path, asset and map extraction, voxel data, matching recipes and executables,
and hardware testing. The existing Emerald patches must not be assumed to
apply to pokeruby. Propose small, tested changes linked to #247, beginning
with a clearly identified English game/revision.

Keep the upstream checkout and its generated data out of this repository.
The default destinations are ignored under `build/`; a custom destination
outside `build/` may need a local ignore rule. Follow the publication rules
in [CONTRIBUTING.md](../CONTRIBUTING.md) for any subsequent contribution.
