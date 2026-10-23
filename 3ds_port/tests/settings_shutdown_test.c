/* Run the real saver while its first SD write is deliberately stalled. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <3ds.h>
static int TestClose(FILE *file);
static int TestRemove(const char *path);
static int TestRename(const char *from, const char *to);
#define fclose TestClose
#define remove TestRemove
#define rename TestRename
#include "../src/3ds_settings.c"
#undef fclose
#undef remove
#undef rename
static unsigned renames;
static int TestRemove(const char *path) { (void)path; return 0; }
static int TestRename(const char *from, const char *to) {
    assert(strcmp(from, "sdmc:/3ds/emerald3ds/settings.txt.tmp") == 0);
    assert(strcmp(to, "sdmc:/3ds/emerald3ds/settings.txt") == 0);
    ++renames; return 0;
}
static pthread_mutex_t diskMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t diskCond = PTHREAD_COND_INITIALIZER;
static bool entered, releaseDisk;
static unsigned writes, errors;
static unsigned clockUpdates;
static int fault;
static char saved[256];
void CtrDayNight_Update(void) {
    assert(CtrSettings_GetInt("clock_speed", 0) == 0);
    ++clockUpdates;
}
FILE *CtrFs_OpenData(const char *name, const char *mode) {
    assert(strcmp(name, "settings.txt.tmp") == 0 && strcmp(mode, "w") == 0);
    pthread_mutex_lock(&diskMutex);
    entered = true; pthread_cond_signal(&diskCond);
    while (!releaseDisk) pthread_cond_wait(&diskCond, &diskMutex);
    pthread_mutex_unlock(&diskMutex);
    return fault == 1 ? NULL : tmpfile();
}
static int TestClose(FILE *file) {
    rewind(file); size_t n = fread(saved, 1, sizeof(saved)-1, file); saved[n] = 0;
    ++writes; int rc = fclose(file); return fault == 2 ? -1 : rc;
}
void CtrLog_Write(CtrLogCategory category, const char *fmt, ...) {
    (void)fmt; if (category == CTR_LOG_ERROR) ++errors;
}
static void *SlowCard(void *unused) {
    (void)unused; struct timespec delay = {1, 0}; nanosleep(&delay, NULL);
    pthread_mutex_lock(&diskMutex); releaseDisk = true;
    pthread_cond_signal(&diskCond); pthread_mutex_unlock(&diskMutex); return NULL;
}
/* Every extra key at its longest round-trips through the saved text; one past
 * the table is refused, logged once. No card I/O: Text and ApplyLine only. */
static void ExtraCapacity(void) {
    static char text[sizeof(sSaveText)];
    char key[32], line[64];
    CtrSettings_Defaults();
    for (int i = 0; i < EXTRA_KEYS; ++i) {
        snprintf(line, sizeof(line), "extra_key_padded_to_%03d=%d", i, i == 0 ? -2147483647 - 1 : -1000000 - i);
        CtrSettings_ApplyLine(line);
    }
    errors = 0;
    CtrSettings_ApplyLine("one_key_past_the_table=7");
    CtrSettings_ApplyLine("two_keys_past_the_table=7");
    assert(errors == 1 && CtrSettings_GetInt("one_key_past_the_table", -5) == -5);
    int used = CtrSettings_Text(text, sizeof(text));
    assert(used > 0 && used < (int)sizeof(text));
    CtrSettings_Defaults();
    for (char *at = text, *end; *at != '\0'; at = end + 1) {
        end = strchr(at, '\n'); assert(end != NULL && end - at < (int)sizeof(line));
        memcpy(line, at, end - at); line[end - at] = '\0';
        CtrSettings_ApplyLine(line);
    }
    for (int i = 0; i < EXTRA_KEYS; ++i) {
        snprintf(key, sizeof(key), "extra_key_padded_to_%03d", i);
        assert(strlen(key) == 23 && CtrSettings_GetInt(key, 1) == (i == 0 ? -2147483647 - 1 : -1000000 - i));
    }
    CtrSettings_Defaults();
    errors = 0;
}
int main(int argc, char **argv) {
    fault = argc > 1 ? argv[1][0] - '0' : 0;
    ExtraCapacity();
    CtrSettings_SetVoxel(true);
    pthread_mutex_lock(&diskMutex);
    while (!entered) pthread_cond_wait(&diskCond, &diskMutex);
    pthread_mutex_unlock(&diskMutex);
    /* Queue newer text while the worker owns an older snapshot. */
    CtrSettings_SetVoxelBlur(false); CtrSettings_SetShowFps(true);
    CtrSettings_SetInt("day_night", 1); CtrSettings_SetInt("clock_speed", 5);
    CtrSettings_SetInt("timed_encounters", 1); CtrSettings_SetInt("show_clock", 1);
    assert(clockUpdates == 1);
    CtrSettings_SetInt("clock_speed", 5);
    assert(clockUpdates == 1);
    assert(CtrSettings_GetInt("timed_encounters", 0) == 1 && CtrSettings_GetInt("show_clock", 0) == 1);
    assert(CtrSettings_GetInt("day_night", 0) == 1 && CtrSettings_GetInt("clock_speed", 0) == 5);
    pthread_t card; assert(pthread_create(&card, NULL, SlowCard, NULL) == 0);
    CtrSettings_Shutdown(); pthread_join(card, NULL);
    assert(sSaver == NULL && sSaveQuit && !sSavePending);
    assert(releaseDisk);
    if (fault == 1) assert(errors == 2 && writes == 0);
    else {
        assert(writes == 2 && strstr(saved, "voxel=1\n") && strstr(saved, "voxel_blur=0\n") && strstr(saved, "fps=1\n"));
        assert(strstr(saved, "day_night=1\n") && strstr(saved, "clock_speed=5\n"));
        assert(strstr(saved, "timed_encounters=1\n") && strstr(saved, "show_clock=1\n"));
        assert(errors == (fault == 2 ? 2u : 0u));
        assert(renames == (fault == 2 ? 0u : 2u));
    }
    CtrSettings_Shutdown(); /* idempotent, no freed-thread access */
    puts("PASS settings: extra keys round trip at capacity, stalled write, latest snapshot drained, close and open errors, repeated shutdown");
    return 0;
}
