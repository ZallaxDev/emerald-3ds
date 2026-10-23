#ifndef CTR_VOXEL_H
#define CTR_VOXEL_H

#include <stdbool.h>
#include <citro3d.h>

bool CtrVoxel_Init(void);
void CtrVoxel_Shutdown(void);

/* Cheap and side-effect free: decides which renderer composes this frame. */
bool CtrVoxel_IsAvailable(void);

/* Called after C3D_FrameBegin(), with the previous frame's GPU work finished:
 * everything it writes goes to linear memory that C3D_FrameEnd() then flushes. */
bool CtrVoxel_Update(void);
/*
 * Called after C3D_FrameEnd() of a frame the voxel world was drawn in, with
 * the tick C3D_FrameBegin() returned on: builds chunks and atlases while the
 * GPU draws that frame, in what is left of it before the next VBlank once the
 * game and the audio have had theirs. Nothing it does touches the GPU; what it
 * finishes is uploaded by the next Update.
 */
void CtrVoxel_AfterSubmit(uint64_t frameBeginTick);
/* Called just before the next C3D_FrameBegin() waits, with the tick: what ran
 * between the end of AfterSubmit and here (the game, the audio, the bottom
 * screen, the compositor's preparation, the log) is the share the next
 * AfterSubmit leaves free. */
void CtrVoxel_NoteFrameWait(uint64_t waitTick);
/* Called instead after a frame the world was not drawn in: what it holds only
 * to be drawn (the night lamps' buffers) goes once no queued frame reads it. */
void CtrVoxel_Idle(void);
/* Game VRAM animation transfer: tile numbers are relative to BG_VRAM. */
void CtrVoxel_NotifyTilesetAnimWrite(unsigned firstTile, unsigned tileCount);

/* Draws into the given target, from the middle camera (eye -1) or from one
 * eye of the stereo pair (0 left, 1 right; see CtrVoxel_SetStereo). Never
 * opens or closes a frame. */
void CtrVoxel_Draw(C3D_RenderTarget *target, int eye);
/* The swoop to and from the 2D field (voxel_swoop.c): how far the camera
 * stands towards its top-down pose this frame, 0..1, and the world's height
 * (1 as built, 0 flattened onto the map: the pop-up book), and whether the
 * sprite cards and their cast shadows are drawn at all. Before Update. */
void CtrVoxel_SetSwoop(float t, float height, bool cards, bool shadows);
/* How far the swoop has taken the world's light to neutral, 0..1 (the
 * grade, haze and bloom gone with it): 3ds_video.c eases its blur the same way. */
float CtrVoxel_SwoopNeutral(void);
/* The GBA brightness effect (BLDY) of the frame about to be drawn, as the
 * compositor reads it: on the backgrounds and on the sprites, towards white
 * or black. The palette fade is read by the voxel module itself. */
void CtrVoxel_SetBrightness(float backgrounds, float sprites, bool white);
/* How much glow the 2D compositor adds around the brightest parts of the
 * voxel picture this frame (0: none): the light's bloom, 0 indoors. */
float CtrVoxel_Bloom(void);
/* Whether the world draws the game's fog itself (in the scene, see
 * ctr_voxel.c): the compositor then leaves the fog's flat sprites out. */
bool CtrVoxel_DrawsFog(void);
/* The dark of a cave over the frame just drawn: an alpha texture to lay in
 * black, centred on (x, y) of the logical surface, size pixels across, at
 * amount. NULL when there is none. */
const C3D_Tex *CtrVoxel_Gloom(float *x, float *y, float *size, float *amount);

/*
 * Stereoscopy (3D slider): the world is drawn once per eye, each from its
 * own camera (see ctr_voxel.c). Before Update, with the frame's slider
 * (0: one picture): it sets the pair's separation, widens the frustum the
 * chunks are picked by, and makes Draw with eye 0 or 1 draw that eye. What
 * is placed over the world afterwards (CtrVoxel_ProjectPictureTile,
 * CtrVoxel_PlayerLightSpot, CtrVoxel_Gloom) is placed as the last Draw's
 * eye saw it.
 */
void CtrVoxel_SetStereo(float slider); /* 0..2: above 1 is deeper than the slider's full travel */
/* The horizontal shift, in whole pixels of the logical surface, that puts
 * the game's text windows and prompts over the world in front of the nearest
 * world the last Draw's eye can see (3ds_video.c, ComposeVoxelOverlay):
 * 0 from the middle camera or with the slider down. */
float CtrVoxel_OverlayParallax(void);

typedef struct
{
    unsigned instances;
    unsigned chunks;
    unsigned visibleChunks;
    unsigned vertices;
    unsigned atlasRebuilds;
    unsigned meshRebuilds;
    unsigned spriteUpdates;
    unsigned animationUploads, animatedMetatiles;
    unsigned reflections;
    unsigned errors;
    /* Vertices the last chunk build could not fit. */
    unsigned dropped;
    /*
     * Chunks the view needed this frame and did not get, because the per-frame
     * build budget ran out before reaching them. A steady non-zero number is
     * the cache thrashing rather than filling, which on screen is a black
     * square-edged hole - so it is worth a number of its own.
     */
    unsigned chunksMissing;
    /* Milliseconds the last frame spent visiting the view and building
     * chunks and atlases, and the worst seen. The steady frame is fine; what
     * the overlay could not show was the spike. */
    float meshMs, meshPeakMs;
    /* The whole update, billboards and page streaming included. */
    float updateMs, updatePeakMs;
    /* Where the rest of the update went: the maps on screen and the lighting
     * reset that follows a change to them (region layouts come off RomFS
     * there), the atlas job, and the billboards. */
    float worldMs, atlasMs, spritesMs;
    /* ... and of what no other figure holds: the building pages' stream, the
     * animated tiles' recomposition, the drafts. */
    float streamMs, animMs, draftMs;
    /* Spent building after the last FrameEnd (CtrVoxel_AfterSubmit), and the
     * budget it had. */
    float afterMs, afterBudgetMs;
    /* Chunks built this frame, and still waiting for a later one. */
    unsigned frameBuilds, pendingBuilds;
    /* Squares drawn this frame as their draft only, and drafts made in all. */
    unsigned draftsVisible, draftsMade;
    /* Bytes still free in the two pools this competes for. VRAM is here
     * because it is almost entirely unused, which is the whole argument for
     * moving the mesh into it. */
    unsigned long linearFree, vramFree;
} CtrVoxelStats;

const CtrVoxelStats *CtrVoxel_GetStats(void);

/*
 * Gives back the VRAM of atlases the overworld is not using right now - every
 * tileset pair but the current map's. For the 2D compositor, on a frame the
 * overworld does not draw. Returns the bytes released.
 */
unsigned long CtrVoxel_ReleaseIdleVram(void);

/*
 * As CtrVoxel_ReleaseIdleVram, for one allocation of `bytes`: the atlases of
 * maps out of view go first, least recently used first, one at a time until
 * VRAM has a block that large; with `inView`, the ones in view too if that
 * was not enough. Returns the bytes released.
 */
unsigned long CtrVoxel_ReleaseIdleVramFor(uint32_t bytes, bool inView);

/*
 * All the world's VRAM - arenas, chunks, building pages, atlases - given back
 * while the world is off, and claimed again (false: no room yet, free other
 * surfaces and retry). Parked, the world is unavailable; back, it is built
 * again as a map just entered.
 */
bool CtrVoxel_HoldsVram(void);
void CtrVoxel_ParkVram(void);
bool CtrVoxel_UnparkVram(void);
/* Bytes used and largest gap of the chunk and building-page arenas. */
void CtrVoxel_ArenaUse(uint32_t *chunkUsed, uint32_t *chunkLargest, uint32_t *pageUsed, uint32_t *pageLargest);

/* A map just entered by a cut is still being built (see VOXEL_WARMUP_MS). */
bool CtrVoxel_IsWarmingUp(void);

/*
 * Where the camera shows a point of the 240x160 picture the game lays out
 * around the player: `tileX`, `tileY` are tiles from the player's tile
 * centre (right, down), the point standing a tile up in the air - the top of
 * a counter. The result is on the 400x240 screen. False without a world.
 */
bool CtrVoxel_ProjectPictureTile(float tileX, float tileY, float *screenX, float *screenY);
/* The player on the logical surface, for the circle of light of a dark cave,
 * and how many surface pixels one GBA pixel of the ground covers there. */
bool CtrVoxel_PlayerLightSpot(float *x, float *y, float *scaleX, float *scaleY);

/*
 * The 3D battle (3ds_video.c, RenderBattleWorld). From Begin to End, Update
 * draws the world as the battle's scenery: seen from a camera of its own on
 * a stage chosen near the player (voxel_battle.c), with nobody in it. Begin
 * once per battle, on its first frame; Update ends it by itself, giving the
 * field its camera back, once the game has left the battle.
 */
bool CtrVoxel_IsAvailableForBattle(void);
void CtrVoxel_BeginBattle(void);
void CtrVoxel_EndBattle(void);
bool CtrVoxel_InBattle(void);
/* Before each battle Update: whether the intro is sliding its scenery in
 * (the camera glides in meanwhile), and how far BG3 is scrolled from rest in
 * GBA pixels (a move shaking the scenery shakes the camera). */
void CtrVoxel_SetBattleFrame(bool introSliding, float shakeX, float shakeY);
/*
 * The battle's entrance as of the last Update, for the effects laid over the
 * world (3ds_video.c, BattleIntroEffects); all zero outside a battle and once
 * the camera has landed. Each 0..1 but `time`, the battle's frames so far.
 */
typedef struct
{
    float flight; /* the camera's arc: up and away mid-flight */
    float speed;  /* how fast it travels */
    float impact; /* the landing, dying away */
    float bars;   /* the cinema bars, closed in */
    float time;
} CtrVoxelBattleIntro;
void CtrVoxel_BattleIntro(CtrVoxelBattleIntro *intro);

#endif
