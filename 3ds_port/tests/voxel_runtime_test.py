"""Compile actual runtime functions in a host harness, without emulating PICA."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from video_text import read_video

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
    video = read_video()
    source = r'''
#include <assert.h>
#include <math.h>
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
#define VOXEL_ATLAS_PIXELS (VOXEL_ATLAS_W * VOXEL_ATLAS_H)
#define GPU_RGBA5551 0
#define GPU_NEAREST 0
#define GPU_CLAMP_TO_EDGE 0
enum { NEED_HOLE, NEED_STALE, NEED_AHEAD, NEED_AHEAD_STALE };
typedef struct { int mapGroup, mapNum, originX, originY, width, height; const void *primaryTileset, *secondaryTileset; } VoxelMapInstance;
typedef struct { VoxelMapInstance *inst; int cx, cy, x0, y0, x1, y1; bool border; } ChunkSite;
typedef struct { void *data; } C3D_Tex;
typedef struct { bool valid; const void *primaryTileset, *secondaryTileset;
    C3D_Tex tex, extra[3]; unsigned generation, stamp; } VoxelAtlasSlot;
static bool AtlasPressed(const VoxelAtlasSlot *slot) { (void)slot; return false; }
static VoxelAtlasSlot sAtlases[6];
static void AnimForget(VoxelAtlasSlot *slot) { (void)slot; }
static struct { VoxelAtlasSlot *slot; bool forView; } sAtlasJob;
static unsigned allocated;
static VoxelMapInstance maps[2];
static int sViewRect[4] = {0, 0, 32, 16};
static unsigned VoxelWorld_InstanceCount(void) { return 2; }
static const VoxelMapInstance *VoxelWorld_Instance(unsigned i) { return &maps[i]; }
/* Flat maps: no base, so AtlasInView's rows are the map's own. */
static float VoxelRelief_Base(const VoxelMapInstance *inst) { (void)inst; return 0.0f; }
static bool C3D_TexInitVRAM(C3D_Tex *tex, unsigned w, unsigned h, int fmt) {
    (void)w; (void)h; (void)fmt; if (allocated >= 6) return false;
    ++allocated; tex->data = &allocated; return true;
}
/* Room for one more atlas exactly while fewer than six are allocated. */
static unsigned long CtrGpuMem_LargestVram(void) { return allocated < 6 ? VOXEL_ATLAS_PIXELS * 2u : 0u; }
static void C3D_TexDelete(C3D_Tex *tex) { assert(tex->data && allocated); --allocated; tex->data = NULL; }
static void C3D_TexSetFilter(C3D_Tex *t, int a, int b) { (void)t; (void)a; (void)b; }
static void C3D_TexSetWrap(C3D_Tex *t, int a, int b) { (void)t; (void)a; (void)b; }
/* No building pages' arena here: nothing is borrowed, every page is VRAM. */
static void AtlasPageRelease(C3D_Tex *tex) { if (tex->data) C3D_TexDelete(tex); }
static bool sAtlasWaitsForGap;
static bool AtlasPageBorrow(C3D_Tex *tex) { (void)tex; return false; }
typedef struct { bool draft; void *vram; unsigned audit; } VoxelChunk;
typedef struct { ChunkSite site; VoxelAtlasSlot *atlas; VoxelChunk *chunk; uint32_t hash;
    bool hashKnown, done; unsigned key, need, waitIndex; } BuildRequest;
static struct { int group, map, cx, cy; bool border, used; uint32_t seen, progress, noRoomUntil; } sRequestWait[VOXEL_REQUESTS_MAX];
static BuildRequest sRequests[VOXEL_REQUESTS_MAX];
static uint8_t sWaitHead[128], sWaitNext[VOXEL_REQUESTS_MAX];
#define VOXEL_DRAFT_STARVE_FRAMES 90u
static unsigned sRequestCount, sFrame, starts;
enum { JOB_GROUND, JOB_TREES, JOB_MODELS, JOB_BORDER, JOB_BORDER_TREES, JOB_SORT, JOB_PACK, JOB_STATICS, JOB_DONE };
#define VOXEL_GROUND_SLICE_CELLS 1
#define VOXEL_MODEL_SLICE_TRIANGLES 64
#define VOXEL_STATIC_SLICE_TRIANGLES 64
#define VOXEL_SLICE_CAP_MS 5.0f
static bool sHolesOnly = true, validJob = true;
static struct { bool active, forView, hole, overdue; unsigned waitIndex; ChunkSite site;
    int phase, row, col, models; unsigned buildingFirst;
    unsigned sortPage, sortScan, sortWrite, terrainCount, terrainFirst[5], staticIndex; uint64_t ticks; } sJob;
typedef struct { float u, marker; } VoxelVertex;
static VoxelVertex sScratch[6000];
static bool JobValid(void) { return validJob; }
static void JobCancel(void) { sJob.active = false; }
static bool ColdTake(const ChunkSite *s, uint32_t h, VoxelAtlasSlot *a) { (void)s; (void)h; (void)a; return false; }
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
/* The clock, a tick a millisecond: a job's start is timed (NoteStartCost). */
static uint64_t testTick = 1;
static uint64_t svcGetSystemTick(void) { return testTick; }
static float TicksMs(uint64_t ticks) { return (float)ticks; }
/* The arena, as ChunkVram would ask it ahead of time (AheadRoomNone). */
#define VOXEL_AHEAD_BACKOFF_FRAMES 30u
static unsigned sAheadBackoff, sAheadNoRoomBytes;
static bool roomPossible, roomAskedForView;
static unsigned roomAsks, roomAskedBytes;
static bool ChunkRoomPossible(unsigned bytes, bool forView) {
    ++roomAsks; roomAskedBytes = bytes; roomAskedForView = forView; return roomPossible;
}
'''
    for signature in ['static unsigned WaitBucket(', 'static unsigned RequestWait(', 'static bool RequestOverdue(', 'static int CompareRequests(']:
        source += function(voxel, signature)
    source += through(voxel, '#define VOXEL_START_COST_MAX_MS', 'static void NoteStartCost(')
    source += through(voxel, 'static unsigned sAheadRoomBytes;', 'static bool AheadRoomNone(')
    for signature in ['static bool StartNextJob(', 'static bool JobSort(']:
        source += function(voxel, signature)
    source += through(voxel, 'static float sPhaseMs[JOB_DONE + 1];', 'static void NotePhaseCost(')
    for signature in ['static C3D_Tex *AtlasTex(', 'static void FreeAtlasTextures(', 'static unsigned CountAllocatedAtlases(', 'static int DrawnRows(', 'static bool AtlasInView(const VoxelAtlasSlot *slot)\n{', 'static bool AllocateAtlasPage(']:
        source += function(voxel, signature)
    source += r'''
static struct { unsigned count, staticCount; } sBuilder;
static bool sHaveBuildings;
/* Static models: each mesh cached once its slices of conversion are done. */
typedef struct { unsigned pageModel; bool lit; } VoxelStaticDraw;
static VoxelStaticDraw sJobStatics[4];
static struct StaticMesh { unsigned slicesLeft; } meshes[4];
static unsigned prepares;
static const struct StaticMesh *StaticMeshCurrent(unsigned pageModel, bool lit) {
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
#define VoxelTree_EmitInstance(b, inst, x0, y0, x1, y1) ((void)(b), (void)(inst), EmitCells(true, x0, y0, x1, y1))
#define VoxelMesh_EmitBorder(b, x0, y0, x1, y1) ((void)(b), EmitCells(false, x0, y0, x1, y1))
#define VoxelTree_EmitBorder(b, x0, y0, x1, y1) ((void)(b), EmitCells(true, x0, y0, x1, y1))
#define VoxelBuildings_EmitSome(...) true
static bool JobPack(void) { return true; }
'''
    source += r'''
/* Drive the real post-submit entry point, not just cost admission in isolation. */
typedef struct { float gameMs, vblankMs; } CtrTiming;
static CtrTiming timing;
static struct { float afterMs, afterBudgetMs, afterOwedMs, tailMs, tailExpectedMs; } sStats;
static bool sReady, sViewReady, sCrossingSoon;
static uint64_t sAfterEndTick;
static float sTailMs;
static unsigned postStarts;
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
static float MsSince(uint64_t ticks) { return TicksMs(testTick - ticks); }
static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static void VoxelWorld_BeginBatch(void) {}
static void RunAtlasJob(uint64_t a, float b, bool c) { (void)a; (void)b; (void)c; }
/* The cold mesh cache and the atlas pressure are not under test: never a hit,
 * nothing pending, no page held back. */
static unsigned sColdPendingCount;
static struct { float coldMs; } sKeyCounts;
static void ColdLand(void) {}
/* What AfterSubmit ran, in order: 'o' the missing models' owed slice (no
 * budget), 'j' the chunks' jobs, 'm' more of the missing models and 's' the
 * stale ones, in what the jobs leave. */
static char afterOrder[16];
static unsigned afterSteps;
static void AfterStep(char step) { if (afterSteps < sizeof(afterOrder) - 1) afterOrder[afterSteps++] = step; }
static void AfterOrderReset(void) { memset(afterOrder, 0, sizeof(afterOrder)); afterSteps = 0; }
/* The owed slice runs, and costs this many ticks, only while `owedTicks`. */
static uint64_t owedTicks;
static bool StaticWantsRun(uint64_t started, float budget, bool missing) {
    (void)started; AfterStep(!missing ? 's' : budget == 0.0f ? 'o' : 'm');
    if (!missing || budget != 0.0f || owedTicks == 0) return false;
    testTick += owedTicks; return true;
}
static unsigned RunJobs(uint64_t a, float holes, float ahead, bool inFrame, bool force, unsigned *missing) {
    (void)force; (void)missing;
    if (inFrame) { inFrameRemaining = holes - TicksMs(testTick - a); return 0; }
    AfterStep('j');
    if (StartNextJob(0, holes, ahead, false)) ++postStarts;
    return 0;
}
'''
    source += function(voxel, 'static float InFrameBudget(')
    source += function(voxel, 'void CtrVoxel_NoteFrameWait(')
    source += function(voxel, 'static void AfterOwedStatics(')
    source += function(voxel, 'void CtrVoxel_AfterSubmit(')
    source += function(voxel, 'static void JobStep(')
    source += '#define GX_QUEUE_SHARED 32u\nstatic unsigned sAnimCommands;\n'
    source += function(video, 'static unsigned SharedCapacity(')
    source += function(video, 'static unsigned SharedUsed(')
    source += function(video, 'bool CtrVideo_TryVoxelUpload(')
    source += function(video, 'bool CtrVideo_TryVoxelAnimUpload(')
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
    for (sFrame = 0; sFrame < 4; ++sFrame) { RequestWait(&sRequests[0].site, -1); assert(!StartNextJob(0, 2, 0, false)); }
    RequestWait(&sRequests[0].site, -1);
    /* Waiting for room (NoRoomForView): neither overdue nor started. */
    sRequestWait[sRequests[0].waitIndex].noRoomUntil = sFrame + 1;
    assert(!RequestOverdue(&sRequests[0]) && !StartNextJob(0, 2, 0, false) && starts == 0);
    sRequestWait[sRequests[0].waitIndex].noRoomUntil = sFrame;
    /* A start is let in only if what starts cost fits as well, counted at
     * no more than VOXEL_START_COST_MAX_MS; the job of an owed slice
     * (`forced`) starts whatever it costs. */
    NoteStartCost(false, 3.0f); assert(StartCost(false) == VOXEL_START_COST_MAX_MS);
    assert(!StartNextJob(1.0f, 2, 0, false) && starts == 0 && !sRequests[0].done);
    assert(StartNextJob(0, 2, 0, false) && starts == 1 && sJob.overdue && !sJob.hole);
    sRequests[0].done = false;
    assert(StartNextJob(1.0f, 2, 0, true) && starts == 2 && sJob.overdue && sRequests[0].done);
    memset(sStartMs, 0, sizeof(sStartMs)); memset(sStartFrame, 0, sizeof(sStartFrame));
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
    assert(!StartNextJob(0, 2, 2, false));
    /* UpdateView clears its in-frame restriction before AfterSubmit. */
    sHolesOnly = false;
    AfterOrderReset();
    CtrVoxel_AfterSubmit(1);
    assert(postStarts == 1 && sJob.active && !sJob.forView && sStats.afterBudgetMs > 0);
    /* A static model missing from the picture has its slice first, the
     * chunks' jobs counting it in; more of the missing, then the stale
     * ones (drawn meanwhile), in what the jobs leave. */
    assert(strcmp(afterOrder, "ojms") == 0);
    /* Both stale prefetch and the next frame's positive budget recover too. */
    sJob.active = false; sRequests[0].done = false; sRequests[0].need = NEED_AHEAD_STALE;
    sViewReady = true; sHolesOnly = false;
    CtrVoxel_AfterSubmit(1); assert(postStarts == 2);
    /* No work without budget or during the explicit ahead backoff. */
    sJob.active = false; sRequests[0].done = false; sViewReady = true;
    sAheadBackoff = sFrame + 10;
    CtrVoxel_AfterSubmit(1); assert(postStarts == 2 && !sRequests[0].done);
    sAheadBackoff = 0; sViewReady = true; testTick = 100;
    /* (a missing model's owed slice runs even then, timed apart into
     * afterOwedMs and counted in afterMs as well) */
    AfterOrderReset(); owedTicks = 2;
    CtrVoxel_AfterSubmit(0); assert(postStarts == 2 && !sRequests[0].done);
    assert(strcmp(afterOrder, "o") == 0);
    assert(sStats.afterOwedMs == 2.0f && sStats.afterMs == 2.0f);
    /* No slice owed, none timed; the next frame starts from nothing. */
    owedTicks = 0; sViewReady = true;
    CtrVoxel_AfterSubmit(0); assert(sStats.afterOwedMs == 0.0f);
    sViewReady = true; testTick = 1; owedTicks = 2; AfterOrderReset();
    CtrVoxel_AfterSubmit(1); assert(postStarts == 3);
    /* With a budget too, the owed slice first and timed alone. */
    assert(strcmp(afterOrder, "ojms") == 0);
    assert(sStats.afterOwedMs == 2.0f && sStats.afterMs == 2.0f);
    /* Reset with every call, even one that does nothing. */
    owedTicks = 0; sReady = false;
    CtrVoxel_AfterSubmit(1); assert(sStats.afterOwedMs == 0.0f && sStats.afterMs == 0.0f);
    sReady = sViewReady = true; testTick = 1;
    CtrVoxel_AfterSubmit(1); assert(sStats.afterOwedMs == 0.0f && postStarts == 3);
    /* A tail longer than a frame - a held top screen, a return from HOME -
     * spans more than the frame it follows: kept in the stats as measured,
     * but the budget is not held to the game's cap for it. */
    assert(sAfterEndTick == testTick);
    CtrVoxel_NoteFrameWait(sAfterEndTick + 20);
    assert(sTailMs == 0.0f && sStats.tailMs == 20.0f && sAfterEndTick == 0);
    sViewReady = true; sJob.active = true;
    CtrVoxel_AfterSubmit(1);
    assert(sStats.afterBudgetMs == VOXEL_AFTER_MAX_MS && sStats.tailExpectedMs == 0.0f);
    /* The frame's real tail (end of AfterSubmit to the next FrameBegin) is
     * left free when it is longer than the game and the audio counted:
     * 16.7 - 0 elapsed - 12 tail (the cap) - 1 margin. Read once only. */
    assert(sAfterEndTick == testTick);
    CtrVoxel_NoteFrameWait(sAfterEndTick + 14); assert(sTailMs == 14.0f && sAfterEndTick == 0);
    sViewReady = true;
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
    /* A queue lengthened to 128: the animation's copies have the 96 past the
     * shared 32 to themselves, and leave the shared room as it was. */
    {
        gxCmdQueue_s queue = {128, 0}; sFrameQueue = &queue; sUploadCommands = sAnimCommands = 0; sRenderReserve = 24;
        unsigned anim = 0; while (CtrVideo_TryVoxelAnimUpload()) ++anim;
        assert(anim == 48);
        queue.numEntries = sAnimCommands;
        unsigned uploads = 0; while (CtrVideo_TryVoxelUpload()) ++uploads;
        assert(uploads == 4);
        gxCmdQueue_s small = {32, 0}; sFrameQueue = &small; sUploadCommands = sAnimCommands = 0;
        anim = 0; while (CtrVideo_TryVoxelAnimUpload()) ++anim;
        assert(anim == 4); /* not lengthened: the shared room */
        sAnimCommands = 0;
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
        while (sJob.phase < JOB_SORT) { JobStep(); assert(++steps <= 128); }
        assert(sJob.terrainCount == 64*6 && sJob.buildingFirst == 64*18);
        assert(sJob.col == 0 && sJob.row == 0);
        for (unsigned c = 0; c < 64; ++c) assert(cells[0][c] == 1 && cells[1][c] == 1);
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
    for (unsigned i = 0; i < 3; ++i) assert(StaticMeshCurrent(i, false));
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
    /* A slow start (a signature read through the asset cache) is counted at
     * no more than VOXEL_START_COST_MAX_MS, and against the starts that work
     * a signature out alone. Aged as a phase's estimate is, the start it
     * keeps out of a window gets in a few frames on, with no start made in
     * between to measure it down: after FrameEnd none is forced. */
    memset(sStartMs, 0, sizeof(sStartMs)); memset(sStartFrame, 0, sizeof(sStartFrame));
    sSlotFreeSeen = false;
    sHolesOnly = false; sJob.active = false; sAheadBackoff = 0; sAheadNoRoomBytes = 0;
    sRequestCount = 1;
    sRequests[0] = (BuildRequest){.site = {.inst = &inst}, .atlas = &atlas, .need = NEED_AHEAD,
                                  .key = NEED_AHEAD * 100000u};
    sRequests[0].waitIndex = RequestWait(&sRequests[0].site, -1);
    NoteStartCost(false, 3.0f);
    assert(sStartMs[0] == VOXEL_START_COST_MAX_MS && StartCost(false) == VOXEL_START_COST_MAX_MS);
    assert(StartCost(true) == 0.0f);
    unsigned before = starts;
    assert(!StartNextJob(1.0f, 0, 2.0f, false) && starts == before && !sRequests[0].done);
    sRequests[0].hashKnown = true; /* (no signature to work out) */
    assert(StartNextJob(1.0f, 0, 2.0f, false) && starts == before + 1);
    sRequests[0].hashKnown = sRequests[0].done = false;
    /* Aged by the frames the job slot stood free (asked, no start made),
     * not by those of the job the last start opened. */
    sFrame += VOXEL_STARVE_FRAMES; /* (the job's own frames: nothing aged) */
    assert(StartCost(false) == VOXEL_START_COST_MAX_MS);
    assert(!StartNextJob(1.0f, 0, 2.0f, false) && starts == before + 1);
    sFrame += VOXEL_STARVE_FRAMES; /* (free that long: still kept whole) */
    assert(StartCost(false) == VOXEL_START_COST_MAX_MS);
    assert(!StartNextJob(1.0f, 0, 2.0f, false) && starts == before + 1);
    sFrame += VOXEL_STARVE_FRAMES;
    assert(StartCost(false) == VOXEL_START_COST_MAX_MS * 0.5f);
    assert(StartNextJob(1.0f, 0, 2.0f, false) && starts == before + 2);
    /* The start just made (0 ms) eases it down from the aged figure. */
    assert(StartCost(false) == VOXEL_START_COST_MAX_MS * 0.5f * 0.75f);
    /* A build ahead of time thrown away for want of room (AheadNoRoom) left
     * its size: while the arena could not make room for that much ahead of
     * time - ChunkRoomPossible, as ChunkVram asks it - no build ahead starts,
     * the backoff is kept and the arena asked once a frame; a view request
     * never waits for it. Once room could be made, or the size is forgotten,
     * the ring builds again. */
    memset(sStartMs, 0, sizeof(sStartMs));
    sRequests[0].done = false; before = starts; roomAsks = 0; sAheadSkipped = 0;
    sAheadNoRoomBytes = 4096; roomPossible = false;
    assert(!StartNextJob(0, 2, 2, false) && starts == before && !sRequests[0].done);
    assert(sAheadSkipped == 1 && sAheadBackoff == sFrame + VOXEL_AHEAD_BACKOFF_FRAMES);
    assert(roomAsks == 1 && roomAskedBytes == 4096 && !roomAskedForView);
    assert(!StartNextJob(0, 2, 2, false) && roomAsks == 1 && sAheadSkipped == 2);
    /* (not even asked without the time for a start) */
    assert(!StartNextJob(0, 2, 0, false) && roomAsks == 1 && sAheadSkipped == 2);
    sRequests[0].need = NEED_STALE;
    assert(StartNextJob(0, 2, 2, false) && starts == before + 1 && roomAsks == 1);
    sRequests[0].need = NEED_AHEAD; sRequests[0].done = false;
    ++sFrame; roomPossible = true;
    assert(StartNextJob(0, 2, 2, false) && starts == before + 2 && roomAsks == 2);
    /* A smaller mesh refused within the frame is asked after anew. */
    sRequests[0].done = false; roomPossible = false; sAheadNoRoomBytes = 2048;
    assert(!StartNextJob(0, 2, 2, false) && roomAsks == 3 && roomAskedBytes == 2048);
    sAheadNoRoomBytes = 0;
    assert(StartNextJob(0, 2, 2, false) && starts == before + 3 && roomAsks == 3);
    assert(sAheadSkipped == 3 && sAheadLost == 0);
    /* A chunk rebuilt in the block it has asks the arena for nothing: it is
     * not held with the new ones. */
    {
        static VoxelChunk own;
        unsigned asks = roomAsks;

        own.vram = &own;
        sRequests[0].done = false; sRequests[0].chunk = &own;
        sAheadNoRoomBytes = 4096; roomPossible = false; ++sFrame;
        assert(StartNextJob(0, 2, 2, false) && starts == before + 4 && roomAsks == asks && sAheadSkipped == 3);
        sRequests[0].chunk = NULL; sAheadNoRoomBytes = 0;
    }
    puts("PASS runtime: in-frame visit charged once, warm-up cap, post-submit prefetch admission, start cost admission, start cost aged and apart for known signatures, ahead starts held while no room for the last refused mesh (not a rebuild in its own block), zero budget/backoff, owed static slice timed apart, static model wants order, long tail ignored, stale deadline, preemption, promotion, cancellation ordering, page triangle ranges, queue admission, static model slices");
    return 0;
}
'''
    # The real RunJobs and its slice admission (JobSliceCost), around a job
    # whose slices cost what the test says: the clock counts microseconds.
    jobs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SYSCLOCK_ARM11 1000000.0
static uint64_t clockTicks = 1;
static uint64_t svcGetSystemTick(void) { return clockTicks; }
'''
    jobs += function(voxel, 'static float MsSince(') + function(voxel, 'static float TicksMs(')
    jobs += r'''
enum { JOB_GROUND, JOB_TREES, JOB_MODELS, JOB_BORDER, JOB_BORDER_TREES, JOB_SORT, JOB_PACK, JOB_STATICS, JOB_DONE };
typedef enum { BUILD_OK, BUILD_NO_STAGING, BUILD_NO_VRAM } BuildResult;
#define VOXEL_STARVE_FRAMES 4u
#define VOXEL_STARVE_MS 0.75f
#define VOXEL_SLICE_CAP_MS 5.0f
#define VOXEL_PACK_SLICE 256u
#define VOXEL_STAGING_VERTICES 12288u
#define VOXEL_CHUNK_UPLOADS_MAX 8u
#define VOXEL_AHEAD_BACKOFF_FRAMES 30u
enum { CTR_LOG_VIDEO };
static void CtrLog_Write(int channel, const char *format, ...) { (void)channel; (void)format; }
static uint32_t sFrame = 1000, sWarmupUntil, sAheadBackoff;
static bool sHolesOnly;
typedef struct { int mapGroup, mapNum; } VoxelMapInstance;
typedef struct { const VoxelMapInstance *inst; int cx, cy, baseX, baseY; bool border; } ChunkSite;
typedef struct { unsigned unused; } VoxelChunk;
static VoxelMapInstance inst;
static VoxelChunk theChunk;
static struct { bool active, forView, hole, overdue; ChunkSite site; VoxelChunk *chunk;
    int phase, row, col; unsigned waitIndex, packed, rays, steps, surfaces;
    uint64_t ticks, phaseTicks[JOB_DONE + 1]; uint32_t firstFrame; bool cold; } sJob;
static struct { uint32_t progress; } sRequestWait[1];
static struct { unsigned count, staticCount; } sBuilder;
static unsigned sStagingUsed, sChunkUploads;
struct CtrAssetStats { unsigned misses; };
static const struct CtrAssetStats *CtrAssets_GetStats(void) { static struct CtrAssetStats stats; return &stats; }
static unsigned LightRays(void) { return 0; }
static unsigned LightSteps(void) { return 0; }
static unsigned LightSurfaceTests(void) { return 0; }
/* A slice: the clock moves on by what it costs, the job to `nextPhase` if set. */
static unsigned slicesRun, uploads;
static uint64_t sliceTicks, uploadTicks;
static int nextPhase = -1;
static void JobStep(void) { ++slicesRun; clockTicks += sliceTicks; if (nextPhase >= 0) sJob.phase = nextPhase; }
/* A static model converted with nowhere to go yet (StaticBuildFinish). */
static bool held;
static bool StaticBuildHeld(void) { return held; }
/* The next job, if one is queued, as StartNextJob would take it. */
static unsigned queued;
static int queuedPhase;
static bool queuedView;
static bool StartNextJob(float elapsed, float holeMs, float aheadMs, bool forced) {
    (void)elapsed; (void)holeMs; (void)aheadMs; (void)forced;
    if (queued == 0) return false;
    --queued; memset(&sJob, 0, sizeof(sJob));
    sJob.active = true; sJob.forView = queuedView; sJob.phase = queuedPhase; sJob.site.inst = &inst;
    return true;
}
/* An upload: always room, the clock moved on by what it costs. */
static bool IsChunkOf(const VoxelChunk *chunk, const ChunkSite *site) { (void)chunk; (void)site; return true; }
static VoxelChunk *FindChunk(bool border, int group, int num, int cx, int cy) {
    (void)border; (void)group; (void)num; (void)cx; (void)cy; return &theChunk;
}
static VoxelChunk *FreeChunkSlot(bool forView) { (void)forView; return &theChunk; }
/* Eviction with no cold cache behind it: room, as FreeChunkSlot, and no count. */
#define STAGING_ROOM VOXEL_STAGING_VERTICES
static unsigned sStagingPromise, sCopyPromise;
static struct { unsigned coldHits, coldMs; } sKeyCounts;
static unsigned CtrVideo_VoxelUploadsLeft(void) { return VOXEL_CHUNK_UPLOADS_MAX; }
static void EvictChunk(VoxelChunk *chunk) { (void)chunk; }
static BuildResult finishResult = BUILD_OK;
static BuildResult JobFinish(VoxelChunk *chunk) { (void)chunk; ++uploads; clockTicks += uploadTicks; return finishResult; }
static unsigned viewNoRoom;
static void NoRoomForView(void) { ++viewNoRoom; }
/* What AheadNoRoom reads of the arena, for its log line only. */
typedef struct { float u, v; int16_t x, y, z, shade; } VoxelGpuVertex;
static struct { unsigned long used; } sChunkArena;
static unsigned long VoxelArena_LargestGap(const void *arena) { (void)arena; return 0; }
static unsigned sAheadNoRoomBytes, sAheadLost, sAheadSkipped;
static void NoteBuildCost(VoxelChunk *chunk, const ChunkSite *site, float ms, uint32_t frames) {
    (void)chunk; (void)site; (void)ms; (void)frames;
}
static bool IsDrawn(const VoxelChunk *chunk) { (void)chunk; return true; }
static bool SiteVisible(const ChunkSite *site, const VoxelChunk *chunk) { (void)site; (void)chunk; return false; }
static void Draw(VoxelChunk *chunk, int x, int z) { (void)chunk; (void)x; (void)z; }
'''
    jobs += through(voxel, 'static float sPhaseMs[JOB_DONE + 1];', 'static float JobSliceCost(')
    jobs += function(voxel, 'static void AheadNoRoom(')
    jobs += function(voxel, 'static bool JobSliceMayFinish(')
    jobs += function(voxel, 'static unsigned RunJobs(')
    jobs += r'''
static void Forget(void) {
    memset(sPhaseMs, 0, sizeof(sPhaseMs)); memset(sPhaseFrame, 0, sizeof(sPhaseFrame));
    memset(sPhaseRecent, 0, sizeof(sPhaseRecent)); memset(sPhaseRecentNext, 0, sizeof(sPhaseRecentNext));
}
/* `ms` measured for `phase`, `age` frames before the next call (Run). */
static void Measured(int phase, float ms, uint32_t age) {
    uint32_t now = sFrame;
    sFrame = now + 1 - age; NotePhaseCost(phase, ms); sFrame = now;
}
static void Job(int phase, bool view) {
    memset(&sJob, 0, sizeof(sJob));
    sJob.active = true; sJob.forView = view; sJob.phase = phase; sJob.site.inst = &inst;
}
/* One call in a new frame, after FrameEnd or inside the frame, `ms` for the
 * view and as much ahead: the chunks it uploads. */
static unsigned Run(float ms, bool inFrame) {
    slicesRun = uploads = 0; ++sFrame;
    return RunJobs(clockTicks, ms, ms, inFrame, false, NULL);
}
int main(void) {
    /* A view job's ground slice measured 4 ms forty frames ago, its estimate
     * aged to a tenth of that; its slices now take 0.5 ms. The call's first
     * slice goes by the estimate, as every one did; one chained after it is
     * counted at the 4 ms its phase last took, and 2.4 ms cannot hold it. */
    sliceTicks = 500;
    Forget(); Measured(JOB_GROUND, 4.0f, 40); assert(PhaseCost(JOB_GROUND) < 0.5f);
    Job(JOB_GROUND, true); Run(2.4f, false);
    assert(slicesRun == 1 && sJob.active && sJob.phase == JOB_GROUND);
    assert(JobSliceCost(JOB_GROUND, false, false) == 0.5f && JobSliceCost(JOB_GROUND, false, true) == 4.0f);
    assert(JobSliceCost(JOB_GROUND, true, false) == 0.5f && JobSliceCost(JOB_GROUND, true, true) == 4.0f);
    /* The next call offers it its next slice on the same terms: the floor
     * never holds a job, and every job behind it, out of every window. */
    Run(2.4f, false); assert(slicesRun == 1);
    /* Ahead of time, and in the warm-up, chained slices go by the estimate:
     * four of 0.5 ms into 2.4. */
    Forget(); Measured(JOB_GROUND, 4.0f, 40);
    Job(JOB_GROUND, false); Run(2.4f, false); assert(slicesRun == 4);
    Forget(); Measured(JOB_GROUND, 4.0f, 40); sWarmupUntil = sFrame + 2;
    Job(JOB_GROUND, true); Run(2.4f, false); assert(slicesRun == 4);
    sWarmupUntil = 0;
    /* A stray 20 ms sample is a card read, not the phase's cost: the floor
     * stops at VOXEL_VIEW_FLOOR_MAX_MS, and 7.2 ms then hold three slices. */
    Forget(); Measured(JOB_GROUND, 20.0f, 40);
    Job(JOB_GROUND, true); Run(7.2f, false);
    assert(slicesRun == 3 && JobSliceCost(JOB_GROUND, false, true) == VOXEL_VIEW_FLOOR_MAX_MS);
    /* Inside the frame, once a chunk is uploaded (1 ms), the slice that may
     * finish the next job counts the upload that would follow it whatever
     * the time; a slice that cannot finish it does not. */
    sliceTicks = uploadTicks = 1000; sBuilder.count = 100; sBuilder.staticCount = 0;
    Forget(); Measured(JOB_DONE, 1.0f, 1); Measured(JOB_PACK, 1.0f, 1); Measured(JOB_GROUND, 1.0f, 1);
    Job(JOB_DONE, false); queued = 1; queuedPhase = JOB_PACK; queuedView = false;
    assert(Run(2.5f, true) == 1 && uploads == 1 && slicesRun == 0);
    assert(queued == 0 && sJob.active && sJob.phase == JOB_PACK);
    Job(JOB_DONE, false); queued = 1; queuedPhase = JOB_GROUND;
    assert(Run(2.5f, true) == 1 && slicesRun == 1 && sJob.active && sJob.phase == JOB_GROUND);
    /* As the call's first, the same slice goes in, and its upload follows. */
    Job(JOB_PACK, false); nextPhase = JOB_DONE;
    assert(Run(2.5f, true) == 1 && slicesRun == 1 && uploads == 1 && !sJob.active);
    nextPhase = -1;
    /* A static model converted with nowhere to go yet holds the converter
     * till the next update: the job's slices stop there, time or not. */
    sliceTicks = 500;
    Forget(); Measured(JOB_STATICS, 0.5f, 1);
    Job(JOB_STATICS, true); held = true; Run(8.0f, false);
    assert(slicesRun == 1 && sJob.active);
    held = false; Run(8.0f, false); assert(slicesRun > 1);
    /* An upload ahead of time with no room (BUILD_NO_VRAM) is thrown away,
     * the ring backed off and the smallest such mesh kept (AheadNoRoom); one
     * for the view waits its own way and leaves the size be; an upload ahead
     * of time that finds room forgets it, one for the view does not. */
    sliceTicks = uploadTicks = 0; sAheadBackoff = 0; finishResult = BUILD_NO_VRAM;
    sBuilder.count = 100; sBuilder.staticCount = 0;
    Job(JOB_DONE, false);
    assert(Run(2.5f, true) == 0 && uploads == 1 && !sJob.active);
    assert(sAheadLost == 1 && sAheadNoRoomBytes == 100 * sizeof(VoxelGpuVertex));
    assert(sAheadBackoff == sFrame + VOXEL_AHEAD_BACKOFF_FRAMES);
    sBuilder.count = 300; Job(JOB_DONE, false); Run(2.5f, true);
    assert(sAheadLost == 2 && sAheadNoRoomBytes == 100 * sizeof(VoxelGpuVertex));
    sBuilder.count = 51; Job(JOB_DONE, false); Run(2.5f, true);
    assert(sAheadLost == 3 && sAheadNoRoomBytes == 51 * sizeof(VoxelGpuVertex));
    sAheadBackoff = 0; Job(JOB_DONE, true); Run(2.5f, true);
    assert(viewNoRoom == 1 && sAheadLost == 3 && sAheadBackoff == 0);
    finishResult = BUILD_OK;
    Job(JOB_DONE, true); assert(Run(2.5f, true) == 1 && sAheadNoRoomBytes == 51 * sizeof(VoxelGpuVertex));
    Job(JOB_DONE, false); assert(Run(2.5f, true) == 1 && sAheadNoRoomBytes == 0 && sAheadSkipped == 0);
    puts("PASS runtime jobs: view floor only after other work of the call and bounded, first slice of every call on the estimate, ahead and warm-up on the estimate, in-frame upload counted for a finishing slice, held static conversion ends the call, ahead upload with no room kept as the smallest size and forgotten on an ahead upload");
    return 0;
}
'''
    # The real static-mesh cache: lit meshes kept per sun, converted after
    # FrameEnd, their replacement placed in their own slot.
    begin = voxel.rfind('typedef struct', 0, voxel.index('} VoxelGpuVertex;'))
    end = voxel.index('\n', voxel.index('#define VOXEL_SHADE_SCALE')) + 1
    statics = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CTR_VOXEL_LIGHTING 1
#define SYSCLOCK_ARM11 1000000.0
#define VOXEL_SLICE_CAP_MS 5.0f
#define VOXEL_STATIC_SLICE_TRIANGLES 64u
static uint64_t clockTicks = 1;
static uint64_t svcGetSystemTick(void) { return clockTicks; }
static uint32_t sFrame;
enum { CTR_LOG_VIDEO, CTR_LOG_ERROR };
static unsigned logs;
static void CtrLog_Write(int channel, const char *format, ...) { (void)channel; (void)format; ++logs; }
typedef struct { float x, y, z; float u, v; float shade; } VoxelVertex;
'''
    statics += voxel[begin:end]
    statics += function(voxel, 'static float MsSince(') + function(voxel, 'static float TicksMs(')
    statics += 'static unsigned sPackErrors;\nstatic float sPackWorst;\n' + function(voxel, 'static int16_t Quantise(')
    statics += r'''
typedef struct { float dx, dz, strength; uint32_t key; } VoxelSun;
static VoxelSun gVoxelSun = {0.85f, 0.55f, 1.0f, 0};
/* A triangle's lit shade: under a sun from the east one for every triangle,
 * from the west each its own, so that a quad's two then share no corner. */
static float VoxelLighting_ModelShade(const VoxelVertex *a, const VoxelVertex *b, const VoxelVertex *c, float shade) {
    (void)b; (void)c;
    return gVoxelSun.dx >= 0.0f ? shade * gVoxelSun.dx : shade * 0.5f + a->z * 0.25f;
}
/* Linear memory: heap blocks, none while `linearFull`. */
static bool linearFull;
static unsigned linearLive, linearFrees;
static uintptr_t linearLastFreed;
static void *linearAlloc(size_t size) {
    void *block = linearFull ? NULL : malloc(size);
    if (block) ++linearLive;
    return block;
}
static void linearFree(void *block) {
    assert(block && linearLive);
    --linearLive; ++linearFrees; linearLastFreed = (uintptr_t)block; free(block);
}
static int GSPGPU_FlushDataCache(const void *data, uint32_t size) { (void)data; (void)size; return 0; }
/* Page model 3: 64 quads apart, two triangles each, 2 slices of conversion. */
#define QUADS 64u
static VoxelVertex modelVertices[QUADS * 6u];
static bool VoxelBuildings_StaticModel(unsigned pageModel, const VoxelVertex **vertices,
                                       unsigned *count, float *ox, float *oy, float *su, float *sv) {
    if (pageModel != 3) return false;
    *vertices = modelVertices; *count = QUADS * 6u; *ox = *oy = 0.0f; *su = *sv = 1.0f / 256.0f;
    return true;
}
'''
    statics += voxel[voxel.index('#define VOXEL_STATIC_MESHES 64u'):voxel.index('static void SetModelViewAt(')]
    statics += r'''
#define SLICE VOXEL_STATIC_SLICE_TRIANGLES
int main(void) {
    static const unsigned corners[6] = {0, 1, 2, 2, 1, 3};
    const StaticMesh *mesh;
    uintptr_t first, stale;
    unsigned frees;

    for (unsigned q = 0; q < QUADS; ++q)
        for (unsigned k = 0; k < 6; ++k)
            modelVertices[q * 6 + k] = (VoxelVertex){q * 0.5f + (corners[k] & 1u) * 0.25f, 0.0f,
                                             (float)(corners[k] >> 1), 0.0f, 0.0f, 1.0f};
    /* A model missing from the picture, asked for by a draw: converted after
     * FrameEnd, a slice owed it every frame whatever the budget. */
    sFrame = 1;
    assert(StaticMeshFind(3, true) == NULL);
    StaticWant(3, true, true); /* (DrawStatics: no mesh to draw) */
    StaticWantsRun(clockTicks, 0.0f, false); assert(!sStaticBuild.active);
    StaticWantsRun(clockTicks, 0.0f, true); assert(sStaticBuild.active && sStaticBuild.done == SLICE * 3u);
    StaticWantsRun(clockTicks, 0.0f, true); assert(sStaticBuild.done == SLICE * 3u);
    sFrame = 2; StaticWant(3, true, true);
    StaticWantsRun(clockTicks, 0.0f, true);
    mesh = StaticMeshCurrent(3, true);
    assert(mesh && !sStaticBuild.active && linearLive == 1 && mesh->count == QUADS * 6u && mesh->sunDx == 0.85f);
    first = (uintptr_t)mesh->vertices;
    /* A new sun: the lit mesh is stale, drawn as it is and asked for again
     * (no slice owed: only time left over), until its replacement is
     * converted into its own slot. */
    gVoxelSun.dx = 0.6f; StaticMeshesRelight(); sFrame = 3;
    assert(StaticMeshCurrent(3, true) == NULL && StaticMeshFind(3, true) == mesh);
    assert(sStaticWantCount == 1 && !sStaticWants[0].missing);
    StaticWantsRun(clockTicks, 0.0f, true); StaticWantsRun(clockTicks, 0.0f, false);
    assert(!sStaticBuild.active);
    assert(!StaticMeshPrepare(3, true, SLICE) && sStaticBuild.active); /* a slice (JOB_STATICS) */
    assert(StaticMeshFind(3, true) == mesh && (uintptr_t)mesh->vertices == first);
    StaticWantsRun(clockTicks, 10.0f, false); /* the rest, after FrameEnd */
    assert(StaticMeshCurrent(3, true) == mesh && (uintptr_t)mesh->vertices != first && mesh->sunDx == 0.6f);
    /* The memory this frame drew goes once FrameBegin has waited for the GPU. */
    assert(linearLive == 2 && linearFrees == 0 && sStaticLateCount == 1);
    sFrame = 4; StaticMeshesLateFree(); /* (CtrVoxel_Update) */
    assert(linearLive == 1 && linearFrees == 1 && linearLastFreed == first && sStaticLateCount == 0);
    /* One no draw of this frame read gives its memory up at once. */
    gVoxelSun.dx = 0.7f; sFrame = 5; stale = (uintptr_t)mesh->vertices;
    assert(!StaticMeshPrepare(3, true, SLICE) && StaticMeshPrepare(3, true, SLICE));
    assert(linearLive == 1 && linearFrees == 2 && linearLastFreed == stale && sStaticLateCount == 0);
    /* Back under a sun it was lit under, a mesh is current again as it
     * stands: a door and back with DAY / NIGHT on. */
    gVoxelSun.dx = 0.6f; assert(StaticMeshCurrent(3, true) == NULL);
    gVoxelSun.dx = 0.7f; assert(StaticMeshCurrent(3, true) == mesh);
    /* No room for both, and the stale mesh drawn this frame: the finished
     * conversion is held, the stale mesh drawn meanwhile, and the next update
     * places it in the stale mesh's own block, which no draw reads by then. */
    gVoxelSun.dx = 0.5f; sFrame = 6; frees = linearFrees;
    assert(StaticMeshFind(3, true) == mesh);
    stale = (uintptr_t)mesh->vertices;
    linearFull = true;
    assert(!StaticMeshPrepare(3, true, SLICE) && !StaticMeshPrepare(3, true, SLICE) && StaticBuildHeld());
    assert(StaticMeshFind(3, true) == mesh && (uintptr_t)mesh->vertices == stale && !StaticMeshCurrent(3, true));
    StaticWantsRun(clockTicks, 10.0f, false); /* held: nothing to do till then */
    assert(StaticBuildHeld() && (uintptr_t)mesh->vertices == stale);
    sFrame = 7; StaticMeshesLateFree(); StaticBuildPlaceHeld(); /* (CtrVoxel_Update) */
    assert(!StaticBuildHeld() && !sStaticBuild.active && StaticMeshCurrent(3, true) == mesh);
    assert((uintptr_t)mesh->vertices == stale && mesh->sunDx == 0.5f && linearFrees == frees && linearLive == 1);
    /* There, a stale mesh the last frame drew too small for its replacement
     * (under this sun no corner is shared) stays as it is, drawn stale rather
     * than not at all, and the model is tried again later, converted anew. */
    gVoxelSun.dx = -0.5f; sFrame = 8;
    assert(StaticMeshFind(3, true) == mesh);
    assert(!StaticMeshPrepare(3, true, SLICE) && !StaticMeshPrepare(3, true, SLICE) && StaticBuildHeld());
    sFrame = 9; StaticMeshesLateFree(); StaticBuildPlaceHeld();
    assert(!sStaticBuild.active && StaticBad(3) && StaticMeshCurrent(3, true) == NULL && sStaticFailLogged);
    assert(StaticMeshFind(3, true) == mesh && (uintptr_t)mesh->vertices == stale && linearFrees == frees);
    linearFull = false; sFrame += VOXEL_STATIC_RETRY_FRAMES;
    assert(!StaticBad(3) && !StaticMeshPrepare(3, true, SLICE) && StaticMeshPrepare(3, true, SLICE));
    assert(StaticMeshCurrent(3, true) == mesh && mesh->sunDx == -0.5f && mesh->bytes > (QUADS * 4u) * sizeof(VoxelGpuVertex) + QUADS * 6u * 2u);
    assert(linearLive == 1 && linearFrees == frees + 1 && linearLastFreed == stale);
    StaticMeshesFree(); assert(linearLive == 0);
    puts("PASS runtime statics: missing model owed a slice a frame, stale lit mesh drawn until its replacement takes its slot, late free after the GPU wait, current again under its own sun, held conversion placed at the next update, too-small predecessor kept and retried");
    return 0;
}
'''
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='voxel-runtime-', dir=ROOT / 'build') as temp:
        temp = Path(temp)
        for name, text in (('test', source), ('jobs', jobs), ('statics', statics)):
            path = temp / (name + '.c'); path.write_text(text, encoding='utf-8')
            exe = temp / (name + '.exe')
            subprocess.run([args.cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(exe), '-lm'], check=True)
            subprocess.run([str(exe)], check=True)
if __name__ == '__main__':
    main()
