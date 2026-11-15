# New looks for the people and the player

Optional, in OPTIONS > GRAPHICS:

- **NEW PEOPLE** (on by default): every person of Emerald on the field drawn
  again in a DS style, 32x32, from [Facets](https://github.com/ElvieBlooms/facets)
  by ElvieBlooms: DiegoWT's Black/White-style trainers and townsfolk,
  PurpleZaffre's named characters (gym leaders, Wallace, Archie, Maxie),
  Taiga's ORAS-style trainers, and KyleDove, Jext and Hyo's (with some by
  DiegoWT and Aqua) for everyone else. One look per person, in that order of
  the sets.
- **PLAYER** (ORIGINAL by default): the player in one of nine looks from
  [Emerald Lens](https://github.com/ElvieBlooms/emerald_lens) by ElvieBlooms:
  HGSS Brendan and May (DiegoWT), ORAS Brendan and May (Team Aqua's Asset
  Repo), Ash (PKMNTrainerRick), Kris (LouLilie), Gold (GregarLink10), Silver
  (lichenprincess) and Green (compiled by ChromeValiant; Poltergeist/Coffee
  Cup, Mid117, Wolfang62 and aethestode). On the field: walking, running, both
  bikes, surfing, diving, fishing, field moves, watering and decorating; in
  battle, the back and the throw for the looks that have one (ORAS Brendan and
  May, Ash, Kris, Silver).

The art is free to use with credit and is never in this repository or a
release: the builder fetches it at the commits pinned in
`builder/emerald3ds_builder/npc_looks.py`, checks each file's size and
SHA-256, and puts it in the data pack (`npcs/NAME.bin`,
`players/LOOK/SHEET.bin`) as an optional section, like the followers'. A pack
without it plays with the game's own people. The web builder does not fetch
it yet.

## How it works

`npc_looks.c` (patch 0094) answers `GetObjectEventGraphicsInfo` for a person
with a look: a 32x32 graphics info over the game's own (its anims, shadow and
tracks kept), its frames from the file, its colours on an ext palette of its
own (include/palette.h), so a person never takes an NPC palette slot. The
follower, its ball and the visible wild Pokémon share those sixteen ext
palettes: the people take all but the ones those may need (14 with the wild
Pokémon off, 8 on MANY), the player always gets one, and a person past that
is drawn as the game draws it until a palette is free again. Children
(16x16), the nurse (her bow is her first frame) and every person on the
standard nine frames are covered; the cycling triathletes, the Pokémon that
stand on the map and the rival keep the game's look.

Changing an option shows when the field makes the sprites again (a door, a
warp, a menu).

# Wild Pokémon that run at the player

OPTIONS > FIELD > **WILD CHASE** (SOME by default), with VISIBLE WILD on:
OFF, SOME (one in three) or ALL of the visible wild Pokémon notice the player
within four tiles where they live, hop with the alarm sound and run at them at
the player's walking pace. Touching the player starts the battle, as before.
One gives up after ten seconds or eight tiles and strolls on (patch 0095).
Idea from Twin Embers' aggressive visible wild Pokémon; no code taken.
