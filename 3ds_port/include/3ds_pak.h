#ifndef CTR_PAK_H
#define CTR_PAK_H

/*
 * emerald3ds.pak parsing, free of any SDK so the host tests can run it.
 * The format is documented in builder/emerald3ds_builder/pak.py, which writes
 * it; 3ds_data.c does the file I/O around these functions.
 */

#include <stddef.h>
#include <stdint.h>

#define CTR_PAK_HEADER_BYTES 64u
#define CTR_PAK_ENTRY_BYTES 40u
#define CTR_PAK_SCHEMA_VERSION 1u
#define CTR_PAK_FLAG_COMPRESSED 1u

typedef struct
{
    uint32_t schema, engineAbi, entryCount, indexCrc;
    uint8_t romSha1[20];
    uint64_t indexOffset, dataOffset;
} CtrPakHeader;

typedef struct
{
    uint64_t id;
    uint32_t type, flags;
    uint64_t offset;
    uint32_t storedSize, rawSize, crc32, reserved;
} CtrPakEntry;

typedef enum
{
    CTR_PAK_OK,
    CTR_PAK_BAD_MAGIC,
    CTR_PAK_BAD_HEADER_CRC,
    CTR_PAK_BAD_SCHEMA,
    CTR_PAK_BAD_ABI,
    CTR_PAK_BAD_ROM,
    CTR_PAK_BAD_COUNT,
    CTR_PAK_TRUNCATED_INDEX,
    CTR_PAK_BAD_INDEX_CRC,
    CTR_PAK_BAD_INDEX,
} CtrPakStatus;

uint32_t CtrPak_Crc32(uint32_t crc, const void *data, size_t size);
uint64_t CtrPak_PathId(const char *path);
/* Magic, header CRC, schema, ABI and ROM, in that order of precedence. The
 * header fields are filled even when the ABI or ROM check fails, so the
 * caller can report both ABIs. */
CtrPakStatus CtrPak_ParseHeader(const uint8_t raw[CTR_PAK_HEADER_BYTES], uint32_t engineAbi,
                                const uint8_t romSha1[20], uint64_t fileSize, CtrPakHeader *out);
/* `raw` holds header->entryCount entries; checks the CRC and the order. */
CtrPakStatus CtrPak_ParseIndex(const uint8_t *raw, const CtrPakHeader *header, uint64_t fileSize, CtrPakEntry *out);
const CtrPakEntry *CtrPak_Find(const CtrPakEntry *entries, uint32_t count, const char *path);

#endif
