"""Compile actual runtime functions in a host harness, without emulating PICA."""
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
    voxel = (ROOT / 'src/voxel/ctr_voxel.c').read_text()
    video = (ROOT / 'src/3ds_video.c').read_text()
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VOXEL_REQUESTS_MAX 192u
#define VOXEL_STARVE_FRAMES 4u
#define VOXEL_ATLAS_PAGES 4u
#define VOXEL_ATLAS_SLOTS 6u
#define VOXEL_ATLAS_W 512u
#define VOXEL_ATLAS_H 256u
#define GPU_RGBA5551 0
#define GPU_NEAREST 0
#define GPU_CLAMP_TO_EDGE 0
enum { NEED_HOLE, NEED_STALE, NEED_AHEAD, NEED_AHEAD_STALE };
typedef struct { int mapGroup, mapNum, originX, originY, width, height; const void *primaryTileset, *secondaryTileset; } VoxelMapInstance;
typedef struct { VoxelMapInstance *inst; int cx, cy, x0, y0, x1, y1; bool border; } ChunkSite;
typedef struct { void *data; } C3D_Tex;
typedef struct { bool valid; const void *primaryTileset, *secondaryTileset;
    C3D_Tex tex, extra[3]; unsigned generation, stamp; } VoxelAtlasSlot;
static VoxelAtlasSlot sAtlases[6];
static void AnimForget(VoxelAtlasSlot *slot) { (void)slot; }
static struct { VoxelAtlasSlot *slot; bool forView; } sAtlasJob;
static unsigned allocated;
static VoxelMapInstance maps[2];
static int sViewRect[4] = {0, 0, 32, 16};
static unsigned VoxelWorld_InstanceCount(void) { return 2; }
static const VoxelMapInstance *VoxelWorld_Instance(unsigned i) { return &maps[i]; }
static bool C3D_TexInitVRAM(C3D_Tex *tex, unsigned w, unsigned h, int fmt) {
    (void)w; (void)h; (void)fmt; if (allocated >= 6) return false;
    ++allocated; tex->data = &allocated; return true;
}
static void C3D_TexDelete(C3D_Tex *tex) { assert(tex->data && allocated); --allocated; tex->data = NULL; }
static void C3D_TexSetFilter(C3D_Tex *t, int a, int b) { (void)t; (void)a; (void)b; }
static void C3D_TexSetWrap(C3D_Tex *t, int a, int b) { (void)t; (void)a; (void)b; }
/* No building pages' arena here: nothing is borrowed, every page is VRAM. */
static void AtlasPageRelease(C3D_Tex *tex) { if (tex->data) C3D_TexDelete(tex); }
static bool AtlasPageBorrow(C3D_Tex *tex) { (void)tex; return false; }
typedef enum { VOXEL_TREE_TOPS, VOXEL_TREE_CROWNS } VoxelTreePass;
typedef struct { bool draft; } VoxelChunk;
typedef struct { ChunkSite site; VoxelAtlasSlot *atlas; VoxelChunk *chunk; uint32_t hash;
    bool hashKnown, done; unsigned key, need, waitIndex; } BuildRequest;
static struct { int group, map, cx, cy; bool border, used; uint32_t seen, progress, noRoomUntil; } sRequestWait[VOXEL_REQUESTS_MAX];
static BuildRequest sRequests[VOXEL_REQUESTS_MAX];
static uint8_t sWaitHead[128], sWaitNext[VOXEL_REQUESTS_MAX];
#define VOXEL_DRAFT_STARVE_FRAMES 90u
static unsigned sRequestCount, sFrame, starts;
enum { JOB_GROUND, JOB_TREES, JOB_CROWNS, JOB_MODELS, JOB_BORDER, JOB_BORDER_TREES, JOB_BORDER_CROWNS, JOB_SORT, JOB_PACK, JOB_STATICS, JOB_DONE };
#define VOXEL_GROUND_SLICE_CELLS 1
#define VOXEL_MODEL_SLICE_TRIANGLES 64
#define VOXEL_STATIC_SLICE_TRIANGLES 64
#define VOXEL_SLICE_CAP_MS 5.0f
static float sPhaseMs[JOB_DONE + 1];
static uint32_t sPhaseFrame[JOB_DONE + 1];
static bool sHolesOnly = true, validJob = true;
static struct { bool active, forView, hole, overdue; unsigned waitIndex; ChunkSite site;
    int phase, row, col, models; unsigned buildingFirst, crownFirst;
    unsigned sortPage, sortScan, sortWrite, terrainCount, terrainFirst[5], staticIndex; } sJob;
typedef struct { float u, marker; } VoxelVertex;
static VoxelVertex sScratch[6000];
static bool JobValid(void) { return validJob; }
static void JobCancel(void) { sJob.active = false; }
static bool SameSite(const ChunkSite *a, const ChunkSite *b) { return a->cx == b->cx && a->cy == b->cy && a->inst == b->inst; }
static uint32_t SiteHash(const ChunkSite *site) { (void)site; return 10; }
static void JobStart(const ChunkSite *s, VoxelAtlasSlot *a, uint32_t h, void *c, bool view, bool hole) {
    (void)a; (void)h; (void)c; ++starts; memset(&sJob, 0, sizeof(sJob));
    sJob.active = true; sJob.site = *s; sJob.forView = view; sJob.hole = hole;
}
typedef struct { unsigned maxEntries, numEntries; } gxCmdQueue_s;
static gxCmdQueue_s *sFrameQueue;
static unsigned sUploadCommands, sRenderReserve;
static bool sPrefetchBlocked;
'''
    for signature in ['static unsigned WaitBucket(', 'static unsigned RequestWait(', 'static bool RequestOverdue(', 'static int CompareRequests(', 'static bool StartNextJob(', 'static bool JobSort(', 'static float PhaseCost(', 'static float PhaseCostInFrame(', 'static void NotePhaseCost(']:
        source += function(voxel, signature)
    for signature in ['static C3D_Tex *AtlasTex(', 'static void FreeAtlasTextures(', 'static unsigned CountAllocatedAtlases(', 'static bool AtlasInView(const VoxelAtlasSlot *slot)\n{', 'static bool AllocateAtlasPage(']:
        source += function(voxel, signature)
    source += r'''
static struct { unsigned count, staticCount; } sBuilder;
static bool sHaveBuildings;
/* Static models: each mesh cached once its slices of conversion are done. */
typedef struct { unsigned pageModel; bool lit; } VoxelStaticDraw;
static VoxelStaticDraw sJobStatics[4];
static struct StaticMesh { unsigned slicesLeft; } meshes[4];
static unsigned prepares;
static const struct StaticMesh *StaticMeshFind(unsigned pageModel, bool lit) {
    (void)lit; return meshes[pageModel].slicesLeft ? NULL : &meshes[pageModel];
}
static bool StaticMeshPrepare(unsigned pageModel, bool lit, unsigned triangles) {
    (void)lit; assert(triangles == VOXEL_STATIC_SLICE_TRIANGLES); ++prepares;
    if (meshes[pageModel].slicesLeft) --meshes[pageModel].slicesLeft;
    return meshes[pageModel].slicesLeft == 0;
}
static unsigned cells[2][64];
static void EmitCells(bool trees, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        assert(x >= 0 && x < 8 && y >= 0 && y < 8);
        ++cells[trees][y*8+x]; sBuilder.count += trees ? 12 : 6;
    }
}
#define VoxelMesh_EmitGroundRow(b, inst, x0, x1, y) ((void)(b), (void)(inst), EmitCells(false, x0, y, x1, (y)+1))
#define VoxelTree_EmitInstance(b, inst, x0, y0, x1, y1, pass) do { (void)(b); (void)(inst); (void)(pass); EmitCells(true, x0, y0, x1, y1); } while(0)
#define VoxelMesh_EmitBorder(b, x0, y0, x1, y1) ((void)(b), EmitCells(false, x0, y0, x1, y1))
#define VoxelTree_EmitBorder(b, x0, y0, x1, y1, pass) do { (void)(b); (void)(pass); EmitCells(true, x0, y0, x1, y1); } while(0)
#define VoxelBuildings_EmitSome(...) true
static bool JobPack(void) { return true; }
'''
    source += r'''
/* Drive the real post-submit entry point, not just cost admission in isolation. */
typedef struct { float gameMs, vblankMs; } CtrTiming;
static CtrTiming timing;
static struct { float afterMs, afterBudgetMs; } sStats;
static bool sReady, sViewReady, sCrossingSoon;
static uint64_t sAfterEndTick;
static float sTailMs;
static unsigned sAheadBackoff, postStarts;
static float sLastBuildMs, sInFrameBuildMs;
typedef struct { float cpuMs, gpuMs, gpuStartMs; bool voxelFrame; } CtrVideoStats;
static uint32_t sAfterStarvedFrames;
#define VOXEL_AFTER_STARVED_MS 1.0f
static CtrVideoStats videoTiming;
static const CtrVideoStats *CtrVideo_GetStats(void) { return &videoTiming; }
#define VOXEL_INFRAME_TARGET_MS 14.0f
#define VOXEL_INFRAME_MAX_MS 4.0f
#define VOXEL_STARVE_MS 0.75f
static float inFrameRemaining;
#define VOXEL_OTHERS_CAP_MS 12.0f
#define VOXEL_FRAME_MS 16.7f
#define VOXEL_AFTER_MARGIN_MS 1.0f
#define VOXEL_CROSSING_RESERVE_MS 2.0f
#define VOXEL_AFTER_MAX_MS 8.0f
static const CtrTiming *CtrPlatform_GetTiming(void) { return &timing; }
static uint64_t testTick = 1;
static uint64_t svcGetSystemTick(void) { return testTick; }
static float TicksMs(uint64_t ticks) { return (float)ticks; }
static float MsSince(uint64_t ticks) { (void)ticks; return 0.0f; }
static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static void VoxelWorld_BeginBatch(void) {}
static void RunAtlasJob(uint64_t a, float b, bool c) { (void)a; (void)b; (void)c; }
static unsigned RunJobs(uint64_t a, float holes, float ahead, bool inFrame, bool force, unsigned *missing) {
    (void)force; (void)missing;
    if (inFrame) { inFrameRemaining = holes - TicksMs(testTick - a); return 0; }
    if (StartNextJob(0, holes, ahead)) ++postStarts;
    return 0;
}
'''
    source += function(voxel, 'static float InFrameBudget(')
    source += function(voxel, 'void CtrVoxel_NoteFrameWait(')
    source += function(voxel, 'void CtrVoxel_AfterSubmit(')
    source += function(voxel, 'static void JobStep(')
    source += function(video, 'bool CtrVideo_TryVoxelUpload(')
    begin = voxel.index('    /* Cancel first, then mark the surviving request consumed. */')
    end = voxel.index('    sHolesOnly = !warmup;', begin)
    source += 'static void Refresh(bool holes) {\n' + voxel[begin:end] + '}\n'
    begin = voxel.index('    sHolesOnly = !warmup;')
    end = voxel.index('    sHolesOnly = false;', begin) + len('    sHolesOnly = false;')
    source += 'static void BudgetVisit(bool warmup, bool holes, bool starved) {\n'
    source += 'uint64_t started = 100; float holeMs = 4, aheadMs = 0, visitMs = 0; unsigned built, missing = 1;\n'
    source += voxel[begin:end] + '\n(void)built; (void)visitMs; }\n'
    source += r'''
int main(void) {
    /* The last present spent 5 ms fixed + 2 ms meshing and 6 ms on GPU:
     * meshing has 3 ms. A 2 ms scene visit is already in the fixed cost. */
    videoTiming = (CtrVideoStats){7, 6, 7, true}; sInFrameBuildMs = 2; testTick = 102;
    BudgetVisit(false, true, false);
    assert(inFrameRemaining == 3.0f);
    /* Warm-up intentionally includes the scene visit in its 4 ms cap. */
    BudgetVisit(true, true, false); assert(inFrameRemaining == 2.0f);
    /* The GPU started early, at 6 ms of a 9 ms present: only what came before
     * it is charged (charged whole, the 9 ms left 14 - 7 - 7 = 0). */
    videoTiming = (CtrVideoStats){9, 7, 6, true}; sInFrameBuildMs = 2;
    BudgetVisit(false, true, false); assert(inFrameRemaining == 3.0f);
    /* The first frame back from a 2D screen has no figures of its own. */
    videoTiming = (CtrVideoStats){7, 6, 7, false};
    BudgetVisit(false, true, false); assert(inFrameRemaining == 0.0f);
    videoTiming = (CtrVideoStats){10, 6, 10, true}; sInFrameBuildMs = 0;
    BudgetVisit(false, true, false); assert(inFrameRemaining == 0.0f);
    BudgetVisit(false, true, true); assert(inFrameRemaining == VOXEL_STARVE_MS);
    testTick = 1; sHolesOnly = true;

    VoxelMapInstance inst = {0}; VoxelAtlasSlot atlas = {.valid = true};
    sRequestCount = 1;
    sRequests[0] = (BuildRequest){.site = {.inst = &inst}, .atlas = &atlas, .need = NEED_STALE, .key = 100000};
    sRequests[0].waitIndex = RequestWait(&sRequests[0].site, -1);
    for (sFrame = 0; sFrame < 4; ++sFrame) { RequestWait(&sRequests[0].site, -1); assert(!StartNextJob(0, 2, 0)); }
    RequestWait(&sRequests[0].site, -1);
    /* Waiting for room (NoRoomForView): neither overdue nor started. */
    sRequestWait[sRequests[0].waitIndex].noRoomUntil = sFrame + 1;
    assert(!RequestOverdue(&sRequests[0]) && !StartNextJob(0, 2, 0) && starts == 0);
    sRequestWait[sRequests[0].waitIndex].noRoomUntil = sFrame;
    assert(StartNextJob(0, 2, 0) && starts == 1 && sJob.overdue && !sJob.hole);
    /* A square back in view after a gap waits from then, not from its old
     * record: it was overdue at once and forced a slice into the frame. */
    {
        uint32_t frame = sFrame;
        unsigned w;

        sFrame += 50; w = RequestWait(&sRequests[0].site, -1);
        assert(sRequestWait[w].progress == sFrame && !RequestOverdue(&sRequests[0]));
        ++sFrame; RequestWait(&sRequests[0].site, -1);
        assert(sRequestWait[w].progress == sFrame - 1);
        sFrame = frame; sRequestWait[w].progress = 0; sRequestWait[w].seen = sFrame;
    }
    sRequestWait[sJob.waitIndex].progress = sFrame;
    sRequests[0].done = false; Refresh(true);
    assert(sJob.active && sJob.overdue && sRequests[0].done);
    sJob.overdue = false; sRequests[0].done = false; Refresh(true);
    assert(!sJob.active && !sRequests[0].done);
    /* A completed prefetch mesh must survive new holes until upload. */
    sRequests[0].need = NEED_AHEAD;
    for (int phase = JOB_SORT; phase <= JOB_DONE; ++phase) {
        sJob.active = true; sJob.overdue = false; sJob.phase = phase;
        sRequests[0].done = false; Refresh(true);
        assert(sJob.active && sRequests[0].done);
    }
    sJob.active = true; sJob.forView = false; sJob.hole = false;
    sRequests[0].need = NEED_HOLE; Refresh(true);
    assert(sJob.active && sJob.forView && sJob.hole && sRequests[0].done);
    validJob = false; sRequests[0].done = false; Refresh(true);
    assert(!sJob.active && !sRequests[0].done);
    /* AHEAD must be rejected before submission but run after submission,
     * with an otherwise unchanged request and a positive spare budget. */
    validJob = true; sJob.active = false; sRequestCount = 1;
    sRequests[0].need = NEED_AHEAD; sRequests[0].done = false;
    sReady = sViewReady = true; sHolesOnly = true;
    assert(!StartNextJob(0, 2, 2));
    /* UpdateView clears its in-frame restriction before AfterSubmit. */
    sHolesOnly = false;
    CtrVoxel_AfterSubmit(1);
    assert(postStarts == 1 && sJob.active && !sJob.forView && sStats.afterBudgetMs > 0);
    /* Both stale prefetch and the next frame's positive budget recover too. */
    sJob.active = false; sRequests[0].done = false; sRequests[0].need = NEED_AHEAD_STALE;
    sViewReady = true; sHolesOnly = false;
    CtrVoxel_AfterSubmit(1); assert(postStarts == 2);
    /* No work without budget or during the explicit ahead backoff. */
    sJob.active = false; sRequests[0].done = false; sViewReady = true;
    sAheadBackoff = sFrame + 10;
    CtrVoxel_AfterSubmit(1); assert(postStarts == 2 && !sRequests[0].done);
    sAheadBackoff = 0; sViewReady = true; testTick = 100;
    CtrVoxel_AfterSubmit(0); assert(postStarts == 2 && !sRequests[0].done);
    sViewReady = true; testTick = 1;
    CtrVoxel_AfterSubmit(1); assert(postStarts == 3);
    /* The frame's real tail (end of AfterSubmit to the next FrameBegin) is
     * left free when it is longer than the game and the audio counted:
     * 16.7 - 0 elapsed - 12 tail (the cap) - 1 margin. Read once only. */
    assert(sAfterEndTick == testTick);
    CtrVoxel_NoteFrameWait(sAfterEndTick + 20); assert(sTailMs == 20.0f && sAfterEndTick == 0);
    sViewReady = true; sJob.active = true;
    CtrVoxel_AfterSubmit(1); assert(sStats.afterBudgetMs > 3.6f && sStats.afterBudgetMs < 3.8f);
    CtrVoxel_NoteFrameWait(testTick + 5); assert(sTailMs == 5.0f);
    CtrVoxel_NoteFrameWait(testTick + 5); assert(sTailMs == 0.0f);
    sJob.active = false;
    sHolesOnly = true;
    sRequestCount = 3;
    for (unsigned i = 0; i < 3; ++i) {
        sRequests[i] = sRequests[0]; sRequests[i].site.cx = (int)i;
        sRequests[i].waitIndex = RequestWait(&sRequests[i].site, -1);
        sRequests[i].need = i == 0 ? NEED_STALE : i == 1 ? NEED_HOLE : NEED_AHEAD;
        sRequests[i].key = sRequests[i].need * 100000u;
        sRequestWait[sRequests[i].waitIndex].progress = i == 0 ? 0 : sFrame;
    }
    qsort(sRequests, 3, sizeof(sRequests[0]), CompareRequests);
    assert(sRequests[0].need == NEED_HOLE && sRequests[1].need == NEED_STALE);
    memset(&sJob, 0, sizeof(sJob)); sJob.terrainCount = 6000;
    for (unsigned t = 0; t < 2000; ++t)
        for (unsigned v = 0; v < 3; ++v) sScratch[t*3+v] = (VoxelVertex){2.0f*(t%4)+v*0.5f, (float)t};
    unsigned steps = 1; while (!JobSort()) { assert(++steps < 100); }
    assert(steps > 1 && sJob.terrainFirst[4] == 6000);
    bool seen[2000] = {0};
    for (unsigned p = 0; p < 4; ++p)
        for (unsigned v = sJob.terrainFirst[p]; v < sJob.terrainFirst[p+1]; v += 3) {
            unsigned id = (unsigned)sScratch[v].marker;
            assert(!seen[id]); seen[id] = true;
            for (unsigned k = 0; k < 3; ++k) {
                assert(sScratch[v+k].marker == (float)id);
                assert((unsigned)(sScratch[v+k].u*0.5f) == p);
            }
        }
    memset(&sJob, 0, sizeof(sJob)); assert(JobSort());
    for (unsigned reserve = 24; reserve <= 28; reserve += 4) {
        gxCmdQueue_s queue = {32, 0}; sFrameQueue = &queue; sUploadCommands = 0; sRenderReserve = reserve;
        unsigned uploads = 0; while (CtrVideo_TryVoxelUpload()) ++uploads;
        assert(uploads == (32-reserve)/2);
        queue.numEntries = 31; sUploadCommands = 0;
        assert(!CtrVideo_TryVoxelUpload());
    }
    sFrameQueue = NULL; assert(!CtrVideo_TryVoxelUpload());
    /* Six physical pages total; eviction invalidates all of a hidden pair,
     * while both visible pairs and their generations remain intact. */
    int pairA, pairB, pairOld;
    maps[0] = (VoxelMapInstance){.primaryTileset = &pairA, .width = 16, .height = 16};
    maps[1] = (VoxelMapInstance){.primaryTileset = &pairB, .originX = 16, .width = 16, .height = 16};
    sAtlases[0].primaryTileset = &pairA; sAtlases[1].primaryTileset = &pairB;
    sAtlases[2].primaryTileset = &pairOld; sAtlases[2].valid = true;
    for (unsigned a = 0; a < 3; ++a)
        for (unsigned p = 0; p < 2; ++p) assert(AllocateAtlasPage(&sAtlases[a], p, true));
    assert(allocated == 6 && CountAllocatedAtlases() == 6);
    assert(AllocateAtlasPage(&sAtlases[0], 2, true));
    assert(!sAtlases[2].valid && sAtlases[2].generation == 1 && allocated == 5);
    assert(AllocateAtlasPage(&sAtlases[0], 3, true));
    assert(!AllocateAtlasPage(&sAtlases[1], 2, true) && allocated == 6);
    assert(sAtlases[0].generation == 0 && sAtlases[1].generation == 0);
    /* Production phase transitions must cover each cell once and capture
     * terrainCount once, before the first cell of the tree pass. */
    for (unsigned border = 0; border < 2; ++border) {
        memset(&sJob, 0, sizeof(sJob)); memset(cells, 0, sizeof(cells));
        sBuilder.count = 0;
        sJob.site = (ChunkSite){.inst = &inst, .x1 = 8, .y1 = 8};
        sJob.phase = border ? JOB_BORDER : JOB_GROUND;
        unsigned steps = 0;
        while (sJob.phase < JOB_SORT) { JobStep(); assert(++steps <= 256); }
        assert(sJob.terrainCount == 64*6 && sJob.buildingFirst == 64*30);
        assert(sJob.col == 0 && sJob.row == 0);
        for (unsigned c = 0; c < 64; ++c) assert(cells[0][c] == 1 && cells[1][c] == 2);
    }
    /* A chunk with no static models is done once packed; one with models is
     * done once each is converted (or found cached), a slice a step. */
    memset(&sJob, 0, sizeof(sJob)); sBuilder.staticCount = 0;
    sJob.phase = JOB_PACK; JobStep(); assert(sJob.phase == JOB_DONE);
    for (unsigned i = 0; i < 3; ++i) sJobStatics[i].pageModel = i;
    meshes[1].slicesLeft = 2; meshes[2].slicesLeft = 1; sBuilder.staticCount = 3;
    sJob.phase = JOB_PACK; JobStep(); assert(sJob.phase == JOB_STATICS);
    unsigned staticSteps = 0;
    while (sJob.phase == JOB_STATICS) { JobStep(); assert(++staticSteps <= 8); }
    assert(staticSteps == 4 && prepares == 4 && sJob.staticIndex == 3);
    for (unsigned i = 0; i < 3; ++i) assert(StaticMeshFind(i, false));
    /* Prefetch has only 1 ms per frame. A historical 5 ms sample must
     * become runnable without promoting the chunk to visible first. */
    sFrame = 100; NotePhaseCost(1, 10.0f);
    assert(PhaseCost(1) == 5.0f);
    unsigned waiting = 0;
    while (PhaseCost(1) > 1.0f) { ++sFrame; assert(++waiting <= 20); }
    /* Inside the frame it keeps half of what it last cost. */
    assert(PhaseCostInFrame(1) == 2.5f);
    NotePhaseCost(1, 0.2f);
    assert(PhaseCost(1) <= 1.0f); /* do not resurrect the stale outlier */
    ++sFrame; NotePhaseCost(1, 2.0f); assert(PhaseCost(1) == 2.0f);
    puts("PASS runtime: in-frame visit charged once, warm-up cap, post-submit prefetch admission, zero budget/backoff, stale deadline, preemption, promotion, cancellation ordering, page triangle ranges, queue admission, static model slices");
    return 0;
}
'''
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='voxel-runtime-', dir=ROOT / 'build') as temp:
        temp = Path(temp)
        path = temp / 'test.c'; path.write_text(source)
        exe = temp / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
if __name__ == '__main__':
    main()
