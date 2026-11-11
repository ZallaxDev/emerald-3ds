/*
 * Voxel overworld on Citro3D.
 *
 * This is the only file of the voxel module that knows what a GPU is. It owns
 * the shader, the metatile atlas textures, the chunk cache in VRAM, the staging
 * buffers and the matrices; the map, the camera, the atlas composition and the
 * geometry come from the SDK-free modules beside it.
 *
 * Frame ownership is unchanged: CtrVideo_Present() opens and closes the one
 * Citro3D frame, and nothing here calls C3D_Init, C3D_FrameBegin or
 * C3D_FrameEnd. Update() runs inside the open frame, after FrameBegin has
 * waited for the previous frame's GPU work, so every staging buffer written
 * last frame is free again when it starts.
 *
 * What an Old 3DS can afford decides the shape of everything below:
 *
 *   - nothing is rebuilt, re-hashed or re-read on a frame where nothing
 *     changed. A frame that only walks costs the draw calls and the billboards;
 *   - the work that does come - a chunk entering the view, an atlas, a
 *     building page - is spent against a time budget measured from the frame
 *     itself, nearest first, and done ahead of time when the frame has room;
 *   - no file is read on the render thread while the game waits for it.
 */

#include <3ds.h>
#include <citro3d.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_assets.h"
#include "3ds_log.h"
#include "3ds_platform.h"
#include "3ds_daynight.h"
#include "3ds_gpumem.h"
#include "3ds_video.h"
#include "3ds_map_events.h"

#include "ctr_voxel.h"
#include "voxel_arena.h"
#include "voxel_atlas.h"
#include "voxel_battle.h"
#include "voxel_camera.h"
#include "voxel_entities.h"
#include "voxel_regions.h"
#include "voxel_mesh_builder.h"
#include "voxel_world.h"
#include "voxel_tree.h"
#include "voxel_building.h"
#include "voxel_grade.h"
#include "voxel_relief.h"
#include "voxel_lighting.h"
#include "voxel_sign.h"
#include "../../compat/port_prof.h"

#define VOXEL_SHADER_PATH "romfs:/shaders/voxel.shbin"

#ifndef CTR_VOXEL_TRACE
#define CTR_VOXEL_TRACE 0
#endif

/*
 * The depth buffer is 16-bit. At 0.1 the near plane spent its precision on the
 * first tile before the eye, and at the player's distance (8-17 units) a cast
 * shadow 0.03 over the ground no longer told apart from it: striped, and gone
 * next to the feet. Nothing on screen comes within a unit of the eye (the
 * lowest camera is ~4.5 up), and 1.0 gives ten times the resolution.
 */
#define VOXEL_NEAR 1.0f
#define VOXEL_FAR 200.0f

/* The logical surface is a 512x256 texture of which the game uses the top-left
 * 400x240. The 2D compositor reaches that sub-rectangle through its ortho
 * projection; a 3D pass has to fold the same mapping into its own matrix,
 * because the viewport covers the whole texture. */
#define VOXEL_SURFACE_W 512.0f
#define VOXEL_SURFACE_H 256.0f

/* â”€â”€ GPU vertex format â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * The builder works in VoxelVertex (six floats, 24 bytes). What the GPU reads
 * is this: the texture coordinates stay float, because the building pages are
 * not powers of two and their texel edges must stay exact, while position and
 * shade become shorts - positions in 1/512 tile relative to the draw's own
 * origin (a chunk corner, or the camera for the billboards), shade in 1/16384.
 * voxel.v.pica undoes both scales.
 *
 * A third less of everything: VRAM per chunk, staging, GPU copies and vertex
 * fetch. 1/512 of a tile is 1/32 of a pixel, and the origin a vertex is
 * measured from is always a whole tile, so two chunks quantise a shared edge
 * to exactly the same place.
 */
typedef struct
{
    float u, v;
    int16_t x, y, z, shade;
} VoxelGpuVertex;

#define VOXEL_POS_SCALE 512.0f
#define VOXEL_SHADE_SCALE 16384.0f

/* Vertices a single chunk may produce before the builder refuses more. */
#if CTR_VOXEL_LIGHTING
/* Up to 64 ground quads gain 18 vertices at a shadow boundary. Preserve the
 * original structure budget instead of letting refinement evict walls. */
#define VOXEL_CHUNK_SCRATCH (8192u + 64u * 18u)
#define VOXEL_SHADOW_RESERVE (VOXEL_SPRITE_SLOTS * VOXEL_CAST_SHADOW_VERTICES)
#else
#define VOXEL_CHUNK_SCRATCH 8192u
#define VOXEL_SHADOW_RESERVE 0u
#endif

/*
 * Linear memory the frame's chunk uploads are packed into, in build order.
 * Each queued GPU copy still owns its source until the frame has completed,
 * so the pool is only rewound at the start of the next Update. What does not
 * fit waits for the next frame; a single chunk always fits.
 */
#define VOXEL_STAGING_VERTICES 12288u

/*
 * GPU copies queued per frame for chunks. Inside a frame every
 * C3D_SyncTextureCopy splits the command list and adds up to two entries to
 * Citro3D's GX queue, which holds 32 per frame - and a full queue is not a
 * wait but svcBreak(USERBREAK_PANIC) in gxCmdQueueAdd. The frame's own splits,
 * its display transfer, one atlas and one page slice come on top of these.
 * This is only a ceiling: each copy also asks CtrVideo_TryVoxelUpload, which
 * keeps the frame's reserve free and so allows 4 copies a frame, 2 in stereo
 * or a voxel battle, shared by chunks, drafts (VOXEL_DRAFT_PER_FRAME is a
 * ceiling as well), pages and animated texels.
 */
#define VOXEL_CHUNK_UPLOADS_MAX 8u

/*
 * Vertices held back for the billboards: six for each sprite slot (objects,
 * effects and the visible wild ones), and some over. Without the reserve a
 * dense town would leave the player undrawn, which is a far worse failure
 * than a missing wall.
 */
#define VOXEL_SPRITE_RESERVE (VOXEL_SPRITE_SLOTS * 6u + 32u)
#define VOXEL_REFLECTION_RESERVE (VOXEL_SPRITE_SLOTS * VOXEL_REFLECTION_VERTICES + VOXEL_CLOUD_VERTICES)
#define VOXEL_DYNAMIC_VERTICES (VOXEL_SPRITE_RESERVE + VOXEL_SHADOW_RESERVE + VOXEL_REFLECTION_RESERVE)
#define VOXEL_SHADOW_FIRST VOXEL_SPRITE_RESERVE
#define VOXEL_REFLECTION_FIRST (VOXEL_SHADOW_FIRST + VOXEL_SHADOW_RESERVE)

/*
 * The night lamps' glass and the pools they cast (UpdateNightLamps): buffers
 * of their own, glass first. 64 KiB of linear memory and 96 KiB of heap,
 * which an Old 3DS has little of: taken when lamps are first lit and given
 * back once they are not (DAY / NIGHT off, indoors, by day, a battle). A pool
 * is one quad on a street and at most a quad a cell elsewhere (LampGround).
 */
#define VOXEL_LAMP_RESERVE 2046u
#define VOXEL_LAMP_GROUND_RESERVE 2046u
#define VOXEL_LAMP_VERTICES (VOXEL_LAMP_RESERVE + VOXEL_LAMP_GROUND_RESERVE)
#define VOXEL_LAMP_GROUND_FIRST VOXEL_LAMP_RESERVE
/* Unlit this long, no frame still queued draws from them: freed. */
#define VOXEL_LAMP_RELEASE_MS 100.0f

/* One atlas per tileset pair, shared by every map that uses that pair. Six:
 * walking back and forth through a string of towns and routes reaches five
 * or six pairs, and with four they evicted each other on every crossing. */
#define VOXEL_ATLAS_SLOTS 6u

typedef struct
{
    C3D_Tex tex;
    C3D_Tex extra[VOXEL_ATLAS_PAGES - 1u];
    const void *primaryTileset;
    const void *secondaryTileset;
    uint32_t stamp;
    uint32_t generation;
    /* Bumped when ids are added in place (an extending atlas job): old
     * UVs stay valid, only chunks that missed an id need building again. */
    uint32_t extension;
    bool extendPending;
    bool valid;
    VoxelAtlasMap map;
} VoxelAtlasSlot;

/* A map composed ahead of time found no free VRAM: none is tried again until
 * an atlas gives its VRAM back or the player is on another map (its group and
 * number here), or each try composed a whole atlas to throw it away. */
static bool sPrefetchBlocked;
static int sPrefetchBlockedGroup, sPrefetchBlockedNum;

static C3D_Tex *AtlasTex(VoxelAtlasSlot *slot, unsigned page)
{
    return page == 0 ? &slot->tex : &slot->extra[page - 1u];
}

static void AtlasPageRelease(C3D_Tex *tex);

static void FreeAtlasTextures(VoxelAtlasSlot *slot)
{
    for (unsigned page = 0; page < VOXEL_ATLAS_PAGES; ++page)
    {
        C3D_Tex *tex = AtlasTex(slot, page);
        if (tex->data != NULL)
        {
            AtlasPageRelease(tex);
            sPrefetchBlocked = false;
        }
        memset(tex, 0, sizeof(*tex));
    }
    slot->valid = false;
    ++slot->generation;
}
static bool sAtlasCapacityBlocked;

static bool sReady;
static DVLB_s *sDvlb;
static shaderProgram_s sProgram;
/* The world's program without the shadow map's work (voxel_plain.v.pica), for
 * every Draw that does not sample or draw one. Its `projection` register holds
 * projection * modelView (SetProjection, UploadModelView). */
static shaderProgram_s sPlainProgram;
static bool sHavePlainProgram;
static int sUniProjection = -1, sUniModelView = -1, sUniOrigin = -1;
static int sUniShadeTint = -1, sUniTintDiff = -1, sUniGrade = -1, sUniFog = -1;
static int sUniDappleU = -1, sUniDappleV = -1;
static int sUniShadowU = -1, sUniShadowV = -1, sUniShadowW = -1, sUniShadowSel = -1;
/* The tree crowns' brightness against the rest of the art, applied once as
 * the tree texture loads (BrightenCrowns). */
#define VOXEL_TREE_BRIGHTNESS 1.12f
/* The sun dapples' pattern; see MakeDapple. */
static C3D_Tex sDappleTex;
static bool sHaveDapple;
/* Where the current map's origin lies on the ground the dapples are laid
 * over: crossing into a connected map moves the origin, not the pattern. */
static int sDappleAnchorX, sDappleAnchorZ;
/* The sun rays' mesh over the screen, linear; see MakeRays. */
static VoxelGpuVertex *sRays;
static unsigned sRayCount;
/* The sunlit dust's vertices, rewritten every frame, linear; see DrawMotes. */
static VoxelGpuVertex *sMotes;
/* This frame's bloom strength, for the 2D compositor (CtrVoxel_Bloom). */
static float sBloomStrength;
/* The fog's drifting banks and the dark of a cave (FogSheets, MakeGloom). */
static C3D_Tex sFogTex, sGloomTex;
static VoxelGpuVertex *sFogSheets;
static bool sHaveFog, sHaveGloom, sOwnsFog;
static float sGloomAmount;
/* The rain's, the sand's and the ash's particles (MakeWeather, DrawWeather):
 * their own program, the second in voxel.shbin, and two buffers made once. */
typedef struct
{
    int8_t across, along, pad[2]; /* the corner, in 1/64 of the vectors */
    int16_t x, y, z, alpha;       /* place in the box, 1/32768; 1/16384 */
} WeatherVertex;
static shaderProgram_s sWeatherProgram;
static bool sHaveWeatherProgram, sOwnsWeather;
static int sUniBoxOffset = -1, sUniBoxSize = -1, sUniBoxBase = -1, sUniAcross = -1, sUniAlong = -1;
static WeatherVertex *sStreaks, *sFlakes;
#if CTR_VOXEL_LIGHTING
static float sGloomX, sGloomY;
#endif
#if CTR_VOXEL_LIGHTING
static void MakeDapple(void);
static void MakeRays(void);
static void MakeMotes(void);
static void MakeFog(void);
static void MakeGloom(void);
static void MakeWeather(void);
static void LoadWeatherProgram(void);
static void FadeFor(bool sprites);
#endif
/* Static models one chunk may place (their spots, not their geometry). */
#define VOXEL_CHUNK_STATICS 32u
static VoxelVertex *sScratch;         /* chunk builder output, ordinary heap */
static VoxelStaticDraw sJobStatics[VOXEL_CHUNK_STATICS]; /* the job's static models */
static void StaticMeshesFree(void);
#if CTR_VOXEL_LIGHTING
static void StaticMeshesRelight(void);
#endif
static VoxelVertex *sDynamicScratch;   /* billboard builder output */
static VoxelGpuVertex *sStaging;       /* chunk uploads, linear */
static unsigned sStagingUsed, sChunkUploads;
static VoxelGpuVertex *sDynamic;       /* billboards and shadows, linear */
static VoxelBuilder sBuilder;
static VoxelCamera sCamera;
/* The swoop's share of the camera this frame (CtrVoxel_SetSwoop), and the
 * orthographic end of its projection: half the 2D field in tiles. */
static float sSwoop;
static float sSwoopHeight = 1.0f;
static bool sSwoopCards = true, sSwoopShadows = true;
/* The Fly cinematic's share of the camera this frame (CtrVoxel_SetFly). */
static CtrVoxelFly sFly;
/* Flattening, height becomes distance up the picture, as the GBA's art
 * draws a tall thing in the tiles above its base: tiles north per tile of
 * height lost. 1 puts a two-tall crown on the tile above its trunk. */
#define VOXEL_SWOOP_SHEAR 0.0f /* 0 since the oblique end camera does this itself */

/*
 * ── The wide view ───────────────────────────────────────────────────────────
 * A camera that sees more squares than a frame can build: the swoop's
 * top-down framing (sSwoop > 0: its view rectangle is grown to the 2D field's
 * whole extent, UpdateFrustum), a camera a caller holds high
 * (CtrVoxel_SetWideView: the Fly's). Under it the streaming does not leave
 * black squares but degrades in rings around the camera's target: the
 * nearest squares get their full build, the ring beyond stays on drafts
 * (DraftChunk: eight a frame within VOXEL_WIDE_DRAFT_MS), and past that a
 * square the frame has nothing for is a flat quad in the colour of its
 * ground (FillHoles: the atlas's mean of each metatile) - and the backdrop
 * under all of it is the map's own colour (CtrVoxel_WideBackdrop), not the
 * GBA's black. The rings close in as the frames go by: a hole is drafted the
 * frame after it appears, and the builds take the drafts nearest the target
 * as soon as no hole in the draft ring is left. The walking view never meets
 * these gates - everything it wants fits the frame - and is left as it was;
 * nor does the warm-up behind a cut, which builds the whole view before
 * showing it.
 */
/* Full builds, in tiles from the camera's target (the larger axis: the rings
 * are squares, as the chunks are). The settled walking view is about 20 x 15
 * tiles round the player plus a chunk of slack, so a square whose centre is
 * within 12 tiles is one the camera will show when it lands: built for real
 * during the swoop, it does not pop in afterwards. */
#define VOXEL_BAND_FULL 12.0f
/* Drafts. The swoop's framing is the 2D field, 30 x 20 tiles plus the slack,
 * so 28 tiles from the target covers all of it; a camera that sees further
 * (the Fly's ninety squares) drafts this ring - (56/8)^2 = 49 squares, two
 * or three frames of drafts - and fills the rest flat. */
#define VOXEL_BAND_DRAFT 28.0f
/* The drafts' budget a frame under the wide view (VOXEL_DRAFT_PER_FRAME and
 * VOXEL_DRAFT_MS otherwise): eight is the frame's ceiling of chunk uploads,
 * and three milliseconds is what eight cost at most. */
#define VOXEL_WIDE_DRAFT_PER_FRAME VOXEL_CHUNK_UPLOADS_MAX
#define VOXEL_WIDE_DRAFT_MS 3.0f
/* The squares a view spans are logged ("VOXEL drafts:", "VOXEL wide:"): the
 * walking view is a dozen to twenty, the swoop's about twenty-five, the
 * Fly's ninety. Only the camera decides the wide view (CameraHigh): a big
 * walking view (zoomed out, pitched up) keeps the walking streaming. */
static bool sWideViewAsked;      /* CtrVoxel_SetWideView */
static bool sWideView;           /* this update's verdict (UpdateView) */
static unsigned sViewSquares;    /* chunks the view rectangle spans (UpdateFrustum) */
static unsigned sWideHolesLeft;  /* holes inside the draft ring not drafted this frame */
/* The fill (see there): the frame's flat squares and their vertices. */
#define VOXEL_FILL_MAX 128u
static unsigned sFillCount;
static VoxelGpuVertex *sFillGpu; /* VOXEL_FILL_MAX * 6, linear */

/* Whether the camera is up: the swoop's share of it, the Fly cinematic's
 * high pose (CtrVoxel_SetFly), or a caller's word. */
static bool CameraHigh(void)
{
    return sSwoop > 0.0f || sWideViewAsked || (sFly.active && sFly.lift > 0.05f);
}

void CtrVoxel_SetWideView(bool on)
{
    sWideViewAsked = on;
}
/*
 * The swoop's colour: as the camera goes to the 2D field's pose the world's
 * light eases to neutral - plain texture colour, no grade, haze, dapples,
 * rays, motes, moonlight or bloom (and 3ds_video.c takes the diorama blur
 * off with it) - so the crossfade is between two pictures of the same
 * colour. From this far along the camera's way (0..1) the light starts
 * towards neutral, reaching it with the camera at its end pose.
 */
#define VOXEL_SWOOP_NEUTRAL_START 0.0f
/* The ease's shape: 1 linear in the camera, above it the colour holds its
 * own longer and goes late. */
#define VOXEL_SWOOP_NEUTRAL_POWER 1.0f
#define VOXEL_SWOOP_HALF_W (CTR_GAME_WIDTH / 16.0f / 2.0f)
#define VOXEL_SWOOP_HALF_H (CTR_GAME_HEIGHT / 16.0f / 2.0f)
static VoxelAtlasSlot sAtlases[VOXEL_ATLAS_SLOTS];
static uint32_t sAtlasStamp;
static unsigned sAtlasExtensionsSkipped;
static bool sAtlasCapped;
/* One atlas upload per frame: the queued copy still owns this source after
 * AcquireAtlas returns. Cache hits need no upload and remain unrestricted. */
static uint16_t *sAtlasStaging;
static bool sAtlasUploadPending;
/* The animated tiles (AnimateTiles): those written since the last pass
 * began, and the pass in progress - the atlas each of its pairs is and the
 * generation it had then, and the next of its slots to write out. */
static uint8_t sAnimDirty[128];
static bool sAnimPending, sAnimActive, sAnimFresh;
static uint32_t sAnimWaitFrom;
static VoxelAnimPass sAnimPass;
static VoxelAtlasSlot *sAnimAtlas[VOXEL_ANIM_PAIRS];
static uint32_t sAnimGeneration[VOXEL_ANIM_PAIRS];
/* What the pass's atlases were when it found its slots (AnimBegin). */
static struct
{
    bool shares;
    uint32_t generation, extension;
    unsigned used;
} sAnimKey[VOXEL_ANIM_PAIRS];
static unsigned sAnimCursor, sAnimFirstAtlas;
static uint32_t sAnimBegan;
_Static_assert(VOXEL_ATLAS_SLOTS <= VOXEL_ANIM_PAIRS, "a pass numbers its pairs by atlas");
/* The building pages' texels drawn from animated tiles (Rustboro's fountain:
 * its jet, VoxelBuildings_AnimRun): the BG tiles written since they were
 * last redrawn, and the linear buffer their blocks are composed in for the
 * GPU to copy into the page - reused once the frame's copies are done. */
#define VOXEL_BUILDING_ANIM_BYTES 4096u
static uint8_t sBuildingAnimDirty[128];
static bool sBuildingAnimPending;
static uint16_t *sBuildingAnimOut;
static void AnimForget(VoxelAtlasSlot *slot)
{
    for (unsigned i = 0; i < VOXEL_ANIM_PAIRS; ++i)
        if (sAnimAtlas[i] == slot)
            sAnimAtlas[i] = NULL;
}

void CtrVoxel_NotifyTilesetAnimWrite(unsigned firstTile, unsigned tileCount)
{
    if (firstTile >= 1024) return;
    if (tileCount > 1024 - firstTile) tileCount = 1024 - firstTile;
    for (unsigned t = firstTile; t < firstTile + tileCount; ++t)
    {
        sAnimDirty[t >> 3] |= 1u << (t & 7);
        sBuildingAnimDirty[t >> 3] |= 1u << (t & 7);
    }
    sAnimPending = sAnimFresh = true;
    sBuildingAnimPending = true;
}
static bool AtlasJobBusy(void);
static void AtlasJobCancel(void);
static CtrVoxelStats sStats;
static uint32_t sFrame;

/*
 * A tileset pair whose atlas could not be built is remembered, because
 * retrying it is the single most expensive thing this module does and doing it
 * once per frame is what turns a broken map into a 30 fps slideshow.
 */
static const void *sFailedPrimary, *sFailedSecondary;
static bool sHasFailed;

static float MsSince(uint64_t start)
{
    return (float)((svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11);
}

static float TicksMs(uint64_t ticks)
{
    return (float)(ticks * 1000.0 / SYSCLOCK_ARM11);
}

/* â”€â”€ Packing â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

static void JobCancel(void);
static bool StaticMeshPrepare(unsigned pageModel, bool lit, unsigned triangles);
static const struct StaticMesh *StaticMeshFind(unsigned pageModel, bool lit);
static const struct StaticMesh *StaticMeshCurrent(unsigned pageModel, bool lit);
static void StaticMeshesLateFree(void);
static bool StaticBuildHeld(void);
static void StaticBuildPlaceHeld(void);
static bool StaticWantsRun(uint64_t started, float budget, bool missing);

static unsigned sPackErrors;
/* The furthest value that did not fit, and which of x, y, z or shade it was:
 * what the log needs to say which builder produced it. */
static float sPackWorst;
static char sPackAxis;

static int16_t Quantise(float value, float scale)
{
    float scaled = value * scale + 0.5f;
    int whole;

    /*
     * Round half up, never half away from zero: that is what keeps the
     * result of a shared edge independent of the origin it is measured from.
     * floor() by truncation and one correction, not floorf: ARMv6 has no
     * rounding instruction and the library call was a third of a chunk build.
     */
    if (scaled >= 32767.0f || scaled < -32768.0f)
    {
        if (scaled >= 32768.0f || scaled < -32768.0f)
        {
            ++sPackErrors;
            if (fabsf(value) > fabsf(sPackWorst))
                sPackWorst = value;
        }
        return scaled >= 0.0f ? 32767 : -32768;
    }
    whole = (int)scaled;
    if ((float)whole > scaled)
        --whole;
    return (int16_t)whole;
}

static void Pack(const VoxelVertex *src, unsigned count, VoxelGpuVertex *dst)
{
    for (unsigned i = 0; i < count; ++i)
    {
        dst[i].u = src[i].u;
        dst[i].v = src[i].v;
        unsigned before = sPackErrors;

        dst[i].x = Quantise(src[i].x, VOXEL_POS_SCALE);
        dst[i].y = Quantise(src[i].y, VOXEL_POS_SCALE);
        dst[i].z = Quantise(src[i].z, VOXEL_POS_SCALE);
        if (sPackErrors != before && sPackAxis == 0)
            sPackAxis = 'p';
        dst[i].shade = Quantise(src[i].shade, VOXEL_SHADE_SCALE);
        if (sPackErrors != before && sPackAxis == 0)
            sPackAxis = 's';
    }
}

/* Says which mesh held values the vertex format cannot carry, at most once
 * every few seconds: they are clamped, so a steady stream is one fault. */
static void ReportPackErrors(const char *what, int a, int b, int c, int d)
{
    static uint32_t sReported;

    if (sPackErrors != 0 && (sReported == 0 || sFrame - sReported >= 300))
    {
        sReported = sFrame;
        ++sStats.errors;
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: %u vertices out of packing range in %s %d,%d of %d:%d "
                     "(worst %s %.2f)", sPackErrors, what, a, b, c, d,
                     sPackAxis == 's' ? "shade" : "position", sPackWorst);
    }
    sPackErrors = 0;
    sPackWorst = 0.0f;
    sPackAxis = 0;
}

/*
 * The same packing without a floating-point comparison per value. On the Old
 * 3DS's VFP every comparison stalls the pipeline to hand its flags over, and
 * Quantise makes up to four per value: a dense chunk took 13 ms to pack on
 * hardware. Here the conversion itself rounds towards minus infinity - the
 * FPSCR's rounding mode, set for the loop - which is floor() in one
 * instruction, and the range is checked on the integer it gives. What comes
 * before the conversion is exact in any rounding mode (a float times a power
 * of two, plus a half, far inside 24 bits), so every result is Quantise's;
 * PackSelfTest checks that on the console itself before it is used.
 *
 * It also measures the chunk on the packed integers (min/max of each axis, in
 * 1/512 tile): what the GPU draws, and no float comparison either.
 */
typedef struct
{
    int32_t lo[3], hi[3];
} PackBounds;

static void GrowBounds(PackBounds *bounds, const VoxelGpuVertex *out)
{
    int32_t q[3] = {out->x, out->y, out->z};

    for (int a = 0; a < 3; ++a)
    {
        if (q[a] < bounds->lo[a]) bounds->lo[a] = q[a];
        if (q[a] > bounds->hi[a]) bounds->hi[a] = q[a];
    }
}

#if defined(__arm__)
#define VOXEL_HAVE_FAST_PACK 1
static bool sFastPack;

static inline uint32_t FpscrFloor(void)
{
    uint32_t old, mode;

    __asm__ volatile ("vmrs %0, fpscr" : "=r"(old));
    mode = (old & ~(3u << 22)) | (2u << 22); /* RMode: towards minus infinity */
    __asm__ volatile ("vmsr fpscr, %0" : : "r"(mode));
    return old;
}

static inline void FpscrRestore(uint32_t old)
{
    __asm__ volatile ("vmsr fpscr, %0" : : "r"(old));
}

static inline int32_t ConvertFloor(float f)
{
    float r;
    int32_t i;

    __asm__ volatile ("vcvtr.s32.f32 %0, %1" : "=t"(r) : "t"(f));
    __asm__ volatile ("vmov %0, %1" : "=r"(i) : "t"(r));
    return i;
}

static inline int16_t Clamp16(int32_t i, float value, char axis)
{
    if (i > 32767 || i < -32768)
    {
        ++sPackErrors;
        if (fabsf(value) > fabsf(sPackWorst))
            sPackWorst = value;
        if (sPackAxis == 0)
            sPackAxis = axis;
        return i > 0 ? 32767 : -32768;
    }
    return (int16_t)i;
}

/* Never inlined: nothing of the caller's arithmetic may land between the two
 * changes of rounding mode. */
static __attribute__((noinline)) void PackFloor(const VoxelVertex *src, unsigned count,
                                                VoxelGpuVertex *dst, PackBounds *bounds)
{
    uint32_t fpscr = FpscrFloor();

    for (unsigned i = 0; i < count; ++i)
    {
        VoxelVertex v;
        VoxelGpuVertex out;

        /* By memcpy both ways: the two formats may share the buffer. */
        memcpy(&v, &src[i], sizeof(v));
        out.u = v.u;
        out.v = v.v;
        out.x = Clamp16(ConvertFloor(v.x * VOXEL_POS_SCALE + 0.5f), v.x, 'p');
        out.y = Clamp16(ConvertFloor(v.y * VOXEL_POS_SCALE + 0.5f), v.y, 'p');
        out.z = Clamp16(ConvertFloor(v.z * VOXEL_POS_SCALE + 0.5f), v.z, 'p');
        out.shade = Clamp16(ConvertFloor(v.shade * VOXEL_SHADE_SCALE + 0.5f), v.shade, 's');
        memcpy(&dst[i], &out, sizeof(out));
        if (bounds != NULL)
            GrowBounds(bounds, &out);
    }
    FpscrRestore(fpscr);
}

/*
 * Quantise against PackFloor on the console: exact halves and the values
 * next to them, both signs, the range's ends and past them, and a spread.
 * Any disagreement keeps the old packing.
 */
static void PackSelfTest(void)
{
    unsigned mismatches = 0, tested = 0;
    uint32_t seed = 12345u;

    sFastPack = false;
    for (unsigned i = 0; i < 6144; ++i)
    {
        VoxelVertex v;
        VoxelGpuVertex a, b;
        float values[4];

        for (int k = 0; k < 4; ++k)
        {
            int32_t step;

            seed = seed * 1664525u + 1013904223u;
            step = (int32_t)(seed >> 8) % 80000 - 40000; /* past both ends */
            if (i % 3 == 0)
                values[k] = ((float)step + 0.5f) / VOXEL_POS_SCALE;  /* a half */
            else if (i % 3 == 1)
                values[k] = nextafterf(((float)step + 0.5f) / VOXEL_POS_SCALE,
                                       (seed & 1) ? 1000.0f : -1000.0f);
            else
                values[k] = (float)(int32_t)seed / 2147483648.0f * 70.0f;
        }
        v.x = values[0];
        v.y = values[1];
        v.z = values[2];
        /* The shade's scale is 32 times the position's. */
        v.shade = values[3] / 32.0f;
        v.u = values[1];
        v.v = values[2];
        Pack(&v, 1, &a);
        PackFloor(&v, 1, &b, NULL);
        ++tested;
        if (memcmp(&a, &b, sizeof(a)) != 0)
            ++mismatches;
    }
    sPackErrors = 0;
    sPackWorst = 0.0f;
    sPackAxis = 0;
    sFastPack = mismatches == 0;
    CtrLog_Write(mismatches == 0 ? CTR_LOG_VIDEO : CTR_LOG_ERROR,
                 "VOXEL pack self-test: %u of %u vertices differ; %s packing", mismatches, tested,
                 sFastPack ? "fast" : "old");
}
#endif

/* Packs [first, first + count) of the scratch in place, growing `bounds`. */
static void PackRange(VoxelVertex *vertices, unsigned first, unsigned count, PackBounds *bounds)
{
    VoxelGpuVertex *packed = (VoxelGpuVertex *)(void *)vertices;

#ifdef VOXEL_HAVE_FAST_PACK
    if (sFastPack)
    {
        PackFloor(vertices + first, count, packed + first, bounds);
        return;
    }
#endif
    for (unsigned i = first; i < first + count; ++i)
    {
        VoxelVertex v;
        VoxelGpuVertex out;

        memcpy(&v, &vertices[i], sizeof(v));
        Pack(&v, 1, &out);
        memcpy(&packed[i], &out, sizeof(out));
        GrowBounds(bounds, &out);
    }
}

/* â”€â”€ Static chunks â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * The map is cut into squares of VOXEL_CHUNK tiles and each square is meshed
 * once, into VRAM, relative to its own corner. Walking brings new squares into
 * view; the ones already built are drawn where they are.
 *
 * Relative to the map rather than to the world, because crossing a border
 * moves every map on screen: the map's position becomes a translation at draw
 * time, so a crossing costs one matrix instead of every chunk of every map.
 *
 * The belt of border metatiles outside every map is cut the same way, on a
 * grid of the current map, and cached like any other chunk. It used to be one
 * mesh of the whole view, rebuilt - shadows and all - whenever the view moved
 * two tiles, which near a town's edge was a spike every other step.
 *
 * A chunk is rebuilt only when what it was built from changes: its tiles and
 * the casters that shade it (a signature of both), the tilesets, or its
 * atlas. The signature itself is only recomputed when the world has changed
 * at all - see the world epoch below.
 */
#define VOXEL_CHUNK 8
#define VOXEL_CHUNK_SLOTS 128u
/* A bit for every atlas id (a chunk's UV key). */
#define VOXEL_UV_WORDS ((VOXEL_METATILE_IDS + 31u) / 32u)
/*
 * The meshes' own block of VRAM, claimed whole at start-up and cut by
 * voxel_arena.c: taken piece by piece from the console's allocator, a few
 * thousand variable-sized meshes left VRAM in crumbs that no building page or
 * atlas could be placed in. 96K vertices: Rustboro's view alone draws 54K, and
 * the prefetch ring around it has to fit as well.
 */
#define VOXEL_CHUNK_VRAM_BUDGET (1536u * 1024u)

/* A chunk build's crown cards, for its trees' shadows (TreeShadowCards). */
#define VOXEL_JOB_CARDS 768u
static VoxelTreeCard sJobCards[VOXEL_JOB_CARDS];

static unsigned TreeShadowCards(VoxelBuilder *builder);

/* HIGH notes the crowns (VoxelTree_NoteCards): the room for them, else 0. */
static unsigned TreeCardsWanted(void)
{
#if CTR_VOXEL_LIGHTING
    return VoxelLighting_Quality() == VOXEL_SHADOWS_HIGH && sBuilder.lighting ? VOXEL_JOB_CARDS : 0u;
#else
    return 0u;
#endif
}

static void TreeCrowns(void);

/*
 * A chunk's ground in four parts, a quarter of its square each, by where a
 * triangle's middle stands: north-west, north-east, south-east, south-west,
 * in that order within every atlas page, so that the two a screen edge
 * leaves in sight are one run of vertices (but for the west pair). A chunk
 * is on the draw list with a corner in the picture, and on an Old 3DS the
 * ground's vertices are most of what the GPU works on (4 of its 9 ms a
 * picture on Route 112, three quarters of them out of the picture): only the
 * parts whose own box meets the frustum are drawn (Draw, ChunkPartsSeen).
 * `end` is where each of the first three ends, in vertices from the chunk's
 * start; `box` is each part's, over every page, in tiles from the chunk's
 * corner and heights in the world (x0 y0 w0 x1 y1 w1; x0 > x1: empty).
 *
 * w is z - y, the row a point stands on: the relief sets what is lifted as
 * far south as it is high, so a hillside is a slab leaning south, a part is
 * told by its rows and not by its z, and a box of x, y and z round it would
 * be mostly air (on Route 112 it kept 37k of 50k vertices where this keeps
 * far fewer). The frustum's planes are linear in x, y and w all the same
 * (SlabVisible).
 */
#define VOXEL_PARTS 4u
#define VOXEL_PARTS_ALL ((1u << VOXEL_PARTS) - 1u)

typedef struct
{
    bool on; /* cut in parts (a draft is not: drawn whole) */
    uint16_t end[VOXEL_ATLAS_PAGES][VOXEL_PARTS - 1u];
    float box[VOXEL_PARTS][6];
} VoxelChunkParts;

typedef struct
{
    bool used, border;
    int mapGroup, mapNum; /* a border chunk carries the current map's */
    int cx, cy;           /* chunk coordinates, local to the map or the belt's grid */
    uint32_t beltGrid;    /* a border chunk's grid (sBeltGrid) */
    /* A border chunk's tiles outside every map when built, and whether that
     * still held in epoch `openEpoch` (BeltCoverSame). */
    unsigned openTiles;
    uint32_t openEpoch;
    bool openSame;
    uint32_t hash, epoch;
    /* Found stale in epoch `staleEpoch` with signature `staleHash`: not
     * hashed again every frame it waits for its rebuild. */
    uint32_t staleHash, staleEpoch;
    const void *primary, *secondary, *layout;
    VoxelAtlasSlot *atlas;
    uint32_t atlasGeneration;
    VoxelGpuVertex *vram;
    unsigned bytes, count, terrainCount;
    unsigned terrainFirst[VOXEL_ATLAS_PAGES + 1u];
    VoxelChunkParts parts;
    unsigned buildingFirst; /* trees are [terrainCount, buildingFirst) */
    /* the last of the trees' vertices: their crowns turned to the sun, drawn
     * only into HIGH's shadow map (TreeShadowCards) */
    unsigned treeShadow;
    /* ... and before those, HIGH's crowns, after their grounds: drawn out of
     * the map's reach (VoxelTree_NoteCards) */
    unsigned treeCrowns;
    int buildingPage;       /* texture page of [buildingFirst, count) */
    uint32_t stamp;         /* last frame the view or the prefetch ring saw it */
    uint32_t viewStamp;     /* last frame it was on screen */
    /* Tiles its geometry covers, relative to the chunk corner: more than the
     * chunk's own square wherever a building or a crown reaches past it. */
    int gx0, gz0, gx1, gz1;
    /* ... and the heights it spans, in the world: with the square above, the
     * box the view is tested against (SiteVisible). */
    float gy0, gy1;
    float buildMs;          /* what its last build cost */
    /* Built with ids its atlas lacked, against this extension of it. */
    bool uncovered;
    uint32_t atlasExtension;
    /* Only the draft of the square (DraftChunk): its ground laid flat, drawn
     * until the real build replaces it. Always asked for again. */
    bool draft;
    /* Static models it places (voxel_building.c): drawn from their own
     * meshes, each at its spot in the chunk's frame (DrawStatics). */
    VoxelStaticDraw statics[VOXEL_CHUNK_STATICS];
    unsigned staticCount;
    /*
     * Its UV key (UvKeyOf): the slots of the ids its build read, over the
     * ids in `uvIds`. A new atlas of its pair whose slots for those ids are
     * the same draws it as it is (ChunkAdopt) instead of its being built
     * again. uvKnown false: no key to keep it by - built with ids its atlas
     * lacked, or with the table changing under the build, or a draft.
     */
    bool uvKnown;
    uint32_t uvKey[2];
    uint32_t uvIds[VOXEL_UV_WORDS];
    /* The atlas, as of its generation, its key was last found not to fit:
     * not worked out again until either changes. */
    const VoxelAtlasSlot *uvTried;
    uint32_t uvTriedGeneration;
    /* What else its vertices were made with, for its cold mesh's key
     * (ColdMesh): its map's base, south ease and whether buildings stood,
     * and where in the world its square lay - the current map is the
     * origin, and the lighting works in world coordinates: built from
     * another map, a shade may round a step apart. */
    float builtBase, builtStretch[3];
    bool builtBuildings;
    int builtX, builtY;
    uint32_t sum;           /* its packed vertices' checksum (SumVertices) */
    /* An audit owed (AuditWanted): built again while drawn as it is, the new
     * mesh's checksum compared with `auditSum` when built from the same
     * world (`auditHash`) and the same slots (`auditKey`). */
    uint8_t audit;
    uint32_t auditSum, auditHash, auditKey[2];
} VoxelChunk;

static VoxelChunk sChunks[VOXEL_CHUNK_SLOTS];
/*
 * The belt's grid: where its square 0,0 lies in the world, and which grid it
 * is. A crossing moves the world under the player, and the grid with it, so
 * the belt squares on screen are the same squares after it, whichever map is
 * current; cut on the new map's origin instead, every one of them was a new
 * square at once and the edges of the view went black until the belt was
 * built again. A cut starts a new grid.
 */
static int sBeltOriginX, sBeltOriginY;
static uint32_t sBeltGrid;
static void *sChunkBlock;
static VoxelArena sChunkArena;
/* One more than the slots: a rebuild takes its new block before it frees the
 * old one. */
static VoxelArenaPiece sChunkPieces[VOXEL_CHUNK_SLOTS + 1];

/* What is drawn this frame. */
#define VOXEL_DRAW_MAX 160u

typedef struct
{
    const VoxelChunk *chunk;
    int worldX, worldZ; /* the chunk corner's place in the world this frame */
    uint8_t parts;      /* which of its ground's parts are in the picture (VoxelChunkParts) */
} VoxelChunkDraw;

static VoxelChunkDraw sDraws[VOXEL_DRAW_MAX];
static unsigned sDrawCount;

/*
 * What the camera sees is worked out from the camera itself, every frame: the
 * four side planes of its frustum, and the rectangle of tiles they reach over
 * the ground. It used to be a fixed window (14 tiles north, 6 south, 14 each
 * side) measured for a 40-degree pitch at the default zoom; flatter or further
 * out - the player's own options - the camera saw past it, and the far end of
 * the picture was the clear colour until a step brought its chunks in. Every
 * chunk is now drawn if and only if its box meets the frustum (the square
 * corners behind the camera's trapezoid are no longer sent to the GPU), and
 * built ahead of time if it lies within a chunk of what the camera reaches.
 */
typedef struct
{
    float x, y, z, w;
} VoxelPlane;

static VoxelPlane sFrustum[4];
static bool sFrustumValid;
/* The tiles the frustum reaches over the ground this frame: x0, z0, x1, z1. */
static int sViewRect[4];
/* Tiles from the player to where they will be shortly (TrackMotion). */
static float sLeadX, sLeadZ;

/* Tiles of view beyond what the frustum reaches, for the camera's easing. */
#define VOXEL_WINDOW_SLACK 1
/* The rectangle never grows beyond this many tiles from the camera's target,
 * whatever the camera: a bound on the chunks one frame visits. */
#define VOXEL_VIEW_REACH_MAX 44
/*
 * The heights a square not yet built is assumed to span, over its map's base,
 * when asking whether it is on screen: ground and trees. A taller model is
 * found once its chunk is built (it is inside the prefetch ring anyway), and
 * then drawn by its real box.
 */
#define VOXEL_UNBUILT_LOW (-1.0f)
#define VOXEL_UNBUILT_HIGH 2.5f
/*
 * Chunks this far outside the view are built ahead of time, with whatever the
 * frame has to spare, so that by the time a step brings them in they are
 * already there. One chunk: the player never outwalks it.
 */
#define VOXEL_PREFETCH VOXEL_CHUNK
/* Every map the view or the ring around it can reach from the current map is
 * one of the instances (voxel_world.c): a map left out is the belt, or black,
 * wherever it lies on screen. */
_Static_assert(VOXEL_WORLD_REACH >= VOXEL_VIEW_REACH_MAX + VOXEL_WINDOW_SLACK + VOXEL_PREFETCH,
               "the world's maps reach as far as the view and its ring");
/* Tiles past the view whose metatiles an atlas is composed for: a whole route, not
 * the 24 around the view, whose edge ids arrived by extension - a few frames of
 * black cells each - as the player walked towards them. */
#define VOXEL_MATERIAL_MARGIN 64

/* The camera's matrices, as the draw uses them: `fit` folds in the logical
 * surface (FitToLogicalSurface), which the frustum must not see; `eye` is
 * the eye of a stereo pair (0 left, 1 right) or -1 for the middle camera. */
static void FitToLogicalSurface(C3D_Mtx *mtx);
/* This frame's 3D slider (0: one picture) and the eye the last Draw was for
 * (-1: the middle camera), which places what is laid over the world; see
 * the stereoscopy section below. */
static float sStereoSlider;
static int sDrawEye = -1;
static float StereoIod(float *screen);
static float CameraTanX(void);
/* The frustum the chunks are picked by (UpdateFrustum) is widened by this
 * many times the pair's parallax of the far distance: what an eye sees past
 * the middle camera's edge, with room to spare. */
#define VOXEL_STEREO_CULL_SLACK 4.0f

static void CameraMatrices(C3D_Mtx *projection, C3D_Mtx *view, bool fit, int eye)
{
    float screen, iod = eye >= 0 && sStereoSlider > 0.0f ? StereoIod(&screen) : 0.0f;

    Mtx_Persp(projection, C3D_AngleFromDegrees(sCamera.fov),
              (float)CTR_GAME_WIDTH / (float)CTR_GAME_HEIGHT,
              VOXEL_NEAR, VOXEL_FAR, false);
    if (iod > 0.0f)
    {
        /*
         * One eye of the pair: the camera moved half the interocular
         * distance sideways, and the frustum skewed back so that the plane
         * `screen` in front of it lands where the middle camera puts it.
         * In clip x, with the view looking down -z: x - e, plus (-z) e /
         * screen, over the tangent of half the horizontal field of view.
         * (Mtx_PerspStereo scales the skew by that tangent but not the
         * move, which put the plane of zero parallax 0.6 of the way.)
         */
        float e = (eye == 0 ? -0.5f : 0.5f) * iod / CameraTanX();

        projection->r[0].z = -e / screen;
        projection->r[0].w = -e;
    }
    if (sSwoop > 0.0f)
    {
        /*
         * The swoop to the 2D field (CtrVoxel_SetSwoop): the projection
         * blends, element by element, into an orthographic one of the 2D
         * field's extent - 25 by 15 tiles of ground - which is the GBA's
         * own art, an oblique view with no perspective.
         * The camera stands where the perspective ground framing is the same
         * (VoxelCamera_Swoop), so the ground holds still and only the walls
         * and the trees lose their splay. The eye's skew carries over: in
         * the orthographic end it is a parallax linear in depth, which is
         * what a flat stereo pair wants.
         */
        C3D_Mtx ortho;
        float k = sSwoop > 1.0f ? 1.0f : sSwoop;

        /* The 2D field's extent of ground: its half height foreshortened by
         * the pitch, so that the ground comes out square, 1:1, and a tile of
         * height a tile up the picture at 45 degrees (VOXEL_SWOOP_PITCH). */
        float halfH = VOXEL_SWOOP_HALF_H * sinf(C3D_AngleFromDegrees(VOXEL_SWOOP_PITCH));

        Mtx_Ortho(&ortho, -VOXEL_SWOOP_HALF_W, VOXEL_SWOOP_HALF_W, -halfH, halfH,
                  VOXEL_NEAR, VOXEL_FAR, false);
        if (iod > 0.0f)
        {
            float e = (eye == 0 ? -0.5f : 0.5f) * iod / VOXEL_SWOOP_HALF_W;

            ortho.r[0].z = -e / screen;
            ortho.r[0].w = -e;
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                projection->r[r].c[c] = projection->r[r].c[c] * (1.0f - k) + ortho.r[r].c[c] * k;
    }
    if (fit)
        FitToLogicalSurface(projection);
    Mtx_LookAt(view,
               FVec3_New(sCamera.x, sCamera.y, sCamera.z),
               FVec3_New(sCamera.targetX, sCamera.targetY, sCamera.targetZ),
               FVec3_New(0.0f, 1.0f, 0.0f), false);
    if (sSwoopHeight < 1.0f)
    {
        /* The pop-up book (CtrVoxel_SetSwoop): the world squashed towards
         * the ground under the player, and sheared north as it goes
         * (VOXEL_SWOOP_SHEAR), so that flat it is the 2D map seen from
         * above. Folded into the view, so every draw takes it. */
        float h = sSwoopHeight > 0.002f ? sSwoopHeight : 0.002f;
        C3D_Mtx squash, out;

        Mtx_Identity(&squash);
        squash.r[1].y = h;
        squash.r[2].y = -(1.0f - h) * VOXEL_SWOOP_SHEAR;
        Mtx_Translate(view, 0.0f, sCamera.ground, 0.0f, true);
        Mtx_Multiply(&out, view, &squash);
        Mtx_Copy(view, &out);
        Mtx_Translate(view, 0.0f, -sCamera.ground, 0.0f, true);
    }
}

static float StereoParallax(int eye, float depth);

/* Where a world point lands on the logical surface as the last Draw's eye
 * saw it, false behind the camera. */
static bool ProjectWorldPoint(const C3D_Mtx *view, float x, float y, float z, float *screenX, float *screenY)
{
    float vx = view->r[0].x * x + view->r[0].y * y + view->r[0].z * z + view->r[0].w;
    float vy = view->r[1].x * x + view->r[1].y * y + view->r[1].z * z + view->r[1].w;
    float vz = view->r[2].x * x + view->r[2].y * y + view->r[2].z * z + view->r[2].w;
    float tanY;

    if (vz > -VOXEL_NEAR)
        return false;
    tanY = tanf(C3D_AngleFromDegrees(sCamera.fov) * 0.5f);
    *screenX = (vx / -vz / (tanY * (float)CTR_GAME_WIDTH / (float)CTR_GAME_HEIGHT) + 1.0f) * 0.5f * CTR_GAME_WIDTH
             + StereoParallax(sDrawEye, -vz);
    *screenY = (1.0f - vy / -vz / tanY) * 0.5f * CTR_GAME_HEIGHT;
    return true;
}

bool CtrVoxel_ProjectPictureTile(float tileX, float tileY, float *screenX, float *screenY)
{
    C3D_Mtx projection, view;
    float playerX, playerZ;

    if (!sReady)
        return false;
    VoxelEntities_GetPlayerWorldPos(&playerX, &playerZ);
    CameraMatrices(&projection, &view, false, sDrawEye);
    /* A tile up in the air: seen from above, what stands a tile high is a
     * tile further up the picture than the floor under it. */
    return ProjectWorldPoint(&view, playerX + 0.5f + tileX, sCamera.ground + 1.0f,
                             playerZ + 0.5f + tileY + 1.0f, screenX, screenY);
}

bool CtrVoxel_PlayerLightSpot(float *x, float *y, float *scaleX, float *scaleY)
{
    C3D_Mtx projection, view;
    float playerX, playerZ, cx, cz, x0, y0, x1, y1;

    if (!sReady)
        return false;
    VoxelEntities_GetPlayerWorldPos(&playerX, &playerZ);
    CameraMatrices(&projection, &view, false, sDrawEye);
    cx = playerX + 0.5f;
    cz = playerZ + 0.5f;
    /* Half a tile up the player: the middle of the GBA's circle is the
     * middle of the player's metatile, which is where the body stands. */
    if (!ProjectWorldPoint(&view, cx, sCamera.ground + 0.5f, cz, x, y))
        return false;
    /* The ground a GBA pixel covers, across and (foreshortened) down. */
    if (!ProjectWorldPoint(&view, cx - 1.0f, sCamera.ground, cz, &x0, &y0)
     || !ProjectWorldPoint(&view, cx + 1.0f, sCamera.ground, cz, &x1, &y1))
        return false;
    *scaleX = fabsf(x1 - x0) / 32.0f;
    if (!ProjectWorldPoint(&view, cx, sCamera.ground, cz - 1.0f, &x0, &y0)
     || !ProjectWorldPoint(&view, cx, sCamera.ground, cz + 1.0f, &x1, &y1))
        return false;
    *scaleY = fabsf(y1 - y0) / 32.0f;
    return *scaleX > 0.01f && *scaleY > 0.01f;
}

static void GrowRect(float *rect, float x, float z)
{
    if (x < rect[0]) rect[0] = x;
    if (z < rect[1]) rect[1] = z;
    if (x > rect[2]) rect[2] = x;
    if (z > rect[3]) rect[3] = z;
}

/*
 * The frustum's side planes (clip-space w +- x and w +- y, which all pass
 * through the eye: together they are the pyramid in front of it, so no near
 * plane is needed), and the rectangle its four corner rays sweep between the
 * lowest and the highest ground on screen.
 */
static void UpdateFrustum(void)
{
    C3D_Mtx projection, view, clip;
    float tanY = tanf(C3D_AngleFromDegrees(sCamera.fov) * 0.5f);
    float tanX = tanY * (float)CTR_GAME_WIDTH / (float)CTR_GAME_HEIGHT;
    float tallest = VoxelBuildings_MaxTop();
    float heights[2];
    float rect[4] = {sCamera.targetX, sCamera.targetZ, sCamera.targetX, sCamera.targetZ};
    const C3D_FVec *r;

    CameraMatrices(&projection, &view, false, -1);
    /* With the slider up each eye sees past the middle camera's side edges:
     * the frustum is widened by what the pair's parallax can reach. */
    if (sStereoSlider > 0.0f)
    {
        float screen, iod = StereoIod(&screen);
        float widen = 1.0f + VOXEL_STEREO_CULL_SLACK * 0.5f * iod / (screen * tanX);

        Mtx_Persp(&projection, C3D_AngleFromDegrees(sCamera.fov),
                  widen * (float)CTR_GAME_WIDTH / (float)CTR_GAME_HEIGHT,
                  VOXEL_NEAR, VOXEL_FAR, false);
        tanX *= widen;
    }
    Mtx_Multiply(&clip, &projection, &view);
    r = clip.r;
    for (int i = 0; i < 2; ++i)
    {
        float s = i == 0 ? 1.0f : -1.0f;

        sFrustum[i].x = r[3].x + s * r[0].x;
        sFrustum[i].y = r[3].y + s * r[0].y;
        sFrustum[i].z = r[3].z + s * r[0].z;
        sFrustum[i].w = r[3].w + s * r[0].w;
        sFrustum[2 + i].x = r[3].x + s * r[1].x;
        sFrustum[2 + i].y = r[3].y + s * r[1].y;
        sFrustum[2 + i].z = r[3].z + s * r[1].z;
        sFrustum[2 + i].w = r[3].w + s * r[1].w;
    }
    sFrustumValid = true;

    /* Ground a few tiles under the player's (a terrace below, the sea) up to
     * the tallest model's roof: what stands high is seen from further. */
    if (tallest < 3.0f)
        tallest = 3.0f;
    if (tallest > 6.0f)
        tallest = 6.0f;
    heights[0] = sCamera.ground - 2.0f;
    heights[1] = sCamera.ground + tallest;
    /* The view's rows: right, up, and back towards the eye. */
    for (int sy = -1; sy <= 1; sy += 2)
        for (int sx = -1; sx <= 1; sx += 2)
        {
            float dx = -view.r[2].x + sx * tanX * view.r[0].x + sy * tanY * view.r[1].x;
            float dy = -view.r[2].y + sx * tanX * view.r[0].y + sy * tanY * view.r[1].y;
            float dz = -view.r[2].z + sx * tanX * view.r[0].z + sy * tanY * view.r[1].z;

            for (int h = 0; h < 2; ++h)
            {
                float t = dy < -0.001f ? (heights[h] - sCamera.y) / dy : -1.0f;

                /* A ray that never comes down to that height (or does so
                 * behind the eye) is followed out to the bound instead. */
                if (t < 0.0f || t > 4.0f * VOXEL_VIEW_REACH_MAX)
                    t = 4.0f * VOXEL_VIEW_REACH_MAX;
                GrowRect(rect, sCamera.x + dx * t, sCamera.z + dz * t);
            }
        }
    if (sSwoop > 0.0f)
    {
        /* The swoop's orthographic end sees the 2D field's whole extent at
         * every depth, which the perspective rays above narrow down. */
        GrowRect(rect, sCamera.targetX - VOXEL_SWOOP_HALF_W - 2.0f, sCamera.targetZ - VOXEL_SWOOP_HALF_H - 4.0f);
        GrowRect(rect, sCamera.targetX + VOXEL_SWOOP_HALF_W + 2.0f, sCamera.targetZ + VOXEL_SWOOP_HALF_H + 2.0f);
    }
    {
        float lo[2] = {sCamera.targetX - VOXEL_VIEW_REACH_MAX, sCamera.targetZ - VOXEL_VIEW_REACH_MAX};
        float hi[2] = {sCamera.targetX + VOXEL_VIEW_REACH_MAX, sCamera.targetZ + VOXEL_VIEW_REACH_MAX};

        for (int k = 0; k < 2; ++k)
        {
            rect[k] = rect[k] < lo[k] ? lo[k] : rect[k];
            rect[k + 2] = rect[k + 2] > hi[k] ? hi[k] : rect[k + 2];
        }
    }
    sViewRect[0] = (int)floorf(rect[0]) - VOXEL_WINDOW_SLACK;
    sViewRect[1] = (int)floorf(rect[1]) - VOXEL_WINDOW_SLACK;
    sViewRect[2] = (int)ceilf(rect[2]) + VOXEL_WINDOW_SLACK;
    sViewRect[3] = (int)ceilf(rect[3]) + VOXEL_WINDOW_SLACK;
    sViewSquares = (unsigned)((sViewRect[2] - sViewRect[0] + VOXEL_CHUNK - 1) / VOXEL_CHUNK)
                 * (unsigned)((sViewRect[3] - sViewRect[1] + VOXEL_CHUNK - 1) / VOXEL_CHUNK);
}

/* Does the box meet the frustum? Outside only if wholly behind one plane. */
static bool BoxVisible(float x0, float y0, float z0, float x1, float y1, float z1)
{
    if (!sFrustumValid)
        return true;
    for (int i = 0; i < 4; ++i)
    {
        const VoxelPlane *p = &sFrustum[i];
        float x = p->x > 0.0f ? x1 : x0, y = p->y > 0.0f ? y1 : y0, z = p->z > 0.0f ? z1 : z0;

        if (p->x * x + p->y * y + p->z * z + p->w < 0.0f)
            return false;
    }
    return true;
}

/*
 * World epoch. Bumped when a live tile changes (the digest of the live grid
 * moves) or the set of maps on screen does (a crossing, a warp). Everything
 * cached against the world - chunk signatures, the lighting caches - is
 * checked again only when it has moved. A chunk remembers the epoch it was
 * last verified in.
 */
static uint32_t sEpoch = 1;
static uint32_t sLiveDigest, sInstanceSignature;
/* The map version last seen (3ds_map_events.c), whether sLiveDigest holds a
 * hash yet, and the writes the version did not announce. */
#define VOXEL_MAP_CHECK_FRAMES 60u
static uint32_t sMapVersion, sUnannounced;
static bool sDigestKnown;

static int sMeshMapGroup = -1, sMeshMapNum = -1;
/* Worst overflow logged so far: reporting only the first one hid how far
 * over budget a town really goes, and whether a change has helped. */
static unsigned sWorstDropped;
static bool sOverflowReported;

/* Time the previous Update spent on builds, so the next budget can tell the
 * frame's own cost from what this module added to it. */
static float sLastBuildMs;

static C3D_Tex sSpriteAtlas;
static C3D_Tex sTreeAtlas;
/* The modelled buildings' own art, ground made transparent. Empty when
 * buildings.bin is absent, and then the houses are their cells' flat drawing. */
static bool sHaveBuildings;
/* The world's VRAM given back while it is off (CtrVoxel_ParkVram); the
 * buildings' models stay loaded for when it comes back. */
static bool sParked, sBuildingsLoaded;
/* Unloaded by CtrVoxel_UnparkVram for want of the pages' VRAM (loaded again
 * at an unpark that finds it), and the lighting still to forget them. */
static bool sBuildingsDropped, sBuildingsChanged;
static unsigned sSpriteVertices;
static unsigned sReflectionVertices;
static unsigned sLampVertices, sLampGroundVertices;
static bool sLampReady;
static VoxelVertex *sLampScratch;     /* builder output, VOXEL_LAMP_VERTICES */
static VoxelGpuVertex *sLampGpu;      /* linear, VOXEL_LAMP_VERTICES */
static uint64_t sLampDrawn;           /* tick of the last draw from sLampGpu */
static uint32_t sLampRetry;           /* no buffers asked for before this frame */
static bool sLampNoRoomLogged;
static C3D_Tex sLampPoolTex;          /* a pool's falloff (MakeLampPool), with the buffers */
static void LampsRelease(bool now);
static uint32_t sLampSignature, sLampDigest;
static int sLampX, sLampZ;             /* the lamps' own origin (drawn from it) */
static int sLampView[4];               /* the rectangle they were built over */
/*
 * Built over the view and this many tiles more on each side, and built again
 * only once the view leaves that (or the maps or their cells change). Built
 * over the view alone, every tile the camera moved at night rebuilt every
 * lamp of every map on screen - each pool walking its cells for relief and
 * light - and a crossing in a lit town was a dropped frame (port.log,
 * 2026-10-15: sprites=6-9 ms on the crossing frames). Should the wider
 * rectangle fill the buffers, the lamps are built over the view as before.
 */
#define VOXEL_LAMP_PAD 8
#if CTR_VOXEL_LIGHTING
static unsigned sShadowVertices;
#endif
static int sDynamicX, sDynamicZ; /* the billboards' origin this frame */

/* â”€â”€ Building pages â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * The buildings' texture pages, one per map, in VRAM. The pages of the maps on
 * screen are what is needed - the current map and its connections - so four
 * are cached and the least recently used is reused, never one drawn in the
 * last two frames (its draws may still be queued).
 *
 * A page is up to 512 KiB of RomFS. Read on the render thread, as it used to
 * be, a slice of it stalled the frame for as long as the SD card took, which
 * an emulator never shows. A worker thread reads the slices instead, into a
 * linear buffer the GPU then copies into the page: VRAM is not the CPU's to
 * write (a data abort on hardware), and the render thread only ever queues
 * the copy. The worker runs below the game's priority, so it takes the CPU
 * only while the game is waiting for the display - and a read spends almost
 * all of its time waiting for the card anyway.
 */
#define VOXEL_BUILDING_PAGES 4
#define VOXEL_PAGE_SLICE (64u * 1024u) /* texels per read: 128 KiB */
/* Pages take their whole power-of-two texture (PageBytes): the Battle
 * Frontier's two halves, in view together, are 512 KiB each. 64 KiB more
 * for the small page drawn beside two of them: on Route 110 by Slateport
 * its page 23 (512 KiB padded), Slateport's page 1 (512 KiB) and a 16 KiB
 * one are all on screen, and with 1 MiB Slateport's buildings waited for the
 * small one to leave the view. Paid for by the top screens' depth buffers
 * (TopScreenTarget, 375 KiB). */
#define VOXEL_PAGE_VRAM_BUDGET (1088u * 1024u)
/* Atlas pages that may borrow a gap of the pages' arena (AtlasPageBorrow). */
#define VOXEL_ATLAS_BORROW_MAX 2u
/* The arena's pieces: a page's texture and its window mask, the borrowed
 * atlas pages, and the gap lent to the battle. */
#define VOXEL_PAGE_PIECES (VOXEL_BUILDING_PAGES * 2u + VOXEL_ATLAS_BORROW_MAX + 1u)
/* A page that found no room is asked for again only after this long. */
#define VOXEL_PAGE_RETRY_FRAMES 120u

typedef struct
{
    int page;           /* -1: empty */
    unsigned w, h;
    unsigned loaded;    /* texels copied so far; w*h when ready */
    C3D_Tex tex;
    uint32_t used;      /* frame last needed */
    /*
     * The page's window-glass mask (buildings.bin's L8 glow), lit at night:
     * a piece of the arena of its own, taken only while windows glow and
     * only where it fits - it never pushes a page out, and gives up its
     * piece before any page has to go. Streamed in after the texels.
     */
    C3D_Tex glow;
    unsigned glowLoaded;  /* mask bytes copied so far; w*h when ready */
    uint32_t glowUsed;    /* frame last drawn from */
    uint32_t glowRetry;   /* no piece asked for again before this frame */
    uint32_t glowDropped; /* frame it stopped being drawn from */
    bool glowDropping;    /* no longer drawn from: freed two frames on */
    uint64_t requested; /* tick it was asked for, for the log */
} BuildingPageSlot;
static BuildingPageSlot sPageSlots[VOXEL_BUILDING_PAGES];
extern uint8_t __C3D_Context[];
#define C3DI_CONTEXT_FLAGS 0x20u
#define C3DI_CONTEXT_TEX 0x118u
#define C3DI_FLAG_TEX(unit) (1u << (23 + (unit)))
static C3D_Tex *sGlowBound;

enum { STREAM_IDLE, STREAM_READING, STREAM_DONE, STREAM_FAILED };

static struct
{
    Thread thread;
    LightEvent wake;
    volatile bool quit;
    volatile int state;
    /* The request, written by the render thread before it wakes the worker. */
    BuildingPageSlot *slot;
    int page;
    unsigned first, count;
    uint16_t *buffer;   /* linear, VOXEL_PAGE_SLICE texels */
    bool glow;          /* the slice is of the page's window mask (bytes) */
    bool copyQueued;    /* the buffer is the source of this frame's copy */
    bool systemCore;    /* the worker runs on the system core */
} sStream;

/* Pages that failed to read are not asked for again: retrying one every
 * frame would only log the same error for as long as its map is on screen.
 * One flag a page, for as many pages as buildings.bin can name: a page to
 * each layout that places anything (gen_voxel_buildings.py, CONSOLE_PAGES),
 * 247 of them with the interiors done, and the caves and dungeons to come.
 * Eleven bytes a page here; a page's texels are read only while its map is
 * on screen. */
#define VOXEL_MAX_PAGES 1024
static bool sBadPages[VOXEL_MAX_PAGES];

/*
 * The pages' block of VRAM, claimed whole at start-up like the meshes' (see
 * VOXEL_CHUNK_VRAM_BUDGET): the 512 KiB page of a city needs 512 KiB in one
 * piece, which a VRAM shared with everything else stopped having. Pages of
 * 256 KiB and more are placed from the bottom, smaller ones from the top, so
 * the small ones never strand a large gap.
 */
static void *sPageBlock;
static VoxelArena sPageArena;
static VoxelArenaPiece sPagePieces[VOXEL_PAGE_PIECES];
static uint32_t sPageRetry[VOXEL_MAX_PAGES];
static uint32_t sPageRingRetry[VOXEL_MAX_PAGES];
static bool sPageNoRoomLogged[VOXEL_MAX_PAGES];
static bool sGlowNoRoomLogged[VOXEL_MAX_PAGES];

static void PageRetriesForget(void)
{
    memset(sPageRetry, 0, sizeof(sPageRetry));
    memset(sPageRingRetry, 0, sizeof(sPageRingRetry));
    memset(sPageNoRoomLogged, 0, sizeof(sPageNoRoomLogged));
    memset(sGlowNoRoomLogged, 0, sizeof(sGlowNoRoomLogged));
}
/* Windows glow (DAY / NIGHT, outdoors, afternoon to dawn): the pages in
 * view want their masks. */
static bool sGlowWanted;
/* Console measurement switches, the scenario settings measure_glow and
 * measure_lamps (on unless set to 0): the night's lit windows and lamps
 * left out, to tell what each costs on the hardware. Read once a second. */
static bool sMeasureGlow = true, sMeasureLamps = true;
/* settings.txt voxel_plain_vs=0: the full vertex program always (A/B measure) */
static bool sMeasurePlain = true;
/* settings.txt voxel_fused_blur=1: the tilt-shift bands drawn by
 * CtrVoxel_DioramaFused instead of blended in four passes over the picture.
 * Off by default: on an Old 3DS (2026-11-10, Route 112) it took 0.2-0.4 ms
 * off the GPU and put 0.2-0.4 ms on the CPU, and the GPU's queue drained at
 * the same time (20.50 against 20.45 ms in stereo). */
static bool sMeasureFusedBlur = false;
/* settings.txt voxel_cut=N: what each part of the picture costs the GPU, told
 * by leaving it out (CtrVoxel_Update). The bits of what is left out now. */
static unsigned sMeasureCutEvery;
static unsigned sMeasurePartsEvery;
static unsigned sMeasureCut;
/* settings.txt voxel_scissor=0: the world drawn over the whole surface, as it
 * was (A/B measure) */
static bool sMeasureScissor = true;
/* The ground drawn by parts (VoxelChunkParts); `voxel_parts=0` draws every
 * chunk whole, as before. */
static bool sMeasureParts = true;
/* settings.txt voxel_ab: frames per configuration of the A/B round, 0 for none */
static unsigned sMeasureAb;
#define VOXEL_SCISSOR_MARGIN 4u
static uint32_t sMeasureRead;

/* A page texture over a piece of the arena. Never C3D_TexDelete'd: that
 * would hand the piece to the console's allocator, which never gave it.
 * The texture is `th` tall, a power of two, and so is its piece (PageBytes):
 * only the page's own rows are streamed in, the rest are never sampled. */
static void PageTexInit(C3D_Tex *tex, void *data, unsigned w, unsigned th)
{
    memset(tex, 0, sizeof(*tex));
    tex->data = data;
    tex->fmt = GPU_RGBA5551;
    tex->size = w * th * sizeof(uint16_t);
    tex->width = (u16)w;
    tex->height = (u16)th;
    tex->param = GPU_TEXTURE_MODE(GPU_TEX_2D);
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

/*
 * An atlas page over a piece of the pages' arena. A battle over the world
 * frames more maps than the field does, with the battle scene's 512 KiB out
 * of VRAM: on Route 110 beside Slateport the battle view wanted a fifth atlas
 * page where VRAM held four, the atlas job waited on it the whole battle and
 * the chunks behind it were never built - parts of the world missing
 * (port.log 2026-10-15: pending=61 for the whole battle). The arena had
 * 480 KiB unused there, as a battle frames few buildings. Borrowed only when
 * VRAM has no room and no atlas off screen can give its own, and moved back
 * to VRAM once there is room (AtlasPagesReturn): the field's buildings want
 * the arena whole.
 */
static bool AtlasPageBorrowed(const C3D_Tex *tex)
{
    const uint8_t *data = (const uint8_t *)tex->data;

    return sPageBlock != NULL && data >= (const uint8_t *)sPageBlock
        && data < (const uint8_t *)sPageBlock + VOXEL_PAGE_VRAM_BUDGET;
}

static void AtlasPageRelease(C3D_Tex *tex)
{
    if (tex->data == NULL)
        return;
    if (AtlasPageBorrowed(tex))
        VoxelArena_Free(&sPageArena, tex->data);
    else
        C3D_TexDelete(tex);
    memset(tex, 0, sizeof(*tex));
}

static unsigned CountBorrowedAtlasPages(void)
{
    unsigned count = 0;

    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
        for (unsigned p = 0; p < VOXEL_ATLAS_PAGES; ++p)
            count += AtlasPageBorrowed(AtlasTex(&sAtlases[i], p));
    return count;
}

/* The gap lent to the battle (CtrVoxel_LendPagesGap), and whether the arena
 * has asked for it back since. */
static void *sPagesLent;
static bool sPagesLentWanted;

/*
 * Something of the arena's found no room: the gap back, if that is what would
 * make it. Asked only then - a voxel battle updates the world all through a
 * move, and a piece that has room, or would have none anyway, is no reason to
 * walk the move's layer tile by tile, or to make its texture again. Whether it
 * was asked; the one asking looks again once the gap is back.
 */
static bool PagesLentAsk(uint32_t bytes)
{
    bool freed[VOXEL_PAGE_PIECES];
    int piece;

    if (sPagesLent == NULL || (piece = VoxelArena_PieceOf(&sPageArena, sPagesLent)) < 0)
        return false;
    memset(freed, 0, sizeof(freed));
    freed[piece] = true;
    if (!VoxelArena_FitsFreeing(&sPageArena, freed, bytes))
        return false;
    sPagesLentWanted = true;
    return true;
}

/* The atlas page last asked for found no room but for the lent gap, now asked
 * back (AtlasPageBorrow): a wait of a frame or two, as a page's or a mask's.
 * Not a want of VRAM: its map is not drawn flat for it, the 3D surfaces are
 * not sent away and the atlas cache is not capped. */
static bool sAtlasWaitsForGap;

static bool AtlasPageBorrow(C3D_Tex *tex)
{
    const uint32_t bytes = VOXEL_ATLAS_PIXELS * sizeof(uint16_t);
    void *block;

    if (sPageBlock == NULL || CountBorrowedAtlasPages() >= VOXEL_ATLAS_BORROW_MAX)
        return false;
    block = VoxelArena_Alloc(&sPageArena, bytes, false);
    if (block == NULL)
    {
        sAtlasWaitsForGap = PagesLentAsk(bytes);
        return false;
    }
    PageTexInit(tex, block, VOXEL_ATLAS_W, VOXEL_ATLAS_H);
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: atlas page borrowed from the building pages' arena "
                 "(VRAM free=%lu, arena gap left %lu KiB)", (unsigned long)vramSpaceFree(),
                 (unsigned long)(VoxelArena_LargestGap(&sPageArena) >> 10));
    return true;
}

void *CtrVoxel_LendPagesGap(uint32_t bytes)
{
    void *block;

    if (sPageBlock == NULL || sPagesLent != NULL || sParked)
        return NULL;
    block = VoxelArena_Alloc(&sPageArena, bytes, false);
    if (block == NULL)
        return NULL;
    sPagesLent = block;
    sPagesLentWanted = false;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: %lu KiB of the building pages' arena lent to the battle "
                 "(arena gap left %lu KiB)", (unsigned long)(bytes >> 10),
                 (unsigned long)(VoxelArena_LargestGap(&sPageArena) >> 10));
    return block;
}

bool CtrVoxel_PagesGapWanted(void)
{
    return sPagesLent != NULL && (sPagesLentWanted || sParked);
}

void CtrVoxel_ReturnPagesGap(void *block)
{
    if (block == NULL || block != sPagesLent)
        return;
    VoxelArena_Free(&sPageArena, block);
    sPagesLent = NULL;
    sPagesLentWanted = false;
}

/*
 * The VRAM a page's texture takes: all `th` rows the GPU is told it has, not
 * only the `h` rows the file holds. The rows past the page are never
 * sampled, so on the console a piece of `h` rows was enough - but an
 * emulator's texture cache (Azahar, Citra) takes the texture's whole range
 * as its own, and over a neighbouring page, or past the arena into another
 * surface, it was given the wrong texels: a map's houses drawn in one flat
 * colour, or in another page's art, now and then, after the menu had moved
 * the VRAM around them.
 */
static unsigned PageBytes(unsigned w, unsigned th)
{
    return w * th * sizeof(uint16_t);
}

/* The stream is reading into this slot, or copying what it read. */
static bool StreamHolds(const BuildingPageSlot *slot)
{
    return sStream.slot == slot && (sStream.state != STREAM_IDLE || sStream.copyQueued);
}

/* Window masks (BuildingPageSlot.glow) */

static void GlowFree(BuildingPageSlot *slot)
{
    if (slot->glow.data != NULL)
        VoxelArena_Free(&sPageArena, slot->glow.data);
    memset(&slot->glow, 0, sizeof(slot->glow));
    slot->glowLoaded = 0;
    slot->glowDropping = false;
}

/* No longer drawn from; its piece goes once no queued draw can read it. */
static void GlowDrop(BuildingPageSlot *slot)
{
    if (slot->glow.data == NULL || slot->glowDropping)
        return;
    slot->glowDropping = true;
    slot->glowDropped = sFrame;
    /* and not asked for again at once: the room was wanted for a page */
    slot->glowRetry = sFrame + VOXEL_PAGE_RETRY_FRAMES;
}

/* Frees the dropped masks drawn from two frames ago at the latest, and with
 * `idle` also those not drawn from in that long. Whether any went. */
static bool GlowRelease(bool idle)
{
    bool freed = false;

    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
    {
        BuildingPageSlot *slot = &sPageSlots[i];
        uint32_t last = slot->glowDropping ? slot->glowDropped : slot->glowUsed;

        if (slot->glow.data == NULL || (!slot->glowDropping && !idle)
         || sFrame - last < 2u || StreamHolds(slot))
            continue;
        GlowFree(slot);
        freed = true;
    }
    return freed;
}

/* Every other slot's mask dropped, for a page in view that finds no room;
 * whether there was any (gone within two frames). */
static bool GlowDropOthers(const BuildingPageSlot *keep)
{
    bool any = false;

    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
        if (&sPageSlots[i] != keep && sPageSlots[i].glow.data != NULL)
        {
            GlowDrop(&sPageSlots[i]);
            any = true;
        }
    return any;
}

/* A page or a mask the lent gap stands in the way of asks again this many
 * frames on: the gap is back by the next frame's preparation. */
#define VOXEL_PAGE_LENT_RETRY_FRAMES 2u

/*
 * A piece for a page's mask, if it has one and windows glow: only from
 * the room there is - a mask never pushes anything out - and when there is
 * none, asked for again a while later. Its windows stay unlit meanwhile. A
 * gap lent to the battle is no reason for that: it comes back (PagesLentAsk).
 */
static void GlowWant(BuildingPageSlot *slot)
{
    unsigned bytes;
    void *block;

    if (!sGlowWanted)
    {
        GlowDrop(slot);
        return;
    }
    if (slot->glow.data != NULL || slot->page < 0 || sFrame < slot->glowRetry
     || !VoxelBuildings_PageHasGlow((unsigned)slot->page))
        return;
    /* as the page's texture: all the rows the GPU is told it has (PageBytes) */
    bytes = slot->w * slot->tex.height;
    block = VoxelArena_Alloc(&sPageArena, bytes, bytes < 256u * 1024u);
    if (block == NULL && PagesLentAsk(bytes))
    {
        slot->glowRetry = sFrame + VOXEL_PAGE_LENT_RETRY_FRAMES;
        return;
    }
    if (block == NULL)
    {
        slot->glowRetry = sFrame + VOXEL_PAGE_RETRY_FRAMES;
        if (slot->page < VOXEL_MAX_PAGES && !sGlowNoRoomLogged[slot->page])
        {
            sGlowNoRoomLogged[slot->page] = true;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: no room yet for building page %d's window mask "
                         "(%u KiB, largest gap %lu KiB): its windows stay unlit until there is",
                         slot->page, bytes >> 10,
                         (unsigned long)(VoxelArena_LargestGap(&sPageArena) >> 10));
        }
        return;
    }
    PageTexInit(&slot->glow, block, slot->w, slot->tex.height);
    slot->glow.fmt = GPU_L8;
    slot->glow.size = bytes;
    slot->glowLoaded = 0;
    slot->glowUsed = sFrame;
    slot->glowDropping = false;
}

/* The mask still to come in, after all of the page's texels. */
static bool GlowPending(const BuildingPageSlot *slot)
{
    return slot->glow.data != NULL && !slot->glowDropping && slot->loaded == slot->w * slot->h
        && slot->glowLoaded < slot->w * slot->h;
}

/* A page's mask to draw its windows with: all of it in and not dropped. */
static C3D_Tex *GlowTex(BuildingPageSlot *slot)
{
    if (slot == NULL || slot->glow.data == NULL || slot->glowDropping
     || slot->glowLoaded < slot->w * slot->h)
        return NULL;
    slot->glowUsed = sFrame;
    return &slot->glow;
}

static void PageFree(BuildingPageSlot *slot)
{
    GlowFree(slot);
    VoxelArena_Free(&sPageArena, slot->tex.data);
    memset(&slot->tex, 0, sizeof(slot->tex));
    slot->page = -1;
    slot->loaded = 0;
}

/* Could this slot's page be dropped now? Not if needed in the last `keep`
 * frames - never fewer than two, its draws may still be queued - nor while
 * the stream uses it. */
static bool PageDroppable(const BuildingPageSlot *slot, uint32_t keep)
{
    return slot->tex.data != NULL && sFrame - slot->used >= (keep > 2u ? keep : 2u)
        && !StreamHolds(slot);
}

/* Asset reads share the page worker; region roles are resident. */
enum { AHEAD_IDLE, AHEAD_READING, AHEAD_DONE };

/* And, one at a time too, an asset payload of a map near the view (see
 * VoxelWorld_NearbyPayloads): its tilesets and border, read here instead of
 * off the card in the middle of the frame that first draws from them. */
static struct
{
    volatile int state;
    CtrAssetPrefetch request;
    void *result;
} sAssetAhead;

/* A slice of a page: `count` of its texels from texel `first`, or with
 * `glow` `count` bytes of its window mask from byte `first`. */
static bool ReadPageSlice(int page, bool glow, unsigned first, unsigned count, void *dest)
{
    return glow ? VoxelBuildings_ReadGlow((unsigned)page, first, count, dest)
                : VoxelBuildings_ReadPage((unsigned)page, first, count, dest);
}

static void StreamWorker(void *arg)
{
    (void)arg;
    for (;;)
    {
        LightEvent_Wait(&sStream.wake);
        if (sStream.quit)
            break;
        if (sStream.state == STREAM_READING)
        {
            bool ok = ReadPageSlice(sStream.page, sStream.glow, sStream.first, sStream.count,
                                    sStream.buffer);

            /* Written from this core's cache: out to memory for the GPU's
             * copy before the render thread is told. */
            if (ok)
                GSPGPU_FlushDataCache(sStream.buffer,
                                      sStream.count * (sStream.glow ? 1u : sizeof(uint16_t)));
            __sync_synchronize();
            sStream.state = ok ? STREAM_DONE : STREAM_FAILED;
        }
        if (sAssetAhead.state == AHEAD_READING)
        {
            sAssetAhead.result = CtrAssets_PrefetchRead(&sAssetAhead.request);
            __sync_synchronize();
            sAssetAhead.state = AHEAD_DONE;
        }
    }
}

/* Adopt completed asset reads and prefetch payloads near the view. */
#define VOXEL_AHEAD_PAYLOADS 192u

static void ReadAssetsAhead(void)
{
    static const void *payloads[VOXEL_AHEAD_PAYLOADS];
    static unsigned count, cursor;
    static uint32_t signature, evictions;
    static bool cached;
    uint32_t now = VoxelWorld_PayloadSignature();
    uint32_t retired = CtrAssets_GetStats()->evictions;

    if (sAssetAhead.state == AHEAD_DONE)
    {
        __sync_synchronize();
        CtrAssets_PrefetchAdopt(&sAssetAhead.request, sAssetAhead.result);
        sAssetAhead.result = NULL;
        sAssetAhead.state = AHEAD_IDLE;
    }
    if (!cached || signature != now)
    {
        count = VoxelWorld_NearbyPayloads(payloads, VOXEL_AHEAD_PAYLOADS);
        cursor = 0;
        signature = now;
        cached = true;
    }
    if (evictions != retired)
    {
        evictions = retired;
        cursor = 0;
    }
    if (sAssetAhead.state != AHEAD_IDLE)
        return;
    /* Bounded residency checks; a fully resident list costs no more lookups
     * until a map/graphics change or an asset eviction invalidates it. */
    for (unsigned checked = 0; cursor < count && checked < 8; ++checked)
        /* A payload found is reserved for the worker until it is handed
         * back (CtrAssets_PrefetchAdopt): none may be skipped here. One whose
         * read failed is never found again. */
        if (CtrAssets_PrefetchFind(payloads[cursor++], &sAssetAhead.request))
        {
            __sync_synchronize();
            sAssetAhead.state = AHEAD_READING;
            LightEvent_Signal(&sStream.wake);
            return;
        }
}

static void ReadAhead(void)
{
    if (sStream.thread != NULL)
        ReadAssetsAhead();
}

static void StreamStart(void)
{
    s32 priority = 0x30;

    memset(&sStream, 0, sizeof(sStream));
    memset(sBadPages, 0, sizeof(sBadPages));
    memset(&sAssetAhead, 0, sizeof(sAssetAhead));
    sStream.buffer = linearAlloc(VOXEL_PAGE_SLICE * sizeof(uint16_t));
    if (sStream.buffer == NULL)
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no linear memory for the page stream; "
                     "pages borrow the atlas staging");
        return;
    }
    LightEvent_Init(&sStream.wake, RESET_ONESHOT);
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    /*
     * On the system core when the console lets the application have some of
     * it (an Old 3DS gives up to 30%): the reads, and the colour grade of each
     * slice, then run beside the game instead of in the gaps it leaves. On
     * the application core, below the game's priority, the worker only ran
     * while the game waited for the display - and not at all through the
     * long frames of a warm-up, which is exactly when a new map's pages are
     * wanted. A slice is 128 KiB read and graded: far inside 30%. The sound
     * engine's mixer may already have asked for more of that core
     * (CtrAudio_StartWorker): never lower it.
     */
    u32 limit = 0;

    if ((R_SUCCEEDED(APT_GetAppCpuTimeLimit(&limit)) && limit >= 30)
        || R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)))
        sStream.thread = threadCreate(StreamWorker, NULL, 16 * 1024, priority, 1, false);
    if (sStream.thread != NULL)
        sStream.systemCore = true;
    else
        sStream.thread = threadCreate(StreamWorker, NULL, 16 * 1024,
                                      priority < 0x3F ? priority + 1 : 0x3F, -2, false);
    /* Without the worker the reads stay on this thread, one slice a frame:
     * slower to arrive, but still never more than a slice per frame. */
    if (sStream.thread == NULL)
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no page stream thread; reading on the render thread");
}

static void StreamStop(void)
{
    if (sStream.thread != NULL)
    {
        sStream.quit = true;
        LightEvent_Signal(&sStream.wake);
        threadJoin(sStream.thread, U64_MAX);
        threadFree(sStream.thread);
    }
    /* Read or not - the worker may quit before it gets to a request it was
     * signalled for - every request found is handed back, and its
     * reservation with it. */
    if (sAssetAhead.state != AHEAD_IDLE)
        CtrAssets_PrefetchAdopt(&sAssetAhead.request, sAssetAhead.result);
    memset(&sAssetAhead, 0, sizeof(sAssetAhead));
    linearFree(sStream.buffer);
    memset(&sStream, 0, sizeof(sStream));
}

static BuildingPageSlot *FindPage(int page)
{
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
        if (sPageSlots[i].page == page && sPageSlots[i].tex.data != NULL)
            return &sPageSlots[i];
    return NULL;
}

/*
 * Claims a slot for a page a map on screen needs; it is filled by
 * StreamPages. Asked for by the map, as soon as it is in view or in the
 * prefetch ring, not by its first chunk with a building once built: the page
 * streams in while the chunks are built, instead of after them - which is
 * what left houses missing for a moment in a scene already drawn.
 *
 * `keep`: the pages needed in the last that many frames are not given up
 * for this one - two for a map in view, a few seconds for one only in the
 * ring, which must never push out a page on screen. `quiet`: asked for by a
 * map rather than by the chunks being drawn - a page that finds no room then
 * is asked for again soon and says nothing, and never holds back the request
 * its chunks make once they are on screen.
 */
#define VOXEL_PAGE_KEEP_VIEW 2u
#define VOXEL_PAGE_KEEP_RING 240u
#define VOXEL_PAGE_QUIET_RETRY_FRAMES 30u

/* A frame `age` frames ago, never before the first. */
static uint32_t FramesAgo(uint32_t age)
{
    return sFrame > age ? sFrame - age : 0u;
}

/* What WantPage may free for a page: the victim's texture, the droppable
 * pages, their masks and the masks not drawn from in two frames - and with
 * `masks` every other mask too. */
static void PageRoomFreed(bool *freed, const BuildingPageSlot *victim, uint32_t keep, bool masks)
{
    memset(freed, 0, VOXEL_PAGE_PIECES * sizeof(freed[0]));
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
    {
        const BuildingPageSlot *slot = &sPageSlots[i];
        uint32_t last = slot->glowDropping ? slot->glowDropped : slot->glowUsed;
        bool page = slot == victim || PageDroppable(slot, keep);
        int piece;

        if (page && (piece = VoxelArena_PieceOf(&sPageArena, slot->tex.data)) >= 0)
            freed[piece] = true;
        if ((page || masks || (sFrame - last >= 2u && !StreamHolds(slot)))
         && (piece = VoxelArena_PieceOf(&sPageArena, slot->glow.data)) >= 0)
            freed[piece] = true;
    }
}

/*
 * Would freeing all that leave a gap of `bytes`? The arena is first fit, as
 * the chunks' (ChunkRoomPossible). Pages were freed one by one until the new
 * one fitted, and when it never did they had gone for nothing, to stream
 * straight back in: on Route 110 by Slateport the city's
 * 512 KiB page asked for room every other frame beside another of 512 KiB,
 * freed one of the two 16 KiB pages each time - the other was still streaming
 * back - and pages 16 and 46 were read some 300 times in ten seconds while
 * the city stood without its buildings (port.log 2026-10-21).
 */
static bool PageRoomPossible(const BuildingPageSlot *victim, unsigned bytes, uint32_t keep, bool masks)
{
    bool freed[VOXEL_PAGE_PIECES];

    PageRoomFreed(freed, victim, keep, masks);
    return VoxelArena_FitsFreeing(&sPageArena, freed, bytes);
}

/* A page that found no room: asked for again later, and how soon. */
static void PageNoRoom(const BuildingPageSlot *victim, int page, unsigned w, unsigned h, bool quiet)
{
    bool soon = false;

    if (quiet)
    {
        sPageRingRetry[page] = sFrame + VOXEL_PAGE_QUIET_RETRY_FRAMES;
        return;
    }
    /* A page not needed this frame - the map left behind by a warp - is
     * droppable in a frame or two: ask again then, not seconds later, or the
     * new map stands without its buildings meanwhile. */
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
        if (&sPageSlots[i] != victim && sPageSlots[i].tex.data != NULL
         && sPageSlots[i].used != sFrame)
            soon = true;
    sPageRetry[page] = sFrame + (soon ? 2u : VOXEL_PAGE_RETRY_FRAMES);
    if (!soon && !sPageNoRoomLogged[page])
    {
        sPageNoRoomLogged[page] = true;
        ++sStats.errors;
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no room for the %ux%u building page %d "
                     "(largest gap %lu of %lu KiB); retrying later",
                     w, h, page, (unsigned long)(VoxelArena_LargestGap(&sPageArena) >> 10),
                     (unsigned long)(sPageArena.size >> 10));
    }
}

/* The slot a new page goes to: an empty one, else the least recently needed
 * of those that may be dropped (PageDroppable); NULL if none may. */
static BuildingPageSlot *PageVictim(uint32_t keep)
{
    BuildingPageSlot *victim = NULL;

    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
    {
        BuildingPageSlot *slot = &sPageSlots[i];

        if (slot->page < 0)
            return slot;
        if (!PageDroppable(slot, keep))
            continue;
        if (victim == NULL || slot->used < victim->used)
            victim = slot;
    }
    return victim;
}

/*
 * Room made by sliding what is in the way. On Route 110 by Slateport the
 * arena held 568 KiB free in three gaps, the largest 280 KiB, and page 23
 * (512 KiB) did not load (port.log 2026-10-28): a 256 KiB page had gone in
 * at 280 KiB, between the room at the bottom and the room under the small
 * pages at the top - pages come and go in orders no placement rule rules
 * out. Pages are that large because a texture is a power of two tall, all
 * the PICA200 samples: a page of 152 rows is a texture of 256, and its piece
 * takes all 256 for an emulator's texture cache (PageBytes). So when a page
 * in view finds no gap even after all WantPage may free, and sliding pieces
 * along the arena would leave it one (VoxelArena_SlidePlan), they slide, one
 * a frame: a GPU copy of what the piece holds, in parts no longer than the
 * distance it moves, queued in an order where none reads what an earlier one
 * wrote (VoxelArena_SlideSteps) - queued as the page slices are, so the GX
 * queue runs them one after another, after every write queued to the piece
 * before and before this frame's draws, which bind the texture at its new
 * place. FrameBegin waited for the draws of the frames before, and whatever
 * goes into the room left behind is written by copies queued after these.
 * From the start of the stream's step (PageMovesStep), at most
 * VOXEL_PAGE_MOVES_MAX slides every VOXEL_PAGE_RETRY_FRAMES frames.
 *
 * Only page texels and window masks slide, and not those the stream holds;
 * a borrowed atlas page stays where it is. A piece slides whole: its texture
 * keeps all its rows.
 */
#define VOXEL_PAGE_MOVES_MAX 8u
/* The largest single copy: an atlas page's upload, which every map makes. */
#define VOXEL_PAGE_MOVE_COPY (256u * 1024u)
/* The copies one slide may take: room the GX queue has in any frame, even
 * in stereo or a voxel battle (VOXEL_CHUNK_UPLOADS_MAX). */
#define VOXEL_PAGE_SLIDE_COPIES 2u

typedef struct
{
    BuildingPageSlot *slot; /* NULL: not a page's (a borrowed atlas page) */
    bool glow;              /* the slot's window mask rather than its texels */
    bool freed;             /* freed for the page anyway (PageRoomPossible) */
    bool movable;
} PagePiece;

static struct
{
    bool wanted;            /* a slide asked for by WantPage, for the next step */
    int page;               /* the page the room is made for */
    unsigned bytes;
    uint32_t keep, asked;
    unsigned slides;        /* slides made for that page so far */
    uint32_t since;         /* the frame the current count of slides began */
    unsigned recent;        /* slides since then */
} sPageMoves;

/* What each live piece of the arena is, as PageRoomPossible sees it without
 * the masks of pages in view. */
static void PagePieces(PagePiece *pieces, const BuildingPageSlot *victim, uint32_t keep)
{
    memset(pieces, 0, sPageArena.count * sizeof(pieces[0]));
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
    {
        BuildingPageSlot *slot = &sPageSlots[i];
        uint32_t last = slot->glowDropping ? slot->glowDropped : slot->glowUsed;
        bool page = slot == victim || PageDroppable(slot, keep);
        bool held = StreamHolds(slot);
        int piece;

        if ((piece = VoxelArena_PieceOf(&sPageArena, slot->tex.data)) >= 0)
        {
            pieces[piece].slot = slot;
            pieces[piece].freed = page;
            pieces[piece].movable = !page && !held && slot->page >= 0;
        }
        if ((piece = VoxelArena_PieceOf(&sPageArena, slot->glow.data)) >= 0)
        {
            pieces[piece].slot = slot;
            pieces[piece].glow = true;
            pieces[piece].freed = page || (sFrame - last >= 2u && !held);
            pieces[piece].movable = !pieces[piece].freed && !held && !slot->glowDropping;
        }
    }
}

/* What a live piece holds: the texels or mask bytes copied in so far. */
static uint32_t PagePieceHeld(const PagePiece *piece)
{
    if (piece->slot == NULL)
        return 0;
    return piece->glow ? piece->slot->glowLoaded : piece->slot->loaded * (uint32_t)sizeof(uint16_t);
}

/* A way to a gap of `bytes` for the page by sliding pieces, as `pieces`
 * (filled here) has them; its first slide in `slide` (may be NULL). With
 * `gone` a piece taken as freed (the lent gap), or -1. */
static bool PageSlidePlan(const BuildingPageSlot *victim, unsigned bytes, uint32_t keep,
                          PagePiece *pieces, VoxelArenaSlide *slide, int gone)
{
    uint8_t kind[VOXEL_PAGE_PIECES];
    uint32_t held[VOXEL_PAGE_PIECES];

    if (sPageArena.base == NULL || sPageArena.count == 0)
        return false;
    PagePieces(pieces, victim, keep);
    for (unsigned i = 0; i < sPageArena.count; ++i)
    {
        kind[i] = pieces[i].freed || (int)i == gone ? VOXEL_ARENA_FREED
                : pieces[i].movable ? VOXEL_ARENA_SLIDES : VOXEL_ARENA_STAYS;
        held[i] = PagePieceHeld(&pieces[i]);
    }
    return VoxelArena_SlidePlan(&sPageArena, kind, held, bytes, VOXEL_PAGE_MOVE_COPY,
                                VOXEL_PAGE_SLIDE_COPIES, slide);
}

/*
 * A page drawn from with no room: the lent gap back, if room for it, or a way
 * to it by sliding, is there with the gap gone (PagesLentAsk). Before anything
 * is slid or dropped, and before the wait and the error of a page with no room:
 * the piece that stood in its way was only lent. With `masks`, the room there
 * is with the gap gone and the masks in view dropped as well: WantPage's last
 * step, for a page neither the gap nor the masks make room for alone. Sliding
 * is not looked at then: WantPage never does both.
 */
static bool PageWaitsForLentGap(const BuildingPageSlot *victim, int page, unsigned bytes, uint32_t keep, bool masks)
{
    PagePiece pieces[VOXEL_PAGE_PIECES];
    bool freed[VOXEL_PAGE_PIECES];
    int lent;

    if (sPagesLent == NULL || (lent = VoxelArena_PieceOf(&sPageArena, sPagesLent)) < 0)
        return false;
    PageRoomFreed(freed, victim, keep, masks);
    freed[lent] = true;
    if (!VoxelArena_FitsFreeing(&sPageArena, freed, bytes)
     && (masks || !PageSlidePlan(victim, bytes, keep, pieces, NULL, lent)))
        return false;
    sPagesLentWanted = true;
    sPageRetry[page] = sFrame + VOXEL_PAGE_LENT_RETRY_FRAMES;
    return true;
}

/* A page in view with no room: whether sliding pieces would make it some,
 * and if so the slides asked for (PageMovesStep, from the next frame). */
static bool PageMovesWanted(const BuildingPageSlot *victim, int page, unsigned bytes, uint32_t keep)
{
    PagePiece pieces[VOXEL_PAGE_PIECES];

    if (sFrame - sPageMoves.since >= VOXEL_PAGE_RETRY_FRAMES)
    {
        sPageMoves.since = sFrame;
        sPageMoves.recent = 0;
    }
    /* One page at a time: another asked for this frame keeps its turn, and
     * this one asks again next frame. */
    if (sPageMoves.wanted && sPageMoves.asked == sFrame && sPageMoves.page != page)
        return true;
    if (sPageMoves.recent >= VOXEL_PAGE_MOVES_MAX
     || !PageSlidePlan(victim, bytes, keep, pieces, NULL, -1))
        return false;
    if (sPageMoves.page != page)
    {
        sPageMoves.page = page;
        sPageMoves.slides = 0;
    }
    sPageMoves.wanted = true;
    sPageMoves.bytes = bytes;
    sPageMoves.keep = keep;
    sPageMoves.asked = sFrame;
    return true;
}

/*
 * The slide asked for, if it still makes sense: the page still missing and
 * still short of room, the plan still there with the arena as it now is,
 * room in the GX queue for all of its copies. Inside the frame, before its
 * draws (StreamPages): never between FrameEnd and the next FrameBegin, while
 * the GPU may still draw from the piece's old place.
 */
static void PageMovesStep(void)
{
    PagePiece pieces[VOXEL_PAGE_PIECES];
    struct { BuildingPageSlot *slot; bool glow; } over[VOXEL_PAGE_PIECES];
    VoxelArenaCopy steps[VOXEL_PAGE_SLIDE_COPIES];
    VoxelArenaSlide slide;
    BuildingPageSlot *slot, *victim;
    uint32_t from, size, held, low, high, before;
    unsigned copies, freeing = 0;
    C3D_Tex *tex;
    void *dest;
    bool glow;

    if (!sPageMoves.wanted)
        return;
    sPageMoves.wanted = false; /* asked for again while the page finds no room */
    if (sFrame - sPageMoves.asked > 2u || FindPage(sPageMoves.page) != NULL
     || (victim = PageVictim(sPageMoves.keep)) == NULL
     || PageRoomPossible(victim, sPageMoves.bytes, sPageMoves.keep, false)
     || !PageSlidePlan(victim, sPageMoves.bytes, sPageMoves.keep, pieces, &slide, -1)
     || (slot = pieces[slide.piece].slot) == NULL)
        return;
    glow = pieces[slide.piece].glow;
    tex = glow ? &slot->glow : &slot->tex;
    from = sPageArena.pieces[slide.piece].offset;
    size = sPageArena.pieces[slide.piece].size;
    held = PagePieceHeld(&pieces[slide.piece]);
    copies = VoxelArena_SlideSteps(from, slide.offset, held, VOXEL_PAGE_MOVE_COPY, steps,
                                   VOXEL_PAGE_SLIDE_COPIES);
    if (copies > VOXEL_PAGE_SLIDE_COPIES)
        return; /* never: the plan counted them */
    /* What it passes over: only pieces WantPage would free for the page. */
    low = slide.offset < from ? slide.offset : from + size;
    high = slide.offset < from ? from : slide.offset + size;
    for (unsigned i = 0; i < sPageArena.count; ++i)
    {
        const VoxelArenaPiece *p = &sPageArena.pieces[i];

        if ((int)i == slide.piece || p->offset >= high || p->offset + p->size <= low)
            continue;
        if (!pieces[i].freed || pieces[i].slot == NULL)
            return; /* never: the plan passes over nothing else */
        over[freeing].slot = pieces[i].slot;
        over[freeing++].glow = pieces[i].glow;
    }
    /* The GX queue's room for every copy first: none goes without the rest. */
    if (CtrVideo_VoxelUploadsLeft() < copies)
    {
        sPageMoves.wanted = true; /* the next frame, as asked */
        return;
    }
    for (unsigned k = 0; k < copies; ++k)
        if (!CtrVideo_TryVoxelUpload())
        {
            sPageMoves.wanted = true;
            return;
        }
    before = VoxelArena_LargestGap(&sPageArena);
    for (unsigned k = 0; k < freeing; ++k)
    {
        if (over[k].glow)
            GlowFree(over[k].slot);
        else
            PageFree(over[k].slot);
    }
    dest = VoxelArena_Move(&sPageArena, tex->data, slide.offset);
    if (dest == NULL)
    {
        ++sStats.errors;
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: building page %d's %s would not slide to %lu KiB",
                     slot->page, glow ? "window mask" : "texels", (unsigned long)(slide.offset >> 10));
        return;
    }
    for (unsigned k = 0; k < copies; ++k)
        C3D_SyncTextureCopy((u32 *)(void *)(sPageArena.base + steps[k].from), 0,
                            (u32 *)(void *)(sPageArena.base + steps[k].to), 0, steps[k].size, 8);
    tex->data = dest;
    ++sPageMoves.slides;
    ++sPageMoves.recent;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: building page %d's %s slid from %lu to %lu KiB in the pages' "
                 "arena (%lu KiB in %u copies, %u pieces freed) for page %d, slide %u: largest gap "
                 "%lu -> %lu KiB", slot->page, glow ? "window mask" : "texels",
                 (unsigned long)(from >> 10), (unsigned long)(slide.offset >> 10),
                 (unsigned long)(held >> 10), copies, freeing, sPageMoves.page, sPageMoves.slides,
                 (unsigned long)(before >> 10), (unsigned long)(VoxelArena_LargestGap(&sPageArena) >> 10));
}

/*
 * A page in view waiting on slides: asked for this frame or the last
 * (PageMovesWanted), and still missing. Its room is made a slide a frame,
 * each freeing what lies in its path - prefetched ring pages among it - before
 * the whole room is there: a ring page placed meanwhile would come straight
 * back from the card, perhaps into the room being made, to be freed again by
 * the next slide. WantPage places none until then.
 */
static bool PageMovesActive(void)
{
    return sPageMoves.asked != 0 && sFrame - sPageMoves.asked <= 1u
        && FindPage(sPageMoves.page) == NULL;
}

static void WantPage(int page, uint32_t keep, bool quiet)
{
    BuildingPageSlot *victim = NULL;
    unsigned w, h, th;
    bool ring = keep > VOXEL_PAGE_KEEP_VIEW;

    if (page < 0 || page >= VOXEL_MAX_PAGES || sBadPages[page] || sFrame < sPageRetry[page])
        return;
    if ((victim = FindPage(page)) != NULL)
    {
        /* A ring map's page is not being drawn: it keeps its place only
         * against other ring pages. Its mask is wanted once it is. */
        if (!ring)
        {
            victim->used = sFrame;
            GlowWant(victim);
        }
        else if (victim->used < FramesAgo(keep / 2u))
            victim->used = FramesAgo(keep / 2u);
        return;
    }
    if (quiet && sFrame < sPageRingRetry[page])
        return;
    /* Not while slides make room for a page in view (PageMovesActive). */
    if (ring && PageMovesActive())
        return;
    victim = PageVictim(keep);
    if (victim == NULL || !VoxelBuildings_PageSize((unsigned)page, &w, &h, &th))
        return;
    /* Nothing given up for a page that would not fit even then (see
     * PageRoomPossible). For a page in view, the pieces in the way are
     * moved if that would do (PageMovesWanted), and only then are the
     * masks dropped, if that would. */
    if ((victim->tex.data == NULL || victim->w != w || victim->h != h)
     && !PageRoomPossible(victim, PageBytes(w, th), keep, false))
    {
        if (!quiet && PageWaitsForLentGap(victim, page, PageBytes(w, th), keep, false))
            return;
        if (!quiet && PageMovesWanted(victim, page, PageBytes(w, th), keep))
            sPageRetry[page] = sFrame + 1u;
        else if (!quiet && PageRoomPossible(victim, PageBytes(w, th), keep, true)
         && GlowDropOthers(victim))
            sPageRetry[page] = sFrame + 3u;
        else if (quiet || !PageWaitsForLentGap(victim, page, PageBytes(w, th), keep, true))
            PageNoRoom(victim, page, w, h, quiet);
        return;
    }
    /* The texture is kept for a page of the same size; the mask is the old
     * page's, and goes (the slot is droppable: not drawn from in two frames). */
    GlowFree(victim);
    if (victim->tex.data != NULL && (victim->w != w || victim->h != h))
        PageFree(victim);
    /*
     * Room in the arena, dropping the least recently needed pages that are
     * safe to drop until the piece fits. When it cannot fit - everything else
     * resident is on screen - the page is asked for again a while later, not
     * every frame: that retry, and its error line, each frame is what once
     * brought a city to a crawl.
     */
    while (victim->tex.data == NULL)
    {
        unsigned bytes = PageBytes(w, th);
        void *block = VoxelArena_Alloc(&sPageArena, bytes, bytes < 256u * 1024u);
        BuildingPageSlot *drop = NULL;

        if (block != NULL)
        {
            PageTexInit(&victim->tex, block, w, th);
            victim->w = w;
            victim->h = h;
            break;
        }
        /* Masks give up their room before any page does: first those not
         * drawn from just now. */
        if (GlowRelease(true))
            continue;
        for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
        {
            BuildingPageSlot *slot = &sPageSlots[i];

            if (slot != victim && PageDroppable(slot, keep) && (drop == NULL || slot->used < drop->used))
                drop = slot;
        }
        /* Then those of the pages in view, for a page in view: they stop
         * being drawn from now, and the page asks again once they are gone. */
        if (drop == NULL && !quiet && GlowDropOthers(victim))
        {
            victim->page = -1;
            sPageRetry[page] = sFrame + 3u;
            return;
        }
        if (drop == NULL)
        {
            victim->page = -1;
            PageNoRoom(victim, page, w, h, quiet);
            return;
        }
        PageFree(drop);
    }
    victim->page = page;
    victim->loaded = 0;
    victim->used = ring ? FramesAgo(keep / 2u) : sFrame;
    victim->requested = svcGetSystemTick();
    victim->glowRetry = 0;
    if (!ring)
        GlowWant(victim);
}

static void MarkBadPage(int page)
{
    ++sStats.errors;
    CtrLog_Write(CTR_LOG_ERROR, "VOXEL: building page %d unreadable", page);
    if (page >= 0 && page < VOXEL_MAX_PAGES)
        sBadPages[page] = true;
}

/* The most recently needed page that is still coming in. */
static BuildingPageSlot *NextPageSlot(void)
{
    BuildingPageSlot *next = NULL;

    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
    {
        BuildingPageSlot *slot = &sPageSlots[i];

        if (slot->page >= 0 && slot->tex.data != NULL
         && (slot->loaded < slot->w * slot->h || GlowPending(slot))
         && (next == NULL || slot->used > next->used))
            next = slot;
    }
    return next;
}

/* Its texels first, then its mask: is the next slice of the slot a mask's? */
static bool SliceIsGlow(const BuildingPageSlot *slot)
{
    return slot->loaded == slot->w * slot->h;
}

/* The next slice's size: texels, or a mask's bytes - as many bytes as a
 * slice of texels, the buffer's size. */
static unsigned SliceOf(const BuildingPageSlot *slot)
{
    bool glow = SliceIsGlow(slot);
    unsigned count = slot->w * slot->h - (glow ? slot->glowLoaded : slot->loaded);
    unsigned most = glow ? VOXEL_PAGE_SLICE * sizeof(uint16_t) : VOXEL_PAGE_SLICE;

    return count > most ? most : count;
}

static void QueuePageCopy(BuildingPageSlot *slot, uint16_t *source, unsigned count)
{
    GSPGPU_FlushDataCache(source, count * sizeof(uint16_t));
    C3D_SyncTextureCopy((u32 *)source, 0,
                        (u32 *)((uint16_t *)slot->tex.data + slot->loaded), 0,
                        count * sizeof(uint16_t), 8);
    slot->loaded += count;
    if (slot->loaded == slot->w * slot->h)
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL building page %d loaded (%ux%u) in %.0f ms",
                     slot->page, slot->w, slot->h, MsSince(slot->requested));
}

/* A slice of the slot's mask, `count` bytes (whole 8x8 tiles: a page is
 * whole tiles), to its piece of VRAM. */
static void QueueGlowCopy(BuildingPageSlot *slot, void *source, unsigned count)
{
    GSPGPU_FlushDataCache(source, count);
    C3D_SyncTextureCopy((u32 *)source, 0, (u32 *)((uint8_t *)slot->glow.data + slot->glowLoaded), 0,
                        count, 8);
    slot->glowLoaded += count;
    if (slot->glowLoaded == slot->w * slot->h)
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL building page %d windows loaded (%ux%u L8) in %.0f ms",
                     slot->page, slot->w, slot->h, MsSince(slot->requested));
}

/* A mask that would not read: the page keeps its texels, unlit windows. */
static void GlowBad(BuildingPageSlot *slot)
{
    ++sStats.errors;
    CtrLog_Write(CTR_LOG_ERROR, "VOXEL: building page %d's window mask unreadable", slot->page);
    GlowDrop(slot);
    slot->glowRetry = UINT32_MAX;
}

/* A slice read for the slot, texels or mask: its copy queued. */
static void QueueSlice(BuildingPageSlot *slot, bool glow, void *source, unsigned count)
{
    if (glow)
        QueueGlowCopy(slot, source, count);
    else
        QueuePageCopy(slot, source, count);
}

/* One step of the page stream per frame: hand a finished slice to the GPU,
 * then ask the worker for the next one. */
static void StreamPages(void)
{
    BuildingPageSlot *next;

    /* FrameBegin waited for last frame's copy: the buffer is free again. */
    sStream.copyQueued = false;
    /* The masks dropped two frames ago are drawn from no more. */
    GlowRelease(false);
    /* Room slid along the arena for a page in view that asked (WantPage),
     * before any slice is queued to a piece this frame. */
    PageMovesStep();
    if (sStream.buffer == NULL)
    {
        bool glow;
        unsigned count;

        /* No buffer of its own: borrow the atlas staging on a frame that is
         * not uploading an atlas, and read here, one slice at a time. */
        if (sAtlasUploadPending || AtlasJobBusy() || sAtlasStaging == NULL
         || (next = NextPageSlot()) == NULL || !CtrVideo_TryVoxelUpload())
            return;
        glow = SliceIsGlow(next);
        count = SliceOf(next);
        if (!ReadPageSlice(next->page, glow, glow ? next->glowLoaded : next->loaded, count,
                           sAtlasStaging))
        {
            if (glow)
                GlowBad(next);
            else
            {
                MarkBadPage(next->page);
                next->page = -1;
            }
            return;
        }
        QueueSlice(next, glow, sAtlasStaging, count);
        sAtlasUploadPending = true;
        return;
    }

    if (sStream.state == STREAM_DONE || sStream.state == STREAM_FAILED)
    {
        BuildingPageSlot *slot = sStream.slot;
        bool current = slot->page == sStream.page
                    && (sStream.glow ? GlowPending(slot) && slot->glowLoaded == sStream.first
                                     : slot->loaded == sStream.first);

        __sync_synchronize();
        if (sStream.state == STREAM_FAILED)
        {
            if (sStream.glow)
            {
                if (current)
                    GlowBad(slot);
            }
            else
            {
                MarkBadPage(sStream.page);
                if (current)
                    slot->page = -1;
            }
        }
        else if (current)
        {
            if (!CtrVideo_TryVoxelUpload())
                return;
            QueueSlice(slot, sStream.glow, sStream.buffer, sStream.count);
            sStream.copyQueued = true;
        }
        sStream.state = STREAM_IDLE;
    }
    if (sStream.state != STREAM_IDLE || sStream.copyQueued || (next = NextPageSlot()) == NULL)
        return;
    sStream.slot = next;
    sStream.page = next->page;
    sStream.glow = SliceIsGlow(next);
    sStream.first = sStream.glow ? next->glowLoaded : next->loaded;
    sStream.count = SliceOf(next);
    if (sStream.thread != NULL)
    {
        sStream.state = STREAM_READING;
        __sync_synchronize();
        LightEvent_Signal(&sStream.wake);
    }
    else
    {
        /* Picked up at the top of the next frame, like the worker's result. */
        sStream.state = ReadPageSlice(sStream.page, sStream.glow, sStream.first, sStream.count,
                                      sStream.buffer)
                      ? STREAM_DONE : STREAM_FAILED;
    }
}

/* The page's texture, or NULL while it is still coming in. */
static C3D_Tex *BuildingPage(int page)
{
    BuildingPageSlot *slot = page >= 0 ? FindPage(page) : NULL;

    if (slot == NULL || slot->loaded < slot->w * slot->h)
        return NULL;
    slot->used = sFrame;
    return &slot->tex;
}

/* â”€â”€ Chunk cache â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/*
 * Which slot holds a square, found without walking all of them: every frame
 * asks after every square of the view and of the ring around it, and a linear
 * search touched the whole 128-slot array (some 25 KiB, more than the data
 * cache) for each, which was most of the time the visit itself took on
 * hardware. A chain per bucket, by the slot number plus one (0 ends it). A
 * slot is entered when a build gives it its square (JobFinish, DraftChunk) and
 * left when it is released; nothing else changes who a slot is.
 *
 * Counted each time a slot enters or leaves (sChunkIndexChanges): what every
 * FindChunk would answer is the same while the count is, as the kept view
 * walk (VisitReplay) relies on.
 */
#define VOXEL_CHUNK_BUCKETS 128u
static uint8_t sChunkHead[VOXEL_CHUNK_BUCKETS];
static uint8_t sChunkNext[VOXEL_CHUNK_SLOTS];
static uint32_t sChunkIndexChanges;

static unsigned ChunkBucket(bool border, int a, int b, int cx, int cy)
{
    uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u
               ^ (uint32_t)a * 83492791u ^ (uint32_t)b * 2654435761u ^ (border ? 0x9E3779B1u : 0u);

    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h & (VOXEL_CHUNK_BUCKETS - 1u);
}

static unsigned ChunkBucketOf(const VoxelChunk *c)
{
    return c->border ? ChunkBucket(true, (int)c->beltGrid, 0, c->cx, c->cy)
                     : ChunkBucket(false, c->mapGroup, c->mapNum, c->cx, c->cy);
}

static void ChunkUnindex(const VoxelChunk *chunk)
{
    unsigned slot = (unsigned)(chunk - sChunks);
    uint8_t *link = &sChunkHead[ChunkBucketOf(chunk)];

    ++sChunkIndexChanges;
    while (*link != 0)
    {
        if ((unsigned)*link - 1u == slot)
        {
            *link = sChunkNext[slot];
            sChunkNext[slot] = 0;
            return;
        }
        link = &sChunkNext[*link - 1u];
    }
}

static void ChunkIndex(const VoxelChunk *chunk)
{
    unsigned slot = (unsigned)(chunk - sChunks);
    uint8_t *head = &sChunkHead[ChunkBucketOf(chunk)];

    ++sChunkIndexChanges;
    sChunkNext[slot] = *head;
    *head = (uint8_t)(slot + 1u);
}

/*
 * The smallest mesh a build ahead of time found no room for (BUILD_NO_VRAM)
 * since the last one that did; 0 for none. No build ahead of time starts
 * while the arena could not make room for one that large (AheadRoomNone).
 * Every block given back to the arena forgets it: room may be there now.
 */
static unsigned sAheadNoRoomBytes;

static void ChunkBlockFree(void *block)
{
    if (block == NULL)
        return;
    VoxelArena_Free(&sChunkArena, block);
    sAheadNoRoomBytes = 0;
}

static void ReleaseChunk(VoxelChunk *chunk)
{
    if (chunk->used)
        ChunkUnindex(chunk);
    ChunkBlockFree(chunk->vram);
    memset(chunk, 0, sizeof(*chunk));
}

static void ReleaseAllChunks(void)
{
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i)
        ReleaseChunk(&sChunks[i]);
    sDrawCount = 0;
}

static VoxelChunk *FindChunk(bool border, int mapGroup, int mapNum, int cx, int cy)
{
    unsigned link = sChunkHead[border ? ChunkBucket(true, (int)sBeltGrid, 0, cx, cy)
                                      : ChunkBucket(false, mapGroup, mapNum, cx, cy)];

    while (link != 0)
    {
        VoxelChunk *c = &sChunks[link - 1u];

        /* A belt square is the grid's, whichever map's border it was built
         * from (VisitSite). */
        if (c->used && c->cx == cx && c->cy == cy && c->border == border
         && (border ? c->beltGrid == sBeltGrid : c->mapGroup == mapGroup && c->mapNum == mapNum))
            return c;
        link = sChunkNext[link - 1u];
    }
    return NULL;
}

/*
 * The chunk that has gone longest without being needed. A build for the view
 * may take anything not on screen this frame; a build ahead of time may take
 * only what this frame has not touched at all, or the ring would evict itself.
 */
static bool Evictable(const VoxelChunk *c, bool forView)
{
    return c->used && c->viewStamp != sFrame && (forView || c->stamp != sFrame);
}

static VoxelChunk *OldestChunk(bool forView)
{
    VoxelChunk *victim = NULL;

    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i)
    {
        VoxelChunk *c = &sChunks[i];

        if (!Evictable(c, forView))
            continue;
        if (victim == NULL || c->stamp < victim->stamp)
            victim = c;
    }
    return victim;
}

static VoxelChunk *FreeChunkSlot(bool forView)
{
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i)
        if (!sChunks[i].used)
            return &sChunks[i];
    return OldestChunk(forView);
}

/*
 * Would evicting everything OldestChunk may take leave a gap of `bytes`? The
 * arena is first fit: in pieces, a large mesh may find no gap even with the
 * whole ring gone, and a view job then flushed the ring for nothing.
 */
static bool ChunkRoomPossible(unsigned bytes, bool forView)
{
    bool freed[VOXEL_CHUNK_SLOTS + 1];

    memset(freed, 0, sizeof(freed));
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i)
        if (Evictable(&sChunks[i], forView))
        {
            int piece = VoxelArena_PieceOf(&sChunkArena, sChunks[i].vram);

            if (piece >= 0)
                freed[piece] = true;
        }
    return VoxelArena_FitsFreeing(&sChunkArena, freed, bytes);
}

/*
 * The staging the caller of an eviction (EvictChunk) has still to fill this
 * frame, in vertices, and the read-backs of evicted meshes at its top
 * (ColdOffer): the room an upload has is what lies between. sCopyPromise is
 * the GX copies that caller has still to reserve (CtrVideo_TryVoxelUpload):
 * a read-back never takes those.
 */
static unsigned sStagingPromise, sStagingTop, sCopyPromise;
#define STAGING_ROOM (VOXEL_STAGING_VERTICES - sStagingTop)

static void ColdOffer(const VoxelChunk *victim);

/* A chunk given up for room: its mesh offered to the cold cache first. */
static void EvictChunk(VoxelChunk *victim)
{
    ColdOffer(victim);
    ReleaseChunk(victim);
}

/* VRAM for one chunk, evicting the least recently needed until there is room
 * - and nothing at all when the room cannot be made (ChunkRoomPossible). */
static VoxelGpuVertex *ChunkVram(unsigned bytes, bool forView)
{
    void *block = VoxelArena_Alloc(&sChunkArena, bytes, false);

    if (block != NULL || !ChunkRoomPossible(bytes, forView))
        return block;
    for (;;)
    {
        VoxelChunk *victim = OldestChunk(forView);

        if (victim == NULL)
            return NULL;
        EvictChunk(victim);
        block = VoxelArena_Alloc(&sChunkArena, bytes, false);
        if (block != NULL)
            return block;
    }
}

/* â”€â”€ Init / shutdown â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/*
 * Tree crowns a little lighter than the art: dense woods read as dark masses
 * in 3D. Only the crowns - where gen_voxel_trees.py packs them - and not the
 * trunks, whose ground is the tileset's own and must match the grass around.
 * Once, on the texture as it loads: nothing per frame.
 */
static void BrightenCrowns(uint16_t *texels)
{
    static const struct
    {
        unsigned x, y, w, h;
    } crowns[] = {{0, 0, 32, 36}, {32, 32, 16, 32}, {64, 0, 16, 32}, {0, 64, 128, 32}};

    for (unsigned c = 0; c < sizeof(crowns) / sizeof(crowns[0]); ++c)
        for (unsigned y = crowns[c].y; y < crowns[c].y + crowns[c].h; ++y)
            for (unsigned x = crowns[c].x; x < crowns[c].x + crowns[c].w; ++x)
                VoxelGrade_Brighten(&texels[CtrVideo_Texel(x, y, VOXEL_TREE_TEXTURE_DIM)], 1,
                                    VOXEL_TREE_BRIGHTNESS);
}

static bool ColdYield(void);
static void ColdDrop(bool global);

/*
 * The world's program without the shadow map (voxel_plain.v.pica), third in
 * voxel.shbin. Not fatal: without it every Draw uses the full program, as
 * before. Its uniforms must sit where the full program's do, since one set of
 * uploads (sUni*) serves both.
 */
static void LoadPlainProgram(void)
{
    DVLE_s *plain;
    const DVLE_s *full = &sDvlb->DVLE[0];
    shaderInstance_s *vs;

    if (sDvlb->numDVLE < 3)
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: voxel.shbin has no plain program, the full one is used");
        return;
    }
    plain = &sDvlb->DVLE[2];
    shaderProgramInit(&sPlainProgram);
    if (R_FAILED(shaderProgramSetVsh(&sPlainProgram, plain)))
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: the plain program does not bind, the full one is used");
        shaderProgramFree(&sPlainProgram);
        return;
    }
    vs = sPlainProgram.vertexShader;
    if (shaderInstanceGetUniformLocation(vs, "projection") != sUniProjection
     || shaderInstanceGetUniformLocation(vs, "modelView") != sUniModelView
     || shaderInstanceGetUniformLocation(vs, "origin") != sUniOrigin
     || shaderInstanceGetUniformLocation(vs, "shadeTint") != sUniShadeTint
     || shaderInstanceGetUniformLocation(vs, "tintDiff") != sUniTintDiff
     || shaderInstanceGetUniformLocation(vs, "grade") != sUniGrade
     || shaderInstanceGetUniformLocation(vs, "fog") != sUniFog
     || shaderInstanceGetUniformLocation(vs, "dappleU") != sUniDappleU
     || shaderInstanceGetUniformLocation(vs, "dappleV") != sUniDappleV)
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: the plain program's uniforms do not match, the full one is used");
        shaderProgramFree(&sPlainProgram);
        return;
    }
    sHavePlainProgram = true;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL init: plain vertex program loaded, %u instructions (full: %u)",
                 (unsigned)(plain->endmainOffset - plain->mainOffset),
                 (unsigned)(full->endmainOffset - full->mainOffset));
}

bool CtrVoxel_Init(void)
{
    sLampReady = false;
    sLampVertices = sLampGroundVertices = 0;
    const char *step = "open " VOXEL_SHADER_PATH;
    FILE *file = fopen(VOXEL_SHADER_PATH, "rb");
    long size;
    void *blob = NULL;

    if (file == NULL)
        goto fail;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0
     || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        step = "read voxel.shbin";
        goto fail;
    }
    blob = malloc((size_t)size);
    if (blob == NULL || fread(blob, 1, (size_t)size, file) != (size_t)size)
    {
        fclose(file);
        free(blob);
        blob = NULL;
        step = "read voxel.shbin";
        goto fail;
    }
    fclose(file);

    step = "parse voxel.shbin";
    sDvlb = DVLB_ParseFile(blob, (u32)size);
    /* DVLB_ParseFile keeps pointers into the blob, so it is never freed. */
    if (sDvlb == NULL || sDvlb->numDVLE == 0)
        goto fail;
    shaderProgramInit(&sProgram);
    if (R_FAILED(shaderProgramSetVsh(&sProgram, &sDvlb->DVLE[0])))
    {
        step = "bind vertex shader";
        goto fail;
    }
    sUniProjection = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");
    sUniModelView = shaderInstanceGetUniformLocation(sProgram.vertexShader, "modelView");
    sUniOrigin = shaderInstanceGetUniformLocation(sProgram.vertexShader, "origin");
    sUniShadeTint = shaderInstanceGetUniformLocation(sProgram.vertexShader, "shadeTint");
    sUniTintDiff = shaderInstanceGetUniformLocation(sProgram.vertexShader, "tintDiff");
    sUniGrade = shaderInstanceGetUniformLocation(sProgram.vertexShader, "grade");
    sUniFog = shaderInstanceGetUniformLocation(sProgram.vertexShader, "fog");
    sUniDappleU = shaderInstanceGetUniformLocation(sProgram.vertexShader, "dappleU");
    sUniDappleV = shaderInstanceGetUniformLocation(sProgram.vertexShader, "dappleV");
    sUniShadowU = shaderInstanceGetUniformLocation(sProgram.vertexShader, "shadowU");
    sUniShadowV = shaderInstanceGetUniformLocation(sProgram.vertexShader, "shadowV");
    sUniShadowW = shaderInstanceGetUniformLocation(sProgram.vertexShader, "shadowW");
    sUniShadowSel = shaderInstanceGetUniformLocation(sProgram.vertexShader, "shadowSel");
    if (sUniProjection < 0 || sUniModelView < 0 || sUniOrigin < 0 || sUniShadeTint < 0
     || sUniTintDiff < 0 || sUniGrade < 0 || sUniFog < 0
     || sUniDappleU < 0 || sUniDappleV < 0
     || sUniShadowU < 0 || sUniShadowV < 0 || sUniShadowW < 0 || sUniShadowSel < 0)
    {
        step = "locate shader uniforms";
        goto fail;
    }
#if CTR_VOXEL_LIGHTING
    LoadWeatherProgram();
#endif
    LoadPlainProgram();

    /*
     * The arenas are not claimed here. The world starts parked, as
     * CtrVoxel_ParkVram leaves it, and claims them on the first frame it is
     * wanted (CtrVoxel_UnparkVram, from 3ds_video.c's VoxelVramManage): the
     * field, a battle or a transition reached with the option on. Claimed at
     * start-up, they held 2.6 MiB through the intro, the title and the main
     * menu for nothing (console log of 0b3b36d91), the VRAM those screens'
     * layers and depth planes want. The claim is made from VRAM the screens
     * before have given back, in one piece as here; failing, the field is the
     * 2D one meanwhile, and the claim is tried again.
     */
    sChunkBlock = sPageBlock = NULL;
    VoxelArena_Init(&sChunkArena, NULL, 0, 16, sChunkPieces, VOXEL_CHUNK_SLOTS + 1);
    VoxelArena_Init(&sPageArena, NULL, 0, 128, sPagePieces, VOXEL_PAGE_PIECES);
    sParked = true;
    PageRetriesForget();

    /* The cold meshes give the heap back to any allocation that would fail. */
    ColdDrop(true);
    CtrPlatform_SetHeapYield(ColdYield);
    step = "chunk build scratch";
    sScratch = malloc(VOXEL_CHUNK_SCRATCH * sizeof(VoxelVertex));
    if (sScratch == NULL)
        goto fail;
    sDynamicScratch = malloc(VOXEL_DYNAMIC_VERTICES * sizeof(VoxelVertex));
    if (sDynamicScratch == NULL)
        goto fail;
    step = "chunk staging in linear memory";
    sStaging = linearAlloc(VOXEL_STAGING_VERTICES * sizeof(VoxelGpuVertex));
    if (sStaging == NULL)
        goto fail;
    step = "billboard buffer in linear memory";
    sDynamic = linearAlloc(VOXEL_DYNAMIC_VERTICES * sizeof(VoxelGpuVertex));
    if (sDynamic == NULL)
        goto fail;
    step = "fill buffer in linear memory";
    sFillGpu = linearAlloc(VOXEL_FILL_MAX * 6u * sizeof(VoxelGpuVertex));
    if (sFillGpu == NULL)
        goto fail;
    VoxelRegions_Init();
    VoxelRelief_Init();
    VoxelSign_Init();
    VoxelCamera_Init(&sCamera);
#ifdef VOXEL_HAVE_FAST_PACK
    PackSelfTest();
#endif
    /* Before any texture is made from the art. */
    VoxelGrade_Init();

    step = "atlas staging buffer in linear memory";
    sAtlasStaging = linearAlloc(VOXEL_ATLAS_PIXELS * sizeof(uint16_t));
    if (sAtlasStaging == NULL)
        goto fail;

    step = "sprite atlas in linear memory";
    if (!C3D_TexInit(&sSpriteAtlas, VOXEL_SPRITE_ATLAS_W, VOXEL_SPRITE_ATLAS_H, GPU_RGBA5551))
        goto fail;
    C3D_TexSetFilter(&sSpriteAtlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sSpriteAtlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(sSpriteAtlas.data, 0, VOXEL_SPRITE_PIXELS * sizeof(uint16_t));
    VoxelEntities_Reset();

    step = "tree texture " VOXEL_TREE_TEXTURE_PATH;
    if (!C3D_TexInit(&sTreeAtlas, VOXEL_TREE_TEXTURE_DIM, VOXEL_TREE_TEXTURE_DIM, GPU_RGBA5551))
        goto fail;
    file = fopen(VOXEL_TREE_TEXTURE_PATH, "rb");
    if (file == NULL)
        goto fail;
    {
        size_t bytes = VOXEL_TREE_TEXTURE_DIM * VOXEL_TREE_TEXTURE_DIM * sizeof(uint16_t);
        bool loaded = fread(sTreeAtlas.data, 1, bytes, file) == bytes;
        fclose(file);
        if (!loaded)
            goto fail;
        VoxelGrade_Texels(sTreeAtlas.data, VOXEL_TREE_TEXTURE_DIM * VOXEL_TREE_TEXTURE_DIM);
        BrightenCrowns(sTreeAtlas.data);
    }
    C3D_TexSetFilter(&sTreeAtlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTreeAtlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
#if CTR_VOXEL_LIGHTING
    MakeDapple();
    MakeRays();
    MakeMotes();
    MakeFog();
    MakeGloom();
    MakeWeather();
#endif

    /* Not fatal: without models the houses stay their cells' flat drawing.
     * Loaded now, drawn once the claim brings the pages' arena, or unloaded
     * by CtrVoxel_UnparkVram if it never finds the room for it - as for a
     * world parked and brought back.
     *
     * The page stream starts here all the same, as it did with the arenas
     * claimed here: its worker and its 128 KiB of linear memory are not
     * VRAM, so the change stays to the VRAM alone - the linear heap laid out
     * as before, and no thread made or system-core time asked for
     * (APT_SetAppCpuTimeLimit) on the first frame of the field. Until the
     * claim it has nothing to read: no page is asked for while the world is
     * parked (sHaveBuildings). */
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
        sPageSlots[i].page = -1;
    sBuildingsLoaded = VoxelBuildings_Init();
    sHaveBuildings = false;
    if (sBuildingsLoaded)
        StreamStart();

    /* Optional: animation only. Failure must not prevent the overworld from
     * loading when homebrew linear memory is tighter than on Azahar. */
    if (sBuildingsLoaded && VoxelBuildings_AnimRunCount() != 0)
    {
        sBuildingAnimOut = linearAlloc(VOXEL_BUILDING_ANIM_BYTES);
        if (sBuildingAnimOut == NULL)
            CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no linear memory for the buildings' animated texels");
    }
    sAnimActive = false;
    sReady = true;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL stream policy 7: drafts for holes, hashed chunk lookup");
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL lighting: %s",
                 CTR_VOXEL_LIGHTING ? "clock sun + cached shadows + cast sprite shadows" : "disabled");
    CtrLog_Write(CTR_LOG_VIDEO,
                 "VOXEL init: %u staging + %u billboard vertices (%lu KiB linear), up to %u VRAM "
                 "atlases of %lu KiB (%ux%u, %u slots), page stream %s, arenas (%lu+%lu KiB) claimed "
                 "when the world is first wanted, linear free=%lu, VRAM free=%lu",
                 VOXEL_STAGING_VERTICES, VOXEL_DYNAMIC_VERTICES,
                 (unsigned long)((VOXEL_STAGING_VERTICES + VOXEL_DYNAMIC_VERTICES)
                                 * sizeof(VoxelGpuVertex) >> 10),
                 VOXEL_ATLAS_SLOTS,
                 (unsigned long)(VOXEL_ATLAS_PIXELS * sizeof(uint16_t) >> 10),
                 VOXEL_ATLAS_W, VOXEL_ATLAS_H, VOXEL_ATLAS_MAX_SLOTS,
                 !sBuildingsLoaded ? "off (no buildings)" : sStream.thread == NULL ? "inline"
                 : sStream.systemCore ? "on the system core" : "threaded",
                 (unsigned long)(VOXEL_CHUNK_VRAM_BUDGET >> 10), (unsigned long)(VOXEL_PAGE_VRAM_BUDGET >> 10),
                 (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree());
    return true;

fail:
    CtrLog_Write(CTR_LOG_ERROR, "VOXEL init failed: %s (linear free=%lu)",
                 step, (unsigned long)linearSpaceFree());
    free(blob);
    CtrVoxel_Shutdown();
    return false;
}

void CtrVoxel_Shutdown(void)
{
    LampsRelease(true);
    StreamStop();
    AtlasJobCancel();
    ReleaseAllChunks();
    CtrPlatform_SetHeapYield(NULL);
    ColdDrop(true);
    VoxelRegions_Shutdown();
    VoxelSign_Shutdown();
    memset(sAnimDirty, 0, sizeof(sAnimDirty));
    sAnimPending = sAnimActive = sAnimFresh = false;
    sAnimWaitFrom = 0;
    linearFree(sBuildingAnimOut);
    sBuildingAnimOut = NULL;
    memset(sBuildingAnimDirty, 0, sizeof(sBuildingAnimDirty));
    sBuildingAnimPending = false;
    linearFree(sAtlasStaging);
    sAtlasStaging = NULL;
    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
    {
        FreeAtlasTextures(&sAtlases[i]);
        memset(&sAtlases[i], 0, sizeof(sAtlases[i]));
    }
    if (sSpriteAtlas.data != NULL)
    {
        C3D_TexDelete(&sSpriteAtlas);
        memset(&sSpriteAtlas, 0, sizeof(sSpriteAtlas));
    }
    if (sTreeAtlas.data != NULL)
    {
        C3D_TexDelete(&sTreeAtlas);
        memset(&sTreeAtlas, 0, sizeof(sTreeAtlas));
    }
    if (sDappleTex.data != NULL)
    {
        C3D_TexDelete(&sDappleTex);
        memset(&sDappleTex, 0, sizeof(sDappleTex));
    }
    sHaveDapple = false;
    linearFree(sRays);
    sRays = NULL;
    sRayCount = 0;
    linearFree(sMotes);
    sMotes = NULL;
    if (sFogTex.data != NULL)
    {
        C3D_TexDelete(&sFogTex);
        memset(&sFogTex, 0, sizeof(sFogTex));
    }
    if (sGloomTex.data != NULL)
    {
        C3D_TexDelete(&sGloomTex);
        memset(&sGloomTex, 0, sizeof(sGloomTex));
    }
    linearFree(sFogSheets);
    sFogSheets = NULL;
    sHaveFog = sHaveGloom = sOwnsFog = false;
    linearFree(sStreaks);
    linearFree(sFlakes);
    sStreaks = sFlakes = NULL;
    sOwnsWeather = false;
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
    {
        memset(&sPageSlots[i], 0, sizeof(sPageSlots[i]));
        sPageSlots[i].page = -1;
    }
    /* A gap lent to the battle comes back before the arena goes. */
    if (sPagesLent != NULL)
        CtrVideo_PagesGapRevoke();
    memset(&sPageArena, 0, sizeof(sPageArena));
    memset(&sChunkArena, 0, sizeof(sChunkArena));
    if (sPageBlock != NULL)
        vramFree(sPageBlock);
    if (sChunkBlock != NULL)
        vramFree(sChunkBlock);
    sPageBlock = sChunkBlock = NULL;
    sHaveBuildings = false;
    StaticMeshesFree();
    VoxelBuildings_Shutdown();
    VoxelRelief_Shutdown();
    free(sScratch);
    sScratch = NULL;
    free(sDynamicScratch);
    sDynamicScratch = NULL;
    JobCancel();
    linearFree(sStaging);
    sStaging = NULL;
    linearFree(sDynamic);
    sDynamic = NULL;
    linearFree(sFillGpu);
    sFillGpu = NULL;
    sFillCount = 0;
    if (sHaveWeatherProgram)
    {
        shaderProgramFree(&sWeatherProgram);
        sHaveWeatherProgram = false;
    }
    if (sHavePlainProgram)
    {
        shaderProgramFree(&sPlainProgram);
        sHavePlainProgram = false;
    }
    if (sDvlb != NULL)
    {
        shaderProgramFree(&sProgram);
        DVLB_Free(sDvlb);
        sDvlb = NULL;
    }
    sUniProjection = sUniModelView = sUniOrigin = -1;
    sAtlasCapped = false;
    sMeshMapGroup = sMeshMapNum = -1;
    sReady = false;
}

bool CtrVoxel_IsAvailable(void)
{
    if (sReady && !sParked && VoxelWorld_IsMapAvailable())
        return true;
    /* Not an overworld frame: the 2D compositor owns it. */
    return false;
}

const CtrVoxelStats *CtrVoxel_GetStats(void) { return &sStats; }

/*
 * Keys of the chunks' meshes.
 *
 * A chunk's vertices hold its ids' atlas slots as texture coordinates, so a
 * chunk was drawable only under the atlas generation it was built in: an
 * atlas evicted and made again - on an Old 3DS, with VRAM for one atlas at a
 * time, every time the player went from one tileset pair to another and back
 * - left every chunk of its pair to be built again, 10-30 ms each. Now:
 *
 * - The slots of a pair are given by its plan (voxel_atlas.h), the same in
 *   every atlas of the pair while the plan is kept.
 * - Each chunk keeps its UV key: the slot its build read for every id it read
 *   (VoxelBuilder readIds), with the id's void bit (indoors, the black filler
 *   is read off the drawing, VoxelAtlas_IsVoid), and indoors the ids of every
 *   cell of its build window, which the classification reads. Under another
 *   atlas of its pair whose slots for those ids are the same, the chunk is
 *   drawn as it is (ChunkAdopt; counted uv-kept), else built again as before
 *   (uv-changed).
 * - A chunk evicted for room keeps its mesh in a cold cache in the heap
 *   (ColdMesh), keyed by everything its vertices were made from; built again,
 *   a square whose key matches is a copy instead of a build.
 * - Audits (AuditWanted): one kept chunk or cold mesh in VOXEL_AUDIT_EVERY,
 *   every one in the FRAME_DUMP builds, is built again while drawn as it is,
 *   and the new mesh's checksum compared with the kept one's; a difference
 *   is a CONTRACT line in the log - a key that was not complete.
 *
 * nocache.txt bit 32768 (FRAME_DUMP builds): none of this - the order of
 * meeting for the slots, chunks by atlas generation, no cold meshes, as before.
 * Bit 262144: the console's pressure in an emulator that has room to spare -
 * one atlas holding VRAM at a time and the chunks' arena at a third - so that
 * atlases are made again and chunks evicted as on an Old 3DS (both sides of a
 * fidelity pair set it). Bit 524288: the keys and cold meshes kept, the slots
 * in the order of meeting (a control of what the plan alone changes). Bit
 * 1048576: every atlas dropped every 1200 updates outside a battle and made
 * again, a test of fresh rebuilds (CtrVoxel_Update).
 */
static bool sKeysOn = true;
#ifdef CTR_FRAME_DUMP
static bool sPressure;
#endif

static void DumpBitsRead(void)
{
#ifdef CTR_FRAME_DUMP
    static bool read;

    if (read)
        return;
    read = true;
    sKeysOn = !(CtrVideo_DumpOff() & 32768u);
    sPressure = (CtrVideo_DumpOff() & 262144u) != 0;
    /* 524288: the keys and cold meshes kept, the atlas slots laid out as
     * before the plan (sequentially) - a control: the plan moves textures in
     * the atlas, and nearest sampling may then round a texel edge apart. */
    VoxelAtlas_SetStablePlan(sKeysOn && !(CtrVideo_DumpOff() & 524288u));
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL dump bits: chunk keys and cold meshes %s, pressure %s, atlas plan %s",
                 sKeysOn ? "on" : "off (bit 32768)", sPressure ? "on (bit 262144)" : "off",
                 sKeysOn && !(CtrVideo_DumpOff() & 524288u) ? "on" : "off");
#endif
}

/* What the keys did, logged every VOXEL_KEYS_LOG_FRAMES (KeysLog). */
#define VOXEL_KEYS_LOG_FRAMES 600u
static struct
{
    unsigned uvKept, uvChanged;
    unsigned coldHits, coldMisses, coldStale, coldOrigin, coldStored, coldSame, coldSkipped, coldBad;
    unsigned coldEvicted, coldYields, coldYieldedMeshes, coldReleases, coldNoHeap, epochGlobal;
    unsigned audits, auditStale, auditVoid;
    unsigned builds, missingFrames;
    float buildMs, coldMs;
} sKeyCounts;

/* A checksum of packed vertices, carried on from `sum`. */
typedef uint32_t __attribute__((may_alias)) AliasWord;

static uint32_t SumVertices(uint32_t sum, const VoxelGpuVertex *vertices, unsigned count)
{
    const AliasWord *w = (const AliasWord *)(const void *)vertices;

    _Static_assert(sizeof(VoxelGpuVertex) == 16, "four words a vertex");
    for (unsigned i = 0; i < count; ++i, w += 4)
    {
        sum = (sum ^ (w[0] + ((w[1] << 7) | (w[1] >> 25)))) * 16777619u;
        sum = (sum ^ (w[2] + ((w[3] << 13) | (w[3] >> 19)))) * 16777619u;
    }
    return sum;
}

/*
 * The UV key of the ids in `ids` (a bit each): the pair, and for each id its
 * slot - from `slots` (the build's own reads) or else the table `map` - and
 * its void bit. Two independent 32-bit hashes.
 */
static void UvKeyOf(const uint32_t *ids, const uint16_t *slots, const VoxelAtlasMap *map,
                    const void *primary, const void *secondary, uint32_t key[2])
{
    uint32_t a = 2166136261u ^ (uint32_t)(uintptr_t)primary;
    uint32_t b = 0x9E3779B9u + (uint32_t)(uintptr_t)secondary * 0x85EBCA6Bu;

    for (unsigned w = 0; w < VOXEL_UV_WORDS; ++w)
    {
        uint32_t bits = ids[w];

        while (bits != 0)
        {
            unsigned id = w * 32u + (unsigned)__builtin_ctz(bits);
            unsigned slot = slots != NULL ? slots[id] : map->slotOf[id];
            uint32_t v = ((uint32_t)id << 17) ^ slot
                       ^ (VoxelAtlas_VoidBit(primary, secondary, id) ? 0x10000u : 0u);

            bits &= bits - 1u;
            a = (a ^ v) * 16777619u;
            b += v * 0xCC9E2D51u;
            b = ((b << 15) | (b >> 17)) * 0x1B873593u;
        }
    }
    key[0] = a ^ (a >> 16);
    key[1] = b ^ (b >> 13);
}

/* One kept chunk or cold mesh in this many is audited (every one with
 * FRAME_DUMP). */
#define VOXEL_AUDIT_EVERY 16u

static bool AuditWanted(void)
{
#ifdef CTR_FRAME_DUMP
    return true;
#else
    static unsigned turn;

    return ++turn % VOXEL_AUDIT_EVERY == 0;
#endif
}


/* â”€â”€ Atlas cache â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

static unsigned CountAllocatedAtlases(void)
{
    unsigned count = 0;

    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
        for (unsigned p = 0; p < VOXEL_ATLAS_PAGES; ++p)
            count += AtlasTex(&sAtlases[i], p)->data != NULL;
    return count;
}

/*
 * Least recently used slot that already owns a texture and was not claimed
 * during this update.
 *
 * That last condition is the whole point. Several maps are visible at once,
 * and if more tileset pairs are on screen than there are atlases, an
 * unguarded LRU would evict the atlas the previous instance just built and
 * rebuild it again next frame, for ever. Returning NULL instead drops one map
 * from the frame, which is visible but cheap and self-correcting.
 */
static uint32_t sRebuildStamp;

static bool AtlasInView(const VoxelAtlasSlot *slot);

static VoxelAtlasSlot *LeastRecentlyUsedAllocated(void)
{
    VoxelAtlasSlot *lru = NULL;

    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
        if (sAtlases[i].tex.data != NULL && sAtlases[i].stamp <= sRebuildStamp && !AtlasInView(&sAtlases[i])
         && (lru == NULL || sAtlases[i].stamp < lru->stamp))
            lru = &sAtlases[i];
    return lru;
}

static VoxelAtlasSlot *FindAtlas(const VoxelMapInstance *inst)
{
    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
    {
        VoxelAtlasSlot *slot = &sAtlases[i];

        if (slot->valid && slot->primaryTileset == inst->primaryTileset
         && slot->secondaryTileset == inst->secondaryTileset)
            return slot;
    }
    return NULL;
}

/*
 * The one atlas being composed, over as many frames as it takes (see
 * VoxelAtlas_JobBegin). Into the staging buffer, which it owns until it ends;
 * its table is installed with the upload, never before.
 */
static struct
{
    VoxelAtlasJob job;
    VoxelAtlasSlot *slot;
    bool forView;       /* a map on screen waits for it */
    /* Composed - after a FrameEnd, as a rule - and waiting for the next
     * frame to be uploaded in; the staging buffer is still the job's. */
    bool ready;
} sAtlasJob;

static bool AtlasJobBusy(void)
{
    return sAtlasJob.job.active || sAtlasJob.ready;
}

static void AtlasJobCancel(void)
{
    if (sAtlasJob.job.active)
        VoxelAtlas_JobCancel(&sAtlasJob.job);
    sAtlasJob.slot = NULL;
    sAtlasJob.ready = false;
}

static void AtlasFailed(int mapGroup, int mapNum, const void *primary, const void *secondary)
{
    ++sStats.errors;
    sHasFailed = true;
    sFailedPrimary = primary;
    sFailedSecondary = secondary;
    CtrLog_Write(CTR_LOG_ERROR, "VOXEL: atlas for %d:%d has no composable metatiles",
                 mapGroup, mapNum);
}

/*
 * The atlas of a map, or NULL while it is still being composed. `mayEvict` is
 * false for a map that is only in the prefetch ring: it may take an empty
 * slot, never the atlas of something else - which could be a map on screen.
 */
/* How many rows south of its cells a map is drawn: its base (a point stands
 * as far south as it is high in the world, VoxelBuilder_StretchZ). The view
 * (sViewRect) is where things are drawn; a map's chunks are its cells. */
static int DrawnRows(const VoxelMapInstance *inst)
{
    /* floorf without the library call: asked for every map a few times a
     * frame (atlases in view, the ring, the lamps, the near maps). */
    float rows = VoxelRelief_Base(inst) + 0.5f;
    int i = (int)rows;

    return (float)i > rows ? i - 1 : i;
}

/* Bound all atlas pages together to the original six-texture budget. */
static bool AtlasInView(const VoxelAtlasSlot *slot)
{
    for (unsigned i = 0; i < VoxelWorld_InstanceCount(); ++i)
    {
        const VoxelMapInstance *m = VoxelWorld_Instance(i);
        if (m->primaryTileset == slot->primaryTileset && m->secondaryTileset == slot->secondaryTileset
         && (i == 0 || (m->originX < sViewRect[2] && m->originX + m->width > sViewRect[0]
                    && m->originY + DrawnRows(m) < sViewRect[3]
                    && m->originY + m->height + DrawnRows(m) > sViewRect[1])))
            return true;
    }
    return false;
}

/*
 * `mayEvict`: for a map on screen. One composed ahead of time (the prefetch
 * ring) takes free VRAM only: allowed to free another atlas, two neighbours
 * off screen freed each other's for ever, each rebuilt in turn (Slateport
 * with Routes 109 and 110, twenty rebuilds in forty seconds).
 */
/* nocache.txt bit 262144 (FRAME_DUMP): another atlas holds VRAM, and only one
 * may - as on an Old 3DS whose VRAM held one ("atlas cache capped at 1"). */
static bool AtlasPressed(const VoxelAtlasSlot *slot)
{
#ifdef CTR_FRAME_DUMP
    if (sPressure)
        for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
            if (&sAtlases[i] != slot && sAtlases[i].tex.data != NULL)
                return true;
#else
    (void)slot;
#endif
    return false;
}

static bool AllocateAtlasPage(VoxelAtlasSlot *slot, unsigned page, bool mayEvict)
{
    C3D_Tex *tex = AtlasTex(slot, page);

    sAtlasWaitsForGap = false;
    if (tex->data != NULL)
        return true;
    for (;;)
    {
        /* Asked of the allocator only when it can answer yes: a map in the
         * prefetch ring asked every frame of a VRAM in pieces, and every
         * refusal is a GPUMEM line (769 in a port.log of 2026-10-26). */
        if (CountAllocatedAtlases() < VOXEL_ATLAS_SLOTS && !AtlasPressed(slot)
         && CtrGpuMem_LargestVram() >= VOXEL_ATLAS_PIXELS * sizeof(uint16_t)
         && C3D_TexInitVRAM(tex, VOXEL_ATLAS_W, VOXEL_ATLAS_H, GPU_RGBA5551))
        {
            C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            return true;
        }
        VoxelAtlasSlot *victim = NULL;
        for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
        {
            VoxelAtlasSlot *candidate = &sAtlases[i];
            if (candidate == slot || candidate == sAtlasJob.slot || AtlasInView(candidate)
             || candidate->tex.data == NULL)
                continue;
            if (victim == NULL || candidate->stamp < victim->stamp)
                victim = candidate;
        }
        if (!mayEvict)
            return false;
        /* Every atlas holding VRAM is on screen: a gap of the pages' arena,
         * if there is one, rather than a map left unbuilt. */
        if (victim == NULL)
            return CountAllocatedAtlases() < VOXEL_ATLAS_SLOTS && AtlasPageBorrow(tex);
        AnimForget(victim);
        FreeAtlasTextures(victim);
    }
}

/*
 * A borrowed atlas page goes back to VRAM as soon as a piece of it is free
 * (a battle's scene surface given back, a 2D screen's layers): a GPU copy,
 * queued like an atlas upload, one page a frame.
 */
static void AtlasPagesReturn(void)
{
    static uint32_t sNextTry;
    const uint32_t bytes = VOXEL_ATLAS_PIXELS * sizeof(uint16_t);

    if (sFrame < sNextTry || sAtlasUploadPending)
        return;
    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
        for (unsigned p = 0; p < VOXEL_ATLAS_PAGES; ++p)
        {
            C3D_Tex *tex = AtlasTex(&sAtlases[i], p);
            C3D_Tex home;

            if (tex->data == NULL || !AtlasPageBorrowed(tex))
                continue;
            /* Asked of the allocator only when it can answer yes: every
             * failed request is a GPUMEM line in the log. */
            if (CtrGpuMem_LargestVram() < bytes
             || !C3D_TexInitVRAM(&home, VOXEL_ATLAS_W, VOXEL_ATLAS_H, GPU_RGBA5551))
            {
                sNextTry = sFrame + 30u;
                return;
            }
            if (!CtrVideo_TryVoxelUpload())
            {
                C3D_TexDelete(&home);
                return;
            }
            C3D_TexSetFilter(&home, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&home, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            C3D_SyncTextureCopy((u32 *)tex->data, 0, (u32 *)home.data, 0, bytes, 8);
            sAtlasUploadPending = true;
            VoxelArena_Free(&sPageArena, tex->data);
            *tex = home;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: borrowed atlas page back in VRAM (VRAM free=%lu)",
                         (unsigned long)vramSpaceFree());
            return;
        }
}

static VoxelAtlasSlot *AcquireAtlas(const VoxelMapInstance *inst, bool mayEvict)
{
    VoxelAtlasSlot *victim = &sAtlases[0];
    VoxelAtlasSlot *hit = FindAtlas(inst);

    if (hit != NULL)
    {
        hit->stamp = ++sAtlasStamp;
        /*
         * A chunk met ids this atlas was not built with - a neighbour that
         * came into view, a metatile a script placed. The atlas grows in place
         * instead of being rebuilt: rebuilt, it had a new generation and every
         * chunk drawn from it went black at once until each was built again.
         */
        if (hit->extendPending && !AtlasJobBusy() && !sAtlasUploadPending
         && sAtlasStaging != NULL)
        {
            hit->extendPending = false;
            if (VoxelAtlas_JobBegin(&sAtlasJob.job, inst, sAtlasStaging, &hit->map))
            {
                sAtlasJob.slot = hit;
                sAtlasJob.forView = true; /* its chunks are missing tiles */
            }
            else if (sAtlasJob.job.ok)
                ++sAtlasExtensionsSkipped;
        }
        return hit;
    }
    /* Being composed: a map on screen makes the job urgent. */
    if (AtlasJobBusy() && !sAtlasJob.job.extend
     && sAtlasJob.job.primary == inst->primaryTileset
     && sAtlasJob.job.secondary == inst->secondaryTileset)
    {
        sAtlasJob.slot->stamp = ++sAtlasStamp;
        sAtlasJob.forView = sAtlasJob.forView || mayEvict;
        return NULL;
    }
    if (sHasFailed && inst->primaryTileset == sFailedPrimary
     && inst->secondaryTileset == sFailedSecondary)
        return NULL;
    if (!mayEvict && sPrefetchBlocked)
    {
        int group, num;

        VoxelWorld_GetLocation(&group, &num);
        if (group == sPrefetchBlockedGroup && num == sPrefetchBlockedNum)
            return NULL;
        sPrefetchBlocked = false;
    }
    /* A map on screen does not wait behind one composed ahead of time. */
    if (AtlasJobBusy() && !sAtlasJob.forView && mayEvict)
        AtlasJobCancel();
    /* One at a time: the job owns the staging buffer, and an upload queued
     * from it this frame has not been made yet. */
    if (AtlasJobBusy() || sAtlasUploadPending || sAtlasStaging == NULL)
        return NULL;

    /* An unused slot first, then the least recently used one. The atlas of
     * the current map is stamped every frame, so it is never the victim. */
    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
    {
        VoxelAtlasSlot *slot = &sAtlases[i];

        if (!slot->valid)
        {
            if (victim->valid || slot->stamp < victim->stamp)
                victim = slot;
        }
        else if (victim->valid && slot->stamp < victim->stamp)
        {
            victim = slot;
        }
    }

    if (!mayEvict && victim->valid)
        return NULL;
    if (victim->valid && (victim->stamp > sRebuildStamp || AtlasInView(victim)))
    {
        victim = LeastRecentlyUsedAllocated();
        if (victim == NULL)
        {
            sAtlasCapacityBlocked = mayEvict;
            return NULL;
        }
    }

    /*
     * The atlases live in VRAM, composed in the linear staging buffer first
     * and copied over by the GPU: VRAM is device memory, and writing a 512x256
     * atlas into it texel by texel from the CPU would cost far more than the
     * copy does (and faults outright on hardware).
     */
    if (victim->tex.data == NULL
     && !AllocateAtlasPage(victim, 0, mayEvict))
    {
        /* The gap is on its way back: asked for again next frame, as a map
         * whose atlas is being composed, with nothing given up meanwhile. */
        if (sAtlasWaitsForGap)
            return NULL;
        /* Still not a failure: the cache simply holds fewer atlases and
         * recycles the least recently used one. */
        if (!sAtlasCapped)
        {
            sAtlasCapped = true;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: atlas cache capped at %u (VRAM free=%lu)",
                         CountAllocatedAtlases(), (unsigned long)vramSpaceFree());
        }
        if (!mayEvict)
            return NULL;
        victim = LeastRecentlyUsedAllocated();
        if (victim == NULL)
        {
            /*
             * Not a failure to remember: the VRAM is usually held by the 2D
             * compositor's depth planes, left from a screen shown in 3D. They
             * are given back before the next frame and the atlas tried again
             * then - marked as failed, the map stayed in 2D for good.
             */
            static bool sReported;

            sAtlasCapacityBlocked = true;
            CtrVideo_RequestPlaneRelease();
            if (!sReported)
            {
                sReported = true;
                ++sStats.errors;
                CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no VRAM for a %ux%u atlas (free=%lu); "
                             "asking for the depth planes back",
                             VOXEL_ATLAS_W, VOXEL_ATLAS_H, (unsigned long)vramSpaceFree());
            }
            return NULL;
        }
    }
    if (victim->tex.data != NULL && !victim->valid)
    {
        C3D_TexSetFilter(&victim->tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&victim->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    if (victim->primaryTileset != inst->primaryTileset
     || victim->secondaryTileset != inst->secondaryTileset)
    {
        /* Retain page zero for reuse, release the old pair's extra pages. */
        for (unsigned p = 1; p < VOXEL_ATLAS_PAGES; ++p)
        {
            C3D_Tex *tex = AtlasTex(victim, p);
            AtlasPageRelease(tex);
            memset(tex, 0, sizeof(*tex));
        }
    }
    /* Out of use from now on: what it held is being replaced. */
    victim->valid = false;
    AnimForget(victim);
    victim->primaryTileset = inst->primaryTileset;
    victim->secondaryTileset = inst->secondaryTileset;
    victim->stamp = ++sAtlasStamp;
    if (!VoxelAtlas_JobBegin(&sAtlasJob.job, inst, sAtlasStaging, NULL))
    {
        AtlasFailed(inst->mapGroup, inst->mapNum, inst->primaryTileset, inst->secondaryTileset);
        return NULL;
    }
    sAtlasJob.slot = victim;
    sAtlasJob.forView = mayEvict;
    return NULL;
}

/*
 * Advances the atlas job within `budget` ms of `started`; `inFrame`, uploads
 * it once composed. Composing is CPU work into the staging buffer and runs
 * after FrameEnd as a rule; the upload is a GPU copy and needs the frame. With
 * a budget at all, a map on screen gets at least one step, so that it always
 * arrives.
 */
/*
 * The last frame the atlas job, and a hole on screen, made any progress.
 * Composing and building run in the time a frame leaves after FrameEnd; on a
 * map whose frames leave none - a heavy route on an Old 3DS, frame after
 * frame - an atlas waited for that time for ever: its map's chunks were
 * never built, and with the current map's missing the overworld fell back
 * to 2D. Past VOXEL_STARVE_FRAMES without progress either is given
 * VOXEL_STARVE_MS inside the frame, however the frame is going: a dropped
 * frame now and then while it arrives, never a map that does not.
 */
#define VOXEL_STARVE_FRAMES 4u
#define VOXEL_STARVE_MS 2.0f
static uint32_t sAtlasProgressFrame;
/*
 * Frames in a row whose time after FrameEnd could not take a slice
 * (CtrVoxel_AfterSubmit). A request only starves while this holds: with time
 * after the frame its turn comes there, and a request merely waiting behind
 * others in the queue is no reason to build inside the frame.
 */
static uint32_t sAfterStarvedFrames;
#define VOXEL_AFTER_STARVED_MS 1.0f

/* The most a slice's cost estimate holds (chunk phases and atlas steps). */
#define VOXEL_SLICE_CAP_MS 5.0f

static void RunAtlasJob(uint64_t started, float budget, bool inFrame)
{
    VoxelAtlasJob *job = &sAtlasJob.job;
    VoxelAtlasSlot *slot = sAtlasJob.slot;
    unsigned steps = 0;

    if (!AtlasJobBusy() || sAtlasUploadPending)
        return;
    /* The slot must still be the one the job is for. */
    if (slot == NULL || slot->primaryTileset != job->primary
     || slot->secondaryTileset != job->secondary || slot->valid != job->extend)
    {
        AtlasJobCancel();
        return;
    }
    if (!sAtlasJob.ready && job->pageReady)
        VoxelAtlas_JobNextPage(job);
    while (!sAtlasJob.ready)
    {
        /*
         * A step costs what the last ones of its phase did: started with any
         * time left at all, a 3-3.5 ms composing step ran past the after-submit
         * budget, by up to 6 ms (port.log, 2026-10-15: after=14.7/9.0). The
         * first step for the map on screen still always runs.
         */
        static float sAtlasStepMs[4];
        unsigned phase = job->phase;
        float *estimate = &sAtlasStepMs[phase & 3u];

        if (MsSince(started) + *estimate > budget && !(sAtlasJob.forView && steps == 0 && budget > 0.0f))
            return;
        uint64_t stepStart = svcGetSystemTick();
        sAtlasJob.ready = VoxelAtlas_JobStep(job, 24);
        float stepMs = MsSince(stepStart);
        float capped = stepMs < VOXEL_SLICE_CAP_MS ? stepMs : VOXEL_SLICE_CAP_MS;

        *estimate = capped > *estimate ? capped : *estimate * 0.75f + capped * 0.25f;
        static uint32_t lastSlow;
        if (stepMs > 2.0f && (!lastSlow || sFrame - lastSlow >= 120))
        {
            lastSlow = sFrame;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL atlas slice phase=%u %.2fms view=%u",
                         phase, stepMs, (unsigned)sAtlasJob.forView);
        }
        ++steps;
        sAtlasProgressFrame = sFrame;
    }
    /* One upload a frame from the staging, and the frame's own. */
    if (!inFrame || sAtlasUploadPending)
        return;

    if (!job->ok)
    {
        AtlasJobCancel();
        if (!job->extend)
            AtlasFailed(job->mapGroup, job->mapNum, job->primary, job->secondary);
        return;
    }
    if (!AllocateAtlasPage(slot, job->page, sAtlasJob.forView))
    {
        if (!sAtlasJob.forView)
        {
            /* Ahead of time and no room: not now (the map, once on screen,
             * asks again and may take a slot). */
            AtlasJobCancel();
            sPrefetchBlocked = true;
            VoxelWorld_GetLocation(&sPrefetchBlockedGroup, &sPrefetchBlockedNum);
            return;
        }
        if (!sAtlasWaitsForGap)
        {
            sAtlasCapacityBlocked = true;
            CtrVideo_RequestPlaneRelease();
        }
        return; /* Keep the completed page: retry without recomposing it. */
    }
    if (!CtrVideo_TryVoxelUpload())
        return;
    sAtlasJob.ready = false;
    /*
     * Staging to VRAM, by the GPU. The atlas is already in the swizzled layout
     * the PICA samples, so this is a straight byte copy - flags 8 is the raw
     * texture copy, with no format or tiling conversion asked of it. Queued
     * ahead of this frame's draws, so the table installed with it is never
     * drawn with the pixels it replaced.
     */
    GSPGPU_FlushDataCache(sAtlasStaging, VOXEL_ATLAS_PIXELS * sizeof(uint16_t));
    C3D_SyncTextureCopy((u32 *)sAtlasStaging, 0, (u32 *)AtlasTex(slot, job->page)->data, 0,
                        VOXEL_ATLAS_PIXELS * sizeof(uint16_t), 8);
    sAtlasUploadPending = true;
    /* A page replaced whole starts the animation over from its still frames. */
    AnimForget(slot);
    if (job->active)
        return; /* Other pages still own the job; publish only when all landed. */
    sAtlasJob.slot = NULL;
    bool changed = memcmp(&slot->map, &job->map, sizeof(slot->map)) != 0;
    slot->map = job->map;
    if (job->extend)
    {
        /* Any new assignment, including a confirmed absence, repairs chunks
         * that were waiting for this extension; existing UVs stay valid. */
        if (changed)
            ++slot->extension;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL atlas for %d:%d extended to %u/%u slots%s",
                     job->mapGroup, job->mapNum, slot->map.used, VOXEL_ATLAS_MAX_SLOTS,
                     slot->map.overflowed ? " OVERFLOW" : "");
        return;
    }
    ++slot->generation;
    slot->valid = true;
    ++sStats.atlasRebuilds;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL atlas for %d:%d: %u/%u slots%s (rebuilds=%u, linear free=%lu)",
                 job->mapGroup, job->mapNum, slot->map.used, VOXEL_ATLAS_MAX_SLOTS,
                 slot->map.overflowed ? " OVERFLOW" : "", sStats.atlasRebuilds,
                 (unsigned long)linearSpaceFree());
}

/* â”€â”€ Building â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/* What one chunk stands for: a square of a map, or of the border belt. */
typedef struct
{
    const VoxelMapInstance *inst; /* the map; for the belt, the current map */
    bool border;
    int cx, cy;
    int x0, y0, x1, y1;  /* world tiles it covers, clipped to its map */
    int baseX, baseY;    /* world tile its vertices are measured from */
} ChunkSite;

static void SiteOf(ChunkSite *site, const VoxelMapInstance *inst, bool border, int cx, int cy)
{
    site->inst = inst;
    site->border = border;
    site->cx = cx;
    site->cy = cy;
    site->baseX = (border ? sBeltOriginX : inst->originX) + cx * VOXEL_CHUNK;
    site->baseY = (border ? sBeltOriginY : inst->originY) + cy * VOXEL_CHUNK;
    site->x0 = site->baseX;
    site->y0 = site->baseY;
    site->x1 = site->x0 + VOXEL_CHUNK;
    site->y1 = site->y0 + VOXEL_CHUNK;
    if (!border)
    {
        /* In one place because the build and the signature must agree. */
        if (site->x1 > inst->originX + inst->width) site->x1 = inst->originX + inst->width;
        if (site->y1 > inst->originY + inst->height) site->y1 = inst->originY + inst->height;
    }
}

static unsigned OpenTiles(const ChunkSite *site);

/*
 * Everything a chunk's geometry reads: its tiles, the margin columns and face
 * culling look into, and with lighting the casters that shade it.
 */
static uint32_t SiteHash(const ChunkSite *site)
{
    /* which layout's models stand on the current map
     * (VoxelBuildings_LiveSignature): 0 as it was entered. And whether any
     * do: the signature is 0 as the map was entered with the buildings as
     * without them, and a chunk built without them (the flat drawing of the
     * houses' cells) is stale once their pages come back with the world up
     * (CtrVoxel_ClaimPages). */
    uint32_t standing = sHaveBuildings && !site->border
                      ? VoxelBuildings_LiveSignature(site->inst) * 2654435761u ^ 0x5BD1E995u : 0u;

    /* the lift its map was placed at (the belt's: the current map's), all
     * of it that much higher (VoxelRelief_Base) */
    standing ^= (uint32_t)site->inst->lift * 0x9E3779B1u;

#if CTR_VOXEL_LIGHTING
    if (!site->inst->indoor)
        return VoxelLighting_Hash(site->x0, site->y0, site->x1, site->y1) ^ standing;
#endif
    return VoxelWorld_BlockHash(site->x0 - 1, site->y0 - VOXEL_CHUNK_MARGIN_NORTH - 1,
                                site->x1 + 1, site->y1 + 2) ^ standing;
}

/*
 * â”€â”€ Building, a few rows at a time â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * A chunk of a dense town costs 10-30 ms on an Old 3DS: far more than any
 * frame has to spare, so built in one go every new chunk was a dropped frame.
 * It is built as a job instead - ground rows in VoxelMesh_EmitInstance's
 * order, tree rows, then the models a few dozen triangles at a time - a few
 * slices per frame against the frame's budget, and uploaded when done.
 * One job at a time: the mesh builder's classification window is its own.
 *
 * The world it reads must not move under it. A job remembers the epoch and
 * the atlas generation it started in and is dropped - never finished with
 * mixed data - if either changes; nothing of the chunk it was rebuilding has
 * been touched by then.
 *
 * The classification cache is opened over the square plus a margin, because
 * a face is culled by looking at the neighbour beyond it and what stands on a
 * cell can depend on the rows north of it: the build reads past its own edges
 * even though it writes nothing there.
 */
typedef enum { BUILD_OK, BUILD_NO_STAGING, BUILD_NO_VRAM } BuildResult;

enum
{
    JOB_GROUND, JOB_TREES, JOB_MODELS, /* map chunks */
    JOB_BORDER, JOB_BORDER_TREES,      /* belt chunks */
    JOB_SORT, JOB_PACK,                /* into the GPU's vertex format */
    JOB_STATICS,                       /* its static models' meshes */
    JOB_DONE
};

static struct
{
    bool active;
    ChunkSite site;
    VoxelAtlasSlot *atlas;
    uint32_t atlasGeneration, epoch, hash, beltGrid;
    /* The atlas's extension when the job started: its table may grow while
     * the job runs, and a chunk that missed ids in its early rows must still
     * count as built against the smaller one. */
    uint32_t atlasExtension;
    VoxelChunk *chunk;      /* the stale chunk being rebuilt, or NULL */
    bool forView;
    bool hole;              /* on screen with nothing drawn in its place */
    int phase, row, col;
    unsigned waitIndex;
    bool overdue;
    unsigned terrainCount, buildingFirst, treeShadow, treeCrowns;
    unsigned terrainFirst[VOXEL_ATLAS_PAGES + 1u];
    unsigned sortPage, sortScan, sortWrite;
    /* ... then each page's parts (JobSortParts): three splits a page, and
     * the parts' boxes measured last */
    unsigned sortStep, boxScan;
    bool sortOpen;
    VoxelChunkParts parts;
    VoxelBuildingCursor models;
    unsigned staticIndex;   /* JOB_STATICS: the next of sJobStatics */
    bool lightWarm;         /* JOB_GROUND: its rays' casters read (VoxelLighting_Warm) */
    unsigned warmCursor;
    uint64_t ticks;         /* spent on it, over every frame it took */
    /* ... and by phase, for the slow chunk log: where a chunk's time goes on
     * hardware is what says what to make cheaper next. */
    uint64_t phaseTicks[JOB_DONE + 1];
    unsigned rays;          /* shadow rays its build cast (VoxelLighting_Rays) */
    unsigned steps;         /* ... the points they tested (VoxelLighting_Steps) */
    unsigned surfaces;      /* ... and relief surface tests (VoxelLighting_SurfaceTests) */
    uint32_t firstFrame;
    /* Measured by JOB_PACK, a slice of vertices at a time: the chunk's gx0..
     * and gy0.. (VoxelChunk). */
    unsigned packed;
    PackBounds bounds;
    int gx0, gz0, gx1, gz1;
    float gy0, gy1;
    /* Its packed vertices' checksum, slice by slice (SumVertices). */
    uint32_t sum;
    /* The ids whose slots it read (VoxelBuilder readIds; the slots in
     * sJobReadSlots), for its UV key. */
    uint32_t readIds[VOXEL_UV_WORDS];
    /* A cold mesh put back (ColdTake): no geometry built, the mesh, its key
     * and what it was made with as the cache kept them. */
    bool cold, coldAudit;
    uint32_t uvKey[2];
    float builtBase, builtStretch[3];
    bool builtBuildings;
    int builtX, builtY;
} sJob;
static uint16_t sJobReadSlots[VOXEL_METATILE_IDS];

static unsigned sJobStarts, sJobCancels;
static float sCancelledBuildMs;

static void JobCancel(void)
{
    if (sJob.active)
    {
        ++sJobCancels;
        sCancelledBuildMs += TicksMs(sJob.ticks);
    }
    sJob.active = false;
}

static void JobStart(const ChunkSite *site, VoxelAtlasSlot *atlas, uint32_t hash,
                     VoxelChunk *chunk, bool forView, bool hole)
{
    const VoxelMapInstance *inst = site->inst;

    ++sJobStarts;
    memset(&sJob, 0, sizeof(sJob));
    sJob.active = true;
    sJob.site = *site;
    sJob.atlas = atlas;
    sJob.atlasGeneration = atlas->generation;
    sJob.atlasExtension = atlas->extension;
    sJob.epoch = sEpoch;
    sJob.beltGrid = sBeltGrid;
    sJob.hash = hash;
    sJob.chunk = chunk;
    sJob.forView = forView;
    sJob.hole = hole;
    sJob.phase = site->border ? JOB_BORDER : JOB_GROUND;
    sJob.row = site->y0;
    sJob.col = site->x0;
    sJob.firstFrame = sFrame;
    VoxelBuilder_Init(&sBuilder, sScratch, VOXEL_CHUNK_SCRATCH);
    sBuilder.statics = sJobStatics;
    sBuilder.staticCapacity = VOXEL_CHUNK_STATICS;
    VoxelBuilder_SetAtlas(&sBuilder, &atlas->map);
    if (sKeysOn)
    {
        sBuilder.readIds = sJob.readIds;
        sBuilder.readSlots = sJobReadSlots;
    }
    VoxelBuilder_SetOrigin(&sBuilder, site->baseX, site->baseY);
    /* The whole map stands at its base (voxel_relief.h), the belt round the
     * world at the base of the map it rings. */
    sBuilder.base = VoxelRelief_Base(inst);
    /* its eased rows, its own (the belt keeps its place) */
    if (!site->border)
        VoxelBuilder_SetStretch(&sBuilder, inst);
#if CTR_VOXEL_LIGHTING
    sBuilder.lighting = !inst->indoor;
    /* The belt keeps the fixed cost of the old one: corner shading only. */
    sBuilder.lightingRefine = !site->border;
#else
    (void)inst;
#endif
    VoxelMesh_BeginWindow(site->x0 - 1, site->y0 - VOXEL_CHUNK_MARGIN_NORTH,
                          site->x1 + 1, site->y1 + 1);
}

/* Still building from the world it started in? */
static bool JobValid(void)
{
    const VoxelAtlasSlot *atlas = sJob.atlas;

    return sJob.epoch == sEpoch && sJob.beltGrid == sBeltGrid
        && atlas->valid && atlas->generation == sJob.atlasGeneration
        && atlas->primaryTileset == sJob.site.inst->primaryTileset
        && atlas->secondaryTileset == sJob.site.inst->secondaryTileset;
}

#define VOXEL_MODEL_SLICE_TRIANGLES 64u
/* About 11 us a triangle on hardware (lit, packed, shared): under a ms. */
#define VOXEL_STATIC_SLICE_TRIANGLES 64u

/*
 * The finished geometry measured and packed, in place: a GPU vertex is
 * smaller than the builder's and written no further along the buffer than
 * the one it is read from. Its own slice of the job, after FrameEnd like the
 * rest, rather than inside the frame at the upload - for a dense chunk it is
 * a few milliseconds the GPU would have waited for.
 */
#define VOXEL_PACK_SLICE 256u
#define VOXEL_GROUND_SLICE_CELLS 1
/* Blocks of casters a slice reads ahead of the ground's rays: one is ~80 cells
 * classified on a drawn map, under a ms on an Old 3DS. */
#define VOXEL_WARM_SLICE_BLOCKS 1u

/* Floor and ceiling of a packed coordinate, in whole tiles. */
static int PackedFloor(int32_t q) { return q >> 9; }
static int PackedCeil(int32_t q) { return -((-q) >> 9); }

/* A triangle of the builder's, by its middle: in the chunk's south half (by
 * the row it stands on, z - y), in its east half? (VoxelChunkParts; measured
 * from the chunk's corner.) */
static bool SortSouth(const VoxelVertex *v)
{
    return v[0].z - v[0].y + v[1].z - v[1].y + v[2].z - v[2].y >= 1.5f * VOXEL_CHUNK;
}

static bool SortEast(const VoxelVertex *v)
{
    return v[0].x + v[1].x + v[2].x >= 1.5f * VOXEL_CHUNK;
}

/*
 * Each page's triangles into its four parts, as JobSort put them by page: in
 * place and in slices. Three splits a page - north before south, then west
 * before east in the north and east before west in the south.
 */
static bool JobSortParts(unsigned *budget)
{
    while (sJob.sortStep < VOXEL_ATLAS_PAGES * 3u)
    {
        unsigned page = sJob.sortStep / 3u, step = sJob.sortStep % 3u;
        uint16_t *ends = sJob.parts.end[page];
        unsigned lo = step == 2u ? ends[1] : sJob.terrainFirst[page];
        unsigned hi = step == 1u ? ends[1] : sJob.terrainFirst[page + 1u];

        if (!sJob.sortOpen)
        {
            sJob.sortScan = sJob.sortWrite = lo;
            sJob.sortOpen = true;
        }
        while (sJob.sortScan < hi)
        {
            VoxelVertex *v = sScratch + sJob.sortScan;

            if (*budget == 0)
                return false;
            --*budget;
            if (step == 0u ? !SortSouth(v) : step == 1u ? !SortEast(v) : SortEast(v))
            {
                if (sJob.sortWrite != sJob.sortScan)
                {
                    VoxelVertex temp[3];

                    memcpy(temp, v, sizeof(temp));
                    memcpy(v, sScratch + sJob.sortWrite, sizeof(temp));
                    memcpy(sScratch + sJob.sortWrite, temp, sizeof(temp));
                }
                sJob.sortWrite += 3;
            }
            sJob.sortScan += 3;
        }
        ends[step == 0u ? 1 : step == 1u ? 0 : 2] = (uint16_t)sJob.sortWrite;
        sJob.sortOpen = false;
        ++sJob.sortStep;
    }
    return true;
}

/* ... and the box of each part, over every page. */
static bool JobSortBoxes(unsigned *budget)
{
    unsigned end = sJob.terrainFirst[VOXEL_ATLAS_PAGES];

    if (sJob.boxScan == 0)
        for (unsigned part = 0; part < VOXEL_PARTS; ++part)
            for (int a = 0; a < 3; ++a)
            {
                sJob.parts.box[part][a] = 1e9f;
                sJob.parts.box[part][3 + a] = -1e9f;
            }
    while (sJob.boxScan < end)
    {
        const VoxelVertex *v = sScratch + sJob.boxScan;
        unsigned page = 0, part = 0;
        float *box;

        if (*budget == 0)
            return false;
        --*budget;
        while (sJob.boxScan >= sJob.terrainFirst[page + 1u])
            ++page;
        while (part < VOXEL_PARTS - 1u && sJob.boxScan >= sJob.parts.end[page][part])
            ++part;
        box = sJob.parts.box[part];
        for (int k = 0; k < 3; ++k)
        {
            if (v[k].x < box[0]) box[0] = v[k].x;
            if (v[k].y < box[1]) box[1] = v[k].y;
            if (v[k].z - v[k].y < box[2]) box[2] = v[k].z - v[k].y;
            if (v[k].x > box[3]) box[3] = v[k].x;
            if (v[k].y > box[4]) box[4] = v[k].y;
            if (v[k].z - v[k].y > box[5]) box[5] = v[k].z - v[k].y;
        }
        sJob.boxScan += 3;
    }
    sJob.parts.on = true;
    return true;
}

/* Partition triangles in bounded slices, without another vertex buffer.
 * U carries page*2 until packing; the gap avoids ambiguity at texture edges. */
static bool JobSort(void)
{
    unsigned budget = 128;
    while (sJob.sortPage < VOXEL_ATLAS_PAGES && budget--)
    {
        if (sJob.sortScan >= sJob.terrainCount)
        {
            sJob.terrainFirst[++sJob.sortPage] = sJob.sortWrite;
            sJob.sortScan = sJob.sortWrite;
            continue;
        }
        VoxelVertex *v = sScratch + sJob.sortScan;
        unsigned page = (unsigned)(v[0].u * 0.5f);
        if (page == sJob.sortPage)
        {
            VoxelVertex temp[3];
            memcpy(temp, v, sizeof(temp));
            memcpy(v, sScratch + sJob.sortWrite, sizeof(temp));
            memcpy(sScratch + sJob.sortWrite, temp, sizeof(temp));
            sJob.sortWrite += 3;
        }
        sJob.sortScan += 3;
    }
    if (sJob.sortPage < VOXEL_ATLAS_PAGES)
        return false;
    return JobSortParts(&budget) && JobSortBoxes(&budget);
}

/* A slice of the packing; true once the whole scratch is packed. */
static bool JobPack(void)
{
    const ChunkSite *site = &sJob.site;
    unsigned first = sJob.packed, count = sBuilder.count - first;

    if (first == 0)
        for (int a = 0; a < 3; ++a)
        {
            sJob.bounds.lo[a] = INT32_MAX;
            sJob.bounds.hi[a] = INT32_MIN;
        }
    if (count > VOXEL_PACK_SLICE)
        count = VOXEL_PACK_SLICE;
    for (unsigned i = first; i < first + count && i < sJob.terrainCount; ++i)
        sScratch[i].u -= 2.0f * (unsigned)(sScratch[i].u * 0.5f);
    PackRange(sScratch, first, count, &sJob.bounds);
    sJob.sum = SumVertices(sJob.sum, (const VoxelGpuVertex *)(const void *)sScratch + first, count);
    sJob.packed += count;
    if (sJob.packed < sBuilder.count)
        return false;

    /* The chunk's own square always, and whatever reaches past it. */
    sJob.gx0 = sJob.gz0 = 0;
    sJob.gx1 = sJob.gz1 = VOXEL_CHUNK;
    sJob.gy0 = sJob.gy1 = 0.0f;
    if (sBuilder.count != 0)
    {
        int lx = PackedFloor(sJob.bounds.lo[0]), lz = PackedFloor(sJob.bounds.lo[2]);
        int hx = PackedCeil(sJob.bounds.hi[0]), hz = PackedCeil(sJob.bounds.hi[2]);

        if (lx < sJob.gx0) sJob.gx0 = lx;
        if (lz < sJob.gz0) sJob.gz0 = lz;
        if (hx > sJob.gx1) sJob.gx1 = hx;
        if (hz > sJob.gz1) sJob.gz1 = hz;
        sJob.gy0 = (float)sJob.bounds.lo[1] / VOXEL_POS_SCALE;
        sJob.gy1 = (float)sJob.bounds.hi[1] / VOXEL_POS_SCALE;
    }
    /* ... and the static models it places, which are not in its geometry. */
    for (unsigned i = 0; i < sBuilder.staticCount; ++i)
    {
        const VoxelStaticDraw *s = &sJobStatics[i];
        int lx = (int)floorf(s->x), lz = (int)floorf(s->z);
        int hx = (int)ceilf(s->x + s->w), hz = (int)ceilf(s->z + s->d);

        if (lx < sJob.gx0) sJob.gx0 = lx;
        if (lz < sJob.gz0) sJob.gz0 = lz;
        if (hx > sJob.gx1) sJob.gx1 = hx;
        if (hz > sJob.gz1) sJob.gz1 = hz;
        if (s->y < sJob.gy0) sJob.gy0 = s->y;
        if (s->y + s->top > sJob.gy1) sJob.gy1 = s->y + s->top;
    }
    ReportPackErrors(site->border ? "border chunk" : "chunk", site->cx, site->cy,
                     site->inst->mapGroup, site->inst->mapNum);
    return true;
}

/* One slice of the job: a row of one pass, or one of the small passes. */
static void JobStep(void)
{
    const ChunkSite *site = &sJob.site;
    const VoxelMapInstance *inst = site->inst;

    switch (sJob.phase)
    {
    case JOB_GROUND:
    {
        /* A few cells at a time: over a drawn mountain one row of lit relief
         * lattice was 6 ms on hardware, more than a frame's spare time. */
        int x1 = sJob.col + VOXEL_GROUND_SLICE_CELLS;

#if CTR_VOXEL_LIGHTING
        /* The casters its rays will read first, a block a slice: read by
         * its first rays they were all one slice, 6-8 ms on hardware. */
        if (!sJob.lightWarm && sBuilder.lighting)
        {
            sJob.lightWarm = VoxelLighting_Warm(site->x0, site->y0, site->x1, site->y1,
                                                &sJob.warmCursor, VOXEL_WARM_SLICE_BLOCKS);
            return;
        }
#endif
        if (x1 > site->x1)
            x1 = site->x1;
        VoxelMesh_EmitGroundRow(&sBuilder, inst, sJob.col, x1, sJob.row);
        sJob.col = x1;
        if (sJob.col < site->x1)
            return;
        sJob.col = site->x0;
        break;
    }
    case JOB_TREES:
        if (sJob.row == site->y0 && sJob.col == site->x0)
        {
            sJob.terrainCount = sBuilder.count;
            VoxelTree_NoteCards(sJobCards, TreeCardsWanted());
        }
        VoxelTree_EmitInstance(&sBuilder, inst, sJob.col, sJob.row, sJob.col + 1, sJob.row + 1);
        if (++sJob.col < site->x1)
            return;
        sJob.col = site->x0;
        if (++sJob.row < site->y1)
            return;
        sJob.row = site->y0;
        TreeCrowns();
        sJob.buildingFirst = sBuilder.count;
        sJob.phase = sHaveBuildings ? JOB_MODELS : JOB_SORT;
        return;
    case JOB_MODELS:
        /* A few dozen lit triangles a slice: one large model is thousands. */
        if (VoxelBuildings_EmitSome(&sBuilder, inst, site->x0, site->y0, site->x1, site->y1,
                                    &sJob.models, VOXEL_MODEL_SLICE_TRIANGLES))
            sJob.phase = JOB_SORT;
        return;
    case JOB_BORDER:
        VoxelMesh_EmitBorder(&sBuilder, sJob.col, sJob.row, sJob.col + 1, sJob.row + 1);
        if (++sJob.col < site->x1)
            return;
        sJob.col = site->x0;
        if (++sJob.row < site->y1)
            return;
        sJob.row = site->y0;
        sJob.terrainCount = sBuilder.count;
        VoxelTree_NoteCards(sJobCards, TreeCardsWanted());
        sJob.phase = JOB_BORDER_TREES;
        return;
    case JOB_BORDER_TREES:
        /* One lit crown cell: a row can exceed the whole post-submit budget. */
        VoxelTree_EmitBorder(&sBuilder, sJob.col, sJob.row, sJob.col + 1, sJob.row + 1);
        if (++sJob.col < site->x1)
            return;
        sJob.col = site->x0;
        if (++sJob.row < site->y1)
            return;
        sJob.row = site->y0;
        TreeCrowns();
        sJob.buildingFirst = sBuilder.count;
        sJob.phase = JOB_SORT;
        return;
    case JOB_SORT:
        if (JobSort())
            sJob.phase = JOB_PACK;
        return;
    case JOB_PACK:
        if (JobPack())
            sJob.phase = sBuilder.staticCount != 0 ? JOB_STATICS : JOB_DONE;
        return;
    case JOB_STATICS:
        /* Each model it places converted (or found cached) before the chunk
         * is: it appears whole, and no draw converts one inside the frame.
         * A stale mesh (another sun's) is converted again, drawn till then. */
        while (sJob.staticIndex < sBuilder.staticCount)
        {
            const VoxelStaticDraw *s = &sJobStatics[sJob.staticIndex];

            bool cached = StaticMeshCurrent(s->pageModel, s->lit) != NULL;

            if (!StaticMeshPrepare(s->pageModel, s->lit, VOXEL_STATIC_SLICE_TRIANGLES))
                return;
            ++sJob.staticIndex;
            if (!cached)
                return; /* one slice of conversion a step */
        }
        sJob.phase = JOB_DONE;
        return;
    default:
        return;
    }
    /* The ground row pass. */
    if (++sJob.row >= site->y1)
    {
        sJob.row = site->y0;
        ++sJob.phase;
    }
}

/*
 * Uploads a finished job into its chunk. On NO_STAGING or NO_VRAM nothing is
 * touched; on NO_STAGING the finished geometry waits for another frame, on
 * NO_VRAM RunJobs drops it.
 */
static bool JobUvKey(const VoxelAtlasSlot *atlas);
static bool IsChunkOf(const VoxelChunk *chunk, const ChunkSite *site);
static void AuditBegin(VoxelChunk *chunk);
static void AuditEnd(VoxelChunk *chunk, bool uvKnown);

static BuildResult JobFinish(VoxelChunk *chunk)
{
    const ChunkSite *site = &sJob.site;
    const VoxelMapInstance *inst = site->inst;
    VoxelAtlasSlot *atlas = sJob.atlas;
    unsigned bytes;
    VoxelGpuVertex *staging;

    if (sStagingUsed + sBuilder.count > STAGING_ROOM)
        return BUILD_NO_STAGING; /* next frame, with the pool rewound */
    bytes = (sBuilder.count * sizeof(VoxelGpuVertex) + 15u) & ~15u;
    if (bytes != 0 && !CtrVideo_TryVoxelUpload())
        return BUILD_NO_STAGING;
    if (bytes != 0 && (chunk->vram == NULL || chunk->bytes < bytes))
    {
        /*
         * The new block before the old one goes: a stale chunk may already be
         * on this frame's draw list, and if there is no room it must stay
         * exactly as it was. Never evict the slot itself to make the room.
         */
        VoxelGpuVertex *block;

        if (chunk->used)
            chunk->stamp = chunk->viewStamp = sFrame;
        sStagingPromise = sBuilder.count;
        block = ChunkVram(bytes, sJob.forView);
        sStagingPromise = 0;
        if (block == NULL)
            return BUILD_NO_VRAM;
        ChunkBlockFree(chunk->vram);
        chunk->vram = block;
        chunk->bytes = bytes;
    }

    if (sBuilder.dropped != 0 && sBuilder.dropped > sWorstDropped)
    {
        sWorstDropped = sBuilder.dropped;
        ++sStats.errors;
        CtrLog_Write(CTR_LOG_ERROR,
                     "VOXEL: chunk scratch full, %u triangles refused at %d,%d of %d:%d%s"
                     " (%u of %u vertices)",
                     sBuilder.dropped, site->cx, site->cy, inst->mapGroup, inst->mapNum,
                     site->border ? " border" : "", sBuilder.count, VOXEL_CHUNK_SCRATCH);
    }
    sStats.dropped = sBuilder.dropped;
    if (bytes != 0)
    {
        /*
         * Staged in linear memory and copied by the GPU, as the atlases are.
         * SyncTextureCopy only QUEUES the transfer inside an open frame, so
         * the staging slice stays reserved until the next Update. Flags 8 is
         * the raw copy, with no format conversion.
         */
        staging = sStaging + sStagingUsed;
        memcpy(staging, sScratch, sBuilder.count * sizeof(VoxelGpuVertex));
        sStagingUsed += (bytes + sizeof(VoxelGpuVertex) - 1) / sizeof(VoxelGpuVertex);
        GSPGPU_FlushDataCache(staging, bytes);
        C3D_SyncTextureCopy((u32 *)staging, 0, (u32 *)chunk->vram, 0, bytes, 8);
        ++sChunkUploads;
    }

    {
        /* Its UV key (a cold mesh brings its own), and an audit owed by the
         * chunk this build replaces settled before its fields go. */
        bool uvKnown = sJob.cold || JobUvKey(atlas);

        if (chunk->used && chunk->audit != 0 && !sJob.cold && IsChunkOf(chunk, site))
            AuditEnd(chunk, uvKnown);
        chunk->uvKnown = uvKnown;
    }
    chunk->audit = 0;
    chunk->uvKey[0] = sJob.uvKey[0];
    chunk->uvKey[1] = sJob.uvKey[1];
    memcpy(chunk->uvIds, sJob.readIds, sizeof(chunk->uvIds));
    chunk->uvTried = NULL;
    chunk->sum = sJob.sum;
    if (sJob.cold)
    {
        chunk->builtBase = sJob.builtBase;
        memcpy(chunk->builtStretch, sJob.builtStretch, sizeof(chunk->builtStretch));
        chunk->builtBuildings = sJob.builtBuildings;
        chunk->builtX = sJob.builtX;
        chunk->builtY = sJob.builtY;
    }
    else
    {
        chunk->builtBase = sBuilder.base;
        chunk->builtStretch[0] = sBuilder.stretchFrom;
        chunk->builtStretch[1] = sBuilder.stretchTo;
        chunk->builtStretch[2] = sBuilder.stretchAdd;
        chunk->builtBuildings = sHaveBuildings;
        chunk->builtX = site->baseX;
        chunk->builtY = site->baseY;
    }
    if (chunk->used)
        ChunkUnindex(chunk);
    chunk->used = true;
    chunk->border = site->border;
    chunk->mapGroup = inst->mapGroup;
    chunk->mapNum = inst->mapNum;
    chunk->cx = site->cx;
    chunk->cy = site->cy;
    chunk->beltGrid = sBeltGrid;
    chunk->draft = false;
    ChunkIndex(chunk);
    if (site->border)
    {
        chunk->openTiles = OpenTiles(site);
        chunk->openEpoch = sEpoch;
        chunk->openSame = true;
    }
    chunk->hash = sJob.hash;
    chunk->epoch = sEpoch;
    chunk->staleEpoch = 0;
    chunk->primary = inst->primaryTileset;
    chunk->secondary = inst->secondaryTileset;
    chunk->layout = inst->layout;
    chunk->atlas = atlas;
    chunk->atlasGeneration = atlas->generation;
    chunk->atlasExtension = sJob.atlasExtension;
    chunk->uncovered = sBuilder.uncovered != 0;
    chunk->count = sBuilder.count;
    chunk->terrainCount = sJob.terrainCount;
    chunk->buildingFirst = sJob.buildingFirst;
    chunk->treeShadow = sJob.treeShadow;
    chunk->treeCrowns = sJob.treeCrowns;
    memcpy(chunk->terrainFirst, sJob.terrainFirst, sizeof(chunk->terrainFirst));
    chunk->parts = sJob.parts;
    chunk->gx0 = sJob.gx0;
    chunk->gz0 = sJob.gz0;
    chunk->gx1 = sJob.gx1;
    chunk->gz1 = sJob.gz1;
    chunk->gy0 = sJob.gy0;
    chunk->gy1 = sJob.gy1;
    chunk->buildingPage = sHaveBuildings && !site->border ? VoxelBuildings_PageOf(inst) : -1;
    chunk->staticCount = sBuilder.staticCount;
    memcpy(chunk->statics, sJobStatics, sBuilder.staticCount * sizeof(VoxelStaticDraw));
    chunk->stamp = sFrame;
    if (sJob.forView)
        chunk->viewStamp = sFrame;
    if (chunk->count > chunk->buildingFirst || chunk->staticCount != 0)
        WantPage(chunk->buildingPage, sJob.forView ? VOXEL_PAGE_KEEP_VIEW : VOXEL_PAGE_KEEP_RING,
                 !sJob.forView);
    /* Ids the atlas has never seen: it grows to take them (see AcquireAtlas). */
    if (sBuilder.uncovered != 0)
        atlas->extendPending = true;
    if (sJob.cold)
    {
        /* Put back, not built (ColdTake). */
        ++sKeyCounts.coldHits;
        if (sJob.coldAudit)
            AuditBegin(chunk);
        return BUILD_OK;
    }
    if (CTR_VOXEL_TRACE && sStats.meshRebuilds < 48)
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL chunk %d,%d of %d:%d%s tiles %d,%d..%d,%d -> %u verts",
                     site->cx, site->cy, inst->mapGroup, inst->mapNum,
                     site->border ? " border" : "", site->x0, site->y0, site->x1, site->y1,
                     sBuilder.count);
    ++sStats.meshRebuilds;
    return BUILD_OK;
}

/*
 * The cold meshes (see the keys above). A chunk evicted for room is read
 * back from VRAM by the GPU into the top of the staging (ColdOffer), and once
 * the GPU has written it - a frame later, after FrameEnd (ColdLand) - copied
 * into a block of the heap, cut like the arenas (voxel_arena.c), the least
 * recently used given up first. Built again, a square whose cold mesh's key
 * matches has it copied into the build scratch instead (ColdTake), and is
 * uploaded as a build is: a few tenths of a ms against 10-30 ms of build.
 *
 * Its key is everything its vertices were made from, as a chunk's own
 * (VisitSite) and more, since a cold mesh outlives the world it was built in:
 * - the square: map group and number, chunk coordinates (no belt squares:
 *   their grid is new with every cut, and they are cheap);
 * - the layout and the tileset pair;
 * - SiteHash: its tiles and the margins the build reads, the casters that
 *   light it and the sun's key, the maps round it (layouts, tilesets, places),
 *   the buildings standing on its map and whether any do, its map's lift;
 * - the UV key against the atlas it would be drawn with (UvKeyOf);
 * - its map's base and south ease as the builder took them, whether the
 *   buildings were loaded and the page their models are drawn from;
 * - where its square lay in the world: the current map is the world's
 *   origin, the lighting works in world coordinates, and a mesh built while
 *   a neighbour was the current map may have a shade rounded a step apart
 *   (a chunk on the arena is kept through a crossing all the same, by its
 *   SiteHash; a cold mesh is only put back where it was built);
 * - and the format of a mesh, which is this build's.
 * The world's epoch is not in it: a chunk on the arena is kept through a new
 * epoch by its SiteHash too, and a cold one is checked the same way.
 *
 * Its memory is the heap's, never VRAM nor linear memory, and given back to
 * anything that asks: the heap allocator calls ColdYield when an allocation
 * would fail (3ds_platform.c), a battle releases it (none is kept through
 * one: the battle's own allocations first), and parking the world drops it
 * with the chunks. After any of these none is taken again for
 * VOXEL_COLD_HOLD_FRAMES, nor while the heap has less than its size and
 * VOXEL_COLD_HEAP_MARGIN to spare.
 */
#define VOXEL_COLD_BYTES (384u * 1024u)
#define VOXEL_COLD_MESHES 48u
#define VOXEL_COLD_HOLD_FRAMES 600u
#define VOXEL_COLD_HEAP_MARGIN (1024u * 1024u)
/* Read back and not yet landed: at most these, in at most half the staging. */
#define VOXEL_COLD_PENDING 4u
#define VOXEL_COLD_TRANSIT_MAX (VOXEL_STAGING_VERTICES / 2u)
/* The format of a mesh: bumped with any change to what a vertex holds. */
#define VOXEL_COLD_FORMAT 2u

typedef struct
{
    bool used;
    int mapGroup, mapNum, cx, cy;
    const void *layout, *primary, *secondary;
    uint32_t hash, uvKey[2], sum, format;
    uint32_t uvIds[VOXEL_UV_WORDS];
    float base, stretch[3];
    bool buildings;
    int buildingPage;
    int x, y;           /* its square's place in the world as it was built */
    unsigned count, terrainCount, buildingFirst, treeShadow, treeCrowns, staticCount;
    unsigned terrainFirst[VOXEL_ATLAS_PAGES + 1u];
    VoxelChunkParts parts;
    int gx0, gz0, gx1, gz1;
    float gy0, gy1;
    uint32_t stamp;
    uint8_t *data;      /* in the block: its packed vertices, then its statics */
} ColdMesh;

static ColdMesh sCold[VOXEL_COLD_MESHES];

/*
 * HIGH's tree shadows. A crown is a card leant back towards the camera, and
 * seen from the sun it is a sliver: its shadow on the shadow map would be a
 * line. Each card the trees emitted (VoxelTree_NoteCards) is copied, its art
 * and its size kept, onto the plane across the sun through its middle - a
 * sprite's shadow, the crown's own outline, laid along the sun - after the
 * trees' vertices, where only the shadow map draws it (treeShadow). Shade 0:
 * not pushed back as a lit caster is (voxel.v.pica). The baked rays cast no
 * crown in HIGH any more (voxel_lighting.c): a crown's shadow is the map's,
 * a pixel wide, where the rays' lattice left a lone corner shaded, a blob.
 * The vertices it adds; 0 in LOW, at night, or with no lighting.
 */
/* tiles the turned trees stand north of their feet (TreeShadowCards) */
#define VOXEL_TREE_SHADOW_NORTH 0.4f

static unsigned TreeShadowCards(VoxelBuilder *builder)
{
    unsigned noted = VoxelTree_CardsNoted(), first = builder->count;
    const VoxelTreeCard *cards = VoxelTree_NotedCards();
#if CTR_VOXEL_LIGHTING
    float l[3] = {gVoxelSun.dx, -1.0f, gVoxelSun.dz};
    float n = sqrtf(l[0] * l[0] + 1.0f + l[2] * l[2]);
    float across[3], up[3];
#endif

#if CTR_VOXEL_LIGHTING
    if (cards == NULL || VoxelLighting_Quality() != VOXEL_SHADOWS_HIGH || !builder->lighting || gVoxelSun.strength <= 0.0f)
        return 0;
    for (int i = 0; i < 3; ++i)
        l[i] /= n;
    /* across: level and square to the sun; up: square to it and to that */
    {
        float len = sqrtf(l[2] * l[2] + l[0] * l[0]);

        if (len < 0.0001f)
        {
            across[0] = 1.0f; across[1] = across[2] = 0.0f;
        }
        else
        {
            across[0] = l[2] / len; across[1] = 0.0f; across[2] = -l[0] / len;
        }
        up[0] = l[1] * across[2] - l[2] * across[1];
        up[1] = l[2] * across[0] - l[0] * across[2];
        up[2] = l[0] * across[1] - l[1] * across[0];
        if (up[1] < 0.0f)
            for (int i = 0; i < 3; ++i)
                up[i] = -up[i];
    }
    for (unsigned k = 0; k < noted && builder->count + 6u <= builder->capacity; ++k)
    {
        const VoxelTreeCard *card = &cards[k];
        const VoxelVertex *c = card->corner, *foot = &card->foot;
        float w[3] = {c[1].x - c[0].x, c[1].y - c[0].y, c[1].z - c[0].z};
        float h[3] = {c[0].x - c[3].x, c[0].y - c[3].y, c[0].z - c[3].z};
        float wl = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        float hl = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
        float side = 1.0f;
        VoxelVertex out[4];

        if (wl < 0.0001f || hl < 0.0001f)
            continue;
        /* the art the way round it is drawn, as far as the sun allows */
        if (across[0] * w[0] + across[2] * w[2] < 0.0f)
            side = -1.0f;
        /* each corner: as far across and up from the tree's foot as on its
         * card, the whole tree turned at once about its foot */
        for (int j = 0; j < 4; ++j)
        {
            float d[3] = {c[j].x - foot->x, c[j].y - foot->y, c[j].z - foot->z};
            float a = (d[0] * w[0] + d[1] * w[1] + d[2] * w[2]) / wl * side;
            float b = (d[0] * h[0] + d[1] * h[1] + d[2] * h[2]) / hl;

            out[j] = c[j];
            out[j].x = foot->x + across[0] * a + up[0] * b;
            out[j].y = foot->y + across[1] * a + up[1] * b;
            /* pivoted a little north of the foot, under the crown: the trunk
             * is drawn flat on the ground, and a shadow begun at its foot
             * let the sun through beside it */
            out[j].z = foot->z - VOXEL_TREE_SHADOW_NORTH + across[2] * a + up[2] * b;
            out[j].shade = 0.0f;
        }
        builder->vertices[builder->count++] = out[0];
        builder->vertices[builder->count++] = out[1];
        builder->vertices[builder->count++] = out[2];
        builder->vertices[builder->count++] = out[0];
        builder->vertices[builder->count++] = out[2];
        builder->vertices[builder->count++] = out[3];
    }
#else
    (void)noted;
    (void)cards;
#endif
    return builder->count - first;
}

/* The trees done: the crowns noted after their grounds, then their turned
 * copies for the shadow map. */
static void TreeCrowns(void)
{
    unsigned first;

    VoxelTree_NoteCards(NULL, 0);
    first = sBuilder.count;
    VoxelTree_EmitNotedCards(&sBuilder);
    sJob.treeCrowns = sBuilder.count - first;
    sJob.treeShadow = TreeShadowCards(&sBuilder);
}
static void *sColdBlock;
static VoxelArena sColdArena;
static VoxelArenaPiece sColdPieces[VOXEL_COLD_MESHES + 1];
static LightLock sColdLock = 1;
static uint32_t sColdStamp;
static volatile uint32_t sColdHoldUntil;
static struct
{
    ColdMesh mesh;
    VoxelStaticDraw statics[VOXEL_CHUNK_STATICS];
    VoxelGpuVertex *transit;
    uint32_t frame;
} sColdPending[VOXEL_COLD_PENDING];
static unsigned sColdPendingCount;

static unsigned ColdBytes(const ColdMesh *m)
{
    return m->count * (unsigned)sizeof(VoxelGpuVertex) + m->staticCount * (unsigned)sizeof(VoxelStaticDraw);
}

static void ColdFreeLocked(ColdMesh *m)
{
    if (m->used && m->data != NULL)
        VoxelArena_Free(&sColdArena, m->data);
    memset(m, 0, sizeof(*m));
}

/* Every cold mesh and the block given back; how many meshes there were. */
static unsigned ColdDropLocked(void)
{
    unsigned meshes = 0;

    for (unsigned i = 0; i < VOXEL_COLD_MESHES; ++i)
        meshes += sCold[i].used;
    memset(sCold, 0, sizeof(sCold));
    if (sColdBlock != NULL)
        free(sColdBlock);
    sColdBlock = NULL;
    VoxelArena_Init(&sColdArena, NULL, 0, 16, sColdPieces, VOXEL_COLD_MESHES + 1);
    return meshes;
}

/* From the heap allocator, on whichever thread an allocation failed. */
static bool ColdYield(void)
{
    bool freed;

    LightLock_Lock(&sColdLock);
    freed = sColdBlock != NULL;
    if (freed)
    {
        sKeyCounts.coldYieldedMeshes += ColdDropLocked();
        ++sKeyCounts.coldYields;
        sColdHoldUntil = sFrame + VOXEL_COLD_HOLD_FRAMES;
    }
    LightLock_Unlock(&sColdLock);
    return freed;
}

/* Dropped with what is still being read back: `global` for the world's
 * chunks going too (parked), else a release (a battle). The read-backs'
 * staging stays reserved till the next update: the GPU may still write it. */
static void ColdDrop(bool global)
{
    bool had;

    LightLock_Lock(&sColdLock);
    had = sColdBlock != NULL || sColdPendingCount != 0;
    ColdDropLocked();
    LightLock_Unlock(&sColdLock);
    sColdPendingCount = 0;
    if (!had)
        return;
    if (global)
        ++sKeyCounts.epochGlobal;
    else
        ++sKeyCounts.coldReleases;
    sColdHoldUntil = sFrame + VOXEL_COLD_HOLD_FRAMES;
}

void CtrVoxel_ColdRelease(void)
{
    if (sColdBlock != NULL || sColdPendingCount != 0)
        ColdDrop(false);
}

static bool ColdWanted(void)
{
    return sKeysOn && !CtrVoxel_InBattle() && sFrame >= sColdHoldUntil;
}

/* The block, taken when a mesh lands and the heap has room for it. */
static bool ColdEnsureBlock(void)
{
    void *block;

    if (sColdBlock != NULL)
        return true;
    if (!ColdWanted())
        return false;
    if (CtrPlatform_HeapRoom() < VOXEL_COLD_BYTES + VOXEL_COLD_HEAP_MARGIN)
    {
        ++sKeyCounts.coldNoHeap;
        sColdHoldUntil = sFrame + VOXEL_COLD_HOLD_FRAMES;
        return false;
    }
    /* (a failure here finds no block to yield: no recursion) */
    block = malloc(VOXEL_COLD_BYTES);
    if (block == NULL)
    {
        ++sKeyCounts.coldNoHeap;
        sColdHoldUntil = sFrame + VOXEL_COLD_HOLD_FRAMES;
        return false;
    }
    LightLock_Lock(&sColdLock);
    if (sColdBlock == NULL)
    {
        sColdBlock = block;
        VoxelArena_Init(&sColdArena, block, VOXEL_COLD_BYTES, 16, sColdPieces, VOXEL_COLD_MESHES + 1);
        block = NULL;
    }
    LightLock_Unlock(&sColdLock);
    free(block);
    return true;
}

static ColdMesh *ColdFindLocked(int mapGroup, int mapNum, int cx, int cy)
{
    for (unsigned i = 0; i < VOXEL_COLD_MESHES; ++i)
    {
        ColdMesh *m = &sCold[i];

        if (m->used && m->cx == cx && m->cy == cy && m->mapNum == mapNum && m->mapGroup == mapGroup)
            return m;
    }
    return NULL;
}

/* A free entry and a place in the block for `bytes`, the least recently used
 * meshes given up until there are. */
static ColdMesh *ColdAllocLocked(unsigned bytes, uint8_t **data)
{
    if (sColdBlock == NULL || bytes > VOXEL_COLD_BYTES)
        return NULL;
    for (;;)
    {
        ColdMesh *slot = NULL, *oldest = NULL;

        for (unsigned i = 0; i < VOXEL_COLD_MESHES; ++i)
        {
            if (!sCold[i].used)
            {
                if (slot == NULL)
                    slot = &sCold[i];
            }
            else if (oldest == NULL || sCold[i].stamp < oldest->stamp)
                oldest = &sCold[i];
        }
        if (slot != NULL && (*data = VoxelArena_Alloc(&sColdArena, bytes, false)) != NULL)
            return slot;
        if (oldest == NULL)
            return NULL;
        ColdFreeLocked(oldest);
        ++sKeyCounts.coldEvicted;
    }
}

/*
 * A chunk about to be evicted (EvictChunk): its mesh read back by the GPU into
 * the top of the staging, queued before the copy that reuses its block. Only
 * inside the frame, with room in the queue and in the staging beyond what the
 * evicting caller has still to stage (sStagingPromise), and in the GX queue
 * beyond the copy it has still to reserve (sCopyPromise): an eviction never
 * leaves its own upload without one. A later upload of the frame may wait a
 * frame for the copy a read-back took; keeping one more spare left the cache
 * empty in stereo (two copies a frame) and cost more builds and frames with
 * holes than it saved (nocache bit 262144, slider up). One the cache holds
 * as it is already (put back and not built since) is only marked used.
 */
static void ColdOffer(const VoxelChunk *c)
{
    unsigned bytes, vertices;
    VoxelGpuVertex *transit;
    ColdMesh *m;

    if (!ColdWanted() || !c->used || c->border || c->draft || !c->uvKnown || c->count == 0
     || c->vram == NULL)
        return;
    LightLock_Lock(&sColdLock);
    m = ColdFindLocked(c->mapGroup, c->mapNum, c->cx, c->cy);
    if (m != NULL && m->hash == c->hash && m->sum == c->sum && m->uvKey[0] == c->uvKey[0]
     && m->uvKey[1] == c->uvKey[1] && m->layout == c->layout && m->count == c->count
     && m->x == c->builtX && m->y == c->builtY)
    {
        m->stamp = ++sColdStamp;
        LightLock_Unlock(&sColdLock);
        ++sKeyCounts.coldSame;
        return;
    }
    LightLock_Unlock(&sColdLock);
    bytes = c->count * (unsigned)sizeof(VoxelGpuVertex);
    /* in 64-byte steps: no cache line shared with the uploads below */
    vertices = (c->count + 3u) & ~3u;
    if (sColdPendingCount == VOXEL_COLD_PENDING
     || bytes + c->staticCount * sizeof(VoxelStaticDraw) > VOXEL_COLD_BYTES / 3u
     || sStagingTop + vertices > VOXEL_COLD_TRANSIT_MAX
     || sStagingUsed + sStagingPromise + sStagingTop + vertices > VOXEL_STAGING_VERTICES
     || CtrVideo_VoxelUploadsLeft() < sCopyPromise + 1u
     || !CtrVideo_TryVoxelUpload())
    {
        ++sKeyCounts.coldSkipped;
        return;
    }
    sStagingTop += vertices;
    transit = sStaging + VOXEL_STAGING_VERTICES - sStagingTop;
    /* No line of it dirty in the CPU's cache, to be written over the GPU's. */
    GSPGPU_FlushDataCache(transit, vertices * sizeof(VoxelGpuVertex));
    C3D_SyncTextureCopy((u32 *)(void *)c->vram, 0, (u32 *)(void *)transit, 0, bytes, 8);
    {
        ColdMesh *p = &sColdPending[sColdPendingCount].mesh;

        memset(p, 0, sizeof(*p));
        p->used = true;
        p->mapGroup = c->mapGroup;
        p->mapNum = c->mapNum;
        p->cx = c->cx;
        p->cy = c->cy;
        p->layout = c->layout;
        p->primary = c->primary;
        p->secondary = c->secondary;
        p->hash = c->hash;
        p->uvKey[0] = c->uvKey[0];
        p->uvKey[1] = c->uvKey[1];
        p->sum = c->sum;
        p->format = VOXEL_COLD_FORMAT;
        memcpy(p->uvIds, c->uvIds, sizeof(p->uvIds));
        p->base = c->builtBase;
        memcpy(p->stretch, c->builtStretch, sizeof(p->stretch));
        p->buildings = c->builtBuildings;
        p->buildingPage = c->buildingPage;
        p->x = c->builtX;
        p->y = c->builtY;
        p->count = c->count;
        p->terrainCount = c->terrainCount;
        p->buildingFirst = c->buildingFirst;
        p->treeShadow = c->treeShadow;
        p->treeCrowns = c->treeCrowns;
        p->staticCount = c->staticCount;
        memcpy(p->terrainFirst, c->terrainFirst, sizeof(p->terrainFirst));
        p->parts = c->parts;
        p->gx0 = c->gx0;
        p->gz0 = c->gz0;
        p->gx1 = c->gx1;
        p->gz1 = c->gz1;
        p->gy0 = c->gy0;
        p->gy1 = c->gy1;
        memcpy(sColdPending[sColdPendingCount].statics, c->statics, c->staticCount * sizeof(VoxelStaticDraw));
        sColdPending[sColdPendingCount].transit = transit;
        sColdPending[sColdPendingCount].frame = sFrame;
        ++sColdPendingCount;
    }
}

/*
 * The read-backs of an earlier frame - written by now: CtrVideo_Present waited
 * for that frame's GPU before this frame's update - copied into the block.
 * After FrameEnd as a rule (CtrVoxel_AfterSubmit), at the next update if that
 * did not come. The staging they held is given back.
 */
static void ColdLand(void)
{
    unsigned kept = 0, top = 0;

    for (unsigned i = 0; i < sColdPendingCount; ++i)
    {
        const ColdMesh *p = &sColdPending[i].mesh;
        const VoxelGpuVertex *transit = sColdPending[i].transit;
        unsigned bytes = p->count * (unsigned)sizeof(VoxelGpuVertex);
        uint8_t *data = NULL;
        ColdMesh *m;

        if (sColdPending[i].frame >= sFrame)
        {
            unsigned extent = (unsigned)(sStaging + VOXEL_STAGING_VERTICES - transit);

            if (extent > top)
                top = extent;
            if (kept != i)
                sColdPending[kept] = sColdPending[i];
            ++kept;
            continue;
        }
        GSPGPU_InvalidateDataCache(transit, bytes);
        /* Every read-back checked against the build's own checksum: a GPU
         * copy into memory the CPU then reads is a hazard an emulator hides,
         * and a sum costs far less than the build it would replace. */
        if (SumVertices(0, transit, p->count) != p->sum)
        {
            ++sKeyCounts.coldBad;
            CtrLog_Write(CTR_LOG_ERROR, "CONTRACT voxel cold mesh %d,%d of %d:%d read back unlike its build",
                         p->cx, p->cy, p->mapGroup, p->mapNum);
            continue;
        }
        if (!ColdEnsureBlock())
        {
            ++sKeyCounts.coldSkipped;
            continue;
        }
        LightLock_Lock(&sColdLock);
        if ((m = ColdFindLocked(p->mapGroup, p->mapNum, p->cx, p->cy)) != NULL)
            ColdFreeLocked(m);
        m = ColdAllocLocked(ColdBytes(p), &data);
        if (m != NULL)
        {
            memcpy(data, transit, bytes);
            memcpy(data + bytes, sColdPending[i].statics, p->staticCount * sizeof(VoxelStaticDraw));
            *m = *p;
            m->data = data;
            m->stamp = ++sColdStamp;
        }
        LightLock_Unlock(&sColdLock);
        if (m != NULL)
            ++sKeyCounts.coldStored;
        else
            ++sKeyCounts.coldSkipped;
    }
    sColdPendingCount = kept;
    sStagingTop = top;
}

/*
 * The cold mesh of the square a job has just started for (StartNextJob), put
 * back if its key matches: the vertices into the scratch and the statics into
 * the job's, the job then past its geometry - its static models' meshes, if
 * any, made ready as for a build (JOB_STATICS) - and uploaded as a build is
 * (JobFinish). One whose key does not match is dropped.
 */
static bool ColdTake(const ChunkSite *site, uint32_t hash, const VoxelAtlasSlot *atlas)
{
    const VoxelMapInstance *inst = site->inst;
    float stretch[3] = {0.0f, 0.0f, 0.0f}, base;
    int page;
    uint32_t key[2];
    bool audit;
    ColdMesh *m;

    if (!ColdWanted() || site->border || sColdBlock == NULL)
        return false;
    base = VoxelRelief_Base(inst);
    page = sHaveBuildings ? VoxelBuildings_PageOf(inst) : -1;
    /* (as VoxelBuilder_SetStretch takes it) */
    if (!VoxelRelief_Stretch(inst, &stretch[0], &stretch[1], &stretch[2]))
        stretch[2] = 0.0f;
    LightLock_Lock(&sColdLock);
    m = ColdFindLocked(inst->mapGroup, inst->mapNum, site->cx, site->cy);
    if (m == NULL)
    {
        LightLock_Unlock(&sColdLock);
        ++sKeyCounts.coldMisses;
        return false;
    }
    /* Built while another map was the world's origin: kept for when it is
     * again, and built here meanwhile. */
    if (m->x != site->baseX || m->y != site->baseY)
    {
        LightLock_Unlock(&sColdLock);
        ++sKeyCounts.coldOrigin;
        return false;
    }
    if (m->format == VOXEL_COLD_FORMAT && m->layout == inst->layout
     && m->primary == inst->primaryTileset && m->secondary == inst->secondaryTileset
     && atlas->valid && atlas->primaryTileset == m->primary && atlas->secondaryTileset == m->secondary
     && m->hash == hash && m->buildings == sHaveBuildings && m->buildingPage == page
     && memcmp(&m->base, &base, sizeof(base)) == 0 && memcmp(m->stretch, stretch, sizeof(stretch)) == 0)
        UvKeyOf(m->uvIds, NULL, &atlas->map, m->primary, m->secondary, key);
    else
    {
        key[0] = ~m->uvKey[0];
        key[1] = m->uvKey[1];
    }
    if (key[0] != m->uvKey[0] || key[1] != m->uvKey[1])
    {
        ColdFreeLocked(m);
        LightLock_Unlock(&sColdLock);
        ++sKeyCounts.coldStale;
        return false;
    }
    memcpy(sScratch, m->data, m->count * sizeof(VoxelGpuVertex));
    /* Every mesh summed again as it leaves the cache; one in
     * VOXEL_AUDIT_EVERY also rebuilt and compared as it is drawn. */
    if (SumVertices(0, (const VoxelGpuVertex *)(const void *)sScratch, m->count) != m->sum)
    {
        ColdFreeLocked(m);
        LightLock_Unlock(&sColdLock);
        ++sKeyCounts.coldBad;
        CtrLog_Write(CTR_LOG_ERROR, "CONTRACT voxel cold mesh %d,%d of %d:%d changed in the cache",
                     site->cx, site->cy, inst->mapGroup, inst->mapNum);
        return false;
    }
    audit = AuditWanted();
    memcpy(sJobStatics, m->data + m->count * sizeof(VoxelGpuVertex), m->staticCount * sizeof(VoxelStaticDraw));
    sBuilder.count = sJob.packed = m->count;
    sBuilder.staticCount = m->staticCount;
    sJob.terrainCount = m->terrainCount;
    sJob.buildingFirst = m->buildingFirst;
    sJob.treeShadow = m->treeShadow;
    sJob.treeCrowns = m->treeCrowns;
    memcpy(sJob.terrainFirst, m->terrainFirst, sizeof(sJob.terrainFirst));
    sJob.parts = m->parts;
    sJob.gx0 = m->gx0;
    sJob.gz0 = m->gz0;
    sJob.gx1 = m->gx1;
    sJob.gz1 = m->gz1;
    sJob.gy0 = m->gy0;
    sJob.gy1 = m->gy1;
    sJob.sum = m->sum;
    memcpy(sJob.readIds, m->uvIds, sizeof(sJob.readIds));
    sJob.uvKey[0] = m->uvKey[0];
    sJob.uvKey[1] = m->uvKey[1];
    sJob.builtBase = m->base;
    memcpy(sJob.builtStretch, m->stretch, sizeof(sJob.builtStretch));
    sJob.builtBuildings = m->buildings;
    sJob.builtX = m->x;
    sJob.builtY = m->y;
    m->stamp = ++sColdStamp;
    LightLock_Unlock(&sColdLock);
    sJob.cold = true;
    sJob.coldAudit = audit;
    sJob.phase = sBuilder.staticCount != 0 ? JOB_STATICS : JOB_DONE;
    return true;
}

/*
 * The UV key of the build just finished, into sJob.uvKey: the slots it read,
 * and indoors the ids of its whole window - the black filler is read off the
 * drawing by their void bits (VoxelWorld_ClassifyTile). False when it has
 * none to be kept by: ids its atlas lacked, the table changed under it, or a
 * window reaching another pair's cells indoors.
 */
static bool JobUvKey(const VoxelAtlasSlot *atlas)
{
    const ChunkSite *site = &sJob.site;
    const VoxelMapInstance *inst = site->inst;

    if (!sKeysOn || site->border || sBuilder.uncovered != 0 || sBuilder.readMixed)
        return false;
    if (inst->indoor)
        for (int y = site->y0 - VOXEL_CHUNK_MARGIN_NORTH; y < site->y1 + 1; ++y)
            for (int x = site->x0 - 1; x < site->x1 + 1; ++x)
            {
                const VoxelMapInstance *at = VoxelWorld_GetInstanceAt(x, y);
                unsigned id;

                if (at == NULL)
                    continue;
                if (at->primaryTileset != inst->primaryTileset || at->secondaryTileset != inst->secondaryTileset)
                    return false;
                id = (unsigned)VoxelWorld_GetMetatileId(x, y);
                if (id < VOXEL_METATILE_IDS && !(sJob.readIds[id >> 5] & (1u << (id & 31u))))
                {
                    sJob.readIds[id >> 5] |= 1u << (id & 31u);
                    sJobReadSlots[id] = atlas->map.slotOf[id];
                }
            }
    UvKeyOf(sJob.readIds, sJobReadSlots, NULL, inst->primaryTileset, inst->secondaryTileset, sJob.uvKey);
    return true;
}

/* A chunk drawn as it is because its key matched: built again while drawn,
 * the new mesh compared (AuditEnd). */
static void AuditBegin(VoxelChunk *chunk)
{
    chunk->audit = 1;
    chunk->auditSum = chunk->sum;
    chunk->auditHash = chunk->hash;
    chunk->auditKey[0] = chunk->uvKey[0];
    chunk->auditKey[1] = chunk->uvKey[1];
}

/* The audited chunk built again (JobFinish, before its fields are replaced):
 * from the same world and slots, the same mesh, or its key was not complete.
 * Not compared when its square has moved in the world since it was built (a
 * crossing): kept through one by its SiteHash, as every chunk is, its shades
 * may round apart from a build made here (ColdMesh). */
static void AuditEnd(VoxelChunk *chunk, bool uvKnown)
{
    chunk->audit = 0;
    if (sJob.hash != chunk->auditHash || !uvKnown || sJob.uvKey[0] != chunk->auditKey[0]
     || sJob.uvKey[1] != chunk->auditKey[1] || chunk->builtX != sJob.site.baseX
     || chunk->builtY != sJob.site.baseY)
    {
        ++sKeyCounts.auditVoid;
        return;
    }
    ++sKeyCounts.audits;
    if (sJob.sum != chunk->auditSum)
    {
        ++sKeyCounts.auditStale;
        CtrLog_Write(CTR_LOG_ERROR, "CONTRACT voxel chunk %d,%d of %d:%d kept unlike its build "
                     "(%u verts kept, %u built)", chunk->cx, chunk->cy, chunk->mapGroup, chunk->mapNum,
                     chunk->count, sBuilder.count);
    }
}

/*
 * A chunk in the view whose atlas is not the one it was built under (a new
 * generation, or another slot of the cache with its pair): drawn as it is if
 * its ids have the same slots there (uv-kept), else left to be built again
 * (uv-changed), as every one was before. Tried once per atlas and generation.
 */
static bool ChunkAdopt(VoxelChunk *chunk, VoxelAtlasSlot *atlas)
{
    uint32_t key[2];

    if (!sKeysOn || !chunk->uvKnown || chunk->draft || chunk->border || !atlas->valid
     || chunk->primary != atlas->primaryTileset || chunk->secondary != atlas->secondaryTileset
     || (chunk->uvTried == atlas && chunk->uvTriedGeneration == atlas->generation))
        return false;
    UvKeyOf(chunk->uvIds, NULL, &atlas->map, chunk->primary, chunk->secondary, key);
    if (key[0] != chunk->uvKey[0] || key[1] != chunk->uvKey[1])
    {
        chunk->uvTried = atlas;
        chunk->uvTriedGeneration = atlas->generation;
        ++sKeyCounts.uvChanged;
        return false;
    }
    chunk->atlas = atlas;
    chunk->atlasGeneration = atlas->generation;
    chunk->atlasExtension = atlas->extension;
    ++sKeyCounts.uvKept;
    if (AuditWanted())
        AuditBegin(chunk);
    return true;
}

/*
 * â”€â”€ The draft of a square â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * A square the view needs and has not got is a hole, and a hole is black. Its
 * real build is 10-30 ms of lighting rays, trees and models on an Old 3DS - a
 * dozen frames of the time the frames can spare - so on a crossing, or round a
 * bend at a run, the player was on the ground before the ground was.
 *
 * The draft is what the square looks like without any of that: every cell's
 * own drawing, flat, unlit (VoxelMesh_DraftCell). A few hundred microseconds,
 * packed straight into the frame's staging and uploaded in the same frame the
 * hole is found, so nothing is ever drawn black where there is a map. It is
 * marked, and asked for again as an ordinary stale square: the real build
 * replaces it as soon as the frames have the time, drawn meanwhile as it is.
 *
 * It reads nothing of the build that may be running - the scratch, the
 * builder, the classification window are that job's - so it never waits for
 * it and never cancels it.
 */
#define VOXEL_DRAFT_PER_FRAME 4u
#define VOXEL_DRAFT_CELLS (VOXEL_CHUNK * VOXEL_CHUNK)

/*
 * ── The fill ────────────────────────────────────────────────────────────────
 * A square of the view the frame has neither mesh nor draft for is drawn as
 * one flat quad at its map's base, in the mean colour of its ground - green
 * for grass, blue for the sea, sand for the desert - so that the wide view
 * (see there) never shows the black of the backdrop. The colour comes from
 * the metatiles, which are known before any build (VoxelAtlasMap.mean, read
 * off the atlas's composition), and is kept per square in the wait table
 * until the world changes. Six vertices a square, in a small linear buffer
 * of their own, packed at the end of the update and drawn first
 * (DrawFills), under everything.
 */
/* The quad lies this far under the ground's level: never level with a
 * draft or a mesh that lands on the square the same frame. */
#define VOXEL_FILL_UNDER (1.0f / 16.0f)
/* Vertices are shorts of a 1/512 tile from the fill's origin: a square
 * further out than this from it is clipped to it. */
#define VOXEL_FILL_REACH 60.0f

typedef struct
{
    int16_t x0, z0, x1, z1; /* world tiles */
    float base;             /* the map's base: the quad's level, and rows south (VoxelBuilder_StretchZ) */
    uint32_t colour;        /* 0xAABBGGRR */
} FillSquare;

static FillSquare sFills[VOXEL_FILL_MAX];
static int sFillOriginX, sFillOriginZ;
/* The current map's own colour: its border block's, 0 while unknown. The
 * fill of a square whose map has no atlas yet, and the wide view's backdrop. */
static uint32_t sMapFill;

/* How far a square's centre stands from the camera's target, in tiles, the
 * larger of the two axes: the wide view's rings. */
static float SiteReach(const ChunkSite *site)
{
    float dx = fabsf((float)(site->x0 + site->x1) * 0.5f - sCamera.targetX);
    float dz = fabsf((float)(site->y0 + site->y1) * 0.5f - sCamera.targetZ);

    return dx > dz ? dx : dz;
}

/* The mean of RGBA5551 means (VoxelAtlasMap.mean) as 0xAABBGGRR; 0 for none. */
static uint32_t MeanColour(unsigned r, unsigned g, unsigned b, unsigned n)
{
    if (n == 0)
        return 0;
    return 0xFF000000u | (uint32_t)((b * 255u + 15u * n) / (31u * n)) << 16
         | (uint32_t)((g * 255u + 15u * n) / (31u * n)) << 8
         | (uint32_t)((r * 255u + 15u * n) / (31u * n));
}

/* The colour of a square's ground: the mean of its cells' metatiles (of the
 * belt, the border block that stands there), or 0 while the atlas has not
 * composed them. 64 lookups: a few microseconds, and kept per square. */
static uint32_t SquareColour(const ChunkSite *site, const VoxelAtlasSlot *atlas)
{
    unsigned r = 0, g = 0, b = 0, n = 0;

    if (atlas == NULL || !atlas->valid)
        return 0;
    for (int y = site->y0; y < site->y1; ++y)
        for (int x = site->x0; x < site->x1; ++x)
        {
            int m = site->border ? VoxelWorld_BorderMetatile(x, y) : VoxelWorld_GetMetatileId(x, y);
            uint16_t t;

            if (m < 0 || m >= (int)VOXEL_METATILE_IDS)
                continue;
            t = atlas->map.mean[m];
            if ((t & 1u) == 0)
                continue;
            r += t >> 11;
            g += (t >> 6) & 31u;
            b += (t >> 1) & 31u;
            ++n;
        }
    return MeanColour(r, g, b, n);
}

static void AddFill(const ChunkSite *site, uint32_t colour)
{
    FillSquare *f;

    if (!sWideView || colour == 0 || sFillCount >= VOXEL_FILL_MAX)
        return;
    f = &sFills[sFillCount++];
    f->x0 = (int16_t)site->x0;
    f->z0 = (int16_t)site->y0;
    f->x1 = (int16_t)site->x1;
    f->z1 = (int16_t)site->y1;
    f->base = VoxelRelief_Base(site->inst);
    f->colour = colour;
}

/* The current map's colour (sMapFill): its border block's mean, as the atlas
 * has it. Indoors there is no belt and the void is black: none. */
static void UpdateMapFill(const VoxelMapInstance *current)
{
    const VoxelAtlasSlot *atlas;
    int m;
    uint16_t t;

    sMapFill = 0;
    if (current == NULL || current->indoor)
        return;
    atlas = FindAtlas(current);
    if (atlas == NULL || !atlas->valid)
        return;
    m = VoxelWorld_BorderMetatile((int)floorf(sCamera.targetX), (int)floorf(sCamera.targetZ));
    if (m < 0 || m >= (int)VOXEL_METATILE_IDS)
        return;
    t = atlas->map.mean[m];
    if (t & 1u)
        sMapFill = MeanColour(t >> 11, (t >> 6) & 31u, (t >> 1) & 31u, 1u);
}

static float Clamp(float value, float lo, float hi);

/* The frame's fills as vertices, from the camera's tile (DrawFills). */
static void PackFills(void)
{
    VoxelVertex quad[6];

    sFillOriginX = (int)floorf(sCamera.targetX);
    sFillOriginZ = (int)floorf(sCamera.targetZ);
    if (sFillGpu == NULL)
    {
        sFillCount = 0;
        return;
    }
    /* By colour, so that DrawFills sets the colour and draws once per run
     * (the sea, a meadow: most of a wide view is a handful of colours). */
    for (unsigned i = 1; i < sFillCount; ++i)
    {
        FillSquare f = sFills[i];
        unsigned j = i;

        for (; j > 0 && sFills[j - 1u].colour > f.colour; --j)
            sFills[j] = sFills[j - 1u];
        sFills[j] = f;
    }
    for (unsigned i = 0; i < sFillCount; ++i)
    {
        const FillSquare *f = &sFills[i];
        float x0 = Clamp((float)(f->x0 - sFillOriginX), -VOXEL_FILL_REACH, VOXEL_FILL_REACH);
        float x1 = Clamp((float)(f->x1 - sFillOriginX), -VOXEL_FILL_REACH, VOXEL_FILL_REACH);
        float z0 = Clamp((float)f->z0 + f->base - (float)sFillOriginZ, -VOXEL_FILL_REACH, VOXEL_FILL_REACH);
        float z1 = Clamp((float)f->z1 + f->base - (float)sFillOriginZ, -VOXEL_FILL_REACH, VOXEL_FILL_REACH);
        float y = f->base - VOXEL_FILL_UNDER;

        quad[0] = (VoxelVertex){x0, y, z0, 0.0f, 0.0f, 1.0f};
        quad[1] = (VoxelVertex){x1, y, z0, 0.0f, 0.0f, 1.0f};
        quad[2] = (VoxelVertex){x1, y, z1, 0.0f, 0.0f, 1.0f};
        quad[3] = quad[0];
        quad[4] = quad[2];
        quad[5] = (VoxelVertex){x0, y, z1, 0.0f, 0.0f, 1.0f};
        Pack(quad, 6, sFillGpu + i * 6u);
    }
    if (sFillCount != 0)
        GSPGPU_FlushDataCache(sFillGpu, sFillCount * 6u * sizeof(VoxelGpuVertex));
}

static unsigned sDrafts;
static float sDraftMs;

/* The frame's own vertices (sprites, their shadows and reflections, fog), by
 * the fast packing where the console has proved it: the old one's VFP compares
 * per value were most of what packing the sprites cost. */
static void PackDynamic(const VoxelVertex *src, unsigned count, VoxelGpuVertex *dst)
{
#ifdef VOXEL_HAVE_FAST_PACK
    if (sFastPack)
    {
        PackFloor(src, count, dst, NULL);
        return;
    }
#endif
    Pack(src, count, dst);
}

/* The draft's vertices a cell at a time, by the fast packing where the console
 * has proved it (PackSelfTest) and measuring them as it goes. */
static void PackDraft(const VoxelVertex *src, unsigned count, VoxelGpuVertex *dst,
                      PackBounds *bounds)
{
#ifdef VOXEL_HAVE_FAST_PACK
    if (sFastPack)
    {
        PackFloor(src, count, dst, bounds);
        return;
    }
#endif
    Pack(src, count, dst);
    for (unsigned i = 0; i < count; ++i)
        GrowBounds(bounds, &dst[i]);
}

static VoxelChunk *DraftChunk(const ChunkSite *site, VoxelAtlasSlot *atlas, VoxelChunk *chunk)
{
    const VoxelMapInstance *inst = site->inst;
    int16_t slotOf[VOXEL_DRAFT_CELLS];
    unsigned terrainFirst[VOXEL_ATLAS_PAGES + 1u];
    unsigned uncovered = 0, total = 0, bytes, written = 0, width, height;
    VoxelVertex quad[6];
    VoxelBuilder b;
    VoxelGpuVertex *staging;
    PackBounds bounds;

    if (site->border)
        return NULL;
    width = (unsigned)(site->x1 - site->x0);
    height = (unsigned)(site->y1 - site->y0);
    if (width > VOXEL_CHUNK || height > VOXEL_CHUNK)
        return NULL;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            int slot = VoxelMesh_DraftSlot(inst, &atlas->map, site->x0 + (int)x, site->y0 + (int)y,
                                           &uncovered);

            if (slot >= (int)(VOXEL_ATLAS_PAGES * VOXEL_ATLAS_MAX_SLOTS))
                slot = -1;
            slotOf[y * VOXEL_CHUNK + x] = (int16_t)slot;
            total += slot >= 0 ? 6u : 0u;
        }
    bytes = total * (unsigned)sizeof(VoxelGpuVertex);
    if (sStagingUsed + total > STAGING_ROOM || sChunkUploads >= VOXEL_CHUNK_UPLOADS_MAX)
        return NULL;
    if (bytes != 0 && !CtrVideo_TryVoxelUpload())
        return NULL;

    sStagingPromise = total;
    if (chunk == NULL)
    {
        chunk = FreeChunkSlot(true);
        if (chunk == NULL)
        {
            sStagingPromise = 0;
            return NULL;
        }
        EvictChunk(chunk);
    }
    if (bytes != 0 && (chunk->vram == NULL || chunk->bytes < bytes))
    {
        VoxelGpuVertex *block;

        if (chunk->used)
            chunk->stamp = chunk->viewStamp = sFrame;
        /* Small pieces from the top of the block, the real meshes from the
         * bottom: the drafts come and go and must not strand the big gaps. */
        block = VoxelArena_Alloc(&sChunkArena, bytes, true);
        if (block == NULL)
            block = ChunkVram(bytes, true);
        if (block == NULL)
        {
            sStagingPromise = 0;
            return NULL;
        }
        ChunkBlockFree(chunk->vram);
        chunk->vram = block;
        chunk->bytes = bytes;
    }

    sStagingPromise = 0;
    VoxelBuilder_Init(&b, quad, 6);
    VoxelBuilder_SetAtlas(&b, &atlas->map);
    VoxelBuilder_SetOrigin(&b, site->baseX, site->baseY);
    b.base = VoxelRelief_Base(inst);
    if (!site->border)
        VoxelBuilder_SetStretch(&b, inst);
    b.lighting = false;
    staging = sStaging + sStagingUsed;
    for (int a = 0; a < 3; ++a)
    {
        bounds.lo[a] = INT32_MAX;
        bounds.hi[a] = INT32_MIN;
    }
    /* By atlas page, as the draw takes them (JobSort does the same for a
     * real build): one pass over the cells for each page. */
    for (unsigned page = 0; page < VOXEL_ATLAS_PAGES; ++page)
    {
        terrainFirst[page] = written;
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
            {
                int slot = slotOf[y * VOXEL_CHUNK + x];

                if (slot < 0 || (unsigned)slot / VOXEL_ATLAS_MAX_SLOTS != page)
                    continue;
                b.count = 0;
                VoxelMesh_DraftCell(&b, inst, site->x0 + (int)x, site->y0 + (int)y, slot);
                /* U carries page*2 until packed: the draw binds the page. */
                for (unsigned k = 0; k < 6; ++k)
                    quad[k].u -= 2.0f * (float)page;
                PackDraft(quad, 6, staging + written, &bounds);
                written += 6;
            }
    }
    terrainFirst[VOXEL_ATLAS_PAGES] = written;
    ReportPackErrors("draft chunk", site->cx, site->cy, inst->mapGroup, inst->mapNum);
    if (bytes != 0)
    {
        GSPGPU_FlushDataCache(staging, bytes);
        C3D_SyncTextureCopy((u32 *)staging, 0, (u32 *)chunk->vram, 0, bytes, 8);
        sStagingUsed += total;
        ++sChunkUploads;
    }

    if (chunk->used)
        ChunkUnindex(chunk);
    chunk->used = true;
    chunk->border = false;
    chunk->mapGroup = inst->mapGroup;
    chunk->mapNum = inst->mapNum;
    chunk->cx = site->cx;
    chunk->cy = site->cy;
    chunk->beltGrid = sBeltGrid;
    chunk->draft = true;
    ChunkIndex(chunk);
    /* Never the signature of anything: asked for again at once. */
    chunk->hash = chunk->epoch = 0;
    chunk->staleHash = chunk->staleEpoch = 0;
    chunk->primary = inst->primaryTileset;
    chunk->secondary = inst->secondaryTileset;
    chunk->layout = inst->layout;
    chunk->atlas = atlas;
    chunk->atlasGeneration = atlas->generation;
    chunk->atlasExtension = atlas->extension;
    chunk->uncovered = uncovered != 0;
    chunk->count = chunk->terrainCount = chunk->buildingFirst = total;
    chunk->treeShadow = chunk->treeCrowns = 0;
    memcpy(chunk->terrainFirst, terrainFirst, sizeof(chunk->terrainFirst));
    chunk->parts.on = false;
    chunk->gx0 = chunk->gz0 = 0;
    chunk->gx1 = chunk->gz1 = VOXEL_CHUNK;
    chunk->gy0 = chunk->gy1 = 0.0f;
    if (total != 0)
    {
        int lx = PackedFloor(bounds.lo[0]), lz = PackedFloor(bounds.lo[2]);
        int hx = PackedCeil(bounds.hi[0]), hz = PackedCeil(bounds.hi[2]);

        if (lx < chunk->gx0) chunk->gx0 = lx;
        if (lz < chunk->gz0) chunk->gz0 = lz;
        if (hx > chunk->gx1) chunk->gx1 = hx;
        if (hz > chunk->gz1) chunk->gz1 = hz;
        chunk->gy0 = (float)bounds.lo[1] / VOXEL_POS_SCALE;
        chunk->gy1 = (float)bounds.hi[1] / VOXEL_POS_SCALE;
    }
    chunk->buildingPage = -1;
    chunk->staticCount = 0;
    chunk->buildMs = 0.0f;
    chunk->uvKnown = false;
    chunk->audit = 0;
    chunk->stamp = chunk->viewStamp = sFrame;
    /* Ids the atlas has never seen: it grows to take them (see AcquireAtlas). */
    if (uncovered != 0)
        atlas->extendPending = true;
    ++sDrafts;
    return chunk;
}

/* â”€â”€ The view â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/*
 * Chunks that need building this frame, most urgent first: a hole in the view
 * before a stale square that can still be drawn as it is, both before
 * anything ahead of time, and within each class the nearest to the player.
 */
#define VOXEL_REQUESTS_MAX 192u

enum
{
    NEED_HOLE,     /* in view, nothing to draw in its place */
    NEED_STALE,    /* in view, the old mesh stands meanwhile */
    NEED_AHEAD,    /* in the prefetch ring */
    NEED_AHEAD_STALE
};

typedef struct
{
    ChunkSite site;
    VoxelAtlasSlot *atlas;
    VoxelChunk *chunk;  /* the stale chunk, or NULL for a new one */
    uint32_t hash;
    bool hashKnown;     /* else computed if and when it is built */
    bool done;          /* taken as a job, or being built by the running one */
    unsigned key;       /* class, then distance */
    unsigned need, waitIndex;
} BuildRequest;

static BuildRequest sRequests[VOXEL_REQUESTS_MAX];
static unsigned sRequestCount;
/* With the list full: its entry of largest key (Request), and how many were
 * turned away since the last log. */
static unsigned sRequestWorst, sRequestsDropped;

/* Retain progress across the frame-local request list, recycling its oldest
 * entry when full. Map identity never uses a mutable instance pointer. */
static struct
{
    int group, map, cx, cy;
    bool border, used;
    uint32_t seen, progress;
    uint32_t noRoomUntil; /* not built before this frame (RunJobs, BUILD_NO_VRAM) */
    /* The square's fill colour (SquareColour), as of this epoch of the world
     * and this extension of its atlas; 0 unknown. */
    uint32_t fill, fillEpoch, fillExtension;
} sRequestWait[VOXEL_REQUESTS_MAX];

/* The wait table is looked up once per request, every frame: chained by hash
 * of the square, like the chunks (the entry number plus one; 0 ends a chain). */
static uint8_t sWaitHead[128];
static uint8_t sWaitNext[VOXEL_REQUESTS_MAX];

static unsigned WaitBucket(int group, int map, int cx, int cy, bool border)
{
    uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u
               ^ (uint32_t)group * 83492791u ^ (uint32_t)map * 2654435761u
               ^ (border ? 0x9E3779B1u : 0u);

    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h & (sizeof(sWaitHead) - 1u);
}

/* `reuse`, if not negative, is the record to take on a miss: the one of the
 * request this one displaces from a full list (Request), its only user. Every
 * record of a full list was seen this frame, and the oldest would be a live
 * request's. */
static unsigned RequestWait(const ChunkSite *site, int reuse)
{
    unsigned bucket = WaitBucket(site->inst->mapGroup, site->inst->mapNum, site->cx, site->cy,
                                 site->border);
    unsigned oldest = 0, link;

    for (link = sWaitHead[bucket]; link != 0; link = sWaitNext[link - 1u])
    {
        unsigned i = link - 1u;

        if (sRequestWait[i].used && sRequestWait[i].group == site->inst->mapGroup
         && sRequestWait[i].map == site->inst->mapNum && sRequestWait[i].border == site->border
         && sRequestWait[i].cx == site->cx && sRequestWait[i].cy == site->cy)
        {
            /* A square that drops out of the view and comes back waits from
             * now: its old record's `progress` made it overdue at once and
             * bought it a forced slice inside the frame. */
            if (sRequestWait[i].seen + 1u < sFrame)
                sRequestWait[i].progress = sFrame;
            sRequestWait[i].seen = sFrame;
            return i;
        }
    }
    if (reuse >= 0 && (unsigned)reuse < VOXEL_REQUESTS_MAX)
        oldest = (unsigned)reuse;
    else
        for (unsigned i = 0; i < VOXEL_REQUESTS_MAX; ++i)
            if (!sRequestWait[i].used || sRequestWait[i].seen < sRequestWait[oldest].seen)
                oldest = i;
    if (sRequestWait[oldest].used)
    {
        /* Out of its chain before it names another square. */
        uint8_t *prev = &sWaitHead[WaitBucket(sRequestWait[oldest].group, sRequestWait[oldest].map,
                                              sRequestWait[oldest].cx, sRequestWait[oldest].cy,
                                              sRequestWait[oldest].border)];

        while (*prev != 0 && (unsigned)*prev - 1u != oldest)
            prev = &sWaitNext[*prev - 1u];
        if (*prev != 0)
            *prev = sWaitNext[oldest];
    }
    sRequestWait[oldest].group = site->inst->mapGroup;
    sRequestWait[oldest].map = site->inst->mapNum;
    sRequestWait[oldest].cx = site->cx;
    sRequestWait[oldest].cy = site->cy;
    sRequestWait[oldest].border = site->border;
    sRequestWait[oldest].fill = 0; /* another square's colour */
    sRequestWait[oldest].used = true;
    sRequestWait[oldest].seen = sRequestWait[oldest].progress = sFrame;
    sRequestWait[oldest].noRoomUntil = 0;
    sWaitNext[oldest] = sWaitHead[bucket];
    sWaitHead[bucket] = (uint8_t)(oldest + 1u);
    return oldest;
}

/* A square held by its draft (DraftChunk) is on screen already: it is built
 * for real at the pace the frames allow, and only rescued once it has waited
 * this long. */
#define VOXEL_DRAFT_STARVE_FRAMES 90u

static bool RequestOverdue(const BuildRequest *r)
{
    unsigned limit = r->chunk != NULL && r->chunk->draft ? VOXEL_DRAFT_STARVE_FRAMES
                                                          : VOXEL_STARVE_FRAMES;

    /* Waiting for room, not for time: no rescue would find it any. */
    if (sFrame < sRequestWait[r->waitIndex].noRoomUntil)
        return false;
    return r->need <= NEED_STALE && sFrame - sRequestWait[r->waitIndex].progress >= limit;
}


static bool IsChunkOf(const VoxelChunk *chunk, const ChunkSite *site)
{
    return chunk->used && chunk->border == site->border
        && (site->border ? chunk->beltGrid == sBeltGrid
                         : chunk->mapGroup == site->inst->mapGroup
                           && chunk->mapNum == site->inst->mapNum)
        && chunk->cx == site->cx && chunk->cy == site->cy;
}

static bool Overlaps(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1)
{
    return ax0 < bx1 && ax1 > bx0 && ay0 < by1 && ay1 > by0;
}

/*
 * On screen? Once built, by the box its geometry really fills: a building is
 * emitted by the chunk of its top-left cell and stands south of it, and a
 * roof is seen from further than the ground under it. Not built yet, by its
 * tiles at the heights of ground and trees over its map's base.
 */
static bool SiteVisible(const ChunkSite *site, const VoxelChunk *chunk)
{
    float base;

    if (chunk != NULL && chunk->count != 0)
        return BoxVisible((float)(site->baseX + chunk->gx0), chunk->gy0 - 0.05f,
                          (float)(site->baseY + chunk->gz0),
                          (float)(site->baseX + chunk->gx1), chunk->gy1 + 0.05f,
                          (float)(site->baseY + chunk->gz1));
    base = VoxelRelief_Base(site->inst);
    /* (as high as its base, and as far south: VoxelBuilder_StretchZ) */
    return BoxVisible((float)site->x0, base + VOXEL_UNBUILT_LOW, (float)site->y0 + base,
                      (float)site->x1, base + VOXEL_UNBUILT_HIGH, (float)site->y1 + base);
}

/* Does the slab meet the frustum? x and y between theirs and z - y between
 * w0 and w1 (VoxelChunkParts), from (ox, oz). As BoxVisible: outside only if
 * wholly behind one plane, whose furthest point forward is picked by the
 * plane's own slopes - along y, its y's and its z's together. */
static bool SlabVisible(float ox, float oz, float x0, float y0, float w0, float x1, float y1, float w1)
{
    if (!sFrustumValid)
        return true;
    for (int i = 0; i < 4; ++i)
    {
        const VoxelPlane *p = &sFrustum[i];
        float rise = p->y + p->z;
        float x = p->x > 0.0f ? x1 : x0, y = rise > 0.0f ? y1 : y0, w = p->z > 0.0f ? w1 : w0;

        if (p->x * (ox + x) + rise * y + p->z * (oz + w) + p->w < 0.0f)
            return false;
    }
    return true;
}

/* Which parts of the chunk's ground meet the frustum (VoxelChunkParts), as
 * bits; all of them where it is not cut in parts or the switch is off. */
static unsigned ChunkPartsSeen(const VoxelChunk *chunk, int worldX, int worldZ)
{
    unsigned seen = 0;

    if (!chunk->parts.on || !sMeasureParts)
        return VOXEL_PARTS_ALL;
    for (unsigned part = 0; part < VOXEL_PARTS; ++part)
    {
        const float *box = chunk->parts.box[part];

        /* (the margins: a packed vertex is rounded to 1/512 of a tile) */
        if (box[0] <= box[3]
         && SlabVisible((float)worldX, (float)worldZ, box[0] - 0.01f, box[1] - 0.05f, box[2] - 0.06f,
                        box[3] + 0.01f, box[4] + 0.05f, box[5] + 0.06f))
            seen |= 1u << part;
    }
    return seen;
}

/* A page's ground in the chunk: where part `part` begins (4: where the page ends). */
static unsigned ChunkPartFirst(const VoxelChunk *chunk, unsigned page, unsigned part)
{
    return part == 0 ? chunk->terrainFirst[page]
         : part < VOXEL_PARTS ? chunk->parts.end[page][part - 1u] : chunk->terrainFirst[page + 1u];
}

/* The ground's vertices the parts not in `seen` hold. */
static unsigned ChunkPartsLeftOut(const VoxelChunk *chunk, unsigned seen)
{
    unsigned out = 0;

    if (seen == VOXEL_PARTS_ALL)
        return 0;
    for (unsigned page = 0; page < VOXEL_ATLAS_PAGES; ++page)
        for (unsigned part = 0; part < VOXEL_PARTS; ++part)
            if (!(seen >> part & 1u))
                out += ChunkPartFirst(chunk, page, part + 1u) - ChunkPartFirst(chunk, page, part);
    return out;
}

/*
 * Which parts of each chunk on the list are in the picture: told at the
 * draw and not when the chunk went on the list, because a build finished
 * later in the same update is adopted into the slot of the chunk it
 * replaces, with parts of its own (the audit caught a part in sight left
 * out that way). The frustum is still the update's. The vertices left out
 * come off the update's count once, however many pictures are drawn of it.
 */
static void DrawParts(void)
{
    static uint32_t counted = ~0u;
    bool count = counted != sFrame;

    counted = sFrame;
    for (unsigned i = 0; i < sDrawCount; ++i)
    {
        sDraws[i].parts = (uint8_t)ChunkPartsSeen(sDraws[i].chunk, sDraws[i].worldX, sDraws[i].worldZ);
        if (count)
        {
            unsigned out = ChunkPartsLeftOut(sDraws[i].chunk, sDraws[i].parts);

            sStats.vertices -= out < sStats.vertices ? out : sStats.vertices;
        }
    }
}

static void Draw(VoxelChunk *chunk, int worldX, int worldZ)
{
    if (chunk->count == 0)
        return;
    if (sDrawCount >= VOXEL_DRAW_MAX)
    {
        static bool told;

        if (!told)
        {
            told = true;
            ++sStats.errors;
            CtrLog_Write(CTR_LOG_ERROR, "VOXEL: draw list full (%u), chunks left out", VOXEL_DRAW_MAX);
        }
        return;
    }
    /* On the draw list, so on screen this frame: a later view job's eviction
     * (OldestChunk) must not empty a slot sDraws points at - an ahead job's
     * chunk drawn here kept an older viewStamp. */
    chunk->viewStamp = sFrame;
    sDraws[sDrawCount].chunk = chunk;
    sDraws[sDrawCount].worldX = worldX;
    sDraws[sDrawCount].worldZ = worldZ;
    sDraws[sDrawCount].parts = VOXEL_PARTS_ALL; /* told at the draw (DrawParts) */
    ++sDrawCount;
    sStats.vertices += chunk->count;
    sStats.draftsVisible += chunk->draft;
}

/*
 * `voxel_view_audit=N` in settings.txt: every N frames, how much of the
 * ground the chunks on the draw list hold is out of the picture, and how much
 * of that a finer pick would leave out - by whole triangles (the most there
 * is to gain), and by blocks of 4, 2 and 1 cells boxed as the chunks are.
 * One line in the log. Reads the chunks' vertices back: a diagnostic.
 */
static void ViewAudit(void)
{
    static const int sizes[3] = {4, 2, 1};
    unsigned total = 0, outTri = 0, outBlock[3] = {0, 0, 0}, chunks = 0, wrong = 0;

    for (unsigned i = 0; i < sDrawCount; ++i)
    {
        const VoxelChunk *chunk = sDraws[i].chunk;
        const VoxelGpuVertex *v = chunk->vram;
        unsigned count = chunk->terrainCount;
        float ox = (float)sDraws[i].worldX, oz = (float)sDraws[i].worldZ;

        if (v == NULL || count == 0)
            continue;
        ++chunks;
        total += count;
        if (count != chunk->terrainFirst[VOXEL_ATLAS_PAGES])
        {
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL VIEW AUDIT odd: draw %u chunk %d,%d draft %d border %d parts %d terrain %u "
                         "pages end %u count %u", i, chunk->cx, chunk->cy, chunk->draft, chunk->border, chunk->parts.on,
                         count, chunk->terrainFirst[VOXEL_ATLAS_PAGES], chunk->count);
            if (count > chunk->terrainFirst[VOXEL_ATLAS_PAGES])
                count = chunk->terrainFirst[VOXEL_ATLAS_PAGES];
        }
        for (unsigned t = 0; t + 2 < count; t += 3)
        {
            bool out = false;

            for (int p = 0; p < 4 && !out; ++p)
            {
                const VoxelPlane *pl = &sFrustum[p];
                int behind = 0;

                for (int k = 0; k < 3; ++k)
                    behind += pl->x * (ox + v[t + k].x / 512.0f) + pl->y * (v[t + k].y / 512.0f)
                            + pl->z * (oz + v[t + k].z / 512.0f) + pl->w < 0.0f;
                out = behind == 3;
            }
            outTri += out ? 3u : 0u;
            if (!out && sDraws[i].parts != VOXEL_PARTS_ALL)
            {
                /* in the picture: its part must be drawn */
                unsigned page = 0, part = 0;

                while (t >= chunk->terrainFirst[page + 1u])
                    ++page;
                while (part < VOXEL_PARTS - 1u && t >= chunk->parts.end[page][part])
                    ++part;
                if (!(sDraws[i].parts >> part & 1u))
                {
                    const float *b = chunk->parts.box[part];

                    if (wrong == 0)
                        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL VIEW AUDIT wrong: draw %u at %d,%d chunk %d,%d draft %d border %d t %u "
                                     "page %u part %u seen %x firsts %u %u ends %u %u %u count %u box %.2f %.2f %.2f .. %.2f "
                                     "%.2f %.2f tri %d,%d,%d %d,%d,%d %d,%d,%d", i, sDraws[i].worldX, sDraws[i].worldZ,
                                     chunk->cx, chunk->cy, chunk->draft, chunk->border, t, page, part, sDraws[i].parts,
                                     chunk->terrainFirst[page], chunk->terrainFirst[page + 1u], chunk->parts.end[page][0],
                                     chunk->parts.end[page][1], chunk->parts.end[page][2], chunk->terrainCount,
                                     (double)b[0], (double)b[1], (double)b[2], (double)b[3], (double)b[4], (double)b[5],
                                     v[t].x, v[t].y, v[t].z, v[t + 1].x, v[t + 1].y, v[t + 1].z,
                                     v[t + 2].x, v[t + 2].y, v[t + 2].z);
                    wrong += 3u;
                }
            }
        }
        for (int s = 0; s < 3; ++s)
        {
            /* cells by the triangle's middle on the ground plan, 24 a side
             * with the margins a chunk's geometry may reach into */
            enum { SIDE = 24 };
            static float box[SIDE * SIDE][6];
            static uint16_t held[SIDE * SIDE];
            int size = sizes[s], side = SIDE / size;

            memset(held, 0, sizeof(held));
            for (unsigned t = 0; t + 2 < count; t += 3)
            {
                float mx = (v[t].x + v[t + 1].x + v[t + 2].x) / (3.0f * 512.0f);
                float mz = (v[t].z + v[t + 1].z + v[t + 2].z) / (3.0f * 512.0f);
                int cx = ((int)floorf(mx) + 8) / size, cz = ((int)floorf(mz) + 8) / size, at;

                cx = cx < 0 ? 0 : cx >= side ? side - 1 : cx;
                cz = cz < 0 ? 0 : cz >= side ? side - 1 : cz;
                at = cz * side + cx;
                for (int k = 0; k < 3; ++k)
                {
                    float x = ox + v[t + k].x / 512.0f, y = v[t + k].y / 512.0f, z = oz + v[t + k].z / 512.0f;

                    if (held[at] == 0 && k == 0)
                    {
                        box[at][0] = box[at][3] = x;
                        box[at][1] = box[at][4] = y;
                        box[at][2] = box[at][5] = z;
                    }
                    if (x < box[at][0]) box[at][0] = x;
                    if (y < box[at][1]) box[at][1] = y;
                    if (z < box[at][2]) box[at][2] = z;
                    if (x > box[at][3]) box[at][3] = x;
                    if (y > box[at][4]) box[at][4] = y;
                    if (z > box[at][5]) box[at][5] = z;
                }
                held[at] += 3;
            }
            for (int at = 0; at < side * side; ++at)
                if (held[at] != 0 && !BoxVisible(box[at][0], box[at][1], box[at][2], box[at][3], box[at][4], box[at][5]))
                    outBlock[s] += held[at];
        }
    }
    {
        unsigned outside = 0, wide = 0, partsOut = 0, hist[6] = {0};

        for (unsigned i = 0; i < sDrawCount; ++i)
        {
            const VoxelChunk *chunk = sDraws[i].chunk;
            const VoxelGpuVertex *v = chunk->vram;

            if (v == NULL)
                continue;
            partsOut += ChunkPartsLeftOut(chunk, sDraws[i].parts);
            for (unsigned t = 0; t + 2 < chunk->terrainCount; t += 3)
            {
                int lo[2] = {32767, 32767}, hi[2] = {-32768, -32768}, mx, mz, span;

                for (int k = 0; k < 3; ++k)
                {
                    if (v[t + k].x < lo[0]) lo[0] = v[t + k].x;
                    if (v[t + k].z < lo[1]) lo[1] = v[t + k].z;
                    if (v[t + k].x > hi[0]) hi[0] = v[t + k].x;
                    if (v[t + k].z > hi[1]) hi[1] = v[t + k].z;
                }
                mx = v[t].x + v[t + 1].x + v[t + 2].x;
                mz = v[t].z + v[t + 1].z + v[t + 2].z;
                outside += mx < 0 || mz < 0 || mx >= 3 * 8 * 512 || mz >= 3 * 8 * 512 ? 3u : 0u;
                span = hi[0] - lo[0] > hi[1] - lo[1] ? hi[0] - lo[0] : hi[1] - lo[1];
                wide += span > 2 * 512 ? 3u : 0u;
                hist[span <= 512 ? 0 : span <= 1024 ? 1 : span <= 2048 ? 2 : span <= 4096 ? 3 : span <= 8192 ? 4 : 5] += 3u;
            }
        }
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL VIEW AUDIT parts leave out %u, WRONGLY %u; middles outside the square %u; "
                     "wider than 2 tiles %u; spans <=1 %u <=2 %u <=4 %u <=8 %u <=16 %u more %u", partsOut, wrong,
                     outside, wide, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
        if (wrong != 0)
            ++sStats.errors;
    }
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL VIEW AUDIT frame %lu: %u chunks, ground %u vertices, out of the picture %u "
                 "by triangle, %u by 4x4, %u by 2x2, %u by cell", (unsigned long)sFrame, chunks, total, outTri,
                 outBlock[0], outBlock[1], outBlock[2]);
}

static void Request(const ChunkSite *site, VoxelAtlasSlot *atlas, VoxelChunk *chunk,
                    const uint32_t *hash, unsigned need, float playerX, float playerZ)
{
    BuildRequest *r;
    int displaced = -1;
    float dx = (site->x0 + site->x1) * 0.5f - playerX;
    float dz = (site->y0 + site->y1) * 0.5f - playerZ;
    unsigned key = need * 100000u + (unsigned)(dx * dx + dz * dz);

    /*
     * Full: the list is visited map by map, so what came first is not what
     * matters most - a hole of the second map on screen was turned away for
     * the first map's ring. The new request takes the place of the least
     * urgent one if it is more urgent.
     */
    if (sRequestCount >= VOXEL_REQUESTS_MAX)
    {
        static uint32_t sDropLogged;

        ++sRequestsDropped;
        if (sFrame - sDropLogged >= 600u)
        {
            sDropLogged = sFrame;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: request list full (%u), %u turned away",
                         VOXEL_REQUESTS_MAX, sRequestsDropped);
            sRequestsDropped = 0;
        }
        if (key >= sRequests[sRequestWorst].key)
            return;
        r = &sRequests[sRequestWorst];
        displaced = (int)r->waitIndex;
    }
    else
        r = &sRequests[sRequestCount++];
    r->site = *site;
    r->atlas = atlas;
    r->chunk = chunk;
    r->hashKnown = hash != NULL;
    r->done = false;
    r->hash = hash != NULL ? *hash : 0;
    r->need = need;
    r->waitIndex = RequestWait(site, displaced);
    r->key = key;
    if (sRequestCount == VOXEL_REQUESTS_MAX)
    {
        /* The new worst: once when the list fills, then after each swap. */
        sRequestWorst = 0;
        for (unsigned i = 1; i < VOXEL_REQUESTS_MAX; ++i)
            if (sRequests[i].key > sRequests[sRequestWorst].key)
                sRequestWorst = i;
    }
}

/* The tiles of a belt square that no map covers. */
static unsigned OpenTiles(const ChunkSite *site)
{
    unsigned open = 0;

    for (int y = site->y0; y < site->y1; ++y)
        for (int x = site->x0; x < site->x1; ++x)
            open += VoxelWorld_GetInstanceAt(x, y) == NULL;
    return open;
}

/* Does the belt square still lie where no map is, as it did when built?
 * Asked again only when the world has changed. */
static bool BeltCoverSame(VoxelChunk *chunk, const ChunkSite *site)
{
    if (chunk->openEpoch != sEpoch)
    {
        chunk->openSame = OpenTiles(site) == chunk->openTiles;
        chunk->openEpoch = sEpoch;
    }
    return chunk->openSame;
}

/* Signatures VisitSite may compute this frame (see there); set by UpdateView. */
#define VOXEL_HASHES_PER_FRAME 10u
static unsigned sHashesLeft;

/*
 * One square of the view or of the ring around it: drawn if it can be, asked
 * for if it has to be built.
 */
static void VisitSite(const ChunkSite *site, VoxelAtlasSlot *atlas,
                      float playerX, float playerZ, unsigned *missing)
{
    const VoxelMapInstance *inst = site->inst;
    VoxelChunk *chunk = FindChunk(site->border, inst->mapGroup, inst->mapNum,
                                  site->cx, site->cy);
    bool drawable, stale = false, hashKnown = false;
    uint32_t hash = 0;
    bool inView = SiteVisible(site, chunk);

    if (chunk == NULL)
    {
        if (atlas != NULL)
            Request(site, atlas, NULL, NULL, inView ? NEED_HOLE : NEED_AHEAD,
                    playerX, playerZ);
        else if (inView)
            AddFill(site, sMapFill); /* no atlas to read its own colour from */
        if (inView)
            ++*missing;
        return;
    }

    chunk->stamp = sFrame;
    if (inView)
        chunk->viewStamp = sFrame;
    /* An old UV range cannot be drawn with recycled atlas pixels: keep old
     * geometry only while its material is still the one it was built for.
     * A belt square is drawn with its own atlas: after a crossing it holds
     * the last map's border until it is built from the new one's, which is
     * what the view shows meanwhile - unless maps have come to cover part
     * of it, whose ground it would stand on. */
    if (site->border)
        drawable = chunk->atlas->valid && chunk->atlasGeneration == chunk->atlas->generation
                && BeltCoverSame(chunk, site);
    else
    {
        drawable = atlas != NULL && chunk->atlas == atlas
                && chunk->atlasGeneration == atlas->generation;
        /* Another atlas of its pair: drawn as it is if its slots are the
         * same there (the UV key). */
        if (!drawable && atlas != NULL && chunk->layout == inst->layout)
            drawable = ChunkAdopt(chunk, atlas);
    }
    if (atlas == NULL)
    {
        if (!inView)
            return;
        if (drawable)
            Draw(chunk, site->baseX, site->baseY);
        else
        {
            AddFill(site, sMapFill);
            ++*missing;
        }
        return;
    }
    if (!drawable || chunk->layout != inst->layout
     || chunk->primary != inst->primaryTileset || chunk->secondary != inst->secondaryTileset)
        stale = true;
    else if (chunk->atlas != atlas)
        stale = true; /* a belt square drawn from another pair's atlas */
    else if (chunk->draft)
        stale = true; /* only its draft stands (DraftChunk); drawable meanwhile */
    else if (chunk->uncovered && chunk->atlasExtension != atlas->extension)
        stale = true; /* the atlas grew the ids it was missing; still drawable */
    else if (chunk->audit != 0)
        stale = true; /* an audit owed (AuditBegin): built again, drawn meanwhile */
    else if (chunk->staleEpoch == sEpoch)
    {
        stale = hashKnown = true;
        hash = chunk->staleHash;
    }
    else if (chunk->epoch != sEpoch && sHashesLeft != 0)
    {
        /* The world moved since this chunk was last verified: the one time
         * its signature is worth computing. A crossing moves it for every
         * chunk at once, and verifying them all in the frame after it was
         * 3-4 ms on top of the frame that loaded the map: a few a frame
         * instead, the others drawn as they are until their turn. */
        --sHashesLeft;
        hash = SiteHash(site);
        hashKnown = true;
        if (hash == chunk->hash)
            chunk->epoch = sEpoch;
        else
        {
            stale = true;
            chunk->staleHash = hash;
            chunk->staleEpoch = sEpoch;
        }
    }
    if (stale)
        Request(site, atlas, chunk, hashKnown ? &hash : NULL,
                inView ? (drawable ? NEED_STALE : NEED_HOLE)
                       : (drawable ? NEED_AHEAD_STALE : NEED_AHEAD),
                playerX, playerZ);
    if (!inView)
        return;
    if (drawable)
        Draw(chunk, site->baseX, site->baseY);
    else
        ++*missing;
}

/* Most belt squares lie wholly inside one map and are rejected here, without
 * asking which map owns each of their 64 tiles. */
static bool InsideOneMap(const ChunkSite *site)
{
    for (unsigned i = 0; i < VoxelWorld_InstanceCount(); ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);

        if (site->x0 >= inst->originX && site->x1 <= inst->originX + inst->width
         && site->y0 >= inst->originY && site->y1 <= inst->originY + inst->height)
            return true;
    }
    return false;
}

/*
 * The walk of the view's squares, kept from one frame to the next while
 * nothing it reads has moved. What it decides - the draws, the requests, the
 * holes - is a function of the view (the frustum, its rectangle and the
 * ring's), the instances, the epoch and the belt's grid, the atlas each map
 * was given and the atlases' state, and the chunks' state; and of nothing
 * else but the frame's number, which it stamps on the chunks it passes and on
 * the requests' records (RequestWait), and the player's place, which only
 * orders the requests (their keys). All of those the same as when the last
 * walk ended, a frame after it, the walk would make the same draws and the
 * same requests in the same order: they are put back as it made them, its
 * stamps and records written as it writes them, the keys measured from where
 * the player now is. A walk counted as 0.5-2.6 ms of an Old 3DS's frame
 * (port.log, visit=, a line only frames with holes write).
 *
 * Kept only from a walk with the camera at rest - the frustum as the frame
 * before had it, so a frame of a moving view never pays to keep one - that
 * left no chunk unverified (no signature left that frame, VOXEL_HASHES_PER_
 * FRAME) and did not fill the request list. Compared exactly, field by field:
 * a chunk built, drafted, dropped or verified since, an atlas made or
 * recycled, a new view, and the walk is made afresh.
 *
 * The comparison has to cost less than the walk it saves, and a walk with the
 * camera still is cheap: that visit= has signatures, atlas work and requests
 * in it. The view's whole update (mesh=) was 0.02 ms in a room of one square,
 * 0.14 ms outside with twenty on screen (Azahar, T01, 2026-10-31). Every one
 * of the 128 slots made into a key and compared by memcmp was some 450
 * instructions a slot, 0.2 ms a frame: more than either walk, and the room's
 * update took 0.22 ms. So only the chunks the walk found (VisitSite stamps
 * them) are compared, field by field, some 130 instructions each: another
 * could be found by the next walk only through the slot index, which counts
 * its changes (sChunkIndexChanges). And a walk of fewer squares than
 * VOXEL_VISIT_KEEP_SITES is neither keyed nor kept: the key, worked out and
 * compared, is near a thousand instructions more, and a square some 300 to
 * walk against some 150 to check and put back - about even at seven.
 */
#define VOXEL_VISIT_KEEP_SITES 8u

typedef struct
{
    VoxelPlane frustum[4];
    int frustumValid;
    int view[4], ring[4];
    int beltOriginX, beltOriginY;
    uint32_t epoch, beltGrid, instanceBuilds;
    unsigned instances;
    VoxelAtlasSlot *atlases[MAX_VOXEL_MAP_INSTANCES], *belt;
    uint8_t inRing[MAX_VOXEL_MAP_INSTANCES];
} VisitKey;

/* What of a chunk the walk reads (VisitSite, SiteVisible, FindChunk). */
typedef struct
{
    uint8_t used, border, openSame, uncovered, draft, audit;
    int mapGroup, mapNum, cx, cy;
    uint32_t beltGrid, openTiles, openEpoch, hash, epoch, staleHash, staleEpoch;
    const void *primary, *secondary, *layout;
    const VoxelAtlasSlot *atlas;
    uint32_t atlasGeneration, atlasExtension;
    unsigned count;
    int gx0, gz0, gx1, gz1;
    float gy0, gy1;
} VisitChunkKey;

/* ... and of an atlas. */
typedef struct
{
    uint8_t valid;
    uint32_t generation, extension;
    const void *primaryTileset, *secondaryTileset;
} VisitAtlasKey;

static struct
{
    bool valid;
    uint32_t frame;
    VisitKey key;
    /* The chunks the walk found, by slot, as it left them; whether on screen. */
    unsigned foundCount;
    uint8_t found[VOXEL_CHUNK_SLOTS], viewed[VOXEL_CHUNK_SLOTS];
    VisitChunkKey chunks[VOXEL_CHUNK_SLOTS];
    uint32_t indexChanges;
    VisitAtlasKey atlases[VOXEL_ATLAS_SLOTS];
    unsigned missing, requestCount, drawCount;
    BuildRequest requests[VOXEL_REQUESTS_MAX];
    VoxelChunkDraw draws[VOXEL_DRAW_MAX];
} sVisitMemo;
/* The view of the last walk, kept or not: whether the camera has stood still. */
static VisitKey sVisitLastKey;
static uint32_t sVisitLastFrame;
/* The squares of the last walk made: whether to key the next. */
static unsigned sVisitLastSites;
/* Walks put back, made and keyed, made too small to key; and the time each
 * kind took (UpdateView), for the log. */
enum
{
    VISIT_SMALL,
    VISIT_MADE,
    VISIT_KEPT,
};
static unsigned sVisitsKept, sVisitsMade, sVisitsSmall;
static uint64_t sVisitTicks[VISIT_KEPT + 1];

/* (no padding to clear: compared field by field, VisitChunkSame) */
static void VisitChunkKeyOf(VisitChunkKey *key, const VoxelChunk *c)
{
    key->used = c->used;
    key->border = c->border;
    key->openSame = c->openSame;
    key->uncovered = c->uncovered;
    key->draft = c->draft;
    key->audit = c->audit;
    key->mapGroup = c->mapGroup;
    key->mapNum = c->mapNum;
    key->cx = c->cx;
    key->cy = c->cy;
    key->beltGrid = c->beltGrid;
    key->openTiles = c->openTiles;
    key->openEpoch = c->openEpoch;
    key->hash = c->hash;
    key->epoch = c->epoch;
    key->staleHash = c->staleHash;
    key->staleEpoch = c->staleEpoch;
    key->primary = c->primary;
    key->secondary = c->secondary;
    key->layout = c->layout;
    key->atlas = c->atlas;
    key->atlasGeneration = c->atlasGeneration;
    key->atlasExtension = c->atlasExtension;
    key->count = c->count;
    key->gx0 = c->gx0;
    key->gz0 = c->gz0;
    key->gx1 = c->gx1;
    key->gz1 = c->gz1;
    key->gy0 = c->gy0;
    key->gy1 = c->gy1;
}

/* Every field VisitChunkKeyOf keeps, as it kept it? */
static bool VisitChunkSame(const VisitChunkKey *k, const VoxelChunk *c)
{
    return k->used == c->used && k->border == c->border && k->openSame == c->openSame
        && k->uncovered == c->uncovered && k->draft == c->draft && k->audit == c->audit
        && k->mapGroup == c->mapGroup && k->mapNum == c->mapNum && k->cx == c->cx && k->cy == c->cy
        && k->beltGrid == c->beltGrid && k->openTiles == c->openTiles && k->openEpoch == c->openEpoch
        && k->hash == c->hash && k->epoch == c->epoch
        && k->staleHash == c->staleHash && k->staleEpoch == c->staleEpoch
        && k->primary == c->primary && k->secondary == c->secondary && k->layout == c->layout
        && k->atlas == c->atlas && k->atlasGeneration == c->atlasGeneration
        && k->atlasExtension == c->atlasExtension && k->count == c->count
        && k->gx0 == c->gx0 && k->gz0 == c->gz0 && k->gx1 == c->gx1 && k->gz1 == c->gz1
        && k->gy0 == c->gy0 && k->gy1 == c->gy1;
}

static void VisitAtlasKeyOf(VisitAtlasKey *key, const VoxelAtlasSlot *a)
{
    key->valid = a->valid;
    key->generation = a->generation;
    key->extension = a->extension;
    key->primaryTileset = a->primaryTileset;
    key->secondaryTileset = a->secondaryTileset;
}

static bool VisitAtlasSame(const VisitAtlasKey *k, const VoxelAtlasSlot *a)
{
    return k->valid == a->valid && k->generation == a->generation && k->extension == a->extension
        && k->primaryTileset == a->primaryTileset && k->secondaryTileset == a->secondaryTileset;
}

/* The chunks and the atlases as the kept walk left them: the same slots in
 * the index, the chunks it found as they were, every atlas as it was. */
static bool VisitStateSame(void)
{
    if (sVisitMemo.indexChanges != sChunkIndexChanges)
        return false;
    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
        if (!VisitAtlasSame(&sVisitMemo.atlases[i], &sAtlases[i]))
            return false;
    for (unsigned i = 0; i < sVisitMemo.foundCount; ++i)
        if (!VisitChunkSame(&sVisitMemo.chunks[i], &sChunks[sVisitMemo.found[i]]))
            return false;
    return true;
}

/* The kept walk put back for this frame, if it is still the walk this frame
 * would make: true if it was. */
static bool VisitReplay(const VisitKey *key, float playerX, float playerZ, unsigned *missing)
{
    if (!sVisitMemo.valid || sVisitMemo.frame + 1u != sFrame
     || memcmp(key, &sVisitMemo.key, sizeof(*key)) != 0 || !VisitStateSame())
        return false;
    /* The requests, in the walk's order, keyed as Request keys them. */
    sRequestCount = sVisitMemo.requestCount;
    memcpy(sRequests, sVisitMemo.requests, sRequestCount * sizeof(sRequests[0]));
    for (unsigned i = 0; i < sRequestCount; ++i)
    {
        BuildRequest *r = &sRequests[i];
        float dx = (r->site.x0 + r->site.x1) * 0.5f - playerX;
        float dz = (r->site.y0 + r->site.y1) * 0.5f - playerZ;

        r->key = r->need * 100000u + (unsigned)(dx * dx + dz * dz);
        /* (RequestWait finding its record: nothing has taken one since) */
        if (sRequestWait[r->waitIndex].seen + 1u < sFrame)
            sRequestWait[r->waitIndex].progress = sFrame;
        sRequestWait[r->waitIndex].seen = sFrame;
    }
    /* The stamps of what it passed (VisitSite), then its draws (Draw). */
    for (unsigned i = 0; i < sVisitMemo.foundCount; ++i)
    {
        VoxelChunk *c = &sChunks[sVisitMemo.found[i]];

        c->stamp = sFrame;
        if (sVisitMemo.viewed[i])
            c->viewStamp = sFrame;
    }
    for (unsigned i = 0; i < sVisitMemo.drawCount; ++i)
        Draw(&sChunks[sVisitMemo.draws[i].chunk - sChunks], sVisitMemo.draws[i].worldX,
             sVisitMemo.draws[i].worldZ);
    *missing += sVisitMemo.missing;
    sVisitMemo.frame = sVisitLastFrame = sFrame;
    ++sVisitsKept;
    return true;
}

/* The walk just made, kept if it may be (see above). `drawsBefore`: the draw
 * list before it (none, as UpdateView calls it); `missing`: the holes it
 * found. */
static void VisitRemember(const VisitKey *key, unsigned drawsBefore, unsigned missing)
{
    bool still = sVisitLastFrame + 1u == sFrame && memcmp(key, &sVisitLastKey, sizeof(*key)) == 0;

    ++sVisitsMade;
    sVisitLastKey = *key;
    sVisitLastFrame = sFrame;
    sVisitMemo.valid = false;
    if (!still || sHashesLeft == 0 || sRequestCount >= VOXEL_REQUESTS_MAX || drawsBefore != 0)
        return;
    sVisitMemo.foundCount = 0;
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i)
    {
        const VoxelChunk *c = &sChunks[i];

        /* (nothing else stamps a chunk between the update's start and here,
         * and what is on screen it found: a slot that is not is never kept) */
        if (!c->used || (c->stamp != sFrame && c->viewStamp != sFrame))
            continue;
        if (c->stamp != sFrame)
            return;
        sVisitMemo.found[sVisitMemo.foundCount] = (uint8_t)i;
        sVisitMemo.viewed[sVisitMemo.foundCount] = c->viewStamp == sFrame;
        VisitChunkKeyOf(&sVisitMemo.chunks[sVisitMemo.foundCount++], c);
    }
    sVisitMemo.indexChanges = sChunkIndexChanges;
    sVisitMemo.key = *key;
    sVisitMemo.frame = sFrame;
    sVisitMemo.missing = missing;
    sVisitMemo.requestCount = sRequestCount;
    memcpy(sVisitMemo.requests, sRequests, sRequestCount * sizeof(sRequests[0]));
    sVisitMemo.drawCount = sDrawCount;
    memcpy(sVisitMemo.draws, sDraws, sDrawCount * sizeof(sDraws[0]));
    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
        VisitAtlasKeyOf(&sVisitMemo.atlases[i], &sAtlases[i]);
    sVisitMemo.valid = true;
}

/* What the walk reads of the view, the instances and the atlases each map was
 * given (VisitKey); cleared first, as it is compared whole. */
static void VisitKeyOf(VisitKey *key, int px0, int py0, int px1, int py1, unsigned instances,
                       VoxelAtlasSlot *const *atlases, const bool *inRing, VoxelAtlasSlot *belt)
{
    memset(key, 0, sizeof(*key));
    memcpy(key->frustum, sFrustum, sizeof(key->frustum));
    key->frustumValid = sFrustumValid;
    memcpy(key->view, sViewRect, sizeof(key->view));
    key->ring[0] = px0;
    key->ring[1] = py0;
    key->ring[2] = px1;
    key->ring[3] = py1;
    key->beltOriginX = sBeltOriginX;
    key->beltOriginY = sBeltOriginY;
    key->epoch = sEpoch;
    key->beltGrid = sBeltGrid;
    key->instanceBuilds = VoxelWorld_InstanceBuilds();
    key->instances = instances;
    for (unsigned i = 0; i < instances; ++i)
    {
        key->atlases[i] = atlases[i];
        key->inRing[i] = inRing[i];
    }
    key->belt = belt;
}

static float VisitMeanMs(unsigned kind, unsigned walks)
{
    return walks != 0 ? TicksMs(sVisitTicks[kind]) / (float)walks : 0.0f;
}

/*
 * Walks every chunk of every map, and of the belt, that reaches into the
 * ring. VISIT_KEPT if the last frame's walk was put back instead (see
 * above); VISIT_SMALL if the walk made was too small to key.
 */
static unsigned VisitView(float playerX, float playerZ, unsigned *missing, bool atlasAhead)
{
    int vx0 = sViewRect[0], vy0 = sViewRect[1], vx1 = sViewRect[2], vy1 = sViewRect[3];
    /* A chunk all round, and as far again as the player is heading. */
    int px0 = vx0 - VOXEL_PREFETCH + (sLeadX < 0.0f ? (int)sLeadX : 0);
    int py0 = vy0 - VOXEL_PREFETCH + (sLeadZ < 0.0f ? (int)sLeadZ : 0);
    int px1 = vx1 + VOXEL_PREFETCH + (sLeadX > 0.0f ? (int)sLeadX : 0);
    int py1 = vy1 + VOXEL_PREFETCH + (sLeadZ > 0.0f ? (int)sLeadZ : 0);
    unsigned instances = VoxelWorld_InstanceCount();
    const VoxelMapInstance *current = VoxelWorld_Instance(0);
    VoxelAtlasSlot *atlases[MAX_VOXEL_MAP_INSTANCES], *belt;
    bool inRing[MAX_VOXEL_MAP_INSTANCES], keyed;
    VisitKey key;
    unsigned missingBefore = *missing, drawsBefore = sDrawCount, sites = 0;

    if (instances > MAX_VOXEL_MAP_INSTANCES)
        instances = MAX_VOXEL_MAP_INSTANCES;
    /*
     * Atlases first, the maps on screen before the ones only in the ring: an
     * on-screen map must never find its slot, or the frame's one atlas
     * upload, taken by a map nobody can see yet. A ring map gets its atlas
     * ahead of time only into an empty slot, and only when the frame can
     * afford the build.
     */
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < instances; ++i)
        {
            const VoxelMapInstance *inst = VoxelWorld_Instance(i);
            /* (its rows where they are drawn, as far south as its base is
             * high: VoxelBuilder_StretchZ) */
            int drawn = DrawnRows(inst);
            int ex = inst->originX + inst->width, ey = inst->originY + inst->height + drawn;
            bool inView = Overlaps(inst->originX, inst->originY + drawn, ex, ey, vx0, vy0, vx1, vy1);

            if (pass == 0)
            {
                inRing[i] = Overlaps(inst->originX, inst->originY + drawn, ex, ey, px0, py0, px1, py1);
                atlases[i] = inView ? AcquireAtlas(inst, true) : NULL;
            }
            else if (inRing[i] && !inView)
            {
                atlases[i] = FindAtlas(inst);
                if (atlases[i] != NULL)
                    atlases[i]->stamp = ++sAtlasStamp; /* not recycled this update */
                else if (atlasAhead)
                    atlases[i] = AcquireAtlas(inst, false);
            }
        }

    belt = current != NULL && !current->indoor ? FindAtlas(current) : NULL;
    /* Each kind's mean as UpdateView times the call, the atlas pass above
     * included: a walk kept should cost a fraction of one made. */
    if (sVisitsKept + sVisitsMade + sVisitsSmall >= 1200u)
    {
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL view walks: %u of %u kept from the frame before "
                     "(%.3fms each), %u made (%.3fms), %u under %u squares made unkeyed (%.3fms)",
                     sVisitsKept, sVisitsKept + sVisitsMade + sVisitsSmall,
                     VisitMeanMs(VISIT_KEPT, sVisitsKept), sVisitsMade,
                     VisitMeanMs(VISIT_MADE, sVisitsMade), sVisitsSmall, VOXEL_VISIT_KEEP_SITES,
                     VisitMeanMs(VISIT_SMALL, sVisitsSmall));
        sVisitsKept = sVisitsMade = sVisitsSmall = 0;
        memset(sVisitTicks, 0, sizeof(sVisitTicks));
    }
    /* The walk as the last frame made it, if nothing it reads has moved - and
     * the last one made was large enough to be keyed at all. */
    keyed = sVisitLastSites >= VOXEL_VISIT_KEEP_SITES;
    if (keyed)
    {
        VisitKeyOf(&key, px0, py0, px1, py1, instances, atlases, inRing, belt);
        if (drawsBefore == 0 && VisitReplay(&key, playerX, playerZ, missing))
            return VISIT_KEPT;
    }

    for (unsigned i = 0; i < instances; ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);
        VoxelAtlasSlot *atlas = atlases[i];
        int ix0, iy0, ix1, iy1;

        if (!inRing[i])
            continue;
        /* the ring in its rows (DrawnRows) */
        ix0 = px0 > inst->originX ? px0 : inst->originX;
        iy0 = py0 - DrawnRows(inst) > inst->originY ? py0 - DrawnRows(inst) : inst->originY;
        ix1 = px1 < inst->originX + inst->width ? px1 : inst->originX + inst->width;
        iy1 = py1 - DrawnRows(inst) < inst->originY + inst->height ? py1 - DrawnRows(inst)
                                                                 : inst->originY + inst->height;
        for (int cy = (iy0 - inst->originY) / VOXEL_CHUNK;
             cy <= (iy1 - 1 - inst->originY) / VOXEL_CHUNK; ++cy)
            for (int cx = (ix0 - inst->originX) / VOXEL_CHUNK;
                 cx <= (ix1 - 1 - inst->originX) / VOXEL_CHUNK; ++cx)
            {
                ChunkSite site;

                ++sites;
                SiteOf(&site, inst, false, cx, cy);
                VisitSite(&site, atlas,
                          playerX, playerZ, missing);
            }
    }

    /* The belt, on the current map's grid: only squares some tile of which
     * belongs to no map at all. Indoors there is none; the clear is black. */
    if (current != NULL && !current->indoor)
    {
        VoxelAtlasSlot *atlas = belt;
        /* A belt laid as drawn stands its squares' blocks rows south of them
         * (VoxelMesh_EmitBorder): the squares that far north reach the ring. */
        int oy0 = py0 - (int)VoxelRelief_Belt(current) - DrawnRows(current) - sBeltOriginY;
        int oy1 = py1 - 1 - DrawnRows(current) - sBeltOriginY;
        int ox0 = px0 - sBeltOriginX, ox1 = px1 - 1 - sBeltOriginX;
        int cy0 = (int)floorf(oy0 / (float)VOXEL_CHUNK), cy1 = (int)floorf(oy1 / (float)VOXEL_CHUNK);
        int cx0 = (int)floorf(ox0 / (float)VOXEL_CHUNK), cx1 = (int)floorf(ox1 / (float)VOXEL_CHUNK);

        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
            {
                ChunkSite site;
                bool open = false;

                ++sites; /* (asked after, visited or not) */
                SiteOf(&site, current, true, cx, cy);
                if (InsideOneMap(&site))
                    continue;
                for (int y = site.y0; y < site.y1 && !open; ++y)
                    for (int x = site.x0; x < site.x1 && !open; ++x)
                        open = VoxelWorld_GetInstanceAt(x, y) == NULL;
                if (!open)
                    continue;
                VisitSite(&site, atlas,
                          playerX, playerZ, missing);
            }
    }
    sVisitLastSites = sites;
    if (sites < VOXEL_VISIT_KEEP_SITES)
    {
        /* (and nothing kept to compare with while the walks stay this small) */
        sVisitMemo.valid = false;
        ++sVisitsSmall;
        return VISIT_SMALL;
    }
    if (!keyed)
        VisitKeyOf(&key, px0, py0, px1, py1, instances, atlases, inRing, belt);
    VisitRemember(&key, drawsBefore, *missing - missingBefore);
    return VISIT_MADE;
}

static int CompareRequests(const void *a, const void *b)
{
    const BuildRequest *ra = a, *rb = b;
    /* A drawable stale mesh must not outrank a missing visible chunk merely
     * because it waited four frames. Age resolves fairness within a class. */
    if (ra->need != rb->need) return ra->need < rb->need ? -1 : 1;
    bool oa = RequestOverdue(ra), ob = RequestOverdue(rb);
    if (oa != ob) return oa ? -1 : 1;
    if (oa)
    {
        uint32_t aa = sFrame - sRequestWait[ra->waitIndex].progress;
        uint32_t ab = sFrame - sRequestWait[rb->waitIndex].progress;
        if (aa != ab) return aa > ab ? -1 : 1;
    }
    unsigned ka = ra->key, kb = rb->key;

    return ka < kb ? -1 : ka > kb;
}

/*
 * When to build: the one decision that sets the frame rate.
 *
 * A frame is FrameBegin on a VBlank, this module's Update and the draws, then
 * FrameEnd - which is when the command list reaches the GPU - and then the
 * game's next frame and the audio, while the GPU draws. A town takes the GPU
 * 6-8 ms on an Old 3DS, and it cannot start before the world's draws are
 * recorded (3ds_video.c starts it there, GpuStartEarly; before 2026-10-07 it
 * waited for FrameEnd): every millisecond built inside the frame, before the
 * draws, pushed the picture that much closer to missing the next VBlank. On hardware a frame that built 8-10 ms of chunks was a dropped
 * frame, however much of it the CPU still had - it was waiting for the GPU.
 *
 * So the building runs after FrameEnd (CtrVoxel_AfterSubmit), in the time the
 * CPU used to spend waiting for the VBlank while the GPU drew, against what
 * is left of the frame once the game and the audio have had their share.
 * Inside the frame there is only what cannot move out of it: the uploads of
 * what was finished (a GPU copy is queued in an open frame), a little after a
 * crossing, and - behind the fade after a cut - the warm-up.
 */
#define VOXEL_FRAME_MS (1000.0f / 60.0f)
/* Inside the frame, for the ring while warming up only (see SpareMs). */
#define VOXEL_WORK_TARGET_MS 13.0f
#define VOXEL_AHEAD_MAX_MS 8.0f
#define VOXEL_AHEAD_MARGIN_MS 1.5f
/*
 * After FrameEnd: the most one frame builds, and what is left untouched before
 * the VBlank for whatever the estimate of the game and the audio misses (the
 * FrameBegin that returns on it, a late event). 1.8 ms left an Old 3DS
 * coming back at 16.8-18 ms in most of its missed VBlanks while biking
 * (port.log 2026-10-28: ready p95 14.7, p99 15.3).
 */
#define VOXEL_AFTER_MAX_MS 9.0f
#define VOXEL_AFTER_MARGIN_MS 2.8f
/* The game's and the audio's share counts at most this much in the estimate:
 * a frame whose game ran longer (a map load) was lost anyway, and a peak held
 * at that height would starve the builds for a second after it. */
#define VOXEL_OTHERS_CAP_MS 9.0f
/*
 * The game frame that crosses into the next map loads it - its data, its
 * tilesets, its people - and on an Old 3DS that is about 7 ms more than any
 * other. Building after the FrameEnd before it, as much as the estimate of an
 * ordinary frame allowed, the CPU came back to the VBlank late: a dropped
 * frame on every crossing. While the player heads for an edge of the map
 * that close (VOXEL_CROSSING_NEAR tiles), the builds leave that much free.
 *
 * Only the last step: the frame that loads the neighbour is lost to the game
 * itself (the load and the audio alone fill it), and held back over the whole
 * approach - two and a half tiles, with the reserve at 8 ms - the builds
 * stopped for nearly a second just before every crossing, which is exactly
 * when the map beyond it is the one that has to be built.
 */
#define VOXEL_CROSSING_RESERVE_MS 5.0f
#define VOXEL_CROSSING_NEAR 0.5f
static bool sCrossingSoon;
/*
 * Inside any other frame, for a hole on screen (see sHolesOnly): what the
 * last frame left between the end of its GPU work and the VBlank - its
 * present without this module's builds, plus the GPU's drawing, against
 * VOXEL_INFRAME_TARGET_MS - and never more than VOXEL_INFRAME_MAX_MS.
 */
#define VOXEL_INFRAME_TARGET_MS 14.0f
#define VOXEL_INFRAME_MAX_MS 4.0f
/* What this module built inside the last frame (not the visit). */
static float sInFrameBuildMs;
#define VOXEL_AHEAD_BACKOFF_FRAMES 30u

static uint32_t sAheadBackoff;

/*
 * After a cut - a warp, a door, a loaded game - the new map is built in full
 * before anything else, whatever it costs: the frames go by behind the fade
 * anyway. Spread over frames like any other work, the first frames of the
 * new map had nothing to draw, and the video fell back to the 2D compositor
 * until they had - the plain GBA picture flashing up before the 3D one.
 * Ends once the view has no hole left, or after a few seconds regardless.
 */
#define VOXEL_WARMUP_MS 4.0f
#define VOXEL_WARMUP_FRAMES 180u
static uint32_t sWarmupUntil;
/* Set when atlases were given back to the 2D compositor: the overworld warms
 * up again when it returns, as after a cut. */
static bool sResumeWarmup;
/* The view Update visited this frame, which is what AfterSubmit works from. */
static bool sViewReady;

static float Clamp(float value, float lo, float hi)
{
    return value < lo ? lo : value > hi ? hi : value;
}

static float InFrameBudget(void)
{
    const CtrVideoStats *video = CtrVideo_GetStats();
    /* What comes before the GPU starts: the whole present when it waited for
     * FrameEnd, only up to the world's draws now that it starts there. */
    float fixed = video->gpuStartMs - sInFrameBuildMs;

    /* The first frame back from a 2D screen (a battle's end) has only that
     * screen's figures, which say nothing of this one's: no time inside it. */
    if (!video->voxelFrame)
        return 0.0f;
    if (fixed < 0.0f)
        fixed = 0.0f;
    return Clamp(VOXEL_INFRAME_TARGET_MS - fixed - video->gpuMs, 0.0f, VOXEL_INFRAME_MAX_MS);
}

static unsigned LightRays(void)
{
#if CTR_VOXEL_LIGHTING
    return VoxelLighting_Rays();
#else
    return 0;
#endif
}

static unsigned LightSteps(void)
{
#if CTR_VOXEL_LIGHTING
    return VoxelLighting_Steps();
#else
    return 0;
#endif
}

static unsigned LightSurfaceTests(void)
{
#if CTR_VOXEL_LIGHTING
    return VoxelLighting_SurfaceTests();
#else
    return 0;
#endif
}

static void NoteBuildCost(VoxelChunk *chunk, const ChunkSite *site, float ms, uint32_t frames)
{
    static unsigned sSlowLogged, sBuilds, sRaysAll, sStepsAll, sSurfacesAll;
    static float sMsAll;

    chunk->buildMs = ms;
    ++sKeyCounts.builds;
    sKeyCounts.buildMs += ms;
    /* Running totals, to compare two builds over one recorded walk. */
    ++sBuilds;
    sMsAll += ms;
    sRaysAll += sJob.rays;
    sStepsAll += sJob.steps;
    sSurfacesAll += sJob.surfaces;
    if (sBuilds % 32 == 0)
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL builds %u: %.1f ms, %u rays %u steps %u surface",
                     sBuilds, sMsAll, sRaysAll, sStepsAll, sSurfacesAll);
    /* Where the time goes on hardware, which an emulator cannot say. */
    if (ms >= 8.0f && sSlowLogged < 64)
    {
        ++sSlowLogged;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL slow chunk %d,%d of %d:%d%s: %.1f ms over %lu frames, "
                     "%u verts, %u rays %u steps %u surface (ground %.1f trees %.1f models %.1f pack+upload %.1f)",
                     site->cx, site->cy, site->inst->mapGroup, site->inst->mapNum,
                     site->border ? " border" : "", ms, (unsigned long)frames, chunk->count,
                     sJob.rays, sJob.steps, sJob.surfaces,
                     TicksMs(sJob.phaseTicks[JOB_GROUND] + sJob.phaseTicks[JOB_BORDER]),
                     TicksMs(sJob.phaseTicks[JOB_TREES] + sJob.phaseTicks[JOB_BORDER_TREES]),
                     TicksMs(sJob.phaseTicks[JOB_MODELS] + sJob.phaseTicks[JOB_STATICS]),
                     TicksMs(sJob.phaseTicks[JOB_PACK] + sJob.phaseTicks[JOB_DONE]));
    }
}

static float SpareMs(void)
{
    const CtrTiming *timing = CtrPlatform_GetTiming();
    float fixed = timing->workMs - sLastBuildMs;

    if (fixed < 0.0f)
        fixed = 0.0f;
    return VOXEL_WORK_TARGET_MS - fixed;
}

/*
 * Estimate each phase independently and admit slices within the remaining
 * budget. Recent expensive samples take effect immediately; idle estimates
 * decay with frame age, allowing prefetch to recover without a visible rescue.
 */
static float sPhaseMs[JOB_DONE + 1];
static uint32_t sPhaseFrame[JOB_DONE + 1];

/* An idle phase must age without executing: otherwise a 5 ms outlier
 * permanently excludes prefetch from the usual 1-4 ms post-submit window.
 * Retain recent measurements; discount old ones before the next sample. */
static float PhaseCost(int phase)
{
    uint32_t age = sFrame - sPhaseFrame[phase];
    float cost = sPhaseMs[phase];
    if (age > VOXEL_STARVE_FRAMES)
        cost *= (float)VOXEL_STARVE_FRAMES / (float)age;
    return cost;
}

/*
 * Inside the frame a slice also delays the GPU's start: there the estimate
 * never falls below half the last measure. Aged as above, a 3-5 ms ground
 * slice was let into a 1 ms window in the frame as if it cost nothing; after
 * FrameEnd the aging stays, so prefetch recovers from an outlier.
 */
static float PhaseCostInFrame(int phase)
{
    float cost = PhaseCost(phase), floor = sPhaseMs[phase] * 0.5f;

    return cost > floor ? cost : floor;
}

/*
 * The last few slices of each phase as measured, whole: the estimate above
 * holds none over VOXEL_SLICE_CAP_MS, and ages towards nothing while its
 * phase is idle - which is what lets prefetch past an outlier, but a view
 * job's slice then went into what little was left of the window as if it
 * cost nothing: 2-5.6 ms slices let in with 0.1-0.6 ms to go, the window
 * overrun 10.0 against 6.4 (port.log 2026-10-28, "slice border-trees ...
 * 5.57ms remaining=0.61"). A view job's slice that follows other work of the
 * same call is never counted at less than the largest of these (JobSliceCost).
 */
#define VOXEL_PHASE_SAMPLES 4u
static float sPhaseRecent[JOB_DONE + 1][VOXEL_PHASE_SAMPLES];
static unsigned sPhaseRecentNext[JOB_DONE + 1];

static void NotePhaseCost(int phase, float ms)
{
    float *estimate = &sPhaseMs[phase];
    float previous = PhaseCost(phase);

    sPhaseRecent[phase][sPhaseRecentNext[phase]] = ms;
    sPhaseRecentNext[phase] = (sPhaseRecentNext[phase] + 1u) % VOXEL_PHASE_SAMPLES;
    if (ms > VOXEL_SLICE_CAP_MS)
        ms = VOXEL_SLICE_CAP_MS;
    *estimate = ms > previous ? ms : previous * 0.75f + ms * 0.25f;
    sPhaseFrame[phase] = sFrame;
}

/*
 * The floor's own bound: a slice above it is a card read or the like, not
 * the phase's cost.
 */
#define VOXEL_VIEW_FLOOR_MAX_MS 6.0f

/*
 * What the running job's next slice is counted at: the phase's estimate,
 * aged (PhaseCost), or inside the frame never under half its last value
 * (PhaseCostInFrame) - and for a view job, once this call has run a slice or
 * uploaded a chunk (`afterWork`), never under the largest of the phase's last
 * few slices either: that is the slice the aged estimate let into what was
 * left of the window (a new phase's first, its estimate aged since the last
 * job). The call's first slice keeps the estimate alone, as all did: a floor
 * that does not fit then fits no better later in the same window, and one
 * larger than the whole window - 2-5.6 ms ground slices with lighting
 * against the ~2.4 ms an Old 3DS leaves after the frame - would hold the job,
 * and every job behind it, since only one runs, for a window that may never
 * come. Every call thus still offers a job its next slice on the old terms;
 * only what it would chain after that is held to the floor. Ahead of time the
 * aged estimate alone, so prefetch recovers from an outlier; behind the
 * warm-up's fade the frames are the build's.
 */
static float JobSliceCost(int phase, bool inFrame, bool afterWork)
{
    float cost = inFrame ? PhaseCostInFrame(phase) : PhaseCost(phase), floor = 0.0f;

    if (!sJob.forView || !afterWork || sFrame < sWarmupUntil)
        return cost;
    for (unsigned i = 0; i < VOXEL_PHASE_SAMPLES; ++i)
        if (sPhaseRecent[phase][i] > floor)
            floor = sPhaseRecent[phase][i];
    if (floor > VOXEL_VIEW_FLOOR_MAX_MS)
        floor = VOXEL_VIEW_FLOOR_MAX_MS;
    return cost > floor ? cost : floor;
}

/*
 * Starting a job - its square's signature if the visit did not work it out,
 * and JobStart opening the builder's window - is work like a slice, let in
 * only if it fits as well: it was let in on the time alone. Measured, risen
 * to at once and eased down, and kept apart for the starts that work the
 * signature out (the slow part: it reads layouts through the asset cache)
 * and those whose visit had it: one is no measure of the other. Stored no
 * higher than VOXEL_START_COST_MAX_MS, as a slice's floor is bounded, and
 * aged as a phase's estimate is (PhaseCost). It changes only when a start is
 * made: unaged, one slow start shut every window shorter than it to every
 * start after it - after FrameEnd none is forced - and with no start made
 * nothing measured it down again, so the starts in such windows stalled for
 * good where they had gone ahead on the time alone. Aged, they go ahead
 * again a few frames on. Aged by the frames the job slot has stood free
 * since (StartNextJob asked with none made), not since the last start: the
 * frames of the job that start opened refused nothing.
 */
#define VOXEL_START_COST_MAX_MS 1.5f
static float sStartMs[2];          /* [hashKnown] */
static uint32_t sStartFrame[2];    /* the frame of the last start of each */
static bool sSlotFreeSeen;         /* StartNextJob asked since the last start, */
static uint32_t sSlotFreeFrame;    /* first in this frame */

static float StartCost(bool hashKnown)
{
    uint32_t since = sSlotFreeSeen && sSlotFreeFrame > sStartFrame[hashKnown] ? sSlotFreeFrame
                   : sStartFrame[hashKnown];
    uint32_t age = sFrame - since;
    float cost = sStartMs[hashKnown];

    if (age > VOXEL_STARVE_FRAMES)
        cost *= (float)VOXEL_STARVE_FRAMES / (float)age;
    return cost;
}

static void NoteStartCost(bool hashKnown, float ms)
{
    float previous = StartCost(hashKnown);

    if (ms > VOXEL_START_COST_MAX_MS)
        ms = VOXEL_START_COST_MAX_MS;
    sStartMs[hashKnown] = ms > previous ? ms : previous * 0.75f + ms * 0.25f;
    sStartFrame[hashKnown] = sFrame;
}

static bool SameSite(const ChunkSite *a, const ChunkSite *b)
{
    return a->border == b->border && a->cx == b->cx && a->cy == b->cy
        && a->inst->mapGroup == b->inst->mapGroup && a->inst->mapNum == b->inst->mapNum;
}

static bool IsDrawn(const VoxelChunk *chunk)
{
    for (unsigned i = 0; i < sDrawCount; ++i)
        if (sDraws[i].chunk == chunk)
            return true;
    return false;
}

/*
 * Inside a frame that is neither warming up nor crossing, only a hole on
 * screen may be built, and only in the time the GPU left free (InFrameBudget):
 * a square missing from the picture is worth a millisecond of the GPU's
 * frame; a stale one, drawn as it was, or one ahead of time is not.
 */
static bool sHolesOnly;

/*
 * A build ahead of time whose mesh finds no room even with all the ring may
 * give up gone (ChunkVram) is thrown away, and the ring waited
 * VOXEL_AHEAD_BACKOFF_FRAMES only to build the next and throw it away too,
 * for as long as the view held the arena full: in a battle over the world a
 * whole chunk built after FrameEnd every 31 frames, 4.9-6.8 ms each, builds=
 * and pending= the same across some 135 of them (Azahar, 2026-10-31). So the
 * smallest mesh refused since the last that found room is kept
 * (sAheadNoRoomBytes, AheadNoRoom), and while the arena could not make room
 * for one that large either - the very question ChunkVram asks ahead of
 * time, ChunkRoomPossible - no build ahead of time starts, but a chunk's
 * rebuilt in the block it has. The backoff is kept, so it is asked at most
 * once in each, and the answer once a frame. Any block given back
 * (ChunkBlockFree), a mesh ahead that found room, a cut forget the size, and
 * the ring builds as before. A new mesh smaller than the size kept waits as
 * well: ahead of time only, the view building what it comes to need.
 */
static unsigned sAheadRoomBytes; /* the size the answer below is for, */
static uint32_t sAheadRoomFrame; /* the frame it was asked in, */
static bool sAheadRoomNone;      /* and whether no room could be made */
/* In all: builds ahead of time thrown away for want of room (AheadNoRoom),
 * and starts ahead of time skipped for it (StartNextJob). */
static unsigned sAheadLost, sAheadSkipped;

static bool AheadRoomNone(void)
{
    if (sAheadNoRoomBytes == 0)
        return false;
    if (sAheadRoomFrame != sFrame || sAheadRoomBytes != sAheadNoRoomBytes)
    {
        sAheadRoomFrame = sFrame;
        sAheadRoomBytes = sAheadNoRoomBytes;
        sAheadRoomNone = !ChunkRoomPossible(sAheadNoRoomBytes, false);
    }
    return sAheadRoomNone;
}

/* Takes the most urgent request the frame can afford as the next job: its
 * start as well as its time so far inside the budget, unless `forced` (the
 * slice a hole is owed, RunJobs). */
static bool StartNextJob(float elapsed, float holeMs, float aheadMs, bool forced)
{
    bool roomless = false;

    if (!sSlotFreeSeen)
    {
        sSlotFreeSeen = true;
        sSlotFreeFrame = sFrame;
    }
    for (unsigned i = 0; i < sRequestCount; ++i)
    {
        BuildRequest *r = &sRequests[i];
        const VoxelMapInstance *inst = r->site.inst;
        unsigned need = r->need;
        bool forView = need == NEED_HOLE || need == NEED_STALE;
        bool hashKnown = r->hashKnown;
        float limit = forView ? holeMs : aheadMs;
        uint64_t startTick;

        if (r->done || sFrame < sRequestWait[r->waitIndex].noRoomUntil)
            continue;
        /* The wide view: no build while a hole inside the draft ring still
         * waits for its draft (it is the next frame's draft, and a build
         * would take the frames the drafts need and the VRAM a view this
         * size has not got), none past the full ring, none for the prefetch
         * ring: the squares the camera shows when it settles are the ones
         * worth 10-30 ms now. */
        if (sWideView && (sWideHolesLeft != 0 || !forView || SiteReach(&r->site) > VOXEL_BAND_FULL))
            continue;
        /* The Fly cinematic's high camera: the drafts alone until the
         * landing, not even a hole's build (a hole left is the next frame's
         * draft): full builds under it failed for room and left the square
         * black for VOXEL_NO_ROOM_FRAMES (Azahar). */
        if (sWideView && sFly.active && sFly.lift > 0.05f)
            continue;
        if (sHolesOnly && need != NEED_HOLE && !RequestOverdue(r))
            continue; /* sorted: holes come first */
        if (elapsed >= limit || (!forced && elapsed + StartCost(hashKnown) > limit))
            return false; /* sorted: nothing after it is more urgent */
        /* No room ahead of time (see above), but for a chunk rebuilt in its
         * own block: one that still fits there asks the arena for nothing.
         * Not a draft's: its block is the flat draft's, a fraction of what
         * its build takes, and the build was made and thrown away again
         * after every backoff for as long as the arena stayed full. */
        if (!forView && (r->chunk == NULL || r->chunk->vram == NULL || r->chunk->draft)
         && AheadRoomNone())
        {
            roomless = true;
            continue;
        }
        /* The slot may have been recycled for another tileset this update. */
        if (!r->atlas->valid || r->atlas->primaryTileset != inst->primaryTileset
         || r->atlas->secondaryTileset != inst->secondaryTileset)
            continue;
        startTick = svcGetSystemTick();
        if (!hashKnown)
            r->hash = SiteHash(&r->site);
        r->done = true;
        JobStart(&r->site, r->atlas, r->hash, r->chunk, forView, need == NEED_HOLE);
        sJob.waitIndex = r->waitIndex;
        sJob.overdue = RequestOverdue(r);
        NoteStartCost(hashKnown, TicksMs(svcGetSystemTick() - startTick));
        /* Its cold mesh, if the cache holds one with its key - never for an
         * audit, which is the build itself. */
        if (r->chunk == NULL || r->chunk->audit == 0)
        {
            uint64_t coldStart = svcGetSystemTick();

            if (ColdTake(&r->site, r->hash, r->atlas))
                sJob.ticks += svcGetSystemTick() - coldStart;
        }
        sSlotFreeSeen = false;
        return true;
    }
    if (roomless)
    {
        ++sAheadSkipped;
        sAheadBackoff = sFrame + VOXEL_AHEAD_BACKOFF_FRAMES;
    }
    return false;
}

/*
 * A view job finished with no gap for its mesh even once all off screen was
 * gone (ChunkVram). Its square waits, drawn by its draft or its old mesh,
 * rather than being built again every frame and thrown away.
 */
#define VOXEL_NO_ROOM_FRAMES 60u

static void NoRoomForView(void)
{
    static unsigned sNoRoom;
    static uint32_t sNoRoomLogged;

    sRequestWait[sJob.waitIndex].noRoomUntil = sFrame + VOXEL_NO_ROOM_FRAMES;
    if (sNoRoom++ == 0 || sFrame - sNoRoomLogged >= 600u)
    {
        sNoRoomLogged = sFrame;
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no VRAM for chunk %d,%d of %d:%d%s (%u bytes; arena used %lu KiB,"
                     " largest gap %lu KiB; %u times): waits %u frames",
                     sJob.site.cx, sJob.site.cy, sJob.site.inst->mapGroup, sJob.site.inst->mapNum,
                     sJob.site.border ? " border" : "",
                     (unsigned)(sBuilder.count * sizeof(VoxelGpuVertex)),
                     (unsigned long)(sChunkArena.used >> 10),
                     (unsigned long)(VoxelArena_LargestGap(&sChunkArena) >> 10), sNoRoom,
                     VOXEL_NO_ROOM_FRAMES);
    }
}

/*
 * A job ahead of time finished with no gap for its mesh, as above: the ring
 * waits VOXEL_AHEAD_BACKOFF_FRAMES, and builds nothing more ahead of time
 * while no room could be made for a mesh this large (AheadRoomNone).
 * `before` is the size kept when its upload began: a slot given up for this
 * job forgot it (ChunkBlockFree), and the smallest is the one kept.
 */
static void AheadNoRoom(unsigned before)
{
    static uint32_t sLostLogged;
    /* (what JobFinish asked ChunkVram for) */
    unsigned bytes = (unsigned)((sBuilder.count * sizeof(VoxelGpuVertex) + 15u) & ~15u);

    sAheadBackoff = sFrame + VOXEL_AHEAD_BACKOFF_FRAMES;
    if (before != 0 && (sAheadNoRoomBytes == 0 || before < sAheadNoRoomBytes))
        sAheadNoRoomBytes = before;
    if (bytes != 0 && (sAheadNoRoomBytes == 0 || bytes < sAheadNoRoomBytes))
        sAheadNoRoomBytes = bytes;
    if (sAheadLost++ == 0 || sFrame - sLostLogged >= 600u)
    {
        sLostLogged = sFrame;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: no VRAM ahead for chunk %d,%d of %d:%d%s (%u bytes; arena used"
                     " %lu KiB, largest gap %lu KiB): aheadLost=%u aheadSkipped=%u",
                     sJob.site.cx, sJob.site.cy, sJob.site.inst->mapGroup, sJob.site.inst->mapNum,
                     sJob.site.border ? " border" : "", bytes,
                     (unsigned long)(sChunkArena.used >> 10),
                     (unsigned long)(VoxelArena_LargestGap(&sChunkArena) >> 10), sAheadLost, sAheadSkipped);
    }
}

/*
 * Whether the running job's next slice may be its last, leaving it finished:
 * the one that packs the rest with no static model to follow, or any of its
 * static models' (one slice may find all that are left cached).
 */
static bool JobSliceMayFinish(void)
{
    if (sJob.phase == JOB_PACK)
        return sBuilder.staticCount == 0 && sBuilder.count - sJob.packed <= VOXEL_PACK_SLICE;
    return sJob.phase == JOB_STATICS;
}

/*
 * Slices of the running job and of the next ones, most urgent first, until
 * the budgets run out: `holeMs` for what the view is missing or has stale,
 * `aheadMs` for the prefetch ring. A finished job is uploaded only `inFrame`
 * (its GPU copy needs the open frame); after FrameEnd it waits, its geometry
 * in the scratch, for the next Update. The upload - a copy and a flush,
 * milliseconds inside the frame - follows whatever the time, since until it
 * is no other job can be built; so once one is made, the slice that may
 * finish another counts its upload in. With `forceHole` a hole on screen
 * gets one slice however the frame is going. Returns the chunks uploaded:
 * the runtime test counts them (voxel_runtime_test.py).
 */
static unsigned RunJobs(uint64_t started, float holeMs, float aheadMs, bool inFrame,
                        bool forceHole, unsigned *missing)
{
    unsigned built = 0, slices = 0;

    for (;;)
    {
        float elapsed = MsSince(started);
        float budget;
        bool forced = forceHole && slices == 0;

        if (!sJob.active && !StartNextJob(forced ? 0.0f : elapsed, holeMs, aheadMs, forced))
            break;
        budget = sHolesOnly && !sJob.hole && !sJob.overdue ? 0.0f : sJob.forView ? holeMs : aheadMs;
        if (sJob.phase != JOB_DONE)
        {
            int phase = sJob.phase;
            uint64_t sliceStart, ticks;
            unsigned rays, steps, surfaces, misses;
            int cellX = sJob.col, cellY = sJob.row;

            /*
             * A slice starts only if it should also end inside the budget,
             * save for the one a hole may be owed.
             */
            bool owed;

            /* The owed slice too, unless it is far over what a rescue may
             * spend and its square has not waited long enough to need it. */
            float cost = JobSliceCost(phase, inFrame, slices != 0 || built != 0);

            /*
             * Inside the frame, once a chunk is uploaded, the slice that may
             * finish a job only if the upload it leads to fits as well: the
             * upload follows it whatever the time (below), and a job left
             * finished would hold the only job slot through the window after
             * FrameEnd, which cannot upload. Behind the warm-up's fade the
             * frames are the build's, as for the slices.
             */
            if (inFrame && built != 0 && sFrame >= sWarmupUntil && JobSliceMayFinish())
                cost += PhaseCostInFrame(JOB_DONE);
            owed = forceHole && sJob.forView && slices == 0 && budget > 0.0f
                && (elapsed + cost <= budget + VOXEL_STARVE_MS
                 || sFrame - sRequestWait[sJob.waitIndex].progress >= VOXEL_STARVE_FRAMES * 4u);
            if (!owed && (elapsed >= budget || elapsed + cost > budget))
                break;
            sliceStart = svcGetSystemTick();
            rays = LightRays();
            steps = LightSteps();
            surfaces = LightSurfaceTests();
            misses = CtrAssets_GetStats()->misses;
            JobStep();
            if (sJob.forView)
                sRequestWait[sJob.waitIndex].progress = sFrame;
            ticks = svcGetSystemTick() - sliceStart;
            sJob.rays += LightRays() - rays;
            sJob.steps += LightSteps() - steps;
            sJob.surfaces += LightSurfaceTests() - surfaces;
            sJob.ticks += ticks;
            sJob.phaseTicks[phase] += ticks;
            NotePhaseCost(phase, TicksMs(ticks));
            /* Keep diagnosing beyond the initial 64 chunks, including routes
             * reached minutes into play. At most one line every two seconds. */
            static uint32_t lastSlowSlice;
            if (TicksMs(ticks) > 2.0f && sFrame - lastSlowSlice >= 120u)
            {
                static const char *names[] = {"ground", "trees", "models", "border", "border-trees", "sort", "pack", "statics", "upload"};
                lastSlowSlice = sFrame;
                CtrLog_Write(CTR_LOG_VIDEO, "VOXEL slice %s of %d:%d cell=%d,%d: %.2fms remaining=%.2f view=%d rays=%u misses=%u",
                             names[phase], sJob.site.inst->mapGroup, sJob.site.inst->mapNum,
                             cellX, cellY, TicksMs(ticks), budget - elapsed, sJob.forView,
                             LightRays() - rays, (unsigned)(CtrAssets_GetStats()->misses - misses));
            }
            ++slices;
            /* A static model converted with nowhere to go yet (StaticBuildFinish)
             * holds the converter till the next update, and the job with it:
             * its slices would find nothing to do for the rest of the budget. */
            if (sJob.phase == JOB_STATICS && StaticBuildHeld())
                break;
            continue;
        }
        if (!inFrame)
            break;

        /* Finished: upload it into its chunk. */
        {
            VoxelChunk *chunk = sJob.chunk;
            BuildResult result;
            /* (before a slot given up for it forgets it, ChunkBlockFree) */
            unsigned noRoomBefore = sAheadNoRoomBytes;

            /* (and no slot given up for it with no copy left to fill it) */
            if (STAGING_ROOM - sStagingUsed < sBuilder.count
             || sChunkUploads >= VOXEL_CHUNK_UPLOADS_MAX
             || (sBuilder.count != 0 && CtrVideo_VoxelUploadsLeft() == 0))
                break; /* next frame, the geometry stays in the scratch */
            /* An earlier upload may have evicted the chunk this job was
             * rebuilding, and even handed its slot to another square. */
            if (chunk != NULL && !IsChunkOf(chunk, &sJob.site))
                chunk = NULL;
            /* A draft may have been made for the square since the job began. */
            if (chunk == NULL)
                chunk = FindChunk(sJob.site.border, sJob.site.inst->mapGroup,
                                  sJob.site.inst->mapNum, sJob.site.cx, sJob.site.cy);
            if (chunk == NULL)
            {
                chunk = FreeChunkSlot(sJob.forView);
                if (chunk == NULL)
                {
                    sJob.active = false;
                    continue;
                }
                /* JobFinish reserves its copy after this eviction */
                sStagingPromise = sBuilder.count;
                sCopyPromise = sBuilder.count != 0 ? 1u : 0u;
                EvictChunk(chunk);
                sStagingPromise = sCopyPromise = 0;
            }
            {
                uint64_t finishStart = svcGetSystemTick(), ticks;

                result = JobFinish(chunk);
                ticks = svcGetSystemTick() - finishStart;
                if (result == BUILD_OK)
                {
                    sJob.ticks += ticks;
                    sJob.phaseTicks[JOB_DONE] += ticks;
                    NotePhaseCost(JOB_DONE, TicksMs(ticks));
                }
            }
            if (result == BUILD_NO_STAGING)
                break;
            if (result == BUILD_NO_VRAM)
            {
                /* Nothing more fits. Ahead of time, stop trying for a while,
                 * and until room could be made for a mesh this large: the
                 * ring would otherwise build a chunk and discard it for as
                 * long as the view holds the cache full (AheadNoRoom). */
                if (!sJob.forView)
                    AheadNoRoom(noRoomBefore);
                else
                    NoRoomForView();
                sJob.active = false;
                break;
            }
            /* Room found ahead of time: the ring builds as before. */
            if (!sJob.forView)
                sAheadNoRoomBytes = 0;
            sJob.active = false;
            ++built;
            if (sJob.cold)
                sKeyCounts.coldMs += TicksMs(sJob.ticks);
            else
                NoteBuildCost(chunk, &sJob.site, TicksMs(sJob.ticks), sFrame - sJob.firstFrame + 1);
            /* A stale chunk is already on the draw list, which points at its
             * slot; a hole joins it now, if it is on screen. */
            if (!IsDrawn(chunk) && SiteVisible(&sJob.site, chunk))
            {
                Draw(chunk, sJob.site.baseX, sJob.site.baseY);
                if (missing != NULL && *missing > 0)
                    --*missing;
            }
        }
    }
    return built;
}

/*
 * Where the player is heading. The prefetch ring reaches this much further
 * that way, and its squares are built nearest to the point the player will
 * be at in VOXEL_LEAD_FRAMES rather than to where they stand: on a bike the
 * ring one chunk deep was outrun, and chunks were built in its trail as
 * readily as ahead of it.
 */
#define VOXEL_LEAD_FRAMES 90.0f
#define VOXEL_LEAD_MAX 16.0f
static float sVelocityX, sVelocityZ;
/* Where the player stood last frame; moved with the coordinates on a crossing
 * (HandleMapChange), so that the heading carries across it. */
static float sLastX, sLastZ;

static void TrackMotion(float x, float z, bool reset)
{
    if (reset)
        sVelocityX = sVelocityZ = 0.0f;
    else
    {
        float dx = Clamp(x - sLastX, -1.0f, 1.0f), dz = Clamp(z - sLastZ, -1.0f, 1.0f);

        sVelocityX += (dx - sVelocityX) * 0.2f;
        sVelocityZ += (dz - sVelocityZ) * 0.2f;
    }
    sLastX = x;
    sLastZ = z;
    sLeadX = Clamp(sVelocityX * VOXEL_LEAD_FRAMES, -VOXEL_LEAD_MAX, VOXEL_LEAD_MAX);
    sLeadZ = Clamp(sVelocityZ * VOXEL_LEAD_FRAMES, -VOXEL_LEAD_MAX, VOXEL_LEAD_MAX);
}

/*
 * Every hole of the view the frame can afford to draft, nearest first (the
 * requests are sorted): a hole is on screen, so none is left for the real
 * build to find. It becomes an ordinary stale square - drawn as it is, built
 * for real after the frame - and joins this frame's draw list. A few at a
 * time and within VOXEL_DRAFT_MS, but always one.
 */
#define VOXEL_DRAFT_MS 1.2f

static bool DraftHoles(unsigned *missing)
{
    uint64_t start = svcGetSystemTick();
    unsigned made = 0;
    /* The wide view (see there): twice the drafts, inside its draft ring. */
    unsigned perFrame = sWideView ? VOXEL_WIDE_DRAFT_PER_FRAME : VOXEL_DRAFT_PER_FRAME;
    float budgetMs = sWideView ? VOXEL_WIDE_DRAFT_MS : VOXEL_DRAFT_MS;
    bool room = true;

    for (unsigned i = 0; i < sRequestCount && sRequests[i].need == NEED_HOLE; ++i)
    {
        BuildRequest *r = &sRequests[i];
        const VoxelMapInstance *inst = r->site.inst;
        VoxelChunk *chunk;

        if (r->done || r->site.border)
            continue;
        if (sWideView && SiteReach(&r->site) > VOXEL_BAND_DRAFT)
            continue; /* the fill's ring */
        if (!room)
            continue; /* no room this frame (below): filled flat */
        if (made >= perFrame || (made != 0 && MsSince(start) >= budgetMs))
        {
            if (!sWideView)
                break;
            ++sWideHolesLeft; /* the next frame's draft; no build before it */
            continue;
        }
        /* The running job is for this square: let it finish. */
        if (sJob.active && SameSite(&r->site, &sJob.site))
            continue;
        if (!r->atlas->valid || r->atlas->primaryTileset != inst->primaryTileset
         || r->atlas->secondaryTileset != inst->secondaryTileset)
            continue;
        chunk = DraftChunk(&r->site, r->atlas, r->chunk);
        if (chunk == NULL)
        {
            /* No room in the frame's uploads or in VRAM: next frame. Under the
             * wide view the holes left are filled flat, and they do not hold
             * the builds back: VRAM that will not take a draft would hold
             * them for the whole flight. */
            if (!sWideView)
                break;
            room = false;
            continue;
        }
        ++made;
        r->chunk = chunk;
        r->hashKnown = false;
        r->need = NEED_STALE;
        r->key += 100000u;
        if (!IsDrawn(chunk) && SiteVisible(&r->site, chunk))
        {
            Draw(chunk, r->site.baseX, r->site.baseY);
            if (missing != NULL && *missing > 0)
                --*missing;
        }
    }
    return made != 0;
}

/*
 * Every hole left in the view - undrafted this frame, the belt's (which has no
 * draft), the running job's - is filled flat in its ground's colour (see the
 * fill): read once per square from its metatiles, then kept in the wait table
 * while the world and the atlas stay as they were.
 */
static void FillHoles(void)
{
    for (unsigned i = 0; i < sRequestCount; ++i)
    {
        BuildRequest *r = &sRequests[i];
        uint32_t colour;

        if (r->need != NEED_HOLE)
            continue;
        if (r->waitIndex < VOXEL_REQUESTS_MAX)
        {
            if (sRequestWait[r->waitIndex].fill == 0 || sRequestWait[r->waitIndex].fillEpoch != sEpoch
             || sRequestWait[r->waitIndex].fillExtension != r->atlas->extension)
            {
                sRequestWait[r->waitIndex].fill = SquareColour(&r->site, r->atlas);
                sRequestWait[r->waitIndex].fillEpoch = sEpoch;
                sRequestWait[r->waitIndex].fillExtension = r->atlas->extension;
            }
            colour = sRequestWait[r->waitIndex].fill;
        }
        else
            colour = SquareColour(&r->site, r->atlas);
        AddFill(&r->site, colour != 0 ? colour : sMapFill);
    }
}

static void UpdateView(float playerX, float playerZ, bool cut)
{
    uint64_t started = svcGetSystemTick();
    float spare = SpareMs();
    float holeMs = 0.0f, aheadMs = 0.0f, visitMs = 0.0f;
    unsigned missing = 0;
    bool holes = false, starved = false;
    bool warmup;

    if (cut || sResumeWarmup)
        sWarmupUntil = sFrame + VOXEL_WARMUP_FRAMES;
    /* A cut: the scene left behind is stamped no more, its chunks the ring's
     * to give up (Evictable) - room to build ahead into again. */
    if (cut)
        sAheadNoRoomBytes = 0;
    sResumeWarmup = false;
    warmup = sFrame < sWarmupUntil;
    if (warmup)
    {
        holeMs = VOXEL_WARMUP_MS;
        aheadMs = sFrame < sAheadBackoff ? 0.0f
                : Clamp(spare - VOXEL_AHEAD_MARGIN_MS, 0.0f, VOXEL_AHEAD_MAX_MS);
    }
    UpdateFrustum();
    /* The wide view's verdict for this update (see there), never behind the
     * warm-up's fade (that builds the whole view before it lifts) nor in a
     * battle (its scenery has a curtain of its own). */
    sWideView = !warmup && !CtrVoxel_InBattle() && CameraHigh();
    sWideHolesLeft = 0;
    sFillCount = 0;
    if (sWideView)
        UpdateMapFill(VoxelWorld_Instance(0));
    else
        sMapFill = 0;
    VoxelWorld_SetMaterialView(sViewRect, VOXEL_MATERIAL_MARGIN);
    /* The atlas: what was composed after the last FrameEnd is uploaded now;
     * composing inside the frame is for the warm-up alone. No chunk of its
     * map can be built before it, and a map on screen without one is a hole
     * of its own. */
    {
        float atlasMs = warmup ? holeMs : 0.0f;

        if (!warmup && AtlasJobBusy() && sAtlasJob.forView && !sAtlasJob.ready
         && sFrame - sAtlasProgressFrame >= VOXEL_STARVE_FRAMES
         && sAfterStarvedFrames >= VOXEL_STARVE_FRAMES)
            atlasMs = VOXEL_STARVE_MS;
        RunAtlasJob(started, atlasMs, true);
        AtlasPagesReturn();
    }
    sRebuildStamp = sAtlasStamp;
    sDrawCount = 0;
    sRequestCount = 0;
    sStats.vertices = 0;
    sStats.draftsVisible = 0;
    sStats.draftMs = 0.0f;
    /* Behind the warm-up's fade every signature is checked at once, as the
     * view is built in full anyway. */
    sHashesLeft = warmup ? UINT_MAX : VOXEL_HASHES_PER_FRAME;
    {
        uint64_t visitStart = svcGetSystemTick(), visitEnd;
        unsigned kind = VisitView(playerX + sLeadX, playerZ + sLeadZ, &missing,
                                  warmup ? aheadMs > 0.0f : sFrame >= sAheadBackoff);

        visitEnd = svcGetSystemTick();
        sVisitTicks[kind] += visitEnd - visitStart; /* (for its log line) */
        sStats.atlasMs = TicksMs(visitEnd - started);
    }
    if (sRequestCount > 1)
        qsort(sRequests, sRequestCount, sizeof(sRequests[0]), CompareRequests);
    /*
     * Holes are covered at once with the draft of their square, except behind
     * the warm-up's fade, where the real thing is built before it lifts.
     */
    if (!warmup && sRequestCount != 0 && sRequests[0].need == NEED_HOLE)
    {
        uint64_t draftStart = svcGetSystemTick();
        unsigned before = sDrafts;

        if (DraftHoles(&missing) && sRequestCount > 1)
            qsort(sRequests, sRequestCount, sizeof(sRequests[0]), CompareRequests);
        sStats.draftMs = MsSince(draftStart);
        sDraftMs += sStats.draftMs;
        static uint32_t lastDraftLog;
        if (sFrame - lastDraftLog >= 120u && sDrafts != before)
        {
            lastDraftLog = sFrame;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL drafts: %u this frame, %u in all, %.1fms in all, "
                         "%u holes left, pending=%u, view=%u squares%s", sDrafts - before, sDrafts, sDraftMs,
                         missing, sRequestCount, sViewSquares, sWideView ? " (wide)" : "");
        }
    }
    /* What is still a hole after the drafts is filled flat (the fill): under
     * the wide view only, the walking view's streaming is left as it was. */
    if (sWideView)
        FillHoles();
    if (sWideView)
    {
        static uint32_t lastWideLog;

        if (sFrame - lastWideLog >= 20u)
        {
            lastWideLog = sFrame;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL wide: view %dx%d tiles (%u squares) swoop=%.2f drawn=%u "
                         "drafted=%u fills=%u holesLeft=%u missing=%u",
                         sViewRect[2] - sViewRect[0], sViewRect[3] - sViewRect[1], sViewSquares,
                         sSwoop, sDrawCount, sDrafts - (sStats.draftsMade), sFillCount,
                         sWideHolesLeft, missing);
        }
    }
    for (unsigned i = 0; i < sRequestCount; ++i)
    {
        /* Outside the warm-up a hole is one the draft could not cover: the
         * border belt, which has none. The others wait for their frame. */
        holes = holes || (sRequests[i].need == NEED_HOLE && (warmup || sRequests[i].site.border));
        starved = starved || RequestOverdue(&sRequests[i]);
    }
    if (sAfterStarvedFrames < VOXEL_STARVE_FRAMES)
        starved = false;
    /* Cancel first, then mark the surviving request consumed. */
    if (sJob.active && !JobValid())
        JobCancel();
    if (sJob.active)
    {
        bool found = false;
        for (unsigned i = 0; i < sRequestCount; ++i)
            if (SameSite(&sRequests[i].site, &sJob.site))
            {
                found = true;
                sJob.hole = sRequests[i].need == NEED_HOLE;
                sJob.forView = sRequests[i].need <= NEED_STALE;
                sJob.overdue = sJob.overdue || RequestOverdue(&sRequests[i]);
                sJob.waitIndex = sRequests[i].waitIndex;
                break;
            }
        /* Once geometry is complete, finish its bounded sort/pack/upload
         * instead of discarding all the CPU work for a newly visible hole. */
        if (!found || (holes && !sJob.hole && !sJob.overdue && sJob.phase < JOB_SORT))
            JobCancel();
    }
    for (unsigned i = 0; i < sRequestCount; ++i)
        if (sJob.active && SameSite(&sRequests[i].site, &sJob.site))
            sRequests[i].done = true;

    sHolesOnly = !warmup;
    if (!warmup)
    {
        holeMs = holes || starved ? InFrameBudget() : 0.0f;
        if (starved && holeMs < VOXEL_STARVE_MS)
            holeMs = VOXEL_STARVE_MS;
    }
    /* InFrameBudget removes the previous mesh work from the present cost:
     * the remaining fixed cost already includes visiting the scene/atlases.
     * Start the mesh clock here, or that visit is charged a second time.
     * Warm-up instead keeps its total update deadline, including the visit.
     * A forced rescue can overrun by one bounded slice. */
    {
        uint64_t buildStart = svcGetSystemTick();
        visitMs = TicksMs(buildStart - started);
        RunJobs(warmup ? started : buildStart, holeMs, aheadMs, true, warmup || starved, &missing);
        sInFrameBuildMs = MsSince(buildStart);
    }
    sHolesOnly = false;
    PackFills();

    if (sWarmupUntil != 0 && missing == 0 && !AtlasJobBusy())
        sWarmupUntil = 0;
    sLastBuildMs = MsSince(started);
    sStats.meshMs = sLastBuildMs;
    if (sStats.meshMs > sStats.meshPeakMs)
        sStats.meshPeakMs = sStats.meshMs;
    sStats.chunksMissing = missing;
    if (missing != 0)
        ++sKeyCounts.missingFrames;
    sStats.visibleChunks = sDrawCount;
    sStats.pendingBuilds = sRequestCount;
    sStats.draftsMade = sDrafts;
    sStats.aheadLost = sAheadLost;
    sStats.aheadSkipped = sAheadSkipped;
    static uint32_t lastMissingLog;
    if (missing != 0 && sFrame - lastMissingLog >= 120u)
    {
        lastMissingLog = sFrame;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL stream frame=%lu missing=%u pending=%u phase=%d age=%lu estimate=%.2fms starts=%u cancels=%u wasted=%.1fms mesh=%.2f/%.2f visit=%.2fms atlasSkip=%u drafts=%u",
                     (unsigned long)sFrame, missing, sRequestCount, sJob.active ? sJob.phase : -1,
                     (unsigned long)(sJob.active ? sFrame - sJob.firstFrame : 0),
                     sJob.active ? PhaseCost(sJob.phase) : 0.0f,
                     sJobStarts, sJobCancels, sCancelledBuildMs, sInFrameBuildMs, holeMs, visitMs,
                     sAtlasExtensionsSkipped, sDrafts);
    }
    sViewReady = true;
}

void CtrVoxel_Idle(void)
{
    LampsRelease(false);
}

/*
 * The last frame's real tail: from the end of AfterSubmit to the next
 * FrameBegin. gameMs + vblankMs left out the bottom screen, the compositor's
 * preparation and whatever else ran there; on an Old 3DS the tail was ~3 ms
 * against ~0.9 counted, the builds took the difference, and the CPU came
 * back to the VBlank at 17 ms - a dropped frame in every twenty (port.log,
 * 2026-10-15). Measured only across two consecutive voxel frames.
 *
 * And only a tail that fits in a frame: one longer spans more than the frame
 * it follows - a held top screen (3ds_video.c returns from the present
 * without a frame, on the VBlank), a resume from the HOME menu - and taken as
 * the game's share it held the budget at VOXEL_OTHERS_CAP_MS, let go over a
 * dozen frames, after every return. A game frame that long is in gameMs as
 * well; the log keeps the tail as measured.
 */
static uint64_t sAfterEndTick;
static float sTailMs;

void CtrVoxel_NoteFrameWait(uint64_t waitTick)
{
    float tail = sAfterEndTick && waitTick > sAfterEndTick ? TicksMs(waitTick - sAfterEndTick) : 0.0f;

    sTailMs = tail <= VOXEL_FRAME_MS ? tail : 0.0f;
    sStats.tailMs = tail;
    sAfterEndTick = 0;
}

/*
 * The slice owed a model missing from the picture (StaticWantsRun), run
 * first whatever the budget: the draw that found the model missing once
 * converted it inside the frame. Work of the drawn path, then, not of the
 * builds' elastic time: timed apart into afterOwedMs (0 when no slice ran),
 * which afterMs counts as well.
 */
static void AfterOwedStatics(uint64_t started)
{
    uint64_t owedStart = svcGetSystemTick();

    if (StaticWantsRun(started, 0.0f, true))
        sStats.afterOwedMs = TicksMs(svcGetSystemTick() - owedStart);
}

void CtrVoxel_AfterSubmit(uint64_t frameBeginTick)
{
    static float sOthersMs;
    const CtrTiming *timing = CtrPlatform_GetTiming();
    float others = timing->gameMs + timing->vblankMs;

    if (sTailMs > others)
        others = sTailMs;
    float budget, aheadMs;
    uint64_t started;

    sStats.afterMs = sStats.afterBudgetMs = sStats.afterOwedMs = 0.0f;
    if (!sReady || !sViewReady)
        return;
    sViewReady = false;
    /* The cold meshes read back in an earlier frame, written by now. */
    if (sColdPendingCount != 0)
    {
        uint64_t landStart = svcGetSystemTick();

        ColdLand();
        sKeyCounts.coldMs += MsSince(landStart);
    }
    /* The game's and the audio's share of the frame to come, read off the
     * last ones: taken at once when it rises, let go slowly. */
    if (others > VOXEL_OTHERS_CAP_MS)
        others = VOXEL_OTHERS_CAP_MS;
    sOthersMs = others > sOthersMs ? others : sOthersMs * 0.85f + others * 0.15f;
    sStats.tailExpectedMs = sOthersMs;
    started = svcGetSystemTick();
    budget = VOXEL_FRAME_MS - TicksMs(started - frameBeginTick) - sOthersMs - VOXEL_AFTER_MARGIN_MS;
    if (sCrossingSoon)
        budget -= VOXEL_CROSSING_RESERVE_MS;
    budget = Clamp(budget, 0.0f, VOXEL_AFTER_MAX_MS);
    sStats.afterBudgetMs = budget;
    if (budget < VOXEL_AFTER_STARVED_MS)
        ++sAfterStarvedFrames;
    else
        sAfterStarvedFrames = 0;
    if (budget < 0.5f)
    {
        /* (a model missing from the picture has its slice all the same) */
        AfterOwedStatics(started);
        sStats.afterMs = MsSince(started);
        sLastBuildMs += sStats.afterMs;
        sAfterEndTick = svcGetSystemTick();
        return;
    }
    aheadMs = sFrame < sAheadBackoff ? 0.0f : budget;
    /* The asset payloads the builds resolve are the ones the frame's own
     * update resolved: nothing has run in between that retires one. */
    VoxelWorld_BeginBatch();
    /* A model missing from the picture has its slice first, the chunks'
     * jobs then counting it in; more of the missing ones, and the stale
     * ones (drawn meanwhile), in what the chunks leave. */
    AfterOwedStatics(started);
    RunAtlasJob(started, sAtlasJob.forView ? budget : aheadMs, false);
    if (sJob.active && !JobValid())
        JobCancel();
    RunJobs(started, budget, aheadMs, false, false, NULL);
    StaticWantsRun(started, budget, true);
    StaticWantsRun(started, budget, false);
    sStats.afterMs = MsSince(started);
    sLastBuildMs += sStats.afterMs;
    sAfterEndTick = svcGetSystemTick();
}

/* â”€â”€ Map changes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/*
 * Where each map sat in world space before this frame. Crossing a border makes
 * the neighbour the new origin, so everything that was on screen moves by that
 * neighbour's old offset.
 */
static struct
{
    int mapGroup, mapNum, originX, originY;
} sPreviousOrigins[MAX_VOXEL_MAP_INSTANCES];
static unsigned sPreviousOriginCount;

static void RememberInstanceOrigins(void)
{
    unsigned count = VoxelWorld_InstanceCount();

    sPreviousOriginCount = 0;
    for (unsigned i = 0; i < count && i < MAX_VOXEL_MAP_INSTANCES; ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);

        sPreviousOrigins[sPreviousOriginCount].mapGroup = inst->mapGroup;
        sPreviousOrigins[sPreviousOriginCount].mapNum = inst->mapNum;
        sPreviousOrigins[sPreviousOriginCount].originX = inst->originX;
        sPreviousOrigins[sPreviousOriginCount].originY = inst->originY;
        ++sPreviousOriginCount;
    }
}

/*
 * Walking from Littleroot to Route 101 is not a teleport: the player keeps
 * moving in a straight line and only the coordinate system changes underneath.
 * If the new map was one of the connections we were already drawing, the
 * camera is shifted by that map's old offset and the view does not move at
 * all. Anything else - a warp, a door, a new game - is a genuine cut, and
 * there the camera snaps.
 */
/* How far, in tiles, the player may seem to move on a crossing. */
#define VOXEL_CROSSING_STEP 3.0f

/* True for a crossing, false for a cut. */
static bool HandleMapChange(int mapGroup, int mapNum, float playerX, float playerZ)
{
    for (unsigned i = 0; i < sPreviousOriginCount; ++i)
    {
        if (sPreviousOrigins[i].mapGroup != mapGroup || sPreviousOrigins[i].mapNum != mapNum)
            continue;
        if (sPreviousOrigins[i].originX == 0 && sPreviousOrigins[i].originY == 0)
            break; /* already the origin: nothing moved */
        /* A map placed further away is reached by a warp as readily as on
         * foot: a crossing leaves the player where they were. */
        if (fabsf(playerX - (sLastX - sPreviousOrigins[i].originX)) > VOXEL_CROSSING_STEP
         || fabsf(playerZ - (sLastZ - sPreviousOrigins[i].originY)) > VOXEL_CROSSING_STEP)
            break;
        VoxelCamera_Shift(&sCamera, (float)-sPreviousOrigins[i].originX,
                          (float)-sPreviousOrigins[i].originY);
        sDappleAnchorX += sPreviousOrigins[i].originX;
        sDappleAnchorZ += sPreviousOrigins[i].originY;
        sBeltOriginX -= sPreviousOrigins[i].originX;
        sBeltOriginY -= sPreviousOrigins[i].originY;
        sLastX -= (float)sPreviousOrigins[i].originX;
        sLastZ -= (float)sPreviousOrigins[i].originY;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: crossed into %d:%d, camera shifted by %d,%d",
                     mapGroup, mapNum, -sPreviousOrigins[i].originX,
                     -sPreviousOrigins[i].originY);
        return true;
    }
    /* A cut shows a new scene: the pattern may start afresh, and the belt is
     * cut on a grid of its own, of which nothing built so far is part. */
    sDappleAnchorX = sDappleAnchorZ = 0;
    sBeltOriginX = sBeltOriginY = 0;
    ++sBeltGrid;
    VoxelCamera_SetGround(&sCamera, VoxelEntities_PlayerGround(), 1);
    VoxelCamera_Snap(&sCamera, playerX, playerZ);
    return false;
}

/* Everything about the maps on screen that cached geometry depends on. */
static uint32_t InstanceSignature(void)
{
    uint32_t hash = 2166136261u;
    unsigned count = VoxelWorld_InstanceCount();

    hash = (hash ^ count) * 16777619u;
    for (unsigned i = 0; i < count; ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);
        const uint32_t fields[] = {
            (uint32_t)inst->mapGroup, (uint32_t)inst->mapNum,
            (uint32_t)inst->originX, (uint32_t)inst->originY,
            (uint32_t)inst->width, (uint32_t)inst->height,
            (uint32_t)(uintptr_t)inst->layout,
            (uint32_t)(uintptr_t)inst->primaryTileset,
            (uint32_t)(uintptr_t)inst->secondaryTileset,
            inst->indoor ? 1u : 0u,
            (uint32_t)inst->lift,
        };

        for (unsigned f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f)
            hash = (hash ^ fields[f]) * 16777619u;
    }
    return hash;
}

/* â”€â”€ Frame update â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/* â”€â”€ The 3D battle â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/*
 * A battle's scenery is the world itself (3ds_video.c, RenderBattleWorld),
 * seen from a camera of its own: on the stage voxel_battle.c chooses near the
 * player, lower and closer than the field's (VOXEL_BATTLE_PITCH, _DISTANCE),
 * with nobody in it - the battle draws its own Pokemon and trainers, and the
 * sprites the billboards are decoded from are the battle's now. The camera
 * glides in from where the field left it while the intro slides its scenery
 * in, which is what that slide was on the GBA.
 *
 * The glide is a shot of its own (BattleCamera): the camera cranes up as it
 * leaves the field, a dolly zoom on the way - further back and narrower, the
 * framing at the target kept - flattens the world into a telephoto swoop,
 * then it rushes in, lands with a push and a short quake, and settles. Only
 * the camera moves: what is on screen is never more than the field or the
 * stage shows (a narrower view from further back covers less ground), so no
 * cell is drawn that the battle would not draw anyway. RenderBattleWorld
 * lays the effects over it (CtrVoxel_BattleIntro).
 */
#define VOXEL_BATTLE_GLIDE_FRAMES 100u
/* The share of the glide spent travelling; the rest is the landing. */
#define VOXEL_BATTLE_TRAVEL 0.78f
/* Degrees the camera climbs at the top of its arc. */
#define VOXEL_BATTLE_CRANE 16.0f
#define VOXEL_BATTLE_CRANE_MAX 75.0f
/* How far the field of view narrows mid-flight (the dolly zoom). */
#define VOXEL_BATTLE_VERTIGO 0.40f
/* The landing: how much closer its push comes, and its quake, in tiles. */
#define VOXEL_BATTLE_PUNCH 0.16f
#define VOXEL_BATTLE_QUAKE 0.07f
/* Frames the cinema bars take to close in at the start. */
#define VOXEL_BATTLE_BARS_IN 18.0f
/* An intro that never starts (a battle resumed without one): glide anyway. */
#define VOXEL_BATTLE_INTRO_WAIT 150u
/* The stage search's slice per frame (VoxelBattle_StepStage): 1023 cells,
 * then 169 spots. */
#define VOXEL_BATTLE_STAGE_CELLS 32u
#define VOXEL_BATTLE_STAGE_SPOTS 8u

static struct
{
    bool on, begun, chosen, sliding, glided;
    uint32_t frames, glide, searchFrames;
    float searchMs, searchWorstMs;
    VoxelCamera field;
    float targetX, targetZ, ground;
    float shakeX, shakeY;
    CtrVoxelBattleIntro intro;
} sBattle;

bool CtrVoxel_IsAvailableForBattle(void)
{
    return sReady && !sParked && VoxelWorld_IsBattleMapAvailable();
}

void CtrVoxel_BeginBattle(void)
{
    if (!sReady || sBattle.on)
        return;
    memset(&sBattle, 0, sizeof(sBattle));
    sBattle.on = true;
    sBattle.field = sCamera;
}

void CtrVoxel_EndBattle(void)
{
    if (!sBattle.on)
        return;
    sBattle.on = false;
    sCamera = sBattle.field;
    TrackMotion(sLastX, sLastZ, true);
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL battle over: the field's camera back");
}

bool CtrVoxel_InBattle(void)
{
    return sBattle.on;
}

void CtrVoxel_SetBattleFrame(bool introSliding, float shakeX, float shakeY)
{
    sBattle.sliding = introSliding;
    sBattle.shakeX = shakeX;
    sBattle.shakeY = shakeY;
}

static float Smooth(float t)
{
    t = Clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* Smootherstep: slower off the mark and into the stop than Smooth. */
static float Smoother(float t)
{
    t = Clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

void CtrVoxel_BattleIntro(CtrVoxelBattleIntro *intro)
{
    static const CtrVoxelBattleIntro none;

    *intro = sBattle.on ? sBattle.intro : none;
}

/* The battle camera this frame: on its way from the field's to the stage
 * until the glide is over, moved by the scenery's shake. */
static void BattleCamera(void)
{
    const float degrees = 3.14159265f / 180.0f;
    const VoxelCamera *from = &sBattle.field;
    float u, p, q, t, arc, fov, punch, quake, k, x, z, ground, pitch, distance;
    CtrVoxelBattleIntro *intro = &sBattle.intro;

    if (sBattle.sliding || sBattle.frames >= VOXEL_BATTLE_INTRO_WAIT)
        sBattle.glided = true;
    ++sBattle.frames;
    /* Where to is not known yet: still where the field left it. */
    if (sBattle.chosen && sBattle.glided && sBattle.glide < VOXEL_BATTLE_GLIDE_FRAMES)
        ++sBattle.glide;
    u = (float)sBattle.glide / (float)VOXEL_BATTLE_GLIDE_FRAMES;
    /* p: the flight, 0..1; q: the landing after it, 0..1. */
    p = Clamp(u / VOXEL_BATTLE_TRAVEL, 0.0f, 1.0f);
    q = Clamp((u - VOXEL_BATTLE_TRAVEL) / (1.0f - VOXEL_BATTLE_TRAVEL), 0.0f, 1.0f);
    t = Smoother(p);
    arc = p > 0.0f && p < 1.0f ? sinf(p * 3.14159265f) : 0.0f;
    x = from->targetX + (sBattle.targetX - from->targetX) * t;
    z = from->targetZ + (sBattle.targetZ - from->targetZ) * t;
    ground = from->ground + (sBattle.ground - from->ground) * t;
    pitch = from->pitch + (VOXEL_BATTLE_PITCH - from->pitch) * t;
    distance = from->distance + (VOXEL_BATTLE_DISTANCE - from->distance) * t;
    /* The crane: up over the world mid-flight, short of looking straight down. */
    pitch += VOXEL_BATTLE_CRANE * arc;
    if (pitch > VOXEL_BATTLE_CRANE_MAX && pitch > from->pitch)
        pitch = from->pitch > VOXEL_BATTLE_CRANE_MAX ? from->pitch : VOXEL_BATTLE_CRANE_MAX;
    /* The dolly zoom: narrower and further back, the target framed the same. */
    fov = from->fov * (1.0f - VOXEL_BATTLE_VERTIGO * arc * arc);
    if (arc > 0.0f)
        distance *= tanf(from->fov * 0.5f * degrees) / tanf(fov * 0.5f * degrees);
    else
        fov = from->fov;
    /* The landing: a push in that springs back, and a quake dying away. */
    punch = q > 0.0f && q < 1.0f ? expf(-6.0f * q) * sinf(3.0f * 3.14159265f * q) : 0.0f;
    quake = q > 0.0f && q < 1.0f ? VOXEL_BATTLE_QUAKE * expf(-7.0f * q) : 0.0f;
    distance *= 1.0f - VOXEL_BATTLE_PUNCH * punch;
    x += quake * sinf((float)sBattle.glide * 2.7f);
    z += quake * cosf((float)sBattle.glide * 3.9f);
    sCamera.fov = fov;

    /* What the effects over the world need of it (CtrVoxel_BattleIntro). */
    intro->flight = arc;
    /* Smootherstep's slope, 30 p^2 (1 - p)^2, over its peak of 1.875. */
    intro->speed = p > 0.0f && p < 1.0f ? 16.0f * p * p * (1.0f - p) * (1.0f - p) : 0.0f;
    intro->impact = q > 0.0f && q < 1.0f ? expf(-5.0f * q) * (1.0f - q) : 0.0f;
    intro->bars = Smooth((float)sBattle.frames / VOXEL_BATTLE_BARS_IN) * (1.0f - Smooth((q - 0.35f) / 0.65f));
    intro->time = (float)sBattle.frames;
    /*
     * BG3's scroll moves the scenery: a GBA pixel is CTR_BATTLE_ZOOM screen
     * pixels, and a screen pixel at the target this many tiles - across, and
     * along the ground foreshortened by the pitch.
     */
    k = CTR_BATTLE_ZOOM * 2.0f * distance * tanf(sCamera.fov * 0.5f * degrees)
      / (float)CTR_GAME_HEIGHT;
    x += sBattle.shakeX * k;
    z += sBattle.shakeY * k / sinf(pitch * degrees);
    VoxelCamera_Frame(&sCamera, x, z, ground, pitch, distance);
}

/* ── The Fly cinematic ─────────────────────────────────────────────────────── */

/*
 * The camera's high pose over the world (voxel_fly.c, CtrVoxel_SetFly): on
 * top of the player's pose this frame, the pitch steepened and the camera
 * dollied out by `lift`, the view led towards the destination on the old map
 * and trailing behind the player on the new one, and the eye lifted straight
 * up into the cloud deck by `rise`. The lead goes through the camera's swoop
 * shift, which the next follow step takes off again, so it never feeds the
 * damping. The far plane (VOXEL_FAR, 200) and the view's reach
 * (VOXEL_VIEW_REACH_MAX, 44 tiles) bound the pose: at the deck the eye is
 * some 70 tiles up, and the far corner of the ground it sees is within both.
 */
#define VOXEL_FLY_PITCH 62.0f    /* degrees below the horizon, at the top */
#define VOXEL_FLY_DISTANCE 24.0f /* tiles of ground from the target to the eye's foot */
#define VOXEL_FLY_RISE 22.0f     /* tiles the eye goes on up into the deck */
#define VOXEL_FLY_TRAVEL 14.0f   /* tiles the view goes along the way, ahead over the
                                  * old map and in from behind over the new one */

static void FlyCamera(void)
{
    float lift = Clamp(sFly.lift, 0.0f, 1.0f);
    float pitch = sCamera.pitch + (VOXEL_FLY_PITCH - sCamera.pitch) * lift;
    float distance = sCamera.distance + (VOXEL_FLY_DISTANCE - sCamera.distance) * lift;

    /* Below the pose (the bird's dive): drawn in on the player, the pitch kept. */
    if (sFly.lift < 0.0f)
        distance = sCamera.distance * (1.0f + Clamp(sFly.lift, -1.0f, 0.0f));
    /* Under way: the view travels towards the destination, so the whole
     * flight - out over the old map, in over the new - moves one way. The
     * share is scaled by the lift so the new map is built on the pose. */
    float along = VOXEL_FLY_TRAVEL * Clamp(sFly.travel, -1.0f, 1.0f) * lift;
    float dx = sFly.dirX * along, dz = sFly.dirZ * along;


    VoxelCamera_Frame(&sCamera, sCamera.targetX + dx, sCamera.targetZ + dz, sCamera.ground,
                      pitch, distance);
    sCamera.swoopX += dx;
    sCamera.swoopZ += dz;
    sCamera.y += VOXEL_FLY_RISE * Clamp(sFly.rise, 0.0f, 1.0f);
}

void CtrVoxel_SetFly(const CtrVoxelFly *fly)
{
    if (fly == NULL || !fly->active)
    {
        sFly.active = false;
        return;
    }
    sFly = *fly;
}

C3D_Tex *CtrVoxel_CloudTexture(void)
{
    return sHaveFog ? &sFogTex : NULL;
}

/* The lamps' buffers given back: at once (`now`: nothing queued draws from
 * them), or once unlit for VOXEL_LAMP_RELEASE_MS. */
static void LampsRelease(bool now)
{
    if (sLampGpu == NULL && sLampScratch == NULL)
        return;
    if (!now && TicksMs(svcGetSystemTick() - sLampDrawn) < VOXEL_LAMP_RELEASE_MS)
        return;
    linearFree(sLampGpu);
    free(sLampScratch);
    sLampGpu = NULL;
    sLampScratch = NULL;
    if (sLampPoolTex.data != NULL)
    {
        C3D_TexDelete(&sLampPoolTex);
        memset(&sLampPoolTex, 0, sizeof(sLampPoolTex));
    }
    sLampReady = false;
    sLampVertices = sLampGroundVertices = 0;
}

/*
 * A lamp's pool of light: a disc filling the texture, brightest in the middle
 * and gone at its rim, (1 - r^2)^2 - the falloff the pools used to carry in
 * their vertices, now per pixel. L8, filtered, its border black so clamping
 * leaves the corners of a pool's quads unlit. 4 KiB, with the lamps' buffers;
 * without it the pools are not drawn, the glass still is.
 */
#define VOXEL_LAMP_POOL_DIM 64
static void MakeLampPool(void)
{
    uint8_t *texels;

    if (!C3D_TexInit(&sLampPoolTex, VOXEL_LAMP_POOL_DIM, VOXEL_LAMP_POOL_DIM, GPU_L8))
    {
        memset(&sLampPoolTex, 0, sizeof(sLampPoolTex));
        return;
    }
    texels = sLampPoolTex.data;
    for (unsigned y = 0; y < VOXEL_LAMP_POOL_DIM; ++y)
        for (unsigned x = 0; x < VOXEL_LAMP_POOL_DIM; ++x)
        {
            float dx = ((float)x + 0.5f) / (VOXEL_LAMP_POOL_DIM / 2) - 1.0f;
            float dy = ((float)y + 0.5f) / (VOXEL_LAMP_POOL_DIM / 2) - 1.0f;
            float w = 1.0f - (dx * dx + dy * dy);

            texels[CtrVideo_Texel(x, y, VOXEL_LAMP_POOL_DIM)] =
                w > 0.0f ? (uint8_t)(w * w * 255.0f + 0.5f) : 0;
        }
    C3D_TexSetFilter(&sLampPoolTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sLampPoolTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexFlush(&sLampPoolTex);
}

/* The lamps' buffers, for the first lamps lit: whether they are there. When
 * there is no room, the lamps stay dark and are asked for again later. */
static bool LampsTake(void)
{
    if (sLampGpu != NULL)
        return true;
    if (sFrame < sLampRetry)
        return false;
    sLampScratch = malloc(VOXEL_LAMP_VERTICES * sizeof(VoxelVertex));
    sLampGpu = sLampScratch != NULL ? linearAlloc(VOXEL_LAMP_VERTICES * sizeof(VoxelGpuVertex)) : NULL;
    if (sLampGpu == NULL)
    {
        free(sLampScratch);
        sLampScratch = NULL;
        sLampRetry = sFrame + VOXEL_PAGE_RETRY_FRAMES;
        if (!sLampNoRoomLogged)
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: no room for the night lamps (%lu KiB linear, linear free=%lu): "
                         "they stay dark until there is",
                         (unsigned long)(VOXEL_LAMP_VERTICES * sizeof(VoxelGpuVertex) >> 10),
                         (unsigned long)linearSpaceFree());
        sLampNoRoomLogged = true;
        return false;
    }
    MakeLampPool();
    sLampReady = false;
    return true;
}

/* The lamps on screen and the light they cast, rebuilt when the view moves. */
static void UpdateNightLamps(void)
{
    if (!sMeasureLamps || !CtrGame_IsOutdoor() || CtrDayNight_Get()->glow <= 0.0f)
    {
        sLampReady = false;
        sLampVertices = sLampGroundVertices = 0;
        LampsRelease(false);
        return;
    }
    if (!LampsTake())
    {
        sLampVertices = sLampGroundVertices = 0;
        return;
    }
    if (sLampReady && sLampSignature == sInstanceSignature && sLampDigest == sLiveDigest
        && sLampView[0] <= sViewRect[0] - 3 && sLampView[1] <= sViewRect[1] - 3
        && sLampView[2] >= sViewRect[2] + 3 && sLampView[3] >= sViewRect[3] + 3)
        return;
    VoxelBuilder glass, ground;
    int pad = 3 + VOXEL_LAMP_PAD;

    for (;;)
    {
        VoxelBuilder_Init(&glass, sLampScratch, VOXEL_LAMP_RESERVE);
        VoxelBuilder_Init(&ground, sLampScratch + VOXEL_LAMP_GROUND_FIRST, VOXEL_LAMP_GROUND_RESERVE);
        VoxelBuilder_SetOrigin(&glass, sDynamicX, sDynamicZ);
        VoxelBuilder_SetOrigin(&ground, sDynamicX, sDynamicZ);
        sLampView[0] = sViewRect[0] - pad;
        sLampView[1] = sViewRect[1] - pad;
        sLampView[2] = sViewRect[2] + pad;
        sLampView[3] = sViewRect[3] + pad;
        /* (each map's lamps by its rows: DrawnRows) */
        for (unsigned index = 0; index < VoxelWorld_InstanceCount(); ++index)
            VoxelSign_EmitNight(&glass, &ground, VoxelWorld_Instance(index),
                               sLampView[0], sLampView[1] - DrawnRows(VoxelWorld_Instance(index)),
                               sLampView[2], sLampView[3] - DrawnRows(VoxelWorld_Instance(index)));
        /* Near full: some lamp may have been left out (EmitNight stops at
         * the capacity). Over the view alone, as before the padding. */
        if (pad == 3 || (glass.capacity - glass.count >= 64u && ground.capacity - ground.count >= 64u))
            break;
        pad = 3;
    }
    sLampVertices = glass.count;
    sLampGroundVertices = ground.count;
    PackDynamic(sLampScratch, glass.count, sLampGpu);
    PackDynamic(sLampScratch + VOXEL_LAMP_GROUND_FIRST, ground.count, sLampGpu + VOXEL_LAMP_GROUND_FIRST);
    sLampSignature = sInstanceSignature;
    sLampDigest = sLiveDigest;
    sLampX = sDynamicX;
    sLampZ = sDynamicZ;
    sLampReady = true;
}

/*
 * The animated tiles (water, flowers) of the atlases in view. The current
 * map's atlas, and those of the maps beside it in view that share its primary
 * tileset (Route 124's Mossdeep pair beside Lilycove's own), are redrawn by
 * one pass (VoxelAnim_*): it takes the tiles written since the last one,
 * composes what shows them in all those atlases over as many frames as
 * VOXEL_ANIM_COMPOSE_MS allows, and then writes it out at once, a GPU copy
 * for each run of slots side by side in a row of a page (short gaps filled
 * in), all behind one split of the frame's command list - out of the staging
 * buffer the atlas jobs and the page stream borrow too, on a frame neither
 * does. Nothing of a page is read back. When the game writes the same tiles
 * again (every 16 frames) into atlases that have not changed, the pass's
 * slots are kept and only composed anew (VoxelAnim_Again) - or not at all,
 * once the tiles hold a state the pass has composed before.
 *
 * It used to go a few slots a frame, two copies each, one atlas and one page
 * after another: Mossdeep's atlas has some 160 slots of sea and shore, which
 * at two a frame took longer than the game takes to move the sea on (16
 * frames), so its sea all but stood still; its tiles showed different frames
 * along their edges; and the sea of the route beside, another atlas, waited
 * its turn and ran out of step with it.
 */
#define VOXEL_ANIM_COMPOSE_MS 0.5f
/*
 * The game writes its animations a frame apart - the flowers, the sea and
 * its sandy, grassy and waterfall edges on frames 0 to 4 of every 16 - so a
 * pass waits for a frame with no write, up to this many, and takes them all.
 */
#define VOXEL_ANIM_SETTLE 8u
/* The most runs (GPU copies) a frame writes, in the GX queue's room for them
 * (CtrVideo_TryVoxelAnimUpload): the rest go the frame after. The sea round
 * Mossdeep is some 22 runs. */
#define VOXEL_ANIM_RUNS_FRAME 32u

static bool AnimShares(const VoxelAtlasSlot *atlas, const VoxelAtlasSlot *active)
{
    return atlas == active
        || (atlas->valid && atlas->primaryTileset == active->primaryTileset && AtlasInView(atlas));
}

static uint64_t sAnimFrameStart;

static bool AnimStillTime(void)
{
    return svcGetSystemTick() - sAnimFrameStart
         < (uint64_t)(VOXEL_ANIM_COMPOSE_MS * (SYSCLOCK_ARM11 / 1000.0));
}

/* The atlas a pair of the pass writes to, if it is still the one it read. */
static VoxelAtlasSlot *AnimAtlasOf(unsigned pair)
{
    VoxelAtlasSlot *atlas = sAnimAtlas[pair];

    if (atlas == NULL || !atlas->valid || atlas->generation != sAnimGeneration[pair]
     || atlas->primaryTileset != sAnimPass.primary[pair]
     || atlas->secondaryTileset != sAnimPass.secondary[pair])
        return NULL;
    return atlas;
}

static void AnimBegin(const VoxelMapInstance *inst)
{
    static bool fullLogged;
    VoxelAtlasSlot *active = FindAtlas(inst);
    bool fits = true;

    bool again;

    /* an atlas being composed or swapped in: the pass after it */
    if (active == NULL || !active->valid || AtlasJobBusy())
        return;
    /*
     * The same tiles as the last pass's, for the same atlases as they were:
     * the same slots, made of the same blocks, composed again. The game
     * writes the same tiles every 16 frames, and finding the slots - every
     * id of every atlas in view - cost as much as composing them.
     */
    again = sAnimPass.targets != 0 && !sAnimPass.full
         && memcmp(sAnimPass.work, sAnimDirty, sizeof(sAnimDirty)) == 0;
    for (unsigned a = 0; a < VOXEL_ATLAS_SLOTS && again; ++a)
    {
        VoxelAtlasSlot *atlas = &sAtlases[a];
        bool shares = AnimShares(atlas, active);

        again = shares == sAnimKey[a].shares
             && (!shares || (atlas->generation == sAnimKey[a].generation
                             && atlas->extension == sAnimKey[a].extension
                             && atlas->map.used == sAnimKey[a].used
                             && atlas->primaryTileset == sAnimPass.primary[a]
                             && atlas->secondaryTileset == sAnimPass.secondary[a]
                             && (atlas == active) != sAnimPass.shared[a]));
    }
    if (again)
        VoxelAnim_Again(&sAnimPass);
    else
    {
        VoxelAnim_Begin(&sAnimPass, sAnimDirty);
        ++sStats.animationBegins;
    }
    memset(sAnimDirty, 0, sizeof(sAnimDirty));
    sAnimPending = false;
    /* a pass that had no room for every atlas starts the next one with the
     * atlas that did not fit, so none is left standing still for good */
    for (unsigned k = 0, first = sAnimFirstAtlas; k < VOXEL_ATLAS_SLOTS; ++k)
    {
        unsigned a = (first + k) % VOXEL_ATLAS_SLOTS;
        VoxelAtlasSlot *atlas = &sAtlases[a];
        VoxelMapInstance pair;

        sAnimAtlas[a] = NULL;
        sAnimKey[a].shares = false;
        if (!fits || !AnimShares(atlas, active))
            continue;
        sAnimAtlas[a] = atlas;
        sAnimGeneration[a] = atlas->generation;
        sAnimKey[a].shares = true;
        sAnimKey[a].generation = atlas->generation;
        sAnimKey[a].extension = atlas->extension;
        sAnimKey[a].used = atlas->map.used;
        if (again)
            continue;
        memset(&pair, 0, sizeof(pair));
        pair.primaryTileset = atlas->primaryTileset;
        pair.secondaryTileset = atlas->secondaryTileset;
        fits = VoxelAnim_AddAtlas(&sAnimPass, a, &pair, atlas != active, &atlas->map, CtrVideo_GetBgVram());
        if (!fits)
            sAnimFirstAtlas = a;
    }
    if (!fits && !fullLogged)
    {
        fullLogged = true;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL animated tiles: pass full at %u slots, %u blocks; "
                     "the rest wait for the next write", sAnimPass.targets, sAnimPass.blocks);
    }
    sAnimCursor = 0;
    sAnimBegan = sFrame;
    sAnimActive = sAnimPass.targets != 0;
}

static void AnimateTiles(const VoxelMapInstance *inst)
{
    struct { unsigned first, count, offset; } runs[VOXEL_ANIM_RUNS_FRAME];
    unsigned count = 0, used = 0, slots = 0;
    uint8_t *staging = (uint8_t *)sAtlasStaging;

    sAnimFrameStart = svcGetSystemTick();
    if (!sAnimActive)
    {
        if (!sAnimPending)
            return;
        if (sAnimWaitFrom == 0)
            sAnimWaitFrom = sFrame;
        if (sAnimFresh && sFrame - sAnimWaitFrom < VOXEL_ANIM_SETTLE)
        {
            sAnimFresh = false;
            return;
        }
        sAnimFresh = false;
        sAnimWaitFrom = 0;
        AnimBegin(inst);
        if (!sAnimActive)
            return;
    }
    if (!VoxelAnim_Compose(&sAnimPass, CtrVideo_GetBgVram(), AnimStillTime))
        return;
    if (sAtlasUploadPending || AtlasJobBusy() || staging == NULL)
    {
        ++sStats.animationWaits;
        return;
    }
    while (sAnimCursor < sAnimPass.targets && count < VOXEL_ANIM_RUNS_FRAME)
    {
        const VoxelAnimTarget *target = &sAnimPass.target[sAnimCursor];
        unsigned run = VoxelAnim_Run(&sAnimPass, sAnimCursor);
        VoxelAtlasSlot *atlas = AnimAtlasOf(target->pair);

        /* an atlas rebuilt since, or a page the map beside has given up */
        if (atlas == NULL || AtlasTex(atlas, target->page)->data == NULL)
        {
            sAnimCursor += run;
            continue;
        }
        if (used + run * VOXEL_ATLAS_SLOT * VOXEL_ATLAS_SLOT * sizeof(uint16_t)
                > VOXEL_ATLAS_PIXELS * sizeof(uint16_t)
         || !CtrVideo_TryVoxelAnimUpload())
            break;
        VoxelAnim_WriteRun(&sAnimPass, sAnimCursor, run, (uint16_t *)(staging + used));
        runs[count].first = sAnimCursor;
        runs[count].count = run;
        runs[count].offset = used;
        ++count;
        used += run * VOXEL_ATLAS_SLOT * VOXEL_ATLAS_SLOT * sizeof(uint16_t);
        slots += run;
        sAnimCursor += run;
    }
    if (count != 0)
    {
        GSPGPU_FlushDataCache(staging, used);
        /* one split for all of them, not one each (CtrVideo_CopiesBegin) */
        CtrVideo_CopiesBegin();
        for (unsigned i = 0; i < count; ++i)
        {
            const VoxelAnimTarget *target = &sAnimPass.target[runs[i].first];
            uint8_t *dest = (uint8_t *)AtlasTex(AnimAtlasOf(target->pair), target->page)->data;
            /* two lines, the slots' top halves and their bottom halves, a row
             * of blocks apart in the page */
            unsigned line = runs[i].count * VOXEL_ATLAS_SLOT * 8u * sizeof(uint16_t);
            unsigned gap = (VOXEL_ATLAS_W / 8u) * 128u - line;

            GX_TextureCopy((u32 *)(staging + runs[i].offset), GX_BUFFER_DIM(line >> 4, 0),
                           (u32 *)(dest + VoxelAnim_RunOffset(&sAnimPass, runs[i].first)),
                           GX_BUFFER_DIM(line >> 4, gap >> 4), 2u * line, 8);
        }
        CtrVideo_CopiesEnd();
        sStats.animationUploads += count;
        sStats.animatedMetatiles += slots;
        sAtlasUploadPending = true;
    }
    if (count == 0 && sAnimCursor < sAnimPass.targets)
        ++sStats.animationWaits;
    if (sAnimCursor >= sAnimPass.targets)
    {
        sAnimActive = false;
        ++sStats.animationPasses;
        sStats.animationKept += sAnimPass.kept;
        if (sFrame - sAnimBegan + 1u > sStats.animationPeakFrames)
            sStats.animationPeakFrames = sFrame - sAnimBegan + 1u;
    }
}

/*
 * The building pages' animated texels (a fountain's jet): every run that
 * reads a tile the game has written since the last pass, on a page in VRAM,
 * is composed from the tiles as the game now holds them and copied over its
 * blocks by the GPU - a few hundred bytes. Only the current map's: BG VRAM
 * holds its tilesets, not those of the maps beside it, whose runs keep the
 * frame they were drawn with until it is the current map again.
 */
static void AnimateBuildings(const VoxelMapInstance *inst)
{
    unsigned used = 0;
    bool left = false;

    if (!sBuildingAnimPending)
        return;
    if (sBuildingAnimOut == NULL)
    {
        sBuildingAnimPending = false;
        return;
    }
    for (unsigned run = 0; run < VoxelBuildings_AnimRunCount(); ++run)
    {
        unsigned page, offset, bytes;
        int layout;
        BuildingPageSlot *slot;
        uint16_t *out;

        if (!VoxelBuildings_AnimRun(run, &page, &layout, &offset, &bytes)
         || layout != VoxelBuildings_LayoutOf(inst)
         || !VoxelBuildings_AnimRunDirty(run, sBuildingAnimDirty))
            continue;
        slot = FindPage((int)page);
        if (slot == NULL || slot->loaded < slot->w * slot->h)
            continue;   /* not drawn now; redrawn at the next write once it is */
        if (used + bytes > VOXEL_BUILDING_ANIM_BYTES || !CtrVideo_TryVoxelUpload())
        {
            left = true;    /* this frame is full: the rest at the next */
            break;
        }
        out = (uint16_t *)((uint8_t *)sBuildingAnimOut + used);
        VoxelBuildings_ComposeAnimRun(run, CtrVideo_GetBgVram(), out);
        GSPGPU_FlushDataCache(out, bytes);
        C3D_SyncTextureCopy((u32 *)out, 0, (u32 *)((uint8_t *)slot->tex.data + offset), 0, bytes, 8);
        used += bytes;
        ++sStats.animationUploads;
    }
    if (!left)
    {
        memset(sBuildingAnimDirty, 0, sizeof(sBuildingAnimDirty));
        sBuildingAnimPending = false;
    }
}

static void DropTileAnimation(void)
{
    if (sAnimPending)
        memset(sAnimDirty, 0, sizeof(sAnimDirty));
    sAnimPending = sAnimActive = sAnimFresh = false;
    sAnimWaitFrom = 0;
    if (sBuildingAnimPending)
        memset(sBuildingAnimDirty, 0, sizeof(sBuildingAnimDirty));
    sBuildingAnimPending = false;
}

/*
 * Where the update's world= share goes, for the hardware log: 3 ms of it on
 * Route 110 against 1.5-2 elsewhere, before the GPU could start (port.log,
 * 2026-10-26). Mean and worst per part over the last VOXEL_PARTS_FRAMES.
 */
enum { PART_INSTANCES, PART_CHECK, PART_SUN, PART_PLACE, PART_CROSSING, PART_COUNT };
#define VOXEL_PARTS_FRAMES 600u
static uint64_t sPartTicks[PART_COUNT], sPartWorst[PART_COUNT];
static unsigned sPartFrames;

static void NotePart(int part, uint64_t *since)
{
    uint64_t now = svcGetSystemTick(), ticks = now - *since;

    sPartTicks[part] += ticks;
    if (ticks > sPartWorst[part])
        sPartWorst[part] = ticks;
    *since = now;
}

static void LogParts(void)
{
    if (++sPartFrames < VOXEL_PARTS_FRAMES)
        return;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL world parts (ms mean/worst): instances %.2f/%.2f check %.2f/%.2f "
                 "sun %.2f/%.2f place %.2f/%.2f crossing %.2f/%.2f (%u maps)",
                 TicksMs(sPartTicks[PART_INSTANCES]) / sPartFrames, TicksMs(sPartWorst[PART_INSTANCES]),
                 TicksMs(sPartTicks[PART_CHECK]) / sPartFrames, TicksMs(sPartWorst[PART_CHECK]),
                 TicksMs(sPartTicks[PART_SUN]) / sPartFrames, TicksMs(sPartWorst[PART_SUN]),
                 TicksMs(sPartTicks[PART_PLACE]) / sPartFrames, TicksMs(sPartWorst[PART_PLACE]),
                 TicksMs(sPartTicks[PART_CROSSING]) / sPartFrames, TicksMs(sPartWorst[PART_CROSSING]),
                 VoxelWorld_InstanceCount());
    memset(sPartTicks, 0, sizeof(sPartTicks));
    memset(sPartWorst, 0, sizeof(sPartWorst));
    sPartFrames = 0;
}

/*
 * The billboards' share of the update (spritesMs), for the hardware log: the
 * night lamps, the slots gathered from OBJ VRAM (voxel_entities.c: kept
 * while their tiles did not change), the rest of the cards' emission and
 * the packing. Mean and worst a frame, with the slots' counts, over the last
 * VOXEL_PARTS_FRAMES frames that emitted them. Ticks are read only with the
 * profiler on (Port_ProfTick): 0 without, and nothing is logged then anyway.
 */
static void BillboardsLog(float lampsMs, float emitMs)
{
    static float sum[5], worst[5];
    static unsigned frames;
    float parts[5] = { sStats.spritesMs, lampsMs, sStats.gatherMs, emitMs, sStats.packMs };
    VoxelBillboardCounts c;

    for (unsigned i = 0; i < 5; ++i)
    {
        sum[i] += parts[i];
        if (parts[i] > worst[i]) worst[i] = parts[i];
    }
    if (++frames < VOXEL_PARTS_FRAMES)
        return;
    VoxelEntities_TakeCounts(&c);
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL billboards in %u frames: slots %lu, gathered %lu (decoded %lu), "
                 "kept %lu (colours alone %lu), audited %lu (stale %lu); ms mean/worst: all %.2f/%.2f "
                 "lamps %.2f/%.2f gather %.2f/%.2f cards %.2f/%.2f pack %.2f/%.2f",
                 frames, (unsigned long)c.refreshes, (unsigned long)c.raw, (unsigned long)c.regathered,
                 (unsigned long)c.kept, (unsigned long)c.keptPalette, (unsigned long)c.audits,
                 (unsigned long)c.auditStale, sum[0] / frames, worst[0], sum[1] / frames, worst[1],
                 sum[2] / frames, worst[2], sum[3] / frames, worst[3], sum[4] / frames, worst[4]);
    memset(sum, 0, sizeof(sum));
    memset(worst, 0, sizeof(worst));
    frames = 0;
}

/*
 * CtrVoxel_Draw's own CPU time, measure only: in all, and its passes - the
 * chunks' terrain, trees and buildings with their static models; the
 * dynamic buffer's draws (night lamps, reflections, cast shadows, the
 * player's silhouette, the cards); the light in the air (fog and sand banks,
 * motes, rays); the weather's particles. Water is not a pass of its own: it
 * is terrain, animated in the update (animMs), and its reflections are
 * dynamic. Summed over the frame's Draws (both eyes) into sStats for the
 * frame profiler, and logged as mean and worst a frame over the last
 * VOXEL_PARTS_FRAMES frames that drew. Port_ProfTick: nothing without the
 * profiler.
 */
enum { DRAW_ALL, DRAW_CHUNKS, DRAW_CARDS, DRAW_AIR, DRAW_WEATHER, DRAW_PARTS };
static uint32_t sDrawTicks[DRAW_PARTS];

/* At each update, before anything of its frame draws: the last frame's
 * figures are in the record and the log, and this one's begin at 0. */
static void DrawPartsFrame(void)
{
    static float sum[DRAW_PARTS], worst[DRAW_PARTS];
    static unsigned frames;

    if (sDrawTicks[DRAW_ALL] != 0)
    {
        for (unsigned i = 0; i < DRAW_PARTS; ++i)
        {
            float ms = TicksMs(sDrawTicks[i]);

            sum[i] += ms;
            if (ms > worst[i]) worst[i] = ms;
        }
        if (++frames == VOXEL_PARTS_FRAMES)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL draw cpu in %u frames (ms mean/worst a frame, eyes summed): "
                         "all %.2f/%.2f chunks %.2f/%.2f cards %.2f/%.2f air %.2f/%.2f weather %.2f/%.2f",
                         frames, sum[DRAW_ALL] / frames, worst[DRAW_ALL], sum[DRAW_CHUNKS] / frames,
                         worst[DRAW_CHUNKS], sum[DRAW_CARDS] / frames, worst[DRAW_CARDS],
                         sum[DRAW_AIR] / frames, worst[DRAW_AIR], sum[DRAW_WEATHER] / frames,
                         worst[DRAW_WEATHER]);
            memset(sum, 0, sizeof(sum));
            memset(worst, 0, sizeof(worst));
            frames = 0;
        }
    }
    memset(sDrawTicks, 0, sizeof(sDrawTicks));
    sStats.drawMs = sStats.drawChunksMs = sStats.drawCardsMs = 0.0f;
    sStats.drawAirMs = sStats.drawWeatherMs = 0.0f;
}

/* At the end of each Draw: the frame's sums so far. */
static void DrawPartsNote(void)
{
    sStats.drawMs = TicksMs(sDrawTicks[DRAW_ALL]);
    sStats.drawChunksMs = TicksMs(sDrawTicks[DRAW_CHUNKS]);
    sStats.drawCardsMs = TicksMs(sDrawTicks[DRAW_CARDS]);
    sStats.drawAirMs = TicksMs(sDrawTicks[DRAW_AIR]);
    sStats.drawWeatherMs = TicksMs(sDrawTicks[DRAW_WEATHER]);
}

/* What the keys did (see the keys of the chunks' meshes), every
 * VOXEL_KEYS_LOG_FRAMES updates. */
static void KeysLog(void)
{
    static unsigned sAheadLostBefore;
    VoxelAtlasPlanCounts plan;
    unsigned meshes = 0;

    VoxelAtlas_TakePlanCounts(&plan);
    for (unsigned i = 0; i < VOXEL_COLD_MESHES; ++i)
        meshes += sCold[i].used;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL chunk keys in %u frames: builds %u (%.1f ms), missing frames %u, "
                 "ahead lost %u; uv kept %u changed %u, plan kept %u relayouts %u reseeds %u moved %u; cold hits %u "
                 "(%.1f ms) misses %u stale %u origin %u bad %u, stored %u same %u skipped %u evicted %u, yields %u "
                 "(%u meshes) releases %u no-heap %u epoch-global %u, holds %u (%lu KiB); audits %u stale %u void %u",
                 VOXEL_KEYS_LOG_FRAMES, sKeyCounts.builds, sKeyCounts.buildMs, sKeyCounts.missingFrames,
                 sAheadLost - sAheadLostBefore, sKeyCounts.uvKept, sKeyCounts.uvChanged, plan.planned,
                 plan.relayouts, plan.reseeds, plan.moved, sKeyCounts.coldHits, sKeyCounts.coldMs,
                 sKeyCounts.coldMisses, sKeyCounts.coldStale, sKeyCounts.coldOrigin, sKeyCounts.coldBad,
                 sKeyCounts.coldStored, sKeyCounts.coldSame, sKeyCounts.coldSkipped, sKeyCounts.coldEvicted,
                 sKeyCounts.coldYields, sKeyCounts.coldYieldedMeshes,
                 sKeyCounts.coldReleases, sKeyCounts.coldNoHeap, sKeyCounts.epochGlobal, meshes,
                 (unsigned long)(sColdArena.used >> 10), sKeyCounts.audits, sKeyCounts.auditStale,
                 sKeyCounts.auditVoid);
    sAheadLostBefore = sAheadLost;
    memset(&sKeyCounts, 0, sizeof(sKeyCounts));
}

bool CtrVoxel_Update(void)
{
    const VoxelMapInstance *inst;
    float playerX = 0.0f, playerZ = 0.0f;
    float smoothX = 0.0f, smoothZ = 0.0f;
    int mapGroup = -1, mapNum = -1;
    bool mapChanged, cut = false;
    uint64_t started;

    /* The last frame's Draws were reported (3ds_prof.c): this one's begin. */
    DrawPartsFrame();
    if (!sReady || sParked)
        return false;
    /* The game has left the battle: the field is drawn again, as it was. */
    if (sBattle.on && !VoxelBattle_GameInBattle())
        CtrVoxel_EndBattle();
    started = svcGetSystemTick();
    ++sFrame;
    DumpBitsRead();
    if (sFrame % VOXEL_KEYS_LOG_FRAMES == 0)
        KeysLog();

    /* CtrVideo_Present has opened the frame and waited for the previous GPU
     * queue. Only now may the sources of last frame's uploads be reused. */
    sStagingUsed = 0;
    sChunkUploads = 0;
    /* ... and the cold meshes read back then landed, if the time after the
     * last FrameEnd did not do it - every one of an earlier update, written
     * by that same wait, so their staging is this frame's again; none is kept
     * through a battle. */
    if (sBattle.on)
        CtrVoxel_ColdRelease();
    for (unsigned i = 0; i < sColdPendingCount; ++i)
        if (sColdPending[i].frame < sFrame)
        {
            uint64_t landStart = svcGetSystemTick();

            ColdLand();
            sKeyCounts.coldMs += MsSince(landStart);
            break;
        }
    if (sColdPendingCount == 0)
        sStagingTop = 0;
#ifdef CTR_FRAME_DUMP
    /* 1048576: every 1200th update outside a battle, every atlas dropped as
     * a release of atlases out of view drops one, and composed again - a
     * test of the plan and the UV keys over fresh rebuilds. */
    if ((CtrVideo_DumpOff() & 1048576u) && !sBattle.on && sFrame % 1200u == 600u && !AtlasJobBusy()
     && !sAtlasUploadPending)
    {
        unsigned dropped = 0;

        for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
            if (sAtlases[i].valid)
            {
                FreeAtlasTextures(&sAtlases[i]);
                AnimForget(&sAtlases[i]);
                memset(&sAtlases[i].tex, 0, sizeof(sAtlases[i].tex));
                ++dropped;
            }
        sAtlasCapped = false;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL dump test: %u atlases dropped at update %lu", dropped,
                     (unsigned long)sFrame);
    }
#endif
    /* ... and the static meshes replaced while that frame drew them; a
     * converted one that found no room beside the stale mesh that frame drew
     * takes its place now, before anything of this frame draws either. */
    StaticMeshesLateFree();
    StaticBuildPlaceHeld();
    sAtlasUploadPending = false;
    sAtlasCapacityBlocked = false;
    sViewReady = false;
    /* Nothing retires an asset payload between here and the end of the
     * update, so resolved pointers may be reused throughout. */
    VoxelWorld_BeginBatch();
    /* Built again only once what the set is made from has changed: the map,
     * its layout, the map version, a swap's answer (voxel_world.c). */
    VoxelWorld_UpdateInstances();
    ReadAhead();
    inst = VoxelWorld_Instance(0);
    if (inst == NULL)
        return false;
    uint64_t partStart = started;

    NotePart(PART_INSTANCES, &partStart);

    {
        /*
         * A changed map is announced (3ds_map_events.c): hashing all of it
         * every frame to find out cost 0.2 ms on an Old 3DS in every voxel
         * frame. The hash stays, once a second, to catch a write that came
         * by another way; one that does is logged and taken as a change.
         */
        uint32_t version = CtrMap_Version(), signature = InstanceSignature();
        /* The buildings came or went (CtrVoxel_UnparkVram): the lighting's
         * casters with them. */
        bool changed = version != sMapVersion || signature != sInstanceSignature || sBuildingsChanged;

        sBuildingsChanged = false;

        if (changed || sFrame % VOXEL_MAP_CHECK_FRAMES == 0)
        {
            uint32_t digest = VoxelWorld_LiveDigest();

            if (!changed && sDigestKnown && digest != sLiveDigest)
            {
                changed = true;
                if (sUnannounced++ < 50)
                    CtrLog_Write(CTR_LOG_ERROR, "CONTRACT unannounced map change (%lu so far)", (unsigned long)sUnannounced);
            }
            sLiveDigest = digest;
            sDigestKnown = true;
        }
        if (changed)
        {
            /* what stands on it is read off the grid again (voxel_building.h) */
            VoxelBuildings_WorldChanged();
            sMapVersion = version;
            sInstanceSignature = signature;
            /* the belt past the map at its edge's level, or at the base
             * (settings.txt voxel_border_edge=0), read with each new map */
            VoxelMesh_SetBorderFollowsEdge(CtrSettings_GetInt("voxel_border_edge", 1) != 0);
            VoxelBuildings_MapChanged();
            /* what the cards found of the world is found again */
            VoxelEntities_WorldChanged();
            ++sEpoch;
#if CTR_VOXEL_LIGHTING
            VoxelLighting_Reset();
#endif
        }
    }

    /* The window masks take VRAM only while they light anything (GlowWant):
     * none with DAY / NIGHT off, indoors, or from dawn to the afternoon. */
    if (sFrame >= sMeasureRead || sMeasureRead - sFrame > 64u)
    {
        sMeasureGlow = CtrSettings_GetInt("measure_glow", 1) != 0;
        sMeasureLamps = CtrSettings_GetInt("measure_lamps", 1) != 0;
        sMeasurePlain = CtrSettings_GetInt("voxel_plain_vs", 1) != 0;
        sMeasureFusedBlur = CtrSettings_GetInt("voxel_fused_blur", 0) != 0;
        sMeasureScissor = CtrSettings_GetInt("voxel_scissor", 1) != 0;
        sMeasureParts = CtrSettings_GetInt("voxel_parts", 1) != 0;
        sMeasureAb = (unsigned)CtrSettings_GetInt("voxel_ab", 0);
        sMeasureCutEvery = (unsigned)CtrSettings_GetInt("voxel_cut", 0);
        sMeasurePartsEvery = (unsigned)CtrSettings_GetInt("voxel_parts_ab", 0);
        if (sMeasureAb == 0)
            VoxelEntities_SetMemo(-1);
        sMeasureRead = sFrame + 64u;
    }
    /*
     * settings.txt voxel_ab=N: the console tells what each of these is worth
     * by itself. Every N frames the next of five configurations, each adding
     * one to the last - none, the scissor, the plain program, the cards'
     * memo, the fused blur - and a line in the log at each change, by which
     * perf.bin's frames are told apart (the game's frame is its tick).
     */
    if (sMeasureAb != 0)
    {
        static unsigned sAbConfig = ~0u;
        unsigned config = sFrame / sMeasureAb % 5u;

        sMeasureScissor = config >= 1u;
        sMeasurePlain = config >= 2u;
        VoxelEntities_SetMemo(config >= 3u);
        sMeasureFusedBlur = config >= 4u;
        if (config != sAbConfig)
        {
            sAbConfig = config;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL A/B config %u (scissor %d plain %d memo %d fused %d) from game frame %lu",
                         config, sMeasureScissor, sMeasurePlain, config >= 3u, sMeasureFusedBlur,
                         (unsigned long)CtrPlatform_FrameCount());
        }
    }
    /* settings.txt voxel_parts_ab=N: the ground whole and by parts in turns
     * of N frames (VoxelChunkParts), told apart as voxel_ab's. */
    if (sMeasurePartsEvery != 0)
    {
        static unsigned sPartsConfig = ~0u;
        unsigned config = sFrame / sMeasurePartsEvery % 2u;

        sMeasureParts = config != 0;
        if (config != sPartsConfig)
        {
            sPartsConfig = config;
            CtrVideo_GpuOpsFlush();
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL PARTS config %u from game frame %lu", config,
                         (unsigned long)CtrPlatform_FrameCount());
        }
    }
    /*
     * settings.txt voxel_cut=N: every N frames the next of nine pictures, each
     * the whole one less one part - the ground, the trees, the buildings, the
     * right half of the world's pixels (all its vertices still worked), the
     * tilt-shift bands, the bloom, the game's layers over the world - and the
     * last with none of them: what the frame costs with nothing in it. On the
     * console the time the GPU's queue drains at tells each part's share.
     */
    sMeasureCut = 0;
    if (sMeasureCutEvery != 0)
    {
        static const unsigned sCuts[] = {0, CTR_VOXEL_CUT_GROUND, CTR_VOXEL_CUT_TREES, CTR_VOXEL_CUT_BUILDINGS,
            CTR_VOXEL_CUT_HALF, CTR_VOXEL_CUT_DIORAMA, CTR_VOXEL_CUT_BLOOM, CTR_VOXEL_CUT_OVERLAY,
            CTR_VOXEL_CUT_GROUND | CTR_VOXEL_CUT_TREES | CTR_VOXEL_CUT_BUILDINGS | CTR_VOXEL_CUT_DIORAMA
            | CTR_VOXEL_CUT_BLOOM | CTR_VOXEL_CUT_OVERLAY};
        static unsigned sCutConfig = ~0u;
        unsigned config = sFrame / sMeasureCutEvery % 9u;

        sMeasureCut = sCuts[config];
        if (config != sCutConfig)
        {
            sCutConfig = config;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL CUT config %u (bits %02x) from game frame %lu", config, sMeasureCut,
                         (unsigned long)CtrPlatform_FrameCount());
        }
    }
    sGlowWanted = sMeasureGlow && CtrDayNight_Get()->glow > 0.0f;
    NotePart(PART_CHECK, &partStart);

#if CTR_VOXEL_LIGHTING
    {
        const CtrDayNightSun *sun = CtrDayNight_GetSun();

        /*
         * SHADOW QUALITY (OPTIONS), read every few frames: another quality is
         * another signature for every chunk (VoxelLighting_Hash), so they are
         * built again a few a frame as for a new sun, each drawn as it was
         * until then; the job under way was cast the other way and goes.
         */
        static uint32_t sShadowsRead;
        bool readShadows = sFrame >= sShadowsRead || sShadowsRead - sFrame > 8u;

        if (readShadows)
            sShadowsRead = sFrame + 8u;
        if (readShadows
         && VoxelLighting_SetQuality(CtrSettings_VoxelShadowsHigh() ? VOXEL_SHADOWS_HIGH : VOXEL_SHADOWS_LOW))
        {
            ++sEpoch;
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL shadows %s",
                         VoxelLighting_Quality() == VOXEL_SHADOWS_HIGH ? "HIGH" : "LOW");
        }
        if (VoxelLighting_SetSun(sun->dx, sun->dz, sun->strength, sun->key))
        {
            StaticMeshesRelight();
            ++sEpoch;
        }
    }
#endif
    NotePart(PART_SUN, &partStart);

    VoxelWorld_GetLocation(&mapGroup, &mapNum);
    /*
     * Two different positions on purpose. The camera follows the interpolated
     * one, or it lurches a whole tile per step; the view uses the tile-aligned
     * one, because that is the granularity at which the geometry changes.
     */
    VoxelWorld_GetPlayerWorldCoords(&playerX, &playerZ);
    /* In a battle the player's sprite is one of the battle's: its tile,
     * where its ground is drawn (VoxelRelief_ShiftAt). */
    if (sBattle.on)
    {
        smoothX = playerX;
        smoothZ = playerZ + VoxelRelief_ShiftAt(playerX + 0.5f, playerZ + 0.5f);
    }
    else
        VoxelEntities_GetPlayerWorldPos(&smoothX, &smoothZ);

    mapChanged = mapGroup != sMeshMapGroup || mapNum != sMeshMapNum;
    if (mapChanged)
    {
        for (unsigned a = 0; a < VOXEL_ATLAS_SLOTS; ++a)
            sAtlases[a].extendPending = false;
        cut = !HandleMapChange(mapGroup, mapNum, smoothX, smoothZ);
        /* A battle on a map the field never drew: the field's camera is the
         * one the cut just placed on the player. */
        if (sBattle.on)
            sBattle.field = sCamera;
    }
    else if (!sBattle.on)
    {
        VoxelCamera_SetGround(&sCamera, VoxelEntities_PlayerGround(), 0);
        VoxelCamera_Update(&sCamera, smoothX, smoothZ);
    }
    /* The swoop to or from the 2D field, on top of the player's pose. */
    if (!sBattle.on)
        VoxelCamera_Swoop(&sCamera, sSwoop);
    /* The Fly cinematic's high camera, on top of it too. */
    if (!sBattle.on && sFly.active)
        FlyCamera();
    if (sBattle.on)
    {
        if (!sBattle.chosen)
        {
            uint64_t searchStart = svcGetSystemTick();
            float ms;

            if (!sBattle.begun)
                VoxelBattle_BeginStage();
            sBattle.begun = true;
            sBattle.chosen = VoxelBattle_StepStage(VOXEL_BATTLE_STAGE_CELLS, VOXEL_BATTLE_STAGE_SPOTS,
                                                   &sBattle.targetX, &sBattle.targetZ, &sBattle.ground);
            ms = MsSince(searchStart);
            ++sBattle.searchFrames;
            sBattle.searchMs += ms;
            if (ms > sBattle.searchWorstMs)
                sBattle.searchWorstMs = ms;
            if (sBattle.chosen)
                CtrLog_Write(CTR_LOG_VIDEO, "VOXEL battle stage searched in %u frames, %.1f ms "
                             "(worst frame %.1f)", (unsigned)sBattle.searchFrames, sBattle.searchMs,
                             sBattle.searchWorstMs);
        }
        BattleCamera();
        /* Nobody moves in a battle, and nothing is led towards. */
        TrackMotion(sLastX, sLastZ, true);
    }
    else
    {
        /* A cut moves the player; a crossing only the coordinates, and
         * HandleMapChange has moved the last position with them. */
        TrackMotion(smoothX, smoothZ, cut);
    }
    NotePart(PART_PLACE, &partStart);
    /* Heading off the current map into the one beside it (see
     * VOXEL_CROSSING_RESERVE_MS)? */
    {
        const VoxelMapInstance *here = VoxelWorld_Instance(0);
        int dx = sVelocityX > 0.02f ? 1 : sVelocityX < -0.02f ? -1 : 0;
        int dz = sVelocityZ > 0.02f ? 1 : sVelocityZ < -0.02f ? -1 : 0;
        float aheadX = playerX + dx * (VOXEL_CROSSING_NEAR + 1.0f);
        float aheadZ = playerZ + dz * (VOXEL_CROSSING_NEAR + 1.0f);
        const VoxelMapInstance *next = (dx || dz)
            ? VoxelWorld_GetInstanceAt((int)floorf(aheadX), (int)floorf(aheadZ)) : NULL;

        sCrossingSoon = next != NULL && next != here && !sBattle.on;
    }
    /* Recorded after the shift, for the next crossing. */
    RememberInstanceOrigins();
    sMeshMapGroup = mapGroup;
    sMeshMapNum = mapNum;
    NotePart(PART_CROSSING, &partStart);
    LogParts();

    sStats.worldMs = (float)((svcGetSystemTick() - started) * 1000.0 / SYSCLOCK_ARM11);
    {
        uint64_t streamStart = svcGetSystemTick();

        StreamPages();
        sStats.streamMs = MsSince(streamStart);
    }
    /* The view is visited around what is looked at: the stage, in a battle. */
    if (sBattle.on)
        UpdateView(sCamera.targetX, sCamera.targetZ, cut);
    else
        UpdateView(playerX, playerZ, cut);
    /* The pages drawn from first, then those of every map in view - built
     * or not yet - and last, if there is room, of the maps in the ring. */
    for (unsigned d = 0; d < sDrawCount; ++d)
        if (sDraws[d].chunk->count > sDraws[d].chunk->buildingFirst
         || sDraws[d].chunk->staticCount != 0)
            WantPage(sDraws[d].chunk->buildingPage, VOXEL_PAGE_KEEP_VIEW, false);
    if (sHaveBuildings)
        for (unsigned ring = 0; ring < 2; ++ring)
            for (unsigned i = 0; i < VoxelWorld_InstanceCount(); ++i)
            {
                const VoxelMapInstance *map = VoxelWorld_Instance(i);
                int margin = ring ? VOXEL_PREFETCH : 0;
                /* (where its rows are drawn: DrawnRows) */
                int y0 = map->originY + DrawnRows(map), y1 = y0 + map->height;
                bool near = Overlaps(map->originX, y0, map->originX + map->width, y1,
                                     sViewRect[0] - margin, sViewRect[1] - margin,
                                     sViewRect[2] + margin, sViewRect[3] + margin);
                bool inView = Overlaps(map->originX, y0, map->originX + map->width, y1,
                                       sViewRect[0], sViewRect[1], sViewRect[2], sViewRect[3]);

                if (!near || (ring != 0) == inView)
                    continue;
                WantPage(VoxelBuildings_PageOf(map), ring ? VOXEL_PAGE_KEEP_RING : VOXEL_PAGE_KEEP_VIEW,
                         true);
            }
    {
        uint64_t animStart = svcGetSystemTick();

        /*
         * The passes read the tiles from BG VRAM, which in a battle holds the
         * battle's: a pass still pending when one began redrew the sea from
         * them - transparent, the whole battle, as no tile animation runs
         * there to draw it again (Route 110's canal, the Cycling Road's sea,
         * port.log 2026-10-15). Dropped instead: back on the field, the
         * tileset animation writes every frame again and each pass with it.
         */
        if (VoxelBattle_GameInBattle())
            DropTileAnimation();
        else
        {
            AnimateTiles(inst);
            AnimateBuildings(inst);
        }
        sStats.animMs = MsSince(animStart);
    }

    /*
     * Billboards last, rebuilt every frame: they interpolate between tiles, so
     * they move on frames where nothing else does. Built around the camera's
     * tile, which keeps their coordinates small enough to pack. Nobody
     * stands in a battle's scenery: the game's sprites are the battle's,
     * drawn over it.
     */
    if (sBattle.on)
    {
        sDynamicX = (int)floorf(sCamera.targetX);
        sDynamicZ = (int)floorf(sCamera.targetZ);
        sSpriteVertices = sReflectionVertices = 0;
        sLampVertices = sLampGroundVertices = 0;
        sLampReady = false;
        LampsRelease(false);
        sStats.reflections = 0;
#if CTR_VOXEL_LIGHTING
        sShadowVertices = 0;
#endif
        sStats.spritesMs = sStats.gatherMs = sStats.packMs = 0.0f;
    }
    else
    {
        uint64_t spritesStart = svcGetSystemTick();
        VoxelBuilder sprites, reflections;
        VoxelBuilder *shadowBuilder = NULL;
        VoxelVertex *dynamic = sDynamicScratch;
#if CTR_VOXEL_LIGHTING
        VoxelBuilder shadows;
#endif

        uint32_t lampsTicks, emitStart, emitTicks, packStart, packTicks;

        sDynamicX = (int)floorf(sCamera.targetX);
        sDynamicZ = (int)floorf(sCamera.targetZ);

        lampsTicks = Port_ProfTick();
        UpdateNightLamps();
        lampsTicks = Port_ProfTick() - lampsTicks;
        VoxelBuilder_Init(&sprites, dynamic, VOXEL_SPRITE_RESERVE);
        VoxelBuilder_SetOrigin(&sprites, sDynamicX, sDynamicZ);
        VoxelBuilder_Init(&reflections, dynamic + VOXEL_REFLECTION_FIRST,
                          VOXEL_REFLECTION_RESERVE);
        VoxelBuilder_SetOrigin(&reflections, sDynamicX, sDynamicZ);
#if CTR_VOXEL_LIGHTING
        VoxelBuilder_Init(&shadows, dynamic + VOXEL_SHADOW_FIRST, VOXEL_SHADOW_RESERVE);
        VoxelBuilder_SetOrigin(&shadows, sDynamicX, sDynamicZ);
        shadowBuilder = &shadows;
#endif
#ifdef CTR_FRAME_DUMP
        /* nocache.txt bit 65536: every slot gathered, as before (A/B frames).
         * Read here: voxel_entities.c is a game unit, built without the
         * audit builds' defines. */
        VoxelEntities_SetKeep(!(CtrVideo_DumpOff() & 65536));
#endif
        emitStart = Port_ProfTick();
        VoxelEntities_Emit(&sprites, (uint16_t *)sSpriteAtlas.data, &sCamera, shadowBuilder, &reflections);
        packStart = Port_ProfTick();
        emitTicks = packStart - emitStart;
        sReflectionVertices = reflections.count;
        sStats.reflections = reflections.count / 6u;   /* quads (EmitReflection: a strip each) */
        PackDynamic(dynamic + VOXEL_REFLECTION_FIRST, reflections.count,
                    sDynamic + VOXEL_REFLECTION_FIRST);
        packTicks = Port_ProfTick() - packStart;
        if (reflections.dropped != 0)
        {
            ++sStats.errors;
            CtrLog_Write(CTR_LOG_ERROR, "VOXEL: reflection staging overflow");
        }
        sSpriteVertices = sprites.count;
        packStart = Port_ProfTick();
        PackDynamic(dynamic, sprites.count, sDynamic);
#if CTR_VOXEL_LIGHTING
        sShadowVertices = shadows.count;
        PackDynamic(dynamic + VOXEL_SHADOW_FIRST, shadows.count, sDynamic + VOXEL_SHADOW_FIRST);
#endif
        packTicks += Port_ProfTick() - packStart;
#if CTR_VOXEL_LIGHTING
        if (shadows.dropped != 0)
        {
            ++sStats.errors;
            CtrLog_Write(CTR_LOG_ERROR, "VOXEL: cast shadow staging overflow");
        }
#endif
        if (sprites.dropped != 0 && !sOverflowReported)
        {
            sOverflowReported = true;
            ++sStats.errors;
            CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no staging room for billboards");
        }
        sStats.spritesMs = (float)((svcGetSystemTick() - spritesStart) * 1000.0 / SYSCLOCK_ARM11);
        sStats.gatherMs = TicksMs(VoxelEntities_TakeGatherTicks());
        sStats.packMs = TicksMs(packTicks);
        BillboardsLog(TicksMs(lampsTicks), TicksMs(emitTicks) - sStats.gatherMs);
    }
    ReportPackErrors("the billboards around", sDynamicX, sDynamicZ, mapGroup, mapNum);


    sStats.chunks = 0;
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i)
        if (sChunks[i].used)
            ++sStats.chunks;
    sStats.vertices += sSpriteVertices + sReflectionVertices + sLampVertices + sLampGroundVertices;
#if CTR_VOXEL_LIGHTING
    sStats.vertices += sShadowVertices;
#endif
    sStats.updateMs = (float)((svcGetSystemTick() - started) * 1000.0 / SYSCLOCK_ARM11);
    if (sStats.updateMs > sStats.updatePeakMs)
        sStats.updatePeakMs = sStats.updateMs;
    /* A map on screen without its atlas: the frame is the 2D picture (or
     * the battle's 2D scenery). Logged once the run of them is over, so a
     * console log tells a flash of 2D from the world switched off. */
    {
        static unsigned sBlockedRun;

        if (sAtlasCapacityBlocked)
            ++sBlockedRun;
        else if (sBlockedRun != 0)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: %u frame%s drawn flat for want of an atlas's VRAM%s",
                         sBlockedRun, sBlockedRun == 1 ? "" : "s", sBattle.on ? " (battle)" : "");
            sBlockedRun = 0;
        }
    }
    return sDrawCount != 0 && !sAtlasCapacityBlocked
        && !(sWarmupUntil != 0 && sStats.chunksMissing != 0);
}

/* An atlas holding VRAM that nothing shows or builds: not the job's, not in
 * view, not the pair of `current`. */
static bool AtlasIdle(const VoxelAtlasSlot *slot, const VoxelMapInstance *current)
{
    return slot->tex.data != NULL && !(AtlasJobBusy() && slot == sAtlasJob.slot) && !AtlasInView(slot)
        && !(current != NULL && slot->primaryTileset == current->primaryTileset
             && slot->secondaryTileset == current->secondaryTileset);
}

static void AtlasDrop(VoxelAtlasSlot *slot)
{
    FreeAtlasTextures(slot);
    AnimForget(slot);
    memset(&slot->tex, 0, sizeof(slot->tex));
    slot->valid = false;
}

/*
 * Called by the 2D compositor, on a frame the overworld is not drawing, when
 * it cannot find VRAM for its depth planes. The atlases of every tileset pair
 * but the current map's are released; they are composed again, over frames,
 * when their maps are next on screen.
 */
bool CtrVoxel_IsWarmingUp(void)
{
    return sReady && sFrame < sWarmupUntil && !sAtlasCapacityBlocked;
}

bool CtrVoxel_WideBackdrop(uint32_t *rgba)
{
    if (!sReady || !sWideView || sMapFill == 0)
        return false;
    *rgba = sMapFill;
    return true;
}

unsigned long CtrVoxel_ReleaseIdleVram(void)
{
    const VoxelMapInstance *current = VoxelWorld_Instance(0);
    unsigned long freed = 0;
    bool currentKept = false;

    if (!sReady)
        return 0;
    /* A job extending the current pair's atlas keeps going: that slot stays. */
    if (!(sAtlasJob.slot != NULL && current != NULL && sAtlasJob.slot->valid
       && sAtlasJob.slot->primaryTileset == current->primaryTileset
       && sAtlasJob.slot->secondaryTileset == current->secondaryTileset))
        AtlasJobCancel();
    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
    {
        VoxelAtlasSlot *slot = &sAtlases[i];

        if (slot->tex.data == NULL)
            continue;
        if (current != NULL && slot->valid && slot->primaryTileset == current->primaryTileset
         && slot->secondaryTileset == current->secondaryTileset)
        {
            currentKept = true;
            continue;
        }
        /* The battle over the world shows the neighbours' too, and the field
         * comes back to the same view: rebuilt then, it came back in 2D. */
        if (sBattle.on && slot->valid && AtlasInView(slot))
            continue;
        unsigned pages = 0;
        for (unsigned p = 0; p < VOXEL_ATLAS_PAGES; ++p)
            pages += AtlasTex(slot, p)->data != NULL && !AtlasPageBorrowed(AtlasTex(slot, p));
        AtlasDrop(slot);
        freed += pages * VOXEL_ATLAS_PIXELS * sizeof(uint16_t);
    }
    if (freed != 0)
    {
        sAtlasCapped = false;
        /* The current map's atlas is kept, so its chunks are too: the
         * warm-up (in-frame builds, every signature hashed again) is only
         * for a map whose atlas went with the others. */
        sResumeWarmup = !currentKept;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: %lu KiB of idle atlases released (VRAM free=%lu)",
                     freed >> 10, (unsigned long)vramSpaceFree());
    }
    return freed;
}

/*
 * The battle's scene surface, a menu's layers: one block of VRAM, which one
 * atlas or two off screen usually make. Released all at once, the atlases of
 * the maps beside the player went too - a battle's start composed them again
 * under its transition (port.log 2026-10-21: two atlases and their chunks
 * rebuilt there, a third that no longer fitted for the whole battle), and
 * the field came back without them. In view or being composed, an atlas
 * stays unless nothing else makes the room.
 */
unsigned long CtrVoxel_ReleaseIdleVramFor(uint32_t bytes, bool inView)
{
    const VoxelMapInstance *current = VoxelWorld_Instance(0);
    unsigned long freed = 0;

    if (!sReady)
        return 0;
    while (CtrGpuMem_LargestVram() < bytes)
    {
        VoxelAtlasSlot *victim = NULL;
        unsigned victimPages = 0;

        for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
        {
            VoxelAtlasSlot *slot = &sAtlases[i];
            unsigned pages = 0;

            if (!AtlasIdle(slot, current))
                continue;
            for (unsigned p = 0; p < VOXEL_ATLAS_PAGES; ++p)
                pages += AtlasTex(slot, p)->data != NULL && !AtlasPageBorrowed(AtlasTex(slot, p));
            if (pages != 0 && (victim == NULL || slot->stamp < victim->stamp))
            {
                victim = slot;
                victimPages = pages;
            }
        }
        if (victim == NULL)
            break;
        AtlasDrop(victim);
        freed += victimPages * VOXEL_ATLAS_PIXELS * sizeof(uint16_t);
    }
    if (freed != 0)
    {
        sAtlasCapped = false;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: %lu KiB of atlases out of view released for %lu KiB (VRAM free=%lu)",
                     freed >> 10, (unsigned long)(bytes >> 10), (unsigned long)vramSpaceFree());
    }
    if (!inView || CtrGpuMem_LargestVram() >= bytes)
        return freed;
    return freed + CtrVoxel_ReleaseIdleVram();
}

/*
 * One block of `bytes` from one atlas out of view, and only if its going makes
 * it (CtrGpuMem_LargestVramWithout): the oldest of those that do. Given the
 * oldest atlas as CtrVoxel_ReleaseIdleVramFor does, the VRAM was left in
 * pieces still and the atlas lost for nothing (a 2D battle's blending layer,
 * 2026-10-28); on hardware the one atlas between two gaps would have made the
 * 512 KiB (port.log 2026-11-01). Nothing goes when none makes it alone.
 */
unsigned long CtrVoxel_ReleaseAtlasMaking(uint32_t bytes)
{
    const VoxelMapInstance *current = VoxelWorld_Instance(0);
    VoxelAtlasSlot *victim = NULL;
    unsigned victimPages = 0;

    if (!sReady || CtrGpuMem_LargestVram() >= bytes)
        return 0;
    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
    {
        VoxelAtlasSlot *slot = &sAtlases[i];
        void *gone[VOXEL_ATLAS_PAGES];
        unsigned pages = 0;

        if (!AtlasIdle(slot, current))
            continue;
        /* A page borrowed from the pages' arena gives no VRAM back. */
        for (unsigned p = 0; p < VOXEL_ATLAS_PAGES; ++p)
            if (AtlasTex(slot, p)->data != NULL && !AtlasPageBorrowed(AtlasTex(slot, p)))
                gone[pages++] = AtlasTex(slot, p)->data;
        if (pages != 0 && (victim == NULL || slot->stamp < victim->stamp)
            && CtrGpuMem_LargestVramWithout(gone, pages) >= bytes)
        {
            victim = slot;
            victimPages = pages;
        }
    }
    if (victim == NULL)
        return 0;
    AtlasDrop(victim);
    sAtlasCapped = false;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: an atlas out of view released, making a %lu KiB block (VRAM free=%lu)",
                 (unsigned long)(bytes >> 10), (unsigned long)vramSpaceFree());
    return victimPages * VOXEL_ATLAS_PIXELS * sizeof(uint16_t);
}

/*
 * The voxel world's VRAM while the world is off (CtrVideo, VoxelVramManage).
 * The two arenas are 2.25 MiB, claimed whole at start-up; with the 2D field on
 * they held 37% of VRAM for nothing, and the 2D screens lost their 3D for it:
 * the depth planes and the battle scene did not fit beside the layers (F0 on
 * an Old 3DS: 40% of the field's frames without planes, the first nine
 * seconds of a battle composed whole per eye). Parked, everything the world
 * keeps in VRAM goes - arenas, chunks, building pages, atlases - and comes
 * back as for a new map. The caller parks only after the world has not been
 * drawn for a while: nothing queued can still read it.
 */
bool CtrVoxel_HoldsVram(void)
{
    return sReady && !sParked;
}

void CtrVoxel_ParkVram(void)
{
    if (!sReady || sParked)
        return;
    LampsRelease(true);
    StreamStop();
    AtlasJobCancel();
    JobCancel();
    ReleaseAllChunks();
    /* (and their cold meshes: the world goes, and its memory with it) */
    ColdDrop(true);
    /* Linear memory: converted again as the chunks place them (JOB_STATICS). */
    StaticMeshesFree();
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
        if (sPageSlots[i].tex.data != NULL)
            PageFree(&sPageSlots[i]);
    for (unsigned i = 0; i < VOXEL_ATLAS_SLOTS; ++i)
    {
        FreeAtlasTextures(&sAtlases[i]);
        AnimForget(&sAtlases[i]);
        memset(&sAtlases[i].tex, 0, sizeof(sAtlases[i].tex));
    }
    sAtlasCapped = false;
    /* A gap lent to the battle comes back before the arena goes (the
     * borrower's target deleted first, after the GPU is done with it). */
    if (sPagesLent != NULL)
        CtrVideo_PagesGapRevoke();
    VoxelArena_Init(&sChunkArena, NULL, 0, 16, sChunkPieces, VOXEL_CHUNK_SLOTS + 1);
    VoxelArena_Init(&sPageArena, NULL, 0, 128, sPagePieces, VOXEL_PAGE_PIECES);
    if (sPageBlock != NULL)
        vramFree(sPageBlock);
    if (sChunkBlock != NULL)
        vramFree(sChunkBlock);
    sPageBlock = sChunkBlock = NULL;
    sHaveBuildings = false;
    /* Back as for a map never seen: HandleMapChange builds it again. */
    sMeshMapGroup = sMeshMapNum = -1;
    sParked = true;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: VRAM parked while the world is off (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
}

/* False if the chunk arena does not fit, or the pages' when the buildings
 * are loaded: the caller frees its own surfaces first (they come back on
 * demand) and tries again later. */
#define VOXEL_UNPARK_PAGE_TRIES 5u
static unsigned sUnparkPageFails;

bool CtrVoxel_UnparkVram(void)
{
    DumpBitsRead();
    if (!sReady || !sParked)
        return sReady;
    sChunkBlock = vramAlloc(VOXEL_CHUNK_VRAM_BUDGET);
    if (sChunkBlock == NULL)
        return false;
    sPageBlock = vramAlloc(VOXEL_PAGE_VRAM_BUDGET);
    /* Loaded, the buildings' cells are their models' (VoxelBuildings_CellAt):
     * without the pages to draw them the houses were left out, the ground
     * under them alone. The whole world waits for the room instead - for a
     * few tries (the caller's, VOXEL_UNPARK_RETRY_FRAMES apart); then it comes
     * back without the buildings, unloaded as without buildings.bin, so their
     * cells are the flat drawing again. Not for good: while the world is up
     * without them, the caller asks for the pages again (CtrVoxel_ClaimPages)
     * and counts every frame without them as a denial of the world's VRAM -
     * since the arenas are claimed on the first frame the world is wanted,
     * this can be the very first claim of the session, not only one after a
     * park, and with the option on nothing parks the world again. */
    if (sPageBlock == NULL && sBuildingsLoaded)
    {
        if (++sUnparkPageFails < VOXEL_UNPARK_PAGE_TRIES)
        {
            vramFree(sChunkBlock);
            sChunkBlock = NULL;
            return false;
        }
        /* The stream started at start-up may be running (CtrVoxel_Init):
         * stopped before the tables it reads pages by are freed. */
        if (sStream.buffer != NULL)
            StreamStop();
        VoxelBuildings_Shutdown();
        sBuildingsLoaded = false;
        sBuildingsDropped = sBuildingsChanged = true;
        VoxelEntities_WorldChanged();
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no VRAM for the building pages in %u tries (largest=%lu):"
                     " the world comes back without its buildings", sUnparkPageFails,
                     (unsigned long)CtrGpuMem_LargestVram());
    }
    else if (sPageBlock != NULL && sBuildingsDropped)
    {
        sBuildingsLoaded = VoxelBuildings_Init();
        sBuildingsDropped = !sBuildingsLoaded;
        sBuildingsChanged = true;
        VoxelEntities_WorldChanged();
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: building pages have room again, buildings %s",
                     sBuildingsLoaded ? "loaded" : "still unloaded");
    }
    sUnparkPageFails = 0;
    {
        uint32_t arena = VOXEL_CHUNK_VRAM_BUDGET;

#ifdef CTR_FRAME_DUMP
        /* (bit 262144: a third of it, for the chunks to be evicted as on a
         * console's crowded routes - see the keys above) */
        if (sPressure)
            arena = VOXEL_CHUNK_VRAM_BUDGET / 3u;
#endif
        VoxelArena_Init(&sChunkArena, sChunkBlock, arena, 16, sChunkPieces, VOXEL_CHUNK_SLOTS + 1);
    }
    sAheadNoRoomBytes = 0; /* (an empty arena: room for anything) */
    VoxelArena_Init(&sPageArena, sPageBlock, sPageBlock != NULL ? VOXEL_PAGE_VRAM_BUDGET : 0,
                    128, sPagePieces, VOXEL_PAGE_PIECES);
    PageRetriesForget();
    sHaveBuildings = sPageBlock != NULL && sBuildingsLoaded;
    /* The first claim finds the stream started at start-up (CtrVoxel_Init);
     * one after a park, stopped with the world (CtrVoxel_ParkVram). Without
     * the pages there is nothing for it to read, as when it never started. */
    if (sHaveBuildings && sStream.buffer == NULL)
        StreamStart();
    else if (!sHaveBuildings && sStream.buffer != NULL)
        StreamStop();
    sParked = false;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: VRAM back%s (VRAM free=%lu)",
                 sPageBlock != NULL ? "" : ", without the building pages", (unsigned long)vramSpaceFree());
    return true;
}

/*
 * The world up without the building pages it gave up on (CtrVoxel_UnparkVram,
 * its last try): their arena is still wanted. Not while buildings.bin is
 * absent, nor once the buildings failed to load again - nothing would draw
 * from it.
 */
bool CtrVoxel_PagesMissing(void)
{
    return sReady && !sParked && sPageBlock == NULL && sBuildingsDropped;
}

/*
 * The pages' arena claimed with the world up, outside the frame, and the
 * buildings loaded again with it: the same as an unpark that finds the room,
 * without parking the world for it. The chunks built meanwhile hold the
 * houses' flat drawing; the buildings' presence is part of what a chunk is
 * made from (SiteHash), and the change bumps the world's epoch
 * (sBuildingsChanged), so they are found stale and built again a few a frame,
 * drawn as they are until their turn - as for a map written to. The job in
 * flight was started without the models' phase, and goes. True once nothing
 * is missing any more (also when the buildings would not load again: then
 * the world stays without them, as without buildings.bin, and the pages are
 * not asked for again); false while there is no room.
 */
bool CtrVoxel_ClaimPages(void)
{
    if (!CtrVoxel_PagesMissing())
        return true;
    sPageBlock = vramAlloc(VOXEL_PAGE_VRAM_BUDGET);
    if (sPageBlock == NULL)
        return false;
    sBuildingsLoaded = VoxelBuildings_Init();
    sBuildingsDropped = false;
    if (!sBuildingsLoaded)
    {
        vramFree(sPageBlock);
        sPageBlock = NULL;
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: room for the building pages again, but the buildings did not "
                     "load; the world stays without them");
        return true;
    }
    JobCancel();
    VoxelArena_Init(&sPageArena, sPageBlock, VOXEL_PAGE_VRAM_BUDGET, 128, sPagePieces, VOXEL_PAGE_PIECES);
    PageRetriesForget();
    sHaveBuildings = true;
    sBuildingsChanged = true;
    VoxelEntities_WorldChanged();
    if (sStream.buffer == NULL)
        StreamStart();
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: building pages claimed with the world up, buildings back (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
    return true;
}

void CtrVoxel_ArenaUse(uint32_t *chunkUsed, uint32_t *chunkLargest, uint32_t *pageUsed, uint32_t *pageLargest)
{
    *chunkUsed = sChunkArena.used;
    *chunkLargest = sChunkBlock != NULL ? VoxelArena_LargestGap(&sChunkArena) : 0;
    *pageUsed = sPageArena.used;
    *pageLargest = sPageBlock != NULL ? VoxelArena_LargestGap(&sPageArena) : 0;
}

/* â”€â”€ Draw â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/*
 * Folds "render into the top-left 400x240 of a 512x256 surface" into the
 * projection: a clip-space scale and bias, applied before the perspective
 * divide so it costs nothing at run time.
 */
static void FitToLogicalSurface(C3D_Mtx *mtx)
{
    float sx = CTR_GAME_WIDTH / VOXEL_SURFACE_W;
    float tx = sx - 1.0f;
    float sy = CTR_GAME_HEIGHT / VOXEL_SURFACE_H;
    float ty = 1.0f - sy;

    for (int i = 0; i < 4; ++i)
    {
        mtx->r[0].c[i] = sx * mtx->r[0].c[i] + tx * mtx->r[3].c[i];
        mtx->r[1].c[i] = sy * mtx->r[1].c[i] + ty * mtx->r[3].c[i];
    }
}

/*
 * A whole tile near the camera, the same for every draw of a frame: modelView
 * carries it, and each draw's own corner goes to the shader as whole tiles
 * from it (`origin`, added to the position before the matrix). The edge two
 * chunks share then comes out of one matrix as the same numbers on both
 * sides. With the corner folded into each chunk's matrix, the console's
 * 24-bit floats rounded the two matrices apart and the seam - a flat row on
 * screen - let the background through for a frame here and there while
 * walking, a thin dark or light line across the ground.
 */
static int sDrawAnchorX, sDrawAnchorZ;
/* The Draw's view moved to the anchor: the model-view of every draw placed
 * by `origin` (SetModelView), worked out once a Draw. */
static C3D_Mtx sDrawModelView;

/*
 * Each draw's state, sent only where it differs from what is already set.
 * Citro3D uploads a uniform before the next draw whenever it is written, even
 * with the value it holds, and C3D_GetBufInfo has it send the whole vertex
 * buffer table again (some forty command words). Every chunk draw wrote both:
 * the same model-view each time, as only `origin` differs, and its own
 * buffer. So a value is first compared with what citro3d holds for it - its
 * uniform array, its buffer table - and written only if it differs: what
 * citro3d holds it has sent, or sends before the next draw, so every draw
 * sees exactly the values it saw before, from fewer commands. That holds only
 * while nothing but those arrays reaches the registers, and binding a shader
 * program loads its constants straight into theirs: the 2D compositor's
 * programs may keep constants where these uniforms are. So the first write
 * of each in a Draw is made regardless, and again after the weather's program
 * has been bound (sDrawSet, CtrVoxel_Draw).
 */
enum
{
    DRAW_SET_MODELVIEW = 1u << 0,
    DRAW_SET_ORIGIN = 1u << 1,
    DRAW_SET_DAPPLE_U = 1u << 2,
    DRAW_SET_DAPPLE_V = 1u << 3,
    DRAW_SET_BUFFERS = 1u << 4,
};
static unsigned sDrawSet;
static uint32_t sDrawSerial;         /* CtrVoxel_Draw calls, counted */
static C3D_BufInfo *sDrawBuffers;    /* citro3d's own table, read only to compare */
static C3D_BufInfo sBuffersFor;      /* the table BindVertices makes for: */
static const void *sBuffersVertices;

/* What the draws asked for and what was already set, for the log line
 * (CtrVoxel_Draw): the commands each frame no longer sends. */
typedef struct
{
    unsigned modelViews, origins, dapples, buffers, finds, pages;
} DrawSetupCounts;
static DrawSetupCounts sSetupAsked, sSetupKept;

/*
 * The plain program takes projection * modelView in `projection` (one matrix
 * a vertex rather than two) and still reads modelView's third row for the
 * haze, so both are uploaded together and every write of either goes through
 * here, which remembers them: the folded matrix is made only when one of them
 * is written, never per draw. With any other program bound the two are
 * uploaded as they are.
 */
static C3D_Mtx sProjectionNow, sModelViewNow;
static bool sPlainBound;  /* the plain program is the one bound */
static bool sPlainDraw;   /* this Draw's program (BindWorldProgram) */

static void FoldProjection(void)
{
    C3D_Mtx folded;

    Mtx_Multiply(&folded, &sProjectionNow, &sModelViewNow);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniProjection, &folded);
}

static void SetProjection(const C3D_Mtx *projection)
{
    sProjectionNow = *projection;
    if (sPlainBound)
        FoldProjection();
    else
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniProjection, projection);
}

static void UploadModelView(const C3D_Mtx *model)
{
    sModelViewNow = *model;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniModelView, model);
    if (sPlainBound)
        FoldProjection();
}

/*
 * The world's program, the plain one where the Draw has no shadow map. Binding
 * leaves the projection register as another program last had it, so a plain
 * program's is folded again from what is remembered.
 */
static void BindWorldProgram(bool plain)
{
    sPlainDraw = sPlainBound = plain && sHavePlainProgram;
    C3D_BindProgram(sPlainBound ? &sPlainProgram : &sProgram);
    if (sPlainBound)
        FoldProjection();
}

/* A vertex uniform of `count` registers, unless it already holds `value`. */
static bool UniformKept(unsigned flag, int id, const C3D_FVec *value, int count)
{
    if ((sDrawSet & flag) && memcmp(&C3D_FVUnif[GPU_VERTEX_SHADER][id], value,
                                    (size_t)count * sizeof(C3D_FVec)) == 0)
        return true;
    sDrawSet |= flag;
    return false;
}

/* One register, as C3D_FVUnifSet sets it; whether it already held that. */
static bool SetUniform(unsigned flag, int id, float x, float y, float z, float w)
{
    C3D_FVec value;

    /* by name: a C3D_FVec lies in memory w first */
    value.x = x;
    value.y = y;
    value.z = z;
    value.w = w;
    if (UniformKept(flag, id, &value, 1))
        return true;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, id, x, y, z, w);
    return false;
}

static void SetOrigin(float x, float z)
{
    ++sSetupAsked.origins;
    if (SetUniform(DRAW_SET_ORIGIN, sUniOrigin, x, 0.0f, z, 0.0f))
        ++sSetupKept.origins;
}

static void SetModelViewMatrix(const C3D_Mtx *model)
{
    ++sSetupAsked.modelViews;
    if (UniformKept(DRAW_SET_MODELVIEW, sUniModelView, model->r, 4))
        ++sSetupKept.modelViews;
    else
        UploadModelView(model);
}

/* A draw measured from (worldX, worldZ), in whole tiles, from the Draw's view. */
static void SetModelView(int worldX, int worldZ)
{
    SetModelViewMatrix(&sDrawModelView);
    SetOrigin((float)(worldX - sDrawAnchorX), (float)(worldZ - sDrawAnchorZ));
}

/*
 * Citro3D's table set only through C3D_GetBufInfo, which is what marks it to
 * be sent: written through a pointer kept from an earlier call, it was never
 * sent again and every draw after the first read the first buffer's
 * vertices. The kept pointer is only ever read here, to compare.
 */
static void BindVertices(const VoxelGpuVertex *vertices)
{
    if (vertices != sBuffersVertices)
    {
        BufInfo_Init(&sBuffersFor);
        BufInfo_Add(&sBuffersFor, vertices, sizeof(VoxelGpuVertex), 2, 0x10);
        sBuffersVertices = vertices;
    }
    ++sSetupAsked.buffers;
    if ((sDrawSet & DRAW_SET_BUFFERS) && sDrawBuffers != NULL
     && memcmp(sDrawBuffers, &sBuffersFor, sizeof(sBuffersFor)) == 0)
    {
        ++sSetupKept.buffers;
        return;
    }
    sDrawBuffers = C3D_GetBufInfo();
    if (sDrawBuffers == NULL)
        return;
    *sDrawBuffers = sBuffersFor;
    sDrawSet |= DRAW_SET_BUFFERS;
}

/*
 * The chunks' vertices all lie in their block of VRAM (sChunkBlock), so one
 * table can serve every chunk: the draw's first vertex counted from a base in
 * the block rather than from the chunk's own start. The base is a multiple of
 * VOXEL_DRAW_STEP vertices and reaches VOXEL_DRAW_REACH of them - the 65536 a
 * 16-bit index names, never more, whatever width the console counts vertices
 * in (a chunk draws from 0 up to some 9000 otherwise). The one in use is
 * kept while it reaches the chunk, so the table is sent again only where the
 * draws cross from one base to the next. The first vertex to draw from.
 */
#define VOXEL_DRAW_REACH 65536u
#define VOXEL_DRAW_STEP 32768u
_Static_assert(VOXEL_CHUNK_SCRATCH <= VOXEL_DRAW_REACH - VOXEL_DRAW_STEP,
               "a chunk starting anywhere in a step is reached from its base");
static unsigned sDrawBase; /* in vertices from sChunkBlock */

static int BindChunkVertices(const VoxelChunk *chunk, unsigned first, unsigned count)
{
    uintptr_t block = (uintptr_t)sChunkBlock, at = (uintptr_t)chunk->vram;
    unsigned start;

    if (sChunkBlock == NULL || at < block || at - block >= VOXEL_CHUNK_VRAM_BUDGET
     || (at - block) % sizeof(VoxelGpuVertex) != 0)
    {
        BindVertices(chunk->vram);
        return (int)first;
    }
    start = (unsigned)((at - block) / sizeof(VoxelGpuVertex));
    if (sBuffersVertices != (const VoxelGpuVertex *)sChunkBlock + sDrawBase
     || start < sDrawBase || start - sDrawBase + first + count > VOXEL_DRAW_REACH)
        sDrawBase = start / VOXEL_DRAW_STEP * VOXEL_DRAW_STEP;
    /* never, by the assertion above - but a chunk longer than its scratch
     * is drawn as it used to be, from its own start */
    if (start - sDrawBase + first + count > VOXEL_DRAW_REACH)
    {
        BindVertices(chunk->vram);
        return (int)first;
    }
    BindVertices((const VoxelGpuVertex *)sChunkBlock + sDrawBase);
    return (int)(start - sDrawBase + first);
}

/*
 * Colour grade and distance haze, both worked out per vertex by the shader
 * from what the chunks already carry: no extra pass or target, and nothing
 * rebuilt when the values change.
 *
 * The baked shade only ever darkened a texel towards grey. Graded, the same
 * number picks a colour: full sun (1.0) is slightly warm and slightly above
 * the art, the baked shadow (0.70) and the dark sides of structures (0.68)
 * lean blue and a little darker. The art keeps its hues; lit against shaded
 * reads further apart than before.
 *
 * The haze starts a little beyond the player and grows towards the top of the
 * screen, which with this camera is the distance: the far rows fade towards a
 * pale sky colour and the ground reads as receding. It is relative to the
 * camera's own distance, so a wider map framed from further away hazes the
 * same share of the screen.
 *
 * Indoors, and with lighting compiled out, both are the identity.
 */
#define VOXEL_GRADE_SHADE_LOW 0.62f     /* at or below: fully the shadow tint */
#define VOXEL_HAZE_START 1.05f          /* x the eye-to-player distance */
#define VOXEL_HAZE_RAMP 0.60f           /* ... to full haze this much further */
#define VOXEL_HAZE_MAX 0.14f
/* 0xAABBGGRR, as the texture environment takes it. */
#define VOXEL_HAZE_COLOUR 0xFFFAE6D2u

/*
 * Everything the outdoor light is made of, as one value: the grade's tints,
 * how much the distance hazes, how far the dapples darken and brighten, and
 * the sun rays. All of it reaches the GPU as uniforms and
 * texture-environment constants, so a day-night cycle only has to blend two
 * of these: nothing baked into the chunks changes and nothing is rebuilt.
 *
 * The shadow tint is what the baked shadows are drawn with. It is a little
 * lighter than the shade alone would make them, so a shadow reads as cool
 * light rather than as a dark hole; where the shadows fall is unchanged.
 *
 * The sun's warmth is in the tints themselves. It was once a screen-space
 * ramp towards the sun's corner, per vertex; broad as it had to be not to
 * read as a lamp, it differed by a few percent across the screen, and its
 * thirteen shader instructions cost the Old 3DS frames where the view is
 * dense. Its average is kept, folded into sun and shade.
 */
typedef struct
{
    float sun[3], shade[3];      /* the grade at full sun and at full shadow */
    float haze;                  /* the most the distance haze takes */
    float dappleLow, dappleHigh; /* brightness under a dapple's shade, in its light */
    float rays;                  /* the sun rays' strength at their brightest */
    float bloom;                 /* glow around the brightest parts (3ds_video.c) */
    float motes;                 /* the sunlit dust in the air, at its brightest */
    float hazeRgb[3];            /* what the distance fades to */
    float hazeStart, hazeRamp;   /* x the eye-to-player distance, see SetGrade */
} VoxelLight;

#if CTR_VOXEL_LIGHTING
static VoxelLight LightMix(const VoxelLight *a, const VoxelLight *b, float t)
{
    VoxelLight out = *a;

    for (int i = 0; i < 3; ++i)
    {
        out.sun[i] += (b->sun[i] - a->sun[i]) * t;
        out.shade[i] += (b->shade[i] - a->shade[i]) * t;
        out.hazeRgb[i] += (b->hazeRgb[i] - a->hazeRgb[i]) * t;
    }
    out.haze += (b->haze - a->haze) * t;
    out.dappleLow += (b->dappleLow - a->dappleLow) * t;
    out.dappleHigh += (b->dappleHigh - a->dappleHigh) * t;
    out.rays += (b->rays - a->rays) * t;
    out.bloom += (b->bloom - a->bloom) * t;
    out.motes += (b->motes - a->motes) * t;
    out.hazeStart += (b->hazeStart - a->hazeStart) * t;
    out.hazeRamp += (b->hazeRamp - a->hazeRamp) * t;
    return out;
}

/*
 * The game's fog, as light. Outdoors a pale veil that closes in from much
 * nearer than the clear-day haze. Under the ground there is no sun to break
 * into dapples: the light goes cold and dim, and the distance sinks into a
 * blue-black instead of whitening, so the far end of a tunnel is lost in the
 * dark (the fog's drifting banks, FogSheets, stay pale in front of it). Both
 * come in with the fog's own blend, so a fog that fades in or out takes the
 * light with it.
 */
static VoxelLight FogLight(const VoxelLight *clear, bool cave)
{
    VoxelLight fog = *clear;

    if (cave)
    {
        fog.sun[0] = 0.76f; fog.sun[1] = 0.80f; fog.sun[2] = 0.88f;
        fog.shade[0] = 0.58f; fog.shade[1] = 0.64f; fog.shade[2] = 0.78f;
        fog.haze = 0.86f;
        fog.dappleLow = fog.dappleHigh = 1.0f;
        fog.hazeRgb[0] = 0.10f; fog.hazeRgb[1] = 0.12f; fog.hazeRgb[2] = 0.17f;
        fog.hazeStart = 0.86f;
        fog.hazeRamp = 0.55f;
        fog.rays = 0.0f;
        fog.bloom = 0.0f;
        fog.motes = 0.0f;
    }
    else
    {
        /* Fog glows: more bloom, and no dust or rays to see in it. */
        fog.sun[0] = 0.95f; fog.sun[1] = 0.98f; fog.sun[2] = 1.00f;
        fog.haze = 0.70f;
        fog.dappleLow = 0.95f; fog.dappleHigh = 1.02f;
        fog.rays = 0.0f;
        fog.bloom = 0.18f;
        fog.motes = 0.0f;
        fog.hazeRgb[0] = 0.80f; fog.hazeRgb[1] = 0.83f; fog.hazeRgb[2] = 0.87f;
        fog.hazeStart = 0.80f;
        fog.hazeRamp = 0.65f;
    }
    return LightMix(clear, &fog, VoxelWorld_FogDensity());
}
#endif

static VoxelLight BaseLightFor(bool indoor)
{
    VoxelLight light = {{1.00f, 0.99f, 0.95f}, {0.93f, 0.97f, 1.05f}, VOXEL_HAZE_MAX,
                        0.96f, 1.04f, 0.11f, 0.07f, 0.85f,
                        {(VOXEL_HAZE_COLOUR & 255) / 255.0f,
                         ((VOXEL_HAZE_COLOUR >> 8) & 255) / 255.0f,
                         ((VOXEL_HAZE_COLOUR >> 16) & 255) / 255.0f},
                        VOXEL_HAZE_START, VOXEL_HAZE_RAMP};

#if CTR_VOXEL_LIGHTING
    if (!indoor)
    {
        switch (VoxelWorld_Weather())
        {
        case VOXEL_WEATHER_SUN:
            light.sun[0] = 1.04f; light.sun[1] = 1.01f; light.sun[2] = 0.93f;
            light.haze = 0.12f;
            light.dappleLow = 0.94f; light.dappleHigh = 1.07f;
            light.rays = 0.14f;
            light.bloom = 0.10f;
            light.motes = 1.00f;
            break;
        case VOXEL_WEATHER_RAIN:
            /* Overcast: no sun to break into patches, rays or glinting dust. */
            light.sun[0] = 0.81f; light.sun[1] = 0.88f; light.sun[2] = 0.98f;
            light.shade[0] = 0.82f; light.shade[1] = 0.89f; light.shade[2] = 1.00f;
            light.haze = 0.30f;
            light.dappleLow = light.dappleHigh = 1.0f;
            light.rays = 0.0f;
            light.bloom = 0.08f;
            light.motes = 0.0f;
            /* The air grey with rain: the distance goes sooner, colder. */
            light.hazeRgb[0] = 0.62f; light.hazeRgb[1] = 0.68f; light.hazeRgb[2] = 0.76f;
            light.haze = 0.30f + 0.18f * VoxelWorld_WeatherAmount();
            light.hazeStart = 0.85f;
            {
                /* A bolt: the world lit cold white for a moment. */
                float bolt = VoxelWorld_Lightning();

                for (int c = 0; c < 3; ++c)
                {
                    light.sun[c] += (1.55f - light.sun[c]) * bolt;
                    light.shade[c] += (1.45f - light.shade[c]) * bolt;
                    light.hazeRgb[c] += (0.92f - light.hazeRgb[c]) * bolt;
                }
                light.bloom += 0.30f * bolt;
            }
            break;
        case VOXEL_WEATHER_SAND:
        {
            /* A sandstorm: the sun a warm smudge, the shadows filled with
             * the dust's own glow, and the distance gone into ochre. */
            VoxelLight sand = light;

            sand.sun[0] = 1.04f; sand.sun[1] = 0.93f; sand.sun[2] = 0.76f;
            sand.shade[0] = 0.96f; sand.shade[1] = 0.86f; sand.shade[2] = 0.72f;
            sand.haze = 0.72f;
            sand.hazeRgb[0] = 0.86f; sand.hazeRgb[1] = 0.70f; sand.hazeRgb[2] = 0.48f;
            sand.hazeStart = 0.55f;
            sand.hazeRamp = 0.95f;
            sand.dappleLow = sand.dappleHigh = 1.0f;
            sand.rays = 0.0f;
            sand.bloom = 0.16f;
            sand.motes = 0.0f;
            light = LightMix(&light, &sand, VoxelWorld_WeatherAmount());
            break;
        }
        case VOXEL_WEATHER_ASH:
        {
            /* Under the volcano's plume: a dim, warm grey light, no hard sun
             * to cast dapples, the far slopes lost in ash. */
            VoxelLight ash = light;

            ash.sun[0] = 0.90f; ash.sun[1] = 0.87f; ash.sun[2] = 0.84f;
            ash.shade[0] = 0.80f; ash.shade[1] = 0.80f; ash.shade[2] = 0.83f;
            ash.haze = 0.50f;
            ash.hazeRgb[0] = 0.66f; ash.hazeRgb[1] = 0.63f; ash.hazeRgb[2] = 0.61f;
            ash.hazeStart = 0.75f;
            ash.hazeRamp = 0.90f;
            ash.dappleLow = 0.96f; ash.dappleHigh = 1.01f;
            ash.rays = 0.0f;
            ash.bloom = 0.10f;
            ash.motes = 0.0f;
            light = LightMix(&light, &ash, VoxelWorld_WeatherAmount());
            break;
        }
        case VOXEL_WEATHER_FOG:
            light = FogLight(&light, VoxelWorld_Underground());
            break;
        case VOXEL_WEATHER_PARTICLES:
            /* The weather's own snow fills the air instead. */
            light.sun[0] = 0.93f; light.sun[1] = 0.92f; light.sun[2] = 0.88f;
            light.haze = 0.36f;
            light.dappleLow = 0.92f; light.dappleHigh = 1.03f;
            light.rays = 0.0f;
            light.bloom = 0.12f;
            light.motes = 0.0f;
            break;
        case VOXEL_WEATHER_SHADE:
            light.sun[0] = 0.92f; light.sun[1] = 0.95f; light.sun[2] = 0.98f;
            light.haze = 0.25f;
            light.dappleLow = 0.92f; light.dappleHigh = 1.03f;
            light.rays = 0.0f;
            light.bloom = 0.12f;
            light.motes = 0.40f;
            break;
        default:
            break;
        }
        return light;
    }
#endif
    (void)indoor;
    for (int i = 0; i < 3; ++i)
        light.sun[i] = light.shade[i] = 1.0f;
    light.haze = 0.0f;
    light.dappleLow = light.dappleHigh = 1.0f;
    light.rays = 0.0f;
    light.bloom = 0.0f;
    light.motes = 0.0f;
    return light;
}

static float sMoonlight;
static uint32_t sMoonlightColour;

static uint32_t MoonlightColour(float fade)
{
    uint32_t colour = 0xFF000000u;

    for (unsigned channel = 0; channel < 3; ++channel)
        colour |= (uint32_t)(((sMoonlightColour >> (channel * 8u)) & 255u) * (1.0f - fade) + 0.5f)
               << (channel * 8u);
    return colour;
}

static VoxelLight LightFor(bool indoor)
{
    VoxelLight light = BaseLightFor(indoor);

    if (!indoor)
    {
        const CtrDayNightLight *cycle = CtrDayNight_Get();
        CtrDayNightBlend moon = CtrDayNight_Moonlight(cycle, 0.0f, cycle->rgb);

        for (unsigned channel = 0; channel < 3; ++channel)
        {
            light.sun[channel] *= cycle->rgb[channel] * (1.0f - moon.amount);
            light.shade[channel] *= cycle->rgb[channel] * (1.0f - moon.amount);
            light.hazeRgb[channel] *= cycle->rgb[channel];
            light.hazeRgb[channel] += (moon.rgb[channel] - light.hazeRgb[channel]) * moon.amount;
        }
        light.rays *= cycle->daylight;
        light.motes *= cycle->daylight;
        light.dappleLow = 1.0f + (light.dappleLow - 1.0f) * cycle->daylight;
        light.dappleHigh = 1.0f + (light.dappleHigh - 1.0f) * cycle->daylight;
    }
    return light;
}

static void SetGrade(const VoxelLight *light)
{
    float eye = sCamera.distance / cosf(C3D_AngleFromDegrees(sCamera.pitch));
    float fogStart = eye * light->hazeStart;
    float fogScale = light->haze / (eye * light->hazeRamp);

    /* Halved: the texture environment scales by two. */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadeTint,
                  light->shade[0] * 0.5f, light->shade[1] * 0.5f, light->shade[2] * 0.5f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniTintDiff,
                  (light->sun[0] - light->shade[0]) * 0.5f,
                  (light->sun[1] - light->shade[1]) * 0.5f,
                  (light->sun[2] - light->shade[2]) * 0.5f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniGrade, 1.0f / (1.0f - VOXEL_GRADE_SHADE_LOW),
                  -VOXEL_GRADE_SHADE_LOW / (1.0f - VOXEL_GRADE_SHADE_LOW), 0.0f, 0.0f);
    /* haze = (-viewZ - start) * scale = viewZ * -scale - start * scale */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniFog, -fogScale, -fogStart * fogScale, light->haze, 0.0f);
}

/*
 * Sun dapples: soft patches of light and shade, as under broken cloud or a
 * canopy, drifting slowly the way the shadows fall. One small tileable
 * texture made at start-up, laid over the world from above along the sun
 * (texcoord1, see voxel.v.pica) and multiplied in by the texture environment:
 * one more texture read per world pixel, and no vertex, pass or rebuild. The
 * baked shadows are untouched; the dapples only scale the light on them.
 * Sprites take them too, and the sun rays are drawn from the same pattern.
 */
#define VOXEL_DAPPLE_DIM 128
#define VOXEL_DAPPLE_PERIOD 24.0f  /* tiles along the pattern before it repeats */
#define VOXEL_DAPPLE_STRETCH 0.70f /* across it: patches lie along the sun */
#define VOXEL_DAPPLE_DRIFT 0.25f   /* tiles per second */


#if CTR_VOXEL_LIGHTING
static float sDappleDrift; /* this frame's, in tiles along the pattern */
/* The pattern's axes for this frame's sun, worked out once a frame
 * (DappleFrame): DappleUniforms only adds a draw's own offset. */
static float sDappleC, sDappleS, sDappleSu, sDappleSv, sDappleU[3], sDappleV[3];

static float DappleLattice(int x, int y, int period, uint32_t seed)
{
    uint32_t h = (uint32_t)(x % period) * 374761393u + (uint32_t)(y % period) * 668265263u
               + seed * 2246822519u;

    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFu) / 65535.0f;
}

static float Quintic(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* Value noise that repeats every `period` lattice cells. */
static float DappleNoise(float u, float v, int period, uint32_t seed)
{
    int x = (int)u, y = (int)v;
    float fx = Quintic(u - (float)x), fy = Quintic(v - (float)y);
    float a = DappleLattice(x, y, period, seed), b = DappleLattice(x + 1, y, period, seed);
    float c = DappleLattice(x, y + 1, period, seed), d = DappleLattice(x + 1, y + 1, period, seed);
    float top = a + (b - a) * fx, bottom = c + (d - c) * fx;

    return top + (bottom - top) * fy;
}

/*
 * Three octaves of tileable noise, then a soft threshold: broad patches with
 * ragged, blurred edges, a little more light than shade. L8, filtered and
 * repeating. Not fatal: without it the world is drawn as before.
 */
static void MakeDapple(void)
{
    static const struct
    {
        int period;
        float weight;
    } octaves[] = {{3, 1.0f}, {6, 0.5f}, {12, 0.2f}};
    uint8_t *texels;

    if (!C3D_TexInit(&sDappleTex, VOXEL_DAPPLE_DIM, VOXEL_DAPPLE_DIM, GPU_L8))
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no linear memory for the sun dapples");
        return;
    }
    texels = sDappleTex.data;
    for (unsigned y = 0; y < VOXEL_DAPPLE_DIM; ++y)
        for (unsigned x = 0; x < VOXEL_DAPPLE_DIM; ++x)
        {
            float n = 0.0f, total = 0.0f, t;

            for (unsigned o = 0; o < sizeof(octaves) / sizeof(octaves[0]); ++o)
            {
                float scale = (float)octaves[o].period / VOXEL_DAPPLE_DIM;

                n += octaves[o].weight * DappleNoise((float)x * scale, (float)y * scale,
                                                     octaves[o].period, o + 1);
                total += octaves[o].weight;
            }
            t = (n / total - 0.40f) / 0.20f;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            texels[CtrVideo_Texel(x, y, VOXEL_DAPPLE_DIM)] =
                (uint8_t)(t * t * (3.0f - 2.0f * t) * 255.0f + 0.5f);
        }
    C3D_TexSetFilter(&sDappleTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sDappleTex, GPU_REPEAT, GPU_REPEAT);
    C3D_TexFlush(&sDappleTex);
    sHaveDapple = true;
}

/* The clock the light effects drift by (dapples, rays, motes, fog banks,
 * weather). A FRAME_DUMP build runs it on the game's frames instead, so that
 * two runs' pictures compare whatever each frame cost them. */
static double EffectSeconds(void)
{
#ifdef CTR_FRAME_DUMP
    return (double)CtrPlatform_FrameCount() / 60.0;
#else
    return (double)svcGetSystemTick() / SYSCLOCK_ARM11;
#endif
}

static void DappleFrame(void)
{
    double seconds = EffectSeconds();
    float len = sqrtf(VOXEL_SUN_DX * VOXEL_SUN_DX + VOXEL_SUN_DZ * VOXEL_SUN_DZ);
    float c = VOXEL_SUN_DX / len, s = VOXEL_SUN_DZ / len;
    float su = 1.0f / VOXEL_DAPPLE_PERIOD;
    float sv = 1.0f / (VOXEL_DAPPLE_PERIOD * VOXEL_DAPPLE_STRETCH);

    sDappleDrift = (float)fmod(seconds * VOXEL_DAPPLE_DRIFT, VOXEL_DAPPLE_PERIOD);
    sDappleC = c;
    sDappleS = s;
    sDappleSu = su;
    sDappleSv = sv;
    sDappleU[0] = c * su;
    sDappleU[1] = (c * VOXEL_SUN_DX + s * VOXEL_SUN_DZ) * su;
    sDappleU[2] = s * su;
    sDappleV[0] = -s * sv;
    sDappleV[1] = (c * VOXEL_SUN_DZ - s * VOXEL_SUN_DX) * sv;
    sDappleV[2] = c * sv;
}

/*
 * Turns texture unit 1 back off once the world is drawn. Citro3D enables a
 * unit for as long as a texture is bound to it, but its C3D_TexBind reads the
 * texture's type before storing it for units 1 and 2, so binding NULL - the
 * only way to turn one off - faults there (a data abort on the Old 3DS at
 * FAR 0xC; Azahar reads the null page and hid it). Left on, every later draw
 * of the frame, sprites and both 2D screens, would fetch from the unit for
 * nothing.
 *
 * So the slot is cleared as C3D_TexBind stores it, in citro3d 1.7.1's context
 * layout (flags at 0x20, the bound textures at 0x118, a unit's dirty flag at
 * bit 23 + unit), and only once the slot is seen to hold the dapples: under
 * any other layout the unit stays on, which costs a little and draws the same.
 */
static void ReleaseDappleUnit(void)
{
    static bool warned;
    uint32_t *flags = (uint32_t *)(void *)(__C3D_Context + C3DI_CONTEXT_FLAGS);
    C3D_Tex **bound = (C3D_Tex **)(void *)(__C3D_Context + C3DI_CONTEXT_TEX);

    if (bound[1] != &sDappleTex)
    {
        if (!warned)
            CtrLog_Write(CTR_LOG_ERROR, "VOXEL: unknown citro3d layout, texture unit 1 left on");
        warned = true;
        return;
    }
    bound[1] = NULL;
    *flags |= C3DI_FLAG_TEX(1);
}

/*
 * Sun rays: faint warm shafts falling from the sun's corner of the screen
 * (the top left) across the picture, over the world and its sprites. One
 * static mesh over the screen, made at start-up and drawn with additive
 * blending; the shafts are the dapple pattern again, stretched almost to lines
 * along the rays, so they need no texture of their own and drift as slowly.
 * The mesh carries each vertex's strength in its shade, strongest in the
 * corner and gone before the far one; cells with no light are left out, which
 * is what keeps the fill down.
 *
 * Positions are screen units, 0..1 each way with y up, drawn through an
 * orthographic projection, at z -1.
 */
#define VOXEL_RAY_COLS 8
#define VOXEL_RAY_ROWS 5
#define VOXEL_RAY_REACH 0.65f    /* of the screen's diagonal, from the corner */
#define VOXEL_RAY_ANGLE 35.0f    /* degrees off vertical, leaning right */
#define VOXEL_RAY_ACROSS 260.0f  /* pixels across the rays per pattern period */
#define VOXEL_RAY_ALONG 4000.0f  /* ... and along them */
#define VOXEL_RAY_DRIFT 0.012f   /* pattern periods per second, across */
/* 0xAABBGGRR: warm white, the sun's own colour rather than gold. */
#define VOXEL_RAY_COLOUR 0xFFD0F0FFu

static float RayStrength(float x, float y)
{
    float dx = x * CTR_GAME_WIDTH, dy = (1.0f - y) * CTR_GAME_HEIGHT;
    float diagonal = sqrtf((float)(CTR_GAME_WIDTH * CTR_GAME_WIDTH
                                   + CTR_GAME_HEIGHT * CTR_GAME_HEIGHT));
    float s = 1.0f - sqrtf(dx * dx + dy * dy) / (diagonal * VOXEL_RAY_REACH);

    return s <= 0.0f ? 0.0f : s * s;
}

static void RayVertex(VoxelGpuVertex *v, float x, float y)
{
    v->u = v->v = 0.0f;
    v->x = (int16_t)(x * 512.0f + 0.5f);
    v->y = (int16_t)(y * 512.0f + 0.5f);
    v->z = -512;
    v->shade = (int16_t)(RayStrength(x, y) * VOXEL_SHADE_SCALE + 0.5f);
}

/* Not fatal: without it there are no rays. */
static void MakeRays(void)
{
    static const unsigned corner[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
    unsigned count = 0;

    sRays = linearAlloc(VOXEL_RAY_COLS * VOXEL_RAY_ROWS * 6 * sizeof(VoxelGpuVertex));
    if (sRays == NULL)
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no linear memory for the sun rays");
        return;
    }
    for (unsigned row = 0; row < VOXEL_RAY_ROWS; ++row)
        for (unsigned col = 0; col < VOXEL_RAY_COLS; ++col)
        {
            float x0 = (float)col / VOXEL_RAY_COLS, x1 = (float)(col + 1) / VOXEL_RAY_COLS;
            float y0 = (float)row / VOXEL_RAY_ROWS, y1 = (float)(row + 1) / VOXEL_RAY_ROWS;

            if (RayStrength(x0, y0) <= 0.0f && RayStrength(x1, y0) <= 0.0f
             && RayStrength(x0, y1) <= 0.0f && RayStrength(x1, y1) <= 0.0f)
                continue;
            for (unsigned k = 0; k < 6; ++k)
                RayVertex(&sRays[count++], corner[k][0] ? x1 : x0, corner[k][1] ? y1 : y0);
        }
    GSPGPU_FlushDataCache(sRays, count * sizeof(VoxelGpuVertex));
    sRayCount = count;
}

/*
 * Drawn last, with the dapple texture on unit 1. Colour is the constant gold;
 * alpha is (1 - pattern) x the vertex's strength x the light's, so the shafts
 * are the pattern's narrower shaded patches. Neither depth tested nor written:
 * light in the air lies over everything. The fade environment still applies,
 * so a fade to black takes the rays with it.
 */
static void DrawRays(const VoxelLight *light, const VoxelLight *unlit)
{
    float angle = C3D_AngleFromDegrees(VOXEL_RAY_ANGLE);
    float along[2] = {sinf(angle), -cosf(angle)}, across[2] = {cosf(angle), sinf(angle)};
    double seconds = EffectSeconds();
    float drift = (float)fmod(seconds * VOXEL_RAY_DRIFT, 1.0);
    unsigned strength = (unsigned)(light->rays * 255.0f + 0.5f);
    C3D_Mtx ortho, identity;
    C3D_TexEnv *env;

    Mtx_Ortho(&ortho, 0.0f, 1.0f, 0.0f, 1.0f, 0.1f, 10.0f, false);
    FitToLogicalSurface(&ortho);
    Mtx_Identity(&identity);
    SetProjection(&ortho);
    UploadModelView(&identity);
    SetOrigin(0.0f, 0.0f);
    /* The pattern in pixels, turned to the rays. */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleU,
                  across[0] * CTR_GAME_WIDTH / VOXEL_RAY_ACROSS,
                  across[1] * CTR_GAME_HEIGHT / VOXEL_RAY_ACROSS, 0.0f, drift);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleV,
                  along[0] * CTR_GAME_WIDTH / VOXEL_RAY_ALONG,
                  along[1] * CTR_GAME_HEIGHT / VOXEL_RAY_ALONG, 0.0f, 0.0f);
    SetGrade(unlit);

    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_ONE_MINUS_SRC_R, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
    C3D_TexEnvColor(env, VOXEL_RAY_COLOUR);
    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    C3D_TexEnvColor(env, strength << 24);
    C3D_TexEnvInit(C3D_GetTexEnv(2));
    FadeFor(false);

    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
    BindVertices(sRays);
    C3D_DrawArrays(GPU_TRIANGLES, 0, (int)sRayCount);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
}

/*
 * Sunlit dust: a few specks of light drifting in the air, the way dust and
 * pollen glint in sunshine. Each is a small soft diamond - a fan of four
 * triangles, bright in the middle and gone at its corners, so no texture -
 * turned to face the camera. They live in the world, not on the screen: a
 * speck stays where it is as the camera passes (anchored like the dapples)
 * and is hidden behind a roof, depth tested but never written. The box
 * around the camera's target they fill is wrapped, so there are always as
 * many in view, entering at its far edges, off screen.
 *
 * VOXEL_MOTES x 12 vertices, rewritten each frame: the previous frame's draw
 * has finished by then (the frame begins by waiting for it).
 */
#define VOXEL_MOTES 32
#define VOXEL_MOTE_VERTICES 12
#define VOXEL_MOTE_HALF_X 11.0f /* tiles each side of the camera's target */
#define VOXEL_MOTE_HALF_Z 9.0f
#define VOXEL_MOTE_LOW 0.3f     /* tiles above the ground */
#define VOXEL_MOTE_HIGH 2.6f
#define VOXEL_MOTE_SIZE 0.10f   /* tiles from the middle to a corner */
#define VOXEL_MOTE_WIND 0.35f   /* tiles per second, the way the shadows fall */
/* 0xAABBGGRR: warm white. */
#define VOXEL_MOTE_COLOUR 0xFFD8F4FFu

static void MakeMotes(void)
{
    sMotes = linearAlloc(VOXEL_MOTES * VOXEL_MOTE_VERTICES * sizeof(VoxelGpuVertex));
    if (sMotes == NULL)
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no linear memory for the sunlit dust");
}

/* A fixed pseudo-random number in 0..1 for mote i. */
static float MoteRandom(unsigned i, unsigned salt)
{
    uint32_t h = i * 2654435761u ^ salt * 2246822519u;

    h = (h ^ (h >> 15)) * 2246822519u;
    h ^= h >> 13;
    return (float)(h & 0xFFFFu) / 65535.0f;
}

static float Wrap(float value, float size)
{
    return value - floorf(value / size) * size;
}

static void MoteVertex(VoxelGpuVertex *v, float x, float y, float z, float strength)
{
    v->u = v->v = 0.0f;
    v->x = (int16_t)(x * 512.0f);
    v->y = (int16_t)(y * 512.0f);
    v->z = (int16_t)(z * 512.0f);
    v->shade = (int16_t)(strength * VOXEL_SHADE_SCALE);
}

/*
 * What the specks were laid out from, all but the clock. The right eye of a
 * stereo pair draws the left eye's specks as they are (the two share the
 * view, CameraMatrices): laid out again from a clock a few milliseconds on,
 * they were written into the buffer the GPU may still have been drawing the
 * left eye from - it starts on that eye while the right one is recorded
 * (GpuStartEarly, 3ds_video.c).
 */
typedef struct
{
    int originX, originZ, anchorX, anchorZ;
    float targetY, sunX, sunZ, axes[6];
} MotesKey;

static struct
{
    bool reusable;      /* laid out for a left eye */
    uint32_t draw;      /* by that Draw (sDrawSerial) */
    MotesKey key;
} sMotesBuilt;

/*
 * Colour is the constant warm white; alpha is the vertex's strength (the
 * twinkle, gone at the corners) x the light's. Added over what is behind.
 */
static void DrawMotes(const C3D_Mtx *view, const VoxelLight *light, const VoxelLight *unlit)
{
    int originX = (int)floorf(sCamera.targetX), originZ = (int)floorf(sCamera.targetZ);
    float seconds = (float)fmod(EffectSeconds(), 3600.0);
    float len = sqrtf(VOXEL_SUN_DX * VOXEL_SUN_DX + VOXEL_SUN_DZ * VOXEL_SUN_DZ);
    float windX = VOXEL_SUN_DX / len * VOXEL_MOTE_WIND, windZ = VOXEL_SUN_DZ / len * VOXEL_MOTE_WIND;
    /* The camera's right and up in the world: the view's first two rows. */
    float rx = view->r[0].x * VOXEL_MOTE_SIZE, ry = view->r[0].y * VOXEL_MOTE_SIZE;
    float rz = view->r[0].z * VOXEL_MOTE_SIZE;
    float ux = view->r[1].x * VOXEL_MOTE_SIZE, uy = view->r[1].y * VOXEL_MOTE_SIZE;
    float uz = view->r[1].z * VOXEL_MOTE_SIZE;
    unsigned strength = (unsigned)(light->motes * 255.0f + 0.5f);
    VoxelGpuVertex *v = sMotes;
    C3D_TexEnv *env;
    MotesKey key;

    memset(&key, 0, sizeof(key));
    key.originX = originX;
    key.originZ = originZ;
    key.anchorX = sDappleAnchorX;
    key.anchorZ = sDappleAnchorZ;
    key.targetY = sCamera.targetY;
    key.sunX = VOXEL_SUN_DX;
    key.sunZ = VOXEL_SUN_DZ;
    key.axes[0] = rx;
    key.axes[1] = ry;
    key.axes[2] = rz;
    key.axes[3] = ux;
    key.axes[4] = uy;
    key.axes[5] = uz;
    if (sDrawEye == 1 && sMotesBuilt.reusable && sMotesBuilt.draw + 1u == sDrawSerial
     && memcmp(&sMotesBuilt.key, &key, sizeof(key)) == 0)
        v = NULL; /* the left eye's, a Draw ago */
    else
    {
        sMotesBuilt.reusable = sDrawEye == 0;
        sMotesBuilt.draw = sDrawSerial;
        sMotesBuilt.key = key;
    }

    for (unsigned i = 0; v != NULL && i < VOXEL_MOTES; ++i)
    {
        float phase = MoteRandom(i, 1) * 6.2832f;
        /* Where it has drifted to in the anchored world, then brought into
         * the box around the target. */
        float wx = MoteRandom(i, 2) * 64.0f + seconds * windX * (0.6f + 0.8f * MoteRandom(i, 3));
        float wz = MoteRandom(i, 4) * 64.0f + seconds * windZ * (0.6f + 0.8f * MoteRandom(i, 3));
        float x = Wrap(wx - (float)(originX + sDappleAnchorX) + VOXEL_MOTE_HALF_X,
                       2.0f * VOXEL_MOTE_HALF_X) - VOXEL_MOTE_HALF_X;
        float z = Wrap(wz - (float)(originZ + sDappleAnchorZ) + VOXEL_MOTE_HALF_Z,
                       2.0f * VOXEL_MOTE_HALF_Z) - VOXEL_MOTE_HALF_Z;
        float y = sCamera.targetY + VOXEL_MOTE_LOW
                + (VOXEL_MOTE_HIGH - VOXEL_MOTE_LOW) * MoteRandom(i, 5)
                + 0.15f * sinf(seconds * 0.7f + phase);
        float glint = 0.55f + 0.45f * sinf(seconds * (1.1f + MoteRandom(i, 6)) + phase * 2.0f);

        MoteVertex(&v[0], x, y, z, glint);
        MoteVertex(&v[1], x + ux, y + uy, z + uz, 0.0f);
        MoteVertex(&v[2], x + rx, y + ry, z + rz, 0.0f);
        v[3] = v[0];
        MoteVertex(&v[4], x + rx, y + ry, z + rz, 0.0f);
        MoteVertex(&v[5], x - ux, y - uy, z - uz, 0.0f);
        v[6] = v[0];
        MoteVertex(&v[7], x - ux, y - uy, z - uz, 0.0f);
        MoteVertex(&v[8], x - rx, y - ry, z - rz, 0.0f);
        v[9] = v[0];
        MoteVertex(&v[10], x - rx, y - ry, z - rz, 0.0f);
        MoteVertex(&v[11], x + ux, y + uy, z + uz, 0.0f);
        v += VOXEL_MOTE_VERTICES;
    }
    if (v != NULL)
        GSPGPU_FlushDataCache(sMotes, VOXEL_MOTES * VOXEL_MOTE_VERTICES * sizeof(VoxelGpuVertex));

    SetModelView(originX, originZ);
    SetGrade(unlit);
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
    C3D_TexEnvColor(env, (strength << 24) | (VOXEL_MOTE_COLOUR & 0xFFFFFFu));
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
    FadeFor(false);

    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
    BindVertices(sMotes);
    C3D_DrawArrays(GPU_TRIANGLES, 0, VOXEL_MOTES * VOXEL_MOTE_VERTICES);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
}

/*
 * The pattern's coordinates for a draw measured from (worldX, worldZ). A
 * point is first slid along the sun by its height, (x + DX y, z + DZ y), so a
 * roof and the ground beside it take the patch the same light passed through;
 * then turned so the pattern's long axis and its drift follow the sun. The
 * draw's own corner goes in as a wrapped offset: the shader then only ever
 * sees small coordinates, whatever the map. `lift` is the height the draw is
 * translated by (a static model's), slid along the sun like its vertices'.
 * After DappleFrame.
 */
static void DappleUniforms(int worldX, int worldZ, float lift)
{
    /* Anchored: a crossing moves the origin, and the pattern must not move
     * with it. */
    worldX += sDappleAnchorX;
    worldZ += sDappleAnchorZ;
    float c = sDappleC, s = sDappleS;
    float u0 = (c * (float)worldX + s * (float)worldZ - sDappleDrift) * sDappleSu;
    float v0 = (c * (float)worldZ - s * (float)worldX) * sDappleSv;

    if (lift != 0.0f)
    {
        u0 += sDappleU[1] * lift;
        v0 += sDappleV[1] * lift;
    }
    sSetupAsked.dapples += 2;
    sSetupKept.dapples += SetUniform(DRAW_SET_DAPPLE_U, sUniDappleU,
                                     sDappleU[0], sDappleU[1], sDappleU[2], u0 - floorf(u0));
    sSetupKept.dapples += SetUniform(DRAW_SET_DAPPLE_V, sUniDappleV,
                                     sDappleV[0], sDappleV[1], sDappleV[2], v0 - floorf(v0));
}

/*
 * The game's fog, in the scene instead of over it.
 *
 * On the GBA the fog is one 64x64 picture scrolled across the screen by
 * twenty blended sprites. Laid flat over a 3D view it hid the world like a
 * pane of frosted glass, and it moved with the screen instead of the ground.
 * Here it is two banks of drifting mist lying over the ground: one low, about
 * the player's knees, and one at chest height. Each is a single horizontal
 * sheet around the camera, textured with soft tileable noise and blended in
 * after everything else, depth tested but never written: a wall or a tree
 * that rises through a bank parts it, a sprite stands in it to the height the
 * bank reaches, and the banks thin and darken into the distance haze like
 * the ground under them.
 *
 * The cost is two draws of 384 vertices built once at start-up, one texture
 * read per covered pixel and a 16 KiB texture; the banks slide by moving the
 * sheet, never by rewriting it. They come in and leave with the game's fog
 * (VoxelWorld_FogDensity), and the compositor drops the flat sprites while
 * they are up (CtrVoxel_DrawsFog).
 */
#define VOXEL_FOG_DIM 128
#define VOXEL_FOG_GRID 8           /* cells along each side of a sheet */
#define VOXEL_FOG_REACH 32.0f      /* tiles from the sheet's centre to its side */
#define VOXEL_FOG_FADE 0.55f       /* ... share of that reach at which it starts to thin */
#define VOXEL_FOG_SHEET_VERTICES (VOXEL_FOG_GRID * VOXEL_FOG_GRID * 6)

typedef struct
{
    float period;       /* tiles the pattern covers before it repeats */
    float height;       /* tiles above the player's ground */
    float driftX, driftZ; /* tiles per second */
    float strength;     /* the most the bank covers, at full fog */
} FogBank;

static const FogBank sFogBanks[2] = {
    {12.0f, 0.30f, -0.55f, 0.00f, 0.58f},
    {7.0f, 1.10f, -0.85f, -0.25f, 0.34f},
};

/* The sandstorm's dust on the same two sheets (their periods are the sheets'
 * own): low and thick, swept along by the wind (DrawWeather). */
static const FogBank sSandBanks[2] = {
    {12.0f, 0.35f, -5.20f, 0.55f, 0.50f},
    {7.0f, 1.30f, -7.80f, 0.25f, 0.30f},
};

/*
 * The Fly cinematic's cloud wisps on the same two sheets, high over the
 * world under the hovering camera (VOXEL_FLY_PITCH and _DISTANCE put the
 * eye some 45 tiles up): a thin one and a thicker one, streaming back along
 * the way of travel (the drift is set as the flight begins, DrawCloudWisps).
 */
#define VOXEL_FLY_WISP_SPEED 9.0f /* tiles per second, back along the way */
static FogBank sCloudBanks[2] = {
    {12.0f, 24.0f, 0.0f, 0.0f, 0.55f},
    {7.0f, 33.0f, 0.0f, 0.0f, 0.75f},
};

/*
 * Wisps rather than a wash: four octaves of the dapples' noise, thresholded
 * so that clear gaps open between the banks. A8: only coverage is stored,
 * the colour is a constant of the frame.
 */
static void MakeFog(void)
{
    static const struct
    {
        int period;
        float weight;
    } octaves[] = {{4, 1.0f}, {8, 0.55f}, {16, 0.30f}, {32, 0.12f}};
    uint8_t *texels;

    sFogSheets = linearAlloc(2 * VOXEL_FOG_SHEET_VERTICES * sizeof(VoxelGpuVertex));
    if (sFogSheets == NULL || !C3D_TexInit(&sFogTex, VOXEL_FOG_DIM, VOXEL_FOG_DIM, GPU_A8))
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no linear memory for the fog");
        linearFree(sFogSheets);
        sFogSheets = NULL;
        return;
    }
    texels = sFogTex.data;
    for (unsigned y = 0; y < VOXEL_FOG_DIM; ++y)
        for (unsigned x = 0; x < VOXEL_FOG_DIM; ++x)
        {
            float n = 0.0f, total = 0.0f, t;

            for (unsigned o = 0; o < sizeof(octaves) / sizeof(octaves[0]); ++o)
            {
                float scale = (float)octaves[o].period / VOXEL_FOG_DIM;

                n += octaves[o].weight * DappleNoise((float)x * scale, (float)y * scale,
                                                     octaves[o].period, o + 11);
                total += octaves[o].weight;
            }
            t = (n / total - 0.32f) / 0.40f;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            texels[CtrVideo_Texel(x, y, VOXEL_FOG_DIM)] =
                (uint8_t)(t * t * (3.0f - 2.0f * t) * 255.0f + 0.5f);
        }
    C3D_TexSetFilter(&sFogTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sFogTex, GPU_REPEAT, GPU_REPEAT);
    C3D_TexFlush(&sFogTex);

    /*
     * The sheets: a grid around their own centre, the pattern in world tiles
     * (so it repeats every `period` tiles across the sheet), and in the shade
     * the coverage at that vertex, falling to nothing towards the rim so the
     * sheet has no edge to see.
     */
    for (unsigned bank = 0; bank < 2; ++bank)
    {
        static VoxelVertex quad[VOXEL_FOG_SHEET_VERTICES];
        float cell = 2.0f * VOXEL_FOG_REACH / VOXEL_FOG_GRID;
        unsigned n = 0;

        for (unsigned gz = 0; gz < VOXEL_FOG_GRID; ++gz)
            for (unsigned gx = 0; gx < VOXEL_FOG_GRID; ++gx)
            {
                static const unsigned corners[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};

                for (unsigned k = 0; k < 6; ++k)
                {
                    float x = -VOXEL_FOG_REACH + (float)(gx + corners[k][0]) * cell;
                    float z = -VOXEL_FOG_REACH + (float)(gz + corners[k][1]) * cell;
                    float r = sqrtf(x * x + z * z) / VOXEL_FOG_REACH;
                    float edge = (1.0f - r) / (1.0f - VOXEL_FOG_FADE);

                    edge = edge < 0.0f ? 0.0f : edge > 1.0f ? 1.0f : edge;
                    quad[n].x = x;
                    quad[n].y = 0.0f;
                    quad[n].z = z;
                    quad[n].u = x / sFogBanks[bank].period;
                    quad[n].v = z / sFogBanks[bank].period;
                    quad[n].shade = edge * edge * (3.0f - 2.0f * edge);
                    ++n;
                }
            }
        PackDynamic(quad, n, sFogSheets + bank * VOXEL_FOG_SHEET_VERTICES);
    }
    sHaveFog = true;
}

/*
 * The dark of a cave: alpha rising from nothing round the centre to full at
 * the rim, and full beyond it (clamped). Laid in black over the frame by the
 * compositor, centred on the player (CtrVoxel_Gloom).
 */
#define VOXEL_GLOOM_DIM 64
#define VOXEL_GLOOM_INNER 0.30f /* share of the radius left clear */
#define VOXEL_GLOOM_SIZE 520.0f /* pixels across the ring, on the 400x240 view */
#define VOXEL_GLOOM_MAX 0.62f   /* how dark the rim is, at full fog */

static void MakeGloom(void)
{
    uint8_t *texels;

    if (!C3D_TexInit(&sGloomTex, VOXEL_GLOOM_DIM, VOXEL_GLOOM_DIM, GPU_A8))
        return;
    texels = sGloomTex.data;
    for (unsigned y = 0; y < VOXEL_GLOOM_DIM; ++y)
        for (unsigned x = 0; x < VOXEL_GLOOM_DIM; ++x)
        {
            float dx = ((float)x + 0.5f) / (VOXEL_GLOOM_DIM * 0.5f) - 1.0f;
            float dy = ((float)y + 0.5f) / (VOXEL_GLOOM_DIM * 0.5f) - 1.0f;
            float t = (sqrtf(dx * dx + dy * dy) - VOXEL_GLOOM_INNER) / (1.0f - VOXEL_GLOOM_INNER);

            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            texels[CtrVideo_Texel(x, y, VOXEL_GLOOM_DIM)] =
                (uint8_t)(t * t * (3.0f - 2.0f * t) * 255.0f + 0.5f);
        }
    C3D_TexSetFilter(&sGloomTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sGloomTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexFlush(&sGloomTex);
    sHaveGloom = true;
}
#endif

static bool DapplesOn(const VoxelLight *light)
{
    return sHaveDapple && light->dappleHigh - light->dappleLow > 0.01f;
}

/*
 * The texture environment constant for the dapples: with k in alpha and c in
 * colour, the factor 2 (k L + (1 - k) c) runs from dappleLow where the pattern
 * L is 0 to dappleHigh where it is 1.
 */
static uint32_t DappleConstant(const VoxelLight *light)
{
    float k = (light->dappleHigh - light->dappleLow) * 0.5f;
    float c = light->dappleLow / (2.0f * (1.0f - k));
    unsigned a = (unsigned)(k * 255.0f + 0.5f);
    unsigned g = c >= 1.0f ? 255u : (unsigned)(c * 255.0f + 0.5f);

    return a << 24 | g << 16 | g << 8 | g;
}

/*
 * Environments 0-2 build the lit texel; alpha is the texture's alone, so the
 * haze weight the shader puts in the vertex alpha never reaches the alpha
 * test. Plain: environment 0 is texture x the graded colour, doubled, and 1-2
 * pass it on. With the dapples (NULL for none):
 *   0: m = k L + (1 - k) c          the dapple factor, halved
 *   1: m x graded colour, doubled   the graded colour, dappled
 *   2: texture x that, doubled
 * Environment 3 is the haze and 4 the screen fade (FadeFor); one stage later
 * under the windows' glow with the dapples (BuildingGlowEnv).
 */
/*
 * Shadow map (SHADOW QUALITY HIGH).
 *
 * The sun's view of the world around the camera, drawn once a frame with the
 * PICA200's shadow mode (ShadowMapRender): every pixel keeps the depth along
 * the sun of the nearest surface. The world's passes then compare their own
 * depth with it on texture unit 0, the only unit that reads a shadow map, and
 * the hardware filters the comparison over 2x2 texels: a shadow's outline is
 * the caster's own, wherever it falls, on the ground, a wall or a roof, with
 * a soft edge. The chunks bake no cast shadow then (voxel_lighting.c), only
 * how each face turns to the sun, so there is no edge to cut and nothing a
 * ray could miss between two vertices.
 *
 * A crown's transparent texels must not cast: shadow mode skips the alpha
 * test, so they go out as a "soft" write of full light, which the hardware
 * keeps as the least of what it held (255: no change). Opaque texels write
 * the depth.
 *
 * Off on LOW, indoors and without sun: nothing of it is drawn then, and the
 * shader's uniforms leave the texture coordinates as they were.
 */
#define SHADOW_DEBUG 0
#define VOXEL_SHADOW_MAP_DIM 256
/* Tiles from its centre to an edge, across the sun: as much as the view's
 * ground needs (ShadowMapFit), within these. A caster stands on the sun's
 * line through what it shades, at the same place in the map, so only what is
 * seen needs the map, however tall what shades it. */
#define VOXEL_SHADOW_MAP_HALF 20.0f   /* the swoop's, and the most */
#define VOXEL_SHADOW_MAP_HALF_MIN 6.0f
#define VOXEL_SHADOW_MAP_DEPTH 64.0f  /* tiles of depth along the sun */
/* Tiles: what a shaded point must be past what the sun meets first. The lit
 * surfaces' own margin is put on them as they are drawn into the map
 * (sShadowBias); the ones the sun does not reach need none, and so the
 * ground at their foot shades from right there. */
#define VOXEL_SHADOW_MAP_SAMPLE_BIAS 0.03f
/* The soft edge (ShadowMapRender): rings of copies of the casters, this far
 * across the sun (tiles) for the outermost, this much further from it for
 * the unlit ones. 0 rings: the filter's one texel alone. */
#define VOXEL_SHADOW_SOFT_RINGS 0u
#define VOXEL_SHADOW_SOFT_REACH 0.4f
#define VOXEL_SHADOW_SOFT_BACK 0.05f
static C3D_Tex sShadowTex;
static C3D_RenderTarget *sShadowTarget;
static bool sShadowFailed;
/* The world's passes sample it (set by CtrVoxel_Draw around them). */
static bool sShadowOn;
/* This frame's map was drawn, from this matrix: anchored world to (u, v,
 * depth) - the rows the shader is given, times the eye's inverse view. */
static bool sShadowReady;
static C3D_Mtx sShadowTexMtx;
static uint32_t sShadowColour;   /* the shadow's tint, stage 0's constant */
/* The green of a sunlit level vertex (the shader's colour, halved), which
 * the blur divides the darkening it reads by (ShadowBlurPass). */
static float sShadowLitGreen = 0.5f;
/* This frame's extent across the sun (tiles from the centre to an edge) and
 * the depth, in tiles, by which a surface does not shadow itself. */
static float sShadowHalfU = VOXEL_SHADOW_MAP_HALF, sShadowHalfV = VOXEL_SHADOW_MAP_HALF;
static float sShadowBias = 0.25f;
/* The atlas is on this unit while the map is on unit 0. */
static C3D_Tex *sShadowAtlasBound;

static bool ShadowMapMake(void)
{
    if (sShadowTarget != NULL)
        return true;
    if (sShadowFailed)
        return false;
    /* VRAM only (the GPU fills and renders it there), and only with room
     * to spare: the chunks' arenas come first */
    if (CtrGpuMem_LargestVram() < 4u * VOXEL_SHADOW_MAP_DIM * VOXEL_SHADOW_MAP_DIM + 128u * 1024u
     || !C3D_TexInitShadow(&sShadowTex, VOXEL_SHADOW_MAP_DIM, VOXEL_SHADOW_MAP_DIM))
    {
        sShadowFailed = true;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL shadow map: no VRAM (%lu largest)",
                     (unsigned long)CtrGpuMem_LargestVram());
        return false;
    }
    C3D_TexSetFilter(&sShadowTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sShadowTex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
    sShadowTex.border = 0xFFFFFFFFu;
    sShadowTarget = C3D_RenderTargetCreateFromTex(&sShadowTex, GPU_TEXFACE_2D, 0, -1);
    if (sShadowTarget == NULL)
    {
        C3D_TexDelete(&sShadowTex);
        sShadowFailed = true;
        return false;
    }
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL shadow map: %dx%d made", VOXEL_SHADOW_MAP_DIM, VOXEL_SHADOW_MAP_DIM);
    return true;
}

/* The shader's shadow uniforms: off (the texture coordinates as they were),
 * or the map's rows through the inverse of `modelView` (a view-space point
 * to the map). */
static void ShadowUniforms(const C3D_Mtx *modelView)
{
    if (modelView == NULL)
    {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowU, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowV, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowW, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowSel, 1.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    {
        C3D_Mtx inverse, rows;

        Mtx_Copy(&inverse, modelView);
        Mtx_Inverse(&inverse);
        Mtx_Multiply(&rows, &sShadowTexMtx, &inverse);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowU, rows.r[0].x, rows.r[0].y, rows.r[0].z, rows.r[0].w);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowV, rows.r[1].x, rows.r[1].y, rows.r[1].z, rows.r[1].w);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowW, rows.r[2].x, rows.r[2].y, rows.r[2].z, rows.r[2].w);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowSel, 0.0f, 1.0f, 0.0f, 0.0f);
        /* the dapples' coordinates are added to the atlas' on texcoord1 */
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleU, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleV, 0.0f, 0.0f, 0.0f, 0.0f);
        sDrawSet &= ~(DRAW_SET_DAPPLE_U | DRAW_SET_DAPPLE_V);
    }
}

/* The shadow's colour: what the shader makes of a vertex at the ambient light
 * (VoxelLighting_Ambient, HIGH's), graded as voxel.v.pica grades it and
 * halved as its colour is. A face the sun does not reach is baked at that
 * light already, so the map, which never darkens below it, leaves it be. */
static uint32_t ShadowColour(const VoxelLight *light)
{
    float ambient = VoxelLighting_Ambient();
    float t = (ambient - VOXEL_GRADE_SHADE_LOW) / (1.0f - VOXEL_GRADE_SHADE_LOW);
    uint32_t colour = 0xFF000000u;

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    sShadowLitGreen = light->sun[1] * 0.5f;
    for (unsigned c = 0; c < 3; ++c)
    {
        float k = ambient * (light->shade[c] + (light->sun[c] - light->shade[c]) * t) * 0.5f;

        if (k > 1.0f) k = 1.0f;
        if (k < 0.0f) k = 0.0f;
        colour |= (uint32_t)(k * 255.0f + 0.5f) << (c * 8u);
    }
    return colour;
}

/*
 * The world's environment with the map. The colour is drawn in full sun, as
 * without the map (the texel, texture1, x the graded colour, doubled, with
 * the moonlight); the shadow goes to the picture's alpha, for
 * ShadowBlurPass to soften and take away once the world is drawn. The map
 * takes away only the sun's own part, never below the shadow's colour A
 * (ShadowColour), so what was baked at the ambient light already (a face
 * turned from the sun, a crown's shadow) is the same lit or not:
 *   alpha = texel alpha x (1 - (1 - s) max(C - A, 0))   (green alone)
 * with s the filtered comparison: stage 0 is C - A (a subtraction clamps at
 * 0, A in the constant's alpha), stage 1 x (1 - s), stage 2 the rest. An
 * opaque texel stays at or above a half and the alpha test still drops
 * only the clear ones. The haze and the fade stay on 3 and 4 (5 with the
 * glow, which puts the texel's alpha back: no shadow on a glowing wall).
 */
static void ShadowTexEnv(void)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, sMoonlight > 0.0f ? GPU_MULTIPLY_ADD : GPU_MODULATE);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_SUBTRACT);
    C3D_TexEnvColor(env, (sMoonlightColour & 0x00FFFFFFu) | (sShadowColour & 0x0000FF00u) << 16);

    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_ONE_MINUS_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);

    env = C3D_GetTexEnv(2);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE1, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
}

/* Whether what is drawn next takes the map's shadow (ShadowTexEnv's stage
 * 0): off, its constant's alpha is full and C - A never above 0. */
static void ShadowReceive(bool on)
{
    C3D_TexEnvColor(C3D_GetTexEnv(0), (sMoonlightColour & 0x00FFFFFFu)
                    | (on ? (sShadowColour & 0x0000FF00u) << 16 : 0xFF000000u));
}

/* The logical surface the world is drawn into (3ds_video.c). */
C3D_Tex *CtrVideo_LogicalTexture(void);

/*
 * The shadow softened on the picture: once the world is drawn, its alpha
 * holds each pixel's shadow (ShadowTexEnv). A quad over the whole surface
 * reads it back three times round each pixel, filtered (two taps
 * VOXEL_SHADOW_BLUR pixels off, across each other, and the centre), and
 * multiplies the colour by 1 - their mean over the sunlit green: the shadow,
 * soft at its edge over a few pixels, the map's texels no longer showing.
 * Only the colour is written, so the alpha it reads stays as it was; a
 * second quad then puts the alpha back to full, as the world without the
 * map leaves it (the picture is laid on the screen with it). Two quads of
 * the picture, no more geometry.
 */
#define VOXEL_SHADOW_BLUR 1.25f
static void ShadowBlurPass(void)
{
    C3D_Tex *surface = CtrVideo_LogicalTexture();
    uint32_t *flags = (uint32_t *)(void *)(__C3D_Context + C3DI_CONTEXT_FLAGS);
    C3D_Tex **bound = (C3D_Tex **)(void *)(__C3D_Context + C3DI_CONTEXT_TEX);
    C3D_Mtx one;
    float du = VOXEL_SHADOW_BLUR / VOXEL_SURFACE_W, dv = VOXEL_SHADOW_BLUR / VOXEL_SURFACE_H;
    /* the darkening read, (1 - alpha), over the sunlit green: x 4 below */
    float k = sShadowLitGreen > 0.0625f ? 0.25f / sShadowLitGreen : 1.0f;
    C3D_TexEnv *env;

    /* corners in clip space (x 512: the shader's position scale), the
     * texture's coordinates the same corners in 0..1; a buffer of its own,
     * as every draw of the world is (no immediate mode in its state) */
    static VoxelGpuVertex *quadVertices;

    if (surface == NULL || surface->data == NULL)
        return;
    if (quadVertices == NULL)
    {
        static const VoxelGpuVertex corners[4] = {
            {0.0f, 0.0f, -512, -512, -256, 8192}, {1.0f, 0.0f, 512, -512, -256, 8192},
            {0.0f, 1.0f, -512, 512, -256, 8192}, {1.0f, 1.0f, 512, 512, -256, 8192},
        };

        quadVertices = linearAlloc(sizeof(corners));
        if (quadVertices == NULL)
            return;
        memcpy(quadVertices, corners, sizeof(corners));
        GSPGPU_FlushDataCache(quadVertices, sizeof(corners));
    }
    /* the world's alpha to memory before it is read */
    C3D_FrameSplit(0);
    Mtx_Identity(&one);
    SetProjection(&one);
    UploadModelView(&one);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniOrigin, 0.0f, 0.0f, 0.0f, 0.0f);
    /* texcoord0, texcoord1: the corner's own, moved by these; texcoord2 the
     * corner's own (voxel.v.pica: shadowSel 1, 1 adds them up) */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowU, 0.0f, 0.0f, 0.0f, du);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowV, 0.0f, 0.0f, 0.0f, -0.5f * dv);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowW, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleU, 0.0f, 0.0f, 0.0f, -0.5f * du);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleV, 0.0f, 0.0f, 0.0f, dv);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowSel, 1.0f, 1.0f, 0.0f, 0.0f);
    sDrawSet = 0;

    C3D_TexSetFilter(surface, GPU_LINEAR, GPU_LINEAR);
    C3D_TexBind(0, surface);
    C3D_TexBind(1, surface);
    C3D_TexBind(2, surface);

    /* the three taps' mean (in alpha), then the colour: 1 - 4k (1 - mean) */
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE1, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Both, GPU_INTERPOLATE);
    C3D_TexEnvColor(env, 0x80808080u);
    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_TEXTURE2, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Both, GPU_INTERPOLATE);
    C3D_TexEnvColor(env, 0xAAAAAAAAu);
    /* never lighter than the pixel's own shadow: min(own, mean) =
     * own - max(own - mean, 0), two clamped subtractions. A face turned
     * from the sun reads "no shadow" (baked dark already); averaged into the
     * shadowed ground at its foot it would leave a light rim there. The
     * edge still softens outwards, over what is lit. */
    env = C3D_GetTexEnv(2);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE2, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_SUBTRACT);
    env = C3D_GetTexEnv(3);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE2, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_SUBTRACT);
    env = C3D_GetTexEnv(4);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_4);
    {
        uint32_t c = (uint32_t)(k * 255.0f + 0.5f);

        C3D_TexEnvColor(env, 0xFF000000u | c * 0x010101u);
    }
    env = C3D_GetTexEnv(5);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);

    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(false, GPU_ALWAYS, (GPU_WRITEMASK)(GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE));
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_SRC_COLOR, GPU_ZERO, GPU_ONE);
    for (int quad = 0; quad < 2; ++quad)
    {
        if (quad == 1)
        {
            /* the alpha back to full: the constant, alone */
            env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            C3D_TexEnvColor(env, 0xFFFFFFFFu);
            for (int i = 1; i < 6; ++i)
                C3D_TexEnvInit(C3D_GetTexEnv(i));
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALPHA);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ONE, GPU_ZERO);
        }
        BindVertices(quadVertices);
        C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
    }

    /* as the world's passes leave things: no blending, the surface nearest
     * for the compositor, the tree atlas on unit 2, unit 1 free, the
     * uniforms written again */
    C3D_ColorLogicOp(GPU_LOGICOP_COPY);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    C3D_TexSetFilter(surface, GPU_NEAREST, GPU_NEAREST);
    C3D_TexBind(2, &sTreeAtlas);
    if (bound[1] == surface)
    {
        bound[1] = NULL;
        *flags |= C3DI_FLAG_TEX(1);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowSel, 1.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowU, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowV, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleU, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleV, 0.0f, 0.0f, 0.0f, 0.0f);
    sDrawSet = 0;
}

/* Whether the screen's tilt-shift bands are drawn by CtrVoxel_DioramaFused
 * (settings voxel_fused_blur, off by default). */
unsigned CtrVoxel_MeasureCut(void)
{
    return sMeasureCut;
}

bool CtrVoxel_FusedBlurOn(void)
{
    return sMeasureFusedBlur;
}

/* One band's quad: x from 1 to the picture's last column but one, y0..y1, the
 * texture coordinates the picture's own pixels have on the surface (one texel
 * a pixel, v counted from the bottom as citro2d does) and the gradient in
 * `shade`: shade0 on the y0 corners, shade1 on the y1 ones. The shader scales
 * positions by 1/512 and the pass multiplies them by 8 again (its model-view),
 * so a pixel is 64 units: 399 pixels still fit a short. */
static void FusedBandQuad(VoxelGpuVertex *quad, int y0, int y1, int16_t shade0, int16_t shade1)
{
    const int columns[2] = {1, CTR_GAME_WIDTH - 1};

    for (int i = 0; i < 4; ++i)
    {
        int px = columns[i & 1], py = i < 2 ? y0 : y1;

        quad[i].u = (float)px / VOXEL_SURFACE_W;
        quad[i].v = 1.0f - (float)py / VOXEL_SURFACE_H;
        quad[i].x = (int16_t)(px * 64);
        quad[i].y = (int16_t)(py * 64);
        quad[i].z = 0;
        quad[i].shade = i < 2 ? shade0 : shade1;
    }
}

/*
 * The screen's tilt-shift bands in one pass. The compositor blended each band
 * four times over the picture (two taps, each LINEAR half a texel off one way
 * or the other, each over the base), so every band pixel was shaded three
 * times in all. Here each is shaded once, from three texture units that read
 * the same surface:
 *   unit 2: the picture's own texel (an alias of `surface`, NEAREST),
 *   unit 0: the +0.5, +0.5 tap (an alias, LINEAR), unit 1: the -0.5, -0.5 tap
 * and the texture environment makes lerp(lerp(base, tapB, a1), tapC, a2), what
 * two src-alpha blends of the taps over the base come to. a1 and a2 come out
 * of the vertex program as the primary colour's red and green: with grade 0
 * (t = 0) and no tint difference, colour = shade * shadeTint, shade the
 * band's gradient (1 at the screen's edge, 0 at its inner edge, linear across
 * the quad as the compositor's vertex alphas are) and shadeTint (a1, a2) at
 * full strength; no halving, as the environment does not double it here.
 *
 * Drawn into the render target already current (the screen's, 400x240 of
 * pixels, tilted as citro2d's scene), over the rows the caller left undrawn,
 * with the world's program and no blending. `topRows` and `bottomRows` are the
 * bands' heights; `strength` the blur's, 0..1. False, with nothing drawn, if
 * the pass cannot be made: the caller then composes the old way.
 */
bool CtrVoxel_DioramaFused(C3D_Tex *surface, float strength, int topRows, int bottomRows)
{
    /* each band's tap weights: the +0.5 tap's, then the -0.5 tap's */
    static const float weights[2][2] = {{0.67f, 0.50f}, {0.60f, 0.45f}};
    /* the surface under two filters: a C3D_Tex has one, and the taps are
     * LINEAR where the picture itself stays NEAREST. Never deleted: they only
     * point at the surface's memory. */
    static C3D_Tex nearest, linear;
    static VoxelGpuVertex *bands;
    static int builtTop = -1, builtBottom = -1;
    uint32_t *flags = (uint32_t *)(void *)(__C3D_Context + C3DI_CONTEXT_FLAGS);
    C3D_Tex **bound = (C3D_Tex **)(void *)(__C3D_Context + C3DI_CONTEXT_TEX);
    C3D_Mtx ortho, pixels;
    C3D_TexEnv *env;
    const float du = 0.5f / VOXEL_SURFACE_W, dv = 0.5f / VOXEL_SURFACE_H;

    if (!sReady || surface == NULL || surface->data == NULL || topRows <= 0 || bottomRows <= 0
     || topRows + bottomRows > CTR_GAME_HEIGHT
     || sUniProjection < 0 || sUniModelView < 0 || sUniOrigin < 0 || sUniShadeTint < 0
     || sUniTintDiff < 0 || sUniGrade < 0 || sUniDappleU < 0 || sUniDappleV < 0
     || sUniShadowU < 0 || sUniShadowV < 0 || sUniShadowW < 0 || sUniShadowSel < 0)
        return false;
    if (bands == NULL)
    {
        bands = linearAlloc(8 * sizeof(VoxelGpuVertex));
        if (bands == NULL)
            return false;
    }
    if (builtTop != topRows || builtBottom != bottomRows)
    {
        /* top band: full at row 0, clear at its inner edge; the bottom one the
         * other way round. Written once for the sizes it is asked for, so the
         * second eye's pass reads what the first drew from. */
        FusedBandQuad(bands, 0, topRows, 16384, 0);
        FusedBandQuad(bands + 4, CTR_GAME_HEIGHT - bottomRows, CTR_GAME_HEIGHT, 0, 16384);
        GSPGPU_FlushDataCache(bands, 8 * sizeof(VoxelGpuVertex));
        builtTop = topRows;
        builtBottom = bottomRows;
    }
    /* Copies of the surface, taken again each time: its memory and format are
     * what they are now, and only the filter differs. */
    nearest = *surface;
    linear = *surface;
    C3D_TexSetFilter(&nearest, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetFilter(&linear, GPU_LINEAR, GPU_LINEAR);

    BindWorldProgram(false);
    {
        C3D_AttrInfo *attr = C3D_GetAttrInfo();

        /* VoxelGpuVertex: texcoord (2 floats), then position + shade (4 shorts) */
        AttrInfo_Init(attr);
        AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
        AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);
    }
    sDrawSet = 0;

    /* The target's own pixels: 400x240, x right, y down, tilted for the
     * screen as citro2d's scene is. The model-view takes the shader's 1/512
     * back to 1/64 of a unit, 64 to a pixel. */
    Mtx_OrthoTilt(&ortho, 0.0f, (float)CTR_GAME_WIDTH, (float)CTR_GAME_HEIGHT, 0.0f, 1.0f, -1.0f, true);
    Mtx_Identity(&pixels);
    pixels.r[0].x = 8.0f;
    pixels.r[1].y = 8.0f;
    pixels.r[2].z = 8.0f;
    SetProjection(&ortho);
    UploadModelView(&pixels);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniOrigin, 0.0f, 0.0f, 0.0f, 0.0f);
    /* texcoord0 = the corner + (du, -dv): the +0.5, +0.5 tap, v running the
     * other way to the rows; texcoord1 = the corner - (du, -dv); texcoord2 =
     * the corner (voxel.v.pica: shadowSel 1, 1 adds the corner's own to each
     * of the first two). The w of a row is its constant: the view's w is 1. */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowU, 0.0f, 0.0f, 0.0f, du);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowV, 0.0f, 0.0f, 0.0f, -dv);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowW, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleU, 0.0f, 0.0f, 0.0f, -du);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleV, 0.0f, 0.0f, 0.0f, dv);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowSel, 1.0f, 1.0f, 0.0f, 0.0f);
    /* t = clamp(shade * 0 + 0) = 0 and no tint difference: colour.rgb =
     * shade * shadeTint, which each band sets to its two weights below */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniTintDiff, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniGrade, 0.0f, 0.0f, 0.0f, 0.0f);

    C3D_TexBind(0, &linear);
    C3D_TexBind(1, &linear);
    C3D_TexBind(2, &nearest);

    /* tapB x a1 + base x (1 - a1), a1 the primary colour's red; the alpha
     * full, the picture's alpha being what the screen does without */
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE2, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFFFFFFFFu);
    /* tapC x a2 + that x (1 - a2), a2 the primary colour's green */
    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_G);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    for (int i = 2; i < 6; ++i)
        C3D_TexEnvInit(C3D_GetTexEnv(i));

    /* No blending, alpha test or depth: the colour replaces what the picture
     * under the bands would have been (the caller left them undrawn) */
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_ColorLogicOp(GPU_LOGICOP_COPY);
    for (int band = 0; band < 2; ++band)
    {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadeTint,
                      weights[band][0] * strength, weights[band][1] * strength, 0.0f, 0.0f);
        BindVertices(bands);
        C3D_DrawArrays(GPU_TRIANGLE_STRIP, band * 4, 4);
    }

    /* As the world's passes leave things for the compositor: every stage
     * plain (the first two are the pass's), the tree atlas on unit 2, unit 1
     * free, the uniforms written again. */
    for (int i = 0; i < 2; ++i)
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_ColorLogicOp(GPU_LOGICOP_COPY);
    if (sTreeAtlas.data != NULL)
        C3D_TexBind(2, &sTreeAtlas);
    else if (bound[2] == &nearest)
    {
        bound[2] = NULL;
        *flags |= C3DI_FLAG_TEX(2);
    }
    if (bound[1] == &linear)
    {
        bound[1] = NULL;
        *flags |= C3DI_FLAG_TEX(1);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowSel, 1.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowU, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowV, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleU, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleV, 0.0f, 0.0f, 0.0f, 0.0f);
    sDrawSet = 0;
    return true;
}

/* The world's texel on the unit it is read from: 1 under the map. */
static void BindAtlas(C3D_Tex *tex)
{
    if (sShadowOn)
    {
        C3D_TexBind(1, tex);
        sShadowAtlasBound = tex;
    }
    else
        C3D_TexBind(0, tex);
}

static void TerrainTexEnv(const VoxelLight *dapples)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

    if (sShadowOn)
    {
        ShadowTexEnv();
        return;
    }
    C3D_TexEnvInit(env);
    if (dapples == NULL)
    {
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_RGB, sMoonlight > 0.0f ? GPU_MULTIPLY_ADD : GPU_MODULATE);
        C3D_TexEnvColor(env, sMoonlightColour);
        C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvInit(C3D_GetTexEnv(1));
        C3D_TexEnvInit(C3D_GetTexEnv(2));
        return;
    }
    /* texture1 x k + constant x (1 - k) */
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, DappleConstant(dapples));

    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);

    env = C3D_GetTexEnv(2);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, sMoonlight > 0.0f ? GPU_MULTIPLY_ADD : GPU_MODULATE);
    C3D_TexEnvColor(env, sMoonlightColour);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
}

/* The fills' stage 0: the square's colour where the terrain has its texel,
 * graded by the vertex colour as the terrain is; the haze and the fade
 * (stages 3 and 4) stay as the terrain set them. */
static void FillTexEnv(void)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
}

/* The frame's fills (see the fill), under everything else: one quad each,
 * its colour the stage's constant. Leaves the terrain's stages set. */
static void DrawFills(const VoxelLight *dapples)
{
    if (sFillCount == 0 || sFillGpu == NULL)
        return;
    FillTexEnv();
    C3D_AlphaTest(true, GPU_GREATER, 0);
    SetModelView(sFillOriginX, sFillOriginZ);
    BindVertices(sFillGpu);
    /* sorted by colour (PackFills): one draw per run */
    for (unsigned i = 0; i < sFillCount;)
    {
        unsigned end = i + 1u;

        while (end < sFillCount && sFills[end].colour == sFills[i].colour)
            ++end;
        C3D_TexEnvColor(C3D_GetTexEnv(0), sFills[i].colour);
        C3D_DrawArrays(GPU_TRIANGLES, (int)(i * 6u), (int)((end - i) * 6u));
        i = end;
    }
    TerrainTexEnv(dapples);
}

static void ReleaseGlowUnit(void)
{
    uint32_t *flags = (uint32_t *)(void *)(__C3D_Context + C3DI_CONTEXT_FLAGS);
    C3D_Tex **bound = (C3D_Tex **)(void *)(__C3D_Context + C3DI_CONTEXT_TEX);

    if (sGlowBound == NULL)
        return;
    if (bound[2] == sGlowBound)
    {
        bound[2] = NULL;
        *flags |= C3DI_FLAG_TEX(2);
    }
    else
        C3D_TexBind(2, &sTreeAtlas);
    sGlowBound = NULL;
}

/*
 * The lamps' glass: their warm colour laid over the lantern. Their pools: light
 * added to the ground in proportion to the ground's own colour (blended
 * ground x (1 + light), light = the warm colour x the pool's falloff), so the
 * paving shows through brighter and warmer instead of under a flat orange.
 */
#define VOXEL_LAMP_POOL_GAIN 0.5f
static void LampTexEnv(float strength, bool ground)
{
    C3D_TexEnv *env;

    TerrainTexEnv(NULL);
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (ground)
    {
        float k = strength * VOXEL_LAMP_POOL_GAIN;

        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvColor(env, 0xFF000000u | (uint32_t)(k * 60.0f + 0.5f) << 16
                             | (uint32_t)(k * 150.0f + 0.5f) << 8 | (uint32_t)(k * 255.0f + 0.5f));
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ONE, GPU_ZERO, GPU_ONE);
        return;
    }
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, ((uint32_t)(strength * 220.0f + 0.5f) << 24) | 0x006BBFFFu);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
}

static void HazeTexEnv(uint32_t colour, int stage)
{
    C3D_TexEnv *env = C3D_GetTexEnv(stage);

    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, colour);
}

/*
 * Screen fades. On the GBA a fade rewrites the shown palette, and the world's
 * textures are baked from the tilesets' own palettes, so the world has to be
 * faded here: environment 4 blends everything drawn from those textures (and
 * the constant colours drawn over them, the cast shadows and the player's
 * silhouette) towards the fade colour. Sprites are decoded from the shown
 * palette and are faded already; they only take the brightness effect, and a
 * haze colour faded with the world so a distant sprite fades with it.
 */
typedef struct
{
    float amount, rgb[3];
} VoxelFade;
static VoxelFade sWorldFade, sSpriteFade;
static float sBrightBg, sBrightObj;
static bool sBrightWhite;

float CtrVoxel_Bloom(void)
{
    return sReady ? sBloomStrength : 0.0f;
}

float CtrVoxel_SwoopNeutral(void)
{
    float k = sSwoop > 1.0f ? 1.0f : sSwoop;

    if (k <= VOXEL_SWOOP_NEUTRAL_START)
        return 0.0f;
    k = (k - VOXEL_SWOOP_NEUTRAL_START) / (1.0f - VOXEL_SWOOP_NEUTRAL_START);
    if (k >= 1.0f)
        return 1.0f;
    return VOXEL_SWOOP_NEUTRAL_POWER == 1.0f ? k : powf(k, VOXEL_SWOOP_NEUTRAL_POWER);
}

void CtrVoxel_SetSwoop(float t, float height, bool cards, bool shadows)
{
    sSwoop = t;
    sSwoopHeight = height < 0.0f ? 0.0f : height > 1.0f ? 1.0f : height;
    sSwoopCards = cards;
    sSwoopShadows = shadows;
}

void CtrVoxel_SetBrightness(float backgrounds, float sprites, bool white)
{
    sBrightBg = backgrounds;
    sBrightObj = sprites;
    sBrightWhite = white;
}

/* Blends of a then b, as one: (1-a)(1-b)x + (a(1-b)A + bB). */
static VoxelFade FadeThen(VoxelFade first, float amount, float target)
{
    VoxelFade out = first;
    float total = 1.0f - (1.0f - first.amount) * (1.0f - amount);

    if (amount <= 0.0f)
        return out;
    out.amount = total;
    for (int c = 0; c < 3; ++c)
        out.rgb[c] = (first.amount * (1.0f - amount) * first.rgb[c] + amount * target) / total;
    return out;
}

static uint32_t FadeColour(float amount, const float rgb[3])
{
    unsigned a = (unsigned)(amount * 255.0f + 0.5f);
    unsigned r = (unsigned)(rgb[0] * 255.0f + 0.5f);
    unsigned g = (unsigned)(rgb[1] * 255.0f + 0.5f);
    unsigned b = (unsigned)(rgb[2] * 255.0f + 0.5f);

    return a << 24 | b << 16 | g << 8 | r;
}

static void PrepareFades(void)
{
    VoxelFade none = {0.0f, {0.0f, 0.0f, 0.0f}};
    float target = sBrightWhite ? 1.0f : 0.0f;

    sWorldFade = none;
    /* Under the Fly cinematic's cloud the new map fades in from black for
     * the game's sake only: the world is to be seen coming down out of it. */
    if (!(sFly.active && sFly.noFade))
        VoxelWorld_ScreenFade(&sWorldFade.amount, sWorldFade.rgb);
    sWorldFade = FadeThen(sWorldFade, sBrightBg, target);
    sSpriteFade = FadeThen(none, sBrightObj, target);
}

/* What the distance fades to this frame (VoxelLight.hazeRgb). */
static float sHazeRgb[3];

/* The haze colour a sprite pass uses: the world's, faded as the world is. */
static uint32_t SpriteHaze(void)
{
    float rgb[3];

    for (int c = 0; c < 3; ++c)
        rgb[c] = sHazeRgb[c] + (sWorldFade.rgb[c] - sHazeRgb[c]) * sWorldFade.amount;
    return FadeColour(1.0f, rgb);
}

static void FadeTexEnv(const VoxelFade *fade, int stage)
{
    C3D_TexEnv *env = C3D_GetTexEnv(stage);

    C3D_TexEnvInit(env);
    if (fade->amount <= 0.0f)
        return;
    /* constant x a + previous x (1 - a) */
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_CONSTANT);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, FadeColour(fade->amount, fade->rgb));
}

/* Environments 3 and 4 for what is drawn next: the world's or a sprite's. */
static void FadeFor(bool sprites)
{
    HazeTexEnv(sprites ? SpriteHaze() : FadeColour(1.0f, sHazeRgb), 3);
    FadeTexEnv(sprites ? &sSpriteFade : &sWorldFade, 4);
}

/*
 * The windows' glow over the lit texel: stage 1's alpha carries the mask x
 * the strength (its colour never reads the constant), and the glow mixes the
 * warm colour in after the texture. Plain, that is stage 2. With the dapples
 * (NULL for none) stages 0-2 are theirs, so the glow is stage 3 and the haze
 * and the fade move on to 4 and 5; CtrVoxel_Draw puts them back after the
 * building pass.
 */
static void BuildingGlowEnv(C3D_Tex *mask, float strength, const VoxelLight *dapples)
{
    C3D_TexEnv *env;
    bool glow = mask != NULL && strength > 0.0f;
    /* stages 0-2 taken (the dapples', or the map's), and the texel's unit */
    bool three = dapples != NULL || sShadowOn;
    GPU_TEVSRC texel = sShadowOn ? GPU_TEXTURE1 : GPU_TEXTURE0;

    if (sShadowOn)
        dapples = NULL;
    TerrainTexEnv(dapples);
    if (three)
    {
        HazeTexEnv(FadeColour(1.0f, sHazeRgb), glow ? 4 : 3);
        FadeTexEnv(&sWorldFade, glow ? 5 : 4);
        if (!glow)
            C3D_TexEnvInit(C3D_GetTexEnv(5));
    }
    if (!glow)
    {
        ReleaseGlowUnit();
        return;
    }
    C3D_TexBind(2, mask);
    sGlowBound = mask;
    env = C3D_GetTexEnv(1);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE2, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    C3D_TexEnvColor(env, (uint32_t)(strength * 255.0f + 0.5f) << 24);
    /* With the dapples stage 2 passes stage 1's alpha on (TerrainTexEnv). */
    env = C3D_GetTexEnv(three ? 3 : 2);
    if (three)
        C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, texel, texel, texel);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFFA0D8FFu);
}

/* Outdoors and under the ground; a building keeps the game's own sprites. */
bool CtrVoxel_DrawsFog(void)
{
    return sReady && sOwnsFog;
}

bool CtrVoxel_DrawsWeather(void)
{
    return sReady && sOwnsWeather;
}

bool CtrVoxel_DrawsSand(void)
{
    return sReady && sOwnsWeather && VoxelWorld_Weather() == VOXEL_WEATHER_SAND;
}

const C3D_Tex *CtrVoxel_Gloom(float *x, float *y, float *size, float *amount)
{
#if CTR_VOXEL_LIGHTING
    if (!sReady || !sHaveGloom || sGloomAmount <= 0.0f)
        return NULL;
    *x = sGloomX;
    *y = sGloomY;
    *size = VOXEL_GLOOM_SIZE;
    *amount = sGloomAmount;
    return &sGloomTex;
#else
    (void)x;
    (void)y;
    (void)size;
    (void)amount;
    return NULL;
#endif
}

#if CTR_VOXEL_LIGHTING
/*
 * The two banks (MakeFog), after the sprites. Each sheet is centred on the
 * camera, moved in whole repeats of its pattern plus the drift, so the mist
 * stays put on the ground as the player walks and only the wind moves it.
 * The grade is the identity with the frame's haze kept, so the shade carries
 * the coverage unchanged (alpha = texture x shade, doubled back) while the
 * haze still sinks the far banks into the distance colour.
 */
static void DrawBanks(const C3D_Mtx *view, const VoxelLight *light, const FogBank *banks, uint32_t colour,
                      float density)
{
    double seconds = EffectSeconds();
    VoxelLight graded = *light;
    C3D_TexEnv *env;

    for (int c = 0; c < 3; ++c)
        graded.sun[c] = graded.shade[c] = 1.0f;
    SetGrade(&graded);
    C3D_TexBind(0, &sFogTex);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
    BindVertices(sFogSheets);

    for (unsigned bank = 0; bank < 2; ++bank)
    {
        float period = banks[bank].period;
        float dx = (float)fmod(seconds * banks[bank].driftX, period);
        float dz = (float)fmod(seconds * banks[bank].driftZ, period);
        float strength = banks[bank].strength * density;
        C3D_Mtx model;

        Mtx_Copy(&model, view);
        Mtx_Translate(&model, floorf(sCamera.targetX / period) * period + dx,
                      sCamera.ground + banks[bank].height,
                      floorf(sCamera.targetZ / period) * period + dz, true);
        UploadModelView(&model);
        SetOrigin(0.0f, 0.0f);

        /* 0: the fog colour, coverage = texture x shade, doubled. */
        env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
        C3D_TexEnvColor(env, colour);
        /* 1: x the bank's strength. */
        env = C3D_GetTexEnv(1);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvColor(env, (uint32_t)(strength * 255.0f + 0.5f) << 24);
        /* 2: through - the dapples' stage would modulate by this texture. */
        C3D_TexEnvInit(C3D_GetTexEnv(2));
        C3D_DrawArrays(GPU_TRIANGLES, (int)(bank * VOXEL_FOG_SHEET_VERTICES),
                       VOXEL_FOG_SHEET_VERTICES);
    }
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    TerrainTexEnv(NULL);
    SetGrade(light);
}

static void DrawFogBanks(const C3D_Mtx *view, const VoxelLight *light, bool cave, float density)
{
    DrawBanks(view, light, sFogBanks, cave ? 0xFFBFAB9Eu : 0xFFF2EBE6u, density); /* 0xAABBGGRR */
}

/* The Fly cinematic's wisps (sCloudBanks), white, streaming back along the
 * way of travel: the drift is the direction's, so a change of direction
 * between two flights steps the pattern, which nobody sees at no strength. */
static void DrawCloudWisps(const C3D_Mtx *view, const VoxelLight *light, float strength)
{
    for (unsigned bank = 0; bank < 2; ++bank)
    {
        float speed = VOXEL_FLY_WISP_SPEED * (bank == 0 ? 0.7f : 1.0f);

        sCloudBanks[bank].driftX = -sFly.dirX * speed;
        sCloudBanks[bank].driftZ = -sFly.dirZ * speed;
    }
    DrawBanks(view, light, sCloudBanks, 0xFFFBF8F4u, strength);
}

/*
 * The rain, the sandstorm and the volcanic ash, in the scene instead of over
 * it.
 *
 * On the GBA each is a handful of sprites laid over the screen: streaks that
 * slide down the glass, a 64x64 picture of sand or ash scrolled across it
 * (two thousand tiles of the 3DS's wide view, drawn every frame). Here they
 * are particles in the air around the camera, at their own depth: a roof
 * hides the rain that falls behind it, near drops are larger and faster than
 * far ones, the distance haze swallows the far ones, and with the 3D slider
 * every drop stands where it is.
 *
 * Each particle has a fixed place in a box (WeatherVertex) and nothing is
 * rewritten as it falls: the weather's program (voxel_weather.v.pica) moves
 * the box's contents by one offset a draw and wraps every particle back in,
 * fading it out towards the box's sides so none is seen to wrap. The box
 * follows the camera while the particles keep to the world: walking leaves
 * the rain where it falls. A weather is two or three groups of particles,
 * each its own draw with its own speed, wind and size, which gives the depth
 * and the flutter one group could not. How many of each group are drawn
 * follows the game's own weather as it comes and goes
 * (VoxelWorld_WeatherAmount).
 *
 * The CPU cost is a dozen uniforms a draw. The buffers - VOXEL_STREAKS
 * streaks, two triangles each, and VOXEL_FLAKES soft flakes, four - are made
 * at start-up; the GPU draws at most a few thousand vertices and the thin
 * fill of the particles themselves, and the sand's dust takes the fog's
 * sheets (DrawBanks). The compositor drops the game's sprites meanwhile
 * (CtrVoxel_DrawsWeather).
 */
#define VOXEL_STREAKS 768
#define VOXEL_FLAKES 512
#define VOXEL_STREAK_VERTICES 6
#define VOXEL_FLAKE_VERTICES 12
#define VOXEL_WEATHER_GROUPS 3
/* A flake's corners against its middle: soft, but with a body to it. */
#define VOXEL_FLAKE_RIM 0.35f
/* The box's south side, tiles north of the camera's eye: the bottom of the
 * picture looks down through the air just in front of it. */
#define VOXEL_WEATHER_SOUTH 1.0f
/* How sharply a particle fades in from the box's sides, its north and south
 * and its top and bottom: x 1/2 the box, from where it is seen whole to the
 * side. Narrow to the south, where the bottom of the picture is. */
#define VOXEL_WEATHER_EDGE 8.0f
#define VOXEL_WEATHER_EDGE_ALONG 24.0f
#define VOXEL_WEATHER_TOP 6.0f
/* Gone this near the lens, whole from LENS + RAMP tiles away: no streak
 * smeared across the picture. */
#define VOXEL_WEATHER_LENS 1.5f
#define VOXEL_WEATHER_LENS_RAMP 1.6f

typedef struct
{
    float velocity[3]; /* tiles per second */
    float width;       /* a streak's width, or a flake's size */
    float length;      /* a streak's length; 0: a flake */
    float alpha;       /* x the weather's own */
} WeatherGroup;

typedef struct
{
    float box[3];       /* tiles: across, up, north to south */
    float low;          /* the box's bottom, tiles over the player's ground */
    uint32_t colour;    /* 0xAABBGGRR, alpha unused */
    float alpha;        /* the most a particle is seen */
    WeatherGroup groups[VOXEL_WEATHER_GROUPS];
} WeatherKind;

/* Cold and silver: long fast streaks in front, finer and slower ones behind
 * them, the wind leaning them all the same way. */
static const WeatherKind sRainKind = {
    {24.0f, 8.5f, 28.0f}, -3.0f, 0xFFF4E8DEu, 0.62f,
    {
        {{-1.6f, -13.0f, 0.5f}, 0.034f, 0.95f, 1.00f},
        {{-1.3f, -11.0f, 0.4f}, 0.028f, 0.75f, 0.85f},
        {{-0.9f, -8.5f, 0.3f}, 0.020f, 0.45f, 0.60f},
    },
};

/* Fine grains blown along the ground in long low gusts, warm. */
static const WeatherKind sSandKind = {
    {24.0f, 5.0f, 28.0f}, -1.5f, 0xFF7EB8E6u, 0.55f,
    {
        {{-13.0f, -0.6f, 1.0f}, 0.030f, 1.10f, 1.00f},
        {{-17.0f, -0.3f, 0.6f}, 0.024f, 1.60f, 0.75f},
        {{-9.5f, 0.4f, 1.6f}, 0.040f, 0.60f, 0.85f},
    },
};

/* Soft pale flakes drifting down, each layer on its own breeze; large and
 * light enough to read against the grey ground. */
static const WeatherKind sAshKind = {
    {24.0f, 8.0f, 28.0f}, -2.0f, 0xFFE2E6EAu, 1.00f,
    {
        {{-0.50f, -0.75f, 0.15f}, 0.120f, 0.0f, 1.00f},
        {{-0.25f, -1.00f, -0.12f}, 0.100f, 0.0f, 0.95f},
        {{-0.80f, -0.55f, 0.30f}, 0.145f, 0.0f, 0.85f},
    },
};

static void LoadWeatherProgram(void)
{
    if (sDvlb->numDVLE < 2)
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: voxel.shbin has no weather program");
        return;
    }
    shaderProgramInit(&sWeatherProgram);
    if (R_FAILED(shaderProgramSetVsh(&sWeatherProgram, &sDvlb->DVLE[1])))
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: the weather program does not bind");
        shaderProgramFree(&sWeatherProgram);
        return;
    }
    sUniBoxOffset = shaderInstanceGetUniformLocation(sWeatherProgram.vertexShader, "boxOffset");
    sUniBoxSize = shaderInstanceGetUniformLocation(sWeatherProgram.vertexShader, "boxSize");
    sUniBoxBase = shaderInstanceGetUniformLocation(sWeatherProgram.vertexShader, "boxBase");
    sUniAcross = shaderInstanceGetUniformLocation(sWeatherProgram.vertexShader, "across");
    sUniAlong = shaderInstanceGetUniformLocation(sWeatherProgram.vertexShader, "along");
    /* The world's uniforms must be where the world's program has them: one
     * set of registers serves both (voxel_weather.v.pica). */
    if (sUniBoxOffset < 0 || sUniBoxSize < 0 || sUniBoxBase < 0 || sUniAcross < 0 || sUniAlong < 0
     || shaderInstanceGetUniformLocation(sWeatherProgram.vertexShader, "projection") != sUniProjection
     || shaderInstanceGetUniformLocation(sWeatherProgram.vertexShader, "modelView") != sUniModelView
     || shaderInstanceGetUniformLocation(sWeatherProgram.vertexShader, "fog") != sUniFog)
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: the weather program's uniforms do not match");
        shaderProgramFree(&sWeatherProgram);
        return;
    }
    sHaveWeatherProgram = true;
}

static int16_t WeatherFraction(float f)
{
    return (int16_t)(f * 32767.0f);
}

static void WeatherCorner(WeatherVertex *v, int across, int along, const int16_t place[3], float alpha)
{
    v->across = (int8_t)across;
    v->along = (int8_t)along;
    v->pad[0] = v->pad[1] = 0;
    v->x = place[0];
    v->y = place[1];
    v->z = place[2];
    v->alpha = (int16_t)(alpha * VOXEL_SHADE_SCALE);
}

/*
 * A streak runs from its head (along 0), where it is brightest, back up its
 * fall to a tail that is gone (along 64): the eye's blur of a falling drop.
 * A flake is a fan of four triangles, bright in the middle and fainter at
 * its corners (VOXEL_FLAKE_RIM). Sizes, lengths and brightness vary a little from
 * one to the next. Not fatal: without them the game's sprites stay.
 */
static void MakeWeather(void)
{
    sStreaks = linearAlloc(VOXEL_STREAKS * VOXEL_STREAK_VERTICES * sizeof(WeatherVertex));
    sFlakes = linearAlloc(VOXEL_FLAKES * VOXEL_FLAKE_VERTICES * sizeof(WeatherVertex));
    if (sStreaks == NULL || sFlakes == NULL)
    {
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no linear memory for the weather");
        linearFree(sStreaks);
        linearFree(sFlakes);
        sStreaks = sFlakes = NULL;
        return;
    }
    for (unsigned i = 0; i < VOXEL_STREAKS; ++i)
    {
        WeatherVertex *v = sStreaks + i * VOXEL_STREAK_VERTICES;
        int16_t place[3] = {WeatherFraction(MoteRandom(i, 21)), WeatherFraction(MoteRandom(i, 22)),
                            WeatherFraction(MoteRandom(i, 23))};
        int half = 44 + (int)(MoteRandom(i, 24) * 20.0f);
        int tail = 38 + (int)(MoteRandom(i, 25) * 26.0f);
        float alpha = 0.55f + 0.45f * MoteRandom(i, 26);

        WeatherCorner(&v[0], -half, 0, place, alpha);
        WeatherCorner(&v[1], half, 0, place, alpha);
        WeatherCorner(&v[2], half, tail, place, 0.0f);
        v[3] = v[0];
        v[4] = v[2];
        WeatherCorner(&v[5], -half, tail, place, 0.0f);
    }
    GSPGPU_FlushDataCache(sStreaks, VOXEL_STREAKS * VOXEL_STREAK_VERTICES * sizeof(WeatherVertex));
    for (unsigned i = 0; i < VOXEL_FLAKES; ++i)
    {
        static const int fan[4][2][2] = {
            {{0, 1}, {1, 0}}, {{1, 0}, {0, -1}}, {{0, -1}, {-1, 0}}, {{-1, 0}, {0, 1}},
        };
        WeatherVertex *v = sFlakes + i * VOXEL_FLAKE_VERTICES;
        int16_t place[3] = {WeatherFraction(MoteRandom(i, 31)), WeatherFraction(MoteRandom(i, 32)),
                            WeatherFraction(MoteRandom(i, 33))};
        int size = 40 + (int)(MoteRandom(i, 34) * 24.0f);
        float alpha = 0.70f + 0.30f * MoteRandom(i, 35), rim = alpha * VOXEL_FLAKE_RIM;

        for (unsigned k = 0; k < 4; ++k)
        {
            WeatherCorner(&v[k * 3], 0, 0, place, alpha);
            WeatherCorner(&v[k * 3 + 1], fan[k][0][0] * size, fan[k][0][1] * size, place, rim);
            WeatherCorner(&v[k * 3 + 2], fan[k][1][0] * size, fan[k][1][1] * size, place, rim);
        }
    }
    GSPGPU_FlushDataCache(sFlakes, VOXEL_FLAKES * VOXEL_FLAKE_VERTICES * sizeof(WeatherVertex));
}

static float Normalise3(float v[3])
{
    float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    if (length > 0.0001f)
        for (int i = 0; i < 3; ++i)
            v[i] /= length;
    return length;
}

/* Where in the box the particles of a group are this frame, as a fraction of
 * it wrapped to [0, 1): the world's drift since a fixed time, less the box's
 * own corner. */
static float WeatherOffset(double seconds, float velocity, float corner, float size)
{
    double f = (seconds * velocity - corner) / size;

    return (float)(f - floor(f));
}

/*
 * Colour is the weather's constant; alpha is the vertex's (shade x fade x
 * haze, see the program) x the group's x the weather's, doubled back. Depth
 * tested against the finished world, never written. A lightning bolt lights
 * the rain up with it.
 */
static void DrawWeather(const C3D_Mtx *projection, const C3D_Mtx *view, const VoxelLight *light,
                        VoxelWeatherClass weather, float amount)
{
    const WeatherKind *kind = weather == VOXEL_WEATHER_RAIN ? &sRainKind
                            : weather == VOXEL_WEATHER_SAND ? &sSandKind : &sAshKind;
    bool flakes = weather == VOXEL_WEATHER_ASH;
    const WeatherVertex *vertices = flakes ? sFlakes : sStreaks;
    unsigned count = flakes ? VOXEL_FLAKES : VOXEL_STREAKS;
    unsigned per = flakes ? VOXEL_FLAKE_VERTICES : VOXEL_STREAK_VERTICES;
    unsigned group = count / VOXEL_WEATHER_GROUPS;
    /* One time a frame: the right eye's particles where the left eye's are. */
    static double seconds;
    float corner[3] = {sCamera.targetX - kind->box[0] * 0.5f, sCamera.ground + kind->low,
                       sCamera.z - VOXEL_WEATHER_SOUTH - kind->box[2]};
    float forward[3] = {sCamera.targetX - sCamera.x, sCamera.targetY - sCamera.y, sCamera.targetZ - sCamera.z};
    float flash = weather == VOXEL_WEATHER_RAIN ? VoxelWorld_Lightning() : 0.0f;
    float alpha = kind->alpha * (1.0f + 0.5f * flash);
    uint32_t colour = kind->colour;
    VoxelLight graded = *light;
    C3D_AttrInfo *attr;
    C3D_BufInfo *buf;
    C3D_TexEnv *env;
    C3D_Mtx model;

    if (sDrawEye != 1)
        seconds = fmod(EffectSeconds(), 86400.0);
    Normalise3(forward);
    if (flash > 0.0f)
        colour = 0xFFFFFFFFu;
    C3D_BindProgram(&sWeatherProgram);
    sPlainBound = false;
    attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 1, GPU_BYTE, 4);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);
    buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, vertices, sizeof(WeatherVertex), 2, 0x10);

    /* The rays left their own projection and model-view behind. */
    SetProjection(projection);
    Mtx_Copy(&model, view);
    Mtx_Translate(&model, (float)sDrawAnchorX, 0.0f, (float)sDrawAnchorZ, true);
    UploadModelView(&model);
    /* The haze as the world's, with no grade: only fog is read. */
    for (int c = 0; c < 3; ++c)
        graded.sun[c] = graded.shade[c] = 1.0f;
    SetGrade(&graded);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniBoxSize, kind->box[0], kind->box[1], kind->box[2],
                  VOXEL_WEATHER_EDGE);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniBoxBase, corner[0] - (float)sDrawAnchorX, corner[1],
                  corner[2] - (float)sDrawAnchorZ, VOXEL_WEATHER_TOP);

    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    C3D_TexEnvInit(C3D_GetTexEnv(2));
    FadeFor(false);

    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);

    for (unsigned g = 0; g < VOXEL_WEATHER_GROUPS; ++g)
    {
        const WeatherGroup *w = &kind->groups[g];
        unsigned shown = (unsigned)(group * amount + 0.5f);
        float strength = alpha * w->alpha;
        float across[3], along[3];

        if (shown == 0 || strength <= 0.0f)
            continue;
        if (shown > group)
            shown = group;
        if (w->length > 0.0f)
        {
            /* Back up the fall, and across it as the camera sees it. */
            float back[3] = {-w->velocity[0], -w->velocity[1], -w->velocity[2]};

            Normalise3(back);
            across[0] = back[1] * forward[2] - back[2] * forward[1];
            across[1] = back[2] * forward[0] - back[0] * forward[2];
            across[2] = back[0] * forward[1] - back[1] * forward[0];
            Normalise3(across);
            for (int c = 0; c < 3; ++c)
            {
                across[c] *= w->width;
                along[c] = back[c] * w->length;
            }
        }
        else
        {
            /* Turned to the camera: the view's right and up rows. */
            across[0] = view->r[0].x * w->width;
            across[1] = view->r[0].y * w->width;
            across[2] = view->r[0].z * w->width;
            along[0] = view->r[1].x * w->width;
            along[1] = view->r[1].y * w->width;
            along[2] = view->r[1].z * w->width;
        }
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniAcross, across[0], across[1], across[2], VOXEL_WEATHER_LENS);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniAlong, along[0], along[1], along[2],
                      1.0f / VOXEL_WEATHER_LENS_RAMP);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniBoxOffset,
                      WeatherOffset(seconds, w->velocity[0], corner[0], kind->box[0]),
                      WeatherOffset(seconds, w->velocity[1], corner[1], kind->box[1]),
                      WeatherOffset(seconds, w->velocity[2], corner[2], kind->box[2]),
                      VOXEL_WEATHER_EDGE_ALONG);
        C3D_TexEnvColor(C3D_GetTexEnv(0), (colour & 0xFFFFFFu) | 0xFF000000u);
        C3D_TexEnvColor(C3D_GetTexEnv(1),
                        (uint32_t)((strength > 1.0f ? 1.0f : strength) * 255.0f + 0.5f) << 24);
        C3D_DrawArrays(GPU_TRIANGLES, (int)(g * group * per), (int)(shown * per));
    }

    /* The world's program and vertices back, for what draws after. */
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    BindWorldProgram(sPlainDraw);
    attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);
    TerrainTexEnv(NULL);
    SetGrade(light);
}

/* Where the player is on the logical surface, for the dark of a cave. */
static void PlaceGloom(const C3D_Mtx *projection, const C3D_Mtx *view)
{
    C3D_FVec eye = Mtx_MultiplyFVec4(view, FVec4_New(sCamera.targetX, sCamera.ground + 1.0f,
                                                      sCamera.targetZ, 1.0f));
    C3D_FVec clip = Mtx_MultiplyFVec4(projection, eye);

    sGloomX = CTR_GAME_WIDTH * 0.5f;
    sGloomY = CTR_GAME_HEIGHT * 0.5f;
    if (clip.w > 0.0001f)
    {
        sGloomX = (clip.x / clip.w + 1.0f) * 0.5f * CTR_GAME_WIDTH;
        sGloomY = (1.0f - clip.y / clip.w) * 0.5f * CTR_GAME_HEIGHT;
    }
}
#endif

/*
 * Static models (voxel_building.c): the way a game draws a detailed prop.
 * Its mesh is converted once - lit, packed, its page's uv - into linear
 * memory the GPU reads in place, and drawn wherever a chunk on screen places
 * it with that spot as the model-view's translation. Nothing of it goes
 * through a chunk build, its scratch or its VRAM: a model of thousands of
 * cubes costs the chunk nothing and the frame one draw.
 *
 * Kept per (page model, lit) while used; the least recently drawn makes way
 * when the cache is full.
 */
#define VOXEL_STATIC_MESHES 64u

typedef struct StaticMesh
{
    bool used, lit;
    unsigned pageModel;
    VoxelGpuVertex *vertices;   /* linear memory: the vertices, then the indices */
    uint16_t *indices;
    unsigned count;             /* indices */
    unsigned bytes;             /* the size of `vertices`' block, as allocated */
    uint32_t stamp;
    /* A lit one's sun: its direction and strength, all of it ModelShade
     * reads. Under another the mesh is stale (StaticMeshFresh): drawn as it
     * is until its replacement is converted, which then takes its slot - one
     * slot a (page model, lit), stale or not. */
    float sunDx, sunDz, sunStrength;
    int sunQuality; /* SHADOW QUALITY: HIGH signs the faces turned away */
} StaticMesh;

static StaticMesh sStaticMeshes[VOXEL_STATIC_MESHES];
static bool sStaticFailLogged;

/*
 * The linear memory of meshes replaced while a draw of this frame may still
 * read them - a stale mesh drawn this frame, its replacement finished after
 * FrameEnd - freed at the next update, once FrameBegin has waited for the GPU
 * to finish the frame (StaticMeshesLateFree). A slot at most once a frame: it
 * is replaced only from stale to current, and goes stale only in an update.
 */
static void *sStaticLate[VOXEL_STATIC_MESHES];
static unsigned sStaticLateCount;

static void StaticMeshesLateFree(void)
{
    for (unsigned i = 0; i < sStaticLateCount; ++i)
        linearFree(sStaticLate[i]);
    sStaticLateCount = 0;
}

static void StaticMeshFree(StaticMesh *mesh)
{
    if (mesh->vertices != NULL)
        linearFree(mesh->vertices);
    memset(mesh, 0, sizeof(*mesh));
}

static void StaticBuildEnd(bool bad);

static void StaticMeshesFree(void)
{
    StaticBuildEnd(false);
    for (unsigned i = 0; i < VOXEL_STATIC_MESHES; ++i)
        StaticMeshFree(&sStaticMeshes[i]);
    StaticMeshesLateFree();
}

/* Lit under the sun there is now, or not lit at all: what converting it
 * again would make. */
static bool StaticMeshFresh(const StaticMesh *mesh)
{
#if CTR_VOXEL_LIGHTING
    return !mesh->lit || (mesh->sunDx == gVoxelSun.dx && mesh->sunDz == gVoxelSun.dz
                          && mesh->sunStrength == gVoxelSun.strength
                          && mesh->sunQuality == VoxelLighting_Quality());
#else
    (void)mesh;
    return true;
#endif
}

static StaticMesh *StaticMeshSlot(unsigned pageModel, bool lit)
{
    for (unsigned i = 0; i < VOXEL_STATIC_MESHES; ++i)
        if (sStaticMeshes[i].used && sStaticMeshes[i].pageModel == pageModel
         && sStaticMeshes[i].lit == lit)
            return &sStaticMeshes[i];
    return NULL;
}

/* The cached mesh of (page model, lit) as converting it now would make it,
 * marked as used this frame; NULL if there is none, or only a stale one. */
static const StaticMesh *StaticMeshCurrent(unsigned pageModel, bool lit)
{
    StaticMesh *mesh = StaticMeshSlot(pageModel, lit);

    if (mesh == NULL || !StaticMeshFresh(mesh))
        return NULL;
    mesh->stamp = sFrame;
    return mesh;
}

/*
 * The static meshes the draws of this frame asked for: missing ones, which
 * the cache gave up since their chunk was built, and stale ones, drawn lit
 * under another sun. Converted after FrameEnd (StaticWantsRun), never inside
 * the draw: a missing model a slice every frame whatever the budget, as the
 * draw used to, and more while the budget lasts; a stale one, on screen as
 * it is meanwhile, in what the chunks' jobs leave of the budget. A missing
 * one takes the place of a stale one in a full list.
 */
#define VOXEL_STATIC_WANTS 16u

static struct
{
    unsigned pageModel;
    bool lit, missing;
} sStaticWants[VOXEL_STATIC_WANTS];
static unsigned sStaticWantCount;
static uint32_t sStaticWantFrame = ~0u; /* the frame the list is for */

static void StaticWant(unsigned pageModel, bool lit, bool missing)
{
    unsigned at = sStaticWantCount;

    if (sStaticWantFrame != sFrame)
    {
        sStaticWantFrame = sFrame;
        sStaticWantCount = at = 0;
    }
    for (unsigned i = 0; i < sStaticWantCount; ++i)
        if (sStaticWants[i].pageModel == pageModel && sStaticWants[i].lit == lit)
        {
            sStaticWants[i].missing = sStaticWants[i].missing || missing;
            return;
        }
    if (at == VOXEL_STATIC_WANTS)
    {
        if (!missing)
            return; /* asked again next frame */
        for (at = 0; at < VOXEL_STATIC_WANTS && sStaticWants[at].missing; ++at)
            ;
        if (at == VOXEL_STATIC_WANTS)
            return;
    }
    else
        ++sStaticWantCount;
    sStaticWants[at].pageModel = pageModel;
    sStaticWants[at].lit = lit;
    sStaticWants[at].missing = missing;
}

/* The mesh to draw for (page model, lit), marked as drawn this frame: the
 * current one, or the stale one until its replacement is converted - which
 * is asked for then (StaticWant). NULL if there is neither. */
static const struct StaticMesh *StaticMeshFind(unsigned pageModel, bool lit)
{
    StaticMesh *mesh = StaticMeshSlot(pageModel, lit);

    if (mesh == NULL)
        return NULL;
    mesh->stamp = sFrame;
    if (!StaticMeshFresh(mesh))
        StaticWant(pageModel, lit, false);
    return mesh;
}

/*
 * The one static mesh being converted, a slice of triangles at a time: the
 * dome of the Battle Frontier is a few thousand, and converted whole inside
 * the draw it held the frame 15-28 ms on hardware. A chunk's job converts
 * the models it places before it is finished (JOB_STATICS), so the chunk
 * appears with them; the draw only finishes one the cache gave up meanwhile.
 *
 * Packed, then shared: a face's two triangles meet on two corners, so a
 * vertex identical to one already kept is that one again (open addressing
 * on its words, in an ordinary heap scratch).
 */
static struct
{
    bool active, lit;
    unsigned pageModel;
    const VoxelVertex *src;
    unsigned count, done, unique, buckets;
    float ox, oy, su, sv;
    float sunDx, sunDz, sunStrength; /* the sun it is lit under (StaticMesh) */
    int sunQuality;
    VoxelGpuVertex *packed;
    uint16_t *index;
    int32_t *table;
} sStaticBuild;

/* Page models that cannot be converted (too large): never again. */
#define VOXEL_STATIC_BAD_BITS 8192u
static uint32_t sStaticBad[VOXEL_STATIC_BAD_BITS / 32u];

/* ... and the one that found no memory (linear, or the heap for its
 * scratch), not before this frame. */
#define VOXEL_STATIC_RETRY_FRAMES 120u
static unsigned sStaticRetryModel = ~0u;
static uint32_t sStaticRetryFrame;

static bool StaticBad(unsigned pageModel)
{
    if (pageModel == sStaticRetryModel && sFrame < sStaticRetryFrame)
        return true;
    return pageModel < VOXEL_STATIC_BAD_BITS && (sStaticBad[pageModel >> 5] >> (pageModel & 31u)) & 1u;
}

static void StaticBuildEnd(bool bad)
{
    if (bad && sStaticBuild.pageModel < VOXEL_STATIC_BAD_BITS)
        sStaticBad[sStaticBuild.pageModel >> 5] |= 1u << (sStaticBuild.pageModel & 31u);
    free(sStaticBuild.packed);
    free(sStaticBuild.index);
    free(sStaticBuild.table);
    memset(&sStaticBuild, 0, sizeof(sStaticBuild));
}

#if CTR_VOXEL_LIGHTING
/*
 * A new sun: every lit mesh is stale (StaticMeshFresh), and drawn as it is
 * until its replacement is converted - by the job of a chunk that places it
 * (JOB_STATICS), or after FrameEnd for one on screen (StaticWantsRun) - as a
 * stale chunk is. They used to be freed here: every model in view was gone
 * until converted again, a slice a frame inside the draw, some thirty frames
 * for one of two thousand triangles; and with DAY / NIGHT on every door is a
 * new sun, there and back. Back under a sun a mesh was lit under, it is
 * current again as it stands. Only the one being converted goes: what of it
 * is converted is the old sun's.
 */
static void StaticMeshesRelight(void)
{
    if (sStaticBuild.active && sStaticBuild.lit)
        StaticBuildEnd(false);
}
#endif

static bool StaticBuildStart(unsigned pageModel, bool lit)
{
    unsigned count;

    memset(&sStaticBuild, 0, sizeof(sStaticBuild));
    sStaticBuild.pageModel = pageModel;
    sStaticBuild.lit = lit;
#if CTR_VOXEL_LIGHTING
    /* (a new sun ends the conversion: StaticMeshesRelight) */
    sStaticBuild.sunDx = gVoxelSun.dx;
    sStaticBuild.sunDz = gVoxelSun.dz;
    sStaticBuild.sunStrength = gVoxelSun.strength;
    sStaticBuild.sunQuality = VoxelLighting_Quality();
#endif
    if (!VoxelBuildings_StaticModel(pageModel, &sStaticBuild.src, &count, &sStaticBuild.ox,
                                    &sStaticBuild.oy, &sStaticBuild.su, &sStaticBuild.sv)
     || count < 3u || count > 0xFFFFu * 3u)
    {
        StaticBuildEnd(true);
        return false;
    }
    count -= count % 3u;
    sStaticBuild.count = count;
    sStaticBuild.buckets = 1;
    while (sStaticBuild.buckets < count * 2u)
        sStaticBuild.buckets <<= 1;
    sStaticBuild.packed = malloc(count * sizeof(VoxelGpuVertex));
    sStaticBuild.index = malloc(count * sizeof(uint16_t));
    sStaticBuild.table = malloc(sStaticBuild.buckets * sizeof(int32_t));
    if (sStaticBuild.packed == NULL || sStaticBuild.index == NULL || sStaticBuild.table == NULL)
    {
        /* the heap may have room another time */
        StaticBuildEnd(false);
        sStaticRetryModel = pageModel;
        sStaticRetryFrame = sFrame + VOXEL_STATIC_RETRY_FRAMES;
        return false;
    }
    memset(sStaticBuild.table, 0xFF, sStaticBuild.buckets * sizeof(int32_t));
    sStaticBuild.active = true;
    return true;
}

/* Up to `triangles` more of the mesh; true once all are converted. */
static bool StaticBuildSlice(unsigned triangles)
{
    const VoxelVertex *src = sStaticBuild.src;
    unsigned end = sStaticBuild.done + triangles * 3u, mask = sStaticBuild.buckets - 1u;

    if (end > sStaticBuild.count)
        end = sStaticBuild.count;
    for (unsigned k = sStaticBuild.done; k < end; k += 3)
    {
        float shade = src[k].shade;

#if CTR_VOXEL_LIGHTING
        if (sStaticBuild.lit)
            shade = VoxelLighting_ModelShade(&src[k], &src[k + 1], &src[k + 2], src[k].shade);
#endif
        for (unsigned jj = 0; jj < 3; ++jj)
        {
            const VoxelVertex *v = &src[k + jj];
            VoxelGpuVertex g;
            uint32_t words[sizeof(VoxelGpuVertex) / 4u];
            uint32_t h = 2166136261u;
            unsigned at;

            memset(&g, 0, sizeof(g));
            g.u = (sStaticBuild.ox + v->u) * sStaticBuild.su;
            g.v = 1.0f - (sStaticBuild.oy + v->v) * sStaticBuild.sv;
            g.x = Quantise(v->x, VOXEL_POS_SCALE);
            g.y = Quantise(v->y, VOXEL_POS_SCALE);
            g.z = Quantise(v->z, VOXEL_POS_SCALE);
            g.shade = Quantise(sStaticBuild.lit ? shade : v->shade, VOXEL_SHADE_SCALE);
            memcpy(words, &g, sizeof(g));
            for (unsigned t = 0; t < sizeof(words) / 4u; ++t)
                h = (h ^ words[t]) * 16777619u;
            h ^= h >> 15;
            at = h & mask;
            while (sStaticBuild.table[at] >= 0
                && memcmp(&sStaticBuild.packed[sStaticBuild.table[at]], &g, sizeof(g)) != 0)
                at = (at + 1u) & mask;
            if (sStaticBuild.table[at] < 0)
            {
                if (sStaticBuild.unique == 0xFFFFu)
                {
                    StaticBuildEnd(true);
                    return false;
                }
                sStaticBuild.packed[sStaticBuild.unique] = g;
                sStaticBuild.table[at] = (int32_t)sStaticBuild.unique++;
            }
            sStaticBuild.index[k + jj] = (uint16_t)sStaticBuild.table[at];
        }
    }
    sStaticBuild.done = end;
    return end == sStaticBuild.count;
}

/*
 * The converted mesh into linear memory and a cache slot: its stale
 * predecessor's, else a free one, else the one drawn longest ago (never one
 * drawn the last frame drawn). The predecessor, drawn until now, gives up its
 * memory once the new mesh has its own - at once if no draw of this frame
 * reads it, else at the next update (sStaticLate). With no room for both, the
 * new mesh goes into the predecessor's own block if it fits there and no draw
 * still to come reads it, or the predecessor goes first if it is off screen.
 * One this frame drew stays as it is, the conversion held finished
 * (StaticBuildHeld) for the next update to place, before that frame draws
 * either (`update`: FrameBegin has waited for the GPU). There, one the last
 * frame drew and too small for the new mesh is kept, and the model tried
 * again later: drawn stale rather than not at all.
 */
static const StaticMesh *StaticBuildFinish(bool update)
{
    unsigned slot = VOXEL_STATIC_MESHES, unique = sStaticBuild.unique, count = sStaticBuild.count;
    unsigned bytes = unique * sizeof(VoxelGpuVertex) + count * sizeof(uint16_t);
    /* (linearAlloc's own alignment: the block is that large anyway, and so
     * all of it is the mesh's, for the next one in its place) */
    unsigned block = (bytes + 0x7Fu) & ~0x7Fu;
    /* the last frame drawn: this one, or at an update the one before */
    uint32_t shown = update ? sFrame - 1u : sFrame;
    StaticMesh *mesh = StaticMeshSlot(sStaticBuild.pageModel, sStaticBuild.lit);
    VoxelGpuVertex *vertices = NULL;

    if (mesh != NULL)
    {
        vertices = linearAlloc(block);
        if (vertices == NULL && mesh->stamp != sFrame && bytes <= mesh->bytes)
        {
            /* in its own block: nothing of this frame draws it, and the GPU
             * is done with the frames before (as for linearFree below) */
            vertices = mesh->vertices;
            block = mesh->bytes;
            mesh->vertices = NULL;
        }
        else if (vertices == NULL && mesh->stamp != sFrame && mesh->stamp != shown)
        {
            StaticMeshFree(mesh);
            mesh = NULL; /* placed afresh below */
        }
        else if (vertices == NULL && !update)
            return NULL; /* drawn this frame: held for the next update */
        else if (vertices != NULL)
        {
            if (mesh->stamp != sFrame)
                linearFree(mesh->vertices);
            else if (sStaticLateCount < VOXEL_STATIC_MESHES)
                sStaticLate[sStaticLateCount++] = mesh->vertices;
            else
            {
                /* (never: see sStaticLate) kept converted, tried again */
                linearFree(vertices);
                return NULL;
            }
            mesh->vertices = NULL;
        }
    }
    if (mesh == NULL)
    {
        for (unsigned i = 0; i < VOXEL_STATIC_MESHES; ++i)
            if (!sStaticMeshes[i].used)
            {
                slot = i;
                break;
            }
        if (slot == VOXEL_STATIC_MESHES)
        {
            for (unsigned i = 0; i < VOXEL_STATIC_MESHES; ++i)
                if (sStaticMeshes[i].stamp != sFrame && sStaticMeshes[i].stamp != shown
                 && (slot == VOXEL_STATIC_MESHES || sStaticMeshes[i].stamp < sStaticMeshes[slot].stamp))
                    slot = i;
            if (slot == VOXEL_STATIC_MESHES)
                return NULL; /* every one on screen: kept converted, tried again */
            StaticMeshFree(&sStaticMeshes[slot]);
        }
        mesh = &sStaticMeshes[slot];
        vertices = linearAlloc(block);
    }
    if (vertices == NULL)
    {
        /* (a stale predecessor the last frame drew keeps its own: still drawn) */
        if (!sStaticFailLogged)
        {
            sStaticFailLogged = true;
            CtrLog_Write(CTR_LOG_ERROR, "VOXEL static model %u: no linear memory for %u vertices, %u indices",
                         sStaticBuild.pageModel, unique, count);
        }
        /* tried again once memory may have come back, converted anew */
        sStaticRetryModel = sStaticBuild.pageModel;
        sStaticRetryFrame = sFrame + VOXEL_STATIC_RETRY_FRAMES;
        StaticBuildEnd(false);
        return NULL;
    }
    mesh->vertices = vertices;
    mesh->bytes = block;
    mesh->indices = (uint16_t *)(void *)(mesh->vertices + unique);
    memcpy(mesh->vertices, sStaticBuild.packed, unique * sizeof(VoxelGpuVertex));
    memcpy(mesh->indices, sStaticBuild.index, count * sizeof(uint16_t));
    GSPGPU_FlushDataCache(mesh->vertices, bytes);
    mesh->used = true;
    mesh->lit = sStaticBuild.lit;
    mesh->pageModel = sStaticBuild.pageModel;
    mesh->count = count;
    mesh->stamp = sFrame;
    mesh->sunDx = sStaticBuild.sunDx;
    mesh->sunDz = sStaticBuild.sunDz;
    mesh->sunStrength = sStaticBuild.sunStrength;
    mesh->sunQuality = sStaticBuild.sunQuality;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL static model %u: %u vertices, %u indices (%u KiB linear)",
                 mesh->pageModel, unique, count, bytes >> 10);
    StaticBuildEnd(false);
    return mesh;
}

/* A conversion finished that StaticBuildFinish found no place for yet: the
 * converter holds it, and is busy with nothing else, till an update places
 * it at the latest. */
static bool StaticBuildHeld(void)
{
    return sStaticBuild.active && sStaticBuild.done == sStaticBuild.count;
}

/* At the update, before anything of the frame draws: the held conversion
 * into its place, where the stale mesh the last frame drew may make way. */
static void StaticBuildPlaceHeld(void)
{
    if (StaticBuildHeld())
        StaticBuildFinish(true);
}

/*
 * A slice of work towards (page model, lit)'s mesh. True once it is cached,
 * or can never be (nothing more to wait for); false while it is still to
 * come - including while the converter is busy with another, which it
 * carries on with first.
 */
static bool StaticMeshPrepare(unsigned pageModel, bool lit, unsigned triangles)
{
    if (StaticMeshCurrent(pageModel, lit) != NULL || StaticBad(pageModel))
        return true;
    if (!sStaticBuild.active && !StaticBuildStart(pageModel, lit))
        return StaticBad(pageModel);
    if (sStaticBuild.done < sStaticBuild.count && !StaticBuildSlice(triangles))
        return false;
    if (!sStaticBuild.active)
        return false; /* given up in the slice */
    if (sStaticBuild.done == sStaticBuild.count)
    {
        bool mine = sStaticBuild.pageModel == pageModel && sStaticBuild.lit == lit;

        if (StaticBuildFinish(false) == NULL && sStaticBuild.active)
            return false;
        return mine && (StaticMeshCurrent(pageModel, lit) != NULL || StaticBad(pageModel));
    }
    return false;
}

/*
 * After FrameEnd, the static meshes this frame's draws asked for
 * (StaticWant): the `missing` ones - one slice owed every frame, as the draw
 * used to convert one, then more within `budget` from `started` - or the
 * stale ones, within it only. A slice is the converter's next, which may be
 * another model's it is busy with (StaticMeshPrepare); one that moves it on
 * not at all (no slot or memory to finish into) ends the frame's turn.
 * Whether any slice ran.
 */
static float sStaticSliceMs = 0.7f; /* about 11 us a triangle on hardware */
static uint32_t sStaticOwedFrame = ~0u; /* the last frame the owed slice ran */

static bool StaticWantsRun(uint64_t started, float budget, bool missing)
{
    bool ran = false;

    if (sStaticWantFrame != sFrame)
        return false;
    for (unsigned i = 0; i < sStaticWantCount; ++i)
    {
        unsigned pageModel = sStaticWants[i].pageModel;
        bool lit = sStaticWants[i].lit;

        if (sStaticWants[i].missing != missing)
            continue;
        while (StaticMeshCurrent(pageModel, lit) == NULL && !StaticBad(pageModel))
        {
            bool owed = missing && sStaticOwedFrame != sFrame;
            /* (a slice is a fixed 64 triangles: above the cap, a fluke) */
            float cost = sStaticSliceMs < VOXEL_SLICE_CAP_MS ? sStaticSliceMs : VOXEL_SLICE_CAP_MS;
            bool active = sStaticBuild.active;
            unsigned model = sStaticBuild.pageModel, done = sStaticBuild.done;
            uint64_t sliceStart;
            float ms;

            if (!owed && MsSince(started) + cost > budget)
                return ran;
            sliceStart = svcGetSystemTick();
            StaticMeshPrepare(pageModel, lit, VOXEL_STATIC_SLICE_TRIANGLES);
            ms = TicksMs(svcGetSystemTick() - sliceStart);
            sStaticSliceMs = ms > sStaticSliceMs ? ms : sStaticSliceMs * 0.75f + ms * 0.25f;
            sStaticOwedFrame = sFrame;
            ran = true;
            if (active && sStaticBuild.active && sStaticBuild.pageModel == model
             && sStaticBuild.done == done)
                return true;
        }
    }
    return ran;
}

static void SetModelViewAt(const C3D_Mtx *view, float x, float y, float z)
{
    C3D_Mtx model;

    Mtx_Copy(&model, view);
    Mtx_Translate(&model, x, y, z, true);
    SetModelViewMatrix(&model);
    SetOrigin(0.0f, 0.0f);
}

/*
 * The meshes DrawStatics draws, each (page model, lit) looked up once a Draw
 * rather than by StaticMeshFind's walk of the whole cache for every model
 * placed: a street's lamps, a town's fences, the same few meshes many times
 * over. Only a mesh found is kept: one missing is asked for at every model
 * that places it, as before (StaticWant takes each once). What is kept stays
 * that mesh for the rest of the Draw, as nothing converts, replaces or gives
 * up a mesh while a Draw runs - the jobs and StaticWantsRun do, outside it -
 * and the first lookup has stamped it with the frame, as every lookup did.
 */
#define VOXEL_STATIC_MEMO 64u
static struct
{
    uint32_t draw;          /* the Draw it was found in (sDrawSerial) */
    unsigned pageModel;
    bool lit;
    const StaticMesh *mesh;
} sStaticMemo[VOXEL_STATIC_MEMO];

static const StaticMesh *StaticMeshDrawn(unsigned pageModel, bool lit)
{
    unsigned at = ((pageModel * 2u + (lit ? 1u : 0u)) * 2654435761u) >> 26;
    const StaticMesh *mesh;

    _Static_assert(VOXEL_STATIC_MEMO == 64u, "the memo is indexed by the hash's top six bits");
    ++sSetupAsked.finds;
    if (sStaticMemo[at].draw == sDrawSerial && sStaticMemo[at].mesh != NULL
     && sStaticMemo[at].pageModel == pageModel && sStaticMemo[at].lit == lit)
    {
        ++sSetupKept.finds;
        return sStaticMemo[at].mesh;
    }
    mesh = StaticMeshFind(pageModel, lit);
    if (mesh != NULL)
    {
        sStaticMemo[at].draw = sDrawSerial;
        sStaticMemo[at].pageModel = pageModel;
        sStaticMemo[at].lit = lit;
        sStaticMemo[at].mesh = mesh;
    }
    return mesh;
}

/* The static models the chunks on screen place, in the building pass's
 * state (alpha tested, the terrain's TexEnv and grade). */
/* Model draws left out because the model was not ready, in all. */
static unsigned sModelsSkipped;

void CtrVoxel_FrameCounts(unsigned *modelsSkipped, unsigned *spritesLeftOut)
{
    *modelsSkipped = sModelsSkipped;
    *spritesLeftOut = VoxelEntities_SpritesLeftOut();
}

static void DrawStatics(const C3D_Mtx *view, const VoxelLight *dapples, bool glowing)
{
    const C3D_Tex *bound = NULL;

    for (unsigned i = 0; i < sDrawCount; ++i)
    {
        const VoxelChunk *chunk = sDraws[i].chunk;
        C3D_Tex *tex;

        if (chunk->staticCount == 0 || chunk->draft)
            continue;
        tex = BuildingPage(chunk->buildingPage);
        if (tex == NULL)
            continue;
        /* A map's chunks share its page, and with it its windows' mask and
         * environment: set again only for another page, as the passes do. */
        ++sSetupAsked.pages;
        if (tex == bound)
            ++sSetupKept.pages;
        else
        {
            bound = tex;
            BindAtlas(tex);
            if (glowing)
            {
                BuildingPageSlot *page = FindPage(chunk->buildingPage);

                BuildingGlowEnv(GlowTex(page),
                                CtrDayNight_Get()->glow, dapples);
            }
        }
        for (unsigned k = 0; k < chunk->staticCount; ++k)
        {
            const VoxelStaticDraw *s = &chunk->statics[k];
            const StaticMesh *mesh = StaticMeshDrawn(s->pageModel, s->lit);
            float x = (float)sDraws[i].worldX + s->x, z = (float)sDraws[i].worldZ + s->z;

            if (mesh == NULL)
            {
                /* left out of this picture: the frame profiler counts it */
                ++sModelsSkipped;
                /* given up by the cache since its chunk was built: converted
                 * after FrameEnd, a slice a frame at least (StaticWantsRun) */
                StaticWant(s->pageModel, s->lit, true);
                continue;
            }
            SetModelViewAt(view, x, s->y, z);
#if CTR_VOXEL_LIGHTING
            /* The dapples are worked out from the model's own frame: whole
             * tiles of its spot (a model stands on a whole cell), slid along
             * the sun by the height it stands at, as the ground there is. */
            if (dapples != NULL)
                DappleUniforms((int)floorf(x + 0.5f), (int)floorf(z + 0.5f), s->y);
#endif
            BindVertices(mesh->vertices);
            C3D_DrawElements(GPU_TRIANGLES, (int)mesh->count, C3D_UNSIGNED_SHORT, mesh->indices);
        }
    }
}

/*
 * The shadow map for this frame (see "Shadow map"), from the sun, around the
 * camera's target: the chunks to draw, their ranges and textures, as the
 * world's passes have them, and the static models. Its own target with no
 * depth buffer, cleared to the farthest depth and full light; the program
 * and the vertex layout are the Draw's. Leaves the GPU in GL mode and the
 * Draw's target to be put back.
 */
/*
 * The map's extent: the middle camera's corner rays to the ground a little
 * below and above the target (where they rise or reach past the far plane,
 * as far as it), across the sun. Its half extents grow at once and shrink
 * only by whole tiles past two, so that its texels keep their size - and
 * the shadows' edges stand still - while the camera walks.
 */
#define VOXEL_SHADOW_FIT_BELOW 3.0f
#define VOXEL_SHADOW_FIT_ABOVE 4.0f
#define VOXEL_SHADOW_FIT_MARGIN 1.5f   /* the eyes see past the middle */
#define VOXEL_SHADOW_FIT_REACH 60.0f
/* for the log: the extent the rays asked for, and how far the window slid */
static float sShadowFitWanted[2], sShadowFitSlid[2];
static void ShadowMapFit(const float U[3], const float V[3], float *centreU, float *centreV)
{
    float eye[3] = {sCamera.x, sCamera.y, sCamera.z};
    float f[3] = {sCamera.targetX - eye[0], sCamera.targetY - eye[1], sCamera.targetZ - eye[2]};
    float r[3], up[3], length = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    float tanX = CameraTanX();
    float tanY = tanX * (float)CTR_GAME_HEIGHT / (float)CTR_GAME_WIDTH;
    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;
    /* what the bottom of the screen sees, and the target: kept in the map
     * when the far rays need more than it holds */
    float nearMin[2] = {1e9f, 1e9f}, nearMax[2] = {-1e9f, -1e9f};
    float halves[2];

    if (sSwoop > 0.0f || length < 0.001f)
    {
        sShadowHalfU = sShadowHalfV = VOXEL_SHADOW_MAP_HALF;
        *centreU = U[0] * sCamera.targetX + U[1] * sCamera.targetY + U[2] * sCamera.targetZ;
        *centreV = V[0] * sCamera.targetX + V[1] * sCamera.targetY + V[2] * sCamera.targetZ;
        return;
    }
    for (int i = 0; i < 3; ++i)
        f[i] /= length;
    /* right = forward x up, the camera's up = right x forward */
    r[0] = -f[2]; r[1] = 0.0f; r[2] = f[0];
    length = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (length < 0.001f)
    {
        r[0] = 1.0f; r[2] = 0.0f; length = 1.0f;
    }
    r[0] /= length; r[2] /= length;
    up[0] = r[1] * f[2] - r[2] * f[1];
    up[1] = r[2] * f[0] - r[0] * f[2];
    up[2] = r[0] * f[1] - r[1] * f[0];
    for (int corner = 0; corner < 4; ++corner)
    {
        float sx = (corner & 1) ? 1.0f : -1.0f, sy = (corner & 2) ? 1.0f : -1.0f;
        float d[3];

        for (int i = 0; i < 3; ++i)
            d[i] = f[i] + sx * tanX * r[i] + sy * tanY * up[i];
        for (int k = 0; k < 2; ++k)
        {
            float y = sCamera.targetY + (k ? VOXEL_SHADOW_FIT_ABOVE : -VOXEL_SHADOW_FIT_BELOW);
            float s = VOXEL_SHADOW_FIT_REACH, p[3], u, v;

            if (d[1] < -0.0001f && y < eye[1])
            {
                s = (y - eye[1]) / d[1];
                if (s > VOXEL_SHADOW_FIT_REACH)
                    s = VOXEL_SHADOW_FIT_REACH;
            }
            for (int i = 0; i < 3; ++i)
                p[i] = eye[i] + d[i] * s;
            u = U[0] * p[0] + U[1] * p[1] + U[2] * p[2];
            v = V[0] * p[0] + V[1] * p[1] + V[2] * p[2];
            if (u < minU) minU = u;
            if (u > maxU) maxU = u;
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
            if (sy < 0.0f)
            {
                if (u < nearMin[0]) nearMin[0] = u;
                if (u > nearMax[0]) nearMax[0] = u;
                if (v < nearMin[1]) nearMin[1] = v;
                if (v > nearMax[1]) nearMax[1] = v;
            }
        }
    }
    {
        float tu = U[0] * sCamera.targetX + U[1] * sCamera.targetY + U[2] * sCamera.targetZ;
        float tv = V[0] * sCamera.targetX + V[1] * sCamera.targetY + V[2] * sCamera.targetZ;

        if (tu < nearMin[0]) nearMin[0] = tu;
        if (tu > nearMax[0]) nearMax[0] = tu;
        if (tv < nearMin[1]) nearMin[1] = tv;
        if (tv > nearMax[1]) nearMax[1] = tv;
    }
    *centreU = (minU + maxU) * 0.5f;
    *centreV = (minV + maxV) * 0.5f;
    halves[0] = (maxU - minU) * 0.5f + VOXEL_SHADOW_FIT_MARGIN;
    halves[1] = (maxV - minV) * 0.5f + VOXEL_SHADOW_FIT_MARGIN;
    for (int a = 0; a < 2; ++a)
    {
        float *half = a ? &sShadowHalfV : &sShadowHalfU;
        float need = ceilf(halves[a]);

        if (need < VOXEL_SHADOW_MAP_HALF_MIN) need = VOXEL_SHADOW_MAP_HALF_MIN;
        if (need > VOXEL_SHADOW_MAP_HALF) need = VOXEL_SHADOW_MAP_HALF;
        if (need > *half || need < *half - 2.0f)
            *half = need;
    }
    /*
     * More than the map holds (the top of the screen reaching far): it was
     * centred half way, and the ground nearest the camera - the bottom of
     * the screen - fell out of it, its cliffs casting nothing. The window
     * slides back over it: the near side and the target always in, the far
     * side as much as is left.
     */
    for (int a = 0; a < 2; ++a)
    {
        float half = (a ? sShadowHalfV : sShadowHalfU) - VOXEL_SHADOW_FIT_MARGIN;
        float *centre = a ? centreV : centreU;
        float lo = nearMax[a] - half, hi = nearMin[a] + half;
        float was = *centre;

        sShadowFitWanted[a] = halves[a] * 2.0f;

        if (lo > hi) /* the near side alone wider than the map: its middle */
            *centre = (nearMin[a] + nearMax[a]) * 0.5f;
        else if (*centre < lo)
            *centre = lo;
        else if (*centre > hi)
            *centre = hi;
        sShadowFitSlid[a] = *centre - was;
    }
}

/* Everything that shades into the map: the world's passes - of the tree
 * material only the crowns turned to the sun (TreeShadowCards) - then the
 * statics. */
static void ShadowMapCasters(const C3D_Mtx *identity)
{
    for (unsigned pass = 0; pass < VOXEL_ATLAS_PAGES + 2u; ++pass)
    {
        const C3D_Tex *boundTex = NULL;
        bool trees = pass == VOXEL_ATLAS_PAGES;

        /* the crowns' turned cards only, their clear texels dropped: a
         * card's corner writing full light over a roof's shadow */
        C3D_AlphaTest(trees, GPU_GREATER, 0);
        for (unsigned i = 0; i < sDrawCount; ++i)
        {
            const VoxelChunk *chunk = sDraws[i].chunk;
            unsigned first = pass < VOXEL_ATLAS_PAGES ? chunk->terrainFirst[pass]
                           : trees ? chunk->buildingFirst - chunk->treeShadow : chunk->buildingFirst;
            unsigned count = pass < VOXEL_ATLAS_PAGES ? chunk->terrainFirst[pass + 1u] - first
                           : trees ? chunk->treeShadow : chunk->count - first;
            C3D_Tex *tex;

            if (count == 0)
                continue;
            tex = pass < VOXEL_ATLAS_PAGES ? AtlasTex(chunk->atlas, pass)
                : trees ? &sTreeAtlas : BuildingPage(chunk->buildingPage);
            if (tex == NULL)
                continue;
            SetModelView(sDraws[i].worldX, sDraws[i].worldZ);
            if (boundTex != tex)
            {
                boundTex = tex;
                C3D_TexBind(0, tex);
            }
            C3D_DrawArrays(GPU_TRIANGLES, BindChunkVertices(chunk, first, count), (int)count);
        }
        if (pass == VOXEL_ATLAS_PAGES + 1u && sHaveBuildings)
            DrawStatics(identity, NULL, false);
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
}

/*
 * The map as it was last drawn, kept while nothing it holds has changed: its
 * window on the world (whole texels of it, so the texels still lie where
 * they did), what was cast into it and the sun. A camera standing still drew
 * the same map again every frame - every chunk on screen a second time - and
 * one walking, as often as not: the window only has to hold the picture, and
 * holds it with tiles to spare (ShadowMapFit).
 */
static struct
{
    bool valid;
    double texelsU, texelsV; /* the window's centre, texels of the world */
    double depth;            /* its middle along the sun, world */
    float halfU, halfV, bias, ambient, dx, dz;
    uint32_t casters;
} sShadowKept;
static unsigned sShadowRedrawn;
/* How far the middle along the sun may drift before the map is drawn again:
 * the depth it holds is VOXEL_SHADOW_MAP_DEPTH across. */
#define VOXEL_SHADOW_KEEP_DEPTH 8.0
/* And the window across it, when the picture leaves it that much room. */
#define VOXEL_SHADOW_KEEP_ACROSS 1.0f

/* What ShadowMapCasters would draw: the chunks on screen, where they stand
 * and what they hold, and the static models found for them. */
static uint32_t ShadowCasterStamp(void)
{
    uint32_t stamp = 2166136261u;

#define STAMP(v) (stamp = (stamp ^ (uint32_t)(v)) * 16777619u)
    for (unsigned i = 0; i < sDrawCount; ++i)
    {
        const VoxelChunk *chunk = sDraws[i].chunk;

        STAMP((uintptr_t)chunk);
        STAMP(sDraws[i].worldX);
        STAMP(sDraws[i].worldZ);
        STAMP((uintptr_t)chunk->vram);
        STAMP(chunk->count);
        STAMP(chunk->sum);
        STAMP(chunk->epoch);
        STAMP(chunk->buildingFirst);
        STAMP(chunk->treeShadow);
        STAMP(chunk->buildingPage);
        STAMP(chunk->atlasGeneration);
        if (chunk->staticCount == 0 || chunk->draft || !sHaveBuildings)
            continue;
        for (unsigned k = 0; k < chunk->staticCount; ++k)
        {
            const VoxelStaticDraw *s = &chunk->statics[k];
            const StaticMesh *mesh = StaticMeshDrawn(s->pageModel, s->lit);

            STAMP(s->pageModel);
            STAMP((uintptr_t)mesh);
            if (mesh != NULL)
            {
                STAMP((uintptr_t)mesh->vertices);
                STAMP(mesh->count);
            }
        }
    }
#undef STAMP
    return stamp;
}

static void ShadowMapRender(void)
{
    float dx = VOXEL_SUN_DX, dz = VOXEL_SUN_DZ;
    float n = sqrtf(dx * dx + 1.0f + dz * dz), h = sqrtf(dx * dx + dz * dz);
    /* along the light (down), and two axes across it */
    float D[3] = {dx / n, -1.0f / n, dz / n}, U[3], V[3];
    float texelU, texelV, slope, worldU, worldV;
    float cu, cv, cd;
    bool kept;
    C3D_Mtx projection, identity;

    if (h > 0.0001f)
    {
        U[0] = -dz / h; U[1] = 0.0f; U[2] = dx / h;
    }
    else
    {
        U[0] = 1.0f; U[1] = 0.0f; U[2] = 0.0f;
    }
    /* V = D x U */
    V[0] = D[1] * U[2] - D[2] * U[1];
    V[1] = D[2] * U[0] - D[0] * U[2];
    V[2] = D[0] * U[1] - D[1] * U[0];
    ShadowMapFit(U, V, &worldU, &worldV);
    texelU = 2.0f * sShadowHalfU / VOXEL_SHADOW_MAP_DIM;
    texelV = 2.0f * sShadowHalfV / VOXEL_SHADOW_MAP_DIM;
    /*
     * The lit casters' margin: the filter compares with the next texel too,
     * where a lit surface's own depth is a texel's width x its slope to the
     * sun further on - the ground's is h, a wall facing the sun 1 / h; walls
     * more nearly along the light are within the shadow's light
     * (ShadowTexEnv) and cannot show it.
     */
    slope = h > 1.0f ? h : 1.0f;
    if (h > 0.0001f && 1.0f / h > slope)
        slope = 1.0f / h;
    if (slope > 2.0f)
        slope = 2.0f;
    sShadowBias = slope * (texelU > texelV ? texelU : texelV) * 1.05f + 0.02f;
    /* the centre on whole texels of the world - not of the anchor, which is
     * whole tiles but not whole texels across the sun: snapped from it, the
     * texels moved under the shadows at each crossing and their edges
     * shimmered - so that the texels lie still on the ground while the
     * camera moves */
    {
        double ax = sDrawAnchorX, az = sDrawAnchorZ;
        double au = U[0] * ax + U[2] * az, av = V[0] * ax + V[2] * az;
        double texelsU = floor((double)worldU / texelU + 0.5), texelsV = floor((double)worldV / texelV + 0.5);
        double depth = (double)D[0] * sCamera.targetX + (double)D[1] * sCamera.targetY
                     + (double)D[2] * sCamera.targetZ;
        /* the room the picture leaves in the window, either side */
        float roomU = sShadowHalfU - 0.5f * sShadowFitWanted[0] - VOXEL_SHADOW_FIT_MARGIN;
        float roomV = sShadowHalfV - 0.5f * sShadowFitWanted[1] - VOXEL_SHADOW_FIT_MARGIN;
        float ambient = VoxelLighting_Ambient();
        uint32_t casters = ShadowCasterStamp();

        if (roomU > VOXEL_SHADOW_KEEP_ACROSS) roomU = VOXEL_SHADOW_KEEP_ACROSS;
        if (roomV > VOXEL_SHADOW_KEEP_ACROSS) roomV = VOXEL_SHADOW_KEEP_ACROSS;
        if (sSwoop > 0.0f)
            roomU = roomV = 0.0f;
        kept = sShadowKept.valid && sShadowKept.casters == casters
            && sShadowKept.halfU == sShadowHalfU && sShadowKept.halfV == sShadowHalfV
            && sShadowKept.bias == sShadowBias && sShadowKept.ambient == ambient
            && sShadowKept.dx == dx && sShadowKept.dz == dz
            && fabs(texelsU - sShadowKept.texelsU) * texelU <= (roomU > 0.0f ? roomU : 0.0f)
            && fabs(texelsV - sShadowKept.texelsV) * texelV <= (roomV > 0.0f ? roomV : 0.0f)
            && fabs(depth - sShadowKept.depth) <= VOXEL_SHADOW_KEEP_DEPTH;
        if (!kept)
        {
            sShadowKept.valid = true;
            sShadowKept.casters = casters;
            sShadowKept.texelsU = texelsU;
            sShadowKept.texelsV = texelsV;
            sShadowKept.depth = depth;
            sShadowKept.halfU = sShadowHalfU;
            sShadowKept.halfV = sShadowHalfV;
            sShadowKept.bias = sShadowBias;
            sShadowKept.ambient = ambient;
            sShadowKept.dx = dx;
            sShadowKept.dz = dz;
        }
        /* the window the map was drawn for, from this Draw's anchor */
        cu = (float)(sShadowKept.texelsU * texelU - au);
        cv = (float)(sShadowKept.texelsV * texelV - av);
        cd = (float)(sShadowKept.depth - ((double)D[0] * ax + (double)D[2] * az));
    }

    /* clip: x, y = across / half, z = -depth (0 nearest the sun), w = 1 */
    Mtx_Zeros(&projection);
    projection.r[0].x = U[0] / sShadowHalfU;
    projection.r[0].y = U[1] / sShadowHalfU;
    projection.r[0].z = U[2] / sShadowHalfU;
    projection.r[0].w = -cu / sShadowHalfU;
    projection.r[1].x = V[0] / sShadowHalfV;
    projection.r[1].y = V[1] / sShadowHalfV;
    projection.r[1].z = V[2] / sShadowHalfV;
    projection.r[1].w = -cv / sShadowHalfV;
    projection.r[2].x = -D[0] / VOXEL_SHADOW_MAP_DEPTH;
    projection.r[2].y = -D[1] / VOXEL_SHADOW_MAP_DEPTH;
    projection.r[2].z = -D[2] / VOXEL_SHADOW_MAP_DEPTH;
    projection.r[2].w = cd / VOXEL_SHADOW_MAP_DEPTH - 0.5f;
    projection.r[3].w = 1.0f;

    /* the map's coordinates of the same point: u, v in 0..1, w the depth */
    Mtx_Zeros(&sShadowTexMtx);
    sShadowTexMtx.r[0].x = projection.r[0].x * 0.5f;
    sShadowTexMtx.r[0].y = projection.r[0].y * 0.5f;
    sShadowTexMtx.r[0].z = projection.r[0].z * 0.5f;
    sShadowTexMtx.r[0].w = projection.r[0].w * 0.5f + 0.5f;
    sShadowTexMtx.r[1].x = projection.r[1].x * 0.5f;
    sShadowTexMtx.r[1].y = projection.r[1].y * 0.5f;
    sShadowTexMtx.r[1].z = projection.r[1].z * 0.5f;
    sShadowTexMtx.r[1].w = projection.r[1].w * 0.5f + 0.5f;
    sShadowTexMtx.r[2].x = -projection.r[2].x;
    sShadowTexMtx.r[2].y = -projection.r[2].y;
    sShadowTexMtx.r[2].z = -projection.r[2].z;
    sShadowTexMtx.r[2].w = -projection.r[2].w;
    sShadowTexMtx.r[3].w = 1.0f;

    /* the map still holds this window: read as it is, from this anchor */
    if (kept)
    {
        sShadowReady = true;
        return;
    }
    ++sShadowRedrawn;
    C3D_FrameSplit(0);
    C3D_RenderTargetClear(sShadowTarget, C3D_CLEAR_COLOR, 0xFFFFFFFFu, 0);
    C3D_FrameDrawOn(sShadowTarget);
#if SHADOW_DEBUG >= 3
    {
        /* DEBUG: clear by drawing white in GL mode */
        C3D_Mtx one;
        C3D_TexEnv *env = C3D_GetTexEnv(0);

        Mtx_Identity(&one);
        SetProjection(&one);
        UploadModelView(&one);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniOrigin, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, 0xFFFFFFFFu);
        for (int i = 1; i < 6; ++i)
            C3D_TexEnvInit(C3D_GetTexEnv(i));
        C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
        C3D_ImmSendAttrib(-512.0f, -512.0f, -256.0f, 8192.0f); C3D_ImmSendAttrib(0, 0, 0, 1);
        C3D_ImmSendAttrib(512.0f, -512.0f, -256.0f, 8192.0f); C3D_ImmSendAttrib(0, 0, 0, 1);
        C3D_ImmSendAttrib(-512.0f, 512.0f, -256.0f, 8192.0f); C3D_ImmSendAttrib(0, 0, 0, 1);
        C3D_ImmSendAttrib(512.0f, 512.0f, -256.0f, 8192.0f); C3D_ImmSendAttrib(0, 0, 0, 1);
        C3D_ImmDrawEnd();
        sDrawSet = 0;
    }
#endif
    C3D_FragOpMode(GPU_FRAGOPMODE_SHADOW);
    /* a soft write is kept as s / (constant + linear d / stored), and
     * citro3d's (scale, bias) are constant = scale + bias, linear = -scale:
     * (0, 1) keeps s as written. (1, 0) divided it by 1 - d / stored, near
     * 0, and every partial light came out full. */
    C3D_FragOpShadow(0.0f, 1.0f);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_CullFace(GPU_CULL_NONE);
    ShadowUniforms(NULL);
    /* lit casters put sShadowBias further from the sun (voxel.v.pica) */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowSel, 1.0f, 0.0f,
                  sShadowBias / VOXEL_SHADOW_MAP_DEPTH, VoxelLighting_Ambient());
    SetProjection(&projection);

    /* green 0 writes the depth, else full light: 1 - the texel's alpha */
    {
        C3D_TexEnv *env = C3D_GetTexEnv(0);

        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR,
                        GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        for (int i = 1; i < 6; ++i)
            C3D_TexEnvInit(C3D_GetTexEnv(i));
    }

    /* the anchored world as it is: the eye's model-view without its view */
    Mtx_Identity(&sDrawModelView);
    Mtx_Identity(&identity);
    Mtx_Translate(&identity, -(float)sDrawAnchorX, 0.0f, -(float)sDrawAnchorZ, true);
    if (SHADOW_DEBUG != 3)
        ShadowMapCasters(&identity);
#if VOXEL_SHADOW_SOFT_RINGS > 0
    /*
     * The soft edge, a blur outward from every shadow: the casters again,
     * moved across the sun, written as partial light (green) where they are
     * nearer the sun than what the map holds, and kept as the least of all -
     * after every depth, since a later depth keeps the light written under
     * it. Ring k of VOXEL_SHADOW_SOFT_RINGS is four copies moved k / rings of
     * the reach (diagonally, then along the axes, by turns, so that the
     * blur is round), leaving k / (rings + 1) of the light: the nearer the
     * shadow, the less light, in steps a filter's texel apart which it
     * smooths into one slope. The ground at a corner's foot is within the
     * first. Not on a caster's own lit surface: those copies go further
     * back by their slope across the move, and the unlit ones, which the map
     * does not change, a little.
     */
    {
        static const float diagonal[4][2] = {{0.7071f, 0.7071f}, {-0.7071f, 0.7071f},
                                             {0.7071f, -0.7071f}, {-0.7071f, -0.7071f}};
        static const float axes[4][2] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};
        C3D_TexEnv *env;
        C3D_Mtx copy = projection;

        env = C3D_GetTexEnv(1);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        copy.r[2].w = projection.r[2].w - VOXEL_SHADOW_SOFT_BACK / VOXEL_SHADOW_MAP_DEPTH;
        for (unsigned ring = 1; ring <= VOXEL_SHADOW_SOFT_RINGS && SHADOW_DEBUG != 3; ++ring)
        {
            float reach = VOXEL_SHADOW_SOFT_REACH * (float)ring / (float)VOXEL_SHADOW_SOFT_RINGS;
            float light = (float)ring / (float)(VOXEL_SHADOW_SOFT_RINGS + 1u);
            const float (*dirs)[2] = (ring & 1u) ? diagonal : axes;

            /* green = 1 - alpha (1 - light): a clear texel writes full light */
            env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
            C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
            C3D_TexEnvColor(env, 0xFF000000u | (uint32_t)((1.0f - light) * 255.0f + 0.5f) * 0x010101u);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowSel, 1.0f, 0.0f,
                          (sShadowBias + slope * reach) / VOXEL_SHADOW_MAP_DEPTH, VoxelLighting_Ambient());
            for (unsigned k = 0; k < 4u; ++k)
            {
                copy.r[0].w = projection.r[0].w + dirs[k][0] * reach / sShadowHalfU;
                copy.r[1].w = projection.r[1].w + dirs[k][1] * reach / sShadowHalfV;
                SetProjection(&copy);
                ShadowMapCasters(&identity);
            }
        }
    }
#endif
    C3D_FragOpMode(GPU_FRAGOPMODE_GL);
    /* rendered to a texture, then sampled: a split between */
    C3D_FrameSplit(0);
    sShadowReady = true;
}

void CtrVoxel_Draw(C3D_RenderTarget *target, int eye)
{
    /* Its own CPU time and its passes' (DrawPartsFrame). */
    uint32_t drawStart = Port_ProfTick(), partStart, weatherBefore = sDrawTicks[DRAW_WEATHER];
    C3D_Mtx projection, view;
    C3D_AttrInfo *attr;
    C3D_TexEnv *env;
    const VoxelMapInstance *current = VoxelWorld_Instance(0);
    bool indoor = current != NULL && current->indoor;
    VoxelLight light = LightFor(indoor), unlit = LightFor(true);
    /* The swoop: the light towards neutral with the camera (VOXEL_SWOOP_NEUTRAL_START). */
    float neutral = CtrVoxel_SwoopNeutral();
    bool dapples;
#if CTR_VOXEL_LIGHTING
    float fog = !indoor && sHaveFog ? VoxelWorld_FogDensity() : 0.0f;
    bool cave = fog > 0.0f && VoxelWorld_Underground();
    VoxelWeatherClass weather = indoor ? VOXEL_WEATHER_CLEAR : VoxelWorld_Weather();
    bool ownsWeather = sHaveWeatherProgram && sStreaks != NULL
                    && (weather == VOXEL_WEATHER_RAIN || weather == VOXEL_WEATHER_SAND
                        || weather == VOXEL_WEATHER_ASH);
    float weatherAmount = ownsWeather ? VoxelWorld_WeatherAmount() * (1.0f - neutral) : 0.0f;
#endif

    sBloomStrength = 0.0f;

    ++sDrawSerial;
    sDrawEye = eye;
    sGloomAmount = 0.0f;
    sOwnsFog = false;
    sOwnsWeather = false;
    if (!sReady || sDrawCount == 0)
        return;
    const CtrDayNightLight *cycle = CtrDayNight_Get();
    CtrDayNightBlend moon = CtrDayNight_Moonlight(cycle, 0.0f, cycle->rgb);

#if CTR_VOXEL_LIGHTING
    if (neutral > 0.0f)
    {
        /* Neutral is what the 2D compositor does to the field: the day-night
         * cycle's tint and its moonlight (Blend, 3ds_video.c), nothing else. */
        VoxelLight plain = unlit;

        if (!indoor)
            for (unsigned channel = 0; channel < 3; ++channel)
                plain.sun[channel] = plain.shade[channel] = cycle->rgb[channel] * (1.0f - moon.amount);
        light = LightMix(&light, &plain, neutral);
    }
#endif
    dapples = DapplesOn(&light);
    sMoonlight = indoor ? 0.0f : moon.amount;
    sMoonlightColour = 0xFF000000u;
    for (unsigned channel = 0; channel < 3; ++channel)
        sMoonlightColour |= (uint32_t)(moon.rgb[channel] * sMoonlight * 127.5f + 0.5f)
                         << (channel * 8u);
#if CTR_VOXEL_LIGHTING
    sOwnsFog = !indoor && sHaveFog;
    sOwnsWeather = ownsWeather;
#endif

    sBloomStrength = light.bloom;
    /* The shadow map: wanted in HIGH, outdoors, in sunlight; drawn by the
     * first eye of a frame, kept for the second. */
    sShadowOn = false;
    bool shadowMap = false;
#if CTR_VOXEL_LIGHTING
    shadowMap = VoxelLighting_Quality() == VOXEL_SHADOWS_HIGH && !indoor
             && gVoxelSun.strength > 0.0f && neutral <= 0.0f && ShadowMapMake();
#endif
    if (!shadowMap)
    {
        sShadowReady = false;
        sShadowKept.valid = false;
    }

    /* The camera the frustum was cut from in the update (UpdateFrustum).
     * The gloom is placed on the logical surface, before the fit to it. */
#if CTR_VOXEL_LIGHTING
    if (cave && sHaveGloom)
    {
        CameraMatrices(&projection, &view, false, eye);
        PlaceGloom(&projection, &view);
        sGloomAmount = VOXEL_GLOOM_MAX * fog * (1.0f - neutral);
    }
#endif
    CameraMatrices(&projection, &view, true, eye);
    sDrawAnchorX = (int)floorf(sCamera.targetX);
    sDrawAnchorZ = (int)floorf(sCamera.targetZ);
    /* Each value written once in this Draw before any is left as it stands. */
    sDrawSet = 0;

    /* The plain program needs nothing of the shadow map, and only a Draw with
     * no map has no caster pass (ShadowMapRender) or blur pass either. */
    BindWorldProgram(sMeasurePlain && !shadowMap);

    /* VoxelGpuVertex: texcoord (2 floats), then position + shade (4 shorts). */
    attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);

    if (shadowMap && (eye != 1 || !sShadowReady))
    {
        uint32_t shadowStart = Port_ProfTick();
        static uint64_t shadowTicks;
        static unsigned shadowFrames;

        ShadowMapRender();
        shadowTicks += Port_ProfTick() - shadowStart;
        if (++shadowFrames == 600)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL shadow map cpu=%.3f ms a frame, drawn %u of %u, %.0fx%.0f tiles (wanted %.0fx%.0f, slid %.1f %.1f), bias %.3f",
                         (float)shadowTicks * 1000.0f / SYSCLOCK_ARM11 / shadowFrames,
                         sShadowRedrawn, shadowFrames,
                         sShadowHalfU * 2.0f, sShadowHalfV * 2.0f, sShadowFitWanted[0], sShadowFitWanted[1],
                         sShadowFitSlid[0], sShadowFitSlid[1], sShadowBias);
            shadowTicks = 0;
            shadowFrames = 0;
            sShadowRedrawn = 0;
        }
        sDrawSet = 0;
    }
    C3D_FrameDrawOn(target);
    /* The picture is the surface's top-left 400x240 (FitToLogicalSurface) and
     * the viewport all 512x256 of it: without a scissor the world past the
     * picture's right and bottom edges was drawn into the margin, up to a
     * quarter of the fill, where nothing reads it. A few columns and rows are
     * kept past the edge for the taps that blur the picture (SwoopBlur). The
     * GPU's rows count from the bottom, so the picture's are the upper 240.
     * The callers turn the scissor off after the Draw, as they did. */
    if (sMeasureScissor)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, (u32)(VOXEL_SURFACE_H - CTR_GAME_HEIGHT) - VOXEL_SCISSOR_MARGIN,
                       (sMeasureCut & CTR_VOXEL_CUT_HALF) ? (u32)CTR_GAME_WIDTH / 2u
                       : (u32)CTR_GAME_WIDTH + VOXEL_SCISSOR_MARGIN, (u32)VOXEL_SURFACE_H);
    Mtx_Copy(&sDrawModelView, &view);
    Mtx_Translate(&sDrawModelView, (float)sDrawAnchorX, 0.0f, (float)sDrawAnchorZ, true);
    sShadowOn = shadowMap && sShadowReady;
    sShadowColour = ShadowColour(&light);
    if (sShadowOn)
    {
        dapples = false;
        ShadowUniforms(&sDrawModelView);
#if SHADOW_DEBUG == 5
        {
            static C3D_Tex lin;
            if (lin.data == NULL)
            {
                C3D_TexInitWithParams(&lin, NULL, (C3D_TexInitParams){ 8, 8, 0, GPU_RGBA8, GPU_TEX_SHADOW_2D, false });
                memset(lin.data, 0xFF, 8 * 8 * 4);
                GSPGPU_FlushDataCache(lin.data, 8 * 8 * 4);
            }
            C3D_TexBind(0, &lin);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowU, 0.0f, 0.0f, 0.0f, 0.5f);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowV, 0.0f, 0.0f, 0.0f, 0.5f);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniShadowW, 0.0f, 0.0f, 0.0f, 0.5f);
        }
#else
        C3D_TexBind(0, &sShadowTex);
#endif
        C3D_TexShadowParams(false, VOXEL_SHADOW_MAP_SAMPLE_BIAS / VOXEL_SHADOW_MAP_DEPTH);
    }
    else
        ShadowUniforms(NULL);

    /* texture0 x graded colour (dappled), then the distance haze, then the fade. */
    PrepareFades();
    memcpy(sHazeRgb, light.hazeRgb, sizeof(sHazeRgb));
    TerrainTexEnv(dapples ? &light : NULL);
    FadeFor(false);
    C3D_TexEnvInit(C3D_GetTexEnv(5));
#if CTR_VOXEL_LIGHTING
    if (dapples)
    {
        DappleFrame();
        C3D_TexBind(1, &sDappleTex);
    }
#endif

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_ColorLogicOp(GPU_LOGICOP_COPY);

    SetProjection(&projection);
    SetGrade(&light);
    DrawFills(dapples ? &light : NULL);

    /*
     * Ordinary terrain first, then the tree material, then the modelled
     * buildings, all alpha tested: the transparent corners of the crowns and
     * the clear background of a cut tile's rock must write neither colour nor
     * depth. Each chunk keeps all three ranges in the same
     * static VRAM buffer, measured from its own corner, so every draw carries
     * that corner's place in the world in `origin` (the model-view is the
     * Draw's, SetModelView) and is drawn from the chunks' shared buffer table
     * (BindChunkVertices).
     */
    DrawParts();
    {
        static int every = -1;

        if (every < 0) every = CtrSettings_GetInt("voxel_view_audit", 0);
        if (every > 0 && sFrame % (unsigned)every == (unsigned)every / 2u && eye <= 0) ViewAudit();
    }
    partStart = Port_ProfTick();
    for (unsigned pass = 0; pass < VOXEL_ATLAS_PAGES + 2u; ++pass)
    {
        const C3D_Tex *boundTex = NULL;
        bool glowing = pass == VOXEL_ATLAS_PAGES + 1u && !indoor && sGlowWanted;

        C3D_AlphaTest(true, GPU_GREATER, 0);
        if (sShadowOn)
            ShadowReceive(true);
        if (sMeasureCut & (pass < VOXEL_ATLAS_PAGES ? CTR_VOXEL_CUT_GROUND
                           : pass == VOXEL_ATLAS_PAGES ? CTR_VOXEL_CUT_TREES : CTR_VOXEL_CUT_BUILDINGS))
            continue;

        for (unsigned i = 0; i < sDrawCount; ++i)
        {
            const VoxelChunk *chunk = sDraws[i].chunk;
            unsigned first = pass < VOXEL_ATLAS_PAGES ? chunk->terrainFirst[pass]
                           : pass == VOXEL_ATLAS_PAGES ? chunk->terrainCount : chunk->buildingFirst;
            unsigned count = pass < VOXEL_ATLAS_PAGES ? chunk->terrainFirst[pass + 1u] - first
                           : pass == VOXEL_ATLAS_PAGES ? chunk->buildingFirst - chunk->treeShadow - first
                           : chunk->count - first;
            C3D_Tex *tex;

            if (count == 0)
                continue;
            tex = pass < VOXEL_ATLAS_PAGES ? AtlasTex(chunk->atlas, pass)
                : pass == VOXEL_ATLAS_PAGES ? &sTreeAtlas : BuildingPage(chunk->buildingPage);
            if (tex == NULL)
                continue;

            SetModelView(sDraws[i].worldX, sDraws[i].worldZ);
#if CTR_VOXEL_LIGHTING
            if (dapples)
                DappleUniforms(sDraws[i].worldX, sDraws[i].worldZ, 0.0f);
#endif
            if (boundTex != tex)
            {
                boundTex = tex;
                BindAtlas(tex);
                if (glowing)
                {
                    BuildingPageSlot *page = FindPage(chunk->buildingPage);

                    BuildingGlowEnv(GlowTex(page),
                                    CtrDayNight_Get()->glow, dapples ? &light : NULL);
                }
            }
            if (pass == VOXEL_ATLAS_PAGES && sShadowOn && chunk->treeCrowns != 0)
            {
                /* the crowns keep their own light under the map: their
                 * shadows are on the ground, and a crown behind its own (or
                 * a neighbour's) turned card would read as shaded */
                unsigned grounds = count - chunk->treeCrowns;

                if (grounds != 0)
                    C3D_DrawArrays(GPU_TRIANGLES, BindChunkVertices(chunk, first, grounds), (int)grounds);
                ShadowReceive(false);
                C3D_DrawArrays(GPU_TRIANGLES, BindChunkVertices(chunk, first + grounds, chunk->treeCrowns),
                               (int)chunk->treeCrowns);
                ShadowReceive(true);
                continue;
            }
            if (pass < VOXEL_ATLAS_PAGES && sDraws[i].parts != VOXEL_PARTS_ALL)
            {
                /* the parts in the picture, each run of them one draw */
                for (unsigned part = 0; part < VOXEL_PARTS;)
                {
                    unsigned next = part + 1u, from, run;

                    if (!(sDraws[i].parts >> part & 1u))
                    {
                        part = next;
                        continue;
                    }
                    while (next < VOXEL_PARTS && (sDraws[i].parts >> next & 1u))
                        ++next;
                    from = ChunkPartFirst(chunk, pass, part);
                    run = ChunkPartFirst(chunk, pass, next) - from;
                    if (run != 0)
                        C3D_DrawArrays(GPU_TRIANGLES, BindChunkVertices(chunk, from, run), (int)run);
                    part = next;
                }
                continue;
            }
            C3D_DrawArrays(GPU_TRIANGLES, BindChunkVertices(chunk, first, count), (int)count);
        }
        if (pass == VOXEL_ATLAS_PAGES + 1u && sHaveBuildings)
            DrawStatics(&view, dapples ? &light : NULL, glowing);
    }
    ReleaseGlowUnit();
    /* The glow over the dapples moved the haze and the fade a stage on
     * (BuildingGlowEnv): back where every later pass has them. */
    if (dapples || sShadowOn)
    {
        FadeFor(false);
        C3D_TexEnvInit(C3D_GetTexEnv(5));
    }
    if (sShadowOn)
    {
        /* The map off for the rest; the atlas back on unit 0, where the
         * passes after read whatever is bound, and unit 1 free (as the
         * dapples' is freed, ReleaseDappleUnit). */
        uint32_t *flags = (uint32_t *)(void *)(__C3D_Context + C3DI_CONTEXT_FLAGS);
        C3D_Tex **bound = (C3D_Tex **)(void *)(__C3D_Context + C3DI_CONTEXT_TEX);

        sShadowOn = false;
        ShadowUniforms(NULL);
        ShadowBlurPass();
        SetProjection(&projection);
        FadeFor(false);
        C3D_TexEnvInit(C3D_GetTexEnv(5));
        if (sShadowAtlasBound != NULL)
        {
            C3D_TexBind(0, sShadowAtlasBound);
            if (bound[1] == sShadowAtlasBound)
            {
                bound[1] = NULL;
                *flags |= C3DI_FLAG_TEX(1);
            }
        }
        sShadowAtlasBound = NULL;
    }
    {
        uint32_t now = Port_ProfTick();

        sDrawTicks[DRAW_CHUNKS] += now - partStart;
        partStart = now;
    }

    /* The reflections, cast shadows and the player's silhouette take no
     * dapples; the unit stays bound for the sprites and the rays, and is
     * released after them (ReleaseDappleUnit: never C3D_TexBind(1, NULL),
     * which reads address 0xC on the console). */
    TerrainTexEnv(NULL);

    /* Billboards and shadows: the linear buffer, around the camera's tile. */
    SetModelView(sDynamicX, sDynamicZ);
    BindVertices(sDynamic);

    if (!indoor && !sBattle.on && CtrGame_IsOutdoor() && CtrDayNight_Get()->glow > 0.0f
        && sLampGpu != NULL && (sLampVertices != 0 || sLampGroundVertices != 0))
    {
        float strength = CtrDayNight_Get()->glow;
        SetGrade(&unlit);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
        C3D_DepthMap(true, -1.0f, 3.0f / 65535.0f);
        /* From the origin they were built at, kept while the camera moves. */
        SetModelView(sLampX, sLampZ);
        BindVertices(sLampGpu);
        sLampDrawn = svcGetSystemTick();
        if (sLampGroundVertices != 0 && sLampPoolTex.data != NULL)
        {
            /* the sprites, shadows and reflections bind their atlas again */
            C3D_TexBind(0, &sLampPoolTex);
            LampTexEnv(strength, true);
            C3D_DrawArrays(GPU_TRIANGLES, VOXEL_LAMP_GROUND_FIRST, sLampGroundVertices);
        }
        if (sLampVertices != 0)
        {
            LampTexEnv(strength, false);
            C3D_DrawArrays(GPU_TRIANGLES, 0, sLampVertices);
        }
        SetModelView(sDynamicX, sDynamicZ);
        BindVertices(sDynamic);
        C3D_DepthMap(true, -1.0f, 0.0f);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
        TerrainTexEnv(NULL);
        SetGrade(&light);
    }

    if (sReflectionVertices != 0)
    {
        VoxelLight reflected = unlit;

        if (!indoor)
            for (unsigned channel = 0; channel < 3; ++channel)
                reflected.sun[channel] = reflected.shade[channel] = cycle->rgb[channel] * (1.0f - sMoonlight);
        /* Mirrored sprite art lies just above reflective ground. One draw for
         * every visible reflection, blended without writing depth. */
        SetGrade(&reflected);
        C3D_TexBind(0, &sSpriteAtlas);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
        env = C3D_GetTexEnv(0);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvColor(env, 0x60000000u | (MoonlightColour(sWorldFade.amount) & 0xFFFFFFu));
        FadeFor(true);
        C3D_DrawArrays(GPU_TRIANGLES, VOXEL_REFLECTION_FIRST, sReflectionVertices);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
        TerrainTexEnv(NULL);
        FadeFor(false);
        SetGrade(&light);
    }

#if CTR_VOXEL_LIGHTING
    /*
     * Cast shadows: each object's own sprite laid on the ground (see
     * EmitCastShadow), so the silhouette is the texture's alpha. Colour is a
     * constant dark blue; alpha is texture alpha x the strength the vertex
     * carries in shade, which reaches the primary colour unchanged under the
     * identity grade (half of it, doubled back by the alpha scale). Depth
     * tested against the finished terrain and crowns, never written: a shadow
     * must not occlude a sprite. One draw for all of them.
     */
    if (sShadowVertices != 0 && sSwoopShadows)
    {
        SetGrade(&unlit);
        C3D_TexBind(0, &sSpriteAtlas);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
        env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
        C3D_TexEnvColor(env, 0xFF281408u);
        C3D_DrawArrays(GPU_TRIANGLES, VOXEL_SHADOW_FIRST, sShadowVertices);
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
        TerrainTexEnv(NULL);
        SetGrade(&light);
    }
#endif

    /*
     * Billboards last. The atlas is RGBA5551, so transparency is one bit and
     * an alpha test is enough - no blending, and depth is written only where
     * the sprite is actually opaque, which is what makes a wall occlude an NPC
     * without the NPC's empty corners punching a hole in it.
     */
    if (sSpriteVertices != 0 && sSwoopCards)
    {
        int playerFirst = VoxelEntities_PlayerVertexFirst();
        /* Through the swoop the cards tilt back to face its camera
         * (voxel_entities.c) and lean into the walls behind them: over
         * everything, as the 2D field draws its sprites, and no ghost. */
        bool lying = sSwoop > 0.0f;

        C3D_AlphaTest(true, GPU_GREATER, 0);
        C3D_TexBind(0, &sSpriteAtlas);
        if (playerFirst >= 0 && !lying)
        {
            /* Reversed depth: LESS selects only the player's pixels hidden by
             * terrain. Texture alpha preserves the sprite outline. One quad,
             * colour-only writes; neither roofs nor their depth are modified. */
            C3D_DepthTest(true, GPU_LESS, GPU_WRITE_COLOR);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
            env = C3D_GetTexEnv(0);
            C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
            C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_1);
            C3D_TexEnvColor(env, 0xA0D8E8F0u);
            C3D_DrawArrays(GPU_TRIANGLES, playerFirst, 6);
            C3D_ColorLogicOp(GPU_LOGICOP_COPY);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
            /* DrawArrays consumed the dirty flag. Reacquire the environment
             * so Citro3D uploads the restored sprite colours on the next draw. */
            TerrainTexEnv(NULL);
        }
#if CTR_VOXEL_LIGHTING
        /* Sprites stand in the same light as the ground at their feet. */
        if (dapples)
        {
            DappleUniforms(sDynamicX, sDynamicZ, 0.0f);
            TerrainTexEnv(&light);
        }
#endif
        if (sMoonlight > 0.0f)
            C3D_TexEnvColor(C3D_GetTexEnv(dapples ? 2 : 0), MoonlightColour(sWorldFade.amount));
        FadeFor(true);
        if (lying) C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_DrawArrays(GPU_TRIANGLES, 0, sSpriteVertices);
        if (lying) C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    }
    {
        uint32_t now = Port_ProfTick();

        sDrawTicks[DRAW_CARDS] += now - partStart;
        partStart = now;
    }
#if CTR_VOXEL_LIGHTING
    /* The fog's banks lie in the scene, so they go before the light in the
     * air (motes, rays) that is added over everything. */
    if (fog > 0.0f)
    {
        FadeFor(false);
        DrawFogBanks(&view, &light, cave, fog * (1.0f - neutral));
    }
    /* The sand's dust is the fog's sheets, sweeping along the ground. */
    if (weather == VOXEL_WEATHER_SAND && sHaveFog && weatherAmount > 0.0f)
    {
        FadeFor(false);
        DrawBanks(&view, &light, sSandBanks, 0xFF88BEE0u, weatherAmount);
    }
    /* The Fly cinematic's cloud wisps, high over the world. */
    if (sFly.active && sHaveFog && !indoor && sFly.wisps > 0.005f)
    {
        FadeFor(false);
        DrawCloudWisps(&view, &light, Clamp(sFly.wisps, 0.0f, 1.0f));
    }
    /* Before the rays, which leave the orthographic projection behind. */
    if (sMotes != NULL && light.motes > 0.005f)
        DrawMotes(&view, &light, &unlit);
#endif
#if CTR_VOXEL_LIGHTING
    {
        bool rays = sRayCount != 0 && sHaveDapple && light.rays > 0.005f;

        if (rays)
        {
            if (!dapples)
                C3D_TexBind(1, &sDappleTex);
            DrawRays(&light, &unlit);
        }
        if (dapples || rays)
            ReleaseDappleUnit();
    }
    /* Last: the rays put their own projection in. */
    if (weatherAmount > 0.0f)
    {
        /* Its CPU time in the log, every 600 draws (the measure of it). */
        static uint64_t weatherTicks;
        static unsigned weatherDraws;
        uint64_t start = svcGetSystemTick();

        DrawWeather(&projection, &view, &light, weather, weatherAmount);
        /* Its program's constants went into the registers (sDrawSet). */
        sDrawSet = 0;
        {
            uint64_t spent = svcGetSystemTick() - start;

            weatherTicks += spent;
            sDrawTicks[DRAW_WEATHER] += (uint32_t)spent;
        }
        if (++weatherDraws == 600)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL weather class=%u amount=%.2f lightning=%.2f cpu=%.3f ms a draw",
                         (unsigned)weather, weatherAmount, VoxelWorld_Lightning(),
                         weatherTicks * 1000.0f / SYSCLOCK_ARM11 / weatherDraws);
            weatherTicks = 0;
            weatherDraws = 0;
        }
    }
#endif
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    /* The 2D compositor sets up stage 0 only. */
    for (int i = 1; i < 6; ++i)
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    /* What the draws asked for that was already set (SetUniform,
     * BindVertices, StaticMeshDrawn, DrawStatics): the commands each Draw no
     * longer sends, a Draw on average, every 600 Draws. */
    {
        static unsigned setupDraws;

        if (++setupDraws == 600)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "VOXEL draw setup kept, a Draw: model-views %.1f of %.1f, "
                         "origins %.1f of %.1f, dapples %.1f of %.1f, buffer tables %.1f of %.1f, "
                         "model lookups %.1f of %.1f, model pages %.1f of %.1f",
                         sSetupKept.modelViews / 600.0f, sSetupAsked.modelViews / 600.0f,
                         sSetupKept.origins / 600.0f, sSetupAsked.origins / 600.0f,
                         sSetupKept.dapples / 600.0f, sSetupAsked.dapples / 600.0f,
                         sSetupKept.buffers / 600.0f, sSetupAsked.buffers / 600.0f,
                         sSetupKept.finds / 600.0f, sSetupAsked.finds / 600.0f,
                         sSetupKept.pages / 600.0f, sSetupAsked.pages / 600.0f);
            memset(&sSetupKept, 0, sizeof(sSetupKept));
            memset(&sSetupAsked, 0, sizeof(sSetupAsked));
            setupDraws = 0;
        }
    }
    /* The air: what followed the cards, but the weather's particles. With
     * the profiler off every tick read is 0, and so is all of this. */
    {
        uint32_t now = Port_ProfTick();

        if (now != 0)
        {
            uint32_t weatherNow = sDrawTicks[DRAW_WEATHER] - weatherBefore;
            uint32_t after = now - partStart;

            sDrawTicks[DRAW_AIR] += after > weatherNow ? after - weatherNow : 0;
            sDrawTicks[DRAW_ALL] += now - drawStart;
        }
        DrawPartsNote();
    }
}

/*
 * Stereoscopy, for the 3D slider. The world is drawn once per eye
 * (CtrVoxel_Draw with eye 0 and 1), each from a camera moved half the
 * interocular distance sideways with the asymmetric frustum of a stereo pair
 * (Mtx_PerspStereo): the two share the chunk cache, the atlases and the
 * billboards, so the second eye costs its draw calls and its fill, nothing
 * else. Every effect in the scene - roofs over the player, cast shadows,
 * reflections, the fog's banks - comes out at its own depth by itself.
 *
 * The depth is set in screen pixels: at the slider's full travel the far
 * distance separates by VOXEL_STEREO_FAR_PX in each eye, and the plane of
 * zero parallax (the glass) stands VOXEL_STEREO_FOCUS of the way from the
 * camera to the player, so the player is just behind the glass and the
 * ground along the bottom edge comes a few pixels out of it. The game's
 * weather is composed on the glass; its text windows and prompts at the
 * depth of the nearest world in front of it (CtrVoxel_OverlayParallax,
 * 3ds_video.c). On an Old 3DS the second pass took the field under 60 fps,
 * so 3ds_video.c keeps the world flat there.
 */
#define VOXEL_STEREO_FAR_PX 7.0f
#define VOXEL_STEREO_FOCUS 0.85f

void CtrVoxel_SetStereo(float slider)
{
    /* Above 1 with OPTIONS' 3D DEPTH over 100%: twice the depth at most. */
    sStereoSlider = slider > 0.0f ? (slider > 2.0f ? 2.0f : slider) : 0.0f;
}

/* The horizontal tangent of half the field of view. */
static float CameraTanX(void)
{
    return tanf(C3D_AngleFromDegrees(sCamera.fov) * 0.5f) * (float)CTR_GAME_WIDTH / (float)CTR_GAME_HEIGHT;
}

/* The pair's interocular distance in world units for this frame's slider,
 * and the distance of the plane of zero parallax, both from the camera. */
static float StereoIod(float *screen)
{
    /* The camera's `distance` is along the ground; the eye is further. */
    float dx = sCamera.x - sCamera.targetX, dy = sCamera.y - sCamera.targetY, dz = sCamera.z - sCamera.targetZ;
    float focus = sqrtf(dx * dx + dy * dy + dz * dz) * VOXEL_STEREO_FOCUS;

    if (focus < VOXEL_NEAR * 2.0f)
        focus = VOXEL_NEAR * 2.0f;
    *screen = focus;
    /* A point at infinity lands iod / (2 x screen x tanX) of the half width
     * off centre: that is VOXEL_STEREO_FAR_PX pixels at the full slider. */
    return sStereoSlider * 4.0f * VOXEL_STEREO_FAR_PX * focus * CameraTanX() / (float)CTR_GAME_WIDTH;
}

/* The eye's parallax of a point `depth` in front of the camera, in pixels
 * of the logical surface: the left eye sees the far distance to the left. */
static float StereoParallax(int eye, float depth)
{
    float screen, iod;

    if (eye < 0 || sStereoSlider <= 0.0f || depth <= 0.0f)
        return 0.0f;
    iod = StereoIod(&screen);
    return (eye == 0 ? 0.5f : -0.5f) * iod * (1.0f / depth - 1.0f / screen) / CameraTanX()
           * 0.5f * (float)CTR_GAME_WIDTH;
}

/*
 * The text windows and prompts laid over the world (3ds_video.c,
 * ComposeVoxelOverlay). On the glass they were behind the ground along the
 * bottom edge, which comes out of it: the eyes saw the text further away than
 * the world it covers. They stand instead at the depth of the nearest world
 * the picture can hold, rounded out to whole pixels so the GBA's pixel grid
 * stays as it is - never behind the glass, where they were already in front.
 *
 * The camera looks down, so down every column of the screen the ray of its
 * bottom edge is the first to come down to any height below the eye: what
 * stands on the player's ground no higher than VOXEL_STEREO_NEAR_LIFT is seen
 * no nearer than where that ray reaches that height. Two tiles is what the
 * GBA stands on the ground there: its people and its trees, 32 pixels high.
 */
#define VOXEL_STEREO_NEAR_LIFT 2.0f

/* The depth along the view of that point: `drop` is how far the height is
 * below the eye, `forwardY` and `upY` the heights of the view's unit forward
 * and up vectors, `tanY` the tangent of half the vertical field of view. 0
 * when the bottom edge never comes down (no bound: the overlay stays on the
 * glass); the near plane when the eye is not above the height at all. */
static float StereoNearestDepth(float drop, float forwardY, float upY, float tanY)
{
    float fall = tanY * upY - forwardY; /* height lost per unit of depth */
    float depth;

    if (drop <= 0.0f)
        return VOXEL_NEAR;
    if (fall <= 0.0f)
        return 0.0f;
    depth = drop / fall;
    return depth < VOXEL_NEAR ? VOXEL_NEAR : depth;
}

/* The overlay's shift for the eye the last Draw was for (ctr_voxel.h). */
float CtrVoxel_OverlayParallax(void)
{
    float dx = sCamera.targetX - sCamera.x, dy = sCamera.targetY - sCamera.y, dz = sCamera.targetZ - sCamera.z;
    float length = sqrtf(dx * dx + dy * dy + dz * dz), depth, nearest;

    if (!sReady || sDrawEye < 0 || sStereoSlider <= 0.0f || length <= 0.0f)
        return 0.0f;
    /* The view's up vector is the world's up made square to the forward
     * one (Mtx_LookAt): its height is the forward vector's length across. */
    depth = StereoNearestDepth(sCamera.y - (sCamera.ground + VOXEL_STEREO_NEAR_LIFT), dy / length,
                               sqrtf(dx * dx + dz * dz) / length,
                               tanf(C3D_AngleFromDegrees(sCamera.fov) * 0.5f));
    if (depth <= 0.0f)
        return 0.0f;
    /* The left eye sees what is near to the right (StereoParallax). */
    nearest = StereoParallax(sDrawEye, depth) * (sDrawEye == 0 ? 1.0f : -1.0f);
    if (nearest <= 0.0f)
        return 0.0f;
    nearest = ceilf(nearest);
    return sDrawEye == 0 ? nearest : -nearest;
}
