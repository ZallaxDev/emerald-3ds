"""The gap of the building pages' arena lent to the battle, compiled out of
ctr_voxel.c itself and run on the host over the real arena.

- one gap at a time, nothing lent over a parked world, given back to the arena;
- nothing of the arena's asks for it back while it finds room (a voxel battle's
  world updates all through a move);
- a window mask or an atlas page with no room asks only when the gap back would
  make it, and a mask then looks again at once instead of waiting its retry;
- an atlas page waiting for the gap says so (AllocateAtlasPage), and that holds
  for that one answer alone: the callers tell it from a want of VRAM;
- a page drawn from asks before anything slides or goes, when the gap back makes
  room or a way to it by sliding; WantPage's other steps come next, and it asks
  last of all when the gap and the masks in view make room together and neither
  does alone; only then is it a page with no room;
- a page not drawn from never asks.
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


def define(source, name):
    start = source.index('#define ' + name + ' ')
    return source[start:source.index('\n', start) + 1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    voxel = (ROOT / 'src/voxel/ctr_voxel.c').read_text(encoding='utf-8')

    defines = ''.join(define(voxel, name) for name in (
        'VOXEL_BUILDING_PAGES', 'VOXEL_ATLAS_BORROW_MAX', 'VOXEL_PAGE_PIECES', 'VOXEL_PAGE_RETRY_FRAMES',
        'VOXEL_PAGE_LENT_RETRY_FRAMES', 'VOXEL_PAGE_MOVE_COPY', 'VOXEL_PAGE_SLIDE_COPIES',
        'VOXEL_PAGE_MOVES_MAX', 'VOXEL_PAGE_KEEP_VIEW', 'VOXEL_PAGE_QUIET_RETRY_FRAMES'))
    moves_end = voxel.index('} sPageMoves;')
    moves = voxel[voxel.rindex('static struct', 0, moves_end):moves_end + 1]
    functions = ''.join(function(voxel, signature) for signature in (
        'static void GlowFree(', 'static void GlowDrop(', 'static bool StreamHolds(', 'static bool GlowRelease(',
        'static bool GlowDropOthers(', 'static bool PagesLentAsk(', 'static bool AtlasPageBorrow(',
        'static bool AllocateAtlasPage(', 'void *CtrVoxel_LendPagesGap(', 'bool CtrVoxel_PagesGapWanted(',
        'void CtrVoxel_ReturnPagesGap(', 'static void GlowWant(', 'static unsigned PageBytes(',
        'static bool PageDroppable(', 'static void PageFree(', 'static BuildingPageSlot *FindPage(',
        'static uint32_t FramesAgo(', 'static void PageRoomFreed(', 'static bool PageRoomPossible(',
        'static void PageNoRoom(', 'static BuildingPageSlot *PageVictim(', 'static void PagePieces(',
        'static uint32_t PagePieceHeld(', 'static bool PageSlidePlan(', 'static bool PageWaitsForLentGap(',
        'static bool PageMovesWanted(', 'static bool PageMovesActive(', 'static void WantPage('))

    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voxel_arena.h"

#define VOXEL_ATLAS_W 512u
#define VOXEL_ATLAS_H 256u
#define VOXEL_ATLAS_PIXELS (VOXEL_ATLAS_W * VOXEL_ATLAS_H)
#define VOXEL_MAX_PAGES 16
#define VOXEL_ATLAS_SLOTS 2u
#define CTR_LOG_VIDEO 1
#define CTR_LOG_ERROR 2
#define GPU_L8 1
#define GPU_RGBA5551 2
#define GPU_NEAREST 0
#define GPU_CLAMP_TO_EDGE 0
#define K 1024u
''' + defines + r'''
typedef struct { void *data; unsigned fmt, size; uint16_t width, height; } C3D_Tex;
typedef struct
{
    int page;
    unsigned w, h, loaded;
    C3D_Tex tex;
    uint32_t used;
    C3D_Tex glow;
    unsigned glowLoaded;
    uint32_t glowUsed, glowRetry, glowDropped;
    bool glowDropping;
    uint64_t requested;
} BuildingPageSlot;
typedef struct { C3D_Tex tex, page1; uint32_t stamp; } VoxelAtlasSlot;
enum { STREAM_IDLE };
static struct { BuildingPageSlot *slot; int state; bool copyQueued; } sStream;
static BuildingPageSlot sPageSlots[VOXEL_BUILDING_PAGES];
static VoxelArena sPageArena;
static VoxelArenaPiece sPagePieces[VOXEL_PAGE_PIECES];
static uint32_t sPageRetry[VOXEL_MAX_PAGES], sPageRingRetry[VOXEL_MAX_PAGES], sFrame = 1000;
static bool sGlowWanted = true, sGlowNoRoomLogged[VOXEL_MAX_PAGES], sParked;
static bool sBadPages[VOXEL_MAX_PAGES], sPageNoRoomLogged[VOXEL_MAX_PAGES];
static void *sPageBlock, *sPagesLent;
static bool sPagesLentWanted, sAtlasWaitsForGap;
static unsigned sBorrowed, sNewPageBytes;
static uint8_t *sMemory;
static struct { unsigned errors; } sStats;
static VoxelAtlasSlot sAtlases[VOXEL_ATLAS_SLOTS];
static struct { VoxelAtlasSlot *slot; } sAtlasJob;
''' + moves + r''' sPageMoves;

static void CtrLog_Write(int kind, const char *format, ...) { (void)kind; (void)format; }
static unsigned long vramSpaceFree(void) { return 0; }
static unsigned CountBorrowedAtlasPages(void) { return sBorrowed; }
static bool VoxelBuildings_PageHasGlow(unsigned page) { (void)page; return true; }
static uint64_t svcGetSystemTick(void) { return 0; }
/* The page asked for: sNewPageBytes, a texture of as many rows as it has. */
static bool VoxelBuildings_PageSize(unsigned page, unsigned *w, unsigned *h, unsigned *th)
{
    (void)page;
    *w = 512;
    *h = *th = sNewPageBytes / (512u * 2u);
    return true;
}
/* VRAM with no room, every atlas holding some on screen. */
static unsigned CountAllocatedAtlases(void) { return 1; }
static bool AtlasPressed(const VoxelAtlasSlot *slot) { (void)slot; return false; }
static unsigned CtrGpuMem_LargestVram(void) { return 0; }
static bool C3D_TexInitVRAM(C3D_Tex *tex, unsigned w, unsigned h, int fmt)
{
    (void)tex; (void)w; (void)h; (void)fmt;
    return false;
}
static void C3D_TexSetFilter(C3D_Tex *tex, int a, int b) { (void)tex; (void)a; (void)b; }
static void C3D_TexSetWrap(C3D_Tex *tex, int a, int b) { (void)tex; (void)a; (void)b; }
static C3D_Tex *AtlasTex(VoxelAtlasSlot *slot, unsigned page) { return page != 0 ? &slot->page1 : &slot->tex; }
static bool AtlasInView(const VoxelAtlasSlot *slot) { (void)slot; return true; }
static void AnimForget(VoxelAtlasSlot *slot) { (void)slot; }
static void FreeAtlasTextures(VoxelAtlasSlot *slot) { (void)slot; }
static void PageTexInit(C3D_Tex *tex, void *data, unsigned w, unsigned th)
{
    memset(tex, 0, sizeof(*tex));
    tex->data = data;
    tex->width = (uint16_t)w;
    tex->height = (uint16_t)th;
}

typedef struct { BuildingPageSlot *slot; bool glow, freed, movable; } PagePiece;
''' + functions + r'''
/* An arena of 1088 KiB as a battle finds it: two pages drawn from, the gap lent
 * between them, 232 KiB left at the top. */
static void Reset(void)
{
    memset(sPageSlots, 0, sizeof(sPageSlots));
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i) sPageSlots[i].page = -1;
    VoxelArena_Init(&sPageArena, sMemory, 1088u * K, 128, sPagePieces, VOXEL_PAGE_PIECES);
    sPageBlock = sMemory;
    sPagesLent = NULL;
    sPagesLentWanted = sAtlasWaitsForGap = sParked = false;
    sBorrowed = 0;
    sStats.errors = 0;
    memset(sPageRetry, 0, sizeof(sPageRetry));
    memset(sPageRingRetry, 0, sizeof(sPageRingRetry));
    memset(sPageNoRoomLogged, 0, sizeof(sPageNoRoomLogged));
    memset(&sPageMoves, 0, sizeof(sPageMoves));
    memset(&sStream, 0, sizeof(sStream));
    memset(sAtlases, 0, sizeof(sAtlases));
}

static void Page(unsigned i, int page, unsigned bytes)
{
    BuildingPageSlot *slot = &sPageSlots[i];

    slot->page = page;
    slot->w = 512;
    slot->h = bytes / (512u * 2u);
    slot->loaded = slot->w * slot->h;
    slot->tex.data = VoxelArena_Alloc(&sPageArena, bytes, false);
    slot->tex.height = (uint16_t)slot->h;
    slot->used = sFrame;
    assert(slot->tex.data != NULL);
}

/* A window mask, drawn from this frame. */
static void Mask(unsigned i, unsigned bytes)
{
    BuildingPageSlot *slot = &sPageSlots[i];

    slot->glow.data = VoxelArena_Alloc(&sPageArena, bytes, false);
    slot->glowLoaded = bytes;
    slot->glowUsed = sFrame;
    assert(slot->glow.data != NULL);
}

/* The stream on this page: it stays where it is. */
static void Streamed(unsigned i)
{
    sStream.slot = &sPageSlots[i];
    sStream.state = 1;
}

static void Fragmented(void)
{
    Reset();
    Page(0, 0, 300u * K);
    assert(CtrVoxel_LendPagesGap(256u * K) != NULL);
    Page(1, 1, 300u * K);
    assert(VoxelArena_LargestGap(&sPageArena) == 232u * K);
}

static void Loans(void)
{
    void *block;

    Reset();
    assert(!CtrVoxel_PagesGapWanted());
    block = CtrVoxel_LendPagesGap(256u * K);
    assert(block != NULL && sPageArena.used == 256u * K);
    assert(CtrVoxel_LendPagesGap(64u * K) == NULL);
    assert(!CtrVoxel_PagesGapWanted());
    CtrVoxel_ReturnPagesGap(NULL);
    assert(sPagesLent == block);
    sParked = true;
    assert(CtrVoxel_PagesGapWanted());
    CtrVoxel_ReturnPagesGap(block);
    assert(sPagesLent == NULL && sPageArena.used == 0);
    assert(CtrVoxel_LendPagesGap(256u * K) == NULL);
    sParked = false;
    assert(CtrVoxel_LendPagesGap(2000u * K) == NULL);
}

static void Masks(void)
{
    BuildingPageSlot *slot;

    /* Room: no ask, the mask in. */
    Fragmented();
    slot = &sPageSlots[0];
    slot->tex.height = 128;
    slot->w = 512;
    GlowWant(slot);
    assert(slot->glow.data != NULL && !sPagesLentWanted);

    /* In the gap's way: asked, and looked at again right after. */
    Fragmented();
    slot = &sPageSlots[0];
    slot->tex.height = 480;
    slot->w = 512;
    GlowWant(slot);
    assert(slot->glow.data == NULL && sPagesLentWanted && slot->glowRetry == sFrame + VOXEL_PAGE_LENT_RETRY_FRAMES);
    CtrVoxel_ReturnPagesGap(sPagesLent);
    assert(!sPagesLentWanted);
    sFrame += VOXEL_PAGE_LENT_RETRY_FRAMES;
    GlowWant(slot);
    assert(slot->glow.data != NULL);

    /* Too large with the gap back as well: nothing asked, its usual wait. */
    Fragmented();
    slot = &sPageSlots[0];
    slot->tex.height = 1024;
    slot->w = 512;
    GlowWant(slot);
    assert(slot->glow.data == NULL && !sPagesLentWanted && slot->glowRetry == sFrame + VOXEL_PAGE_RETRY_FRAMES);
}

static void Atlases(void)
{
    C3D_Tex tex;

    /* Room for it: taken, the gap stays out. */
    Reset();
    Page(0, 0, 300u * K);
    assert(CtrVoxel_LendPagesGap(256u * K) != NULL);
    assert(AtlasPageBorrow(&tex) && !sPagesLentWanted);

    /* At their limit: no room is wanted. */
    sBorrowed = VOXEL_ATLAS_BORROW_MAX;
    assert(!AtlasPageBorrow(&tex) && !sPagesLentWanted);

    /* 232 KiB left, 256 wanted, and the gap would make them. */
    Fragmented();
    assert(!AtlasPageBorrow(&tex) && sPagesLentWanted);
}

static void AtlasWaits(void)
{
    VoxelAtlasSlot *slot = &sAtlases[0];

    /* The gap in the way: a wait, and the page is there once it is back. */
    Fragmented();
    assert(!AllocateAtlasPage(slot, 0, true) && sAtlasWaitsForGap && sPagesLentWanted);
    CtrVoxel_ReturnPagesGap(sPagesLent);
    assert(AllocateAtlasPage(slot, 0, true) && !sAtlasWaitsForGap && slot->tex.data != NULL);

    /* A wait holds for its own answer alone. */
    Fragmented();
    memset(slot, 0, sizeof(*slot));
    assert(!AllocateAtlasPage(slot, 0, true) && sAtlasWaitsForGap);
    assert(!AllocateAtlasPage(slot, 0, false) && !sAtlasWaitsForGap);

    /* The gap back would not do: a want of VRAM, as it was. */
    Reset();
    Page(0, 0, 500u * K);
    assert(CtrVoxel_LendPagesGap(100u * K) != NULL);
    Page(1, 1, 300u * K);
    assert(VoxelArena_LargestGap(&sPageArena) == 188u * K);
    assert(!AllocateAtlasPage(slot, 0, true) && !sAtlasWaitsForGap && !sPagesLentWanted);

    /* Borrowed all an atlas may: no room is wanted of the arena. */
    Fragmented();
    sBorrowed = VOXEL_ATLAS_BORROW_MAX;
    assert(!AllocateAtlasPage(slot, 0, true) && !sAtlasWaitsForGap && !sPagesLentWanted);

    /* Ahead of time: never as far as the arena. */
    Fragmented();
    assert(!AllocateAtlasPage(slot, 0, false) && !sAtlasWaitsForGap && !sPagesLentWanted);
}

static void Pages(void)
{
    BuildingPageSlot *victim = &sPageSlots[2];

    /* Room for it with the gap back. */
    Fragmented();
    assert(!PageRoomPossible(victim, 240u * K, 2, false));
    assert(PageWaitsForLentGap(victim, 5, 240u * K, 2, false));
    assert(sPagesLentWanted && sPageRetry[5] == sFrame + VOXEL_PAGE_LENT_RETRY_FRAMES);

    /* A way to it by sliding, with the gap gone: 256 + 232 KiB round a page. */
    Fragmented();
    assert(!PageRoomPossible(victim, 480u * K, 2, false));
    assert(PageWaitsForLentGap(victim, 5, 480u * K, 2, false));
    assert(sPagesLentWanted);

    /* More than the gap back could ever make. */
    Fragmented();
    assert(!PageWaitsForLentGap(victim, 5, 700u * K, 2, false));
    assert(!sPagesLentWanted && sPageRetry[5] == 0);

    /* Nothing lent. */
    Reset();
    Page(0, 0, 900u * K);
    assert(!PageWaitsForLentGap(victim, 5, 400u * K, 2, false));
    assert(!sPagesLentWanted);
}

/* 300 KiB of a page, the gap, 100 of a mask in view, a page the stream holds:
 * 132 KiB left at the top, 356 with the gap and the mask gone, no slide helps. */
static void GapBesideMask(void)
{
    Reset();
    Page(0, 0, 300u * K);
    assert(CtrVoxel_LendPagesGap(256u * K) != NULL);
    Mask(0, 100u * K);
    Page(1, 1, 300u * K);
    Streamed(1);
    assert(VoxelArena_LargestGap(&sPageArena) == 132u * K);
}

static void WantPageSteps(void)
{
    /* The gap and the masks in view together: the gap asked for, no error, and
     * the masks left alone until it is back. */
    GapBesideMask();
    sNewPageBytes = 340u * K;
    WantPage(5, VOXEL_PAGE_KEEP_VIEW, false);
    assert(sPagesLentWanted && sPageRetry[5] == sFrame + VOXEL_PAGE_LENT_RETRY_FRAMES);
    assert(sStats.errors == 0 && !sPageNoRoomLogged[5] && !sPageSlots[0].glowDropping);

    /* Back, the masks go as they would have, and the page finds its room. */
    CtrVoxel_ReturnPagesGap(sPagesLent);
    sFrame += VOXEL_PAGE_LENT_RETRY_FRAMES;
    sPageSlots[0].used = sPageSlots[1].used = sPageSlots[0].glowUsed = sFrame;
    WantPage(5, VOXEL_PAGE_KEEP_VIEW, false);
    assert(sPageSlots[0].glowDropping && sPageRetry[5] == sFrame + 3u && !sPagesLentWanted);
    sFrame += 3u;
    sPageSlots[0].used = sPageSlots[1].used = sFrame;
    WantPage(5, VOXEL_PAGE_KEEP_VIEW, false);
    assert(FindPage(5) != NULL && sStats.errors == 0);

    /* The masks alone would do: they go, and the gap stays out. */
    Reset();
    Page(0, 0, 200u * K);
    assert(CtrVoxel_LendPagesGap(256u * K) != NULL);
    Page(1, 1, 200u * K);
    Mask(1, 200u * K);
    Streamed(1);
    sNewPageBytes = 400u * K;
    WantPage(5, VOXEL_PAGE_KEEP_VIEW, false);
    assert(!sPagesLentWanted && sPageSlots[1].glowDropping && sPageRetry[5] == sFrame + 3u);

    /* More than both could make: a page with no room, the gap left out. */
    GapBesideMask();
    sNewPageBytes = 700u * K;
    WantPage(5, VOXEL_PAGE_KEEP_VIEW, false);
    assert(!sPagesLentWanted && sStats.errors == 1 && sPageNoRoomLogged[5]);
    assert(sPageRetry[5] == sFrame + VOXEL_PAGE_RETRY_FRAMES);

    /* Not drawn from: its usual wait, and nothing asked at either step. */
    GapBesideMask();
    sNewPageBytes = 340u * K;
    WantPage(5, VOXEL_PAGE_KEEP_VIEW, true);
    assert(!sPagesLentWanted && sStats.errors == 0);
    assert(sPageRingRetry[5] == sFrame + VOXEL_PAGE_QUIET_RETRY_FRAMES);
}

int main(void)
{
    /* By hand: MinGW's C library has no aligned_alloc. */
    uint8_t *memory = malloc(1088u * K + 4096u);

    assert(memory != NULL);
    sMemory = (uint8_t *)(((uintptr_t)memory + 4095u) & ~(uintptr_t)4095u);
    Loans();
    Masks();
    Atlases();
    AtlasWaits();
    Pages();
    WantPageSteps();
    puts("PASS pages gap: one loan at a time, none over a parked world, nothing asked while there is room or when "
         "the gap back would not do, masks and atlas pages ask, an atlas page waits for it, pages ask before anything "
         "slides or goes, and once more with the masks in view");
    return 0;
}
'''
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='voxel-gap-', dir=ROOT / 'build') as temp:
        temp = Path(temp)
        path = temp / 'test.c'
        path.write_text(source, encoding='utf-8')
        exe = temp / 'test.exe'
        subprocess.run([args.cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-Isrc/voxel',
                        str(path), str(ROOT / 'src/voxel/voxel_arena.c'), '-o', str(exe)],
                       check=True, cwd=ROOT)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
