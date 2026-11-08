"""Window edges past 255 on the 400-pixel screen, end to end on the host:
WIN_RANGE as a wide unit sees it (include/gba/io_reg.h), the game's own
SetGpuReg (src/gpu_regs.c), and the compositor's reading of the registers
(WindowEdge and WindowOver, taken from src/3ds_video.c)."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from video_text import read_video

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent


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
    video = read_video()
    source = r'''
#include "global.h"
#include "gpu_regs.h"
#include <stdbool.h>
#include <stdio.h>
unsigned char REG_BASE[0x400];
static unsigned Reg(unsigned offset) { return *(u16 *)(REG_BASE + offset); }
static unsigned Min(unsigned a, unsigned b) { return a < b ? a : b; }
''' + function(video, 'static int WindowEdge(') + function(video, 'static unsigned WindowOver(') + r'''
static int sFails, sCases;

static void Read(const char *what, int x0, int x1)
{
    unsigned across = Reg(REG_OFFSET_WIN0H), over = WindowOver(0, across);
    int a = WindowEdge(across, over, false, DISPLAY_WIDTH), b = WindowEdge(across, over, true, DISPLAY_WIDTH);

    ++sCases;
    if (a != x0 || b != x1)
    {
        printf("FAIL %s: %d..%d, expected %d..%d\n", what, a, b, x0, x1);
        ++sFails;
    }
}

static void Check(const char *what, u32 value, int x0, int x1)
{
    SetGpuReg(REG_OFFSET_WIN0H, value);
    CopyBufferedValuesToGpuRegs();
    Read(what, x0, x1);
}

int main(void)
{
    InitGpuRegManager();
    REG_DISPCNT = DISPCNT_FORCED_BLANK;   /* every write lands at once */
    Check("option menu", WIN_RANGE(16, DISPLAY_WIDTH - 16), 16, 384);
    Check("battle factory", WIN_RANGE(88, DISPLAY_WIDTH - 88), 88, 312);
    Check("PC opening", WIN_RANGE(120, 280), 120, 280);
    Check("whole width", WIN_RANGE(0, DISPLAY_WIDTH), 0, 400);
    Check("union room chat", WIN_RANGE(64, DISPLAY_WIDTH), 64, 400);
    Check("left edge past 255", WIN_RANGE(300, 390), 300, 390);
    Check("narrow", WIN_RANGE(88, 96), 88, 96);
    /* The GBA meanings stay: 255 the edge of the screen, both saturated empty. */
    Check("overworld 0x00FF", 0xFF, 0, 400);
    Check("show mon 0..255", WIN_RANGE(0, 255), 0, 400);
    Check("closed 0xFFFF", 0xFFFF, 255, 255);
    /* A value cut to u16 reads as the saturated GBA one. */
    {
        u16 cut = WIN_RANGE(16, 384);
        Check("cut to u16", cut, 16, 400);
    }
    /* Kept in an s16 (field_effect.c's tWinHoriz), a left edge of 128 or more
     * arrives sign-extended: still the GBA reading, no edges past 255. */
    {
        s16 kept = WIN_RANGE(239, 255);
        Check("s16, sign-extended", kept, 239, 400);
        kept = WIN_RANGE(255, 255);
        Check("s16, closed", kept, 255, 255);
    }
    /* A write that bypasses SetGpuReg (a DMA, a raw store) drops the edges
     * past 255 a wide write left beside the register. */
    SetGpuReg(REG_OFFSET_WIN0H, WIN_RANGE(16, 384));
    CopyBufferedValuesToGpuRegs();
    *(u16 *)(REG_BASE + REG_OFFSET_WIN0H) = WIN_RANGE(16, 200);
    Read("raw write after a wide one", 16, 200);
    if (sFails)
        return 1;
    printf("PASS wide windows: %d cases (edges past 255, GBA meanings, cut values, raw writes)\n", sCases);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'wide_window_test.c'
        binary = Path(tmp) / 'wide_window_test'
        path.write_text(source, encoding='utf-8')
        subprocess.run([args.cc, '-std=gnu99', '-w', '-DPLATFORM_3DS', '-DPORTABLE', '-DMODERN=1',
                        '-iquote', str(REPO / 'include'), '-iquote', str(REPO / 'include/constants'),
                        '-iquote', str(REPO), str(path), str(REPO / 'src/gpu_regs.c'), '-o', str(binary)],
                       check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
