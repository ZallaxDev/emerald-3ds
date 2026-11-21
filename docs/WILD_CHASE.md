# Wild Pokémon that run at the player

OPTIONS > FIELD > **WILD CHASE** (SOME by default), with VISIBLE WILD on:
OFF, SOME (one in three) or ALL of the visible wild Pokémon notice the player
within four tiles where they live, hop with the alarm sound and run at them at
the player's walking pace. Touching the player starts the battle, as before.
One gives up after ten seconds or eight tiles and strolls on.

- `patches/pokeemerald/0093-visible-wild-chase.patch` (`src/wild_encounter.c`):
  `VisibleRollAggressive` decides, when one appears, whether it may chase;
  `VisibleNotice` watches the player's tile; a chasing one steps towards the
  player at speed 2 and gives up after 600 frames or 8 tiles.
- `3ds_port/src/3ds_extras.c`: the `wild_chase` option (OFF / SOME / ALL).

No art and no new text. The idea comes from Twin Embers' aggressive visible
wild Pokémon; no code was taken from it.
