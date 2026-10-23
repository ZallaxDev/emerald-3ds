# Architecture

Emerald 3DS runs the pokeemerald game code natively on the 3DS's ARM11. The
game keeps its own main loop (`AgbMain`), its GBA register model and its data;
the port supplies what the GBA hardware did, through a small bridge.

```
            game translation units                     native translation units
   (pokeemerald src/, data/ + port bridge files)        (3ds_port/src, libctru)
 ┌──────────────────────────────────────────────┐    ┌──────────────────────────────┐
 │ AgbMain, tasks, sprites, scripts, battles    │    │ main_3ds.c   process, heaps   │
 │   │ GBA registers, VRAM, OAM, palettes (RAM) │───▶│ 3ds_video.c  GPU compositor   │
 │   │ m4a / MP2K mixer (portable float path)   │───▶│ 3ds_audio.c  NDSP queue       │
 │   │ #ifdef PORT_BRIDGE hooks                 │    │ 3ds_input.c  HID, touch       │
 │   ▼                                          │    │ 3ds_data.c   romfs/loose/pak  │
 │ 3ds_assets.c  3ds_map_loader.c               │───▶│ 3ds_log.c    SD log (thread)  │
 │ 3ds_script_loader.c  3ds_compat.c (saves)    │    │ ctr_voxel.c  Citro3D voxel    │
 │ 3ds_bottom_ui.c  voxel/*.c (game side)       │    │ 3ds_bottom_screen.c           │
 └──────────────────────────────────────────────┘    └──────────────────────────────┘
```

## Two kinds of translation unit

- **Game units** (`build/root/**`, `build/bridge/**`, `build/voxel/**`) are the
  decompilation plus the port's bridge files. They compile with
  `-DPORT_BRIDGE -DPLATFORM_3DS -DPORTABLE -DMODERN=1` and see the headers in
  `3ds_port/compat/` (`port_platform.h`, `port_log.h`). They never include
  libctru.
- **Native units** (`build/*.o`) are the backend: libctru, Citro2D/Citro3D,
  threads, files. They compile with `-Werror` and never see game headers.

The boundary is plain C calls declared in `3ds_port/include/`.

Everything builds in **ARM state**: GCC has no Thumb-1 hard-float ABI on
ARMv6K and libctru is hard-float. The places where the game sets bit 0 on
function pointers (callbacks, native script commands) are guarded for
`PLATFORM_3DS`.

## Frame

`AgbMain` runs forever. Every iteration ends in `WaitForVBlank` →
`VBlankIntrWait`, where the backend:

1. runs the VBlank handler (m4a mixes one frame; the frame goes to NDSP;
   with OPTIONS' REAL-TIME PACING the worker mixes on the DSP's clock
   instead, whenever NDSP has fewer than three frames queued, and
   `FrameShown` in `3ds_platform.c` runs a hidden frame when the game has
   fallen a frame behind the display - in the field only: a battle keeps
   the GBA's timing, save under its party menu, summary and bag, where
   hidden frames keep real time with the engine still on the game's frames;
   without a DSP the clock mode mixes once per frame as before);
2. latches the GBA registers, OAM and palettes and presents: the compositor
   decodes changed tiles into an RGBA5551 atlas and draws BG/OBJ layers with
   the PICA200 at native 400x240 on the top screen; the voxel overworld, when
   active, draws the field in 3D from the live map state;
3. scans HID and touch; the bottom-screen UI draws and turns touches into
   game input.

The logical display stays GBA-sized for game logic; the field of view is
widened (`FIELD_VIEW_*`) and screens staged as one GBA picture (title,
intro, credits, battle, region map, title menu and professor's speech, naming
screen, clock) are composed centred with margins from their own art
(`3ds_port/compat/ctr_gba_*.h`): a still picture carries its edge out and
fades it into black, a scrolling one wraps, and a menu screen carries its
plain or patterned background out to the edges. The intro's leaves scene has
margins of new art instead (`3ds_port/scripts/gen_intro_margins.py`): the
shapes its edge cuts carried on from the edge's own pixels, and grass, bushes,
plants and hills drawn for it. Because it carries those pixels on, the builder
makes it from the player's ROM (`stage/leaves.bin`), as it does the voxel data.

## Memory

The linear heap is 8 MiB (`__ctru_linear_heap_size`): the 1024x1024 tile atlas,
Citro3D command buffers, the voxel staging VBO and one metatile atlas. VRAM is
claimed as fixed arenas at start-up (`voxel_arena.c`) because variable-size
allocation fragmented it. The CPU never writes VRAM on hardware; textures are
filled from linear buffers with `C3D_SyncTextureCopy`.

## Data outside the executable

The 3DSX carries code and engine files only; game data comes from
`CtrData_*` (`3ds_port/src/3ds_data.c`): the RomFS of a development build, loose
files under `sdmc:/3ds/emerald3ds/devdata/`, or `sdmc:/3ds/emerald3ds/emerald3ds.pak`.
How each kind of data reaches the game (INCBIN stubs, map descriptors, bundled
regions, the `.gamedata` section) is described in
[ASSET_PIPELINE.md](ASSET_PIPELINE.md).

## Bottom screen

The bottom screen replaces the START menu (`3ds_bottom_ui.c`,
`3ds_bottom_screen.c`): map, party, bag, trainer card, Pokédex, PokéNav, save
and options. Actions that need the game's own logic (switching a Pokémon,
giving an item, field moves) run the original menu hidden while the top screen
holds its last frame, driven by injected key presses; what that menu shows is
read back and drawn as buttons. The PokéNav is the game's own, run as it is:
its screens are composed as a centred GBA screen and drawn into the 240x240
area left of the column instead of the top screen, laid out for its 240 lines
(the header on the top edge, the help bar on the bottom edge, the rest in the
middle), and a tap on them becomes the buttons they read. The PC's boxes and
the summary opened from them are the game's own screens too, composed the same
way but over the whole bottom screen (the picture 1:1 in the middle, the
boxes' scrolling pattern around it); a tap on them acts in the game directly
(`pokemon_storage_system.c`, `pokemon_summary_screen.c`, under
`PLATFORM_3DS`), without key presses. The bag is the game's own as well
(`item_menu.c`): opened from the field it sits left of the column, opened
from a battle, a shop or the PC it takes the whole screen, its stripes carried
on around it, and taps act on it directly too. So is the Pokédex
(`pokedex.c`), left of the column, the tile behind each of its screens carried
out to its edges. While it is open the top screen shows the device itself
(`3ds_dex_top.c`): the red shell, lens and lights around a screen window
with the Pokémon of the info page, or under the list's cursor, at 2x over
the battle terrain of the place it is met. The game side
(`3ds_dex_top_game.c`, patch 0058's `CtrPokedex_InfoDexNum`) decodes the
front picture and the terrain from the game's own tables into plain buffers;
the native side writes them into its two textures inside the frame, after
`C3D_FrameBegin` has waited for the GPU, and gives the textures back when the
Pokédex closes. In battle the column gives way
to the action and move menus.

## Voxel overworld

`3ds_port/src/voxel/`: the map, camera, atlas and mesh modules are game units
(they read `gMapHeader` and the metatiles); `ctr_voxel.c` owns the Citro3D
objects. Chunks are built in slices across frames, uploads per frame are
capped by the GX queue (32 entries), building textures stream from a worker
thread, and a fixed sun with baked shade and cast sprite shadows grades the
scene (`VOXEL_LIGHTING`). Buildings, trees, signposts and drawn terrain relief
are modelled on the host from each map's own art (`3ds_port/scripts/gen_voxel_*.py`)
and read at runtime from `voxel/*.bin`. How to work on them:
[VOXEL_GUIDE.md](VOXEL_GUIDE.md).

With the 3D slider up the world is drawn once per eye, each from a camera
moved half an interocular distance sideways with the asymmetric frustum of a
stereo pair (`Mtx_PerspStereo`); the chunk cache, atlases and billboards are
shared, so the second eye costs its draw calls and fill. The plane of zero
parallax stands just in front of the player; the game's text and weather are
composed on it, the same in both eyes. A New 3DS has the frame for the second
pass; on an Old 3DS the world stays flat unless OPTIONS' 3D WORLD says ON
(`sVoxelStereo` in `3ds_video.c`), and 3D DEPTH scales the slider. Both are
extras on the SETTINGS tab's second screen (`3ds_extras.c`).

## The camera at a map's edge

The 400x240 screen shows more of a map than the GBA's, and past a map's edge
there is only its border tiled over and over. On the edges chosen map by map
(`sCameraEdgeMaps` in `field_camera.c`; none by default) the camera stops at
that edge and only the player walks on. In front of a map joined to part of
such an edge, and in dark caves, whose circle of light is the middle of the
picture, the camera stays on the player. In 2D (`field_camera.c`) the game's
camera still follows the player, as the scripts expect of it; the
picture, backgrounds and sprites alike, is held back from it by a shift, and
the tilemap's ring follows the picture rather than the camera. A map narrower
than the screen, stopped at both ends, is centred on it. In voxel
(`voxel_camera.c`) the point the camera looks at stops short of the player instead, never so far that the
player walks into the diorama's blur. Both ease to a new place when the edges
change under them, as on a crossing, and the people spawned move with the
view (`3ds_field_view.c`).

## Saves and logs

The 128 KiB flash image is mirrored in RAM and written range by range to
`sdmc:/3ds/emerald3ds/emerald3ds.sav`. The log is off by default; if
`sdmc:/3ds/emerald3ds/debug.txt` exists, it goes to
`sdmc:/3ds/emerald3ds/port.log` (the previous session is kept as
`port-prev.log`) from a background thread, because SD writes on the render
thread cost 50–500 ms on hardware.
