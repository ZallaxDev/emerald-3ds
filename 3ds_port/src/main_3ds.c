#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_platform.h"
#include "3ds_video.h"
/* libctru's supported heap override. Citro3D flushes the entire linear heap
 * at FrameEnd(0); the default 32 MiB needlessly penalizes Old 3DS. The tile
 * atlas, geometry and command buffers and LCD buffers fit within 8 MiB. */
uint32_t __ctru_linear_heap_size = 8 * 1024 * 1024;

/*
 * libctru's own __system_allocateHeaps, except that the application heap is
 * clamped to the heap area. libctru gives the heap everything the linear heap
 * does not take; with the linear heap at 8 MiB, on a New 3DS that is ~100 MiB,
 * more than the 96 MiB between OS_HEAP_AREA_BEGIN and OS_HEAP_AREA_END, and
 * svcControlMemory fails before main (svcBreak in __libctru_init). An Old 3DS
 * has too little memory to reach the limit, so nothing changes there.
 */
extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;

void __system_allocateHeaps(void)
{
    Handle limit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    s64 max = 0, used = 0;
    u32 remaining;

    if (R_SUCCEEDED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE)))
    {
        svcGetResourceLimitLimitValues(&max, limit, &type, 1);
        svcGetResourceLimitCurrentValues(&used, limit, &type, 1);
        svcCloseHandle(limit);
    }
    remaining = (u32)(max - used) & ~0xFFFu;
    if (__ctru_linear_heap_size == 0)
        __ctru_linear_heap_size = (remaining / 2) & ~0xFFFu;
    if (__ctru_linear_heap_size >= remaining)
        svcBreak(USERBREAK_PANIC);
    __ctru_heap_size = remaining - __ctru_linear_heap_size;
    if (__ctru_heap_size > OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN)
        __ctru_heap_size = OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN;

    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size,
                                  MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE)))
        svcBreak(USERBREAK_PANIC);
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size,
                                  MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE)))
        svcBreak(USERBREAK_PANIC);
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

/*
 * Started by the HOME Menu forwarder (3ds_port/forwarder): it made its own
 * title Luma3DS's 3DSX title to load this file, and passes the previous one.
 * Put it back, so the forwarder runs its own code next time; Luma's PM applies
 * the "selected" title when this process exits. A 3DSX loaded by hb:ldr may
 * write the shared page.
 */
#define FORWARDER_RESTORE_ARG "emerald3ds-forwarder:hbldr-tid="

extern int __system_argc;
extern char **__system_argv;

static void RestoreLumaHbldrTitle(void)
{
    const size_t prefixLen = sizeof(FORWARDER_RESTORE_ARG) - 1;
    for (int i = 1; i < __system_argc; i++)
    {
        const char *arg = __system_argv[i];
        if (arg == NULL || strncmp(arg, FORWARDER_RESTORE_ARG, prefixLen) != 0)
            continue;
        char *end;
        u64 tid = strtoull(arg + prefixLen, &end, 16);
        s64 luma;
        if (*end == 0 && tid != 0 && R_SUCCEEDED(svcGetSystemInfo(&luma, 0x10000, 0)))
            *(volatile u64 *)(OS_SHAREDCFG_VADDR + 0x808) = tid;
    }
}

int main(void)
{
    RestoreLumaHbldrTitle();
    if (!CtrPlatform_Init())
        CtrPlatform_Fatal("platform initialization failed");
    CtrPlatformHooks hooks = {0};
    hooks.vblank = CtrGame_VBlank;
    hooks.videoPresent = CtrVideo_Present;
    CtrPlatform_SetHooks(&hooks);
    /* The game owns the loop: CtrGame_Init enters AgbMain and
     * returns only through the platform's own exit path. */
    if (!CtrPlatform_BeginFrame())
        return 0;
    CtrGame_Init();

    CtrPlatform_Shutdown();
    return 0;
}
