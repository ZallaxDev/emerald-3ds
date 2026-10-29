# Changelog

## 0.5.0 — 2026-10-29

The first beta.

Most of Hoenn is now recreated in voxels, but the 3D world is still under
construction: there are still many faults, flat elements and other problems,
which will be fixed in the coming versions. Everything keeps being built and
improved.

- Voxel world: most of Hoenn's interiors stand in 3D, by @Deunnis:
  Lilycove's contest stage, lobby and hall (pull request #125, issue #119),
  its Department Store from 1F to the rooftop with the elevator (pull
  requests #129 and #132, issue #126), its museum and the Pokémon Trainer
  Fan Club (pull request #139, issue #138); the Day Care, Lanette's house,
  the Seashore House and Devon Corp 1F (pull request #143, issue #142); the
  Battle Tents, Slateport's Fan Club and Harbor, and Stern's Shipyard (pull
  requests #145 and #155, issue #144); the shops, route houses and stations
  (pull request #156, issue #154); the Mossdeep Game Corner and the
  Sootopolis Mystery Events House (pull request #159, issue #157); the
  Pokémon League (pull request #161, issue #160); the gyms of Mauville,
  Mossdeep, Fortree and Sootopolis (pull request #163, issue #162); the
  Battle Frontier's lobbies, Scott's house, the lounges, battle rooms and
  corridors (pull requests #167 and #168, issue #164); the Trick House's
  puzzle rooms (pull request #170, issue #169); the link rooms, the moving
  truck, the S.S. Tidal and Trainer Hill's entrance (pull requests #172 and
  #173, issue #171). Cells that scripts change (the contest lobby, the
  Mossdeep Game Corner, the Elite Four's rooms) show the change (pull
  request #165). The steps under trees are closed (pull request #120, issue
  #107) and no cut tile's ground shows in a pit (pull request #124, issue
  #117), also by @Deunnis.

- Voxel world: every gym in 3D. Dewford's is finished; Mauville's beams
  switch as its puzzle switches them; Lavaridge's 1F and B1F, Petalburg's
  sliding doors, Fortree's round green columns, Mossdeep's panelled walls
  and Sootopolis's tiers, stairs and slides stand at their own levels. Doors
  open with their own frames on every wall. The gyms are modelled but still
  have faults, to be fixed in the coming versions.

- Voxel world: Fortree's tree houses, plank bridges and treetop palms;
  every house of Sootopolis, its white walls, its cliff and the Cave of
  Origin; the Battle Frontier finished outside, every flag waving, with its
  halls, arches and the Pike; the first room of Meteor Falls; the Safari
  Zone's gate on Route 121 and its building from inside the Zone; Route
  118's pink house and Verdanturf's tent.

- Voxel mountains: Lavaridge, Routes 111 to 113 (Route 112's plateau, stairs
  cliff and rough rock among them), 114, 120 and 121, Lilycove, Mossdeep and
  Routes 124 to 129 stand in hand-marked terraces. Ledges between two levels
  are steps; bridges cross the mountains with the rock under them only where
  they cross; corners keep their grey outline; trees on the rock stand on
  it. Each map stands at the height of the seam it was reached by, so the
  maps round Lilycove and Route 124 meet again.

- Voxel trees and nature: Mossdeep's and Fallarbor's own 2x2 trees, small
  trees on Fortree's, Mauville's and Lavaridge's grounds and the secret base
  tree, long grass in 3D, clouds lying on the water as the GBA shows them,
  and fences built post by post.

- Voxel world: animated water stays in step with the 2D tiles and costs an
  Old 3DS little; shadows fall where the geometry casts them; no seam lines
  between chunks or lines of another metatile along tile edges while
  walking; the land past a map's edge is filled with that edge's own
  ground.

- Voxel overworld: a camera swoop between the 3D world and the 2D field:
  switching VOXEL 3D on or off (or a fallback to 2D) tilts the camera to the
  2D field's view and crossfades, both ways, instead of a cut. By
  @darkmattericecoffee (pull request #121).

- The bottom screen, redesigned (v4.2): a header with the place, the grass
  toggle, Repel, the clock and RUN; a dock of sections (MAP, POKéDEX,
  POKéMON, BAG, POKéNAV, CARD, SAVE, OPTIONS, and NUZLOCKE when it is on);
  the game's screens in a window. The party, summary, bag and Pokédex
  (entries, AREA, CRY, SIZE, search) are drawn natively at the touch
  screen's resolution, in battle too, with held items, a scroll bar in the
  bag and the summary's Pokémon moving with its animation. A tap on a
  section jumps straight there. The trainer card has its back, and CLASSIC MENUS (off by default) brings back
  the game's own party, bag, Pokédex and card in the window. Plates are made
  ahead and kept on the SD card, so the screen redraws much faster.

- OPTIONS, reorganised: every option of the game, the port and the
  enhancements in one place on the bottom screen, sorted into five
  categories - GENERAL (interface, system), WORLD (3D, field, helpers,
  clock), BATTLE (scene, rules, experience), MODES (Nuzlocke, randomizer)
  and CHEATS (catching, perks, Pokédex, items) - each with its groups as
  tabs, six rows to a page. A row is a switch, a value between arrows, an
  action or a line of information; options a build does not have are left
  out.

- Nuzlocke tracker (off by default, OPTIONS > MODES > NUZLOCKE): record by hand
  each area's catch or miss, mark fallen Pokémon and check the next badge's
  level cap. It never enforces a rule or changes the save, and keeps its
  records in `emerald3ds.nuzlocke` beside the save. By @jacobcorbett (pull
  request #112).

- Randomizer (off by default, OPTIONS > MODES > RANDOMIZER): wild Pokémon,
  starters and trainers' Pokémon by one reproducible seed; the ROM and the
  saves are untouched. By @jacobcorbett (pull request #113).

- Field information (off by default, OPTIONS > WORLD > HELPERS and CLOCK): the wild Pokémon of the current area
  with their levels and rates (names hidden until seen), the Repel steps
  left, and a notice when the saved clock had to fall back. SYNC CLOCK
  brings a fast clock back to the console's time without losing the berries'
  growth. By @jacobcorbett (pull requests #115 and #109).

- The game's random number generator is seeded from the console's timing at
  boot, soft reset and new game, instead of always from zero. By
  @jacobcorbett (pull request #116).

- Voxel: nearby wild Pokémon get their sprites before ambient effects, the
  S.S. Tidal and the submarine are drawn at full width, and seated people
  sit right; the text box stands in front of the scenery in 3D, and a door's
  animation is no longer lost while the world loads after a warp. By
  @jacobcorbett (pull requests #110 and #114).

- Windows and web builders: the SD card install is staged and only then
  replaces the old files, the save (and its clock and Nuzlocke files) is
  backed up first, and a missing DSP firmware (`3ds/dspfirm.cdc`) is
  reported; a damaged recipe, a malformed release manifest or a missing
  language executable give a clear error. By @jacobcorbett (pull requests
  #101, #102, #103 and #104). The builder names the other regional Gen 3
  games when one is loaded by mistake, by @abrahammordev (pull request #95).

- Wide screen and GBA screens: the slot machine and the OPTION screen are
  shown centred as GBA screens, the cave transition covers the whole screen,
  the roulette is centred, with its wheel drawn once and the felt and casino
  wall behind it, Fly's bird and its rider fly round the player, and move
  backgrounds that are one picture are magnified to the screen.

- Fixes: an HP bar at 1 HP shows a sliver, as on the GBA; a diving player is
  in the water for the visible wild Pokémon; a 2D map next to one with
  another tileset draws its neighbour with that tileset; with PHYS/SPEC
  SPLIT on, closing the summary no longer read freed memory; taps on a list
  menu's odd entries count again.

- Performance: data files, follower and visible wild sprites are read ahead
  on a worker instead of in the frame; the 3D battle starts without a stall
  and uses less VRAM; the Old 3DS keeps its frame rate on the mountain
  routes (fewer light rays, merged relief quads, a seam cache) and in 3D
  battles with sandstorm or Surf.

- Building from source: the voxel world is always compiled in (`VOXEL=0` is
  gone).

- Voxel world: the Sky Pillar's tower and its five floors, the Abandoned
  Ship's corridors, cabins, deck and flooded rooms, and the Aqua and Magma
  Hideouts stand in 3D, by @Deunnis (pull requests #185, #188, #189 and #191,
  issue #179). Room for 1024 texture pages of modelled maps instead of 256,
  so the caves and dungeons still to come fit, also by @Deunnis (pull request
  #184). docs/VOXEL_STATUS.md has the buildings found on Routes 111 and
  118-134 and a console review of the rows to check, by @Deunnis (pull
  requests #192 and #198).

- Releases can carry the Brazilian Portuguese translation (`build_release.py
  --portuguese`: `payload/pt/`, `Emerald3DS-pt.3dsx`), made from the English
  ROM. Its recipe carries the translated texts, which are in no ROM, and
  `gen_recipe.py --translation` credits only the bytes inside the catalogs'
  texts as the translation's own; everything else keeps the usual limits.

- Builder: one ROM can make more than one language. With the English ROM the
  window asks for the game language (English or Português (Brasil)) when the
  release has both, and the command line takes `--language pt`; `verify`
  tells the two packs apart by their data ABI. The web builder's entry point
  is API 2 (`run_web_build(..., language)`), and `web-manifest.json` may list
  the English ROM twice, once per language.

- OPTIONS > GENERAL > SYSTEM > REAL-TIME PACING is on by default: a new
  settings.txt starts with it on (and so does one that never had it). A
  settings.txt that already chose off keeps off.

- 3D world: the rain, the sandstorm and the volcanic ash fall in the scene
  instead of sliding over the screen - streaks, blowing sand and drifting
  flakes in the air at their own depth, hidden by roofs and fading into the
  distance, with the light of each weather and the thunderstorm's lightning
  on the world. They cost the console almost nothing, and the 3D world no
  longer draws the old sand and ash pictures over itself, which were most
  of an Old 3DS frame.

- 2D field: the sandstorm and the volcanic ash are drawn in one batch
  instead of some two thousand separate pieces a frame.

- Builder: the 3D scenery takes seconds instead of minutes. The release runs
  the voxel generators once per supported ROM and the recipe carries, for
  each of their files, a program that writes it again from the player's ROM
  (tools/voxel_precompile.py, docs/VOXEL_PRECOMPILE.md): the port's own
  geometry as it is, every texel of the game's art as the tile, row and
  palette of the ROM it comes from. The data pack is byte for byte the same.
  A file without a program is still generated as before.

- Releases are made with one fixed Python (upstream.lock: 3.13, Pillow
  12.3.0) - the voxel data depends on it - and `tools/docker_release.py`
  makes one in a pinned environment (devkitARM by digest, the original
  game's ELF built with the pinned agbcc), from scratch.

- Web builder: the buildings came out different under Pyodide than on the
  desktop, so the web build failed its check: a wall of Sootopolis's gym
  took its colour from a tie broken by the order of a set, which follows a
  32-bit hash in the browser. It now follows the desktop's order
  (gen_voxel_relief.set_order, as the relief already did).

- Builder: the Mauville gym's electric barriers' animation frames are among
  the voxel inputs the builder rebuilds from the ROM (and built before the
  generators in a fresh tree): the buildings generator reads them and failed
  without them.

- Builder: the terrain relief is made after the buildings, whose cells it
  reads. Made side by side, the relief depended on which finished first,
  and a data pack could fail to match the release.

- Juan's gym battle no longer crashes the game when it ends, by @DannyM90
  (pull request #182, issue #134). His defeat text is longer than the
  battle's text buffer; on the GBA the rest landed in an unused array, here
  it ran into live variables. The buffer now has room to spare behind it.

- Portuguese, by @carlosmozart (pull request #178): the general texts of the
  scripts (the POKéMON CENTER and MART signs, SELECT with no registered item,
  whiting out, POKéRUS, a TM or HM found, the weird tree...) are translated,
  and the bottom screen's type and contest icons are in Portuguese again.
  In every language, the bottom screen's Pokédex shows an entry's whole
  description, over the box's full width, instead of losing its last line.

- Double battle target choice, by @eathanpage (pull request #176): the
  bottom screen shows the four places of the battle two by two, as the top
  screen has them, instead of left and right arrows and OK. Each plate has
  the Pokémon's icon, name, level, gender and HP bar (the HP figures for
  your side only), the opponents in red and your side in blue; one the move
  cannot reach is grey and takes no tap, and an empty place has no plate. A
  tap on a plate aims there and confirms; the D-pad walks the grid.

- Double battles, by @maxxandres (pull request #152): the bottom screen's
  target plates and the second mon's move menu are made ready while the
  turn plays out, with the action menu and the first mon's moves. Built
  when they first came up, they dropped an Old 3DS's frame rate sharply the
  first time a target, or the second mon's move, was chosen. The second
  mon's moves have pictures of their own, so warming them no longer throws
  out the first mon's.

- Voxel world: a map whose layout its script swaps is drawn with the swapped
  one. The Mirage Tower no longer stands in 3D on the days the game hides it,
  or after it falls, and it no longer shows from Mauville or Routes 112 and
  113 while it is hidden; Route 131 shows the Sky Pillar, and Mirage Island
  and the Sky Pillar's clean floors get their own relief and objects. Route
  111's hand-shaped mountains apply to its layout without the tower too.
  Reported, with the cause, by @Deunnis (issue #179), who also keyed the
  current map's data on the swapped layout (pull request #180).

- Bottom screen map: a magnifier button in the map's bottom right corner
  zooms in on the player's place and back out. Touching a place on the map
  picks it: zoomed in, a panel shows its name and, for a city or town, its
  picture, or the landmarks there. Based on @luksgrin's pull request #108.

- Fly on the bottom screen: FLY from the party menu, or the HM from the Bag,
  opens the bottom screen's map as the Fly map, with the places you can fly
  to marked. Touch a place, then FLY in the header, or A; CANCEL or B goes
  back to the party or the Bag. Meanwhile the dock is locked and the game's
  buttons wait. The PokéNav's HOENN MAP entry is gone: the bottom screen's MAP
  is that map, zoom included. Based on @luksgrin's pull request #108.

- Battle speed, by @maxxandres (pull request #149): in a battle (wild or
  trainer) R speeds the game up and L slows it down, 1x, 1.5x, 2x, 3x and 4x,
  and a badge in the top right corner shows the speed for two seconds after
  each press.
  It is a speed of its own: until the first press the battle follows the
  SPEED the player uses in the overworld, and then keeps its own
  (`battle_speed=` in settings.txt). The bag, the party and the summary opened
  from a battle, and the field, keep the overworld's SPEED; a local wireless
  session and the measured scenarios stay at 1x. In the battle L and R no
  longer reach the game; with the game's L=A button mode L stays A and R
  alone goes round the speeds, 4x back to 1x. The soft reset,
  L+R+START+SELECT, still works. The bottom screen's focus ring and the intervals of its
  background refreshes now go by the clock, so they keep their pace at any
  speed. The quick ball moves from R to X
  (patch 0006-quick-ball); the ball button on the bottom screen is as before.

- Soft reset is now L+R+START+SELECT, as in the 3DS games, instead of the
  GBA's A+B+START+SELECT. By @eathanpage.

- Pokédex on the top screen: while the Pokédex is open on the bottom
  screen, the top shows the device itself - the red shell, lens and lights,
  a screen window - with the Pokémon of the info page, or under the list's
  cursor, at 2x over the battle terrain of the place it is met (tall grass,
  cave, water, pond, mountain; by type for one never met wild). With the 3D
  slider up the Pokémon stands in front of the screen and the terrain
  behind it. Its patch is 0058-pokedex-top-screen. By
  @darkmattericecoffee (pull request #69).

- The camera can stop at the edge of a map: on the edges chosen map by map
  (for now Routes 103 to 107 and Rustboro City), in 2D and in voxel, the
  picture no longer shows the border tiled past the edge that the wider screen
  used to bring into view; only the player walks on. Where another map joins
  part of such an edge, the camera follows the player in front of it. Every
  other map keeps the camera on the player as before. Since then the edges
  of Routes 111 to 114, 116, 117 and 119 to 121, Lavaridge, Fallarbor,
  Lilycove, Sootopolis, Jagged Pass and Mt. Chimney stop it too.

- Brazilian Portuguese, by @carlosmozart (pull request #60): the whole game in
  Portuguese over your own English ROM, as a build option (`tools/bootstrap.py
  --make --port-lang pt_br`, docs/PORTUGUESE.md): every text of the game from
  `tools/locales/pt_br` with Brazil's official names, the port's interface,
  ã/õ in every font, and the graphics with words redrawn by the port from your
  own files (icons, summary titles, POKéDEX and POKéNAV menus, contest titles,
  the title logo). Translation by Carlos Mozart (AllGenWiki). Its patches are
  0057-0066.

- Portuguese title logo: VERSÃO over ESMERALDA, ESMERALDA as the Spanish
  game's logo draws it, on its arc, and VERSÃO in the place and style of its
  EDICIÓN (`tools/logo_ptbr.py` previews it, `make verify-logo-ptbr` checks
  that the port draws it).

- German (experimental), by @DannyM90 (pull request #175, issue #135), with
  work by @andyst-dev (pull request #96): the game in German from your own
  Pokémon Smaragd-Edition (Germany) ROM, built with `tools/bootstrap.py
  --make --german-rom` (`Emerald3DS-de.3dsx`, docs/GERMAN.md). The texts,
  graphics, credits and Trainer Hill come from your ROM; the German layout
  (wider windows, nine-column naming keyboard with Ä Ö Ü, the "KP" bar,
  metres and kilograms) and the port's menus and messages are in German.
  The healthbox's "KP" bar, the length check on raw texts, the German data
  pack errors, the client/builder ROM check test and the cleanup when
  switching back to English come from andyst-dev's German pull request.

- Japanese (experimental), by @luksgrin (pull request #59): the game in
  Japanese from your own Pocket Monsters Emerald (Japan) ROM. The web and Windows builders accept it and pick the
  language from the ROM; every release carries the Japanese variant too
  (`Emerald3DS-ja.3dsx` for Quick Update). The texts, graphics, fonts,
  credits and the maps that differ come from your ROM; text is drawn in the
  game's Japanese glyphs, the naming screen has the Japanese keyboard and
  5-character names, battles draw names, levels and HP as the Japanese game
  does, the touch screen has Japanese labels and the Pokédex shows metres
  and kilograms. See docs/JAPANESE.md for what is still in English. The
  touch screen no longer crashes the Japanese game half a second after a
  save loads (wrapped text measured with the Japanese fonts' fixed width),
  by @luksgrin (pull request #200). Also by @luksgrin (pull request #201):
  the battle info boxes show the Japanese names and levels, the touch battle
  menu puts each word on its own plate, 84 more texts come from the Japanese
  ROM (the evolution message, stat words, type names, bag pockets, records,
  the summary's page titles each on its own page) and the Pokédex info and
  cry pages use the Japanese tilemaps.

- Spanish, French, Italian, German and Japanese battles say "sharply rose!" /
  "harshly fell!" with the game's own two words again instead of an English
  text; only Portuguese joins them. By @luksgrin (pull request #201).

- Locale manifests can be regenerated: `tools/locale_research/` holds the
  matcher that finds the English game's objects in another language's ROM
  and the generator of the Japanese manifest, and `rebase_manifest.py` moves
  a manifest onto a changed tree without the ROM.

## 0.4.0 — 2026-10-16

- The project is now called Emerald 3DS, by Zallax: new name on the HOME
  Menu, in the builder and in the documentation, a new developer logo under
  the title screen and a new HOME Menu banner. The forwarder CIA is 1.1.0.

- More of Hoenn in voxel: Slateport finished (market, museum, contest hall,
  shipyard, Battle Tent, boats); Mauville, Rustboro and Petalburg revised;
  first buildings in many more towns; Route 110's Cycling Road as a bridge on
  girders and the two-storey Trick House; Routes 111 to 115 in terraces;
  Rusturf Tunnel carved out of the rock; Route 109's beach; Rustboro's
  fountain rebuilt with its animated jet; Littleroot's rooms, the Pokémon
  Centers and the Mart refurnished. Still a work in progress: many parts of
  the map and most interiors stay flat, and the Battle Frontier is only an
  early start.

- HD battle interface: the info boxes and the text box are drawn at the top
  screen's resolution, the text box floats with margins, and in a 3D battle
  it slides in as the cinema bars leave. The 3D battle keeps its depth
  through stat changes, blended backgrounds and see-through moves.

- A new SAVE screen on the bottom screen (the save shown as a plate with the
  trainer, location, Pokédex, time, badges and party) and a 1.5x step in the
  speed options.

- Visible wild Pokémon reworked: they stay after a battle or a menu and
  across map borders, grow in over a rustle, a splash or dust, and only MANY
  replaces random encounters (FEW and SOME keep them).

- Battles: an evolution or an egg hatching after a battle could show a
  piece of the last battle's info box in place of the Pokémon; a box is now
  forgotten once its sprites are gone.

- Evolution and the legendaries' cutscenes, by @eathanpage (pull request
  #92): the evolution scene no longer shows a battle platform at the screen's
  left edge. Groudon and Kyogre's fight and Rayquaza's arrival fill the top
  screen, centred, instead of sitting in a corner of it; Rayquaza's descent
  shows its ray of light again, the chase has no black bars, and the bottom
  screen stays dark while they play.

- Spanish, French and Italian: Easy Chat lists the moves (and, in French,
  the Pokémon) in the language's own alphabetical order, read from your ROM,
  instead of the English one; the French link trainer card and Pokéblock
  menu use the French ROM's layout.

- The camera: past a map's corner it shows the nearest map's border, and a
  small town no longer changes the zoom.

- Voxel overworld in stereoscopic 3D on a New 3DS, by @darkmattericecoffee
  (pull request #67): with the 3D slider up the
  world is drawn once per eye from its own camera, so buildings, trees,
  terrain relief, shadows, reflections and the fog stand at their own depth;
  the player is just behind the screen and the text and weather on it. An
  Old 3DS keeps the world flat, as before (the second pass costs it the
  frame). OPTIONS' SETTINGS tab has a second screen for it: 3D WORLD
  (AUTO by console, ON, OFF) and 3D DEPTH (25% to 200% of the slider's
  depth). Build with `VOXEL_STEREO=1` or `=0` to change what AUTO does.

- The 3D battle on a real console, by @darkmattericecoffee (pull request
  #89): fixes for what only hardware showed, found with surface dumps on a
  New 3DS. Sprites no longer pick up stale tiles from the GPU's texture
  cache, new surfaces start cleared, and the voxel battle no longer goes flat
  when VRAM is fragmented, because the world's picture shares the battle
  scene's surface. A Pokémon no longer leaves a lit copy at a battle's end,
  and no lines run down the screen's sides at the stereo shift. The linear
  heap gains about 700 KiB: the bottom screen is RGB565, and the command
  buffer is 512 KiB. Follow-ups: the new surfaces are cleared two per GPU
  queue entry within the room each frame leaves, and the sprites are packed
  more tightly, so they keep their 3D beside the world's picture.

- Touch menu: the POKéDEX button shows a red Pokédex, by @luksgrin (pull
  request #91), in place of the Fame Checker: Emerald has no Pokédex item
  graphic, so it is port art mastered from Pokémon Platinum's menu icon.

- Follower Pokémon (OPTIONS > ENHANCEMENTS > FOLLOWER, off by default): the
  party's lead walks behind the player on its HeartGold/SoulSilver overworld
  sprite, comes out of and goes back into its Poké Ball (warps, surf, bike,
  rooms too low for it), and answers when talked to with its cry and one of
  HGSS's messages. The builder fetches those sprites and texts at pinned
  commits and converts them; a file it cannot get is left out and the build
  goes on. The visible wild Pokémon use the same sprites where they exist.
  Sixteen more OBJ palettes, 3DS only, give them colours of their own. Its
  patch is 0056-follower-pokemon.

- Night lamps light a round pool that brightens the ground under them, in
  place of the orange squares; on an Old 3DS they are no longer rebuilt at
  every step of the camera.

- Quick ball: the arrows reach every ball in the pocket when one kind is in
  two stacks; a change of ball redraws its plate alone.

- Voxel Petalburg: the hedges wrap the houses and the signs stand on their
  own ground.

- A battle keeps its 3D after the bag or the party is closed: the layers the
  last screen left are given back to VRAM at once.

- Code audit: the asset loaders checked against bad data, an audio worker
  race and uninitialised reads fixed, fewer needless redraws of the bottom
  screen and the battle HUD; the frame budget measures the real end of the
  frame.

- The builder rejects a modified French (BPEF) ROM, by @andyst-dev (pull
  request #62).

- Local wireless play (experimental, expect bugs) between 3DS systems nearby, by @eathanpage (pull
  request #76): the Union Room, the Direct Corner's Trade Center and
  Colosseum, the Berry Blender, the wireless minigames, link contests and the
  Battle Tower's link multi battle, two to four players, through an emulated
  Wireless Adapter over 3DS local wireless. Only identical builds link (so
  builds in different languages don't, for now); HOME is refused during a
  session. Its patch is 0055-local-wireless.

- Day/night, city lights and the berry clock, by @jesus0m (pull request
  #57), all off by default. The windows' glow and the lamps take memory
  only while they light: the masks share the voxel page arena and the lamps'
  buffers are freed shortly after the last one goes out, so with day/night
  off nothing is reserved. Known limit: at 120X the sun moves every 7.5
  seconds and each step relights the voxel world, which an Old 3DS shows as
  frame drops; at 1X it runs smoothly.

- Day/night details: cool-blue moonlit nights; warm lit windows on the
  modelled Pokémon Centers, Marts, gyms, houses, Rustboro's apartment blocks
  and Devon, and pools of light under Rustboro's street lamps; the voxel sun
  and its shadows turn with the hour; two-hour eased transitions. Berries grow
  with the clock's speed (time asleep or off stays at 1X). Optional ORIGINAL /
  TIMED grass encounters on eleven routes, shared by random encounters, Sweet
  Scent and visible wild Pokémon. An optional footer under the bottom
  screen's map shows the hour, the time band and the clock's speed. Dewford's
  exteriors are modelled from their own art.
- Spanish: clock accents, EVENING / TARDE and the party EXP text; the wild and
  foe prefixes stay as the Spanish ROM writes them.
- Building from source: a local staging pack takes its ROM fingerprint from
  the compiled game's language; CI builds on Python 3.11.

- Optional outdoor morning, day, sunset and night lighting in classic 2D and
  voxel mode, disabled by default and configurable from ENHANCEMENTS.
- Independent clock speeds from 1X to 120X, without accelerating
  Emerald's daily RTC events or modifying the GBA save format.
- Spanish labels for the touch-screen settings tabs, enhancements, cheats,
  option values and give-items menu; English labels and saved settings stay
  unchanged.

New and improved:

- Italian: the game in Italian from your own Pokémon Versione Smeraldo (Italy) ROM.
  The texts and graphics are staged from that ROM by `tools/localize_italian.py`
  (449 source files, 19 349 edits, 104 graphics, 55 credits pages, the four Trainer
  Hill floors), with the metric Pokédex units and decimal comma, the wider invalid-move
  type table, the Italian ending tilemap and the port menus in Italian.
- Italian build: `tools/bootstrap.py --make --italian-rom` builds it, next to the
  Spanish and French ones.
- French: the game in French from your own Pokémon Émeraude (France) ROM.
  The web and Windows builders accept it and pick the language from the ROM;
  every release carries the French executable (`Emerald3DS-fr.3dsx` for Quick
  Update). The texts, graphics, braille, credits and Trainer Hill come from
  your ROM; the touch screen has French labels and the Pokédex shows metres
  and kilograms. Translation and localization tooling by andyst-dev.
- Building from source: `tools/bootstrap.py --make --french-rom` builds the
  French game, next to the Spanish one.
- French port menus: translated settings, enhancements, cheats and their
  values; UTF-8 accents are converted to the game's existing font encoding.
  Includes IV/EV display, the Repel reminder, reusable TMs and indoor running.
  The font adapter also handles Spanish í/ó/ú/Í/Ó/Ú/¡/¿ and renders `&`, `,`
  and `%` in all languages (including English `TMS & HMS`).
- French data corrections: extract Latin font widths, alphabetical orders
  and default phrase IDs from the user's ROM; preserve two-column phrase
  tables, nine-column naming rows and column positions, and 22-byte type names.
  The native Pokédex uses a decimal comma and French measurement alignment.
- ENHANCEMENTS: four new options, all off by default, by @Deunnis (pull
  request #49). IVS AND EVS: A (or a tap on the stats) on the summary's
  SKILLS page shows the Pokémon's IVs, then its EVs. REPEL PROMPT: when a
  Repel wears off and the Bag still has one, "Use another REPEL?". INFINITE
  TMS: a TM is kept after teaching, like an HM. RUN INDOORS: the Running
  Shoes work in houses, Centers, gyms and caves. HMS WITHOUT MOVE now covers
  Fly and Flash too: HM02 and HM05 used from the Bag offer to use the move,
  and cancelling the Fly map goes back to the Bag; the new texts are in
  Spanish in the Spanish game (pull request #52).
- 3D BLUR has a strength, by @Deunnis (pull request #55): OFF or 10% to 100%
  in steps of 10 (tap the left half to lower it, the right half to raise it)
  instead of on and off. An old `voxel_blur=1` reads as 100%.
- Building from source: the build fails when a song header has a pointer
  missing from the relocation table, as `se_rain` had in 0.3.0's Spanish
  build (the rain crash of issues #40, #44 and #46), by @Deunnis (pull
  request #53).

- OPTIONS > ENHANCEMENTS has REAL-TIME PACING (off by default): the sound
  engine mixes on the DSP's clock instead of once per shown frame, so a
  frame the console takes too long over (a town loading, a busy scene in 3D)
  no longer makes the music click; and the game keeps real time, running a
  hidden frame when it has fallen a frame behind the display, so only the
  picture skips a step. The FPS counter shows pictures/game frames a second.
  In the field only: a battle keeps the GBA's timing. Without a DSP (an
  emulator without its firmware) the engine still runs once per frame.

Fixed:
- The battle intro could hang with the player's trainer sliding up the
  screen for ever: the HD interface's lift of the trainer (down with the
  text box) left an odd y2 on the sprite when the slide across ended first,
  and the game's vertical slide, two pixels a frame, never reached 0. The
  lift is kept even.

- Battles: with the speed multiplier at x2 and above, a Pokémon sent out of
  its Poké Ball (yours or a trainer's) made no cry. The game cleared the cry
  before the sound engine had started it.
- Saves: a save backed up from a cartridge or exported from mGBA (the 128 KiB
  flash image followed by the clock's bytes) was refused, which left only
  NEW GAME; it is read now, by @Deunnis (pull request #47, issue #33).
- Battles: move backgrounds (Confusion's, Solar Beam's, the impact one...)
  covered only part of the wide screen, with leftover scenery beside them;
  they fill it now, by @Deunnis (pull request #54, issue #51).
- The Pokédex page of a Pokémon just caught was laid out as the battle,
  zoomed and cut off; it is shown whole, by @Deunnis (pull request #50,
  issue #29).
- Voxel: the potted plant in the small houses left pixels of its outline on
  the floor, by @Deunnis (pull request #48, issue #23).
- Battle transitions: the Poké Balls rolling across the screen (trainers on
  routes) and the other transitions that draw only their own layer over the
  field were a GBA-sized box in a corner of the screen; they fill it now, at
  the battle's scale.
- Pokémon Centers: the balls on the healing machine and the flash of its
  monitor were drawn 8 pixels too high, above the tray and the screen.
- Hatching an egg: the scene was drawn as a wide screen, its text box and
  backgrounds repeated across a black screen, and the spotlight under the
  Pokémon was noise. The trade animation's GBA had the same noise.
- The cable car: the ride sat in a corner of the screen with black around
  it; it fills the screen, the scene mirrored into its edges and faded
  like the intro's.
- Weather: the rain, the volcanic ash, the sandstorm and the underwater
  bubbles fell over a GBA-sized part in the middle of the screen; they cover
  all of it now, as the fog already did, the cable car's ash included.
- People on the map could be drawn over the whole screen for a frame as
  they or the player moved (the old lady in Route 111's rest house): they
  were taken for the fog.
- Battles: after opening the bag or the party in a battle, a stat change
  (Growl, Defense Curl and the like) covered the whole screen with its
  pattern instead of only the Pokémon.
- Voxel mode: Rustboro's and Mauville's streets no longer stand up in low
  blocks here and there (Rustboro's main road among them). Their paving is
  the same tile as the middle of the white curbs round lawns and landings,
  and was raised as curb.
- Voxel mode: Rustboro's fountain is built again from its drawing: the rim
  stands over the water, the dish sits on a stem in the middle, and its walls
  are solid all round (one side was a dark slab and another see-through). Its
  jet plays the game's animation, as on the GBA.

## 0.3.1 — 2026-10-10

New and improved:

- A new icon and banner on the HOME Menu, for the forwarder CIA and the
  3DSX.

Fixed:

- Taking a fossil in the Mirage Tower crashed the game when the tower
  started to shake (the tower's graphics overran their buffer).
- Hyper Beam could crash a battle (an original game bug that picks an orb
  animation that does not exist; harmless on the GBA, a crash on the 3DS).
- Shops: the BUY screen showed a black background and sat in the top-left
  corner of the screen. It is on the bottom screen now, on the same green as
  the bag you sell from, and works by touch: tap an item to pick it and again
  to buy it, drag the list to scroll it, drag to change how many, tap YES or
  NO. The top screen keeps the world, in 2D and in voxel mode.
- Battles: the stats box shown on a level up was hidden behind the text box
  and ran off the bottom of the screen; it is drawn over the text box again,
  whole.
- Graphics and data the 3DS read from the wrong place (the same cause as the
  Mirage Tower crash): Sootopolis's drought weather no longer fades the
  screen to black; Trainer Hill floors have their maps and walls (by
  @Trukitro, pull request #25); Spinda has
  its spots; the evolution background, the Pokéball, mugshot, Rayquaza and
  grid battle transitions, the PP colours, Pokédex footprints and area glow,
  the contest applause meter, turn numbers and results window, the Secret
  Base decoration preview, the Match Call frame, the underwater map name
  frame, the Pokénav condition cancel icon, the Battle Factory screens, the
  Trainer Hill records, the Battle Pyramid floor colours and the slots' Reel
  Time window are drawn as on the GBA.

## 0.3.0 — 2026-10-08

New and improved:

- Spanish: the game in Spanish from your own Pokémon Esmeralda (Spain) ROM.
  The web and Windows builders accept it and pick the language from the ROM;
  every release carries both languages (`Emerald3DS-es.3dsx` is the Spanish
  executable for Quick Update). The texts, graphics, braille, credits and
  Trainer Hill come from your ROM; the touch screen has Spanish labels and
  the Pokédex shows metres and kilograms. Translation work by Jesus Oliva
  (@jesus0m, pull request #6).
- OPTIONS has tabs: SETTINGS (as before), ENHANCEMENTS and CHEATS, all off
  by default, by @papacult (pull request #11). ENHANCEMENTS: SPEED 1x-4x
  fast-forward (also ZR/ZL on New 3DS, music and sound at normal speed),
  visible wild Pokémon that wander the grass, caves and water instead of
  random encounters, EXP for catching, the modern Exp Share, trade
  evolutions at Lv. 40, field HMs with the badge alone. CHEATS: wild
  encounter rate, shiny odds, every Poké Ball catches, instant victory, fast
  egg hatching, infinite money, Pokédex (Hoenn full, National on or full)
  and GIVE ITEMS.
- Quick ball in wild battles, by @papacult (pull request #11): a button
  beside FIGHT (or R) throws the last ball used without opening the bag.
- Soft reset (A+B+START+SELECT) restarts the game as on the GBA instead of
  crashing, by @papacult (pull request #12).
- A visible wild shiny sparkles more often, so it can be told apart, by
  @Trukitro (pull request #20).
- Bottom screen redrawn: an emerald button column with the game's icons, a
  Y button for the registered item (the console's Y is now SELECT) and a
  RUN toggle that makes running the default (B walks); X puts a focus ring
  on the column for the D-pad; the options in a 2x6 grid, the map in a
  recessed frame with its area name, and one light green background behind
  the game's own screens.
- Battle bottom screen redrawn: plates in the DS/3DS style over a teal
  backdrop, FIGHT with Rayquaza's silhouette, the quick ball, the party's
  six balls, moves in their type's colour with PP in the game's warning
  colours; the screen fades in as a battle starts, and the menus no longer
  drop frames.
- Battles in 3D with the 3D slider: the scenery behind the screen, each
  side's Pokémon at its own depth and the text box at the screen, in 2D
  battles and in 3D BATTLE.
- 3D BATTLE: the camera's flight from the field to the battle is a
  cinematic entrance (crane, dolly zoom, landing with a short quake, speed
  lines, flash and cinema bars).
- Flash: caves that need it (and the Battle Pyramid) now show their
  darkness and the circle of light around the player: the GBA's own circle
  in 2D, a soft, flickering pool of light in voxel.
- The title screen shows the developer's logo and the game's version under
  it.
- Voxel (experimental): mountains on Routes 104, 105, 106 and 116 rebuilt
  level by level on the tile grid, and Route 106 joined to Dewford.
- Voxel: sliding doors (Pokémon Centers, Marts, gyms) open as you walk
  through them, by @Trukitro (pull request #17).
- Voxel: more rooms modelled, by @Trukitro (pull request #19): the house
  with a bed, Mr. Briney's house, the Pretty Petal flower shop; the walls of
  21 rooms of the general indoor tileset and Rustboro's school; doorways for
  the stairs of Rustboro's flats, the Devon Corporation, Lilycove's motel,
  the Fossil Maniac's house and the Trick House; Petalburg's gym walls and
  Dewford's gym maze. No furniture is guessed from tile ids any more.
- Voxel: a ledge jump is drawn as an arc, by @Trukitro (pull request #16).
- Voxel: the surf mon lies on the water under the rider, bobbing, with
  shadows; Dewford's small tree is modelled.
- Much less stalling on Old 3DS: a fixed-point sound mixer, faster copies
  and decompression, a performance pass over the voxel field, 3D battles
  and the bottom screen, and the furniture checks of towns done once
  instead of every frame.
- Builder: the scenery generation takes about half as long, with the same
  pack.
- Building from source: `tools/bootstrap.py --make --spanish-rom` builds the
  Spanish game, and a clean tree now builds in one go (from pull request #6,
  Jesus Oliva).

Fixes:

- Stat changes in battle showed no animation, only their sound (2D and
  3D BATTLE).
- Drums played about three times too fast and too high; cymbals, triangles
  and bells now sound as on the GBA.
- The evolution scene showed half a Pokémon on a black screen, by
  @Trukitro (pull request #16).
- The poison step's flash on the field was not drawn; a violet veil stands
  in for it in 2D and voxel, by @Trukitro (pull request #16).
- Voxel: walkers were hidden by furniture against the back wall, by
  @Deunnis (pull request #9, issue #4), and by the Pokémon Center's PC, by
  @Trukitro (pull request #16).
- Voxel: rug borders and wall tops stood up as television sets, beds and
  mats in some rooms, by @Trukitro (pull request #19).
- Voxel: leaving a 3D BATTLE could fall back to 2D.
- The bag's item icon was drawn off its box, under the list.

## 0.2.0 — 2026-10-04

New and improved:

- The game's own menus on the bottom screen, by touch: the party menu (with
  the summary's move screens), the bag, the Pokédex and the PC boxes are the
  original screens, built as a centred picture with the background carried
  to the edges; every action of the originals is reachable by tapping, and
  they run at 60 fps on Old 3DS.
- 3D BATTLE option (off by default, needs VOXEL 3D): battles are drawn in
  front of the voxel world, on level ground found near the player, with the
  camera gliding in from the field's.
- The FPS counter is now the SHOW FPS option of the bottom screen's OPTION
  list: off by default, turn it on there (it used to be always shown).
- Voxel (experimental, still full of errors and under active work): HD-2D look - tilt-shift blur at the top and bottom of the screen
  (3D BLUR option), bloom, sunlit dust, natural sunlight with richer colour,
  sun rays, soft sun dapples that stay on the ground across map crossings,
  lighter tree crowns and softer shadows.
- Voxel: mountains in their true tile shapes, with cut tiles, rock edges that
  follow the rock and flanks that rise across their drawn run; sea rocks and
  boulders modelled once wherever they repeat; Jagged Pass, Mt Chimney and
  Lavaridge read as mountains; Route 106's ledge apart from its plateau.
- Voxel: fog banks lying over the ground and thinning into the distance, and
  a cold, dim light with a dark ring in caves; in 2D the fog covers the whole
  screen and is see-through.
- Voxel: water lies flush with the ground (no line along the shores), tall
  grass rustles on its own tile, the surf mon, grass, "!" icons and splashes
  are drawn, and sprites stand in front of stairs.
- Much less stalling on the console: assets read ahead by a background
  worker, the sound mixer on another core, unchanged layers not walked, New
  3DS at 804 MHz with L2; the voxel frame scheduling, frustum view and chunk
  builds reworked for Old 3DS, draft chunks filling holes at once, animated
  tiles uploading only what changed.
- Builder about 2.5x faster (a full build takes about 6.5 minutes instead of
  15-17) with a byte-identical pack.
- Releases also carry `Emerald3DS.3dsx` and `Emerald3DS.smdh` on their own
  (quick update) and `Emerald3DS-WebPayload.zip` with `web-manifest.json` for
  the web builder; the builder can run the voxel generators in-process (used
  by the web builder), and the Windows ZIP's `LICENSES/` folder again
  includes `LICENSE-PORT.md`, `NOTICE.md` and `AI_DISCLOSURE.md`.
- Optional HOME Menu forwarder, `Emerald3DS-Forwarder.cia`: installed once
  with FBI, it starts the game's 3DSX through Luma3DS without needing the
  Homebrew Launcher title, so updates only replace the 3DSX. The game's own
  icon is the new project icon.
- The SD log is only written when `sdmc:/3ds/emerald3ds/debug.txt` exists;
  `port.log` holds the current session and `port-prev.log` the previous one.

Fixes:

- Voxel: the overworld crashed on the console as soon as the sun dapples
  drew (a null texture unbind that the emulator let pass).
- Voxel: freezes after battles, chunks that stopped loading near Mauville and
  the overworld falling back to 2D on Old 3DS; each option tap froze the game
  for about a second (settings are now saved by their own thread).
- Voxel: sun dapples jumped at every map crossing.
- Voxel: grass floated between tufts and walkers hopped in it.
- Voxel: black gaps at the edges of the view after a crossing.
- 2D: the Surf, Fly and field-move banner left the rest of the field black.
- 2D: Match Call turned the map to garbage while a call lasted.
- 2D: the map name popup peeked in at the bottom of the screen as it slid
  away.
- Items shown by the PC and the like fell outside their frame, and the
  healing machine's balls were not on the machine.
- Graphics that were read past their stubs and never showed: PC wallpapers,
  party menu and summary tilemaps, Deoxys's form icon.
- The starter choice screen was drawn at the top-left of the top screen with
  a teal backdrop beyond the meadow; it is now centred 1:1 with the meadow
  carried to every edge, and the left starter's label (Treecko) keeps its
  darkened box.
- The forwarder hung on a black screen.

## 0.1.2 — 2026-09-30

New and improved:

- PokéNav on the bottom screen, driven by touch: laid out as header, body
  and help bar, a tap moves the cursor straight to the option or entry, and
  the Hoenn map scrolls smoothly.
- 3D slider: the intro and the title screen now have stereo depth, and 2D
  screens keep blended sprites and text windows at their own depth.
- Battle transitions now play in both the classic 2D field and the voxel
  overworld, at full speed on Old 3DS.
- Voxel: the world fades with the screen on warps and battle starts.
- Voxel: characters are drawn at their proper proportions, stand on their
  feet and meet their shadow; the walking bob is kept.
- Voxel: walls and mountain sides facing away from the sun are in shade,
  lit by the same sun as the cast shadows.
- Voxel: newly modelled Rustboro's gym (its statues stand in all eight gyms),
  Oldale's two houses, Mr Briney's cottage and the Pretty Petal flower shop.
- Voxel: every mountain of the general tileset is drawn; Route 116's
  mountain is built from straight faces and one-level steps; stairs, lamps
  and Rustboro's railings stand again.
- Voxel: Route 104's beach and sea are a level below the route, and Routes
  105 and 106 follow them down.
- Intro, title and menus: the margins around the 240x160 picture are made
  from each screen's own art (Rayquaza stands on the bottom edge under a lit
  sky, the Professor's sky reaches the top), and menu screens are centred.
- Title screen and palette fades much smoother on Old 3DS: layer textures
  are redrawn only where they change, and fades are drawn as a tint.
- Installation guide in `docs/INSTALLATION.md`, and bug report and feature
  request forms on GitHub.

Fixes:

- PokéNav tiles and palettes were wrong.
- With the 3D slider up, black screens came out blue and the save selection
  lost its backdrop; the intro's leaves scene slowed down.
- Voxel: sprites went black on their own during warps and battle starts.
- Voxel: cast shadows striped and came apart from the feet.
- Voxel: black gaps along the shores of recessed water (now a rim).
- Voxel: ledges were sunk into a toothed trench; they now sit on the ground
  as on Route 101, and side ledges are no longer striped by the lighting.
- Voxel: Rustboro's lamps against walls did not stand.

## 0.1.1 — 2026-09-27

- The voxel overworld is now off by default. It is switched on from the new
  **VOXEL 3D** row of the bottom-screen OPTION screen.
- While it is on, **3D ANGLE** (34-46 degrees, default 40) and **3D ZOOM**
  (90-120%, default 100%) adjust its camera.
- These settings are kept in `/3ds/emerald3ds/settings.txt` and apply at once,
  without saving the game.
- Classic 2D field: text boxes, prompts and the map name are centred as in
  the voxel view.
- Classic 2D field: much faster on Old 3DS; the backgrounds are kept in
  textures and only the cells that change are redrawn.
- Classic 2D field: sprites just below the view are no longer drawn at the
  top of the screen.

## 0.1 — 2026-09-26 — first public version

- Native ARM11 port of pokeemerald for Nintendo 3DS: GPU compositor at
  native 400x240, NDSP audio, touch and Circle Pad input, SD saves.
- Bottom-screen interface replacing the START menu (map, party, bag, trainer
  card, Pokédex, PokéNav, save, options) and touch battle menus.
- Optional voxel overworld with buildings, trees, signposts and terrain relief
  modelled from each map's own art, fixed-sun lighting and cast shadows.
  The modelled part is still limited: most of the map and nearly all
  interiors are shown flat for now.
- Game data outside the executable: embedded (development), loose files or a
  single `emerald3ds.pak` with ABI and integrity checks.
- Pokémon Emerald 3Ds Dual Screen Builder: generates the data pack from the player's own ROM and
  installs the game on an SD card, with no toolchain or Python required on
  Windows.
