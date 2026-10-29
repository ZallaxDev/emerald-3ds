#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "3ds_clock_store.h"
#include "3ds_daynight.h"
#include "3ds_data.h"
#include "3ds_pak.h"
#include "3ds_platform.h"

#ifdef _WIN32
/* mingw's mkdir takes no mode. */
#define mkdir(path, mode) mkdir(path)
#endif

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

static uint64_t sNow;
static int64_t sWall = 1700000000;
static int sSpeed = 5;
static bool sFailWrite;

uint64_t CtrPlatform_Milliseconds(void) { return sNow; }
int64_t CtrPlatform_WallSeconds(void) { return sWall; }
bool CtrGame_IsOutdoor(void) { return true; }
bool CtrScenario_SaveWrites(void) { return true; }
bool CtrGame_LocalTimeSeconds(unsigned *seconds) { *seconds = 12 * 3600; return true; }
int CtrSettings_GetInt(const char *key, int fallback)
{
    return strcmp(key, "clock_speed") == 0 ? sSpeed : fallback;
}
/* Linked for CtrDayNight_SyncToConsole; this test never syncs. */
void CtrSettings_SetInt(const char *key, int value)
{
    if (strcmp(key, "clock_speed") == 0)
        sSpeed = value;
}
void CtrLog_Write(CtrLogCategory category, const char *format, ...)
{
    (void)category;
    (void)format;
}
FILE *CtrFs_OpenData(const char *relative, const char *mode)
{
    char path[128];

    if (sFailWrite && mode[0] == 'w')
        return NULL;
    snprintf(path, sizeof(path), "%s%s", CTR_DATA_DIR, relative);
    return fopen(path, mode);
}

static void ReadRecord(unsigned char bytes[40])
{
    FILE *file = CtrFs_OpenData("emerald3ds.clock", "rb");

    assert(file != NULL);
    assert(fread(bytes, 1, 40, file) == 40);
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
}

static void WriteRecord(const unsigned char *bytes, size_t size)
{
    FILE *file = CtrFs_OpenData("emerald3ds.clock", "wb");

    assert(file != NULL);
    assert(fwrite(bytes, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void UpdateChecksum(unsigned char bytes[40])
{
    uint32_t checksum = CtrPak_Crc32(0, bytes, 36);

    for (unsigned index = 0; index < 4; ++index)
        bytes[36 + index] = (unsigned char)(checksum >> (index * 8));
}

int main(void)
{
    char folder[] = "/tmp/emerald-clock-store-XXXXXX";
    unsigned char save[131072] = {1}, unchanged[131072];
    unsigned char record[40], probe[41];
    CtrDayNightSnapshot saved, loaded;

#ifdef _WIN32
    /* The store writes CTR_DATA_DIR ("sdmc:/...") as is, and Windows cannot
     * create a folder named "sdmc:" (nor has /tmp): run it on a POSIX host. */
    puts("SKIP clock store: sdmc:/ paths need a POSIX host");
    return 0;
#endif
    assert(mkdtemp(folder) != NULL && chdir(folder) == 0);
    assert(mkdir("sdmc:", 0700) == 0);
    assert(mkdir("sdmc:/3ds", 0700) == 0);
    assert(mkdir(CTR_DATA_DIR, 0700) == 0);
    memcpy(unchanged, save, sizeof(save));
    /* No save to bind a clock to (a new game): no status to report. */
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_NONE && CtrClockStore_LoadSerial() == 0);
    assert(!CtrClockStore_Load(NULL, 0));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_NONE && CtrClockStore_LoadSerial() == 1);
    assert(!CtrClockStore_NeedsNotice(CTR_CLOCK_STORE_NONE));
    /* An existing save without a clock uses the local RTC and gets a notice. */
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_MISSING && CtrClockStore_LoadSerial() == 2);
    assert(CtrClockStore_NeedsNotice(CTR_CLOCK_STORE_MISSING));
    /* NewGameInitData's hook clears a previous game's fallback status. */
    assert(!CtrClockStore_Load(NULL, 0));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_NONE);
    assert(!CtrClockStore_NeedsNotice(CtrClockStore_Status()));
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_MISSING);
    CtrDayNight_Update();
    sNow = 650;
    assert(CtrDayNight_Snapshot(&saved));
    assert(saved.timeMs == 43278000 && saved.berryBonusMs == 77350);
    assert(CtrClockStore_Save(save, sizeof(save)));
    ReadRecord(record);
    assert(memcmp(record, "EM3DCLK1", 8) == 0);
    sNow += 10000;
    sWall += 60;
    assert(CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_RESTORED);
    assert(!CtrClockStore_NeedsNotice(CTR_CLOCK_STORE_RESTORED));
    assert(CtrDayNight_Snapshot(&loaded));
    assert(loaded.timeMs == saved.timeMs + 60000);
    assert(loaded.berryBonusMs == saved.berryBonusMs);
    assert(CtrDayNight_TakeBerryMinutes() == 1);
    assert(CtrDayNight_TakeBerryMinutes() == 0);
    assert(CtrDayNight_Snapshot(&loaded) && loaded.berryBonusMs == 17350);
    sSpeed = 0;
    sNow += 1000;
    assert(CtrDayNight_Snapshot(&loaded));
    assert(loaded.timeMs == saved.timeMs + 61000 && loaded.berryBonusMs == 17350);
    sFailWrite = true;
    assert(!CtrClockStore_Save(save, sizeof(save)));
    sFailWrite = false;
    ReadRecord(probe);
    assert(memcmp(record, probe, 40) == 0);
    assert(memcmp(save, unchanged, sizeof(save)) == 0);
    save[123] ^= 1;
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_MISMATCH);
    assert(CtrClockStore_NeedsNotice(CTR_CLOCK_STORE_MISMATCH));
    save[123] ^= 1;
    assert(!CtrDayNight_BerryDue());
    memcpy(probe, record, 40);
    probe[20] ^= 1;
    WriteRecord(probe, 40);
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_INVALID);
    assert(CtrClockStore_NeedsNotice(CTR_CLOCK_STORE_INVALID));
    WriteRecord(record, 39);
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_INVALID);
    memcpy(probe, record, 40);
    probe[40] = 0;
    WriteRecord(probe, 41);
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_INVALID);
    memcpy(probe, record, 40);
    probe[12] = 2;
    UpdateChecksum(probe);
    WriteRecord(probe, 40);
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_INVALID);
    memcpy(probe, record, 40);
    memset(probe + 16, 0xFF, 4);
    UpdateChecksum(probe);
    WriteRecord(probe, 40);
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_INVALID);
    memcpy(probe, record, 40);
    memset(probe + 20, 0xFF, 8);
    UpdateChecksum(probe);
    WriteRecord(probe, 40);
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_INVALID);
    /* Reporting changed nothing on disk: the corrupt record is still there. */
    ReadRecord(probe);
    assert(probe[20] == 0xFF);
    WriteRecord(record, 40);
    sWall -= 3600;
    assert(CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_RESTORED);
    assert(CtrDayNight_Snapshot(&loaded) && loaded.timeMs == saved.timeMs);
    assert(loaded.berryBonusMs == saved.berryBonusMs);
    assert(memcmp(save, unchanged, sizeof(save)) == 0);
    assert(CtrClockStore_Save(save, sizeof(save)));
    assert(remove(CTR_DATA_DIR "emerald3ds.clock") == 0);
    assert(CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_RECOVERED);
    assert(!CtrClockStore_NeedsNotice(CTR_CLOCK_STORE_RECOVERED));
    assert(CtrDayNight_Snapshot(&loaded) && loaded.berryBonusMs == saved.berryBonusMs);
    /* A short main file and a backup of another save: the sound record wins
     * the report (mismatch), and the backup is still the fallback for this one. */
    WriteRecord(record, 39);
    save[7] ^= 1;
    assert(!CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_MISMATCH);
    save[7] ^= 1;
    assert(CtrClockStore_Load(save, sizeof(save)));
    assert(CtrClockStore_Status() == CTR_CLOCK_STORE_RECOVERED);
    assert(CtrDayNight_Snapshot(&loaded) && loaded.berryBonusMs == saved.berryBonusMs);
    assert(memcmp(save, unchanged, sizeof(save)) == 0);
    assert(CtrClockStore_Save(save, sizeof(save)));
    assert(remove(CTR_DATA_DIR "emerald3ds.clock") == 0);
    assert(remove(CTR_DATA_DIR "emerald3ds.clock.bak") == 0);
    assert(rmdir(CTR_DATA_DIR) == 0);
    assert(rmdir("sdmc:/3ds") == 0);
    assert(rmdir("sdmc:") == 0);
    assert(chdir("/tmp") == 0 && rmdir(folder) == 0);
    puts("PASS clock store: save binding, CRC, replacement without overwrite, backup recovery, write failure, corruption, fractions, backwards RTC, no save mutation, load status none/missing/restored/recovered/invalid/mismatch");
    return 0;
}
