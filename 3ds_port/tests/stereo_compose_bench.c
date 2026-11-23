/* Synthetic CPU-only throughput comparison. No ROM, device SDK, or GPU.
 * Diagnostic: fail on any byte difference, not on noisy wall-clock ratios. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "3ds_stereo_mon.h"

#define DIM CTR_STEREO_MON_DIM
#define TEX_W 128
#define TEX_H 512
#define GROUPS 4
#define ROUNDS 24

static uint8_t packed[DIM * DIM / 2];
static uint8_t groupMap[DIM * DIM];
static uint16_t picture[DIM * DIM];
static uint16_t reference[TEX_W * TEX_H];
static uint16_t candidate[TEX_W * TEX_H];
static volatile uint32_t check;

static void Setup(void)
{
    uint32_t r = 0x68a13u;
    for (unsigned i = 0; i < sizeof(packed); ++i)
    {
        r = r * 1664525u + 1013904223u;
        unsigned lo = (r >> 14) % 5, hi = (r >> 21) % 5;
        packed[i] = (uint8_t)((lo == 4 ? 15 : lo) | ((hi == 4 ? 15 : hi) << 4));
    }
    for (unsigned i = 0; i < DIM * DIM; ++i)
    {
        r = r * 1664525u + 1013904223u;
        picture[i] = (r % 9 == 0) ? 0 : (uint16_t)(0x8000u | (r & 0x7fffu));
    }
}

static void OldCompose(const CtrStereoMonHeader *h)
{
    for (unsigned g = 0; g < GROUPS; ++g)
        CtrStereoMon_ComposeTile(picture, packed, h, 0, GROUPS, g, reference, TEX_W,
                                 (g % 2) * DIM, (g / 2) * DIM, 2);
}

static void NewCompose(const CtrStereoMonHeader *h)
{
    CtrStereoMon_GroupFrame(packed, h, 0, GROUPS, groupMap);
    for (unsigned g = 0; g < GROUPS; ++g)
        CtrStereoMon_ComposeGroupedTile(picture, groupMap, g, candidate, TEX_W,
                                        (g % 2) * DIM, (g / 2) * DIM, 2);
}

int main(void)
{
    CtrStereoMonHeader h = {.count = 1, .planes = 4, .frames = 1,
                            .width = DIM, .height = DIM, .entryBytes = sizeof(packed)};
    Setup();
    memset(reference, 0xa5, sizeof(reference));
    memset(candidate, 0xa5, sizeof(candidate));
    OldCompose(&h);
    NewCompose(&h);
    if (memcmp(reference, candidate, sizeof(reference)) != 0)
    {
        fputs("stereo_compose_bench: byte mismatch\n", stderr);
        return 1;
    }

    clock_t before = clock();
    for (unsigned i = 0; i < ROUNDS; ++i)
    {
        OldCompose(&h);
        check += reference[(i * 91u) % (TEX_W * TEX_H)];
    }
    clock_t middle = clock();
    for (unsigned i = 0; i < ROUNDS; ++i)
    {
        NewCompose(&h);
        check += candidate[(i * 91u) % (TEX_W * TEX_H)];
    }
    clock_t after = clock();
    if (memcmp(reference, candidate, sizeof(reference)) != 0)
    {
        fputs("stereo_compose_bench: post-benchmark byte mismatch\n", stderr);
        return 1;
    }
    double old_ms = (double)(middle - before) * 1000.0 / CLOCKS_PER_SEC / ROUNDS;
    double new_ms = (double)(after - middle) * 1000.0 / CLOCKS_PER_SEC / ROUNDS;
    printf("stereo_compose_bench: PASS (%u rounds, 4 groups, 2px skirt)\n", ROUNDS);
    printf("Host CPU per sprite: reference %.3f ms; grouped %.3f ms; relative %.2fx [not 3DS FPS]\n",
           old_ms, new_ms, new_ms > 0.0 ? old_ms / new_ms : 0.0);
    return 0;
}
