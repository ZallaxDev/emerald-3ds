#ifndef LR_SNAPSHOT_H
#define LR_SNAPSHOT_H
#include <stddef.h>
#include "lr_runtime.h"

size_t LrSnapshotBytes(const LrWorldState *world);
/* Buffers, world, and written must occupy separate storage. */
bool LrSnapshotWrite(const LrWorldState *world, uint8_t *buffer, size_t capacity, size_t *written);
bool LrSnapshotRestore(LrWorldState *world, const uint8_t *buffer, size_t length, uint32_t contentVersion);
#endif
