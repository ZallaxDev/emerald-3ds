"""The chunk cache's index, the request table's hash and the draft of a square,
compiled out of ctr_voxel.c itself and run on the host with the GPU mocked.

- ChunkIndex/ChunkUnindex/FindChunk against a linear search, through random
  inserts, re-keys and releases, with the buckets forced to collide;
- RequestWait against the linear table it replaced, call for call, and the
  record a full request list hands over on a replacement;
- ChunkVram giving up nothing when no gap could be made;
- DraftChunk: the vertices a draft packs, their order by atlas page, the
  bounds, the upload bookkeeping, and every refusal leaving nothing behind;
- every chunk block given back forgetting the mesh a build ahead of time found
  no room for (ChunkBlockFree), and nothing else forgetting it;
- the kept view walk (VisitRemember/VisitReplay/VisitStateSame) refused after
  any change to the slot index or to a field it keys of a chunk it found or of
  an atlas, and put back as it was kept otherwise.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def through(source, begin, signature):
    """The text from `begin` to the end of the function `signature` after it."""
    start = source.index(begin)
    return source[start:source.index(signature, start)] + function(source[start:], signature)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    voxel = (ROOT / 'src/voxel/ctr_voxel.c').read_text(encoding='utf-8')

    source = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voxel_arena.h"

#define U __attribute__((unused))
#define VOXEL_CHUNK 8
#define VOXEL_CHUNK_SLOTS 128u
#define VOXEL_ATLAS_PAGES 4u
#define VOXEL_ATLAS_MAX_SLOTS 512u
#define VOXEL_STAGING_VERTICES 12288u
#define VOXEL_CHUNK_UPLOADS_MAX 8u
#define VOXEL_REQUESTS_MAX 192u
#define VOXEL_DRAFT_CELLS (VOXEL_CHUNK * VOXEL_CHUNK)
#define VOXEL_STARVE_FRAMES 4u
#define VOXEL_DRAFT_STARVE_FRAMES 90u
#define VOXEL_ATLAS_SLOTS 6u
#define VOXEL_DRAW_MAX 160u
#define MAX_VOXEL_MAP_INSTANCES 8
#define CTR_LOG_ERROR 0
#define CTR_LOG_VIDEO 1
#define u32 uint32_t
typedef struct { float x, y, z, u, v, shade; } VoxelVertex;
/* This fixture supplies its own mesh builder. Use production packing with
 * the fixture's equivalent input vertex and the real GPU output layout. */
#define CTR_VOXEL_MESH_BUILDER_H
#include "voxel_vertex_pack.h"
typedef struct { uint16_t slotOf[2048]; } VoxelAtlasMap;
typedef struct { int mapGroup, mapNum, originX, originY, width, height;
    const void *primaryTileset, *secondaryTileset, *layout; } VoxelMapInstance;
typedef struct { const VoxelMapInstance *inst; bool border; int cx, cy, x0, y0, x1, y1, baseX, baseY; } ChunkSite;
typedef struct { bool valid; uint32_t generation, extension; bool extendPending; VoxelAtlasMap map;
    const void *primaryTileset, *secondaryTileset; } VoxelAtlasSlot;
typedef struct { float x, y, z, w; } VoxelPlane;
typedef struct { ChunkSite site; unsigned key, need, waitIndex; } BuildRequest;
#define VOXEL_PARTS 4u
enum { VOXEL_KIND_GROUND, VOXEL_KIND_TREES, VOXEL_KIND_BUILDINGS, VOXEL_KINDS };
#define VOXEL_RANGES (VOXEL_ATLAS_PAGES + 3u)
typedef struct { bool on; uint16_t end[VOXEL_RANGES][VOXEL_PARTS - 1u]; float box[VOXEL_KINDS][VOXEL_PARTS][6]; } VoxelChunkParts;
typedef struct {
    bool used, border;
    int mapGroup, mapNum, cx, cy;
    uint32_t beltGrid;
    unsigned openTiles; uint32_t openEpoch; bool openSame;
    uint32_t hash, epoch, staleHash, staleEpoch;
    const void *primary, *secondary, *layout;
    VoxelAtlasSlot *atlas; uint32_t atlasGeneration;
    VoxelGpuVertex *vram;
    unsigned bytes, count, terrainCount, terrainFirst[VOXEL_ATLAS_PAGES + 1u], buildingFirst;
    VoxelChunkParts parts;
    unsigned treeShadow, treeCrowns;
    int buildingPage;
    uint32_t stamp, viewStamp;
    int gx0, gz0, gx1, gz1; float gy0, gy1, buildMs;
    bool uncovered; uint32_t atlasExtension; bool draft;
    unsigned staticCount;
    bool uvKnown;
    unsigned audit;
} VoxelChunk;
typedef struct {
    VoxelVertex *vertices; unsigned capacity, count, dropped;
    float originX, originZ; const VoxelAtlasMap *atlas; unsigned uncovered;
    float lift, shift, base; bool lighting;
} VoxelBuilder;
typedef struct { unsigned errors; } Stats;

static VoxelChunk sChunks[VOXEL_CHUNK_SLOTS];
static uint32_t sBeltGrid = 1, sFrame = 100;
static VoxelArena sChunkArena;
static VoxelArenaPiece sChunkPieces[VOXEL_CHUNK_SLOTS + 1];
static uint8_t sArenaBlock[1024 * 1024];
static VoxelGpuVertex *sStaging;
static unsigned sStagingUsed, sChunkUploads, uploadAllowed, copies;
/* No cold mesh cache here: all the staging is the chunks', an eviction a release. */
#define STAGING_ROOM VOXEL_STAGING_VERTICES
static unsigned sStagingPromise;
static void ReleaseChunk(VoxelChunk *chunk);
static void EvictChunk(VoxelChunk *chunk) { ReleaseChunk(chunk); }
static unsigned long copiedBytes;
static Stats sStats U;
static unsigned sDrafts;
static VoxelPackStats sPackStats;
static int reported;
static void CtrLog_Write(int cat, const char *fmt, ...) { (void)cat; (void)fmt; ++reported; }
static void GSPGPU_FlushDataCache(const void *p, unsigned n) { (void)p; (void)n; }
static void C3D_SyncTextureCopy(u32 *src, u32 a, u32 *dst, u32 b, u32 bytes, u32 flags)
{ (void)a; (void)b; (void)flags; memcpy(dst, src, bytes); ++copies; copiedBytes += bytes; }
static bool CtrVideo_TryVoxelUpload(void) { if (uploadAllowed == 0) return false; --uploadAllowed; return true; }
static float VoxelRelief_Base(const VoxelMapInstance *inst) { (void)inst; return 0.0f; }
static void VoxelBuilder_Init(VoxelBuilder *b, VoxelVertex *s, unsigned cap)
{ memset(b, 0, sizeof(*b)); b->vertices = s; b->capacity = cap; }
static void VoxelBuilder_SetAtlas(VoxelBuilder *b, const VoxelAtlasMap *a) { b->atlas = a; }
static void VoxelBuilder_SetOrigin(VoxelBuilder *b, int x, int z) { b->originX = (float)x; b->originZ = (float)z; }
static void VoxelBuilder_SetStretch(VoxelBuilder *b, const VoxelMapInstance *inst) { (void)b; (void)inst; }

/* The world of the test: which cells draw what. id = hash of the cell. */
static int Cell_id(int x, int y) { return (int)(((unsigned)(x * 31 + y * 17 + 5) * 2654435761u) >> 24) % 40; }
static int VoxelMesh_DraftSlot(const VoxelMapInstance *inst, const VoxelAtlasMap *atlas,
                               int x, int y, unsigned *uncovered)
{
    unsigned slot = atlas->slotOf[Cell_id(x - inst->originX, y - inst->originY)];
    if (slot == 0 || slot == 0xFFFEu) { ++*uncovered; return -1; }
    if (slot == 0xFFFFu) return -1;
    return (int)slot - 1;
}
static void VoxelMesh_DraftCell(VoxelBuilder *b, const VoxelMapInstance *inst, int x, int y, int slot)
{
    float page = (float)(slot / 512), col = (float)(slot % 512);
    float lift = (float)((x + y) & 3) * 0.25f;
    static const float cx[6] = {0, 1, 1, 0, 1, 0}, cz[6] = {0, 0, 1, 0, 1, 1};
    (void)inst;
    for (int k = 0; k < 6; ++k)
        b->vertices[b->count++] = (VoxelVertex){(float)x + cx[k] - b->originX, lift,
                                                (float)y + cz[k] - b->originZ,
                                                page * 2.0f + col / 512.0f, 0.5f, 1.0f};
}
'''
    # The shadow-only casters' list as production declares it (ReleaseChunk
    # takes a chunk out of it; the kept walk keeps it).
    begin = voxel.index('#define VOXEL_SHADOW_DRAWS_MAX')
    end = voxel.index('static unsigned sShadowCount;', begin) + len('static unsigned sShadowCount;')
    source += voxel[begin:end] + '\n'
    for signature in ['static void Pack(', 'static void ReportPackErrors(', 'static void PackDraft(',
                      'static int PackedFloor(', 'static int PackedCeil(',
                      'static unsigned ChunkBucket(', 'static unsigned ChunkBucketOf(',
                      'static void ChunkUnindex(', 'static void ChunkIndex(', 'static void ChunkBlockFree(',
                      'static void ReleaseChunk(',
                      'static VoxelChunk *FindChunk(', 'static bool Evictable(',
                      'static VoxelChunk *OldestChunk(', 'static VoxelChunk *FreeChunkSlot(',
                      'static bool ChunkRoomPossible(', 'static VoxelGpuVertex *ChunkVram(',
                      'static VoxelChunk *DraftChunk(']:
        if signature == 'static unsigned ChunkBucket(':
            source += 'static uint8_t sChunkHead[128];\nstatic uint8_t sChunkNext[VOXEL_CHUNK_SLOTS];\n' \
                      'static uint32_t sChunkIndexChanges;\n#define VOXEL_CHUNK_BUCKETS 128u\n'
        if signature == 'static void ChunkBlockFree(':
            source += 'static unsigned sAheadNoRoomBytes;\n'
        source += function(voxel, signature)
    # The linear table RequestWait replaced, for the equivalence run.
    source += r'''
static struct { int group, map, cx, cy; bool border, used; uint32_t seen, progress, noRoomUntil; uint32_t fill, fillEpoch, fillExtension; } sRequestWait[VOXEL_REQUESTS_MAX];
static struct { int group, map, cx, cy; bool border, used; uint32_t seen, progress; } sOld[VOXEL_REQUESTS_MAX];
static uint8_t sWaitHead[128], sWaitNext[VOXEL_REQUESTS_MAX];
'''
    source += function(voxel, 'static unsigned WaitBucket(')
    source += function(voxel, 'static unsigned RequestWait(')
    # The kept view walk, as the visit keeps it and puts it back.
    source += r'''
typedef struct { const VoxelChunk *chunk; int worldX, worldZ; } VoxelChunkDraw;
static VoxelAtlasSlot sAtlases[VOXEL_ATLAS_SLOTS];
static BuildRequest sRequests[VOXEL_REQUESTS_MAX];
static unsigned sRequestCount, sDrawCount, sHashesLeft = 10;
static VoxelChunkDraw sDraws[VOXEL_DRAW_MAX];
static void Draw(VoxelChunk *chunk, int worldX, int worldZ)
{ sDraws[sDrawCount++] = (VoxelChunkDraw){chunk, worldX, worldZ}; }
/* The shadow-only casters' sweep, against a frustum of the test's own. */
static VoxelPlane sFrustum[4];
static bool sFrustumValid;
static struct { float targetX, targetZ; } sCamera;
static float DrawnBase(float base) { return base; }
static const VoxelMapInstance *VoxelWorld_Instance(unsigned i) { (void)i; return NULL; }
'''
    source += function(voxel, 'static bool BoxVisible(')
    source += through(voxel, 'static bool sShadowSweep;', 'static void ShadowCastersSeal(')
    source += through(voxel, '#define VOXEL_VISIT_KEEP_SITES', 'static void VisitRemember(')
    source += r'''
static unsigned RequestWaitOld(const ChunkSite *site)
{
    unsigned oldest = 0;
    for (unsigned i = 0; i < VOXEL_REQUESTS_MAX; ++i)
    {
        if (sOld[i].used && sOld[i].group == site->inst->mapGroup
         && sOld[i].map == site->inst->mapNum && sOld[i].border == site->border
         && sOld[i].cx == site->cx && sOld[i].cy == site->cy)
        {
            sOld[i].seen = sFrame;
            return i;
        }
        if (!sOld[i].used || sOld[i].seen < sOld[oldest].seen)
            oldest = i;
    }
    sOld[oldest].group = site->inst->mapGroup;
    sOld[oldest].map = site->inst->mapNum;
    sOld[oldest].cx = site->cx;
    sOld[oldest].cy = site->cy;
    sOld[oldest].border = site->border;
    sOld[oldest].used = true;
    sOld[oldest].seen = sOld[oldest].progress = sFrame;
    return oldest;
}

static VoxelChunk *Linear(bool border, int g, int m, int cx, int cy)
{
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i) {
        VoxelChunk *c = &sChunks[i];
        if (c->used && c->cx == cx && c->cy == cy && c->border == border
         && (border ? c->beltGrid == sBeltGrid : c->mapGroup == g && c->mapNum == m))
            return c;
    }
    return NULL;
}
static void Put(VoxelChunk *c, bool border, int g, int m, int cx, int cy, uint32_t grid)
{
    if (c->used) ChunkUnindex(c);
    c->used = true; c->border = border; c->mapGroup = g; c->mapNum = m;
    c->cx = cx; c->cy = cy; c->beltGrid = grid;
    ChunkIndex(c);
}
static unsigned rnd(void) { static uint32_t s = 12345; s = s * 1664525u + 1013904223u; return s >> 8; }

static void TestIndex(void)
{
    /* (slots with no block: released, they give nothing back to the arena,
     * and forget nothing - ChunkBlockFree) */
    sAheadNoRoomBytes = 4096;
    /* few distinct keys, so the buckets hold chains */
    for (unsigned step = 0; step < 60000; ++step) {
        unsigned op = rnd() % 10, slot = rnd() % VOXEL_CHUNK_SLOTS;
        if (op < 5) {
            bool border = rnd() % 4 == 0;
            Put(&sChunks[slot], border, (int)(rnd() % 3), (int)(rnd() % 3), (int)(rnd() % 7) - 2,
                (int)(rnd() % 7) - 2, 1 + rnd() % 3);
        } else if (op < 7) {
            if (sChunks[slot].used) { /* a slot released and reused */ }
            ReleaseChunk(&sChunks[slot]);
        } else if (op == 7) {
            sBeltGrid = 1 + rnd() % 3;     /* a cut starts a new grid */
        }
        for (unsigned probe = 0; probe < 6; ++probe) {
            bool border = rnd() % 3 == 0;
            int g = (int)(rnd() % 3), m = (int)(rnd() % 3), cx = (int)(rnd() % 7) - 2, cy = (int)(rnd() % 7) - 2;
            VoxelChunk *a = FindChunk(border, g, m, cx, cy), *b = Linear(border, g, m, cx, cy);
            /* with duplicates the linear scan answers the first slot, the chain the most recent: both are one of them */
            assert((a == NULL) == (b == NULL));
            if (a) assert(a->used && a->cx == cx && a->cy == cy && a->border == border
                       && (border ? a->beltGrid == sBeltGrid : a->mapGroup == g && a->mapNum == m));
        }
    }
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i) ReleaseChunk(&sChunks[i]);
    for (unsigned b = 0; b < 128; ++b) assert(sChunkHead[b] == 0);
    assert(sAheadNoRoomBytes == 4096);
    sBeltGrid = 1;
}

static void TestWait(void)
{
    VoxelMapInstance inst[3] = {{.mapGroup = 0, .mapNum = 1}, {.mapGroup = 0, .mapNum = 2}, {.mapGroup = 1, .mapNum = 0}};
    /* more distinct squares than entries: evictions, chains, reuse */
    for (unsigned step = 0; step < 200000; ++step) {
        ChunkSite site = {.inst = &inst[rnd() % 3], .border = rnd() % 5 == 0,
                          .cx = (int)(rnd() % 14), .cy = (int)(rnd() % 14)};
        unsigned a, b;
        sFrame += rnd() % 3 == 0;
        a = RequestWait(&site, -1);
        b = RequestWaitOld(&site);
        assert(a == b);
        assert(sRequestWait[a].cx == site.cx && sRequestWait[a].cy == site.cy
            && sRequestWait[a].border == site.border && sRequestWait[a].group == site.inst->mapGroup);
        assert(sRequestWait[a].seen == sOld[b].seen);
    }
    /* A replacement in a full list: a square not in the table takes the
     * displaced request's record, out of its old chain and into its own, and
     * starts afresh; a square in the table keeps its own. */
    {
        ChunkSite gone = {.inst = &inst[0], .cx = 3, .cy = 4}, fresh = {.inst = &inst[2], .cx = 40, .cy = 41};
        unsigned w, again;
        ++sFrame;
        w = RequestWait(&gone, -1);
        sRequestWait[w].noRoomUntil = sFrame + 60;
        sRequestWait[w].progress = sFrame - 50;
        assert(RequestWait(&fresh, (int)w) == w);
        assert(sRequestWait[w].cx == 40 && sRequestWait[w].cy == 41 && sRequestWait[w].group == 1);
        assert(sRequestWait[w].noRoomUntil == 0 && sRequestWait[w].progress == sFrame);
        assert(RequestWait(&fresh, -1) == w);
        again = RequestWait(&gone, -1);
        assert(again != w && sRequestWait[again].cx == 3 && sRequestWait[again].cy == 4);
        assert(RequestWait(&fresh, (int)again) == w && sRequestWait[again].cx == 3);
        for (unsigned b = 0; b < 128; ++b)
            for (unsigned link = sWaitHead[b]; link != 0; link = sWaitNext[link - 1u])
                assert(WaitBucket(sRequestWait[link - 1u].group, sRequestWait[link - 1u].map,
                                  sRequestWait[link - 1u].cx, sRequestWait[link - 1u].cy,
                                  sRequestWait[link - 1u].border) == b);
    }
}

static void TestDraft(void)
{
    static VoxelMapInstance inst = {.mapGroup = 1, .mapNum = 2, .originX = -16, .originY = 24, .width = 40, .height = 30};
    static VoxelAtlasSlot atlas = {.valid = true, .generation = 3, .extension = 5};
    int tilesetA, tilesetB, layoutX;
    ChunkSite site;
    VoxelChunk *chunk, *again;
    unsigned drawn = 0, uncovered = 0, perPage[4] = {0};
    inst.primaryTileset = &tilesetA; inst.secondaryTileset = &tilesetB; inst.layout = &layoutX;
    sStaging = calloc(VOXEL_STAGING_VERTICES, sizeof(*sStaging));
    VoxelArena_Init(&sChunkArena, sArenaBlock, sizeof(sArenaBlock), 16, sChunkPieces, VOXEL_CHUNK_SLOTS + 1);
    /* ids 0..39: some on every page, some unresolved */
    for (unsigned id = 0; id < 40; ++id) {
        unsigned page = id % 4;
        atlas.map.slotOf[id] = (uint16_t)(page * 512 + id + 1);
    }
    atlas.map.slotOf[3] = 0; atlas.map.slotOf[7] = 0xFFFEu; atlas.map.slotOf[11] = 0xFFFFu;
    site = (ChunkSite){.inst = &inst, .cx = 1, .cy = 2, .x0 = -8, .y0 = 40, .x1 = 0, .y1 = 48, .baseX = -8, .baseY = 40};
    for (int y = 40; y < 48; ++y) for (int x = -8; x < 0; ++x) {
        int id = Cell_id(x - inst.originX, y - inst.originY);
        unsigned s = atlas.map.slotOf[id];
        if (s == 0 || s == 0xFFFEu) ++uncovered;
        else if (s != 0xFFFFu) { ++drawn; ++perPage[(s - 1) / 512]; }
    }
    assert(drawn > 20 && uncovered > 0);
    uploadAllowed = 8; sStagingUsed = 0; sChunkUploads = 0; copies = 0; copiedBytes = 0;
    chunk = DraftChunk(&site, &atlas, NULL);
    assert(chunk != NULL && chunk->used && chunk->draft && chunk->count == drawn * 6);
    assert(chunk->terrainCount == chunk->count && chunk->buildingFirst == chunk->count && chunk->buildingPage == -1);
    assert(chunk->hash == 0 && chunk->epoch == 0 && chunk->atlas == &atlas && chunk->uncovered);
    assert(chunk->atlasGeneration == 3 && chunk->atlasExtension == 5 && atlas.extendPending);
    assert(FindChunk(false, 1, 2, 1, 2) == chunk);
    assert(copies == 1 && copiedBytes == chunk->count * sizeof(VoxelGpuVertex) && sChunkUploads == 1);
    assert(sStagingUsed == chunk->count && chunk->bytes >= chunk->count * sizeof(VoxelGpuVertex));
    /* by page, the counts the cells give, and U stripped of its page */
    for (unsigned p = 0; p < VOXEL_ATLAS_PAGES; ++p) {
        assert(chunk->terrainFirst[p + 1] - chunk->terrainFirst[p] == perPage[p] * 6);
        for (unsigned v = chunk->terrainFirst[p]; v < chunk->terrainFirst[p + 1]; ++v) {
            const VoxelGpuVertex *g = &chunk->vram[v];
            assert(g->u >= 0.0f && g->u < 1.0f);
            assert(g->shade == 16384);
        }
    }
    assert(chunk->terrainFirst[VOXEL_ATLAS_PAGES] == chunk->count);
    /* positions: relative to the chunk's corner, in 1/512 tile */
    for (unsigned v = 0; v < chunk->count; ++v) {
        const VoxelGpuVertex *g = &chunk->vram[v];
        assert(g->x >= 0 && g->x <= 8 * 512 && g->z >= 0 && g->z <= 8 * 512);
        assert(g->y >= 0 && g->y <= 3 * 128 + 1);
    }
    assert(chunk->gx0 == 0 && chunk->gz0 == 0 && chunk->gx1 == 8 && chunk->gz1 == 8);
    assert(chunk->gy0 == 0.0f && chunk->gy1 > 0.0f && chunk->gy1 <= 0.75f + 1e-6f);
    /* The same square reuses its slot and block but clears the previous
     * build's static models, tree ranges and terrain partition flag. */
    uploadAllowed = 8; chunk->staticCount = 2; sAheadNoRoomBytes = 4096;
    chunk->treeShadow = 12; chunk->treeCrowns = 6; chunk->parts.on = true;
    again = DraftChunk(&site, &atlas, chunk);
    assert(again == chunk && FindChunk(false, 1, 2, 1, 2) == chunk && again->staticCount == 0);
    assert(again->treeShadow == 0 && again->treeCrowns == 0 && !again->parts.on);
    assert(sAheadNoRoomBytes == 4096); /* (no block given back) */
    /* a block too small for the draft: a new one, the old given back, and
     * with it the room a build ahead of time found none of is asked again */
    {
        VoxelGpuVertex *old = chunk->vram;
        uploadAllowed = 8; chunk->bytes = 16;
        again = DraftChunk(&site, &atlas, chunk);
        assert(again == chunk && chunk->vram != old && chunk->bytes >= chunk->count * sizeof(VoxelGpuVertex));
        assert(sAheadNoRoomBytes == 0 && VoxelArena_PieceOf(&sChunkArena, old) < 0);
    }
    {
        unsigned used = 0; for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i) used += sChunks[i].used;
        assert(used == 1);
    }
    /* refusals leave nothing behind */
    {
        ChunkSite other = site; other.cx = 2; other.x0 = 0; other.x1 = 8; other.baseX = 0;
        unsigned used = 0, usedBefore = sStagingUsed, upBefore = sChunkUploads;
        uploadAllowed = 0;
        assert(DraftChunk(&other, &atlas, NULL) == NULL);
        uploadAllowed = 8; sChunkUploads = VOXEL_CHUNK_UPLOADS_MAX;
        assert(DraftChunk(&other, &atlas, NULL) == NULL);
        sChunkUploads = upBefore; sStagingUsed = VOXEL_STAGING_VERTICES - 3;
        assert(DraftChunk(&other, &atlas, NULL) == NULL);
        sStagingUsed = usedBefore;
        for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i) used += sChunks[i].used;
        assert(used == 1 && FindChunk(false, 1, 2, 2, 2) == NULL && uploadAllowed == 8);
        /* VRAM full of chunks on screen this frame: nothing to evict, nothing taken */
        VoxelArena_Init(&sChunkArena, sArenaBlock, 48 * 1024, 16, sChunkPieces, VOXEL_CHUNK_SLOTS + 1);
        for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i) memset(&sChunks[i], 0, sizeof(sChunks[i]));
        memset(sChunkHead, 0, sizeof(sChunkHead)); memset(sChunkNext, 0, sizeof(sChunkNext));
        for (unsigned i = 0; i < 6; ++i) {
            sChunks[i].vram = VoxelArena_Alloc(&sChunkArena, 8 * 1024, i & 1);
            assert(sChunks[i].vram != NULL); sChunks[i].bytes = 8 * 1024;
            Put(&sChunks[i], false, 9, 9, (int)i, 0, 1);
            sChunks[i].viewStamp = sFrame; sChunks[i].stamp = sFrame;
        }
        assert(VoxelArena_Alloc(&sChunkArena, 6 * 1024, false) == NULL);
        uploadAllowed = 8; sStagingUsed = 0; sChunkUploads = 0; copies = 0; sAheadNoRoomBytes = 4096;
        assert(DraftChunk(&other, &atlas, NULL) == NULL);
        assert(FindChunk(false, 1, 2, 2, 2) == NULL && copies == 0 && uploadAllowed == 7);
        assert(sAheadNoRoomBytes == 4096); /* nothing given back, nothing forgotten */
        /* two off screen but apart (8-16 and 24-32 KiB, an on-screen chunk
         * between): no 12 KiB gap even with both gone, so neither is given up */
        assert((uint8_t *)sChunks[2].vram - sArenaBlock == 8 * 1024);
        assert((uint8_t *)sChunks[4].vram - sArenaBlock == 16 * 1024);
        assert((uint8_t *)sChunks[5].vram - sArenaBlock == 24 * 1024);
        sChunks[2].viewStamp = sFrame - 1; sChunks[2].stamp = sFrame - 6;
        sChunks[5].viewStamp = sFrame - 1; sChunks[5].stamp = sFrame - 7;
        assert(ChunkVram(12 * 1024, true) == NULL);
        assert(FindChunk(false, 9, 9, 2, 0) != NULL && FindChunk(false, 9, 9, 5, 0) != NULL);
        assert(sChunkArena.used == 6 * 8 * 1024 && sAheadNoRoomBytes == 4096);
        sChunks[2].viewStamp = sChunks[2].stamp = sFrame;
        sChunks[5].viewStamp = sChunks[5].stamp = sFrame;
        /* one of them off screen: it is the one given up */
        sChunks[3].viewStamp = sFrame - 1; sChunks[3].stamp = sFrame - 5;
        uploadAllowed = 8;
        chunk = DraftChunk(&other, &atlas, NULL);
        assert(chunk != NULL && chunk->draft && FindChunk(false, 1, 2, 2, 2) == chunk);
        assert(FindChunk(false, 9, 9, 3, 0) == NULL && FindChunk(false, 9, 9, 2, 0) != NULL);
        /* its block given back (ReleaseChunk): asked again ahead of time */
        assert(sAheadNoRoomBytes == 0);
    }
}
'''
    source += r'''
/* One walk of the view as VisitView makes it, then kept if it may be: chunks
 * 0 and 1 found (1 on screen and drawn), chunk 2 in the cache but not on the
 * walk's way, one request. */
static VoxelMapInstance sWalkInst = {.mapGroup = 3, .mapNum = 4};
static void Walk(const VisitKey *key)
{
    ChunkSite site = {.inst = &sWalkInst, .cx = 5, .x0 = 40, .y0 = 0, .x1 = 48, .y1 = 8};
    sDrawCount = sRequestCount = 0;
    sChunks[0].stamp = sChunks[1].stamp = sFrame;
    sChunks[1].viewStamp = sFrame;
    Draw(&sChunks[1], 10, 20);
    /* chunk 0 off screen, its shadow on it */
    sShadowCount = 1; sShadowNeeds = 1;
    sShadowDraws[0] = (VoxelShadowDraw){.chunk = &sChunks[0], .worldX = 2, .worldZ = -8, .terrain = true, .far = 3.0f};
    sRequests[sRequestCount++] = (BuildRequest){.site = site, .need = 2, .waitIndex = RequestWait(&site, -1)};
    VisitRemember(key, 0, 1);
}

/* A field of a chunk the walk found: changed, the walk is made afresh; as it
 * was, it is kept again. */
#define KEYED(field, value) do { VoxelChunk saved = *c; c->field = (value); \
    assert(!VisitStateSame()); *c = saved; assert(VisitStateSame()); ++keyed; } while (0)

static void TestVisitMemo(void)
{
    int layoutA, tileA, tileB;
    VisitKey key, other;
    VoxelChunk *c;
    unsigned missing = 0, keyed = 0;

    (void)sVisitLastSites; (void)sVisitsSmall; (void)sVisitTicks;
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i) ReleaseChunk(&sChunks[i]);
    for (unsigned a = 0; a < VOXEL_ATLAS_SLOTS; ++a)
        sAtlases[a] = (VoxelAtlasSlot){.valid = a < 3, .generation = a, .extension = 2 * a,
                                       .primaryTileset = &tileA, .secondaryTileset = &tileB};
    for (unsigned i = 0; i < 3; ++i) {
        Put(&sChunks[i], false, 3, 4, (int)i, 0, 1);
        sChunks[i].hash = 100 + i; sChunks[i].epoch = 7; sChunks[i].layout = &layoutA;
        sChunks[i].primary = &tileA; sChunks[i].secondary = &tileB;
        sChunks[i].atlas = &sAtlases[1]; sChunks[i].atlasGeneration = 1; sChunks[i].count = 600;
        sChunks[i].gx1 = sChunks[i].gz1 = 8; sChunks[i].gy1 = 1.5f;
    }
    memset(&key, 0, sizeof(key));
    key.epoch = 7; key.instances = 1; key.atlases[0] = &sAtlases[1]; key.inRing[0] = 1;
    memcpy(&other, &key, sizeof(key)); other.view[2] = 1;
    /* Kept from the second of two walks of the same view, frames apart by one. */
    sFrame = 500; Walk(&key); assert(!sVisitMemo.valid);
    sFrame = 501; Walk(&key); assert(sVisitMemo.valid && sVisitMemo.foundCount == 2);
    sFrame = 502;
    assert(VisitStateSame());
    /* Another view, a frame missed, or nothing kept: walked afresh. */
    sDrawCount = sRequestCount = 0;
    assert(!VisitReplay(&other, 0.0f, 0.0f, &missing));
    sFrame = 503; assert(!VisitReplay(&key, 0.0f, 0.0f, &missing)); sFrame = 502;
    /* Another sun, or the sweep off: the shadow-only casters change, walked afresh. */
    memcpy(&other, &key, sizeof(key)); other.shadowSweepZ = 1.25f;
    assert(!VisitReplay(&other, 0.0f, 0.0f, &missing));
    memcpy(&other, &key, sizeof(key)); other.shadowSweep = 1;
    assert(!VisitReplay(&other, 0.0f, 0.0f, &missing));
    sVisitMemo.valid = false; assert(!VisitReplay(&key, 0.0f, 0.0f, &missing)); sVisitMemo.valid = true;
    /* Every field it keys of a chunk it found, the one on screen and not. */
    for (unsigned k = 0; k < 2; ++k) {
        c = &sChunks[k];
        KEYED(used, !saved.used); KEYED(border, !saved.border); KEYED(openSame, !saved.openSame);
        KEYED(uncovered, !saved.uncovered); KEYED(draft, !saved.draft);
        KEYED(mapGroup, saved.mapGroup + 1); KEYED(mapNum, saved.mapNum + 1);
        KEYED(cx, saved.cx + 1); KEYED(cy, saved.cy + 1); KEYED(beltGrid, saved.beltGrid + 1);
        KEYED(openTiles, saved.openTiles + 1); KEYED(openEpoch, saved.openEpoch + 1);
        KEYED(hash, saved.hash + 1); KEYED(epoch, saved.epoch + 1);
        KEYED(staleHash, saved.staleHash + 1); KEYED(staleEpoch, saved.staleEpoch + 1);
        KEYED(primary, (const void *)&saved); KEYED(secondary, (const void *)&saved);
        KEYED(layout, (const void *)&saved); KEYED(atlas, &sAtlases[2]);
        KEYED(atlasGeneration, saved.atlasGeneration + 1); KEYED(atlasExtension, saved.atlasExtension + 1);
        KEYED(count, saved.count + 1);
        KEYED(gx0, saved.gx0 + 1); KEYED(gz0, saved.gz0 + 1); KEYED(gx1, saved.gx1 + 1); KEYED(gz1, saved.gz1 + 1);
        KEYED(gy0, saved.gy0 + 0.5f); KEYED(gy1, saved.gy1 + 0.5f);
    }
    assert(keyed == 2 * 29);
    /* Every atlas, in view or not. */
    for (unsigned a = 0; a < VOXEL_ATLAS_SLOTS; ++a) {
        VoxelAtlasSlot saved = sAtlases[a];
        sAtlases[a].valid = !saved.valid; assert(!VisitStateSame()); sAtlases[a] = saved;
        sAtlases[a].generation++; assert(!VisitStateSame()); sAtlases[a] = saved;
        sAtlases[a].extension++; assert(!VisitStateSame()); sAtlases[a] = saved;
        sAtlases[a].primaryTileset = &layoutA; assert(!VisitStateSame()); sAtlases[a] = saved;
        sAtlases[a].secondaryTileset = &layoutA; assert(!VisitStateSame()); sAtlases[a] = saved;
        assert(VisitStateSame());
    }
    /* A chunk the walk did not find is not compared: only through the index
     * could the next walk find it, and the index counts its changes. */
    sChunks[2].hash = 999; assert(VisitStateSame());
    /* Nothing changed: put back as it was kept, the stamps written, the
     * requests keyed from where the player now is. */
    sDrawCount = sRequestCount = 0; missing = 0;
    sShadowCount = sShadowNeeds = 0;
    assert(VisitReplay(&key, 44.0f, 1.0f, &missing));
    assert(missing == 1 && sDrawCount == 1 && sDraws[0].chunk == &sChunks[1]);
    /* the shadow-only casters as the walk listed them */
    assert(sShadowCount == 1 && sShadowNeeds == 1 && sShadowDraws[0].chunk == &sChunks[0]);
    assert(sShadowDraws[0].worldX == 2 && sShadowDraws[0].worldZ == -8 && sShadowDraws[0].terrain);
    assert(sDraws[0].worldX == 10 && sDraws[0].worldZ == 20);
    assert(sRequestCount == 1 && sRequests[0].key == 2u * 100000u + 9u);
    assert(sRequestWait[sRequests[0].waitIndex].seen == sFrame);
    assert(sChunks[0].stamp == sFrame && sChunks[1].stamp == sFrame && sChunks[1].viewStamp == sFrame);
    assert(sChunks[0].viewStamp != sFrame && sChunks[2].stamp != sFrame && sVisitsKept == 1);
    /* The slot index changed - a chunk entered or left, found or not -
     * the walk is made afresh. */
    sFrame = 503;
    assert(VisitStateSame());
    Put(&sChunks[2], false, 3, 4, 2, 0, 1); /* (out and in again, as it was) */
    assert(!VisitStateSame());
    sDrawCount = sRequestCount = 0; assert(!VisitReplay(&key, 0.0f, 0.0f, &missing));
    Walk(&key); assert(sVisitMemo.valid);
    sFrame = 504; assert(VisitStateSame());
    assert(sShadowCount == 1 && sShadowDraws[0].chunk == &sChunks[0]);
    ReleaseChunk(&sChunks[0]); assert(!VisitStateSame());
    assert(sShadowCount == 0); /* a slot released leaves the shadow-only casters */
    sDrawCount = sRequestCount = 0; assert(!VisitReplay(&key, 0.0f, 0.0f, &missing));
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i) ReleaseChunk(&sChunks[i]);
}

/*
 * HIGH's shadow-only casters: a square off screen whose shadow falls on it.
 * The frustum here: x 0..20, z up to 15, and a north edge z - y/2 >= 0 that
 * a box's top leaves first, as the camera's top edge does.
 */
static VoxelChunk *Tall(unsigned slot, float top)
{
    VoxelChunk *c = &sChunks[slot];
    memset(c, 0, sizeof(*c));
    c->count = 600; c->buildingFirst = 400; c->gx1 = c->gz1 = 8; c->gy1 = top;
    return c;
}

static void TestShadowCasters(void)
{
    static VoxelMapInstance inst = {.mapGroup = 7, .mapNum = 1};
    ChunkSite north = {.inst = &inst, .x0 = 4, .y0 = -9, .x1 = 12, .y1 = -1, .baseX = 4, .baseY = -9};
    ChunkSite east = {.inst = &inst, .x0 = 21, .y0 = 2, .x1 = 29, .y1 = 10, .baseX = 21, .baseY = 2};
    ChunkSite far = {.inst = &inst, .x0 = 4, .y0 = -30, .x1 = 12, .y1 = -22, .baseX = 4, .baseY = -30};
    VoxelChunk *house;

    sFrustum[0] = (VoxelPlane){1.0f, 0.0f, 0.0f, 0.0f};
    sFrustum[1] = (VoxelPlane){-1.0f, 0.0f, 0.0f, 20.0f};
    sFrustum[2] = (VoxelPlane){0.0f, -0.5f, 1.0f, 0.0f};
    sFrustum[3] = (VoxelPlane){0.0f, 0.0f, -1.0f, 15.0f};
    sFrustumValid = true;
    sFrame = 900;
    /* (the lighting is not built here: the sweep stays off, the list empty) */
    sShadowCount = 3; sShadowNeeds = 2;
    ShadowSweepBegin();
    assert(!sShadowSweep && sShadowCount == 0 && sShadowNeeds == 0);

    /* A house four tiles tall just past the north edge: not in the picture,
     * its shadow (a tile south for each it falls) is; with the sun from the
     * south, or the sweep off, it is not; much further north it is not. */
    house = Tall(40, 4.0f);
    assert(!BoxVisible(4.0f, 0.0f, -9.0f, 12.0f, 4.0f, -1.0f));
    assert(!SiteShadowReaches(&north, house));
    sShadowSweep = true; sShadowSweepX = 0.0f; sShadowSweepZ = 1.0f;
    assert(SiteShadowReaches(&north, house));
    assert(!SiteShadowReaches(&far, Tall(41, 4.0f)));
    sShadowSweepZ = -1.0f;
    assert(!SiteShadowReaches(&north, house));
    /* Flat ground shades nothing, however near. */
    sShadowSweepZ = 1.0f;
    assert(!SiteShadowReaches(&north, Tall(42, 0.5f)));
    /* Past the east edge: only with the sun from the east, the shadow
     * falling west. */
    sShadowSweepZ = 0.0f; sShadowSweepX = -1.2f;
    assert(SiteShadowReaches(&east, Tall(43, 4.0f)));
    sShadowSweepX = 1.2f;
    assert(!SiteShadowReaches(&east, Tall(43, 4.0f)));
    /* Not built, or only its flat draft: up to a house's height. */
    sShadowSweepX = 0.0f; sShadowSweepZ = 1.0f;
    assert(SiteShadowReaches(&north, NULL));
    {
        VoxelChunk *draft = Tall(44, 0.2f);
        draft->draft = true;
        assert(SiteShadowReaches(&north, draft));
        sShadowSweepZ = -1.0f;
        assert(!SiteShadowReaches(&north, NULL) && !SiteShadowReaches(&north, draft));
        sShadowSweepZ = 1.0f;
    }

    /* Its ground casts only where it rises: parts' boxes (x0 y0 w0 x1 y1 w1). */
    house = Tall(40, 4.0f);
    house->parts.on = true;
    for (unsigned p = 0; p < VOXEL_PARTS; ++p) { house->parts.box[VOXEL_KIND_GROUND][p][0] = 1.0f; house->parts.box[VOXEL_KIND_GROUND][p][3] = 0.0f; }
    house->parts.box[VOXEL_KIND_GROUND][0][0] = 0.0f; house->parts.box[VOXEL_KIND_GROUND][0][1] = 0.0f; house->parts.box[VOXEL_KIND_GROUND][0][2] = 0.0f;
    house->parts.box[VOXEL_KIND_GROUND][0][3] = 8.0f; house->parts.box[VOXEL_KIND_GROUND][0][4] = 0.1f; house->parts.box[VOXEL_KIND_GROUND][0][5] = 8.0f;
    assert(!ShadowTerrainReaches(house, 4, -9));
    house->parts.box[VOXEL_KIND_GROUND][1][0] = 0.0f; house->parts.box[VOXEL_KIND_GROUND][1][1] = 0.0f; house->parts.box[VOXEL_KIND_GROUND][1][2] = 0.0f;
    house->parts.box[VOXEL_KIND_GROUND][1][3] = 8.0f; house->parts.box[VOXEL_KIND_GROUND][1][4] = 2.0f; house->parts.box[VOXEL_KIND_GROUND][1][5] = 0.0f;
    assert(ShadowTerrainReaches(house, 4, -9));
    house->parts.on = false; /* not cut in parts: its whole box */
    assert(ShadowTerrainReaches(house, 4, -9));

    /* The cap: the nearest to the camera's target kept, the farthest turned
     * away; nothing to cast (a draft, an empty chunk) never listed. */
    sCamera.targetX = 10.0f; sCamera.targetZ = 5.0f;
    sShadowCount = 0; sShadowOverflow = 0;
    {
        static const unsigned order[14] = {13, 0, 12, 1, 7, 2, 11, 3, 4, 10, 5, 6, 9, 8};
        for (unsigned n = 0; n < 14; ++n) {
            unsigned k = order[n];
            ShadowCaster(Tall(50 + k, 3.0f), 6, -10 - 8 * (int)k);
        }
        assert(sShadowCount == VOXEL_SHADOW_DRAWS_MAX && sShadowOverflow == 2);
        for (unsigned i = 0; i < sShadowCount; ++i) {
            unsigned k = (unsigned)(sShadowDraws[i].chunk - &sChunks[50]);
            assert(k < 12 && sShadowDraws[i].worldZ == -10 - 8 * (int)k && sShadowDraws[i].worldX == 6);
            /* (its ground: not cut in parts, by its whole box - near enough) */
            assert(sShadowDraws[i].terrain == (k == 0));
        }
        Tall(70, 3.0f)->draft = true;
        ShadowCaster(&sChunks[70], 6, -2);
        Tall(71, 3.0f)->count = 0;
        ShadowCaster(&sChunks[71], 6, -2);
        for (unsigned i = 0; i < sShadowCount; ++i)
            assert(sShadowDraws[i].chunk != &sChunks[70] && sShadowDraws[i].chunk != &sChunks[71]);
        /* a nearer one takes the farthest's place */
        ShadowCaster(Tall(72, 3.0f), 6, 2);
        assert(sShadowCount == VOXEL_SHADOW_DRAWS_MAX && sShadowOverflow == 3);
        {
            bool has72 = false, has11 = false;
            for (unsigned i = 0; i < sShadowCount; ++i) {
                has72 = has72 || sShadowDraws[i].chunk == &sChunks[72];
                has11 = has11 || sShadowDraws[i].chunk == &sChunks[61];
            }
            assert(has72 && !has11);
        }
    }

    /* Sealed: each kept from eviction this frame, as a drawn chunk is; given
     * up when the picture has holes and no room. */
    sShadowPeak = sShadowYielded = 0;
    ShadowCastersSeal(false);
    for (unsigned i = 0; i < sShadowCount; ++i)
        assert(sShadowDraws[i].chunk->viewStamp == sFrame && !Evictable(sShadowDraws[i].chunk, true));
    assert(sShadowPeak == VOXEL_SHADOW_DRAWS_MAX);
    ShadowCastersSeal(true);
    assert(sShadowCount == 0 && sShadowYielded == 1);

    sShadowSweep = false;
    sFrustumValid = false;
    for (unsigned i = 0; i < VOXEL_CHUNK_SLOTS; ++i) memset(&sChunks[i], 0, sizeof(sChunks[i]));
}

int main(void)
{
    TestIndex();
    TestWait();
    TestDraft();
    TestVisitMemo();
    TestShadowCasters();
    puts("PASS draft: chunk index vs linear search, request wait vs linear table and reuse, no futile eviction, draft vertices/pages/bounds/uploads/refusals, freed blocks forget the ahead no-room size, kept view walk refused on index or keyed field changes and replayed otherwise (shadow-only casters with it), shadow-only casters by their sun-swept box, ground only where it rises, cap keeps the nearest, sealed against eviction or given up for room");
    return 0;
}
'''
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='voxel-draft-', dir=ROOT / 'build') as temp:
        temp = Path(temp)
        path = temp / 'test.c'
        path.write_text(source)
        exe = temp / 'test.exe'
        subprocess.run([args.cc, '-std=gnu99', '-O2', '-Wall', '-Wextra', '-Werror', '-Isrc/voxel',
                        str(path), str(ROOT / 'src/voxel/voxel_arena.c'), '-lm', '-o', str(exe)],
                       check=True, cwd=ROOT)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
