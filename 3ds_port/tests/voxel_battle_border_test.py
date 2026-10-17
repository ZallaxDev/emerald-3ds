"""Run production battle cell/ground sampling against the emitted water fixture.

Only game behavior predicates are stubbed; the ReadCell/StageGround bodies and
shared mesh/cache code are compiled unchanged. No SDK, ROM or generated assets.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/voxel/voxel_battle.c').read_text()
# Keep the complete production cell definitions and functions, not copies.
cells = source[source.index('enum\n{\n    GROUND_PLAIN'):source.index('static const StageCell *CellAt')]
# The grid itself is only used by the stage search, outside this test.
cells = cells[:cells.index('static StageCell sGrid')] + cells[cells.index('static uint8_t KindOf'):]
assert '*ground = StageGround(sStage.bestX, sStage.bestZ);' in source
prefix = r'''
#define VoxelRelief_LiftAt TestStageLiftAt
#define main GeometryMain
#include "voxel_water_border_test.c"
#undef main
float TestStageLiftAt(float x, float z)
{
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt((int)floorf(x), (int)floorf(z));
    return TestRelief_Base(inst) + TestRelief_CellLift(inst, (int)floorf(x), (int)floorf(z));
}
unsigned VoxelWorld_GetMetatileBehavior(int x, int z)
{
    return Inside(x, z) && sWater[z][x] ? 3u : 0u;
}
unsigned VoxelWorld_GetCollision(int x, int z) { (void)x; (void)z; return 0; }
static bool MetatileBehavior_IsTallGrass(unsigned b) { return b == 1; }
static bool MetatileBehavior_IsLongGrass(unsigned b) { return b == 1; }
static bool MetatileBehavior_IsSandOrDeepSand(unsigned b) { return b == 2; }
static bool MetatileBehavior_IsSurfableWaterOrUnderwater(unsigned b) { return b == 3; }
static bool MetatileBehavior_IsDeepOrOceanWater(unsigned b) { return b == 3; }
'''
suffix = r'''
static void CheckCell(int x, int z, unsigned kind, float height, float top, bool indoor)
{
    StageCell c;
    ReadCell(&c, x, z, indoor);
    assert(c.kind == kind && fabsf(c.lift - height) < 0.0001f && c.top == top);
    assert(!c.slope);
}
int main(void)
{
    float height = 123.0f;
    GeometryMain();
    Reset(100);
    sWater[H - 1][2] = true;
    sLift[H - 1][2] = -0.5f;
    sBase = 1.25f;
    Build();
    sBorder.base = sBase; /* like the production border chunk builder */
    sBorder.count = 0;
    VoxelMesh_EmitBorder(&sBorder, -3, -3, W + 3, H + 3);
    CheckCell(2, H, GROUND_WATER, 0.75f, 0.0f, false);
    assert(StageGround(2.5f, H + 0.5f) == 0.75f);
    assert(Covered(2.5f, 0.75f, H + 0.5f));
    /* Connected maps always retain their own classification/height. */
    sConnected = true;
    CheckCell(W, 2, GROUND_PLAIN, 0.0f, 0.0f, false);
    assert(!VoxelMesh_BorderWaterAt(W + 0.5f, 2.5f, &height));
    assert(StageGround(W + 0.5f, 2.5f) == 0.0f);
    CheckCell(2, H - 1, GROUND_WATER, 0.75f, 0.0f, false);
    assert(StageGround(2.5f, H - 0.5f) == 0.75f);
    /* Stood ground, indoor void and a drawn stood belt are unchanged. */
    sStoodWestOf = 1000;
    CheckCell(2, H, GROUND_PLAIN, 0.0f, 2.0f, false);
    CheckCell(2, H, GROUND_VOID, 0.0f, 0.0f, true);
    assert(StageGround(2.5f, H + 0.5f) == 0.0f);
    sStoodWestOf = -1000;
    sBelt = 3;
    CheckCell(2, H, GROUND_PLAIN, 0.0f, 2.0f, false);
    sBelt = 0;
    sMap.indoor = true;
    assert(!VoxelMesh_BorderWaterAt(2.5f, H + 0.5f, &height));
    /* A source water/stood split displaced south/north: query the source
     * tile under the rendered point, not the unshifted border pattern. */
    for (int sign = -1; sign <= 1; sign += 2)
    {
        Reset(110 + sign);
        sDrawn = true;
        sWater[H - 1][2] = true;
        sLift[H - 1][2] = (float)sign;
        sBorderSplitY = true;
        sStoodWestOf = 0;
        sWaterBefore = true; /* source north of row 0 is water */
        Build();
        assert(VoxelMesh_BorderWaterAt(-0.5f, -0.5f + sign, &height));
        assert(height == (float)sign);
        assert(Covered(-0.5f, height, -0.5f + sign));
        assert(!VoxelMesh_BorderWaterAt(-0.5f, 0.5f + sign, &height));
        CheckCell(-1, sign - 1, GROUND_WATER, (float)sign, 0.0f, false);
        assert(StageGround(-0.5f, -0.5f + sign) == (float)sign);
    }
    puts("PASS battle water border: production cells/target ground match emitted plane, "
         "base height, both shifts, connected maps, stood ground and indoor void");
    return 0;
}
'''
sources = ['voxel_tree.c', 'voxel_sign.c', 'voxel_building.c', 'voxel_relief.c', 'voxel_grade.c']
with tempfile.TemporaryDirectory(prefix='voxel-battle-border-') as tmp:
    cfile = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    cfile.write_text(prefix + cells + suffix)
    command = shlex.split(os.environ.get('HOSTCC', 'cc')) + [
        '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-DPORT_LOG(...)=((void)0)', '-DVOXEL_HOST_FILES',
        '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'src/voxel'),
        '-I' + str(ROOT / 'tests'), str(cfile),
    ] + [str(ROOT / 'src/voxel' / s) for s in sources] + [
        str(ROOT / 'src/3ds_video_decode.c'), '-lm', '-o', str(exe),
    ]
    command += shlex.split(os.environ.get('BORDER_TEST_CFLAGS', ''))
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True, cwd=ROOT)
