/* emerald3ds.pak parsing; see include/3ds_pak.h. */

#include <string.h>

#include "3ds_pak.h"

/*
 * CRC-32 (zlib's), four bytes a step ("slicing by 4"): every payload read
 * from the pack is checked, on the game's thread too, and a byte at a time
 * was some 9 cycles a byte on an ARM11. The read-only tables are ready
 * before any thread reads a payload.
 */
#include "3ds_pak_crc_table.h"

uint32_t CtrPak_Crc32(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *p = data;

    crc = ~crc;
    /* A byte at a time up to a word's boundary, then a word at a time. */
    for (; size != 0 && ((uintptr_t)p & 3) != 0; --size)
        crc = sCrcTable[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    for (; size >= 4; size -= 4, p += 4)
    {
        uint32_t word;

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        memcpy(&word, p, 4);
#else
        word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
#endif
        word ^= crc;
        crc = sCrcTable[3][word & 0xFF] ^ sCrcTable[2][(word >> 8) & 0xFF]
            ^ sCrcTable[1][(word >> 16) & 0xFF] ^ sCrcTable[0][word >> 24];
    }
    for (; size != 0; --size)
        crc = sCrcTable[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* FNV-1a, 64 bit, over the path exactly as given ("maps/layouts.bin"). */
uint64_t CtrPak_PathId(const char *path)
{
    uint64_t hash = 0xcbf29ce484222325ull;

    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
    {
        hash ^= *p;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

static uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t Le64(const uint8_t *p)
{
    return (uint64_t)Le32(p) | ((uint64_t)Le32(p + 4) << 32);
}

CtrPakStatus CtrPak_ParseHeader(const uint8_t raw[CTR_PAK_HEADER_BYTES], uint32_t engineAbi,
                                const uint8_t romSha1[20], uint64_t fileSize, CtrPakHeader *out)
{
    if (memcmp(raw, "EM3DPAK\0", 8) != 0)
        return CTR_PAK_BAD_MAGIC;
    if (CtrPak_Crc32(0, raw, 60) != Le32(raw + 60))
        return CTR_PAK_BAD_HEADER_CRC;
    out->schema = Le32(raw + 8);
    out->engineAbi = Le32(raw + 12);
    memcpy(out->romSha1, raw + 16, 20);
    out->entryCount = Le32(raw + 36);
    out->indexOffset = Le64(raw + 40);
    out->dataOffset = Le64(raw + 48);
    out->indexCrc = Le32(raw + 56);
    if (out->schema != CTR_PAK_SCHEMA_VERSION)
        return CTR_PAK_BAD_SCHEMA;
    if (out->engineAbi != engineAbi)
        return CTR_PAK_BAD_ABI;
    if (memcmp(out->romSha1, romSha1, 20) != 0)
        return CTR_PAK_BAD_ROM;
    if (out->entryCount == 0 || out->entryCount > (1u << 20))
        return CTR_PAK_BAD_COUNT;
    if (out->indexOffset < CTR_PAK_HEADER_BYTES || out->indexOffset > out->dataOffset
     || (uint64_t)out->entryCount * CTR_PAK_ENTRY_BYTES > out->dataOffset - out->indexOffset)
        return CTR_PAK_BAD_INDEX;
    if (out->indexOffset > fileSize
     || (uint64_t)out->entryCount * CTR_PAK_ENTRY_BYTES > fileSize - out->indexOffset)
        return CTR_PAK_TRUNCATED_INDEX;
    if (out->dataOffset > fileSize)
        return CTR_PAK_BAD_INDEX;
    return CTR_PAK_OK;
}

CtrPakStatus CtrPak_ParseIndex(const uint8_t *raw, const CtrPakHeader *header, uint64_t fileSize, CtrPakEntry *out)
{
    if (CtrPak_Crc32(0, raw, (size_t)header->entryCount * CTR_PAK_ENTRY_BYTES) != header->indexCrc)
        return CTR_PAK_BAD_INDEX_CRC;
    for (uint32_t i = 0; i < header->entryCount; ++i)
    {
        const uint8_t *e = raw + (size_t)i * CTR_PAK_ENTRY_BYTES;
        CtrPakEntry *entry = &out[i];

        entry->id = Le64(e);
        entry->type = Le32(e + 8);
        entry->flags = Le32(e + 12);
        entry->offset = Le64(e + 16);
        entry->storedSize = Le32(e + 24);
        entry->rawSize = Le32(e + 28);
        entry->crc32 = Le32(e + 32);
        entry->reserved = Le32(e + 36);
        if ((i != 0 && entry->id <= out[i - 1].id) || (entry->flags & CTR_PAK_FLAG_COMPRESSED)
         || entry->storedSize != entry->rawSize || entry->offset < header->dataOffset
         || entry->offset > fileSize || entry->storedSize > fileSize - entry->offset)
            return CTR_PAK_BAD_INDEX;
    }
    return CTR_PAK_OK;
}

const CtrPakEntry *CtrPak_Find(const CtrPakEntry *entries, uint32_t count, const char *path)
{
    uint64_t id = CtrPak_PathId(path);
    uint32_t lo = 0, hi = count;

    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        if (entries[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < count && entries[lo].id == id ? &entries[lo] : NULL;
}
