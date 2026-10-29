/*
 * Metatile atlas for the voxel overworld.
 *
 * Packed 512x256 pages per tileset pair, with 504 metatiles and eight solid
 * slots per page. Pages are allocated lazily from a shared VRAM budget. The
 * id-to-slot table preserves assignments when a map needs more materials.
 * Composition matches the reference's two layers of four 8x8 subtiles;
 * see NOTICE.md for the reference renderer's MIT attribution.
 *
 * The destination is written in PICA200 texture order, so the caller hands in
 * the raw pixel buffer of a C3D_Tex and nothing here needs libctru.
 */
#ifndef CTR_VOXEL_ATLAS_H
#define CTR_VOXEL_ATLAS_H

#include <stdbool.h>
#include <stdint.h>

#include "voxel_world.h"

#define VOXEL_ATLAS_W       512u
#define VOXEL_ATLAS_H       256u
#define VOXEL_ATLAS_SLOT    16u
#define VOXEL_ATLAS_COLUMNS (VOXEL_ATLAS_W / VOXEL_ATLAS_SLOT)
#define VOXEL_ATLAS_ROWS    (VOXEL_ATLAS_H / VOXEL_ATLAS_SLOT)
#define VOXEL_ATLAS_MAX_SLOTS (VOXEL_ATLAS_COLUMNS * VOXEL_ATLAS_ROWS)
#define VOXEL_ATLAS_PIXELS  (VOXEL_ATLAS_W * VOXEL_ATLAS_H)

/*
 * The reference draws furniture legs, table sides and television bodies as
 * untextured `glColor3f` geometry. This shader always samples texture0, so
 * rather than add a per-vertex colour - eight more bytes on every vertex, in a
 * build already short of linear memory - the last few atlas slots hold those
 * flat colours. A "solid" face is then an ordinary textured face whose UVs
 * point at one of them.
 */
typedef enum
{
    VOXEL_SOLID_WOOD = 0,   /* table sides */
    VOXEL_SOLID_WOOD_DARK,  /* table legs */
    VOXEL_SOLID_GREY,       /* television stand */
    VOXEL_SOLID_BLACK,      /* television body */
    VOXEL_SOLID_SHEET,      /* bedding */
    VOXEL_SOLID_COUNT
} VoxelSolidColor;

#define VOXEL_SOLID_SLOTS 8u
#define VOXEL_SOLID_BASE  (VOXEL_ATLAS_MAX_SLOTS - VOXEL_SOLID_SLOTS)

/* Texture coordinates of one of the flat colours. */
void VoxelAtlas_SolidUV(VoxelSolidColor color, float *u0, float *v0, float *u1, float *v1);

/*
 * One entry per metatile id. 0 means this atlas has never seen the id, which
 * is the renderer's cue that a script placed something new and the atlas needs
 * rebuilding once. VOXEL_SLOT_ABSENT means it was seen and has no pixels.
 * VOXEL_SLOT_PENDING requests a retry; other entries are page*512 + slot + 1.
 */
#define VOXEL_METATILE_REAL 1024u
/*
 * After the real ids, the ground variants (voxel_building.h): a metatile less
 * the quarters of its upper layer a model stands for - the water round a rock
 * in the sea, without the rock. Id VOXEL_METATILE_REAL + i is variant i of
 * buildings.bin; it is composed, extended and animated like any other id.
 */
#define VOXEL_VARIANTS 160u
/*
 * Then the cut tiles (voxel_relief.h): a rock's tile with the ground drawn
 * behind it clear, for its relief to stand over the ground drawn flat. Id
 * VOXEL_CUT_FIRST + i is cut variant i of relief.bin.
 */
#define VOXEL_CUTS 512u
#define VOXEL_CUT_FIRST (VOXEL_METATILE_REAL + VOXEL_VARIANTS)
#define VOXEL_METATILE_IDS (VOXEL_CUT_FIRST + VOXEL_CUTS)
#define VOXEL_SLOT_ABSENT  0xFFFFu
#define VOXEL_SLOT_PENDING 0xFFFEu
#define VOXEL_ATLAS_PAGES ((VOXEL_METATILE_IDS + VOXEL_SOLID_BASE - 1u) / VOXEL_SOLID_BASE)

typedef struct VoxelAtlasMap
{
    uint16_t slotOf[VOXEL_METATILE_IDS];
    uint8_t tileMask[VOXEL_ATLAS_PAGES][128]; /* source tiles referenced per page */
    unsigned used;      /* slots actually composed */
    bool overflowed;    /* the map referenced more ids than the atlas holds */
} VoxelAtlasMap;

/*
 * Synchronous convenience for page zero only. Fills `dest` (VOXEL_ATLAS_PIXELS
 * uint16_t, RGBA5551, PICA order) and `map` with assignments for all pages.
 * Use the job API to compose every page. Returns false if nothing could be composed, in which case `dest` is
 * left cleared.
 *
 * With `extend`, `map` is an atlas already in use that has met ids it was not
 * built with: every id keeps its slot and is redrawn there, and only the new
 * ones take free slots - so nothing drawn from the old atlas goes stale.
 * If every requested material is already resolved, succeeds with `dest`
 * untouched: the caller retains the atlas pixels already in use.
 */
bool VoxelAtlas_Build(const VoxelMapInstance *inst, uint16_t *dest, VoxelAtlasMap *map,
                      bool extend);

/*
 * The same composition spread over several calls, so that no frame pays for
 * all of it. Begin marks the ids and clears `dest`; each Step decodes at most 512 bytes of one
 * tileset or composes up to `metatiles` metatiles, and returns true once the
 * current page is ready. Upload it, wait for completion, then call NextPage.
 * Publish `map` only with the last page (active == false), if `ok` is true. With `base`, the job extends
 * that table (see VoxelAtlas_Build) rather than starting an empty one.
 *
 * The tileset pair is read afresh at every step; the caller must not change
 * `dest` in between, and cancels the job if the pair stops being wanted.
 */
typedef struct
{
    bool active, extend, ok;
    const void *primary, *secondary;
    int mapGroup, mapNum;
    uint16_t *dest;
    VoxelAtlasMap map;
    uint8_t used[VOXEL_METATILE_IDS];
    uint8_t *primaryTiles, *secondaryTiles;
    bool primaryLoaded, secondaryLoaded;
    VoxelTileLoad tileLoad;
    unsigned phase, next, priority;
    unsigned added;     /* ids given a slot of their own by this job */
    unsigned page;      /* current staging page; slotOf encodes page*512+slot+1 */
    bool pageReady;     /* CPU must not touch dest until its GPU upload retires */
} VoxelAtlasJob;

/* False with job->ok means base already covers the request: no buffers/upload. */
bool VoxelAtlas_JobBegin(VoxelAtlasJob *job, const VoxelMapInstance *inst, uint16_t *dest,
                         const VoxelAtlasMap *base);
bool VoxelAtlas_JobStep(VoxelAtlasJob *job, unsigned metatiles);
void VoxelAtlas_JobCancel(VoxelAtlasJob *job);
unsigned VoxelAtlas_PageCount(const VoxelAtlasMap *map);
/* Call only after the preceding page's upload has completed. */
bool VoxelAtlas_JobNextPage(VoxelAtlasJob *job);
/* Recompose only slots whose source 8x8 tiles were changed in BG_VRAM.
 * dest contains the complete prior atlas and remains valid for unaffected ids. */
unsigned VoxelAtlas_RefreshAnimated(const VoxelMapInstance *inst, const VoxelAtlasMap *map,
                                    uint16_t *dest, const uint8_t *liveTiles,
                                    const uint8_t dirtyTiles[128]);
unsigned VoxelAtlas_RefreshAnimatedPage(const VoxelMapInstance *inst, const VoxelAtlasMap *map,
                                       unsigned page, uint16_t *dest, const uint8_t *liveTiles,
                                       const uint8_t dirtyTiles[128]);
/*
 * The same, also giving the rows of slots it wrote: bit r of `*rows` is the
 * slots 32r..32r+31 of the page (VOXEL_ATLAS_ROWS of them). A row of slots is
 * VOXEL_ATLAS_ROW_BYTES of the page's memory, contiguous (the PICA's 8x8 blocks
 * run along the page, so a row of 16-pixel slots is two block rows), which is
 * all of the page a changed animation frame needs uploading.
 */
#define VOXEL_ATLAS_ROW_BYTES (VOXEL_ATLAS_SLOT * VOXEL_ATLAS_W * 2u)
unsigned VoxelAtlas_RefreshAnimatedPageRows(const VoxelMapInstance *inst, const VoxelAtlasMap *map,
                                            unsigned page, uint16_t *dest, const uint8_t *liveTiles,
                                            const uint8_t dirtyTiles[128], unsigned *rows);

/* Does this metatile of the instance's tileset pair draw nothing but black -
 * an interior's outside? Known once the pair's atlas has been built. */
bool VoxelAtlas_IsVoid(const VoxelMapInstance *inst, int metatile);

/*
 * The animated tiles (water, flowers) redrawn in every atlas in view at once.
 * A pass takes the tiles the game wrote since the last one, finds the slots
 * that show them in each atlas (VoxelAnim_AddAtlas), composes them a block
 * of 8x8 texels at a time - a block is a quadrant's two layers, and the sea's
 * hundred-odd metatiles are a few dozen different blocks - against a time
 * budget over as many frames as it takes (VoxelAnim_Compose), and only then
 * is written out, run by run (VoxelAnim_Run, VoxelAnim_WriteRun): the slots
 * of a run lie side by side in one row of a page, so a run is one GPU copy.
 * Written slot by slot as they were composed, a few a frame, the sea of a
 * town took longer to redraw than the game took to move it on, its tiles
 * showed different frames along their edges, and the sea of the route beside
 * (another atlas) waited its turn behind it.
 *
 * Pair i is the atlas the caller numbers i. A pair that is not the current
 * map's (`shared`) is redrawn only where it is drawn from primary tiles alone
 * - the sea, the shore, the flowers - as BG VRAM holds the current map's
 * secondary tiles, not its own.
 */
#define VOXEL_ANIM_PAIRS 8u
#define VOXEL_ANIM_TARGETS 1024u
#define VOXEL_ANIM_BLOCKS 1024u
#define VOXEL_ANIM_HASH 2048u
/* The longest run: a row of a page. */
#define VOXEL_ANIM_RUN_MAX VOXEL_ATLAS_COLUMNS

typedef struct
{
    uint16_t slot;      /* within its page */
    uint16_t id;        /* atlas id */
    uint8_t pair, page;
    uint16_t block[4];  /* its quadrants among the pass's blocks */
} VoxelAnimTarget;

typedef struct
{
    uint16_t lower, upper;  /* the quadrant's entries; a cut slot's id in `lower` */
    uint8_t pair;           /* the pair it is composed from */
    uint8_t flags;
} VoxelAnimBlock;

typedef struct
{
    uint8_t work[128];
    const void *primary[VOXEL_ANIM_PAIRS], *secondary[VOXEL_ANIM_PAIRS];
    bool shared[VOXEL_ANIM_PAIRS];
    VoxelAnimTarget target[VOXEL_ANIM_TARGETS];
    unsigned targets;
    VoxelAnimBlock block[VOXEL_ANIM_BLOCKS];
    uint16_t pixels[VOXEL_ANIM_BLOCKS][64];
    uint16_t (*out)[64];    /* the composed blocks: `pixels`, or a kept state */
    uint16_t hash[VOXEL_ANIM_HASH];
    uint32_t key[2];        /* the written tiles' content the pass composes */
    int state;              /* the kept state it composes into, or -1 */
    bool kept;              /* taken from a kept state, not composed */
    unsigned blocks, composed;
    bool full;              /* slots left out: they wait for the next write */
} VoxelAnimPass;

/* A new pass over the tiles in `work` (bit t: tile t written). */
void VoxelAnim_Begin(VoxelAnimPass *pass, const uint8_t work[128]);
/* The last pass again, for the same tiles in the same atlases: its slots
 * and blocks kept, composed anew from the tiles as they are now - or taken
 * whole from a state composed before, when the tiles hold one again. */
void VoxelAnim_Again(VoxelAnimPass *pass);
/* The slots of every page of an atlas that show those tiles, as the live
 * tiles (BG VRAM) are now; false when the pass had no room for all of them.
 * Targets end sorted by page and slot within each atlas. */
bool VoxelAnim_AddAtlas(VoxelAnimPass *pass, unsigned pair, const VoxelMapInstance *tilesets,
                        bool shared, const VoxelAtlasMap *map, const uint8_t *liveTiles);
/* Composes blocks from the live tiles (BG VRAM) until `stillTime` says no;
 * true once all of the pass is composed. */
bool VoxelAnim_Compose(VoxelAnimPass *pass, const uint8_t *liveTiles,
                       bool (*stillTime)(void));
/* The run starting at target `first`: how many targets it holds. */
unsigned VoxelAnim_Run(const VoxelAnimPass *pass, unsigned first);
/*
 * Writes the run of `count` targets from `first` into `out`, laid out as the
 * page holds them: the top half of each of its slots, then the bottom halves -
 * `count` * 512 bytes, which go to the page in two lines of `count` * 256
 * bytes, VOXEL_ATLAS_W / 8 blocks apart (VoxelAnim_RunOffset).
 */
void VoxelAnim_WriteRun(const VoxelAnimPass *pass, unsigned first, unsigned count, uint16_t *out);
/* Byte offset in its page of the first line of the run starting at `first`. */
unsigned VoxelAnim_RunOffset(const VoxelAnimPass *pass, unsigned first);

/* Texture coordinates of a packed slot, in the PICA convention where v=1 is
 * the first row in memory. U includes page*2, stripped by the chunk packer. */
void VoxelAtlas_SlotUV(unsigned slot, float *u0, float *v0, float *u1, float *v1);

#endif
