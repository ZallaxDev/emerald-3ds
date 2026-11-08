/*
 * Host test for the console's pack parser against a pack written by the
 * builder (tests/make_test_pak.py): lookup, payload CRCs, and every rejection
 * path a damaged or mismatched pack must take.
 *
 *   pak_test FILE ABI_HEX
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_pak.h"

static const uint8_t kRom[20] = {
    0xf3, 0xae, 0x08, 0x81, 0x81, 0xbf, 0x58, 0x3e, 0x55, 0xda,
    0xf9, 0x62, 0xa9, 0x2b, 0xb4, 0x6f, 0x4f, 0x1d, 0x07, 0xb7,
};

static int sFailures;

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); ++sFailures; } } while (0)

static uint8_t *Load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)*size);
    if (fread(data, 1, (size_t)*size, f) != (size_t)*size)
    {
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    return data;
}

/* CRC-32 bit by bit, as zlib defines it: what the word-at-a-time one must equal. */
static uint32_t BitwiseCrc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;

    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

static CtrPakStatus Parse(const uint8_t *pak, uint32_t abi, const uint8_t *rom, CtrPakEntry *entries,
                          CtrPakHeader *header)
{
    CtrPakStatus status = CtrPak_ParseHeader(pak, abi, rom, header);
    if (status != CTR_PAK_OK)
        return status;
    return CtrPak_ParseIndex(pak + header->indexOffset, header, entries);
}

int main(int argc, char **argv)
{
    long size = 0;
    uint8_t *pak;
    uint32_t abi;
    CtrPakHeader header;
    CtrPakEntry entries[16];
    uint8_t bad[20];
    static const char *const kPaths[] = {
        "maps/layouts.bin", "graphics/fonts/normal.latfont", "gamedata/gamedata.bin", "voxel/relief.bin",
    };

    if (argc != 3 || (pak = Load(argv[1], &size)) == NULL)
    {
        fprintf(stderr, "usage: pak_test FILE ABI_HEX\n");
        return 2;
    }
    abi = (uint32_t)strtoul(argv[2], NULL, 16);

    CHECK(Parse(pak, abi, kRom, entries, &header) == CTR_PAK_OK, "valid pack parses");
    CHECK(header.entryCount == 4, "entry count");
    for (unsigned i = 0; i < 4; ++i)
    {
        const CtrPakEntry *e = CtrPak_Find(entries, header.entryCount, kPaths[i]);
        CHECK(e != NULL, kPaths[i]);
        if (e != NULL)
        {
            CHECK(e->offset + e->rawSize <= (uint64_t)size, "payload inside the file");
            CHECK(e->offset % 32 == 0, "payload alignment");
            CHECK(CtrPak_Crc32(0, pak + e->offset, e->rawSize) == e->crc32, "payload CRC");
        }
    }
    {
        /* The builder writes the payloads in path order (pak.py), which the
         * console's warm-up reads in runs: these sorted, their offsets rise. */
        static const char *const kByPath[] = {
            "gamedata/gamedata.bin", "graphics/fonts/normal.latfont", "maps/layouts.bin", "voxel/relief.bin",
        };
        uint64_t last = 0;
        int ordered = 1;

        for (unsigned i = 0; i < 4; ++i)
        {
            const CtrPakEntry *e = CtrPak_Find(entries, header.entryCount, kByPath[i]);

            ordered &= e != NULL && e->offset >= last;
            if (e != NULL)
                last = e->offset;
        }
        CHECK(ordered, "payloads in path order");
    }
    CHECK(CtrPak_Find(entries, header.entryCount, "maps/layouts.bi") == NULL, "near miss is absent");
    CHECK(CtrPak_PathId("a") == 0xaf63dc4c8601ec8cull, "FNV-1a 64 reference");
    {
        /* From every alignment, over every length up to a few words: the
         * bytes before a word boundary, the words, the bytes after. */
        uint8_t bytes[96];
        int same = 1;

        for (unsigned i = 0; i < sizeof(bytes); ++i)
            bytes[i] = (uint8_t)(i * 37u + 11u);
        for (unsigned start = 0; start < 8; ++start)
            for (unsigned n = 0; start + n <= sizeof(bytes); ++n)
                same &= CtrPak_Crc32(0, bytes + start, n) == BitwiseCrc(bytes + start, n);
        CHECK(same, "word-at-a-time CRC equals the bitwise one");
        CHECK(CtrPak_Crc32(CtrPak_Crc32(0, bytes, 13), bytes + 13, 50) == BitwiseCrc(bytes, 63), "chained CRC");
        CHECK(CtrPak_Crc32(0, "123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    }

    CHECK(Parse(pak, abi ^ 1, kRom, entries, &header) == CTR_PAK_BAD_ABI, "other release is refused");
    CHECK(header.engineAbi == abi, "pack ABI still reported");
    memcpy(bad, kRom, sizeof(bad));
    bad[0] ^= 1;
    CHECK(Parse(pak, abi, bad, entries, &header) == CTR_PAK_BAD_ROM, "other ROM is refused");

    pak[0] ^= 1;
    CHECK(Parse(pak, abi, kRom, entries, &header) == CTR_PAK_BAD_MAGIC, "bad magic");
    pak[0] ^= 1;
    pak[20] ^= 1;
    CHECK(Parse(pak, abi, kRom, entries, &header) == CTR_PAK_BAD_HEADER_CRC, "damaged header");
    pak[20] ^= 1;
    pak[CTR_PAK_HEADER_BYTES + 5] ^= 1;
    CHECK(Parse(pak, abi, kRom, entries, &header) == CTR_PAK_BAD_INDEX_CRC, "damaged index");
    pak[CTR_PAK_HEADER_BYTES + 5] ^= 1;
    CHECK(Parse(pak, abi, kRom, entries, &header) == CTR_PAK_OK, "restored pack parses again");

    free(pak);
    if (sFailures)
        return 1;
    printf("PASS pak: lookup, payload CRCs, ABI/ROM/magic/header/index rejection\n");
    return 0;
}
