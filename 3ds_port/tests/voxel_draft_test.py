"""The chunk cache's index, the request table's hash and the draft of a square,
compiled out of ctr_voxel.c itself and run on the host with the GPU mocked.

- ChunkIndex/ChunkUnindex/FindChunk against a linear search, through random
  inserts, re-keys and releases, with the buckets forced to collide;
- RequestWait against the linear table it replaced, call for call, and the
  record a full request list hands over on a replacement;
- ChunkVram giving up nothing when no gap could be made;
- DraftChunk: the vertices a draft packs, their order by atlas page, the
  bounds, the upload bookkeeping, and every refusal leaving nothing behind.
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
#define VOXEL_POS_SCALE 512.0f
#define VOXEL_SHADE_SCALE 16384.0f
#define VOXEL_STAGING_VERTICES 12288u
#define VOXEL_CHUNK_UPLOADS_MAX 8u
#define VOXEL_REQUESTS_MAX 192u
#define VOXEL_DRAFT_CELLS (VOXEL_CHUNK * VOXEL_CHUNK)
#define VOXEL_STARVE_FRAMES 4u
#define VOXEL_DRAFT_STARVE_FRAMES 90u
#define CTR_LOG_ERROR 0
#define CTR_LOG_VIDEO 1
#define u32 uint32_t
typedef struct { float x, y, z, u, v, shade; } VoxelVertex;
typedef struct { float u, v; int16_t x, y, z, shade; } VoxelGpuVertex;
typedef struct { int32_t lo[3], hi[3]; } PackBounds;
typedef struct { uint16_t slotOf[2048]; } VoxelAtlasMap;
typedef struct { int mapGroup, mapNum, originX, originY, width, height;
    const void *primaryTileset, *secondaryTileset, *layout; } VoxelMapInstance;
typedef struct { const VoxelMapInstance *inst; bool border; int cx, cy, x0, y0, x1, y1, baseX, baseY; } ChunkSite;
typedef struct { bool valid; uint32_t generation, extension; bool extendPending; VoxelAtlasMap map; } VoxelAtlasSlot;
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
    int buildingPage;
    uint32_t stamp, viewStamp;
    int gx0, gz0, gx1, gz1; float gy0, gy1, buildMs;
    bool uncovered; uint32_t atlasExtension; bool draft;
    unsigned staticCount;
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
static unsigned long copiedBytes;
static Stats sStats U;
static unsigned sDrafts;
static unsigned sPackErrors; static float sPackWorst; static char sPackAxis;
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
    for signature in ['static int16_t Quantise(', 'static void Pack(', 'static void ReportPackErrors(',
                      'static void GrowBounds(', 'static void PackDraft(', 'static int PackedFloor(', 'static int PackedCeil(',
                      'static unsigned ChunkBucket(', 'static unsigned ChunkBucketOf(',
                      'static void ChunkUnindex(', 'static void ChunkIndex(', 'static void ReleaseChunk(',
                      'static VoxelChunk *FindChunk(', 'static bool Evictable(',
                      'static VoxelChunk *OldestChunk(', 'static VoxelChunk *FreeChunkSlot(',
                      'static bool ChunkRoomPossible(', 'static VoxelGpuVertex *ChunkVram(',
                      'static VoxelChunk *DraftChunk(']:
        if signature == 'static unsigned ChunkBucket(':
            source += 'static uint8_t sChunkHead[128];\nstatic uint8_t sChunkNext[VOXEL_CHUNK_SLOTS];\n' \
                      '#define VOXEL_CHUNK_BUCKETS 128u\n'
        source += function(voxel, signature)
    # The linear table RequestWait replaced, for the equivalence run.
    source += r'''
static struct { int group, map, cx, cy; bool border, used; uint32_t seen, progress, noRoomUntil; } sRequestWait[VOXEL_REQUESTS_MAX];
static struct { int group, map, cx, cy; bool border, used; uint32_t seen, progress; } sOld[VOXEL_REQUESTS_MAX];
static uint8_t sWaitHead[128], sWaitNext[VOXEL_REQUESTS_MAX];
'''
    source += function(voxel, 'static unsigned WaitBucket(')
    source += function(voxel, 'static unsigned RequestWait(')
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
    /* the same square again reuses its slot and its block, and draws none of
     * the static models a build placed in it */
    uploadAllowed = 8; chunk->staticCount = 2;
    again = DraftChunk(&site, &atlas, chunk);
    assert(again == chunk && FindChunk(false, 1, 2, 1, 2) == chunk && again->staticCount == 0);
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
        uploadAllowed = 8; sStagingUsed = 0; sChunkUploads = 0; copies = 0;
        assert(DraftChunk(&other, &atlas, NULL) == NULL);
        assert(FindChunk(false, 1, 2, 2, 2) == NULL && copies == 0 && uploadAllowed == 7);
        /* two off screen but apart (8-16 and 24-32 KiB, an on-screen chunk
         * between): no 12 KiB gap even with both gone, so neither is given up */
        assert((uint8_t *)sChunks[2].vram - sArenaBlock == 8 * 1024);
        assert((uint8_t *)sChunks[4].vram - sArenaBlock == 16 * 1024);
        assert((uint8_t *)sChunks[5].vram - sArenaBlock == 24 * 1024);
        sChunks[2].viewStamp = sFrame - 1; sChunks[2].stamp = sFrame - 6;
        sChunks[5].viewStamp = sFrame - 1; sChunks[5].stamp = sFrame - 7;
        assert(ChunkVram(12 * 1024, true) == NULL);
        assert(FindChunk(false, 9, 9, 2, 0) != NULL && FindChunk(false, 9, 9, 5, 0) != NULL);
        assert(sChunkArena.used == 6 * 8 * 1024);
        sChunks[2].viewStamp = sChunks[2].stamp = sFrame;
        sChunks[5].viewStamp = sChunks[5].stamp = sFrame;
        /* one of them off screen: it is the one given up */
        sChunks[3].viewStamp = sFrame - 1; sChunks[3].stamp = sFrame - 5;
        uploadAllowed = 8;
        chunk = DraftChunk(&other, &atlas, NULL);
        assert(chunk != NULL && chunk->draft && FindChunk(false, 1, 2, 2, 2) == chunk);
        assert(FindChunk(false, 9, 9, 3, 0) == NULL && FindChunk(false, 9, 9, 2, 0) != NULL);
    }
}
'''
    source += r'''
int main(void)
{
    TestIndex();
    TestWait();
    TestDraft();
    puts("PASS draft: chunk index vs linear search, request wait vs linear table and reuse, no futile eviction, draft vertices/pages/bounds/uploads/refusals");
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
