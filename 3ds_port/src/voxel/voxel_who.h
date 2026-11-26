#ifndef VOXEL_WHO_H
#define VOXEL_WHO_H

/*
 * Which thread asks: 0 the render thread, 1 the chunk build's worker
 * (ctr_voxel.c, BuildWorker). The modules a build reads keep the caches and
 * memos they fill while answering one a thread (`static T sXOf[VOXEL_THREADS]`
 * with `#define sX (sXOf[VOXEL_WHO])`), so the worker builds beside the frame
 * without either reading what the other is half-way through writing. What
 * only the render thread writes (the instances, the sun, the tables read at
 * start-up) stays one, and the worker is called home before it changes
 * (JobAwayHome).
 *
 * Off the console there is one thread and one copy.
 */
#ifdef PLATFORM_3DS
#define VOXEL_THREADS 2
#include <stdint.h>
/*
 * Told by the stack the caller runs on - the worker's is one block, set down
 * by the worker as it starts - rather than by a thread-local variable: that
 * is a call (__aeabi_read_tp) at every use, and these are read in the
 * lighting's and the relief's innermost lookups, on both threads.
 */
extern uintptr_t gVoxelWorkerStack, gVoxelWorkerStackSize;
static inline unsigned VoxelWho(void)
{
    uintptr_t sp;

    __asm__("mov %0, sp" : "=r"(sp));
    return sp - gVoxelWorkerStack < gVoxelWorkerStackSize;
}
#define VOXEL_WHO (VoxelWho())
#else
#define VOXEL_THREADS 1
#define VOXEL_WHO 0
#endif

#endif
