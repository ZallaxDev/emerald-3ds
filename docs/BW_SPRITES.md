# Black 2 / White 2 animated sprites

Optional. With the pictures on the SD card, the Pokémon are drawn from the
animated Black 2 / White 2 sprites instead of Emerald's still ones: in battle
(front, back, shiny, forms, females with their own picture), in the
evolution scene, in the Pokédex (the top screen's window animated, the
bottom screen's list on the first frame) and in the party's Summary
(animated). Without the file, nothing changes. OPTIONS > SCENE > **BW2
SPRITES** turns it off.

Nothing of the art is in this repository or in a release: the player makes
the file with the script below, which downloads the sprites from Bulbagarden
Archives.

## 1. Convert the sprites (on a PC)

Needs Python 3 and Pillow (`pip install pillow`).

```
python tools/bw_sprites.py --out bw_out
```

The script lists Bulbagarden Archives' four categories (front, back and their
shiny versions), downloads the APNGs of #001 to #386 into `bw_cache/` and
writes the converted pictures (the BWS1 format, `3ds_port/include/3ds_bw_mons.h`)
into one file, `bw_out/bw.pak`. For a quick test: `--only 1-9,25,384`. With
the PNGs already in `bw_cache/`: `--offline`.

## 2. Copy it to the SD card

```
sdmc:/3ds/emerald3ds/bw.pak
```

One file opens far faster on the card than a folder of a thousand (FAT walks
the folder at every open). With `--loose` the script writes one file per
picture under `bw/front` and `bw/back`, which the game also reads when there
is no `bw.pak`. Names, in the pack and the folders: `NNN[form][_f][_s].bin`.
The form follows Bulbagarden (Unown `B`-`Z`, `EX`, `QU`; Castform `S`, `R`,
`H`; Deoxys `S`), `_f` is a female's own picture and `_s` the shiny one. A
female without her own takes the common one; a form without its own, the
species'.

## How it works

- `battle_gfx_sfx_util.c` (patch 0093) tells the game side
  (`3ds_bw_mons_game.c`) whenever a battler's graphics are loaded (sending
  out, Transform, Castform, Substitute) and, as a foe faints, how far its
  sprite has gone. The evolution scene registers its two Pokémon the same
  way (`CtrBwMons_Scene`).
- The compositor (`video/bw.inc`, `BwFrame` and `BwDrawInScene`) draws, for
  every OAM entry that shows a battler's tiles, the animation's current frame
  instead, with the sprite's own position, affine matrix (growing out of the
  Poké Ball, the moves' distortions), blend and palette fade (`BwTint`). The
  picture stands on the original's feet. Fronts are drawn 1:1 (one BW pixel
  for one GBA pixel); backs at the height of the original back, between 1x
  and 2x (`BwScale`).
- A battler is drawn only while its sprite is still the one made from its
  position's template, so a sprite id that another screen reuses (the
  nickname screen after a catch, the evolution scene after a battle) is not
  taken for it.
- Moves that copy the Pokémon onto a background layer (`monbg`: Leech Life,
  Faint Attack, Memento, Uproar...) hide the sprite and animate the copy. The
  copy is drawn from the BW picture too, at the layer's scroll, with its
  blend and fade (`BwDrawCopies`).
- The animation stops while the Pokémon is asleep or frozen.
- The pictures are read on a thread of their own (`3ds_bw_mons.c`, core 1),
  never inside a frame: the read starts as the game loads the Pokémon, before
  the ball opens. Until it is in, Emerald's sprite is shown.

## Known limits

- The line-by-line wave some moves put on the background copy is not
  reproduced: the BW picture moves whole.
- With the 3D slider up in battle, the background copy is not drawn.
- The PC boxes and the caught Pokémon's Pokédex page still show Emerald's
  sprites.
- Large Pokémon (Rayquaza, Wailord, Groudon) go past the GBA's 64x64 box, on
  purpose, to keep BW's scale.
- Tested on a 3DS console, not in an emulator.
