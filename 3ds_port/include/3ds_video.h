#ifndef CTR_VIDEO_H
#define CTR_VIDEO_H
#include <stdbool.h>
#include <stdint.h>

/* Reserve a voxel transfer (at most a split and a copy) before touching its
 * destination. False means retain the completed CPU job for the next frame. */
bool CtrVideo_TryVoxelUpload(void);
/* How many more uploads TryVoxelUpload would grant this frame. */
unsigned CtrVideo_VoxelUploadsLeft(void);
/* The same for the animated tiles' copies, from room of their own. */
bool CtrVideo_TryVoxelAnimUpload(void);
/* In a frame: GX_TextureCopy calls between these go behind one split of its
 * command list, with no heap flush each (their sources flushed already). */
void CtrVideo_CopiesBegin(void);
void CtrVideo_CopiesEnd(void);

#define CTR_GAME_WIDTH 400
#define CTR_GAME_HEIGHT 240

/*
 * A GBA stage keeps the native pixel grid: the 240x160 picture sits 1:1 in the
 * middle of the 400x240 screen, and the compositor fills the space around it
 * from the picture's own art (see DrawStageBg in 3ds_video.c).
 */
#define CTR_STAGE_X ((CTR_GAME_WIDTH - 240) / 2)
#define CTR_STAGE_Y ((CTR_GAME_HEIGHT - 160) / 2)
/* The battle scene: centred across, resting on the bottom edge. */
#define CTR_BATTLE_X CTR_STAGE_X
#define CTR_BATTLE_Y (CTR_GAME_HEIGHT - 160)
/*
 * The battle scene itself is magnified: the middle of its bottom edge (GBA
 * 120,112) sits on screen (200,192), on top of the 1:1 text box. At 1.5 the
 * tile grid still lands on whole screen pixels.
 */
#define CTR_BATTLE_ZOOM 1.4f

typedef struct
{
    const uint8_t *vram;
    const uint16_t *palette;
    const uint16_t *oam;
    const uint16_t *regs;
} CtrVideoMemory;

typedef struct
{
    uint32_t frames, tiles, uploads, sprites, errors, stereo;
    /* Layer cells drawn through the compositor's own program (FastCells). */
    uint32_t cells;
    float fps, cpuMs, gpuMs, waitMs;
    /* Of the last frame: when the GPU was set to work, from the frame's start
     * (its early start, or FrameEnd); whether the voxel world was updated. */
    float gpuStartMs;
    bool voxelFrame;
    /* For the frame profiler's records (3ds_prof.c), of the frame just made:
     * the time since the previous FrameBegin and when the CPU got back to it;
     * the preparation before FrameBegin; the voxel stereo's depth read and
     * its two eyes. */
    float gapMs, arriveMs, prepMs, stereoSampleMs, stereoEyesMs;
    /* What the frame was (CTR_MODE_*), how it was drawn (CTR_ROUTE_*), the
     * slider asked for in percent, and why it was less than that
     * (CTR_FLAT_*: flattened groups, or no 3D at all). */
    uint8_t mode, route, sliderPct, flat;
    /* The frame was the voxel world's warm-up black (route CTR_ROUTE_BLANK
     * for it), which holds the field's fade in (3ds_game_full.c,
     * HoldFieldFade). Not the black of a frame whose voxel world was not
     * drawn while the 2D terrain was left stale (FieldCamera_TerrainStale). */
    bool warmupBlack;
    /* Where in the frame the GPU was set to work before FrameEnd
     * (CTR_EARLY_*, 0: at FrameEnd), and the surfaces a consumer wanted and
     * went without this frame (CTR_DENIED_* bits): the dispatch and the
     * residency of the engine reform's stage 1. */
    uint8_t gpuEarly, denied;
    /* Of the frame just made, for perf.bin v6: the time in the layer walks
     * and in the sprites' (with the slider up, both eyes' and the slots'),
     * why its scenery and its text were composed or kept (CTR_SCENE_WHY_*,
     * CTR_TEXT_WHY_*, as bits), why the slots' regions were composed again or
     * could not be kept (bits, 3ds_video.c: SLOTS_AGAIN_*, SLOTS_UNKEPT_*), and
     * the nocache bits a test build had off (BATTLE_TOGGLES). */
    float bgMs, objMs;
    uint16_t sceneWhy, textWhy;
    uint8_t slotsAgain, slotsUnkept;
    uint32_t toggles;
} CtrVideoStats;

/*
 * Why a battle frame's scenery was composed, or that it was kept: bits of one
 * frame (all its composes together), in perf.bin v6 and by bit in the PERF seg
 * lines. First the parts of the kept scenery's key that differ, in the order
 * of 3ds_video.c's SCENE_PART_*; with the key the same, what the check after
 * it found. Bits say what was seen, not one cause: over the world a frame can
 * show a part of the key before the fill, and NEW is only set alone.
 */
enum
{
    CTR_SCENE_WHY_REG,      /* the key: registers and the view */
    CTR_SCENE_WHY_SCROLL,   /* the key: the layers' scroll */
    CTR_SCENE_WHY_LINES,    /* the key: the line by line tables */
    CTR_SCENE_WHY_DRAWABLE, /* the key: what the layers decide beyond the registers */
    CTR_SCENE_WHY_VERSION,  /* the key: the scenery's published version */
    CTR_SCENE_WHY_KEY,      /* the key, in no part: a term outside them (the OBJ window slotted) */
    CTR_SCENE_WHY_PALETTE,  /* a palette bank a layer names changed */
    CTR_SCENE_WHY_STAMP,    /* the stamps of everything else: other BG palettes, tile banks */
    CTR_SCENE_WHY_VRAM,     /* a layer's map or the characters it names */
    CTR_SCENE_WHY_STILL,    /* a layer's measures (StillHolds) */
    CTR_SCENE_WHY_PATH,     /* not a keepable frame: bitmap mode, the OBJ window, composed flat */
    CTR_SCENE_WHY_NEW,      /* nothing kept under this key */
    CTR_SCENE_WHY_SLOTS,    /* over the world: the slots took the scenery's columns */
    CTR_SCENE_WHY_KEPT,     /* a scenery was kept */
    CTR_SCENE_WHY_COMPOSED, /* a scenery was composed */
    CTR_SCENE_WHY_LAYER,    /* the blending layer apart was composed */
    CTR_SCENE_WHY_COUNT
};

/*
 * The same of the 3D battle's text pass, composed once for both eyes: the
 * parts of its key that differ, its pictures, the stamps and VRAM after it.
 */
enum
{
    CTR_TEXT_WHY_REG,       /* the key: DISPCNT, the text layers' registers, windows, blending */
    CTR_TEXT_WHY_VIEW,      /* the key: the view, the clip, the passes */
    CTR_TEXT_WHY_PALETTE,   /* the key: the banks the text names, and the fade */
    CTR_TEXT_WHY_BOX,       /* the key: where the HD box is, hidden or sliding */
    CTR_TEXT_WHY_PICTURES,  /* the HD box's pictures */
    CTR_TEXT_WHY_BG_STAMP,  /* sBgOtherStamp */
    CTR_TEXT_WHY_BANK_STAMP,/* sTilemapBankStamp */
    CTR_TEXT_WHY_VRAM,      /* the text layers' maps or characters */
    CTR_TEXT_WHY_FADE,      /* the frame was fading: the reasons above come with it */
    CTR_TEXT_WHY_PATH,      /* not keepable: over the world, no text kept, line effects, the OBJ window */
    CTR_TEXT_WHY_KEPT,      /* the text was kept */
    CTR_TEXT_WHY_COUNT
};

/*
 * Each queue drain of a submission (QueueDone), for perf.bin v6: when it came
 * and the last point the frame had queued everything up to then, so that a
 * drain says all of that was done by its time. A point stands only after a
 * split: what is recorded and not yet split is not in the queue. The drain
 * in which citro3d swapped the top screen has CTR_STAGE_SWAP too.
 */
enum
{
    CTR_STAGE_NONE,     /* nothing marked yet: uploads, the frame's first lists */
    CTR_STAGE_LAYERS,   /* the layer textures and the OBJ window */
    CTR_STAGE_WORLD,    /* the voxel world's geometry */
    CTR_STAGE_POST,     /* the world to the screen: blur, glow, shadows, curtain */
    CTR_STAGE_SCENE,    /* the battle's scenery */
    CTR_STAGE_LOGICAL,  /* the logical surface: the text box, with the slots in 3D */
    CTR_STAGE_END,      /* FrameEnd reached: the eyes and the transfers */
    CTR_STAGE_COUNT
};
#define CTR_STAGE_SWAP 0x80u
/* A submission's drains kept by name: the first ones and always the last. */
#define CTR_DRAIN_LIST 12

/* Where the GPU was started early (GpuStartEarly), from the first point of
 * the frame that had work queued for it. */
enum
{
    CTR_EARLY_NONE,         /* at FrameEnd, as Citro3D does */
    CTR_EARLY_LAYERS,       /* the layer textures and the OBJ window, before the 2D composition */
    CTR_EARLY_SCENE,        /* the 2D battle's scene, before its text box and boxes */
    CTR_EARLY_EYE,          /* the logical surface composed, before its blit and the other eye */
    CTR_EARLY_PLANE,        /* the furthest depth plane, before the nearer ones */
    CTR_EARLY_TRANSITION,   /* a battle transition's field, before its own picture */
    CTR_EARLY_VOXEL,        /* the voxel world drawn (field, transition) */
    CTR_EARLY_BATTLE,       /* the 3D battle's scenery and the battle over the world */
    CTR_EARLY_COUNT
};

/* A consumer that wanted its surface (or the voxel world its VRAM) and went
 * without it this frame: the tile walk, flat 3D, the scene composed whole. */
enum
{
    CTR_DENIED_LAYER = 1 << 0,  /* a layer texture: its background walked tile by tile */
    CTR_DENIED_PLANE = 1 << 1,  /* the depth planes of a 2D screen in 3D */
    CTR_DENIED_BOTTOM = 1 << 2, /* the bottom screen's surface */
    CTR_DENIED_SCENE = 1 << 3,  /* the battle scene's surface */
    CTR_DENIED_KEEPER = 1 << 4, /* the surface keeping a 2D battle's scenery */
    CTR_DENIED_BLEND = 1 << 5,  /* the 2D battle's blending layer */
    CTR_DENIED_SLOTS = 1 << 6,  /* the 3D battle's sprite slots */
    CTR_DENIED_VOXEL = 1 << 7,  /* the voxel world's arenas, wanted and not claimed: the world
                                 * parked (the field in 2D), or up without its
                                 * building pages (the houses drawn flat) */
    CTR_DENIED_KINDS = 8
};

/*
 * What CtrVideo_Present did in one game frame, for the frame profiler
 * (3ds_prof.c), taken once a frame (CtrVideo_TakeFrameReport) and cleared
 * then: a frame that did not present reports nothing, instead of the last
 * presentation's figures over again. Every GPU submission (one C3D frame)
 * has an id; the drawing time FrameBegin reads belongs to the submission
 * before it, and says so (gpuOf), so that it is never paired with the wrong
 * frame's start.
 */
enum
{
    CTR_PRESENT_NONE,       /* Present not called (a hidden frame), or FrameBegin refused */
    CTR_PRESENT_HELD,       /* the top screen held: nothing new drawn up there */
    CTR_PRESENT_DRAWN,      /* a frame drawn and submitted */
};

typedef struct CtrVideoFrameReport
{
    uint8_t kind;           /* CTR_PRESENT_* */
    /* The first picture after a held top screen or a resume: the interval to
     * the picture before spans that time, and is no frame interval. */
    bool boundary;
    uint32_t image;         /* a drawn frame's submission, 0 = no new picture */
    /* What the CPU waited for in Present (FrameBegin, or the VBlank while held). */
    float waitMs;
    /* Read at this frame's FrameBegin: the drawing time of submission gpuOf
     * (0 = none read), and when that submission set the GPU going, from its
     * own frame's start. */
    uint32_t gpuOf;
    float gpuMs, gpuStartMs;
    /* When submission gpuOf's queue last drained, which is when citro3d
     * swapped the top screen for it (from its frame's start; 0 = no swap
     * seen), and the refreshes the top screen repeated before it, by the
     * VBlanks its swap and the submission's before took effect at
     * (shownKnown false: not known - one of the two swapped no top screen,
     * or a held top screen or a resume came between their submissions). */
    float gpuDoneMs;
    uint8_t shownLost;
    bool shownKnown;
    /* Its swap took effect at the same VBlank as the submission's before: that
     * picture never reached the screen (shownKnown is false then). */
    bool shownTwice;
    /* Submission gpuOf's queue drains: how many (of them at most
     * CTR_DRAIN_LIST kept, the last in the last place), each as ticks from its
     * frame's start and its CTR_STAGE_*; and the drains since the last report
     * that found the ring full. */
    uint8_t drains, drainsLost;
    uint8_t drainStage[CTR_DRAIN_LIST];
    uint32_t drainTicks[CTR_DRAIN_LIST];
} CtrVideoFrameReport;

void CtrVideo_TakeFrameReport(CtrVideoFrameReport *out);
/* Back from sleep or the HOME Menu: the next picture's interval spans it. */
void CtrVideo_NoteResume(void);

/*
 * Per-frame counts of GPU work (the linker wraps of citro3d, 3ds_gpumem.c),
 * read and cleared by the frame profiler: draw calls, render target
 * switches, and copies/transfers queued. `targets` counts C3D_FrameDrawOn,
 * which C2D_SceneBegin calls with the target unchanged too; `switches` the
 * changes SceneOn really made. `vertices`: those the draw calls named.
 */
typedef struct
{
    uint32_t draws, targets, copies, switches, vertices;
} CtrGpuCounts;
void CtrGpuCounts_Take(CtrGpuCounts *out);

enum
{
    CTR_MODE_OTHER,         /* menus and every other 2D screen */
    CTR_MODE_FIELD,         /* the 2D overworld */
    CTR_MODE_FIELD_VOXEL,   /* the voxel overworld (or its warm-up) */
    CTR_MODE_BATTLE,        /* the 2D battle */
    CTR_MODE_BATTLE_VOXEL,  /* the battle over the voxel world */
    CTR_MODE_STAGE,         /* intro, title, credits */
    CTR_MODE_CENTRED,       /* GBA screens shown centred */
    CTR_MODE_BOTTOM,        /* the game on the bottom screen */
    CTR_MODE_COUNT
};

enum
{
    CTR_ROUTE_MONO, CTR_ROUTE_EYES, CTR_ROUTE_PLANES, CTR_ROUTE_BATTLE_STEREO,
    CTR_ROUTE_VOXEL, CTR_ROUTE_VOXEL_STEREO, CTR_ROUTE_BATTLE_WORLD, CTR_ROUTE_BATTLE_WORLD_STEREO,
    CTR_ROUTE_TRANSITION, CTR_ROUTE_BLANK, CTR_ROUTE_BOTTOM,
    CTR_ROUTE_COUNT
};

enum
{
    /* A 3D battle frame composed whole per eye, and why (RenderBattleStereo). */
    CTR_FLAT_BLEND = 1 << 0, CTR_FLAT_MOSAIC = 1 << 1, CTR_FLAT_BG_FRONT = 1 << 2,
    CTR_FLAT_SLOTS = 1 << 3,
    /* Slider up, no 3D: voxel stereo off or unavailable; no shared surface
     * for the voxel battle yet; no planes for a 2D screen; anything else
     * (transition, blank, bottom screen). */
    CTR_FLAT_NO_VOXEL = 1 << 4, CTR_FLAT_NO_SURFACE = 1 << 5, CTR_FLAT_NO_PLANES = 1 << 6,
    CTR_FLAT_OTHER = 1 << 7
};

bool CtrVideo_Init(void);
void CtrVideo_Shutdown(void);
void CtrVideo_Bind(CtrVideoMemory memory);
void CtrVideo_Present(void);
/*
 * The Fly cinematic in the voxel field (voxel_fly.c): begun by the game's
 * Fly once the player is on the bird (3ds_game_full.c, Port_FlyHover), with
 * the way to the destination in world tiles (+X east, +Z south); false when
 * the field is not the voxel world's, and Fly plays as the GBA's. Hovers:
 * the bird waits with the player while the camera climbs into the cloud.
 * Busy: the landing waits while the camera comes down out of it.
 */
bool CtrVideo_FlyBegin(float dirX, float dirZ);
bool CtrVideo_FlyHovers(void);
bool CtrVideo_FlyBusy(void);
/* The camera is coming down onto the new map, `share` 0..1 of the way: the
 * Pokemon flies in beside it to land as it settles. */
bool CtrVideo_FlyLanding(float *share);
/* The cinematic is under way: `share` 0..1 of the whole flight, take-off to
 * landing (the bottom screen's map walks the player's mark along it). */
bool CtrVideo_FlyProgress(float *share);
/* Whether a Fly begun now would play the cinematic: the voxel field shown,
 * no flight or swoop under way, the cloud texture made. */
bool CtrVideo_FlyPossible(void);
/* The cinematic is under way; its sky scene was shown (until the next Fly);
 * and dropped at once, the scene freed (a soft reset, a stuck landing). */
bool CtrVideo_FlyActive(void);
bool CtrVideo_FlySkyShown(void);
void CtrVideo_FlyCancel(void);


/* Keep showing the last top-screen frame instead of the game's screen. */
void CtrVideo_HoldTop(bool hold);
bool CtrVideo_TopHeld(void);
/* The top screen darkened under an arrow to the bottom screen, which has the
 * player's attention (a held top is darkened anyway, a few frames in). */
void CtrVideo_LookDown(bool on);
/* OAM entries belonging to field weather, tagged while BuildOamBuffer sorts sprites. */
/*
 * The true y of the sprite that wrote OAM entries [first, end). OAM keeps 8
 * bits of it, which a 160-line GBA screen never confuses, but the 240-line
 * field view does: a sprite just below it reads as one just above.
 */
void CtrVideo_ClearOamAnchors(void);
void CtrVideo_SetOamAnchor(unsigned first, unsigned end, int y);
void CtrVideo_ClearVoxelWeatherOam(void);
void CtrVideo_MarkVoxelWeatherOam(unsigned first, unsigned end);
/* OAM entries of the clouds of WEATHER_SUNNY_CLOUDS: the GBA shows them only
 * through the water (priority 3, under the ground's layer), and the voxel
 * view lays them on its water itself (voxel_entities.c), never over the view. */
void CtrVideo_MarkVoxelCloudOam(unsigned first, unsigned end);
/* OAM entries of the fog's sprites, one picture on a 64-pixel lattice: the
 * compositor repeats it over the whole view (src/sprite.c). */
void CtrVideo_ClearFogOam(void);
void CtrVideo_MarkFogOam(unsigned first, unsigned end);
/* Weather the GBA lays over its 240x160 screen, carried over the whole view:
 * one picture on a 64-pixel lattice (ash, the sandstorm), or particles that
 * wrap round the screen (rain, snow, sand swirls, bubbles). Cleared with the
 * fog's (CtrVideo_ClearFogOam). */
void CtrVideo_MarkWeatherOam(unsigned first, unsigned end, bool lattice);
/*
 * The OBJ palettes past the GBA's sixteen (include/palette.h, PLTT_EXT_*):
 * the faded colours of all CTR_EXT_PALETTES, handed over when the game copies
 * its palette to PLTT, and which are in use, a bit each (none: nothing here
 * is looked at). OAM entries [first, end) of a sprite drawn with one are
 * marked with it, as BuildOamBuffer writes them (src/sprite.c): attr2's four
 * bits of palette cannot name it. The voxel field reads them for its cards.
 */
#define CTR_EXT_PALETTES 16
void CtrVideo_LatchExtPalettes(const uint16_t *colors, unsigned inUse);
const uint16_t *CtrVideo_ExtPalette(unsigned index);
void CtrVideo_ClearExtPaletteOam(void);
void CtrVideo_MarkExtPaletteOam(unsigned first, unsigned end, unsigned index);
void CtrVideo_NotifyTilesetAnimWrite(const void *dest, unsigned bytes);
/*
 * The field's banks of other tilesets (3ds_tile_banks.h). SetTileBank gives
 * bank `bank` (0 to CTR_TILE_BANKS - 1) a secondary tileset's tiles (4bpp, at
 * most CTR_TILE_BANK_BYTES) and its seven palettes 6-12, unfaded; NULL tiles
 * empty it. A tilemap buffer registered with an array of bank numbers (1 +
 * bank, 0 for none, an entry each) hands them over whenever the game copies
 * it to VRAM (TilemapRequested, from bg.c); any other copy to that VRAM
 * clears them. An entry keeps its bank only while VRAM holds the entry it
 * was handed over with.
 */
void CtrVideo_SetTileBank(unsigned bank, const void *tiles, unsigned bytes, const uint16_t *palettes);
void CtrVideo_RegisterTilemapBanks(const void *buffer, const uint8_t *banks, unsigned entries);
void CtrVideo_UnregisterTilemapBanks(const void *buffer);
void CtrVideo_TilemapRequested(unsigned vramOffset, const void *src, unsigned bytes);
/*
 * A write the game made through its BIOS copies (3ds_prof_wrap.c): VRAM,
 * palette or OAM, by address. Phase F1, part B: which VRAM changed is
 * announced instead of found by comparing; a block that changed without
 * being announced is logged (CONTRACT) - the writers still to cover.
 */
/* Tiles decoded since the last call, by reason, and frames that decoded or
 * redrew layer cells: {new, bytes, palette, unchanged (audit), decode frames,
 * cell frames, cells, tiles}. */
void CtrVideo_DecodeCounts(uint32_t counts[8]);
void CtrVideo_NotifyWrite(const void *dest, unsigned bytes);
/*
 * What the OBJ VRAM did since something was made from it (the voxel field's
 * cards). CtrVideo_VramToken is the token of the frame being presented:
 * recorded with what is copied from OBJ VRAM in it.
 */
uint32_t CtrVideo_VramToken(void);
/*
 * Whether it changed after a token, answered by comparing, as the renderer's
 * own readers do: only inside CtrVideo_Present, from its VramTrustFrame on
 * (the voxel field's update), it compares the blocks of OBJ VRAM [offset,
 * offset + bytes) that may have changed, sets *changed to whether any changed
 * after `token`, and returns true. Then a caller that copies those bytes may
 * record CtrVideo_VramToken() for them: every block of the range was compared
 * at it.
 * Anywhere else it compares nothing, sets *changed and returns false - a token
 * recorded then would not hold (a block changed and changed back unseen).
 */
bool CtrVideo_ObjVramSyncChanged(unsigned offset, unsigned bytes, uint32_t token, bool *changed);
/* The compositor's nocache bits exist in the frame dumps' builds, and in the
 * console test builds that turn them off battle by battle (BATTLE_TOGGLES). */
#if defined(CTR_FRAME_DUMP) || defined(CTR_BATTLE_TOGGLES)
#define CTR_AUDIT_BITS 1
/* Audit builds: the bits of nocache.txt (3ds_video.c, CtrDumpOff). */
unsigned CtrVideo_DumpOff(void);
#endif
const uint8_t *CtrVideo_GetBgVram(void);
/* Frees the 2D compositor's 3D depth planes before the next frame, for the
 * overworld when it cannot place an atlas. */
void CtrVideo_RequestPlaneRelease(void);
/*
 * Whether the frames that follow are a GBA stage: a screen composed as one
 * 240x160 picture (intro, title, credits), shown 1:1 in the middle of the top
 * screen with the space around it filled from its own art. See docs/ARCHITECTURE.md.
 */
void CtrVideo_SetStage(bool stage);
/* Every layer of the stage fades into the margins, scrolling or not (the
 * cable car). */
void CtrVideo_SetStageFaded(bool faded);
/* A stage shown magnified, as large as the top screen holds what can be seen
 * of it, centred (the legendaries' cutscenes; 3ds_video.c, RenderEye). */
void CtrVideo_SetStageZoomed(bool zoomed);
bool CtrVideo_StageZoomed(void);
/* Whether a field move's banner is up: BG0 then wraps across the whole
 * screen instead of being the text band (src/field_effect.c). */
void CtrVideo_SetFieldBanner(bool banner);
/*
 * Which GBA screen shown centred the frames that follow are, if any: 1:1 at
 * the stage position, with the layers that wrap on the GBA and sprites
 * reaching into the margins. Some screens also carry their background out to
 * the edges of the top screen (3ds_video.c, sCentredFills).
 */
enum
{
    CTR_CENTRED_NONE,
    /* The fly map, the Town Map, the OPTION screen, and the others with
     * nothing of their own (ctr_gba_centred.h's default: contests,
     * minigames). */
    CTR_CENTRED_PLAIN,
    /* The title menu and the professor's speech. */
    CTR_CENTRED_MAIN_MENU,
    CTR_CENTRED_NAMING,
    CTR_CENTRED_CLOCK,
    /* The choice of starter from the professor's bag. */
    CTR_CENTRED_STARTER,
    /*
     * The PokéNav, composed as the others but shown on the bottom screen: its
     * 240x240 area left of the button column, its header on the top edge,
     * its help bar on the bottom one and the rest in the middle (NavBand).
     * The top screen keeps the frame it last showed meanwhile.
     */
    CTR_CENTRED_POKENAV,
    /*
     * The PC's boxes, also on the bottom screen: the GBA screen 1:1 in the
     * middle of the 240x240 area, its scrolling background carried on above
     * and below it.
     */
    CTR_CENTRED_STORAGE,
    /* A Pokémon's summary, on the bottom screen the same way. */
    CTR_CENTRED_SUMMARY,
    /*
     * The bag: opened from the field, in the 240x240 area left of the column,
     * the picture in its middle; opened from anything else (a battle, a
     * shop, the PC, giving an item) over the whole bottom screen. Its striped
     * backdrop is carried on around it.
     */
    CTR_CENTRED_BAG,
    CTR_CENTRED_BAG_WHOLE,
    /* The Pokédex, opened from the field: left of the column as the bag. */
    CTR_CENTRED_POKEDEX,
    /*
     * The party menu: opened from the field (the start menu, an item used or
     * given from the bag) left of the column, the picture in its middle;
     * from a battle, a contest or a facility over the whole bottom screen.
     * Its olive frame is carried on around it.
     */
    CTR_CENTRED_PARTY,
    CTR_CENTRED_PARTY_WHOLE,
    /*
     * The shop's BUY screen, over the whole bottom screen on its light green
     * as the bag from a shop is, without the GBA screen's copy of the map
     * and of the people in it (shop.c): the top screen keeps the world.
     */
    CTR_CENTRED_SHOP,
    /*
     * A trade, over the whole bottom screen while the top keeps the room it
     * started from (the Trade Center, the Union Room, the person met):
     * the menu of both parties on its blue stripes, then the trade itself.
     */
    CTR_CENTRED_TRADE_MENU,
    CTR_CENTRED_TRADE,
    /* The Berry Blender: its bowl, an affine layer, kept to the picture. */
    CTR_CENTRED_BLENDER,
    CTR_CENTRED_POKEDEX_CAUGHT,
    /* The trainer card from the field (the classic menus): in the bottom
     * screen's window as the PokéNav. */
    CTR_CENTRED_CARD,
    /* The Game Corner's roulette: its wheel, an affine layer that does not
     * wrap, whole past the picture's left edge and only once. */
    CTR_CENTRED_ROULETTE,
    /* The Game Corner's slot machine, set in the casino's wall. */
    CTR_CENTRED_SLOTS,
    /* The Hall of Fame (the team on its stripes, the champion's card) and
     * the PC's record of it: its bands carried out to the edges. */
    CTR_CENTRED_HALL_OF_FAME,
    /* The Battle Factory's rental and swap screens: their sprites kept to
     * the picture, where the swap menu's bar slides in from past its edge. */
    CTR_CENTRED_FACTORY,
    CTR_CENTRED_SCREENS
};
void CtrVideo_SetCentred(unsigned screen);
/* The HOME button was pressed while it is not allowed: a sign for a moment. */
void CtrVideo_ShowNoHome(void);
/*
 * The window edges an HBlank DMA writes line by line (the PokéNav's glow
 * behind its chosen option, its condition graph): values holds WIN0H for each
 * line, or WIN0H and WIN1H with both; NULL turns it off. Only the PokéNav's
 * screens are composed with them, and a centred screen that also hands over
 * what its windows show (below).
 */
void CtrVideo_SetLineWindow(const uint16_t *values, unsigned lines, bool both);
/*
 * WININ for each of the 160 lines, for a centred screen whose HBlank interrupt
 * moves its windows and changes what they show (the Battle Dome's tourney
 * tree); NULL turns it off.
 */
void CtrVideo_SetLineWindowInside(const uint16_t *values);
/*
 * The circle of light of a dark cave (Flash) or the Battle Pyramid, which the
 * field draws as window 0 edges line by line: its centre in the field's screen
 * coordinates (the sprites' frame) and its radius in pixels. Off with false.
 */
void CtrVideo_SetFieldLight(bool on, int x, int y, int radius);
/* Where a game screen's 240x160 picture sits on the bottom screen, 1:1: the
 * window 3ds_bottom_ui.c frames. Everything else there is its canvas. */
#define CTR_BOTTOM_WIN_X 40
#define CTR_BOTTOM_WIN_Y 40
/* Whether the last frame drew a game screen into the bottom screen's window:
 * the PokéNav, the bag, the party menu, the Pokédex, the boxes. */
bool CtrVideo_BottomInUse(void);
/* The bottom screen draws the game's screen itself (3ds_bottom_ui.c's native
 * views): its picture is not composed, and the top screen stays held. */
void CtrVideo_SetBottomNative(bool native);
/* Whether the field's Pokédex, bag and party menu are drawn natively (the
 * classic menus off): their pictures are never composed then. */
void CtrVideo_SetNativeMenus(bool on);
/* The bottom screen's canvas just put on the screen: its header and dock over
 * the game window's columns are drawn from it (3ds_video.c, StripUpload). */
void CtrVideo_BottomStrips(const uint16_t *canvas);
/* Whether that screen is one that has the bottom screen to itself, with no
 * section dock round it: the PC's boxes, a summary, the bag or the party
 * menu opened from anywhere but the field. */
bool CtrVideo_BottomWhole(void);
/* The picture's line a tap on line y of the bottom screen lands on, -1 off it. */
int CtrVideo_BottomPictureY(int y);
/*
 * Whether the frames that follow are the battle scene: the 240x160 picture 1:1
 * at (CTR_BATTLE_X, CTR_BATTLE_Y), so that its text box lies on the bottom
 * edge of the top screen, the text box stretched to the full width and the
 * scene above and beside it carried on from its own layers. See docs/ARCHITECTURE.md.
 */
void CtrVideo_SetBattle(bool battle);
/*
 * Per-scanline values of background scroll registers for the next frame: reg
 * is the offset from BG0HOFS, wide means two registers per line (32-bit DMA),
 * values holds one unit per line. NULL values turns it off.
 */
void CtrVideo_SetLineScroll(unsigned reg, bool wide, const void *values, unsigned lines);
/*
 * Per-scanline values of REG_BLDALPHA for the next frame (Surf's and Muddy
 * Water's wave band): one unit per line, NULL when the scanline effect writes
 * something else. Composed in bands of equal weights (3ds_video.c, Compose).
 */
void CtrVideo_SetLineAlpha(const uint16_t *values, unsigned lines);
/* A scanline effect on a register the port does not draw line by line (not a
 * BG scroll, WIN0H or BLDALPHA): counted, the frame shows entry 0's value. */
void CtrVideo_NoteLineDropped(void);
/*
 * What the frames since the last call did not draw as the GBA would, or drew
 * a cheaper way, for the profiler: counts of frames, reset by each call.
 */
enum
{
    CTR_MONO_WHY_OBJWIN,    /* an OBJ window: it is shaped by the sprites */
    CTR_MONO_WHY_FRONT,     /* front-tie: no background at or behind the furthest sprite's priority to keep */
    CTR_MONO_WHY_KEEPER,    /* no surface to keep the scenery in (or too narrow) */
    CTR_MONO_WHY_OTHER,     /* a transition, a bitmap mode or forced blank, a scanline register, audit dumps */
    CTR_MONO_WHY_UNSETTLED, /* the scenery not as the frame before left it: kept once it holds */
    CTR_MONO_WHY_COUNT
};
typedef struct
{
    /* Battle frames without the 3D slider (2D or over the voxel world). */
    uint32_t monoKept;         /* the scenery kept from a frame before, the sprites over it */
    uint32_t monoRecomposed;   /* the scenery composed again apart, then kept (as the frame before showed it) */
    uint32_t monoEmpty;        /* no background of the scene drew anything: the sprites alone */
    uint32_t monoFallback[CTR_MONO_WHY_COUNT]; /* composed whole, as before */
    uint32_t monoBlankLayers;  /* blank backgrounds left out of the scene's walk (layers, not frames) */
    uint32_t monoFrontLayers;  /* kept frames whose backgrounds in front of a sprite were composed with the sprites */
    uint32_t edgeBlankLayers;  /* blank backgrounds of the battle's edges measured but not walked (layers) */
    /* Effects. */
    uint32_t lineAlphaBanded;  /* per-line BLDALPHA composed in bands */
    uint32_t lineAlphaDropped; /* per-line BLDALPHA shown with one line's weights */
    uint32_t lineDropped;      /* another per-line register, or per-line windows off the
                                * PokeNav, shown with the first line's value */
    uint32_t objWinIgnored;    /* an OBJ window that only changes effects or hides layers: not drawn */
    uint32_t mosaicIgnored;    /* a mosaic that shows, drawn without it */
} CtrQualityCounts;
void CtrVideo_TakeQualityCounts(CtrQualityCounts *out);
/*
 * Whether the frames that follow show a battle transition: GBA geometry
 * (compat/ctr_gba_transition.h), composed over the field - 2D or voxel - at
 * the battle scene's scale. See 3ds_video.c, RenderTransition.
 */
void CtrVideo_SetTransition(bool transition);
/*
 * The registers from BG0HOFS to BLDY (CTR_LINE_REG_FIRST, CTR_LINE_REGS of
 * them) as each of the GBA's 160 lines of a transition's frame shows them:
 * lines x CTR_LINE_REGS values, NULL when there is no transition.
 */
#define CTR_GBA_LINES 160
#define CTR_LINE_REG_FIRST 0x10
#define CTR_LINE_REGS 35
void CtrVideo_SetLineRegisters(const uint16_t *regs, unsigned lines);
/* Sprites the game places on the screen rather than on the map: a battle
 * transition's, drawn with it (src/sprite.c). */
void CtrVideo_ClearScreenOam(void);
void CtrVideo_MarkScreenOam(unsigned first, unsigned end);
/* Among those, the ones placed by hand on the 240x160 picture around the
 * player (the healing machine's balls and monitors): drawn where the map has
 * the machine rather than where the window layer has its text. */
void CtrVideo_MarkMachineOam(unsigned first, unsigned end);
/* And Fly's bird and its rider, flying on that picture around the player. */
void CtrVideo_MarkFlyOam(unsigned first, unsigned end);
const CtrVideoStats *CtrVideo_GetStats(void);

/* Pure, SDK-free address/format helpers, also used by host regression tests. */
uint32_t CtrVideo_TextMapOffset(unsigned x, unsigned y, unsigned size);
uint32_t CtrVideo_Texel(unsigned x, unsigned y, unsigned width);
uint32_t CtrVideo_RGBA8(uint16_t color, bool opaque);
uint16_t CtrVideo_RGBA5551(uint16_t color);
unsigned CtrVideo_ObjTile(unsigned base, unsigned x, unsigned y,
                          unsigned width, bool color256, bool mapping1d);
int32_t CtrVideo_AffineReference(uint32_t value);
/* A battle is on (its transition included): OPTIONS' REAL-TIME PACING is
 * for the field, where the voxel world costs the frame (3ds_platform.c). */
bool CtrVideo_InBattle(void);
/* Before the voxel world parks its VRAM (CtrVoxel_ParkVram, outside the
 * frame): the battle's keeper or a large layer over a gap of its building
 * pages' arena, if one holds it, given back first (CtrVoxel_LendPagesGap). */
void CtrVideo_PagesGapRevoke(void);

#endif
