/*
 * The Nuzlocke tracker's record on the SD card (3ds_nuzlocke_store.h): written
 * whole to a temporary file then moved over the record, the previous one kept
 * as the backup when the card will not replace a file in one rename. Loaded
 * only when its checks hold and it names this very save (its CRC and size).
 */
#include <stdio.h>
#include <string.h>
#include "3ds_nuzlocke_store.h"
#include "3ds_data.h"
#include "3ds_platform.h"

#define NUZ_FILE "emerald3ds.nuzlocke"
#define NUZ_TEMP "emerald3ds.nuzlocke.tmp"
#define NUZ_BACKUP "emerald3ds.nuzlocke.bak"

/* 0: read and valid; -1: absent; -2: present but not a valid record. */
static int ReadRecord(const char *name, CtrNuzState *state, uint32_t *saveCrc, uint32_t *saveSize)
{
    uint8_t bytes[CTR_NUZ_RECORD_BYTES];
    FILE *file = CtrFs_OpenData(name, "rb");
    bool valid;
    size_t got;

    if (file == NULL)
        return -1;
    got = fread(bytes, 1, sizeof(bytes), file);
    valid = fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0)
        valid = false;
    if (!valid || !CtrNuz_DecodeSized(bytes, got, state, saveCrc, saveSize))
        return -2;
    return 0;
}

CtrNuzLink CtrNuzStore_Load(const void *save, size_t size, CtrNuzState *state)
{
    /* The temp file too: a power cut between Save's FILE->BAK and TMP->FILE
     * leaves the new record only there. Each must name this save. */
    static const char *const names[] = {NUZ_FILE, NUZ_TEMP, NUZ_BACKUP};
    bool other = false, damaged = false;
    uint32_t crc, saveCrc, saveSize;

    CtrNuz_Reset(state);
    if (save == NULL || size == 0 || (size_t)(uint32_t)size != size)
        return CTR_NUZ_LINK_NONE;
    crc = CtrNuz_Crc32(0, save, size);
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); ++index)
    {
        CtrNuzState candidate;
        int result = ReadRecord(names[index], &candidate, &saveCrc, &saveSize);

        if (result == -1)
            continue;
        if (result != 0)
        {
            damaged = true;
            continue;
        }
        if (saveCrc != crc || saveSize != (uint32_t)size)
        {
            other = true;
            continue;
        }
        *state = candidate;
        return CTR_NUZ_LINK_MATCHED;
    }
    if (other)
    {
        CtrLog_Write(CTR_LOG_FS, "nuzlocke: record is of another save, not applied");
        return CTR_NUZ_LINK_OTHER;
    }
    if (damaged)
    {
        CtrLog_Write(CTR_LOG_FS, "nuzlocke: record failed its checks, not applied");
        return CTR_NUZ_LINK_DAMAGED;
    }
    return CTR_NUZ_LINK_NONE;
}

bool CtrNuzStore_Save(const CtrNuzState *state, const void *save, size_t size)
{
    uint8_t bytes[CTR_NUZ_RECORD_BYTES];
    FILE *file;
    bool valid;

    /* A scenario being played saves into memory only (Port_WriteFlash): the
     * player's record stays the one that matches the player's save. */
    if (!CtrScenario_SaveWrites())
        return true;
    if (state == NULL || save == NULL || size == 0 || (size_t)(uint32_t)size != size)
        return false;
    CtrNuz_Encode(state, CtrNuz_Crc32(0, save, size), (uint32_t)size, bytes);
    file = CtrFs_OpenData(NUZ_TEMP, "wb");
    if (file == NULL)
        goto fail;
    valid = fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) && fflush(file) == 0;
    if (fclose(file) != 0)
        valid = false;
    if (valid && rename(CTR_DATA_DIR NUZ_TEMP, CTR_DATA_DIR NUZ_FILE) == 0)
        return true;
    if (valid)
    {
        file = CtrFs_OpenData(NUZ_FILE, "rb");
        if (file != NULL)
        {
            fclose(file);
            remove(CTR_DATA_DIR NUZ_BACKUP);
            if (rename(CTR_DATA_DIR NUZ_FILE, CTR_DATA_DIR NUZ_BACKUP) == 0)
            {
                if (rename(CTR_DATA_DIR NUZ_TEMP, CTR_DATA_DIR NUZ_FILE) == 0)
                    return true;
                rename(CTR_DATA_DIR NUZ_BACKUP, CTR_DATA_DIR NUZ_FILE);
            }
        }
    }
    remove(CTR_DATA_DIR NUZ_TEMP);
fail:
    CtrLog_Write(CTR_LOG_ERROR, "nuzlocke: record write failed; game save is unchanged");
    return false;
}
