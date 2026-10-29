#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "3ds_clock_store.h"
#include "3ds_daynight.h"
#include "3ds_data.h"
#include "3ds_pak.h"
#include "3ds_platform.h"

#define CLOCK_FILE "emerald3ds.clock"
#define CLOCK_TEMP "emerald3ds.clock.tmp"
#define CLOCK_BACKUP "emerald3ds.clock.bak"
#define CLOCK_BYTES 40u

static void WriteNumber(unsigned char *bytes, uint64_t value, unsigned size)
{
    for (unsigned index = 0; index < size; ++index)
        bytes[index] = (unsigned char)(value >> (index * 8u));
}

static uint64_t ReadNumber(const unsigned char *bytes, unsigned size)
{
    uint64_t value = 0;

    for (unsigned index = 0; index < size; ++index)
        value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

static CtrClockStoreStatus sStatus = CTR_CLOCK_STORE_NONE;
static unsigned sLoadSerial;

CtrClockStoreStatus CtrClockStore_Status(void)
{
    return sStatus;
}

unsigned CtrClockStore_LoadSerial(void)
{
    return sLoadSerial;
}

bool CtrClockStore_NeedsNotice(CtrClockStoreStatus status)
{
    return status == CTR_CLOCK_STORE_MISSING || status == CTR_CLOCK_STORE_INVALID || status == CTR_CLOCK_STORE_MISMATCH;
}

bool CtrClockStore_Load(const void *save, size_t size)
{
    unsigned char bytes[CLOCK_BYTES];
    FILE *file;
    bool valid, found = false, mismatch = false;
    CtrDayNightSnapshot snapshot;

    ++sLoadSerial;
    sStatus = CTR_CLOCK_STORE_NONE;
    CtrDayNight_Reset();
    if (save == NULL || size == 0)
        return false;
    /* The temp file too: a power cut between Save's FILE->BAK and TMP->FILE
     * leaves the new state only there. Each must match this save's CRC. */
    const char *const names[] = {CLOCK_FILE, CLOCK_TEMP, CLOCK_BACKUP};

    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); ++index)
    {
        file = CtrFs_OpenData(names[index], "rb");
        if (file == NULL)
        {
            found |= errno != ENOENT;
            continue;
        }
        found = true;
        valid = fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes)
             && fgetc(file) == EOF && !ferror(file);
        if (fclose(file) != 0)
            valid = false;
        /* The same checks as ever; only which one failed is now told apart:
         * a sound record of another save is a mismatch, anything else invalid. */
        if (!valid || memcmp(bytes, "EM3DCLK1", 8) != 0 || bytes[12] > 1
         || bytes[13] != 0 || bytes[14] != 0 || bytes[15] != 0
         || ReadNumber(bytes + 36, 4) != CtrPak_Crc32(0, bytes, 36)
         || ReadNumber(bytes + 28, 8) > INT64_MAX)
            continue;
        if (ReadNumber(bytes + 8, 4) != CtrPak_Crc32(0, save, size))
        {
            mismatch = true;
            continue;
        }
        snapshot.timeMs = (uint32_t)ReadNumber(bytes + 16, 4);
        snapshot.berryBonusMs = ReadNumber(bytes + 20, 8);
        snapshot.wallSeconds = (int64_t)ReadNumber(bytes + 28, 8);
        snapshot.shifted = bytes[12] != 0;
        if (CtrDayNight_Restore(&snapshot))
        {
            sStatus = index == 0 ? CTR_CLOCK_STORE_RESTORED : CTR_CLOCK_STORE_RECOVERED;
            return true;
        }
    }
    sStatus = mismatch ? CTR_CLOCK_STORE_MISMATCH : found ? CTR_CLOCK_STORE_INVALID : CTR_CLOCK_STORE_MISSING;
    CtrLog_Write(CTR_LOG_FS, "clock: no matching valid state, using local RTC (status %d)", (int)sStatus);
    return false;
}

bool CtrClockStore_Save(const void *save, size_t size)
{
    unsigned char bytes[CLOCK_BYTES] = {0};
    CtrDayNightSnapshot snapshot;
    FILE *file;
    bool valid;

    /* A scenario being played saves into memory only (Port_WriteFlash): the
     * player's own clock file stays the one that matches the player's save. */
    if (!CtrScenario_SaveWrites())
        return true;
    if (save == NULL || size == 0 || !CtrDayNight_Snapshot(&snapshot))
        return false;
    memcpy(bytes, "EM3DCLK1", 8);
    WriteNumber(bytes + 8, CtrPak_Crc32(0, save, size), 4);
    bytes[12] = snapshot.shifted;
    WriteNumber(bytes + 16, snapshot.timeMs, 4);
    WriteNumber(bytes + 20, snapshot.berryBonusMs, 8);
    WriteNumber(bytes + 28, (uint64_t)snapshot.wallSeconds, 8);
    WriteNumber(bytes + 36, CtrPak_Crc32(0, bytes, 36), 4);
    file = CtrFs_OpenData(CLOCK_TEMP, "wb");
    if (file == NULL)
        goto fail;
    valid = fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) && fflush(file) == 0;
    if (fclose(file) != 0)
        valid = false;
    if (valid && rename(CTR_DATA_DIR CLOCK_TEMP, CTR_DATA_DIR CLOCK_FILE) == 0)
        return true;
    if (valid)
    {
        file = CtrFs_OpenData(CLOCK_FILE, "rb");
        if (file != NULL)
        {
            fclose(file);
            remove(CTR_DATA_DIR CLOCK_BACKUP);
            if (rename(CTR_DATA_DIR CLOCK_FILE, CTR_DATA_DIR CLOCK_BACKUP) == 0)
            {
                if (rename(CTR_DATA_DIR CLOCK_TEMP, CTR_DATA_DIR CLOCK_FILE) == 0)
                    return true;
                rename(CTR_DATA_DIR CLOCK_BACKUP, CTR_DATA_DIR CLOCK_FILE);
            }
        }
    }
    remove(CTR_DATA_DIR CLOCK_TEMP);
fail:
    CtrLog_Write(CTR_LOG_ERROR, "clock: sidecar write failed; game save is unchanged");
    return false;
}
