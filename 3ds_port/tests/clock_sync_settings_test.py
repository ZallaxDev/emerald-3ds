"""Run clock sync scenarios with the production settings setter's update hook."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from voxel_runtime_test import ROOT, function


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = (ROOT / "tests/daynight_test.c").read_text()
    settings = (ROOT / "src/3ds_settings.c").read_text()
    # Retain the real read-before-write setter. Only the SD persistence and
    # unrelated settings table are replaced; the complete clock suite runs.
    source = source.replace(
        "static int sEnabled, sSpeed, sShowClock, sTimedEncounters;",
        "static int sEnabled, sShowClock, sTimedEncounters;\n"
        "static struct { int value; } sExtra[1];\n"
        "#define sSpeed sExtra[0].value")
    setter = function(settings, "void CtrSettings_SetInt(")
    stubs = r'''
#define CTR_LOG_FS 0
static int ExtraIndex(const char *key, bool create)
{
    (void)create;
    assert(strcmp(key, "clock_speed") == 0);
    return 0;
}
static void Save(void) { ++sSetCalls; }
static void CtrLog_Write(int category, const char *format, ...)
{
    (void)category;
    (void)format;
}
'''
    source = source.replace(function(source, "void CtrSettings_SetInt("), stubs + setter)
    with tempfile.TemporaryDirectory(prefix="clock-sync-settings-") as directory:
        harness = Path(directory) / "test.c"
        executable = Path(directory) / "test"
        harness.write_text(source)
        subprocess.run([args.cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "include"), str(harness),
                        str(ROOT / "src/3ds_daynight.c"), "-lm", "-o", str(executable)],
                       check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS clock sync: production settings setter, RTC failure, berry credit, resume and unshifted restore")


if __name__ == "__main__":
    main()
