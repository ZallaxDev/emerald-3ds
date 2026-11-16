// HOME Menu forwarder for Emerald 3DS.
//
// Installed once as a CIA, it starts sdmc:/3ds/emerald3ds/Emerald3DS.3dsx, so
// updating the game only ever replaces the 3DSX. It asks Luma3DS's hb:ldr to
// load that file, makes its own title the one Luma loads 3DSX files through
// (no Homebrew Launcher title has to be installed) and restarts itself, so Luma
// starts the game in its place. The game puts Luma's previous title back as
// soon as it starts (main_3ds.c), so the next start runs this code again.

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define TARGET_SD_PATH "/3ds/emerald3ds/Emerald3DS.3dsx"
#define TARGET_ARGV0   "sdmc:" TARGET_SD_PATH
#define RESTORE_ARG    "emerald3ds-forwarder:hbldr-tid="
#define FORWARDER_TID  0x000400000E3D5200ULL

// Luma3DS shared config page (private layout, stable since v10).
typedef struct {
    u64 hbldr3dsxTid;
    u64 selectedHbldr3dsxTid;
    bool useHbldr;
} LumaSharedConfig;
#define LUMA_SHARED_CONFIG ((volatile LumaSharedConfig *)(OS_SHAREDCFG_VADDR + 0x800))

// hb:ldr reads whole fixed-size buffers, whatever size is declared.
static char sTarget[0x400] __attribute__((aligned(4)));
static u32 sArgv[0x400 / 4];

static Result HbldrSetTarget(Handle h, const char *path)
{
    size_t length = strlen(path);
    if (length >= sizeof(sTarget))
        return -1;

    size_t size = length + 1;
    memcpy(sTarget, path, size);
    memset(sTarget + size, 0, sizeof(sTarget) - size);
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(size, 0);
    cmd[2] = (u32)sTarget;
    Result res = svcSendSyncRequest(h);
    return R_FAILED(res) ? res : (Result)cmd[1];
}

// argv[0] is the game's path (its RomFS), argv[1] the title to restore.
static Result HbldrSetArgv(Handle h, const char *argv0, u64 restoreTid)
{
    const size_t capacity = sizeof(sArgv) - sizeof(sArgv[0]);
    size_t pathLength = strlen(argv0);
    char *args = (char *)&sArgv[1];

    if (pathLength >= capacity)
        return -1;
    size_t pathSize = pathLength + 1;
    size_t remaining = capacity - pathSize;
    int written = snprintf(args + pathSize, remaining, RESTORE_ARG "%016llX",
                           (unsigned long long)restoreTid);
    if (written < 0 || (size_t)written >= remaining)
        return -1;

    memcpy(args, argv0, pathSize);
    sArgv[0] = 2;
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(3, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(sizeof(sArgv), 1);
    cmd[2] = (u32)sArgv;
    Result res = svcSendSyncRequest(h);
    return R_FAILED(res) ? res : (Result)cmd[1];
}

static bool IsLuma(void)
{
    s64 version;
    return R_SUCCEEDED(svcGetSystemInfo(&version, 0x10000, 0));
}

// Prepares hb:ldr and Luma's 3DSX title; on success main() returns and
// libctru's chainloader restarts this title, which Luma loads as the game.
static Result Forward(const char **why, u64 *chainTid)
{
    struct stat st;
    if (stat(TARGET_ARGV0, &st) != 0)
    {
        *why = "The game is not on the SD card.\n\n"
               "Copy the 3ds folder of your installation ZIP\n"
               "to the root of the SD card, so that this file\n"
               "exists:\n\n  " TARGET_ARGV0;
        return -1;
    }
    if (!IsLuma() || !LUMA_SHARED_CONFIG->useHbldr)
    {
        *why = "This shortcut needs Luma3DS.\n\n"
               "Start the game from the Homebrew Launcher.";
        return -1;
    }

    u64 selfTid;
    if (R_FAILED(APT_GetProgramID(&selfTid)))
        selfTid = FORWARDER_TID;
    u64 previousTid = LUMA_SHARED_CONFIG->selectedHbldr3dsxTid;

    Handle hbldr;
    // A global named port, not an srv service (srv would wait for it forever).
    Result res = svcConnectToPort(&hbldr, "hb:ldr");
    if (R_SUCCEEDED(res))
    {
        res = HbldrSetTarget(hbldr, TARGET_SD_PATH);
        if (R_SUCCEEDED(res))
            res = HbldrSetArgv(hbldr, TARGET_ARGV0, previousTid);
        svcCloseHandle(hbldr);
    }
    if (R_FAILED(res))
    {
        *why = "Luma3DS refused to load the game (hb:ldr).\n\n"
               "Update Luma3DS, or start the game from the\n"
               "Homebrew Launcher.";
        return res;
    }

    // Luma's PM makes the selected title current when this process exits;
    // the restart is then loaded as the 3DSX.
    LUMA_SHARED_CONFIG->selectedHbldr3dsxTid = selfTid;
    *chainTid = selfTid;
    return 0;
}

int main(void)
{
    const char *why = NULL;
    u64 chainTid = 0;
    Result res = Forward(&why, &chainTid);

    if (R_SUCCEEDED(res))
    {
        aptSetChainloader(chainTid, MEDIATYPE_SD);
        return 0;
    }
    else
    {
        gfxInitDefault();
        consoleInit(GFX_TOP, NULL);
        printf("\n Emerald 3DS\n\n");
        printf(" %s\n", why);
        if (res != -1)
            printf("\n Error 0x%08lX\n", (unsigned long)res);
        printf("\n\n Press START to exit.\n");
        while (aptMainLoop())
        {
            hidScanInput();
            if (hidKeysDown() & KEY_START)
                break;
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
    }
    gfxExit();
    return 0;
}
