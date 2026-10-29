"""The door after a warp into the voxel field (#80), on the host, with the
game's own palette engine: TransferPlttBuffer, UpdatePaletteFade,
BeginNormalPaletteFade, UpdateNormalPaletteFade, IsSoftwarePaletteFadeFinishing
and ResetPaletteFadeControl taken from build/upstream/src/palette.c as the
port builds it (PORT_BRIDGE), with include/palette.h, and the port's hold
(FieldFadeHold, Port_HoldPaletteFade, HoldFieldFade) from
src/3ds_game_full.c. Run frame by frame as the field runs them - the port's
wait after the present, the VBlank's TransferPlttBuffer, CB2's
UpdatePaletteFade - with field_screen_effect.c's Task_ExitDoor reduced to
what it waits for (the fade over). Checked: under PORT_BRIDGE
bufferTransferDisabled does not hold a fade (why the hold is a delay gate);
the door waits for the first picture, which comes in black and fades in;
without the hold the door was done behind the black (the report); a black
that never ends lets go at the presented-frame bound; hidden frames at
2x/4x/16x pacing retain the hold; a fade begun while held still blends
its first step; the 2D field, a fade out and leaving the field are left
alone."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
DEFAULT_TREE = REPO if (REPO / 'src/palette.c').is_file() else REPO / 'build/upstream'


def function(source, signature):
    start = 0
    while True:
        start = source.index(signature, start)
        if source[start + len(signature):].lstrip().startswith('{'):
            break
        start += len(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--tree', type=Path, default=DEFAULT_TREE, help='patched pokeemerald tree')
    args = parser.parse_args()
    upstream = args.tree
    game = (ROOT / 'src/3ds_game_full.c').read_text(encoding='utf-8')
    palette = (upstream / 'src/palette.c').read_text(encoding='utf-8')
    bound = re.search(r'^#define FIELD_FADE_HOLD_MAX (\S+)', game, re.M).group(1)
    source = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef uint8_t bool8;
typedef uint16_t bool16;
#define TRUE 1
#define FALSE 0
#define ALIGNED(n) __attribute__((aligned(n)))
#define PLTT_SIZE 0x400
#define PLTT_SIZEOF(n) ((n) * sizeof(u16))
#define PORT_BRIDGE 1
#include "palette.h"
#include "3ds_video.h"

/* What palette.c has around the functions taken from it. */
enum { NORMAL_FADE, FAST_FADE, HARDWARE_FADE };
static u16 sPltt[PLTT_SIZE / 2];
#define PLTT sPltt
#define DmaCopy16(channel, src, dest, size) memcpy((dest), (src), (size))
#define CpuCopy32(src, dest, size) memcpy((dest), (src), (size))
ALIGNED(4) u16 gPlttBufferUnfaded[PLTT_BUFFER_ALLOC];
ALIGNED(4) u16 gPlttBufferFaded[PLTT_BUFFER_ALLOC];
struct PaletteFadeControl gPaletteFade;
static u32 sPlttBufferTransferPending;
static bool8 sPortFadeStarting;
static u8 UpdateNormalPaletteFade(void);
static bool8 IsSoftwarePaletteFadeFinishing(void);
static u8 UpdateFastPaletteFade(void) { return PALETTE_FADE_STATUS_DONE; }
static u8 UpdateHardwarePaletteFade(void) { return PALETTE_FADE_STATUS_DONE; }
static void UpdateBlendRegisters(void) {}
/* util.c's BlendPalette: the unfaded colours `coeff`/16 of the way to `color`. */
static void BlendPalette(u16 offset, u16 count, u8 coeff, u32 color)
{
    for (u16 i = 0; i < count; ++i)
    {
        u16 c = gPlttBufferUnfaded[offset + i];
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        int tr = color & 31, tg = (color >> 5) & 31, tb = (color >> 10) & 31;

        r += ((tr - r) * coeff) >> 4;
        g += ((tg - g) * coeff) >> 4;
        b += ((tb - b) * coeff) >> 4;
        gPlttBufferFaded[offset + i] = (u16)(r | g << 5 | b << 10);
    }
}
'''
    for signature in ('void TransferPlttBuffer(void)', 'u8 UpdatePaletteFade(void)',
                      'bool8 BeginNormalPaletteFade(u32 selectedPalettes, s8 delay, u8 startY, u8 targetY, u16 blendColor)', 'void ResetPaletteFadeControl(void)',
                      'static u8 UpdateNormalPaletteFade(void)', 'static bool8 IsSoftwarePaletteFadeFinishing(void)'):
        source += function(palette, signature)
    source += r'''
/* What 3ds_game_full.c has around the functions taken from it. */
#define CTR_LOG_VIDEO 0
#define CtrLog_Write(...) ((void)0)
#define FIELD_FADE_HOLD_MAX ''' + bound + r'''
static CtrVideoStats sVideo;
static bool sOverworld = true;
const CtrVideoStats *CtrVideo_GetStats(void) { return &sVideo; }
static bool CtrGame_IsOverworld(void) { return sOverworld; }
static u32 sFrames;
static bool sFadeHeld;
static u32 sFadeHeldFrames;
static u32 sFadeLastVideoFrame;
''' + function(game, 'static bool FieldFadeHold(bool blankField, bool fadingIn, u32 heldFrames)') + function(game, 'bool8 Port_HoldPaletteFade(void)') \
        + function(game, 'static void HoldFieldFade(void)') + r'''
static int sFails, sCases;

static void Expect(bool ok, const char *what, int got, int want)
{
    ++sCases;
    if (!ok) { printf("FAIL %s: %d (expected %d)\n", what, got, want); ++sFails; }
}

/* The field's colours, all white; the previous map faded out to black. */
static void Arrive(void)
{
    for (unsigned i = 0; i < PLTT_BUFFER_ALLOC; ++i)
    {
        gPlttBufferUnfaded[i] = 0x7fff;
        gPlttBufferFaded[i] = 0;
    }
    memset(sPltt, 0, sizeof(sPltt));
    ResetPaletteFadeControl();
    sPlttBufferTransferPending = 0;
    sFadeHeld = false;
    sFadeHeldFrames = 0;
    sFadeLastVideoFrame = sVideo.frames;
    sOverworld = true;
}

/*
 * A warp: the fade in begun (FadeScreen's BeginNormalPaletteFade), then
 * `black` frames of the voxel world's black (-1: without end) and its
 * picture. Returns the frame the door opened, -1 if not by `frames`; what
 * PLTT showed on the first picture.
 */
static int Warp(int black, int frames, bool hold, unsigned mode, bool transferOff, int *shownAtPicture)
{
    int door = -1;

    Arrive();
    BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, 0);
    if (transferOff) gPaletteFade.bufferTransferDisabled = TRUE;
    *shownAtPicture = -1;
    for (int frame = 0; frame < frames; ++frame)
    {
        bool blank = black < 0 || frame < black;

        /* CtrVideo_Present, then the port's wait (CtrGame_WaitFrame). */
        sVideo.mode = (uint8_t)mode;
        sVideo.route = blank ? CTR_ROUTE_BLANK : CTR_ROUTE_VOXEL;
        if (!blank && *shownAtPicture < 0) *shownAtPicture = sPltt[1];
        ++sVideo.frames;
        if (hold) HoldFieldFade();
        ++sFrames;
        /* The game's frame: the field's VBlank, then CB2_Overworld. */
        TransferPlttBuffer();
        UpdatePaletteFade();
        /* Task_ExitDoor: the door opens once the fade is over. */
        if (door < 0 && !gPaletteFade.active) door = frame;
    }
    return door;
}

/* EndFrame presents only selected logical frames (FrameShown). The
 * voxel warm-up advances only in CtrVoxel_Update during those presents. */
static void PacedWarp(unsigned logicalPerPicture)
{
    unsigned pictures = 0;
    bool openedInBlack = false;
    int firstPicture = -1;

    Arrive();
    BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, 0);
    for (unsigned logical = 0; logical < 220 * logicalPerPicture; ++logical)
    {
        if (logical % logicalPerPicture == 0)
        {
            ++pictures;
            sVideo.mode = CTR_MODE_FIELD_VOXEL;
            sVideo.route = pictures <= 180 ? CTR_ROUTE_BLANK : CTR_ROUTE_VOXEL;
            if (pictures == 181) firstPicture = sPltt[1];
            ++sVideo.frames;
        }
        HoldFieldFade();
        TransferPlttBuffer();
        UpdatePaletteFade();
        if (pictures <= 180 && !gPaletteFade.active) openedInBlack = true;
    }
    Expect(!openedInBlack, "paced door waits for voxel warm-up", openedInBlack, 0);
    Expect(firstPicture == 0, "paced first picture remains black", firstPicture, 0);
    Expect(!gPaletteFade.active, "paced fade completes after warm-up", gPaletteFade.active, 0);

    /* A genuinely stuck renderer still releases after 240 presented frames,
     * even with arbitrarily many hidden logical frames between presents. */
    Arrive();
    sVideo.route = CTR_ROUTE_BLANK;
    BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, 0);
    for (unsigned picture = 0; picture < FIELD_FADE_HOLD_MAX; ++picture)
    {
        ++sVideo.frames;
        HoldFieldFade();
        for (unsigned hidden = 0; hidden < logicalPerPicture + 10; ++hidden)
        {
            /* Below the bound hidden frames must retain the hold. */
            HoldFieldFade();
            if (picture + 1 < FIELD_FADE_HOLD_MAX && !Port_HoldPaletteFade())
                openedInBlack = true;
            TransferPlttBuffer();
            UpdatePaletteFade();
        }
    }
    Expect(!openedInBlack, "hidden frames do not exhaust hold budget", openedInBlack, 0);
    HoldFieldFade();
    Expect(!Port_HoldPaletteFade(), "stuck renderer reaches presented-frame bound", Port_HoldPaletteFade(), 0);
}

int main(void)
{
    int shown, door;

    PacedWarp(2); /* real-time catch-up / 2x */
    PacedWarp(4); /* 4x fast-forward */
    PacedWarp(16); /* long hidden-frame runs */

    /* Under PORT_BRIDGE the transfer switch holds the colours, not the
     * fade: the door is done in the black regardless. */
    door = Warp(120, 400, false, CTR_MODE_FIELD_VOXEL, true, &shown);
    Expect(door >= 0 && door < 120, "bufferTransferDisabled does not hold the fade", door, 120);
    gPaletteFade.bufferTransferDisabled = FALSE;
    /* The report: 120 frames of black against the fade. */
    door = Warp(120, 400, false, CTR_MODE_FIELD_VOXEL, false, &shown);
    Expect(door >= 0 && door < 120, "without the hold the door is done in the black", door, 120);
    door = Warp(120, 400, true, CTR_MODE_FIELD_VOXEL, false, &shown);
    Expect(door >= 120, "the door waits for the first picture", door, 120);
    Expect(shown == 0, "the world comes in black and fades in", shown, 0);
    Expect(door >= 0 && door < 160, "and the fade then runs as before", door, 160);
    Expect(sPltt[1] == 0x7fff, "faded all the way in", sPltt[1], 0x7fff);
    Expect(!sFadeHeld && !Port_HoldPaletteFade(), "nothing held after", sFadeHeld, 0);

    /* A black that never ends: let go at the bound, the fade then runs. */
    door = Warp(-1, FIELD_FADE_HOLD_MAX + 60, true, CTR_MODE_FIELD_VOXEL, false, &shown);
    Expect(door >= (int)FIELD_FADE_HOLD_MAX && door < (int)FIELD_FADE_HOLD_MAX + 60,
           "a black without end is let go at the bound", door, FIELD_FADE_HOLD_MAX);

    /* The 2D field is never held. */
    door = Warp(120, 400, true, CTR_MODE_FIELD, false, &shown);
    Expect(door >= 0 && door < 120, "the 2D field is not held", door, 120);

    /* A fade out in the black is not held. */
    Arrive();
    for (unsigned i = 0; i < PLTT_BUFFER_ALLOC; ++i) gPlttBufferFaded[i] = 0x7fff;
    sVideo.mode = CTR_MODE_FIELD_VOXEL;
    sVideo.route = CTR_ROUTE_BLANK;
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, 0);
    for (int frame = 0; frame < 60 && gPaletteFade.active; ++frame)
    {
        HoldFieldFade();
        TransferPlttBuffer();
        UpdatePaletteFade();
    }
    Expect(!gPaletteFade.active && !sFadeHeld, "a fade out", gPaletteFade.active, 0);

    /* Held, the field is left (a battle at once) and a fade in begins in
     * the same frame: its first step is still blended and shown. */
    Arrive();
    sVideo.route = CTR_ROUTE_BLANK;
    BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, 0);
    HoldFieldFade();
    Expect(sFadeHeld, "held", sFadeHeld, 1);
    ResetPaletteFadeControl();
    for (unsigned i = 0; i < PLTT_BUFFER_ALLOC; ++i) gPlttBufferFaded[i] = 0x7fff;
    BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, 0);
    Expect(sPltt[1] == 0, "a fade begun while held blends its start", sPltt[1], 0);
    sOverworld = false;
    HoldFieldFade();
    Expect(!sFadeHeld && !Port_HoldPaletteFade(), "leaving the field lets go", sFadeHeld, 0);

    if (sFails)
        return 1;
    printf("PASS field fade hold: %d cases (palette.c's gate, door after the first picture, bounded)\n", sCases);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'field_fade_hold_test.c'
        binary = Path(tmp) / 'field_fade_hold_test'
        path.write_text(source, encoding='utf-8')
        subprocess.run([args.cc, '-std=gnu99', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
                        '-iquote', str(upstream / 'include'), '-iquote', str(ROOT / 'include'),
                        str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
