/* Run the real warm worker with a controlled disk and joinable host thread. */
#include <assert.h>
#include <pthread.h>
#include "../src/3ds_assets.c"

struct CtrThreadTag
{
    pthread_t id;
    void (*entry)(void *);
    void *arg;
    bool finished;
};

static struct CtrThreadTag sHostThread;
static pthread_mutex_t sDiskLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sDiskChanged = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t sCacheLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sEditLock = PTHREAD_MUTEX_INITIALIZER;
static bool sBlockRead, sReading, sFailStart;
static unsigned sStarts, sJoins, sReads, sErrors;
unsigned char g_PortLogActive;

static void *Run(void *arg)
{
    struct CtrThreadTag *thread = arg;

    thread->entry(thread->arg);
    pthread_mutex_lock(&sDiskLock);
    thread->finished = true;
    pthread_cond_broadcast(&sDiskChanged);
    pthread_mutex_unlock(&sDiskLock);
    return NULL;
}

CtrThread CtrPlatform_StartJoinable(void (*entry)(void *), void *arg, unsigned stack, int core)
{
    (void)stack;
    (void)core;
    if (sFailStart)
        return NULL;

    assert(sStarts == sJoins);
    ++sStarts;
    sHostThread.entry = entry;
    sHostThread.arg = arg;
    sHostThread.finished = false;
    assert(pthread_create(&sHostThread.id, NULL, Run, &sHostThread) == 0);
    return &sHostThread;
}

bool CtrPlatform_JoinThread(CtrThread thread, unsigned timeoutMs)
{
    bool finished;

    assert(thread == &sHostThread);
    assert(timeoutMs == 0 || timeoutMs == WARM_JOIN_MS);
    pthread_mutex_lock(&sDiskLock);
    finished = thread->finished;
    pthread_mutex_unlock(&sDiskLock);
    if (!finished)
        return false;
    assert(pthread_join(thread->id, NULL) == 0);
    ++sJoins;
    return true;
}

static void WaitFor(bool *flag)
{
    pthread_mutex_lock(&sDiskLock);
    while (!*flag)
        pthread_cond_wait(&sDiskChanged, &sDiskLock);
    pthread_mutex_unlock(&sDiskLock);
}

void CtrLock_Init(CtrLock *lock) { assert(lock == &sLock); }
void CtrCond_Init(CtrCond *cond) { assert(cond == &sPublished); }
void CtrCond_Broadcast(CtrCond *cond) { assert(cond == &sPublished); }
void CtrLock_Lock(CtrLock *lock) { pthread_mutex_lock(lock == &sLock ? &sCacheLock : &sEditLock); }
void CtrLock_Unlock(CtrLock *lock) { pthread_mutex_unlock(lock == &sLock ? &sCacheLock : &sEditLock); }
uint64_t CtrPlatform_Ticks(void) { return 0; }
float CtrPlatform_TickMs(uint64_t ticks) { return (float)ticks; }
void Port_TransformAsset(const char *path, unsigned char *data, unsigned size)
{
    (void)path;
    (void)data;
    (void)size;
}

void CtrLog_Write(CtrLogCategory category, const char *fmt, ...)
{
    (void)fmt;
    if (category == CTR_LOG_ERROR)
        ++sErrors;
}

void Port_Log_Printf(const char *fmt, ...) { (void)fmt; }

bool CtrData_Locate(const char *path, uint64_t *offset, uint32_t *size, uint32_t *crc)
{
    static const uint8_t payload = 42;

    *offset = strcmp(path, "a") == 0 ? 0 : WARM_RUN * 2;
    *size = 1;
    *crc = CtrPak_Crc32(0, &payload, 1);
    return true;
}

bool CtrData_ReadRange(uint64_t offset, void *dest, uint32_t size)
{
    assert(offset == 0 || offset == WARM_RUN * 2);
    assert(size == 1);
    pthread_mutex_lock(&sDiskLock);
    ++sReads;
    sReading = true;
    pthread_cond_broadcast(&sDiskChanged);
    while (sBlockRead)
        pthread_cond_wait(&sDiskChanged, &sDiskLock);
    pthread_mutex_unlock(&sDiskLock);
    *(uint8_t *)dest = 42;
    return true;
}

CtrDataRead CtrData_ReadFile(const char *path, void *dest, uint32_t size)
{
    (void)path;
    (void)dest;
    (void)size;
    assert(false);  /* The pack fixture must take the ranged read path. */
    return CTR_DATA_READ_IO;
}

const char *CtrData_ReadName(CtrDataRead result)
{
    (void)result;
    return "unexpected read failure";
}

int main(void)
{
    static struct AssetMapEntry entries[] = {{0, 1, 0}, {0, 1, 2}};
    static struct AssetPayload payloads[2];
    static char paths[] = "a\0b";
    static uint8_t states[2];

    sTried = sLoaded = true;
    sEntries = entries;
    sEntryCount = 2;
    sPathBlob = paths;
    sPathBlobSize = sizeof(paths);
    sPayloads = payloads;
    sState = states;

    assert(CtrAssets_StopWarmup());
    sFailStart = true;
    CtrAssets_StartWarmup();
    assert(!CtrAssets_WarmingUp() && sStarts == 0 && sErrors == 1);
    sFailStart = false;

    sBlockRead = true;
    CtrAssets_StartWarmup();
    WaitFor(&sReading);
    assert(CtrAssets_WarmingUp() && sStarts == 1);
    assert(!CtrAssets_StopWarmup() && sErrors == 2);
    CtrAssets_StartWarmup();
    assert(CtrAssets_WarmingUp() && sStarts == 1 && sJoins == 0);
    pthread_mutex_lock(&sDiskLock);
    sBlockRead = false;
    pthread_cond_broadcast(&sDiskChanged);
    pthread_mutex_unlock(&sDiskLock);
    WaitFor(&sHostThread.finished);
    assert(!CtrAssets_WarmingUp());
    assert(CtrAssets_StopWarmup() && CtrAssets_StopWarmup());
    assert(sReads == 1 && states[0] == PAYLOAD_READY && states[1] == PAYLOAD_EMPTY);

    CtrAssets_StartWarmup();
    WaitFor(&sHostThread.finished);
    assert(!CtrAssets_WarmingUp() && sStarts == 2 && sJoins == 1);
    assert(sReads == 2 && states[1] == PAYLOAD_READY && sErrors == 2);
    CtrAssets_StartWarmup();  /* A finished worker is reaped before restarting. */
    WaitFor(&sHostThread.finished);
    assert(CtrAssets_StopWarmup() && sStarts == 3 && sJoins == 3 && sReads == 2);

    free(payloads[0].data);
    free(payloads[1].data);
    puts("PASS asset warm-up: start failure, stop during read, join retry and restart");
    return 0;
}
