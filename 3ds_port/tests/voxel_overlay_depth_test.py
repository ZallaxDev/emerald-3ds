"""The voxel field's text in 3D (#82), on the host: the overlay's per-eye
shift (CtrVoxel_OverlayParallax, StereoNearestDepth and the stereo pair's
StereoParallax/StereoIod/CameraTanX, taken from src/voxel/ctr_voxel.c) is
checked against a brute-force walk of the world the picture can hold - every
row and column of the screen down to the ground, up to the height the bound
names - so that no point of it comes out in front of the text, and the shift
is no more than a whole pixel past the nearest one. One picture and the
slider down keep the old placement (0)."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    """The definition of `signature`, skipping forward declarations."""
    start = 0
    while True:
        start = source.index(signature, start)
        after = source[start + len(signature):].lstrip()
        if after.startswith('{'):
            break
        start += len(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def define(source, name):
    match = re.search(r'^#define %s (\S+)' % name, source, re.M)
    if match is None:
        raise SystemExit('missing #define %s' % name)
    return '#define %s %s\n' % (name, match.group(1))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    voxel = (ROOT / 'src/voxel/ctr_voxel.c').read_text(encoding='utf-8')
    source = r'''
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include "voxel_camera.h"
#define CTR_GAME_WIDTH 400
#define CTR_GAME_HEIGHT 240
#define C3D_AngleFromDegrees(d) ((d) * (float)M_PI / 180.0f)
'''
    for name in ('VOXEL_NEAR', 'VOXEL_STEREO_FAR_PX', 'VOXEL_STEREO_FOCUS', 'VOXEL_STEREO_NEAR_LIFT'):
        source += define(voxel, name)
    source += r'''
static VoxelCamera sCamera;
static float sStereoSlider;
static int sDrawEye = -1;
static bool sReady = true;
float CtrVoxel_OverlayParallax(void);
'''
    for signature in ('static float CameraTanX(void)', 'static float StereoIod(float *screen)',
                      'static float StereoParallax(int eye, float depth)',
                      'static float StereoNearestDepth(float drop, float forwardY, float upY, float tanY)',
                      'float CtrVoxel_OverlayParallax(void)'):
        source += function(voxel, signature)
    source += r'''
static int sFails, sCases;

static void Expect(bool ok, const char *what, float got, float want)
{
    ++sCases;
    if (!ok)
    {
        printf("FAIL %s: %.4f (expected %.4f)\n", what, got, want);
        ++sFails;
    }
}

static void Place(float height, float back, float fov)
{
    sCamera.targetX = 10.5f;
    sCamera.targetZ = 20.5f;
    sCamera.targetY = sCamera.ground = 3.0f;
    sCamera.x = sCamera.targetX;
    sCamera.y = sCamera.ground + height;
    sCamera.z = sCamera.targetZ + back;
    sCamera.fov = fov;
}

static float Shift(int eye, float slider)
{
    sDrawEye = eye;
    sStereoSlider = slider;
    return CtrVoxel_OverlayParallax();
}

/* The nearest parallax, in the left eye, of anything on screen standing on
 * the player's ground up to the bound's height: every ray of a grid over the
 * screen, met with planes from the ground up. Its view depth is the ray's
 * parameter, the forward part of each ray being 1. */
static float NearestParallax(void)
{
    float f[3] = {sCamera.targetX - sCamera.x, sCamera.targetY - sCamera.y, sCamera.targetZ - sCamera.z};
    float n = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]), r[3], u[3], rn;
    float tanY = tanf(C3D_AngleFromDegrees(sCamera.fov) * 0.5f), tanX = tanY * 400.0f / 240.0f;
    float best = -1e9f;

    for (int k = 0; k < 3; ++k) f[k] /= n;
    /* right = forward x world up, up = right x forward (Mtx_LookAt). */
    r[0] = -f[2]; r[1] = 0.0f; r[2] = f[0];
    rn = sqrtf(r[0] * r[0] + r[2] * r[2]);
    r[0] /= rn; r[2] /= rn;
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    sDrawEye = 0;
    for (int row = 0; row <= 240; ++row)
        for (int column = 0; column <= 400; column += 8)
        {
            float sy = 1.0f - 2.0f * row / 240.0f, sx = 2.0f * column / 400.0f - 1.0f;
            float dy = f[1] + sy * tanY * u[1] + sx * tanX * r[1];

            if (dy >= 0.0f) continue;
            for (int h = 0; h <= 16; ++h)
            {
                float height = sCamera.ground + VOXEL_STEREO_NEAR_LIFT * h / 16.0f;
                float t = (height - sCamera.y) / dy, p;

                if (t < VOXEL_NEAR) continue;
                p = StereoParallax(0, t);
                if (p > best) best = p;
            }
        }
    return best;
}

int main(void)
{
    /* One picture, or the slider down: on the glass as before. */
    Place(8.0f, 8.0f, 30.0f);
    Expect(Shift(-1, 1.0f) == 0.0f, "middle camera", Shift(-1, 1.0f), 0.0f);
    Expect(Shift(0, 0.0f) == 0.0f, "slider down, left", Shift(0, 0.0f), 0.0f);
    Expect(Shift(1, 0.0f) == 0.0f, "slider down, right", Shift(1, 0.0f), 0.0f);
    sReady = false;
    Expect(Shift(0, 1.0f) == 0.0f, "voxel module off", Shift(0, 1.0f), 0.0f);
    sReady = true;

    /* Cameras of the field: high and low, steep and shallow, at every depth
     * OPTIONS' 3D DEPTH gives the slider (0.25 to 2). */
    static const float cameras[][3] = {
        {8.0f, 8.0f, 30.0f}, {4.5f, 9.0f, 30.0f}, {12.0f, 6.0f, 35.0f}, {6.0f, 14.0f, 25.0f}, {10.0f, 10.0f, 40.0f},
    };
    static const float sliders[] = {0.05f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f};
    for (unsigned c = 0; c < sizeof(cameras) / sizeof(cameras[0]); ++c)
    {
        float last = 0.0f;

        Place(cameras[c][0], cameras[c][1], cameras[c][2]);
        for (unsigned s = 0; s < sizeof(sliders) / sizeof(sliders[0]); ++s)
        {
            float left = Shift(0, sliders[s]), right = Shift(1, sliders[s]), nearest;
            char what[96];

            sStereoSlider = sliders[s];
            nearest = NearestParallax();
            snprintf(what, sizeof(what), "camera %u slider %.2f", c, sliders[s]);
            /* The world does come out of the glass there: the old 0 was
             * behind it, which is the report. */
            Expect(nearest > 0.0f, what, nearest, 0.0f);
            /* In front of all of it, and by less than a pixel more. */
            Expect(left >= nearest - 1e-4f, what, left, nearest);
            Expect(left < nearest + 1.0f, what, left, nearest + 1.0f);
            /* Whole pixels, the same both ways, never shrinking as the depth grows. */
            Expect(left == floorf(left) && right == -left, what, right, -left);
            Expect(left >= last, what, left, last);
            last = left;
        }
    }

    /* A camera that does not look down bounds nothing: left on the glass. */
    Place(4.0f, 8.0f, 30.0f);
    sCamera.targetY = sCamera.y + 6.0f;
    Expect(Shift(0, 1.0f) == 0.0f, "camera looking up", Shift(0, 1.0f), 0.0f);
    /* An eye no higher than the bound's height: the near plane bounds it. */
    Place(VOXEL_STEREO_NEAR_LIFT, 8.0f, 30.0f);
    {
        float want;

        sDrawEye = 0;
        sStereoSlider = 1.0f;
        want = ceilf(StereoParallax(0, VOXEL_NEAR));
        Expect(Shift(0, 1.0f) == want, "eye at the bound's height", Shift(0, 1.0f), want);
    }
    if (sFails)
        return 1;
    printf("PASS voxel overlay depth: %d cases (in front of the nearest world, whole pixels, flat at 0)\n", sCases);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'voxel_overlay_depth_test.c'
        binary = Path(tmp) / 'voxel_overlay_depth_test'
        path.write_text(source, encoding='utf-8')
        subprocess.run([args.cc, '-std=gnu99', '-Wall', '-Wno-unused-function', '-I', str(ROOT / 'src/voxel'),
                        str(path), '-lm', '-o', str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
