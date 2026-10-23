#ifndef CTR_GPUMEM_H
#define CTR_GPUMEM_H
#include <stdbool.h>
#include <stdint.h>

/*
 * Who holds the GPU's memory (3ds_gpumem.c). Every VRAM and linear-heap
 * allocation goes through the linker's --wrap, and is charged to the code that
 * asked for it: the caller of C3D_TexInit or C3D_RenderTargetCreate when it came
 * through Citro3D, else the allocator's own caller. Sites are code addresses;
 * scripts/perf_report.py names them from the ELF.
 */
typedef struct
{
    uint32_t live, peak, count, failures;
} CtrGpuPool;

typedef struct
{
    CtrGpuPool vram, linear;
    uint32_t untracked;
} CtrGpuMemStats;

/* The largest VRAM block an allocation could still get, found by trying. */
uint32_t CtrGpuMem_LargestVram(void);
/* Totals, then every site holding memory or changed since the last report. */
void CtrGpuMem_Report(const char *why);

#endif
