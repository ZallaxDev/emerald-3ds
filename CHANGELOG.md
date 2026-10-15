# Changelog

## Unreleased

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

- Local wireless play between 3DS systems nearby, by @eathanpage (pull
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

- Dewford exterior models and the bounded window-mask pool
  have user-reported physical-3DS acceptance. Follow-up fixes give the caught-mon
  Pokédex its own clipped, scroll-isolated bottom-screen layout and recover the
  gym-side sign from its original two-cell artwork. The user confirms the final
  preview on a physical 3DS and authorizes publication on October 12, 2026.

- User-tested refinement: brighter, cool-blue outdoor
  nights with a moonlit ambient blend capped at 6%, eased with the existing clock.
  Reuse the 2D/voxel texture stages without extra draw calls or assets; keep warm
  window/lamp emission, neutral interiors/UI and complete black/white fades.
- Local staging packs now select their ROM fingerprint from the compiled game
  language, rather than marking Spanish data as English. An explicitly supplied
  ROM must match that language; synthetic tests cover both profiles and rejection.
- City lighting (user-reported 3DS acceptance): extend authored voxel
  glass masks to shared Pokémon Centers, marts and gyms, Rustboro apartment
  blocks and Devon; correct Littleroot east-house window coordinates.
  Original Rustboro lamps emit warm glass and bounded floor-only light pools,
  cached with the visible map state and faded by the existing shared clock.
  Unmodelled buildings and classic 2D windows remain outside local emission.
- CI: remove Python 3.12-only f-string expressions from asset-table generation
  so the Python 3.11 build container can bootstrap. Add URI/offset regression
  tests in that container and update voxel fixtures for tileset-aware tree APIs,
  atlas dimensions and clock updates. Package CI's engine-only ZIP under the
  existing allowed `payload/` layout without weakening publication checks.
- Experimental follow-up: berry growth shares clock acceleration, with
  fractional bonus-minute carry, continuous speed changes and save-bound clock
  persistence. Sleep/offline time stays at 1X; daily RTC events remain original.
- Correct Spanish clock accents, add display-only EVENING / TARDE and
  translate modern party EXP text; the wild and foe prefixes stay as the
  Spanish ROM writes them.
- Experimental follow-up: clock-driven voxel sun and cast shadows, with
  15-game-minute direction updates through the existing chunk budget,
  retained caster caches, and no additional shadow rendering pass.
- Experimental follow-up: two-hour eased visual transitions and authored
  amber window emission on shared exterior voxel houses (including Petalburg),
  disabled with day/night and excluded from interiors. No new encounter bands.
- Opt-in ORIGINAL / TIMED grass encounters on eleven Hoenn routes, shared
  by random encounters, Sweet Scent and new visible wild spawns, with original
  levels/rates and unchanged water, caves, facilities and special encounters.
- Optional bottom MAP footer showing effective HH:MM, time band and clock
  rate, independently of lighting, localized in Spanish and partial-redrawn.

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
- Stereoscopic Pokémon in battle: an opponent whose species has depth planes
  is drawn as its stack of planes where its sprite stands, with the sprite's
  own palette (so its fades and tints are kept), each plane at its depth with
  the slider, never turned or swayed, so its pixels stay crisp. Move effects pass in front
  of it, the intro's curtain over it; each plane carries a skirt of the
  planes behind it so no gap opens between them; the
  moment the sprite is not the plain
  picture (a Substitute, Transform, the slide of a faint) the sprite itself
  is drawn as before. OPTIONS' SETTINGS 2/2 has 3D BATTLE MONS (ON/OFF);
  3D DEPTH scales the stack's depth as it does the world's.
  CHEATS has 3D WILD ONLY: every wild Pokémon met is one with planes, to try
  it anywhere.
- The player's side of the battle in 3D too: the Pokémon's back sprites,
  the trainers' front sprites in the intro and the player's own back with
  its throw are drawn as their planes, from a second collection
  (`stereo/backs.bin`, `scripts/gen_stereo_backs.py` from the authored
  back-and-trainer work; `STEREO_BACKS` in `local.mk`). Up to four
  pictures share the planes texture: eight planes each for two, four each
  in a double battle.
- The battle's message box in 3D: a panel a little in front of the screen,
  slightly narrower than it and centred over dark gutters, so the Pokémon
  in front of the scene are not walled in by it.
- The planes collection now covers all 385 Hoenn-dex species (two poses
  each; Castform's forms, Unown's other letters and Spinda's spots keep the
  plain sprite). A finished pack is built in with `STEREO_PACK` in
  `local.mk`.
- 3D battle on a New 3DS: a move that blends with the scene (a stat change's
  glow, a screen flash) no longer flattens the Pokémon onto the scenery for
  as long as it lasts; the frame is composed per eye instead, as the voxel
  battle's is.
- Voxel overworld in stereoscopic 3D on a New 3DS: with the 3D slider up the
  world is drawn once per eye from its own camera, so buildings, trees,
  terrain relief, shadows, reflections and the fog stand at their own depth;
  the player is just behind the screen and the text and weather on it. An
  Old 3DS keeps the world flat, as before (the second pass costs it the
  frame). OPTIONS' SETTINGS tab has a second screen for it: 3D WORLD
  (AUTO by console, ON, OFF) and 3D DEPTH (25% to 200% of the slider's
  depth). Build with `VOXEL_STEREO=1` or `=0` to change what AUTO does.

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
