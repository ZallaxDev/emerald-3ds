#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "3ds_nuzlocke_core.h"
#include "3ds_nuzlocke_store.h"
#include "3ds_data.h"
#include "3ds_platform.h"

#ifdef _WIN32
#define mkdir(path, mode) mkdir(path)
#endif

/* The card's rename, which will not replace a file (built with -Drename=CtrTestRename). */
#undef rename
int rename(const char *from, const char *to);

int CtrTestRename(const char *from, const char *to)
{
    FILE *file = fopen(to, "rb");

    if (file != NULL)
    {
        fclose(file);
        errno = EEXIST;
        return -1;
    }
    return rename(from, to);
}

static bool sFailWrite;
static bool sScenario;

bool CtrScenario_SaveWrites(void) { return !sScenario; }
void CtrLog_Write(CtrLogCategory category, const char *format, ...)
{
    (void)category;
    (void)format;
}
FILE *CtrFs_OpenData(const char *relative, const char *mode)
{
    char path[160];

    if (sFailWrite && mode[0] == 'w')
        return NULL;
    snprintf(path, sizeof(path), "%s%s", CTR_DATA_DIR, relative);
    return fopen(path, mode);
}

#define RECORD "emerald3ds.nuzlocke"

static size_t ReadFile(const char *name, unsigned char *bytes, size_t size)
{
    FILE *file = CtrFs_OpenData(name, "rb");
    size_t got;

    assert(file != NULL);
    got = fread(bytes, 1, size, file);
    assert(fclose(file) == 0);
    return got;
}

static void WriteFile(const char *name, const unsigned char *bytes, size_t size)
{
    FILE *file = CtrFs_OpenData(name, "wb");

    assert(file != NULL);
    assert(fwrite(bytes, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void RemoveFile(const char *name)
{
    char path[160];

    snprintf(path, sizeof(path), "%s%s", CTR_DATA_DIR, name);
    remove(path);
}

static bool SameState(const CtrNuzState *a, const CtrNuzState *b)
{
    if (memcmp(a->area, b->area, sizeof(a->area)) != 0 || a->fallenCount != b->fallenCount)
        return false;
    for (unsigned i = 0; i < a->fallenCount; i++)
        if (a->fallen[i].personality != b->fallen[i].personality || a->fallen[i].otId != b->fallen[i].otId || a->fallen[i].shedinja != b->fallen[i].shedinja)
            return false;
    return true;
}

static void FixCrc(uint8_t *record)
{
    uint32_t crc = CtrNuz_Crc32(0, record, CTR_NUZ_RECORD_BYTES - 4);

    for (unsigned i = 0; i < 4; i++)
        record[CTR_NUZ_RECORD_BYTES - 4 + i] = (uint8_t)(crc >> (i * 8));
}

static bool DecodeOk(const uint8_t *record)
{
    CtrNuzState state;
    uint32_t crc, size;

    return CtrNuz_Decode(record, &state, &crc, &size);
}

/* The bit-by-bit CRC-32 the table replaced: the records already on cards
 * were written with it. */
static uint32_t BitwiseCrc32(uint32_t crc, const uint8_t *bytes, size_t size)
{
    crc = ~crc;
    while (size-- > 0)
    {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void TestCrc(void)
{
    static uint8_t buffer[4099];
    uint32_t seed = 0x12345678u;

    for (unsigned round = 0; round < 64; ++round)
    {
        size_t size = round == 0 ? 0 : (seed >> 7) % sizeof(buffer);
        uint32_t start = round & 1 ? seed : 0;

        for (size_t i = 0; i < size; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            buffer[i] = (uint8_t)(seed >> 24);
        }
        seed = seed * 1664525u + 1013904223u;
        assert(CtrNuz_Crc32(start, buffer, size) == BitwiseCrc32(start, buffer, size));
        /* In pieces as in one go. */
        assert(CtrNuz_Crc32(CtrNuz_Crc32(start, buffer, size / 3), buffer + size / 3, size - size / 3)
               == BitwiseCrc32(start, buffer, size));
    }
}

static void TestCore(void)
{
    static CtrNuzState state, decoded;
    static uint8_t record[CTR_NUZ_RECORD_BYTES], probe[CTR_NUZ_RECORD_BYTES];
    /* A synthetic party: identities, not slots. */
    CtrNuzMon party[3] = {{0x11111111u, 0xAAAA0001u, false}, {0x22222222u, 0xAAAA0001u, false}, {0x33333333u, 0xBBBB0002u, false}};
    CtrNuzMon swap;
    unsigned caught, missed, fallen, leader;
    uint32_t crc, size;
    static const unsigned caps[8] = {15, 19, 24, 29, 31, 33, 42, 46};

    assert(CtrNuz_Crc32(0, "123456789", 9) == 0xCBF43926u);
    CtrNuz_Reset(&state);
    assert(CtrNuz_IsEmpty(&state));
    assert(CtrNuz_Area(&state, 5) == CTR_NUZ_UNUSED);
    assert(CtrNuz_Area(&state, 999) == CTR_NUZ_UNUSED);
    assert(!CtrNuz_SetArea(&state, CTR_NUZ_AREA_COUNT, CTR_NUZ_CAUGHT));
    assert(!CtrNuz_SetArea(&state, 1, CTR_NUZ_STATUS_COUNT));
    assert(CtrNuz_IsEmpty(&state));
    assert(CtrNuz_SetArea(&state, 16, CTR_NUZ_CAUGHT));
    assert(CtrNuz_SetArea(&state, 17, CTR_NUZ_MISSED));
    assert(CtrNuz_SetArea(&state, 18, CTR_NUZ_CAUGHT));
    assert(CtrNuz_SetArea(&state, 18, CTR_NUZ_UNUSED));
    assert(!CtrNuz_IsEmpty(&state));
    assert(CtrNuz_Area(&state, 16) == CTR_NUZ_CAUGHT && CtrNuz_Area(&state, 17) == CTR_NUZ_MISSED);

    /* Marked by identity: reordering and depositing do not lose it. */
    assert(CtrNuz_SetFallen(&state, party[1].personality, party[1].otId, false, true));
    assert(!CtrNuz_SetFallen(&state, party[1].personality, party[1].otId, false, true));
    swap = party[0];
    party[0] = party[1];
    party[1] = swap;
    assert(CtrNuz_IsFallen(&state, party[0].personality, party[0].otId, false));
    assert(!CtrNuz_IsFallen(&state, party[1].personality, party[1].otId, false));
    party[0] = party[2];   /* deposited: gone from the party, still fallen */
    assert(CtrNuz_IsFallen(&state, 0x22222222u, 0xAAAA0001u, false));
    /* Same personality, another trainer: another Pokemon. */
    assert(!CtrNuz_IsFallen(&state, 0x22222222u, 0xAAAA0002u, false));
    assert(CtrNuz_SetFallen(&state, party[2].personality, party[2].otId, false, true));
    CtrNuz_Counts(&state, &caught, &missed, &fallen);
    assert(caught == 1 && missed == 1 && fallen == 2);

    /* Clearing keeps the list packed. */
    assert(CtrNuz_SetFallen(&state, 0x22222222u, 0xAAAA0001u, false, false));
    assert(!CtrNuz_SetFallen(&state, 0x22222222u, 0xAAAA0001u, false, false));
    assert(state.fallenCount == 1 && state.fallen[0].personality == 0x33333333u);
    assert(state.fallen[1].personality == 0 && state.fallen[1].otId == 0);

    /* Level caps from badges. */
    for (unsigned badges = 0; badges < 8; badges++)
    {
        assert(CtrNuz_NextCap(badges, &leader) == caps[badges]);
        assert(leader == badges + 1);
    }
    assert(CtrNuz_NextCap(8, &leader) == 58 && leader == 9);
    assert(CtrNuz_NextCap(12, &leader) == 58 && leader == 9);

    /* The record: round trip. */
    CtrNuz_Encode(&state, 0x12345678u, 131072u, record);
    assert(CtrNuz_Decode(record, &decoded, &crc, &size));
    assert(SameState(&state, &decoded) && crc == 0x12345678u && size == 131072u);

    /* Corruption, each with a valid CRC where the CRC is not the point. */
    memcpy(probe, record, sizeof(probe));
    probe[0] ^= 1;
    FixCrc(probe);
    assert(!DecodeOk(probe));
    memcpy(probe, record, sizeof(probe));
    probe[CTR_NUZ_HEADER_BYTES + 16] ^= 1;
    assert(!DecodeOk(probe));
    memcpy(probe, record, sizeof(probe));
    probe[CTR_NUZ_HEADER_BYTES + 20] = CTR_NUZ_STATUS_COUNT;
    FixCrc(probe);
    assert(!DecodeOk(probe));
    memcpy(probe, record, sizeof(probe));
    probe[16] = CTR_NUZ_FALLEN_MAX + 1;
    FixCrc(probe);
    assert(!DecodeOk(probe));
    memcpy(probe, record, sizeof(probe));
    probe[20] = 1;   /* reserved */
    FixCrc(probe);
    assert(!DecodeOk(probe));
    memcpy(probe, record, sizeof(probe));
    probe[CTR_NUZ_HEADER_BYTES + CTR_NUZ_AREA_COUNT + 9] = 7;   /* past the count */
    FixCrc(probe);
    assert(!DecodeOk(probe));
    /* The same Pokemon twice. */
    memcpy(probe, record, sizeof(probe));
    probe[16] = 2;
    memcpy(probe + CTR_NUZ_HEADER_BYTES + CTR_NUZ_AREA_COUNT + 9, probe + CTR_NUZ_HEADER_BYTES + CTR_NUZ_AREA_COUNT, 9);
    FixCrc(probe);
    assert(!DecodeOk(probe));
    /* A failed decode leaves the state alone. */
    decoded = state;
    assert(!CtrNuz_Decode(probe, &decoded, &crc, &size));
    assert(SameState(&state, &decoded));

    /* The list's capacity. */
    CtrNuz_Reset(&state);
    for (unsigned i = 0; i < CTR_NUZ_FALLEN_MAX; i++)
        assert(CtrNuz_SetFallen(&state, i + 1, 7, false, true));
    assert(!CtrNuz_SetFallen(&state, 9999, 7, false, true));
    CtrNuz_Encode(&state, 1, 2, record);
    assert(CtrNuz_Decode(record, &decoded, &crc, &size) && SameState(&state, &decoded));
}

/* A v1 fixture in the original layout, including its original CRC. */
static void MakeLegacy(const uint8_t *current, uint8_t *legacy)
{
    unsigned start = CTR_NUZ_HEADER_BYTES + CTR_NUZ_AREA_COUNT;
    uint32_t crc;

    memcpy(legacy, current, start);
    legacy[7] = '1';
    for (unsigned i = 0; i < CTR_NUZ_FALLEN_MAX; ++i)
        memcpy(legacy + start + i * 8u, current + start + i * 9u, 8);
    crc = CtrNuz_Crc32(0, legacy, CTR_NUZ_RECORD_V1_BYTES - 4);
    for (unsigned i = 0; i < 4; ++i)
        legacy[CTR_NUZ_RECORD_V1_BYTES - 4 + i] = (uint8_t)(crc >> (i * 8u));
}

static void TestShedinja(void)
{
    CtrNuzState state, decoded;
    uint8_t record[CTR_NUZ_RECORD_BYTES], legacy[CTR_NUZ_RECORD_V1_BYTES];
    uint32_t crc, size;
    unsigned flagsAt = CTR_NUZ_HEADER_BYTES + CTR_NUZ_AREA_COUNT + 8;

    CtrNuz_Reset(&state);
    /* Nincada's main lineage keeps its mark when it evolves to Ninjask. The
     * copied Shedinja has the same personality/OT but is a separate Pokemon. */
    assert(CtrNuz_SetFallen(&state, 123, 456, false, true));
    assert(CtrNuz_IsFallen(&state, 123, 456, false));
    assert(!CtrNuz_IsFallen(&state, 123, 456, true));
    assert(CtrNuz_SetFallen(&state, 123, 456, true, true));
    assert(state.fallenCount == 2);
    CtrNuz_Encode(&state, 10, 20, record);
    assert(CtrNuz_Decode(record, &decoded, &crc, &size));
    assert(SameState(&state, &decoded));
    /* Clearing either sibling must leave the other marked. */
    assert(CtrNuz_SetFallen(&decoded, 123, 456, true, false));
    assert(CtrNuz_IsFallen(&decoded, 123, 456, false));
    assert(!CtrNuz_IsFallen(&decoded, 123, 456, true));
    assert(CtrNuz_SetFallen(&state, 123, 456, false, false));
    assert(CtrNuz_IsFallen(&state, 123, 456, true));
    CtrNuz_Encode(&state, 10, 20, record);
    record[flagsAt] = 2;
    FixCrc(record);
    assert(!DecodeOk(record));
    CtrNuz_Encode(&state, 10, 20, record);
    record[flagsAt + 9] = 1; /* nonzero unused slot */
    FixCrc(record);
    assert(!DecodeOk(record));

    CtrNuz_Reset(&state);
    CtrNuz_SetArea(&state, 16, CTR_NUZ_MISSED);
    assert(CtrNuz_SetFallen(&state, 123, 456, false, true));
    CtrNuz_Encode(&state, 10, 20, record);
    MakeLegacy(record, legacy);
    assert(CtrNuz_DecodeSized(legacy, sizeof(legacy), &decoded, &crc, &size));
    assert(SameState(&state, &decoded) && crc == 10 && size == 20);
    assert(!CtrNuz_IsFallen(&decoded, 123, 456, true));
    assert(!CtrNuz_DecodeSized(legacy, sizeof(legacy) - 1, &decoded, &crc, &size));
    legacy[7] = '2'; /* correct sizes are also tied to the format version */
    assert(!CtrNuz_DecodeSized(legacy, sizeof(legacy), &decoded, &crc, &size));
}

static void TestSession(void)
{
    static CtrNuzSession session;

    CtrNuzSession_NewGame(&session);
    assert(CtrNuz_IsEmpty(&session.state));
    assert(CtrNuzSession_ShouldWrite(&session, true));
    assert(CtrNuzSession_Label(&session) == CTR_NUZ_LABEL_NEW);

    /* No record: nothing to write until something is marked. */
    CtrNuzSession_Loaded(&session, CTR_NUZ_LINK_NONE);
    assert(!CtrNuzSession_ShouldWrite(&session, true));
    /* Another save's record: never applied, never replaced by an empty tracker. */
    CtrNuz_SetArea(&session.state, 3, CTR_NUZ_CAUGHT);
    CtrNuzSession_Loaded(&session, CTR_NUZ_LINK_OTHER);
    assert(CtrNuz_IsEmpty(&session.state));
    assert(!CtrNuzSession_ShouldWrite(&session, true));
    assert(CtrNuzSession_Label(&session) == CTR_NUZ_LABEL_OTHER_SAVE);
    CtrNuzSession_Loaded(&session, CTR_NUZ_LINK_DAMAGED);
    assert(CtrNuzSession_Label(&session) == CTR_NUZ_LABEL_DAMAGED);
    assert(!CtrNuzSession_ShouldWrite(&session, true));

    /* An edit is written by the next save. */
    CtrNuz_SetArea(&session.state, 3, CTR_NUZ_MISSED);
    CtrNuzSession_Edited(&session);
    assert(CtrNuzSession_Label(&session) == CTR_NUZ_LABEL_UNSAVED);
    assert(CtrNuzSession_ShouldWrite(&session, true));
    CtrNuzSession_Written(&session, false);
    assert(CtrNuzSession_Label(&session) == CTR_NUZ_LABEL_WRITE_FAILED);
    assert(session.dirty);
    CtrNuzSession_Written(&session, true);
    assert(CtrNuzSession_Label(&session) == CTR_NUZ_LABEL_THIS_SAVE);
    assert(!session.dirty && session.owned);

    /* This save's record is kept, and written again even emptied. */
    CtrNuzSession_Loaded(&session, CTR_NUZ_LINK_MATCHED);
    assert(CtrNuz_Area(&session.state, 3) == CTR_NUZ_MISSED);
    CtrNuz_SetArea(&session.state, 3, CTR_NUZ_UNUSED);
    CtrNuzSession_Edited(&session);
    assert(CtrNuz_IsEmpty(&session.state) && CtrNuzSession_ShouldWrite(&session, true));

    /* The tracker off: an empty unedited tracker writes nothing, a new game
     * included; marks, and edits not yet saved, are still written. */
    CtrNuzSession_NewGame(&session);
    assert(!CtrNuzSession_ShouldWrite(&session, false));
    assert(CtrNuzSession_ShouldWrite(&session, true));
    CtrNuzSession_Loaded(&session, CTR_NUZ_LINK_MATCHED);
    assert(CtrNuz_IsEmpty(&session.state) && !CtrNuzSession_ShouldWrite(&session, false));
    CtrNuz_SetArea(&session.state, 7, CTR_NUZ_CAUGHT);
    CtrNuzSession_Edited(&session);
    assert(CtrNuzSession_ShouldWrite(&session, false));
    CtrNuzSession_Written(&session, true);
    assert(CtrNuzSession_ShouldWrite(&session, false));
    CtrNuz_SetArea(&session.state, 7, CTR_NUZ_UNUSED);
    CtrNuzSession_Edited(&session);
    assert(CtrNuz_IsEmpty(&session.state) && CtrNuzSession_ShouldWrite(&session, false));
    CtrNuzSession_Written(&session, true);
    assert(!CtrNuzSession_ShouldWrite(&session, false) && CtrNuzSession_ShouldWrite(&session, true));
    /* Off then on again: the same rules as before it went off. */
    CtrNuzSession_Loaded(&session, CTR_NUZ_LINK_OTHER);
    assert(!CtrNuzSession_ShouldWrite(&session, false) && !CtrNuzSession_ShouldWrite(&session, true));
}

static void TestStore(void)
{
    static unsigned char saveA[131072], saveB[131072], unchanged[131072];
    static CtrNuzState state, first, loaded;
    static unsigned char bytes[CTR_NUZ_RECORD_BYTES + 1], kept[CTR_NUZ_RECORD_BYTES];

    for (size_t i = 0; i < sizeof(saveA); i++)
        saveA[i] = (unsigned char)(i * 7u + 1u);
    memcpy(saveB, saveA, sizeof(saveB));
    saveB[4000] ^= 1;
    memcpy(unchanged, saveA, sizeof(saveA));

    /* No record. */
    CtrNuz_SetArea(&loaded, 1, CTR_NUZ_CAUGHT);
    assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_NONE);
    assert(CtrNuz_IsEmpty(&loaded));

    /* Written, read back for this save only. */
    CtrNuz_Reset(&first);
    CtrNuz_SetArea(&first, 16, CTR_NUZ_CAUGHT);
    CtrNuz_SetFallen(&first, 0xDEADBEEFu, 0x0001E240u, false, true);
    assert(CtrNuzStore_Save(&first, saveA, sizeof(saveA)));
    assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_MATCHED);
    assert(SameState(&first, &loaded));
    assert(CtrNuzStore_Load(saveB, sizeof(saveB), &loaded) == CTR_NUZ_LINK_OTHER);
    assert(CtrNuz_IsEmpty(&loaded));
    /* The same bytes, a different length: another save. */
    assert(CtrNuzStore_Load(saveA, sizeof(saveA) - 1, &loaded) == CTR_NUZ_LINK_OTHER);
    assert(memcmp(saveA, unchanged, sizeof(saveA)) == 0);

    /* A matching v1 sidecar upgrades on save without losing areas/main-line
     * marks, while its original bytes remain available in the backup. */
    {
        uint8_t old[CTR_NUZ_RECORD_V1_BYTES];
        CtrNuz_Encode(&first, CtrNuz_Crc32(0, saveA, sizeof(saveA)), sizeof(saveA), bytes);
        MakeLegacy(bytes, old);
        WriteFile(RECORD, old, sizeof(old));
        assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_MATCHED);
        assert(SameState(&first, &loaded));
        assert(CtrNuzStore_Save(&loaded, saveA, sizeof(saveA)));
        assert(ReadFile(RECORD, bytes, sizeof(bytes)) == CTR_NUZ_RECORD_BYTES && bytes[7] == '2');
        assert(ReadFile(RECORD ".bak", bytes, sizeof(bytes)) == sizeof(old));
        assert(memcmp(bytes, old, sizeof(old)) == 0);
        RemoveFile(RECORD ".bak");
    }

    /* A failed write keeps the record. */
    assert(ReadFile(RECORD, kept, sizeof(kept)) == sizeof(kept));
    sFailWrite = true;
    CtrNuz_Reset(&state);
    assert(!CtrNuzStore_Save(&state, saveA, sizeof(saveA)));
    sFailWrite = false;
    assert(ReadFile(RECORD, bytes, sizeof(bytes)) == sizeof(kept) && memcmp(bytes, kept, sizeof(kept)) == 0);

    /* A scenario writes nothing. */
    sScenario = true;
    assert(CtrNuzStore_Save(&state, saveA, sizeof(saveA)));
    sScenario = false;
    assert(ReadFile(RECORD, bytes, sizeof(bytes)) == sizeof(kept) && memcmp(bytes, kept, sizeof(kept)) == 0);

    /* Replaced without overwriting (the card's rename): the old one is the backup. */
    state = first;
    CtrNuz_SetArea(&state, 17, CTR_NUZ_MISSED);
    assert(CtrNuzStore_Save(&state, saveA, sizeof(saveA)));
    assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_MATCHED && SameState(&state, &loaded));
    assert(ReadFile(RECORD ".bak", bytes, sizeof(bytes)) == sizeof(kept) && memcmp(bytes, kept, sizeof(kept)) == 0);

    /* A damaged record falls back to the backup. */
    ReadFile(RECORD, bytes, sizeof(kept));
    bytes[100] ^= 0x40;
    WriteFile(RECORD, bytes, sizeof(kept));
    assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_MATCHED && SameState(&first, &loaded));
    /* Damaged, truncated, extended, with no backup: not applied. */
    RemoveFile(RECORD ".bak");
    assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_DAMAGED && CtrNuz_IsEmpty(&loaded));
    WriteFile(RECORD, kept, sizeof(kept) - 1);
    assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_DAMAGED);
    memcpy(bytes, kept, sizeof(kept));
    bytes[sizeof(kept)] = 0;
    WriteFile(RECORD, bytes, sizeof(kept) + 1);
    assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_DAMAGED);

    /* Only the temporary file left (a power cut between the renames). */
    RemoveFile(RECORD);
    WriteFile(RECORD ".tmp", kept, sizeof(kept));
    assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_MATCHED && SameState(&first, &loaded));
    RemoveFile(RECORD ".tmp");

    /* A later save's record, the earlier save's kept as backup: each loads its own. */
    assert(CtrNuzStore_Save(&first, saveA, sizeof(saveA)));
    assert(CtrNuzStore_Save(&state, saveB, sizeof(saveB)));
    assert(CtrNuzStore_Load(saveB, sizeof(saveB), &loaded) == CTR_NUZ_LINK_MATCHED && SameState(&state, &loaded));
    assert(CtrNuzStore_Load(saveA, sizeof(saveA), &loaded) == CTR_NUZ_LINK_MATCHED && SameState(&first, &loaded));
    assert(memcmp(saveA, unchanged, sizeof(saveA)) == 0);

    RemoveFile(RECORD);
    RemoveFile(RECORD ".bak");
    RemoveFile(RECORD ".tmp");
}

int main(void)
{
    char folder[] = "/tmp/emerald-nuzlocke-XXXXXX";

    TestCrc();
    TestCore();
    TestSession();
    TestShedinja();
#ifdef _WIN32
    puts("SKIP nuzlocke store: sdmc:/ paths need a POSIX host");
    puts("PASS nuzlocke core");
    return 0;
#endif
    assert(mkdtemp(folder) != NULL && chdir(folder) == 0);
    assert(mkdir("sdmc:", 0700) == 0);
    assert(mkdir("sdmc:/3ds", 0700) == 0);
    assert(mkdir(CTR_DATA_DIR, 0700) == 0);
    TestStore();
    assert(rmdir(CTR_DATA_DIR) == 0);
    assert(rmdir("sdmc:/3ds") == 0);
    assert(rmdir("sdmc:") == 0);
    assert(chdir("/tmp") == 0 && rmdir(folder) == 0);
    puts("PASS nuzlocke: areas, fallen by identity across reorder/deposit, independent Shedinja, legacy migration, level caps, record checks, "
         "write rules, save binding, other/damaged not applied, backup and temp recovery, failed write, no save mutation");
    return 0;
}
