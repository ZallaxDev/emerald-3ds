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

/* ── GPU vertex format ──────────────────────────────────────────────────────
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
#define VOXEL_REFLECTION_RESERVE (VOXEL_SPRITE_SLOTS * VOXEL_REFLECTION_VERTICES)
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
static int sUniProjection = -1, sUniModelView = -1;
static int sUniShadeTint = -1, sUniTintDiff = -1, sUniGrade = -1, sUniFog = -1;
static int sUniDappleU = -1, sUniDappleV = -1;
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
#if CTR_VOXEL_LIGHTING
static float sGloomX, sGloomY;
#endif
#if CTR_VOXEL_LIGHTING
static void MakeDapple(void);
static void MakeRays(void);
static void MakeMotes(void);
static void MakeFog(void);
static void MakeGloom(void);
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
/* The tree crowns turned to the free-roam rig's eye, drawn again every frame
 * (BuildCrowns): cards of the chunks on screen, nearest first come, the rest
 * staying as built. 72 KiB of linear memory; without it the crowns stay as
 * built. */
#define VOXEL_CROWN_CARDS 768u
#define VOXEL_CROWN_VERTICES (VOXEL_CROWN_CARDS * 6u)
static VoxelGpuVertex *sCrownDyn;
static unsigned sCrownVertices;
static VoxelBuilder sBuilder;
static VoxelCamera sCamera;
static VoxelAtlasSlot sAtlases[VOXEL_ATLAS_SLOTS];
static uint32_t sAtlasStamp;
static unsigned sAtlasExtensionsSkipped;
static bool sAtlasCapped;
/* One atlas upload per frame: the queued copy still owns this source after
 * AcquireAtlas returns. Cache hits need no upload and remain unrestricted. */
static uint16_t *sAtlasStaging;
static bool sAtlasUploadPending;
/* The animated metatiles are redrawn a few at a time into this little linear
 * buffer and copied into their page by the GPU (VoxelAtlas_AnimatedSlots). */
#define VOXEL_ANIM_SLOTS 6u
static VoxelAnimSlot *sAnimOut;
/* The tiles written since each atlas's last pass, by page: the current map's
 * atlas and those of the maps beside it that share its primary tileset
 * (AnimateTiles) each take them in their own time. */
static uint8_t sAnimDirty[VOXEL_ATLAS_SLOTS][VOXEL_ATLAS_PAGES][128];
/* The pass in progress: the page (+1, 0 for none) of this atlas, the tiles it
 * redraws for - taken from the dirty ones when it began, so what is written
 * meanwhile waits for the next pass - and how far down the ids it is. */
static unsigned sAnimWorkPage, sAnimCursor;
static VoxelAtlasSlot *sAnimWorkAtlas;
static uint8_t sAnimWork[128];
/* Where the next pass starts looking for an atlas, and a page of it, with
 * dirty tiles. */
static unsigned sAnimNext, sAnimNextAtlas;
static bool sAnimPending;
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
    if (sAnimWorkAtlas == slot)
    {
        sAnimWorkAtlas = NULL;
        sAnimWorkPage = 0;
    }
}

void CtrVoxel_NotifyTilesetAnimWrite(unsigned firstTile, unsigned tileCount)
{
    if (firstTile >= 1024) return;
    if (tileCount > 1024 - firstTile) tileCount = 1024 - firstTile;
    for (unsigned t = firstTile; t < firstTile + tileCount; ++t)
    {
        for (unsigned a = 0; a < VOXEL_ATLAS_SLOTS; ++a)
            for (unsigned page = 0; page < VOXEL_ATLAS_PAGES; ++page)
                sAnimDirty[a][page][t >> 3] |= 1u << (t & 7);
        sBuildingAnimDirty[t >> 3] |= 1u << (t & 7);
    }
    sAnimPending = true;
    sBuildingAnimPending = true;
}
static bool AtlasJobBusy(void);
static void AtlasJobCancel(void);
static CtrVoxelStats sStats;
static const char *sStatus = "init";
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

/* ── Packing ────────────────────────────────────────────────────────────── */

static void JobCancel(void);
static bool StaticMeshPrepare(unsigned pageModel, bool lit, unsigned triangles);
static const struct StaticMesh *StaticMeshFind(unsigned pageModel, bool lit);

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

/* ── Static chunks ───────────────────────────────────────────────────────
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
/*
 * The meshes' own block of VRAM, claimed whole at start-up and cut by
 * voxel_arena.c: taken piece by piece from the console's allocator, a few
 * thousand variable-sized meshes left VRAM in crumbs that no building page or
 * atlas could be placed in. 96K vertices: Rustboro's view alone draws 54K, and
 * the prefetch ring around it has to fit as well.
 */
#define VOXEL_CHUNK_VRAM_BUDGET (1536u * 1024u)

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
    unsigned buildingFirst; /* trees are [terrainCount, buildingFirst) */
    /* ... the ground under them and the small trees' bases in [terrainCount,
     * crownFirst), the crown cards as built in [crownFirst, buildingFirst):
     * which the free-roam rig leaves out for the same cards turned to its eye,
     * from `crowns` (DrawCrowns). NULL when it has none, or none to turn. */
    unsigned crownFirst;
    VoxelCrown *crowns;
    unsigned crownCount;
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
    bool turnedCrowns;  /* its crowns come from the frame's own (BuildCrowns) */
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

/* The near plane of this frame's camera: the orbit's, or in the free-roam
 * rig (voxel_freecam.h) nearly at the lens, where the eye stands close to
 * what it sees. */
static float NearPlane(void)
{
    return VoxelCamera_Near(&sCamera, VOXEL_NEAR);
}

static void CameraMatrices(C3D_Mtx *projection, C3D_Mtx *view, bool fit, int eye)
{
    float screen, iod = eye >= 0 && sStereoSlider > 0.0f ? StereoIod(&screen) : 0.0f;

    Mtx_Persp(projection, C3D_AngleFromDegrees(sCamera.fov),
              (float)CTR_GAME_WIDTH / (float)CTR_GAME_HEIGHT,
              NearPlane(), VOXEL_FAR, false);
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
    if (fit)
        FitToLogicalSurface(projection);
    Mtx_LookAt(view,
               FVec3_New(sCamera.x, sCamera.y, sCamera.z),
               FVec3_New(sCamera.lookX, sCamera.lookY, sCamera.lookZ),
               FVec3_New(0.0f, 1.0f, 0.0f), false);
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

    if (vz > -NearPlane())
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
    /* How far the view's rectangle goes: the orbit's, or in the rig the
     * distance the haze takes the world at. */
    float reach = VoxelCamera_Reach(&sCamera, (float)VOXEL_VIEW_REACH_MAX);
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
                  NearPlane(), VOXEL_FAR, false);
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
                if (t < 0.0f || t > 4.0f * reach)
                    t = 4.0f * reach;
                GrowRect(rect, sCamera.x + dx * t, sCamera.z + dz * t);
            }
        }
    {
        float lo[2] = {sCamera.targetX - reach, sCamera.targetZ - reach};
        float hi[2] = {sCamera.targetX + reach, sCamera.targetZ + reach};

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

/* ── Building pages ─────────────────────────────────────────────────────────
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
 * Frontier's two halves, in view together, are 512 KiB each. */
#define VOXEL_PAGE_VRAM_BUDGET (1024u * 1024u)
/* Atlas pages that may borrow a gap of the pages' arena (AtlasPageBorrow). */
#define VOXEL_ATLAS_BORROW_MAX 2u
/* The arena's pieces: a page's texture and its window mask, and the
 * borrowed atlas pages. */
#define VOXEL_PAGE_PIECES (VOXEL_BUILDING_PAGES * 2u + VOXEL_ATLAS_BORROW_MAX)
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
 * One flag a page, for as many pages as buildings.bin can name. */
#define VOXEL_MAX_PAGES 256
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
/* Windows glow (DAY / NIGHT, outdoors, afternoon to dawn): the pages in
 * view want their masks. */
static bool sGlowWanted;
/* Console measurement switches, the scenario settings measure_glow and
 * measure_lamps (on unless set to 0): the night's lit windows and lamps
 * left out, to tell what each costs on the hardware. Read once a second. */
static bool sMeasureGlow = true, sMeasureLamps = true;
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

static bool AtlasPageBorrow(C3D_Tex *tex)
{
    const uint32_t bytes = VOXEL_ATLAS_PIXELS * sizeof(uint16_t);
    void *block;

    if (sPageBlock == NULL || CountBorrowedAtlasPages() >= VOXEL_ATLAS_BORROW_MAX)
        return false;
    block = VoxelArena_Alloc(&sPageArena, bytes, false);
    if (block == NULL)
        return false;
    PageTexInit(tex, block, VOXEL_ATLAS_W, VOXEL_ATLAS_H);
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: atlas page borrowed from the building pages' arena "
                 "(VRAM free=%lu, arena gap left %lu KiB)", (unsigned long)vramSpaceFree(),
                 (unsigned long)(VoxelArena_LargestGap(&sPageArena) >> 10));
    return true;
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

/*
 * A piece for a page's mask, if it has one and windows glow: only from
 * the room there is - a mask never pushes anything out - and when there is
 * none, asked for again a while later. Its windows stay unlit meanwhile.
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
    int32_t failed;     /* the last payload that would not read */
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
        if (sAssetAhead.result == NULL)
            sAssetAhead.failed = sAssetAhead.request.index;
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
        if (CtrAssets_PrefetchFind(payloads[cursor++], &sAssetAhead.request)
         && sAssetAhead.request.index != sAssetAhead.failed)
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
    sAssetAhead.failed = -1;
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
    if (sAssetAhead.state == AHEAD_DONE)
        CtrAssets_PrefetchAdopt(&sAssetAhead.request, sAssetAhead.result);
    memset(&sAssetAhead, 0, sizeof(sAssetAhead));
    sAssetAhead.failed = -1;
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
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
    {
        BuildingPageSlot *slot = &sPageSlots[i];

        if (slot->page < 0)
        {
            victim = slot;
            break;
        }
        if (!PageDroppable(slot, keep))
            continue;
        if (victim == NULL || slot->used < victim->used)
            victim = slot;
    }
    if (victim == NULL || !VoxelBuildings_PageSize((unsigned)page, &w, &h, &th))
        return;
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
        if (drop == NULL && quiet)
        {
            victim->page = -1;
            sPageRingRetry[page] = sFrame + VOXEL_PAGE_QUIET_RETRY_FRAMES;
            return;
        }
        if (drop == NULL)
        {
            /* A page not needed this frame - the map left behind by a warp -
             * is droppable in a frame or two: ask again then, not seconds
             * later, or the new map stands without its buildings meanwhile. */
            bool soon = false;

            for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
                if (&sPageSlots[i] != victim && sPageSlots[i].tex.data != NULL
                 && sPageSlots[i].used != sFrame)
                    soon = true;
            victim->page = -1;
            sPageRetry[page] = sFrame + (soon ? 2u : VOXEL_PAGE_RETRY_FRAMES);
            if (!soon)
            {
                if (!sPageNoRoomLogged[page])
                {
                    sPageNoRoomLogged[page] = true;
                    ++sStats.errors;
                    CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no room for the %ux%u building page %d "
                                 "(largest gap %lu of %lu KiB); retrying later",
                                 w, h, page, (unsigned long)(VoxelArena_LargestGap(&sPageArena) >> 10),
                                 (unsigned long)(sPageArena.size >> 10));
                }
            }
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

/* ── Chunk cache ────────────────────────────────────────────────────────── */

/*
 * Which slot holds a square, found without walking all of them: every frame
 * asks after every square of the view and of the ring around it, and a linear
 * search touched the whole 128-slot array (some 25 KiB, more than the data
 * cache) for each, which was most of the time the visit itself took on
 * hardware. A chain per bucket, by the slot number plus one (0 ends it). A
 * slot is entered when a build gives it its square (JobFinish, DraftChunk) and
 * left when it is released; nothing else changes who a slot is.
 */
#define VOXEL_CHUNK_BUCKETS 128u
static uint8_t sChunkHead[VOXEL_CHUNK_BUCKETS];
static uint8_t sChunkNext[VOXEL_CHUNK_SLOTS];

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

    sChunkNext[slot] = *head;
    *head = (uint8_t)(slot + 1u);
}

static void ReleaseChunk(VoxelChunk *chunk)
{
    if (chunk->used)
        ChunkUnindex(chunk);
    VoxelArena_Free(&sChunkArena, chunk->vram);
    free(chunk->crowns);
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
    uint32_t align = sChunkArena.align;

    memset(freed, 0, sizeof(freed));
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i)
        if (Evictable(&sChunks[i], forView))
        {
            int piece = VoxelArena_PieceOf(&sChunkArena, sChunks[i].vram);

            if (piece >= 0)
                freed[piece] = true;
        }
    return VoxelArena_LargestGapFreeing(&sChunkArena, freed) >= ((bytes + align - 1u) & ~(align - 1u));
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
        ReleaseChunk(victim);
        block = VoxelArena_Alloc(&sChunkArena, bytes, false);
        if (block != NULL)
            return block;
    }
}

/* ── Init / shutdown ────────────────────────────────────────────────────── */

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
    } crowns[] = {{0, 0, 32, 36}, {32, 32, 16, 32}, {64, 0, 16, 32}};

    for (unsigned c = 0; c < sizeof(crowns) / sizeof(crowns[0]); ++c)
        for (unsigned y = crowns[c].y; y < crowns[c].y + crowns[c].h; ++y)
            for (unsigned x = crowns[c].x; x < crowns[c].x + crowns[c].w; ++x)
                VoxelGrade_Brighten(&texels[CtrVideo_Texel(x, y, VOXEL_TREE_TEXTURE_DIM)], 1,
                                    VOXEL_TREE_BRIGHTNESS);
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
    sUniShadeTint = shaderInstanceGetUniformLocation(sProgram.vertexShader, "shadeTint");
    sUniTintDiff = shaderInstanceGetUniformLocation(sProgram.vertexShader, "tintDiff");
    sUniGrade = shaderInstanceGetUniformLocation(sProgram.vertexShader, "grade");
    sUniFog = shaderInstanceGetUniformLocation(sProgram.vertexShader, "fog");
    sUniDappleU = shaderInstanceGetUniformLocation(sProgram.vertexShader, "dappleU");
    sUniDappleV = shaderInstanceGetUniformLocation(sProgram.vertexShader, "dappleV");
    if (sUniProjection < 0 || sUniModelView < 0 || sUniShadeTint < 0
     || sUniTintDiff < 0 || sUniGrade < 0 || sUniFog < 0
     || sUniDappleU < 0 || sUniDappleV < 0)
    {
        step = "locate shader uniforms";
        goto fail;
    }

    /* First, while VRAM is still in one piece. */
    step = "chunk mesh VRAM";
    sChunkBlock = vramAlloc(VOXEL_CHUNK_VRAM_BUDGET);
    if (sChunkBlock == NULL)
        goto fail;
    VoxelArena_Init(&sChunkArena, sChunkBlock, VOXEL_CHUNK_VRAM_BUDGET, 16,
                    sChunkPieces, VOXEL_CHUNK_SLOTS + 1);
    /* Not fatal: without it the buildings are not loaded, exactly as without
     * buildings.bin (the houses stay their cells' flat drawing). */
    sPageBlock = vramAlloc(VOXEL_PAGE_VRAM_BUDGET);
    VoxelArena_Init(&sPageArena, sPageBlock, sPageBlock != NULL ? VOXEL_PAGE_VRAM_BUDGET : 0,
                    128, sPagePieces, VOXEL_PAGE_PIECES);
    memset(sPageRetry, 0, sizeof(sPageRetry));
    memset(sPageRingRetry, 0, sizeof(sPageRingRetry));
    memset(sPageNoRoomLogged, 0, sizeof(sPageNoRoomLogged));
    memset(sGlowNoRoomLogged, 0, sizeof(sGlowNoRoomLogged));

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
    /* Optional: the free-roam rig's turned crowns; without it they stay as built. */
    sCrownDyn = linearAlloc(VOXEL_CROWN_VERTICES * sizeof(VoxelGpuVertex));
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
#endif

    /* Not fatal: without models the houses stay their cells' flat drawing. */
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
        sPageSlots[i].page = -1;
    sBuildingsLoaded = sPageBlock != NULL && VoxelBuildings_Init();
    sHaveBuildings = sBuildingsLoaded;
    if (sHaveBuildings)
        StreamStart();

    /* Optional: animation only. Failure must not prevent the overworld from
     * loading when homebrew linear memory is tighter than on Azahar. */
    sAnimOut = linearAlloc(VOXEL_ANIM_SLOTS * sizeof(VoxelAnimSlot));
    if (sAnimOut == NULL)
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no linear memory for the animated tiles");
    if (sHaveBuildings && VoxelBuildings_AnimRunCount() != 0)
    {
        sBuildingAnimOut = linearAlloc(VOXEL_BUILDING_ANIM_BYTES);
        if (sBuildingAnimOut == NULL)
            CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no linear memory for the buildings' animated texels");
    }
    sAnimWorkPage = 0;
    sAnimWorkAtlas = NULL;
    sReady = true;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL stream policy 7: drafts for holes, hashed chunk lookup");
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL lighting: %s",
                 CTR_VOXEL_LIGHTING ? "clock sun + cached shadows + cast sprite shadows" : "disabled");
    CtrLog_Write(CTR_LOG_VIDEO,
                 "VOXEL init: %u staging + %u billboard vertices (%lu KiB linear), up to %u VRAM "
                 "atlases of %lu KiB (%ux%u, %u slots), page stream %s, linear free=%lu, VRAM free=%lu",
                 VOXEL_STAGING_VERTICES, VOXEL_DYNAMIC_VERTICES,
                 (unsigned long)((VOXEL_STAGING_VERTICES + VOXEL_DYNAMIC_VERTICES)
                                 * sizeof(VoxelGpuVertex) >> 10),
                 VOXEL_ATLAS_SLOTS,
                 (unsigned long)(VOXEL_ATLAS_PIXELS * sizeof(uint16_t) >> 10),
                 VOXEL_ATLAS_W, VOXEL_ATLAS_H, VOXEL_ATLAS_MAX_SLOTS,
                 sStream.thread == NULL ? "inline"
                 : sStream.systemCore ? "on the system core" : "threaded",
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
    VoxelRegions_Shutdown();
    VoxelSign_Shutdown();
    linearFree(sAnimOut);
    sAnimOut = NULL;
    sAnimWorkPage = 0;
    sAnimWorkAtlas = NULL;
    memset(sAnimDirty, 0, sizeof(sAnimDirty));
    sAnimPending = false;
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
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
    {
        memset(&sPageSlots[i], 0, sizeof(sPageSlots[i]));
        sPageSlots[i].page = -1;
    }
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
    if (sCrownDyn != NULL)
        linearFree(sCrownDyn);
    sCrownDyn = NULL;
    sCrownVertices = 0;
    if (sDvlb != NULL)
    {
        shaderProgramFree(&sProgram);
        DVLB_Free(sDvlb);
        sDvlb = NULL;
    }
    sUniProjection = sUniModelView = -1;
    sAtlasCapped = false;
    sMeshMapGroup = sMeshMapNum = -1;
    sReady = false;
}

bool CtrVoxel_IsAvailable(void)
{
#if CTR_VOXEL_ENABLED
    if (sReady && !sParked && VoxelWorld_IsMapAvailable())
        return true;
    /* Not an overworld frame: the 2D compositor owns it. Recorded so the
     * overlay can tell "not the overworld" from "the voxel path broke". */
    sStatus = sReady ? "2d" : "off";
    return false;
#else
    return false;
#endif
}

const CtrVoxelStats *CtrVoxel_GetStats(void) { return &sStats; }

/* ── Atlas cache ────────────────────────────────────────────────────────── */

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
/* Bound all atlas pages together to the original six-texture budget. */
static bool AtlasInView(const VoxelAtlasSlot *slot)
{
    for (unsigned i = 0; i < VoxelWorld_InstanceCount(); ++i)
    {
        const VoxelMapInstance *m = VoxelWorld_Instance(i);
        if (m->primaryTileset == slot->primaryTileset && m->secondaryTileset == slot->secondaryTileset
         && (i == 0 || (m->originX < sViewRect[2] && m->originX + m->width > sViewRect[0]
                    && m->originY < sViewRect[3] && m->originY + m->height > sViewRect[1])))
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
static bool AllocateAtlasPage(VoxelAtlasSlot *slot, unsigned page, bool mayEvict)
{
    C3D_Tex *tex = AtlasTex(slot, page);
    if (tex->data != NULL)
        return true;
    for (;;)
    {
        if (CountAllocatedAtlases() < VOXEL_ATLAS_SLOTS
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
        sAtlasCapacityBlocked = true;
        CtrVideo_RequestPlaneRelease();
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

/* ── Building ───────────────────────────────────────────────────────────── */

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
#if CTR_VOXEL_LIGHTING
    if (!site->inst->indoor)
        return VoxelLighting_Hash(site->x0, site->y0, site->x1, site->y1);
#endif
    return VoxelWorld_BlockHash(site->x0 - 1, site->y0 - VOXEL_CHUNK_MARGIN_NORTH - 1,
                                site->x1 + 1, site->y1 + 2);
}

/*
 * ── Building, a few rows at a time ─────────────────────────────────────────
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
    JOB_GROUND, JOB_TREES, JOB_CROWNS, JOB_MODELS, /* map chunks */
    JOB_BORDER, JOB_BORDER_TREES, JOB_BORDER_CROWNS, /* belt chunks */
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
    unsigned terrainCount, crownFirst, buildingFirst;
    unsigned terrainFirst[VOXEL_ATLAS_PAGES + 1u];
    unsigned sortPage, sortScan, sortWrite;
    VoxelBuildingCursor models;
    unsigned staticIndex;   /* JOB_STATICS: the next of sJobStatics */
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
} sJob;

/* The crown cards the build in hand has recorded (voxel_tree.h): a chunk's
 * is at most its cells and the row each may add. */
#define VOXEL_JOB_CROWNS (2u * VOXEL_CHUNK * VOXEL_CHUNK)
static VoxelCrown sJobCrownItems[VOXEL_JOB_CROWNS];
static VoxelCrownList sJobCrowns = {sJobCrownItems, VOXEL_JOB_CROWNS, 0, false};

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
    sJobCrowns.count = 0;
    sJobCrowns.overflow = false;
    sBuilder.crowns = &sJobCrowns;
    sJob.crownFirst = 0;
    VoxelBuilder_SetAtlas(&sBuilder, &atlas->map);
    VoxelBuilder_SetOrigin(&sBuilder, site->baseX, site->baseY);
    /* The whole map stands at its base (voxel_relief.h), the belt round the
     * world at the base of the map it rings. */
    sBuilder.base = VoxelRelief_Base(inst);
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

/* Floor and ceiling of a packed coordinate, in whole tiles. */
static int PackedFloor(int32_t q) { return q >> 9; }
static int PackedCeil(int32_t q) { return -((-q) >> 9); }

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
    return sJob.sortPage == VOXEL_ATLAS_PAGES;
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
            sJob.terrainCount = sBuilder.count;
        VoxelTree_EmitInstance(&sBuilder, inst, sJob.col, sJob.row, sJob.col + 1, sJob.row + 1,
                               VOXEL_TREE_TOPS);
        if (++sJob.col < site->x1)
            return;
        sJob.col = site->x0;
        if (++sJob.row < site->y1)
            return;
        sJob.row = site->y0;
        sJob.crownFirst = sBuilder.count;
        sJob.phase = JOB_CROWNS;
        return;
    case JOB_CROWNS:
        VoxelTree_EmitInstance(&sBuilder, inst, sJob.col, sJob.row, sJob.col + 1, sJob.row + 1,
                               VOXEL_TREE_CROWNS);
        if (++sJob.col < site->x1)
            return;
        sJob.col = site->x0;
        if (++sJob.row < site->y1)
            return;
        sJob.row = site->y0;
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
        sJob.phase = JOB_BORDER_TREES;
        return;
    case JOB_BORDER_TREES:
        /* One lit crown cell: a row can exceed the whole post-submit budget. */
        VoxelTree_EmitBorder(&sBuilder, sJob.col, sJob.row, sJob.col + 1, sJob.row + 1,
                             VOXEL_TREE_TOPS);
        if (++sJob.col < site->x1)
            return;
        sJob.col = site->x0;
        if (++sJob.row < site->y1)
            return;
        sJob.row = site->y0;
        sJob.crownFirst = sBuilder.count;
        sJob.phase = JOB_BORDER_CROWNS;
        return;
    case JOB_BORDER_CROWNS:
        VoxelTree_EmitBorder(&sBuilder, sJob.col, sJob.row, sJob.col + 1, sJob.row + 1,
                             VOXEL_TREE_CROWNS);
        if (++sJob.col < site->x1)
            return;
        sJob.col = site->x0;
        if (++sJob.row < site->y1)
            return;
        sJob.row = site->y0;
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
         * is: it appears whole, and no draw converts one inside the frame. */
        while (sJob.staticIndex < sBuilder.staticCount)
        {
            const VoxelStaticDraw *s = &sJobStatics[sJob.staticIndex];

            bool cached = StaticMeshFind(s->pageModel, s->lit) != NULL;

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
static BuildResult JobFinish(VoxelChunk *chunk)
{
    const ChunkSite *site = &sJob.site;
    const VoxelMapInstance *inst = site->inst;
    VoxelAtlasSlot *atlas = sJob.atlas;
    unsigned bytes;
    VoxelGpuVertex *staging;

    if (sStagingUsed + sBuilder.count > VOXEL_STAGING_VERTICES)
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
        block = ChunkVram(bytes, sJob.forView);
        if (block == NULL)
            return BUILD_NO_VRAM;
        VoxelArena_Free(&sChunkArena, chunk->vram);
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
    chunk->crownFirst = sJob.crownFirst;
    chunk->buildingFirst = sJob.buildingFirst;
    /* The crowns to turn: kept only whole (a card the build had no room for
     * would vanish when the rest were turned). */
    free(chunk->crowns);
    chunk->crowns = NULL;
    chunk->crownCount = 0;
    if (sJobCrowns.count != 0 && !sJobCrowns.overflow && !sBuilder.dropped)
    {
        VoxelCrown *copy = malloc(sJobCrowns.count * sizeof(VoxelCrown));

        if (copy != NULL)
        {
            memcpy(copy, sJobCrownItems, sJobCrowns.count * sizeof(VoxelCrown));
            chunk->crowns = copy;
            chunk->crownCount = sJobCrowns.count;
        }
    }
    memcpy(chunk->terrainFirst, sJob.terrainFirst, sizeof(chunk->terrainFirst));
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
    if (CTR_VOXEL_TRACE && sStats.meshRebuilds < 48)
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL chunk %d,%d of %d:%d%s tiles %d,%d..%d,%d -> %u verts",
                     site->cx, site->cy, inst->mapGroup, inst->mapNum,
                     site->border ? " border" : "", site->x0, site->y0, site->x1, site->y1,
                     sBuilder.count);
    ++sStats.meshRebuilds;
    return BUILD_OK;
}

/*
 * ── The draft of a square ──────────────────────────────────────────────────
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

/*
 * The tree crowns for the free-roam rig. A crown is a card leant towards the
 * orbit's fixed view, built into its chunk; from a head that looks anywhere
 * that is a slab seen from its side. While the rig is engaged (the cards of
 * the billboards, VoxelCamera.cardBlend) each chunk on screen that has
 * records of its crowns leaves the built ones out and has them here instead:
 * every card turned about its trunk to face the eye, and by as much of the
 * way as the cards are blended, so that on the orbit's side of the blend
 * they are the built ones exactly (yaw 0).
 *
 * A chunk whose cards do not all fit the frame's room keeps its built ones;
 * the cards behind the eye or past its reach are left out, being unseen.
 */
static void BuildCrowns(void)
{
    const float blend = sCamera.cardBlend;
    const bool exact = blend >= 0.999f;
    const float eyeX = sCamera.x, eyeZ = sCamera.z;
    float lookX = sCamera.lookX - sCamera.x, lookZ = sCamera.lookZ - sCamera.z;
    const float reach = VOXEL_CAMERA_FREE_REACH + 3.0f;
    float lookLength = sqrtf(lookX * lookX + lookZ * lookZ);
    VoxelVertex quad[6];
    unsigned vertices = 0;

    sCrownVertices = 0;
    if (sCrownDyn == NULL || blend <= 0.0005f)
        return;
    if (lookLength > 0.0001f)
    {
        lookX /= lookLength;
        lookZ /= lookLength;
    }
    else
        lookX = lookZ = 0.0f; /* straight up or down: nothing is behind */
    for (unsigned d = 0; d < sDrawCount; ++d)
    {
        const VoxelChunk *chunk = sDraws[d].chunk;
        float worldX = (float)sDraws[d].worldX, worldZ = (float)sDraws[d].worldZ;
        float offsetX = (float)(sDraws[d].worldX - sDynamicX);
        float offsetZ = (float)(sDraws[d].worldZ - sDynamicZ);

        if (chunk->crowns == NULL || chunk->crownCount == 0
         || vertices + chunk->crownCount * 6u > VOXEL_CROWN_VERTICES)
            continue;
        for (unsigned i = 0; i < chunk->crownCount; ++i)
        {
            const VoxelCrown *crown = &chunk->crowns[i];
            float dx = eyeX - (worldX + crown->px), dz = eyeZ - (worldZ + crown->pz);
            float distance2 = dx * dx + dz * dz, s, c;

            /* The eye is at (dx, dz) from the trunk; the card is at -(dx, dz)
             * from the eye: behind it when that is against the look. */
            if (distance2 > reach * reach || -(dx * lookX + dz * lookZ) < -3.0f)
                continue;
            if (distance2 < 1.0e-6f)
            {
                s = 0.0f;
                c = 1.0f;
            }
            else if (exact)
            {
                float inverse = 1.0f / sqrtf(distance2);

                s = dx * inverse;
                c = dz * inverse;
            }
            else
            {
                /* Part of the way from the orbit's yaw (0) to the eye's. */
                float angle = atan2f(dx, dz) * blend;

                s = sinf(angle);
                c = cosf(angle);
            }
            VoxelTree_CrownVertices(crown, s, c, offsetX, offsetZ, quad);
            PackDynamic(quad, 6, sCrownDyn + vertices);
            vertices += 6;
        }
        sDraws[d].turnedCrowns = true;
    }
    sCrownVertices = vertices;
    if (vertices != 0)
        GSPGPU_FlushDataCache(sCrownDyn, vertices * sizeof(VoxelGpuVertex));
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
    if (sStagingUsed + total > VOXEL_STAGING_VERTICES || sChunkUploads >= VOXEL_CHUNK_UPLOADS_MAX)
        return NULL;
    if (bytes != 0 && !CtrVideo_TryVoxelUpload())
        return NULL;

    if (chunk == NULL)
    {
        chunk = FreeChunkSlot(true);
        if (chunk == NULL)
            return NULL;
        ReleaseChunk(chunk);
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
            return NULL;
        VoxelArena_Free(&sChunkArena, chunk->vram);
        chunk->vram = block;
        chunk->bytes = bytes;
    }

    VoxelBuilder_Init(&b, quad, 6);
    VoxelBuilder_SetAtlas(&b, &atlas->map);
    VoxelBuilder_SetOrigin(&b, site->baseX, site->baseY);
    b.base = VoxelRelief_Base(inst);
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
    chunk->count = chunk->terrainCount = chunk->crownFirst = chunk->buildingFirst = total;
    free(chunk->crowns);
    chunk->crowns = NULL;
    chunk->crownCount = 0;
    memcpy(chunk->terrainFirst, terrainFirst, sizeof(chunk->terrainFirst));
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
    chunk->stamp = chunk->viewStamp = sFrame;
    /* Ids the atlas has never seen: it grows to take them (see AcquireAtlas). */
    if (uncovered != 0)
        atlas->extendPending = true;
    ++sDrafts;
    return chunk;
}

/* ── The view ───────────────────────────────────────────────────────────── */

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
    return BoxVisible((float)site->x0, base + VOXEL_UNBUILT_LOW, (float)site->y0,
                      (float)site->x1, base + VOXEL_UNBUILT_HIGH, (float)site->y1);
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
    sDraws[sDrawCount].turnedCrowns = false;
    ++sDrawCount;
    sStats.vertices += chunk->count;
    sStats.draftsVisible += chunk->draft;
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
        drawable = atlas != NULL && chunk->atlas == atlas
                && chunk->atlasGeneration == atlas->generation;
    if (atlas == NULL)
    {
        if (!inView)
            return;
        if (drawable)
            Draw(chunk, site->baseX, site->baseY);
        else
            ++*missing;
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

/* Walks every chunk of every map, and of the belt, that reaches into the ring. */
static void VisitView(float playerX, float playerZ, unsigned *missing, bool atlasAhead)
{
    int vx0 = sViewRect[0], vy0 = sViewRect[1], vx1 = sViewRect[2], vy1 = sViewRect[3];
    /* A chunk all round, and as far again as the player is heading. */
    int px0 = vx0 - VOXEL_PREFETCH + (sLeadX < 0.0f ? (int)sLeadX : 0);
    int py0 = vy0 - VOXEL_PREFETCH + (sLeadZ < 0.0f ? (int)sLeadZ : 0);
    int px1 = vx1 + VOXEL_PREFETCH + (sLeadX > 0.0f ? (int)sLeadX : 0);
    int py1 = vy1 + VOXEL_PREFETCH + (sLeadZ > 0.0f ? (int)sLeadZ : 0);
    unsigned instances = VoxelWorld_InstanceCount();
    const VoxelMapInstance *current = VoxelWorld_Instance(0);
    VoxelAtlasSlot *atlases[MAX_VOXEL_MAP_INSTANCES];
    bool inRing[MAX_VOXEL_MAP_INSTANCES];

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
            int ex = inst->originX + inst->width, ey = inst->originY + inst->height;
            bool inView = Overlaps(inst->originX, inst->originY, ex, ey, vx0, vy0, vx1, vy1);

            if (pass == 0)
            {
                inRing[i] = Overlaps(inst->originX, inst->originY, ex, ey, px0, py0, px1, py1);
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

    for (unsigned i = 0; i < instances; ++i)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(i);
        VoxelAtlasSlot *atlas = atlases[i];
        int ix0, iy0, ix1, iy1;

        if (!inRing[i])
            continue;
        ix0 = px0 > inst->originX ? px0 : inst->originX;
        iy0 = py0 > inst->originY ? py0 : inst->originY;
        ix1 = px1 < inst->originX + inst->width ? px1 : inst->originX + inst->width;
        iy1 = py1 < inst->originY + inst->height ? py1 : inst->originY + inst->height;
        for (int cy = (iy0 - inst->originY) / VOXEL_CHUNK;
             cy <= (iy1 - 1 - inst->originY) / VOXEL_CHUNK; ++cy)
            for (int cx = (ix0 - inst->originX) / VOXEL_CHUNK;
                 cx <= (ix1 - 1 - inst->originX) / VOXEL_CHUNK; ++cx)
            {
                ChunkSite site;

                SiteOf(&site, inst, false, cx, cy);
                VisitSite(&site, atlas,
                          playerX, playerZ, missing);
            }
    }

    /* The belt, on the current map's grid: only squares some tile of which
     * belongs to no map at all. Indoors there is none; the clear is black. */
    if (current == NULL || current->indoor)
        return;
    {
        VoxelAtlasSlot *atlas = FindAtlas(current);
        /* A belt laid as drawn stands its squares' blocks rows south of them
         * (VoxelMesh_EmitBorder): the squares that far north reach the ring. */
        int oy0 = py0 - (int)VoxelRelief_Belt(current) - sBeltOriginY, oy1 = py1 - 1 - sBeltOriginY;
        int ox0 = px0 - sBeltOriginX, ox1 = px1 - 1 - sBeltOriginX;
        int cy0 = (int)floorf(oy0 / (float)VOXEL_CHUNK), cy1 = (int)floorf(oy1 / (float)VOXEL_CHUNK);
        int cx0 = (int)floorf(ox0 / (float)VOXEL_CHUNK), cx1 = (int)floorf(ox1 / (float)VOXEL_CHUNK);

        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
            {
                ChunkSite site;
                bool open = false;

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
 * FrameBegin that returns on it, a late event).
 */
#define VOXEL_AFTER_MAX_MS 9.0f
#define VOXEL_AFTER_MARGIN_MS 1.8f
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
    static unsigned sSlowLogged;

    chunk->buildMs = ms;
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
                     TicksMs(sJob.phaseTicks[JOB_TREES] + sJob.phaseTicks[JOB_BORDER_TREES]
                           + sJob.phaseTicks[JOB_CROWNS] + sJob.phaseTicks[JOB_BORDER_CROWNS]),
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

static void NotePhaseCost(int phase, float ms)
{
    float *estimate = &sPhaseMs[phase];
    float previous = PhaseCost(phase);

    if (ms > VOXEL_SLICE_CAP_MS)
        ms = VOXEL_SLICE_CAP_MS;
    *estimate = ms > previous ? ms : previous * 0.75f + ms * 0.25f;
    sPhaseFrame[phase] = sFrame;
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

/* Takes the most urgent request the frame can afford as the next job. */
static bool StartNextJob(float elapsed, float holeMs, float aheadMs)
{
    for (unsigned i = 0; i < sRequestCount; ++i)
    {
        BuildRequest *r = &sRequests[i];
        const VoxelMapInstance *inst = r->site.inst;
        unsigned need = r->need;
        bool forView = need == NEED_HOLE || need == NEED_STALE;

        if (r->done || sFrame < sRequestWait[r->waitIndex].noRoomUntil)
            continue;
        if (sHolesOnly && need != NEED_HOLE && !RequestOverdue(r))
            continue; /* sorted: holes come first */
        if (elapsed >= (forView ? holeMs : aheadMs))
            return false; /* sorted: nothing after it is more urgent */
        /* The slot may have been recycled for another tileset this update. */
        if (!r->atlas->valid || r->atlas->primaryTileset != inst->primaryTileset
         || r->atlas->secondaryTileset != inst->secondaryTileset)
            continue;
        if (!r->hashKnown)
            r->hash = SiteHash(&r->site);
        r->done = true;
        JobStart(&r->site, r->atlas, r->hash, r->chunk, forView, need == NEED_HOLE);
        sJob.waitIndex = r->waitIndex;
        sJob.overdue = RequestOverdue(r);
        return true;
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
 * Slices of the running job and of the next ones, most urgent first, until
 * the budgets run out: `holeMs` for what the view is missing or has stale,
 * `aheadMs` for the prefetch ring. A finished job is uploaded only `inFrame`
 * (its GPU copy needs the open frame); after FrameEnd it waits, its geometry
 * in the scratch, for the next Update. With `forceHole` a hole on screen
 * gets one slice however the frame is going. Returns the chunks uploaded.
 */
static unsigned RunJobs(uint64_t started, float holeMs, float aheadMs, bool inFrame,
                        bool forceHole, unsigned *missing)
{
    unsigned built = 0, slices = 0;

    for (;;)
    {
        float elapsed = MsSince(started);
        float budget;

        if (!sJob.active && !StartNextJob(forceHole && slices == 0 ? 0.0f : elapsed, holeMs, aheadMs))
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
            float cost = inFrame ? PhaseCostInFrame(phase) : PhaseCost(phase);

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
                static const char *names[] = {"ground", "trees", "crowns", "models", "border", "border-trees", "border-crowns", "sort", "pack", "statics", "upload"};
                lastSlowSlice = sFrame;
                CtrLog_Write(CTR_LOG_VIDEO, "VOXEL slice %s of %d:%d cell=%d,%d: %.2fms remaining=%.2f view=%d rays=%u misses=%u",
                             names[phase], sJob.site.inst->mapGroup, sJob.site.inst->mapNum,
                             cellX, cellY, TicksMs(ticks), budget - elapsed, sJob.forView,
                             LightRays() - rays, (unsigned)(CtrAssets_GetStats()->misses - misses));
            }
            ++slices;
            continue;
        }
        if (!inFrame)
            break;

        /* Finished: upload it into its chunk. */
        {
            VoxelChunk *chunk = sJob.chunk;
            BuildResult result;

            if (VOXEL_STAGING_VERTICES - sStagingUsed < sBuilder.count
             || sChunkUploads >= VOXEL_CHUNK_UPLOADS_MAX)
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
                ReleaseChunk(chunk);
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
                /* Nothing more fits. Ahead of time, stop trying for a while:
                 * the ring would otherwise build a chunk and discard it for as
                 * long as the view holds the cache full. */
                if (!sJob.forView)
                    sAheadBackoff = sFrame + VOXEL_AHEAD_BACKOFF_FRAMES;
                else
                    NoRoomForView();
                sJob.active = false;
                break;
            }
            sJob.active = false;
            ++built;
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

    for (unsigned i = 0; i < sRequestCount && sRequests[i].need == NEED_HOLE; ++i)
    {
        BuildRequest *r = &sRequests[i];
        const VoxelMapInstance *inst = r->site.inst;
        VoxelChunk *chunk;

        if (r->done || r->site.border)
            continue;
        if (made >= VOXEL_DRAFT_PER_FRAME || (made != 0 && MsSince(start) >= VOXEL_DRAFT_MS))
            break;
        /* The running job is for this square: let it finish. */
        if (sJob.active && SameSite(&r->site, &sJob.site))
            continue;
        if (!r->atlas->valid || r->atlas->primaryTileset != inst->primaryTileset
         || r->atlas->secondaryTileset != inst->secondaryTileset)
            continue;
        chunk = DraftChunk(&r->site, r->atlas, r->chunk);
        if (chunk == NULL)
            break; /* no room in the frame's uploads or in VRAM: next frame */
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

static void UpdateView(float playerX, float playerZ, bool crossed, bool cut)
{
    uint64_t started = svcGetSystemTick();
    (void)crossed; /* a crossing is covered by the drafts like any other hole */
    float spare = SpareMs();
    float holeMs = 0.0f, aheadMs = 0.0f, visitMs = 0.0f;
    unsigned missing = 0, built;
    bool holes = false, starved = false;
    bool warmup;

    if (cut || sResumeWarmup)
        sWarmupUntil = sFrame + VOXEL_WARMUP_FRAMES;
    sResumeWarmup = false;
    warmup = sFrame < sWarmupUntil;
    if (warmup)
    {
        holeMs = VOXEL_WARMUP_MS;
        aheadMs = sFrame < sAheadBackoff ? 0.0f
                : Clamp(spare - VOXEL_AHEAD_MARGIN_MS, 0.0f, VOXEL_AHEAD_MAX_MS);
    }
    UpdateFrustum();
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
    VisitView(playerX + sLeadX, playerZ + sLeadZ, &missing,
              warmup ? aheadMs > 0.0f : sFrame >= sAheadBackoff);
    sStats.atlasMs = MsSince(started);
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
                         "%u holes left, pending=%u", sDrafts - before, sDrafts, sDraftMs,
                         missing, sRequestCount);
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
        built = RunJobs(warmup ? started : buildStart, holeMs, aheadMs, true,
                        warmup || starved, &missing);
        sInFrameBuildMs = MsSince(buildStart);
    }
    sHolesOnly = false;

    if (sWarmupUntil != 0 && missing == 0 && !AtlasJobBusy())
        sWarmupUntil = 0;
    sLastBuildMs = MsSince(started);
    sStats.meshMs = sLastBuildMs;
    if (sStats.meshMs > sStats.meshPeakMs)
        sStats.meshPeakMs = sStats.meshMs;
    sStats.chunksMissing = missing;
    sStats.visibleChunks = sDrawCount;
    sStats.frameBuilds = built;
    sStats.pendingBuilds = sRequestCount;
    sStats.draftsMade = sDrafts;
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
 */
static uint64_t sAfterEndTick;
static float sTailMs;

void CtrVoxel_NoteFrameWait(uint64_t waitTick)
{
    sTailMs = sAfterEndTick && waitTick > sAfterEndTick ? TicksMs(waitTick - sAfterEndTick) : 0.0f;
    sAfterEndTick = 0;
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

    sStats.afterMs = sStats.afterBudgetMs = 0.0f;
    if (!sReady || !sViewReady)
        return;
    sViewReady = false;
    /* The game's and the audio's share of the frame to come, read off the
     * last ones: taken at once when it rises, let go slowly. */
    if (others > VOXEL_OTHERS_CAP_MS)
        others = VOXEL_OTHERS_CAP_MS;
    sOthersMs = others > sOthersMs ? others : sOthersMs * 0.85f + others * 0.15f;
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
        sAfterEndTick = started;
        return;
    }
    aheadMs = sFrame < sAheadBackoff ? 0.0f : budget;
    /* The asset payloads the builds resolve are the ones the frame's own
     * update resolved: nothing has run in between that retires one. */
    VoxelWorld_BeginBatch();
    RunAtlasJob(started, sAtlasJob.forView ? budget : aheadMs, false);
    if (sJob.active && !JobValid())
        JobCancel();
    RunJobs(started, budget, aheadMs, false, false, NULL);
    sStats.afterMs = MsSince(started);
    sLastBuildMs += sStats.afterMs;
    sAfterEndTick = svcGetSystemTick();
}

/* ── Map changes ────────────────────────────────────────────────────────── */

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
    VoxelCamera_SetGround(&sCamera, VoxelRelief_LiftAt(playerX + 0.5f, playerZ + 0.5f)
                                    + VoxelEntities_PlayerDeck(), 1);
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
        };

        for (unsigned f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f)
            hash = (hash ^ fields[f]) * 16777619u;
    }
    return hash;
}

/* ── Frame update ───────────────────────────────────────────────────────── */

/* ── The 3D battle ──────────────────────────────────────────────────────── */

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
    VoxelCamera_ToOrbit(&sCamera);
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
        for (unsigned index = 0; index < VoxelWorld_InstanceCount(); ++index)
            VoxelSign_EmitNight(&glass, &ground, VoxelWorld_Instance(index),
                               sLampView[0], sLampView[1], sLampView[2], sLampView[3]);
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
 * The animated tiles (water, flowers) of the current map's atlas. A pass takes
 * the tiles written since the last one for one page and redraws the metatiles
 * that show them, a few a frame (VoxelAtlas_AnimatedSlots), each copied by the
 * GPU into its two runs of the page. Nothing of the page is read back and
 * nothing but those 512 bytes a slot is flushed or copied: the page used to be
 * read back into a CPU copy, recomposed and written out whole, which was 6-7 ms
 * of a frame whenever it came round.
 *
 * The maps beside it in view are drawn from atlases of their own tileset pair
 * (Route 107's Dewford pair beside Route 108's Slateport one): those that
 * share its primary tileset take their turn too, for the metatiles drawn from
 * primary tiles alone - the sea, the shore, the flowers. Only the current map
 * had its pass, and the sea of the route just left stood still beside the
 * moving sea of the new one.
 */
static bool AnimShares(const VoxelAtlasSlot *atlas, const VoxelAtlasSlot *active)
{
    return atlas == active
        || (atlas->valid && atlas->primaryTileset == active->primaryTileset && AtlasInView(atlas));
}

static void AnimateTiles(const VoxelMapInstance *inst)
{
    VoxelAtlasSlot *active, *work;
    VoxelMapInstance pair;
    unsigned pages, page, room, count;

    if (!sAnimPending || sAnimOut == NULL || sAtlasUploadPending || AtlasJobBusy())
        return;
    active = FindAtlas(inst);
    if (active == NULL || !active->valid)
        return;
    if (sAnimWorkPage != 0 && (sAnimWorkAtlas == NULL || !AnimShares(sAnimWorkAtlas, active)))
        sAnimWorkPage = 0; /* the map changed pair mid-pass: its next write redraws */
    if (sAnimWorkPage == 0)
    {
        bool any = false;

        /* the atlases in turn, from the one after the last pass's */
        page = 0;
        work = NULL;
        for (unsigned k = 0; k < VOXEL_ATLAS_SLOTS && !any; ++k)
        {
            unsigned a = (sAnimNextAtlas + k) % VOXEL_ATLAS_SLOTS;
            VoxelAtlasSlot *atlas = &sAtlases[a];

            if (!AnimShares(atlas, active))
                continue;
            pages = VoxelAtlas_PageCount(&atlas->map);
            for (unsigned step = 0; step < pages && !any; ++step)
            {
                unsigned p = (sAnimNext + step) % pages;

                for (unsigned i = 0; i < sizeof(sAnimDirty[a][p]); ++i)
                {
                    sAnimDirty[a][p][i] &= atlas->map.tileMask[p][i];
                    /* tiles 512 on: BG VRAM holds the current map's secondary */
                    if (atlas != active && i >= 512u / 8u)
                        sAnimDirty[a][p][i] = 0;
                    any = any || sAnimDirty[a][p][i] != 0;
                }
                if (any)
                {
                    page = p;
                    work = atlas;
                    sAnimNextAtlas = (a + 1u) % VOXEL_ATLAS_SLOTS;
                    sAnimNext = (p + 1u) % pages;
                }
            }
        }
        if (!any)
        {
            sAnimPending = false;
            return;
        }
        memcpy(sAnimWork, sAnimDirty[work - sAtlases][page], sizeof(sAnimWork));
        memset(sAnimDirty[work - sAtlases][page], 0, sizeof(sAnimDirty[work - sAtlases][page]));
        sAnimWorkPage = page + 1u;
        sAnimWorkAtlas = work;
        sAnimCursor = 0;
    }
    work = sAnimWorkAtlas;
    page = sAnimWorkPage - 1u;
    room = CtrVideo_VoxelUploadsLeft() / 2u; /* a slot is two copies */
    if (room > VOXEL_ANIM_SLOTS)
        room = VOXEL_ANIM_SLOTS;
    if (AtlasTex(work, page)->data == NULL && work != active)
    {
        /* a page the map beside has given up: its pass with it */
        sAnimWorkPage = 0;
        sAnimWorkAtlas = NULL;
        return;
    }
    if (room == 0 || AtlasTex(work, page)->data == NULL)
        return;
    if (work == active)
        count = VoxelAtlas_AnimatedSlots(inst, &work->map, page, CtrVideo_GetBgVram(), sAnimWork,
                                         &sAnimCursor, sAnimOut, room);
    else
    {
        memset(&pair, 0, sizeof(pair));
        pair.primaryTileset = work->primaryTileset;
        pair.secondaryTileset = work->secondaryTileset;
        count = VoxelAtlas_SharedAnimatedSlots(&pair, &work->map, page, CtrVideo_GetBgVram(), sAnimWork,
                                               &sAnimCursor, sAnimOut, room);
    }
    if (count != 0)
    {
        uint8_t *dest = (uint8_t *)AtlasTex(work, page)->data;

        GSPGPU_FlushDataCache(sAnimOut, count * sizeof(VoxelAnimSlot));
        for (unsigned i = 0; i < count; ++i)
        {
            /* block (2c, 2r) of 64 to a row, 128 bytes each; the second run is
             * the block row below */
            unsigned c = sAnimOut[i].slot % VOXEL_ATLAS_COLUMNS, r = sAnimOut[i].slot / VOXEL_ATLAS_COLUMNS;
            unsigned first = (2u * r * (VOXEL_ATLAS_W / 8u) + 2u * c) * 128u;
            unsigned second = first + (VOXEL_ATLAS_W / 8u) * 128u;

            if (!CtrVideo_TryVoxelUpload() || !CtrVideo_TryVoxelUpload())
            {
                /* The room was counted; if the queue still refuses, the rest
                 * is redrawn by a later pass. */
                sAnimCursor = 0;
                break;
            }
            C3D_SyncTextureCopy((u32 *)sAnimOut[i].pixels, 0, (u32 *)(dest + first), 0, 256, 8);
            C3D_SyncTextureCopy((u32 *)(sAnimOut[i].pixels + 128), 0, (u32 *)(dest + second), 0, 256, 8);
        }
        ++sStats.animationUploads;
        sStats.animatedMetatiles += count;
        sAtlasUploadPending = true;
    }
    if (sAnimCursor >= VOXEL_METATILE_IDS)
    {
        sAnimWorkPage = 0;
        sAnimWorkAtlas = NULL;
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

        if (!VoxelBuildings_AnimRun(run, &page, &layout, &offset, &bytes) || layout != inst->layoutId
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
    if (sAnimPending || sAnimWorkPage != 0)
        memset(sAnimDirty, 0, sizeof(sAnimDirty));
    sAnimPending = false;
    sAnimWorkPage = 0;
    sAnimWorkAtlas = NULL;
    if (sBuildingAnimPending)
        memset(sBuildingAnimDirty, 0, sizeof(sBuildingAnimDirty));
    sBuildingAnimPending = false;
}

bool CtrVoxel_Update(void)
{
    const VoxelMapInstance *inst;
    float playerX = 0.0f, playerZ = 0.0f;
    float smoothX = 0.0f, smoothZ = 0.0f;
    int mapGroup = -1, mapNum = -1;
    bool mapChanged, cut = false;
    uint64_t started;

    if (!sReady || sParked)
    {
        sStatus = sReady ? "parked" : "off";
        return false;
    }
    /* The game has left the battle: the field is drawn again, as it was. */
    if (sBattle.on && !VoxelBattle_GameInBattle())
        CtrVoxel_EndBattle();
    started = svcGetSystemTick();
    ++sFrame;

    /* CtrVideo_Present has opened the frame and waited for the previous GPU
     * queue. Only now may the sources of last frame's uploads be reused. */
    sStagingUsed = 0;
    sChunkUploads = 0;
    sAtlasUploadPending = false;
    sAtlasCapacityBlocked = false;
    sViewReady = false;
    /* Nothing retires an asset payload between here and the end of the
     * update, so resolved pointers may be reused throughout. */
    VoxelWorld_BeginBatch();
    VoxelWorld_BuildInstances();
    sStats.instances = VoxelWorld_InstanceCount();
    ReadAhead();
    inst = VoxelWorld_Instance(0);
    if (inst == NULL)
    {
        sStatus = "nomap";
        return false;
    }

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
            sMapVersion = version;
            sInstanceSignature = signature;
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
        sMeasureRead = sFrame + 64u;
    }
    sGlowWanted = sMeasureGlow && CtrDayNight_Get()->glow > 0.0f;

#if CTR_VOXEL_LIGHTING
    {
        const CtrDayNightSun *sun = CtrDayNight_GetSun();

        if (VoxelLighting_SetSun(sun->dx, sun->dz, sun->strength, sun->key))
        {
            StaticMeshesRelight();
            ++sEpoch;
        }
    }
#endif

    VoxelWorld_GetLocation(&mapGroup, &mapNum);
    /*
     * Two different positions on purpose. The camera follows the interpolated
     * one, or it lurches a whole tile per step; the view uses the tile-aligned
     * one, because that is the granularity at which the geometry changes.
     */
    VoxelWorld_GetPlayerWorldCoords(&playerX, &playerZ);
    /* In a battle the player's sprite is one of the battle's. */
    if (sBattle.on)
    {
        smoothX = playerX;
        smoothZ = playerZ;
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
        {
            VoxelCamera_ToOrbit(&sCamera);
            sBattle.field = sCamera;
        }
    }
    else if (!sBattle.on)
    {
        VoxelCamera_SetGround(&sCamera, VoxelRelief_LiftAt(smoothX + 0.5f, smoothZ + 0.5f)
                                        + VoxelEntities_PlayerDeck(), 0);
        VoxelCamera_Update(&sCamera, smoothX, smoothZ);
    }
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

    sStats.worldMs = (float)((svcGetSystemTick() - started) * 1000.0 / SYSCLOCK_ARM11);
    {
        uint64_t streamStart = svcGetSystemTick();

        StreamPages();
        sStats.streamMs = MsSince(streamStart);
    }
    /* The view is visited around what is looked at: the stage, in a battle. */
    if (sBattle.on)
        UpdateView(sCamera.targetX, sCamera.targetZ, false, cut);
    else
        UpdateView(playerX, playerZ, mapChanged && !cut, cut);
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
                bool near = Overlaps(map->originX, map->originY, map->originX + map->width,
                                     map->originY + map->height,
                                     sViewRect[0] - margin, sViewRect[1] - margin,
                                     sViewRect[2] + margin, sViewRect[3] + margin);
                bool inView = Overlaps(map->originX, map->originY, map->originX + map->width,
                                       map->originY + map->height,
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
        sCrownVertices = 0;
        sLampVertices = sLampGroundVertices = 0;
        sLampReady = false;
        LampsRelease(false);
        sStats.reflections = 0;
#if CTR_VOXEL_LIGHTING
        sShadowVertices = 0;
#endif
        sStats.spritesMs = 0.0f;
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

        sDynamicX = (int)floorf(sCamera.targetX);
        sDynamicZ = (int)floorf(sCamera.targetZ);

        UpdateNightLamps();
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
        sStats.spriteUpdates += VoxelEntities_Emit(&sprites, (uint16_t *)sSpriteAtlas.data,
                                                   &sCamera, shadowBuilder, &reflections);
        sReflectionVertices = reflections.count;
        sStats.reflections = reflections.count / 6u;   /* quads (EmitReflection: a strip each) */
        PackDynamic(dynamic + VOXEL_REFLECTION_FIRST, reflections.count,
                    sDynamic + VOXEL_REFLECTION_FIRST);
        if (reflections.dropped != 0)
        {
            ++sStats.errors;
            CtrLog_Write(CTR_LOG_ERROR, "VOXEL: reflection staging overflow");
        }
        sSpriteVertices = sprites.count;
        PackDynamic(dynamic, sprites.count, sDynamic);
#if CTR_VOXEL_LIGHTING
        sShadowVertices = shadows.count;
        PackDynamic(dynamic + VOXEL_SHADOW_FIRST, shadows.count, sDynamic + VOXEL_SHADOW_FIRST);
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
        /* The crowns of the trees, turned to the eye while the rig is on. */
        BuildCrowns();
    }
    ReportPackErrors("the billboards around", sDynamicX, sDynamicZ, mapGroup, mapNum);


    sStats.chunks = 0;
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i)
        if (sChunks[i].used)
            ++sStats.chunks;
    sStats.vertices += sSpriteVertices + sReflectionVertices + sLampVertices + sLampGroundVertices
                     + sCrownVertices;
#if CTR_VOXEL_LIGHTING
    sStats.vertices += sShadowVertices;
#endif
    /* Each walks its allocator's list: for the stats, twice a second is plenty. */
    if (sFrame % 30u == 0u)
    {
        sStats.linearFree = (unsigned long)linearSpaceFree();
        sStats.vramFree = (unsigned long)vramSpaceFree();
    }
    sStats.updateMs = (float)((svcGetSystemTick() - started) * 1000.0 / SYSCLOCK_ARM11);
    if (sStats.updateMs > sStats.updatePeakMs)
        sStats.updatePeakMs = sStats.updateMs;
    sStatus = sAtlasCapacityBlocked ? "capacity"
            : sWarmupUntil != 0 && sStats.chunksMissing != 0 ? "loading"
            : sDrawCount != 0 ? "on" : "nomesh";
    return sDrawCount != 0 && !sAtlasCapacityBlocked
        && !(sWarmupUntil != 0 && sStats.chunksMissing != 0);
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
        FreeAtlasTextures(slot);
        AnimForget(slot);
        memset(&slot->tex, 0, sizeof(slot->tex));
        slot->valid = false;
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
     * cells are the flat drawing again. */
    if (sPageBlock == NULL && sBuildingsLoaded)
    {
        if (++sUnparkPageFails < VOXEL_UNPARK_PAGE_TRIES)
        {
            vramFree(sChunkBlock);
            sChunkBlock = NULL;
            return false;
        }
        VoxelBuildings_Shutdown();
        sBuildingsLoaded = false;
        sBuildingsDropped = sBuildingsChanged = true;
        CtrLog_Write(CTR_LOG_ERROR, "VOXEL: no VRAM for the building pages in %u tries (largest=%lu):"
                     " the world comes back without its buildings", sUnparkPageFails,
                     (unsigned long)CtrGpuMem_LargestVram());
    }
    else if (sPageBlock != NULL && sBuildingsDropped)
    {
        sBuildingsLoaded = VoxelBuildings_Init();
        sBuildingsDropped = !sBuildingsLoaded;
        sBuildingsChanged = true;
        CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: building pages have room again, buildings %s",
                     sBuildingsLoaded ? "loaded" : "still unloaded");
    }
    sUnparkPageFails = 0;
    VoxelArena_Init(&sChunkArena, sChunkBlock, VOXEL_CHUNK_VRAM_BUDGET, 16,
                    sChunkPieces, VOXEL_CHUNK_SLOTS + 1);
    VoxelArena_Init(&sPageArena, sPageBlock, sPageBlock != NULL ? VOXEL_PAGE_VRAM_BUDGET : 0,
                    128, sPagePieces, VOXEL_PAGE_PIECES);
    memset(sPageRetry, 0, sizeof(sPageRetry));
    memset(sPageRingRetry, 0, sizeof(sPageRingRetry));
    memset(sPageNoRoomLogged, 0, sizeof(sPageNoRoomLogged));
    memset(sGlowNoRoomLogged, 0, sizeof(sGlowNoRoomLogged));
    sHaveBuildings = sPageBlock != NULL && sBuildingsLoaded;
    if (sHaveBuildings)
        StreamStart();
    sParked = false;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL: VRAM back%s (VRAM free=%lu)",
                 sPageBlock != NULL ? "" : ", without the building pages", (unsigned long)vramSpaceFree());
    return true;
}

void CtrVoxel_ArenaUse(uint32_t *chunkUsed, uint32_t *chunkLargest, uint32_t *pageUsed, uint32_t *pageLargest)
{
    *chunkUsed = sChunkArena.used;
    *chunkLargest = sChunkBlock != NULL ? VoxelArena_LargestGap(&sChunkArena) : 0;
    *pageUsed = sPageArena.used;
    *pageLargest = sPageBlock != NULL ? VoxelArena_LargestGap(&sPageArena) : 0;
}

const char *CtrVoxel_Status(void)
{
    return sReady ? sStatus : "off";
}

/* ── Draw ───────────────────────────────────────────────────────────────── */

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

static void SetModelView(const C3D_Mtx *view, int worldX, int worldZ)
{
    C3D_Mtx model;

    Mtx_Copy(&model, view);
    Mtx_Translate(&model, (float)worldX, 0.0f, (float)worldZ, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniModelView, &model);
}

/*
 * Fetched again for every draw, not hoisted: C3D_GetBufInfo is what marks the
 * buffer configuration dirty, so writing through a pointer taken once leaves
 * every draw after the first reading the first buffer's vertices.
 */
static void BindVertices(const VoxelGpuVertex *vertices)
{
    C3D_BufInfo *buf = C3D_GetBufInfo();

    BufInfo_Init(buf);
    BufInfo_Add(buf, vertices, sizeof(VoxelGpuVertex), 2, 0x10);
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
            break;
        case VOXEL_WEATHER_FOG:
            light = FogLight(&light, VoxelWorld_Underground());
            break;
        case VOXEL_WEATHER_PARTICLES:
            /* The weather's own ash or sand fills the air instead. */
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
    float haze = light->haze;
    float fogStart, fogScale;

    /*
     * In the free-roam rig the eye stands on the ground and the view is long:
     * the haze is measured to the rig's reach instead of the orbit's short
     * eye-to-player distance, and rises to the whole of the colour by that
     * distance, so that the world's far edge is the haze (and the sky behind
     * it, CtrVoxel_SkyColour) instead of a cut. Indoors there is none.
     */
    if (haze > 0.001f && sCamera.rig > 0.0f)
    {
        float reach = VOXEL_CAMERA_FREE_REACH * 0.9f;
        float span = light->hazeStart + light->hazeRamp;

        eye += (reach / span - eye) * sCamera.rig;
        haze += (1.0f - haze) * sCamera.rig;
    }
    fogStart = eye * light->hazeStart;
    fogScale = haze / (eye * light->hazeRamp);

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
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniFog, -fogScale, -fogStart * fogScale, haze, 0.0f);
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

static void DappleFrame(void)
{
    double seconds = (double)svcGetSystemTick() / SYSCLOCK_ARM11;
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
#ifdef CTR_F8_LOAD
/* Phase F8's load (see VOXEL_MOTES_ROOM): the rays drawn this many times. */
static unsigned sRayLayers = 1;
static uint32_t sF8Drawn[2];
#endif

static void DrawRays(const VoxelLight *light, const VoxelLight *unlit)
{
    float angle = C3D_AngleFromDegrees(VOXEL_RAY_ANGLE);
    float along[2] = {sinf(angle), -cosf(angle)}, across[2] = {cosf(angle), sinf(angle)};
    double seconds = (double)svcGetSystemTick() / SYSCLOCK_ARM11;
    float drift = (float)fmod(seconds * VOXEL_RAY_DRIFT, 1.0);
    unsigned strength = (unsigned)(light->rays * 255.0f + 0.5f);
    C3D_Mtx ortho, identity;
    C3D_TexEnv *env;

    Mtx_Ortho(&ortho, 0.0f, 1.0f, 0.0f, 1.0f, 0.1f, 10.0f, false);
    FitToLogicalSurface(&ortho);
    Mtx_Identity(&identity);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniProjection, &ortho);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniModelView, &identity);
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
#ifdef CTR_F8_LOAD
    for (unsigned layer = 0; layer < sRayLayers; ++layer)
        C3D_DrawArrays(GPU_TRIANGLES, 0, (int)sRayCount);
    ++sF8Drawn[1];
#else
    C3D_DrawArrays(GPU_TRIANGLES, 0, (int)sRayCount);
#endif
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
/*
 * Phase F8's measurement of the room left for effects (-DCTR_F8_LOAD, never a
 * player's build): every 600 frames of the voxel world the dust's count and
 * how many times the rays are drawn over themselves step through 32/64/128
 * and x1/x2/x3, logged, so the console's profile gives what each load costs.
 */
#ifdef CTR_F8_LOAD
#define VOXEL_MOTES_ROOM 128
static unsigned sMoteCount = VOXEL_MOTES;
#else
#define VOXEL_MOTES_ROOM VOXEL_MOTES
#define sMoteCount VOXEL_MOTES
#endif
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
    sMotes = linearAlloc(VOXEL_MOTES_ROOM * VOXEL_MOTE_VERTICES * sizeof(VoxelGpuVertex));
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
 * Colour is the constant warm white; alpha is the vertex's strength (the
 * twinkle, gone at the corners) x the light's. Added over what is behind.
 */
static void DrawMotes(const C3D_Mtx *view, const VoxelLight *light, const VoxelLight *unlit)
{
    int originX = (int)floorf(sCamera.targetX), originZ = (int)floorf(sCamera.targetZ);
    float seconds = (float)fmod((double)svcGetSystemTick() / SYSCLOCK_ARM11, 3600.0);
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

    for (unsigned i = 0; i < sMoteCount; ++i)
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
    GSPGPU_FlushDataCache(sMotes, sMoteCount * VOXEL_MOTE_VERTICES * sizeof(VoxelGpuVertex));

    SetModelView(view, originX, originZ);
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
    C3D_DrawArrays(GPU_TRIANGLES, 0, sMoteCount * VOXEL_MOTE_VERTICES);
#ifdef CTR_F8_LOAD
    ++sF8Drawn[0];
#endif
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
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleU,
                  sDappleU[0], sDappleU[1], sDappleU[2], u0 - floorf(u0));
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUniDappleV,
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

static const struct
{
    float period;       /* tiles the pattern covers before it repeats */
    float height;       /* tiles above the player's ground */
    float driftX, driftZ; /* tiles per second */
    float strength;     /* the most the bank covers, at full fog */
} sFogBanks[2] = {
    {12.0f, 0.30f, -0.55f, 0.00f, 0.58f},
    {7.0f, 1.10f, -0.85f, -0.25f, 0.34f},
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
static void TerrainTexEnv(const VoxelLight *dapples)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

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
    VoxelWorld_ScreenFade(&sWorldFade.amount, sWorldFade.rgb);
    sWorldFade = FadeThen(sWorldFade, sBrightBg, target);
    sSpriteFade = FadeThen(none, sBrightObj, target);
}

/* What the distance fades to this frame (VoxelLight.hazeRgb). */
static float sHazeRgb[3];
/* How far this frame's haze is the sky (the free-roam rig outdoors, else 0). */
static float sSkyShare;

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

    TerrainTexEnv(dapples);
    if (dapples != NULL)
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
    env = C3D_GetTexEnv(dapples != NULL ? 3 : 2);
    if (dapples != NULL)
        C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFFA0D8FFu);
}

/* The free-roam camera's horizon: the world's far edge is the haze (SetGrade),
 * so what is behind it is the haze's colour, not the game's backdrop. */
uint32_t CtrVoxel_SkyColour(uint32_t backdrop)
{
    float rgb[3];

    if (!sReady || sSkyShare <= 0.0f)
        return backdrop;
    for (int c = 0; c < 3; ++c)
    {
        float from = (float)((backdrop >> (8 * c)) & 255u) / 255.0f;

        rgb[c] = from + (sHazeRgb[c] - from) * sSkyShare;
    }
    return FadeColour(1.0f, rgb);
}

/* Whether the free-roam camera is what the world is drawn from. */
bool CtrVoxel_FreeLook(void)
{
    return sReady && sCamera.rig > 0.5f;
}

/* Outdoors and under the ground; a building keeps the game's own sprites. */
bool CtrVoxel_DrawsFog(void)
{
    return sReady && sOwnsFog;
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
static void DrawFogBanks(const C3D_Mtx *view, const VoxelLight *light, bool cave, float density)
{
    double seconds = (double)svcGetSystemTick() / SYSCLOCK_ARM11;
    VoxelLight graded = *light;
    uint32_t colour = cave ? 0xFFBFAB9Eu : 0xFFF2EBE6u; /* 0xAABBGGRR */
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
        float period = sFogBanks[bank].period;
        float dx = (float)fmod(seconds * sFogBanks[bank].driftX, period);
        float dz = (float)fmod(seconds * sFogBanks[bank].driftZ, period);
        float strength = sFogBanks[bank].strength * density;
        C3D_Mtx model;

        Mtx_Copy(&model, view);
        Mtx_Translate(&model, floorf(sCamera.targetX / period) * period + dx,
                      sCamera.ground + sFogBanks[bank].height,
                      floorf(sCamera.targetZ / period) * period + dz, true);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniModelView, &model);

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
    uint32_t stamp;
} StaticMesh;

static StaticMesh sStaticMeshes[VOXEL_STATIC_MESHES];
static bool sStaticFailLogged;

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
}

/* The cached mesh of (page model, lit), marked as drawn this frame. */
static const struct StaticMesh *StaticMeshFind(unsigned pageModel, bool lit)
{
    for (unsigned i = 0; i < VOXEL_STATIC_MESHES; ++i)
        if (sStaticMeshes[i].used && sStaticMeshes[i].pageModel == pageModel
         && sStaticMeshes[i].lit == lit)
        {
            sStaticMeshes[i].stamp = sFrame;
            return &sStaticMeshes[i];
        }
    return NULL;
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
static void StaticMeshesRelight(void)
{
    if (sStaticBuild.active && sStaticBuild.lit)
        StaticBuildEnd(false);
    for (unsigned index = 0; index < VOXEL_STATIC_MESHES; ++index)
        if (sStaticMeshes[index].used && sStaticMeshes[index].lit)
            StaticMeshFree(&sStaticMeshes[index]);
}
#endif

static bool StaticBuildStart(unsigned pageModel, bool lit)
{
    unsigned count;

    memset(&sStaticBuild, 0, sizeof(sStaticBuild));
    sStaticBuild.pageModel = pageModel;
    sStaticBuild.lit = lit;
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

/* The converted mesh into linear memory and a cache slot: a free one, else
 * the one drawn longest ago (never one drawn this frame). */
static const StaticMesh *StaticBuildFinish(void)
{
    unsigned slot = VOXEL_STATIC_MESHES, unique = sStaticBuild.unique, count = sStaticBuild.count;
    unsigned bytes = unique * sizeof(VoxelGpuVertex) + count * sizeof(uint16_t);
    StaticMesh *mesh;

    for (unsigned i = 0; i < VOXEL_STATIC_MESHES; ++i)
        if (!sStaticMeshes[i].used)
        {
            slot = i;
            break;
        }
    if (slot == VOXEL_STATIC_MESHES)
    {
        for (unsigned i = 0; i < VOXEL_STATIC_MESHES; ++i)
            if (sStaticMeshes[i].stamp != sFrame
             && (slot == VOXEL_STATIC_MESHES || sStaticMeshes[i].stamp < sStaticMeshes[slot].stamp))
                slot = i;
        if (slot == VOXEL_STATIC_MESHES)
            return NULL; /* every one on screen: kept converted, tried again */
        StaticMeshFree(&sStaticMeshes[slot]);
    }
    mesh = &sStaticMeshes[slot];
    mesh->vertices = linearAlloc(bytes);
    if (mesh->vertices == NULL)
    {
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
    mesh->indices = (uint16_t *)(void *)(mesh->vertices + unique);
    memcpy(mesh->vertices, sStaticBuild.packed, unique * sizeof(VoxelGpuVertex));
    memcpy(mesh->indices, sStaticBuild.index, count * sizeof(uint16_t));
    GSPGPU_FlushDataCache(mesh->vertices, bytes);
    mesh->used = true;
    mesh->lit = sStaticBuild.lit;
    mesh->pageModel = sStaticBuild.pageModel;
    mesh->count = count;
    mesh->stamp = sFrame;
    CtrLog_Write(CTR_LOG_VIDEO, "VOXEL static model %u: %u vertices, %u indices (%u KiB linear)",
                 mesh->pageModel, unique, count, bytes >> 10);
    StaticBuildEnd(false);
    return mesh;
}

/*
 * A slice of work towards (page model, lit)'s mesh. True once it is cached,
 * or can never be (nothing more to wait for); false while it is still to
 * come - including while the converter is busy with another, which it
 * carries on with first.
 */
static bool StaticMeshPrepare(unsigned pageModel, bool lit, unsigned triangles)
{
    if (StaticMeshFind(pageModel, lit) != NULL || StaticBad(pageModel))
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

        if (StaticBuildFinish() == NULL && sStaticBuild.active)
            return false;
        return mine && (StaticMeshFind(pageModel, lit) != NULL || StaticBad(pageModel));
    }
    return false;
}

static void SetModelViewAt(const C3D_Mtx *view, float x, float y, float z)
{
    C3D_Mtx model;

    Mtx_Copy(&model, view);
    Mtx_Translate(&model, x, y, z, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniModelView, &model);
}

/* The static models the chunks on screen place, in the building pass's
 * state (alpha tested, the terrain's TexEnv and grade). */
static uint32_t sStaticDrawSliced = ~0u;

static void DrawStatics(const C3D_Mtx *view, const VoxelLight *dapples, bool glowing)
{
    for (unsigned i = 0; i < sDrawCount; ++i)
    {
        const VoxelChunk *chunk = sDraws[i].chunk;
        C3D_Tex *tex;

        if (chunk->staticCount == 0 || chunk->draft)
            continue;
        tex = BuildingPage(chunk->buildingPage);
        if (tex == NULL)
            continue;
        C3D_TexBind(0, tex);
        if (glowing)
        {
            BuildingPageSlot *page = FindPage(chunk->buildingPage);

            BuildingGlowEnv(GlowTex(page),
                            CtrDayNight_Get()->glow, dapples);
        }
        for (unsigned k = 0; k < chunk->staticCount; ++k)
        {
            const VoxelStaticDraw *s = &chunk->statics[k];
            const StaticMesh *mesh = StaticMeshFind(s->pageModel, s->lit);
            float x = (float)sDraws[i].worldX + s->x, z = (float)sDraws[i].worldZ + s->z;

            if (mesh == NULL)
            {
                /* given up by the cache since its chunk was built: a slice
                 * of it a frame until it is back */
                if (sStaticDrawSliced != sFrame)
                {
                    sStaticDrawSliced = sFrame;
                    StaticMeshPrepare(s->pageModel, s->lit, VOXEL_STATIC_SLICE_TRIANGLES);
                }
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

void CtrVoxel_Draw(C3D_RenderTarget *target, int eye)
{
    C3D_Mtx projection, view;
    C3D_AttrInfo *attr;
    C3D_TexEnv *env;
    const VoxelMapInstance *current = VoxelWorld_Instance(0);
    bool indoor = current != NULL && current->indoor;
    VoxelLight light = LightFor(indoor), unlit = LightFor(true);
    bool dapples = DapplesOn(&light);
#if CTR_VOXEL_LIGHTING
    float fog = !indoor && sHaveFog ? VoxelWorld_FogDensity() : 0.0f;
    bool cave = fog > 0.0f && VoxelWorld_Underground();
#endif

    sBloomStrength = 0.0f;

    sDrawEye = eye;
    sGloomAmount = 0.0f;
    sOwnsFog = false;
    if (!sReady || sDrawCount == 0)
        return;
    const CtrDayNightLight *cycle = CtrDayNight_Get();
    CtrDayNightBlend moon = CtrDayNight_Moonlight(cycle, 0.0f, cycle->rgb);

    sMoonlight = indoor ? 0.0f : moon.amount;
    sMoonlightColour = 0xFF000000u;
    for (unsigned channel = 0; channel < 3; ++channel)
        sMoonlightColour |= (uint32_t)(moon.rgb[channel] * sMoonlight * 127.5f + 0.5f)
                         << (channel * 8u);
#if CTR_VOXEL_LIGHTING
    sOwnsFog = !indoor && sHaveFog;
#endif

    sBloomStrength = light.bloom;
    C3D_FrameDrawOn(target);

    /* The camera the frustum was cut from in the update (UpdateFrustum).
     * The gloom is placed on the logical surface, before the fit to it. */
#if CTR_VOXEL_LIGHTING
    if (cave && sHaveGloom)
    {
        CameraMatrices(&projection, &view, false, eye);
        PlaceGloom(&projection, &view);
        sGloomAmount = VOXEL_GLOOM_MAX * fog;
    }
#endif
    CameraMatrices(&projection, &view, true, eye);

    C3D_BindProgram(&sProgram);

    /* VoxelGpuVertex: texcoord (2 floats), then position + shade (4 shorts). */
    attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);

    /* texture0 x graded colour (dappled), then the distance haze, then the fade. */
    PrepareFades();
    memcpy(sHazeRgb, light.hazeRgb, sizeof(sHazeRgb));
    sSkyShare = light.haze > 0.001f ? sCamera.rig : 0.0f;
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

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniProjection, &projection);
    SetGrade(&light);

    /*
     * Ordinary terrain first, then the tree material, then the modelled
     * buildings, all alpha tested: the transparent corners of the crowns and
     * the clear background of a cut tile's rock must write neither colour nor
     * depth. Each chunk keeps all three ranges in the same
     * static VRAM buffer, measured from its own corner, so every draw carries
     * that corner's place in the world in its model-view.
     */
    for (unsigned pass = 0; pass < VOXEL_ATLAS_PAGES + 2u; ++pass)
    {
        const C3D_Tex *boundTex = NULL;
        bool glowing = pass == VOXEL_ATLAS_PAGES + 1u && !indoor && sGlowWanted;

        C3D_AlphaTest(true, GPU_GREATER, 0);

        for (unsigned i = 0; i < sDrawCount; ++i)
        {
            const VoxelChunk *chunk = sDraws[i].chunk;
            unsigned first = pass < VOXEL_ATLAS_PAGES ? chunk->terrainFirst[pass]
                           : pass == VOXEL_ATLAS_PAGES ? chunk->terrainCount : chunk->buildingFirst;
            unsigned count = pass < VOXEL_ATLAS_PAGES ? chunk->terrainFirst[pass + 1u] - first
                           : pass == VOXEL_ATLAS_PAGES
                             ? (sDraws[i].turnedCrowns ? chunk->crownFirst : chunk->buildingFirst) - first
                           : chunk->count - first;
            C3D_Tex *tex;

            if (count == 0)
                continue;
            tex = pass < VOXEL_ATLAS_PAGES ? AtlasTex(chunk->atlas, pass)
                : pass == VOXEL_ATLAS_PAGES ? &sTreeAtlas : BuildingPage(chunk->buildingPage);
            if (tex == NULL)
                continue;

            SetModelView(&view, sDraws[i].worldX, sDraws[i].worldZ);
#if CTR_VOXEL_LIGHTING
            if (dapples)
                DappleUniforms(sDraws[i].worldX, sDraws[i].worldZ, 0.0f);
#endif
            if (boundTex != tex)
            {
                boundTex = tex;
                C3D_TexBind(0, tex);
                if (glowing)
                {
                    BuildingPageSlot *page = FindPage(chunk->buildingPage);

                    BuildingGlowEnv(GlowTex(page),
                                    CtrDayNight_Get()->glow, dapples ? &light : NULL);
                }
            }
            BindVertices(chunk->vram);
            C3D_DrawArrays(GPU_TRIANGLES, (int)first, (int)count);
        }
        /* The crowns turned to the free-roam rig's eye, where the chunks left
         * out their built ones (BuildCrowns). */
        if (pass == VOXEL_ATLAS_PAGES && sCrownVertices != 0 && sTreeAtlas.data != NULL)
        {
            SetModelView(&view, sDynamicX, sDynamicZ);
#if CTR_VOXEL_LIGHTING
            if (dapples)
                DappleUniforms(sDynamicX, sDynamicZ, 0.0f);
#endif
            C3D_TexBind(0, &sTreeAtlas);
            BindVertices(sCrownDyn);
            C3D_DrawArrays(GPU_TRIANGLES, 0, (int)sCrownVertices);
        }
        if (pass == VOXEL_ATLAS_PAGES + 1u && sHaveBuildings)
            DrawStatics(&view, dapples ? &light : NULL, glowing);
    }
    ReleaseGlowUnit();
    /* The glow over the dapples moved the haze and the fade a stage on
     * (BuildingGlowEnv): back where every later pass has them. */
    if (dapples)
    {
        FadeFor(false);
        C3D_TexEnvInit(C3D_GetTexEnv(5));
    }

    /* The reflections, cast shadows and the player's silhouette take no
     * dapples; the unit stays bound for the sprites and the rays, and is
     * released after them (ReleaseDappleUnit: never C3D_TexBind(1, NULL),
     * which reads address 0xC on the console). */
    TerrainTexEnv(NULL);

    /* Billboards and shadows: the linear buffer, around the camera's tile. */
    SetModelView(&view, sDynamicX, sDynamicZ);
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
        SetModelView(&view, sLampX, sLampZ);
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
        SetModelView(&view, sDynamicX, sDynamicZ);
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
    if (sShadowVertices != 0)
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
    if (sSpriteVertices != 0)
    {
        int playerFirst = VoxelEntities_PlayerVertexFirst();

        C3D_AlphaTest(true, GPU_GREATER, 0);
        C3D_TexBind(0, &sSpriteAtlas);
        if (playerFirst >= 0)
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
        C3D_DrawArrays(GPU_TRIANGLES, 0, sSpriteVertices);
    }
#if CTR_VOXEL_LIGHTING
    /* The fog's banks lie in the scene, so they go before the light in the
     * air (motes, rays) that is added over everything. */
    if (fog > 0.0f)
    {
        FadeFor(false);
        DrawFogBanks(&view, &light, cave, fog);
    }
#ifdef CTR_F8_LOAD
    {
        static uint32_t frames, phase = ~0u;
        static const unsigned motes[3] = {32, 64, 128};

        if (frames++ % 600 == 0)
        {
            if (phase != ~0u)
                CtrLog_Write(CTR_LOG_VIDEO, "F8 load done: motes=%u rays=x%u drawn in %lu/%lu frames", sMoteCount,
                             sRayLayers, (unsigned long)sF8Drawn[0], (unsigned long)sF8Drawn[1]);
            phase = (phase + 1) % 9;
            sMoteCount = motes[phase % 3];
            sRayLayers = 1 + phase / 3;
            sF8Drawn[0] = sF8Drawn[1] = 0;
            CtrLog_Write(CTR_LOG_VIDEO, "F8 load motes=%u rays=x%u from voxel frame %lu", sMoteCount, sRayLayers,
                         (unsigned long)frames);
        }
    }
#endif
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
#endif
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    /* The 2D compositor sets up stage 0 only. */
    for (int i = 1; i < 6; ++i)
        C3D_TexEnvInit(C3D_GetTexEnv(i));
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
 * text and weather are composed on the glass, in front of everything
 * (3ds_video.c). On an Old 3DS the second pass took the field under 60 fps,
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

    /* In the rig the eye is in the head: the plane of zero parallax is where
     * the rig puts it, a little way ahead (what is held in front of you
     * stands out of the picture, the rest behind it). */
    if (sCamera.rig > 0.0f && sCamera.focusDistance > 0.0f)
        focus += (sCamera.focusDistance - focus) * sCamera.rig;
    if (focus < NearPlane() * 2.0f)
        focus = NearPlane() * 2.0f;
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
