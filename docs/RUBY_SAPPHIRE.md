# Ruby and Sapphire's wild Pokémon

OPTIONS > POKÉDEX > **RUBY/SAPPHIRE** (on by default): the seven Hoenn
Pokémon that Ruby and Sapphire have in the wild and Emerald does not, on the
maps and slots where those games have them (pret's pokeruby,
`src/data/wild_encounters.json`), at that slot's level:

| Pokémon | Where |
|---|---|
| Zangoose | Route 114 (grass) |
| Roselia | Route 117 (grass) |
| Lunatone | Meteor Falls, every room (land and surfing) |
| Meditite | Mt. Pyre's outside, Victory Road B1F |
| Medicham | Victory Road B1F and B2F |
| Dusclops | Sky Pillar 1F, 3F and 5F |
| Surskit | Routes 102, 111, 114, 117 and 120 (surfing, 4%) |

A slot is taken only where the species it held stays on that table through
another slot, so no Emerald Pokémon leaves a map. Surskit's Ruby/Sapphire
slots are the routes' only Seedot and surfing Goldeen ones, so it takes the
fourth surfing slot (Marill's) instead.

They are met in battle, as visible wild Pokémon, in the bottom screen's list
of the map's wild Pokémon, and the Pokédex's AREA marks their maps.
`3ds_port/src/3ds_encounter_tables.c` holds the slots (checked against the
pinned tables by `3ds_port/tests/encounter_tables_test.py`); patch 0097 makes
the game read the surfing table through it, as it already read the grass one
for the timed encounters, and the AREA screen ask it.
