#ifndef CTR_SAVE_IO_H
#define CTR_SAVE_IO_H

#include <stdbool.h>
#include <stddef.h>

/* Filenames relative to CTR_DATA_DIR. The caller owns recovery and validation. */
bool CtrSave_Replace(const char *name, const char *temp, const char *backup,
                     const void *data, size_t size);

/* Write only the requested flash range and preserve any appended trailer.
 * Update the RAM mirror only after all file operations succeed. */
bool CtrFlash_WriteRange(const char *path, void *flash, size_t flashSize, size_t trailerMax,
                        size_t offset, const void *data, size_t size);

#endif
