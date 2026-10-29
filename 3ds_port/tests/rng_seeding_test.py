"""Exercise the production seed source and boot/new-game call sites on the host."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature + '\n{')
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--source-tree', type=Path,
                        help='bootstrapped upstream tree with production patches applied')
    args = parser.parse_args()
    code = (ROOT / 'src/3ds_game_full.c').read_text()
    seed_fn = function(code, 'u16 Port_RngSeed(u16 fallback)')
    source_tree = args.source_tree or ROOT.parent
    if not (source_tree / 'src/new_game.c').is_file():
        parser.error('run from a bootstrapped tree, or pass --source-tree PATH '
                     'containing the applied production patches')
    main_src = (source_tree / 'src/main.c').read_text()
    new_game_src = (source_tree / 'src/new_game.c').read_text()
    agb = function(main_src, 'void AgbMain(void)')
    begin = agb.index('#if defined(PLATFORM_3DS) && defined(PORT_BRIDGE)')
    end = agb.index('#endif', begin) + len('#endif')
    callsite = agb[begin:end]
    trainer = function(new_game_src, 'static void InitPlayerTrainerId(void)')
    new_game = function(new_game_src, 'void NewGameInitData(void)')
    reseed_fn = function(code, 'void Port_ReseedForNewGame(void)')
    assert 'SeedRng(Port_RngSeed(0))' in callsite
    assert 'Port_ReseedForNewGame();' in trainer
    assert 'InitPlayerTrainerId();' in new_game
    assert agb.index('InitMainCallbacks();') < begin < agb.index('ClearDma3Requests();')
    # Soft reset re-enters the same AgbMain after restoring the initial game data.
    soft = function(code, 'void CtrGame_SoftReset(void)')
    init = function(code, 'void CtrGame_Init(void)')
    assert 'longjmp(sAgbMainEntry, 1)' in soft
    assert init.index('setjmp(sAgbMainEntry)') < init.index('AgbMain();')
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint8_t u8;
static bool scenario;
static uint64_t ticks;
static int64_t wall;
static unsigned reads, calls, rtcCalls;
static u16 lastSeed;
static u32 rng, trainerId;
static unsigned randomCalls;
static struct { u8 playerTrainerId[4]; } saveBlock2;
#define gSaveBlock2Ptr (&saveBlock2)
static bool CtrScenario_Active(void) { return scenario; }
static uint64_t CtrPlatform_Ticks(void) { ++reads; return ticks; }
static int64_t CtrPlatform_WallSeconds(void) { ++reads; return wall; }
static void SeedRng(u16 seed) { ++calls; lastSeed = seed; rng = seed; }
static u16 Random(void)
{
    ++randomCalls;
    rng = rng * UINT32_C(1103515245) + UINT32_C(24691);
    return (u16)(rng >> 16);
}
#ifndef PORTABLE
static u16 GetGeneratedTrainerIdLower(void) { return 0x3456; }
#endif
static void SetTrainerId(u32 id, u8 *dst)
{
    trainerId = id;
    for (unsigned i = 0; i < 4; ++i) dst[i] = (u8)(id >> (8 * i));
}
#ifdef BUGFIX
void SeedRngWithRtc(void) { ++rtcCalls; }
#endif
'''
    harness += seed_fn + '\n' + reseed_fn + '\nstatic void Boot(void)\n{\n' + callsite + '\n}\n' + trainer
    harness += r'''
int main(void)
{
    unsigned distinct = 0;
    bool seen[65536] = {0};
    wall = 1792240000;
    ticks = UINT64_C(0x123456789abcdef0);
    u16 initial = Port_RngSeed(0);
    assert(reads == 2);
    assert(Port_RngSeed(0) == initial); /* injected sources are reproducible */
    for (unsigned i = 0; i < 4096; ++i)
    {
        ticks = UINT64_C(0x123456789abcdef0) + i;
        u16 seed = Port_RngSeed(0);
        if (!seen[seed]) { seen[seed] = true; ++distinct; }
    }
    assert(distinct > 3800); /* not the same seed for equal-frame resets */
    ticks = 0;
    wall = 0;
    (void)Port_RngSeed(0); /* zero/unset wall clock is still a defined input */
    wall = INT64_MIN;
    ticks = UINT64_MAX;
    (void)Port_RngSeed(0); /* unsigned mixing has no signed overflow */
    scenario = true;
    reads = 0;
    assert(Port_RngSeed(0) == 0);
    assert(Port_RngSeed(0xabcd) == 0xabcd);
    assert(reads == 0); /* record/play never consult live entropy */
    scenario = false;
    ticks = 12345;
    wall = 1792240000;
    initial = Port_RngSeed(0);
    calls = rtcCalls = 0;
    Boot();
#if defined(PLATFORM_3DS) && defined(PORT_BRIDGE)
    assert(calls == 1 && rtcCalls == 0 && lastSeed == initial);
#elif defined(BUGFIX)
    assert(calls == 0 && rtcCalls == 1);
#else
    assert(calls == 0 && rtcCalls == 0);
#endif
    rng = UINT32_C(0xabcdef12);
    calls = randomCalls = reads = 0;
    InitPlayerTrainerId();
#if defined(PLATFORM_3DS) && defined(PORT_BRIDGE) && defined(PORTABLE)
    assert(calls == 1 && lastSeed == initial && reads == 2);
    u32 liveExpected = initial;
    for (unsigned i = 0; i < 2; ++i)
        liveExpected = liveExpected * UINT32_C(1103515245) + UINT32_C(24691);
    assert(rng == liveExpected); /* reseed precedes both trainer-ID draws */
#else
    assert(calls == 0 && reads == 0);
#endif
#ifdef PORTABLE
    assert(randomCalls == 2);
#else
    assert(randomCalls == 1 && (trainerId & 0xffff) == 0x3456);
#endif
    assert(saveBlock2.playerTrainerId[0] == (u8)trainerId);
    assert(saveBlock2.playerTrainerId[3] == (u8)(trainerId >> 24));
    /* Starting another new game really samples fresh timing at the portable
     * trainer-ID call, even if the RNG had already advanced since boot. */
    ticks += UINT64_C(1234567);
    u16 next = Port_RngSeed(0);
    calls = reads = 0;
    InitPlayerTrainerId();
#if defined(PLATFORM_3DS) && defined(PORT_BRIDGE) && defined(PORTABLE)
    assert(calls == 1 && reads == 2 && lastSeed == next);
#else
    (void)next;
    assert(calls == 0 && reads == 0);
#endif
    scenario = true;
    reads = calls = rtcCalls = 0;
    Boot();
#if defined(PLATFORM_3DS) && defined(PORT_BRIDGE)
    assert(calls == 1 && lastSeed == 0 && rtcCalls == 0);
#elif defined(BUGFIX)
    assert(calls == 0 && rtcCalls == 1);
#else
    assert(calls == 0 && rtcCalls == 0);
#endif
    /* Scenarios must continue their prior 32-bit state, not reseed from a
     * truncated/zero fallback. Test the actual portable production function. */
    rng = UINT32_C(0xabcdef12);
    calls = randomCalls = reads = 0;
    (void)Random();
#ifdef PORTABLE
    (void)Random();
#endif
    u32 expectedState = rng;
    rng = UINT32_C(0xabcdef12);
    randomCalls = 0;
    InitPlayerTrainerId();
    assert(calls == 0 && reads == 0 && rng == expectedState);
#ifdef PORTABLE
    assert(randomCalls == 2);
#else
    assert(randomCalls == 1);
#endif
    puts("PASS RNG seed: production boot/new-game, scenario stream, variation, bounds");
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / 'seed.c'
        source.write_text(harness)
        variants = [[], ['-DBUGFIX'], ['-DPLATFORM_3DS', '-DPORT_BRIDGE'],
                    ['-DPLATFORM_3DS', '-DPORT_BRIDGE', '-DBUGFIX']]
        for flags in variants + [v + ['-DPORTABLE'] for v in variants]:
            binary = Path(tmp) / 'seed'
            subprocess.run([args.cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                            *flags, str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
