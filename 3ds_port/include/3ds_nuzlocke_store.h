#ifndef CTR_NUZLOCKE_STORE_H
#define CTR_NUZLOCKE_STORE_H

/*
 * The Nuzlocke tracker's record on the SD card (emerald3ds.nuzlocke, with a
 * .tmp and .bak like the clock's), bound to the save image by its CRC. SDK-free.
 */
#include <stdbool.h>
#include <stddef.h>
#include "3ds_nuzlocke_core.h"

/* *state is this save's record when MATCHED, reset otherwise. */
CtrNuzLink CtrNuzStore_Load(const void *save, size_t size, CtrNuzState *state);
/* Never touches the save; false and the old record kept when it cannot write. */
bool CtrNuzStore_Save(const CtrNuzState *state, const void *save, size_t size);

#endif
