#ifndef CTR_DATA_H
#define CTR_DATA_H

/*
 * Game data access.
 *
 * Everything derived from the game itself (graphics, maps, scripts, songs,
 * linked game tables, voxel data) is read through this interface, never with
 * fopen. Three backends serve the same relative paths ("maps/layouts.bin"):
 *
 *   romfs  the executable's own RomFS, in a development build that embeds
 *          its data;
 *   loose  sdmc:/3ds/emerald3ds/devdata/<path>, for iterating on one
 *          generated file without rebuilding anything else;
 *   pak    sdmc:/3ds/emerald3ds/emerald3ds.pak, the single data pack the
 *          Emerald 3DS Builder generates from the player's own ROM.
 *
 * Files that only describe the executable (asset index, relocation tables,
 * shaders, the engine ABI) are not game data and are always read from the
 * RomFS with CtrFs_OpenAsset.
 *
 * SDK-free: game translation units include this header.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CTR_DATA_DIR "sdmc:/3ds/emerald3ds/"
#define CTR_DATA_PAK_PATH CTR_DATA_DIR "emerald3ds.pak"
#define CTR_DATA_LOOSE_DIR CTR_DATA_DIR "devdata/"
#define CTR_DATA_LOOSE_MARKER CTR_DATA_LOOSE_DIR ".emerald3ds-dev"
/* Present in the RomFS of a build that embeds its game data. */
#define CTR_DATA_EMBEDDED_MARKER "romfs:/data.embedded"

/* Pack format, see docs/ASSET_PIPELINE.md and builder/emerald3ds_builder/pak.py. */
#define CTR_PAK_MAGIC "EM3DPAK"
#define CTR_PAK_SCHEMA 1

typedef enum
{
    CTR_DATA_NONE,
    CTR_DATA_ROMFS,
    CTR_DATA_LOOSE,
    CTR_DATA_PAK,
} CtrDataBackend;

/* Chooses and validates the backend. On failure CtrData_ErrorTitle and
 * CtrData_ErrorDetail describe what the player has to do. */
bool CtrData_Init(void);
const char *CtrData_BackendName(void);
const char *CtrData_ErrorTitle(void);
const char *CtrData_ErrorDetail(void);

/* A read-only stdio stream over one data file, or NULL if it does not exist.
 * fseek/ftell/fread/fclose work as on any file. Safe to call from several
 * threads; each stream must stay on the thread that opened it. */
FILE *CtrData_Open(const char *path);
bool CtrData_Exists(const char *path);
bool CtrData_Size(const char *path, uint32_t *outSize);
/* The whole file in a malloc'd buffer (one extra zero byte past the end), or
 * NULL. The caller frees it. */
void *CtrData_Load(const char *path, uint32_t *outSize);
/* A file CtrData_Load will soon be asked for, read ahead on a worker so that
 * the read is not in the frame that wants it (a few, small: sprites). */
void CtrData_Prefetch(const char *path);
/* Where a file lies in the data pack, so that neighbouring files can be read
 * in one go with CtrData_ReadRange. False under any other backend. */
bool CtrData_Locate(const char *path, uint64_t *outOffset, uint32_t *outSize);
bool CtrData_ReadRange(uint64_t offset, void *dest, uint32_t size);

#endif
