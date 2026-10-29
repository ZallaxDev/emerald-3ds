"""Exercise the staged game's berry-update functions with a controllable clock."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from voxel_runtime_test import function, ROOT


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    clock = (args.tree / "src/clock.c").read_text()
    berry = (args.tree / "src/berry.c").read_text()
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "3ds_daynight.h"
#define PLATFORM_3DS 1
#define BERRY_TREES_COUNT 128
#define BERRY_STAGE_BERRIES 5
typedef int32_t s32;
struct Time { int days, hours, minutes, seconds; };
struct BerryTree {
    uint16_t berry, stage, minutesUntilNextStage, berryYield, water;
    bool stopGrowth;
};
static struct { struct BerryTree berryTrees[BERRY_TREES_COUNT]; } sSave1;
static struct { struct Time lastBerryTreeUpdate; } sSave2;
static const struct BerryTree gBlankBerryTree = {0};
#define gSaveBlock1Ptr (&sSave1)
#define gSaveBlock2Ptr (&sSave2)
static uint64_t sNow;
static int sSpeed;
static unsigned sLocal;
uint64_t CtrPlatform_Milliseconds(void) { return sNow; }
int64_t CtrPlatform_WallSeconds(void) { return 1700000000; }
bool CtrGame_IsOutdoor(void) { return true; }
bool CtrGame_LocalTimeSeconds(unsigned *seconds) { *seconds = sLocal; return true; }
int CtrSettings_GetInt(const char *key, int fallback) {
    if (!strcmp(key, "clock_speed")) return sSpeed;
    if (!strcmp(key, "show_clock")) return 1;
    return fallback;
}
void CtrSettings_SetInt(const char *key, int value) {
    if (!strcmp(key, "clock_speed")) sSpeed = value;
}
static int TotalSeconds(const struct Time *time) {
    return time->days * 86400 + time->hours * 3600 + time->minutes * 60 + time->seconds;
}
static struct Time TimeAt(int seconds) {
    return (struct Time){seconds / 86400, seconds / 3600 % 24, seconds / 60 % 60, seconds % 60};
}
static void CalcTimeDifference(struct Time *difference, const struct Time *start, const struct Time *end) {
    *difference = TimeAt(TotalSeconds(end) - TotalSeconds(start));
}
static uint16_t GetStageDurationByBerryType(unsigned berry) { (void)berry; return 240; }
static bool BerryTreeGrow(struct BerryTree *tree) { ++tree->stage; return true; }
'''
    source += function(berry, "void BerryTreeTimeUpdate(")
    source += function(clock, "static void UpdatePerMinute(struct Time *localTime)\n{")
    source += r'''
static void Begin(int speed, int realStart) {
    CtrDayNight_Reset();
    memset(&sSave1, 0, sizeof(sSave1));
    sSave1.berryTrees[0] = (struct BerryTree){1, 1, 240, 0, 3, false};
    sSave1.berryTrees[1] = (struct BerryTree){1, 1, 240, 0, 2, true};
    sSave2.lastBerryTreeUpdate = TimeAt(realStart);
    sNow = 0; sSpeed = speed; sLocal = realStart % 86400;
    CtrDayNight_Update();
}
static void Tick(int seconds, int realStart) {
    sNow = (uint64_t)seconds * 1000;
    sLocal = (realStart + seconds) % 86400;
    struct Time time = TimeAt(realStart + seconds);
    UpdatePerMinute(&time);
    UpdatePerMinute(&time);
}
int main(void) {
    const unsigned rates[] = {1, 6, 12, 24, 60, 120};
    for (unsigned index = 0; index < 6; ++index) {
        Begin(index, 86370);
        for (int second = 1; second <= 120; ++second) Tick(second, 86370);
        struct BerryTree actual = sSave1.berryTrees[0];
        sSave1.berryTrees[0] = (struct BerryTree){1, 1, 240, 0, 3, false};
        BerryTreeTimeUpdate(2 * rates[index]);
        assert(!memcmp(&actual, &sSave1.berryTrees[0], sizeof(actual)));
        assert(sSave1.berryTrees[1].stage == 1 && sSave1.berryTrees[1].minutesUntilNextStage == 240);
        assert(TotalSeconds(&sSave2.lastBerryTreeUpdate) == 86490);
        assert(actual.water == 3);
    }
    Begin(5, 0);
    for (int second = 1; second <= 60; ++second) Tick(second, 0);
    assert(sSave1.berryTrees[0].minutesUntilNextStage == 120);
    sSpeed = 0;
    for (int second = 61; second <= 120; ++second) Tick(second, 0);
    assert(sSave1.berryTrees[0].minutesUntilNextStage == 119);
    Begin(5, 0);
    sNow = 1000;
    sSave2.lastBerryTreeUpdate = TimeAt(3600);
    struct Time backwards = TimeAt(0);
    UpdatePerMinute(&backwards);
    assert(sSave1.berryTrees[0].minutesUntilNextStage == 240);
    assert(CtrDayNight_BerryDue());
    sSave2.lastBerryTreeUpdate = backwards;
    UpdatePerMinute(&backwards);
    assert(sSave1.berryTrees[0].minutesUntilNextStage == 239);
    Begin(5, 0);
    sNow = 86400000;
    struct Time tomorrow = TimeAt(86400);
    UpdatePerMinute(&tomorrow);
    assert(sSave1.berryTrees[0].berry == 0);
    assert(sSave1.berryTrees[1].berry == 1);
    puts("PASS native berry clock: six rates, midnight, 4-hour stage in 2 real minutes at 120X, no double credit, watering, stopped trees, speed change, backwards RTC, expiry");
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(temporary)
        path = folder / "berry_clock.c"
        binary = folder / "berry_clock"
        path.write_text(source)
        subprocess.run([args.cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "include"), str(path),
                        str(ROOT / "src/3ds_daynight.c"), "-lm", "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
