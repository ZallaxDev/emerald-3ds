"""The Portuguese title logo: DrawLogo (src/3ds_icons_ptbr.c), built on the host,
writes the table sLogo into the title screen logo's tiles, pixel for pixel."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <stdio.h>
void Port_TransformAsset(const char *path, unsigned char *data, unsigned size);
const unsigned char *Port_LatinGlyph(unsigned font, unsigned char code, int *width)
{
    (void)font;
    (void)code;
    *width = 0;
    return NULL;
}
/* Into a file opened "wb": Windows' text-mode stdout turns every 0x0A
 * (palette index 10) into 0x0D 0x0A. */
int main(int argc, char **argv)
{
    static unsigned char d[4096];
    FILE *out;

    if (argc != 2 || (out = fopen(argv[1], "wb")) == NULL)
        return 2;
    Port_TransformAsset("graphics/title_screen/emerald_version.8bpp", d, sizeof(d));
    if (fwrite(d, 1, sizeof(d), out) != sizeof(d) || fclose(out) != 0)
        return 1;
    return 0;
}
'''


def tile_offset(x, y):
    """Two 64x32 sprites of 8bpp tiles (gbagfx -mwidth 8 -mheight 4)."""
    return ((x // 64) * 32 + (y // 8) * 8 + (x % 64) // 8) * 64 + (y % 8) * 8 + x % 8


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('logo_ptbr', ROOT.parent / 'tools/logo_ptbr.py')
    logo_ptbr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(logo_ptbr)
    want = logo_ptbr.table(ROOT / 'src/3ds_icons_ptbr.c')

    with tempfile.TemporaryDirectory() as tmp:
        harness = Path(tmp) / 'harness.c'
        harness.write_text(HARNESS)
        exe = Path(tmp) / 'logo_test'
        subprocess.run([args.cc, '-std=gnu99', '-O2', '-Wall', '-Wextra', '-Werror', '-DCTR_LANG_PT_BR=1',
                        '-I' + str(ROOT / 'include'), str(harness), str(ROOT / 'src/3ds_icons_ptbr.c'),
                        '-lm', '-o', str(exe)], check=True)
        tiles = Path(tmp) / 'logo.8bpp'
        subprocess.run([str(exe), str(tiles)], check=True)
        out = tiles.read_bytes()

    wrong = [(x, y) for y in range(32) for x in range(128) if out[tile_offset(x, y)] != want[y][x]]
    if wrong:
        sys.exit('logo_ptbr_test: DrawLogo differs from sLogo at %d pixels, e.g. %s' % (len(wrong), wrong[:5]))
    print('PASS logo pt_br: DrawLogo writes sLogo into the logo tiles pixel for pixel')


if __name__ == '__main__':
    main()
