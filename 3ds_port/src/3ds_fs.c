#include <3ds.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include "3ds_platform.h"

static bool sMounted;

static bool RelativePathValid(const char *path)
{
    if (!path || !*path || *path == '/' || strchr(path, ':') || strchr(path, '\\'))
        return false;
    /* Public API only accepts paths inside its chosen mount. */
    const char *p = path;
    while (*p)
    {
        const char *end = strchr(p, '/');
        size_t size = end ? (size_t)(end - p) : strlen(p);
        if (size == 0 || (size == 1 && p[0] == '.') || (size == 2 && p[0] == '.' && p[1] == '.'))
            return false;
        if (!end)
            break;
        p = end + 1;
    }
    return true;
}

static FILE *Open(const char *base, const char *path, const char *mode)
{
    char full[512];
    if (!RelativePathValid(path) || !mode)
    {
        errno = EINVAL;
        CtrLog_Write(CTR_LOG_ERROR, "invalid filesystem path/mode");
        return NULL;
    }
    int length = snprintf(full, sizeof(full), "%s%s", base, path);
    if (length < 0 || (size_t)length >= sizeof(full))
    {
        errno = length < 0 ? EINVAL : ENAMETOOLONG;
        return NULL;
    }
    FILE *file = fopen(full, mode);
    if (!file)
        CtrLog_Write(CTR_LOG_ERROR, "open %s: errno=%d", full, errno);
    return file;
}

bool CtrFs_Init(void)
{
    Result result = romfsInit();
    sMounted = R_SUCCEEDED(result);
    if (!sMounted)
    {
        CtrLog_Write(CTR_LOG_ERROR, "romfsInit: %08lx", (unsigned long)result);
        return false;
    }
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/emerald3ds", 0777);
    FILE *file = CtrFs_OpenAsset("boot.txt");
    char contents[64] = {0};
    bool valid = file && fgets(contents, sizeof(contents), file)
        && strcmp(contents, "Emerald3DS RomFS v1\n") == 0;
    if (file)
        fclose(file);
    CtrLog_Write(valid ? CTR_LOG_FS : CTR_LOG_ERROR, "RomFS boot marker %s", valid ? "PASS" : "FAIL");
    return valid;
}

void CtrFs_Shutdown(void)
{
    if (sMounted)
        romfsExit();
    sMounted = false;
}

FILE *CtrFs_OpenAsset(const char *relativePath)
{
    if (!sMounted)
    {
        errno = ENODEV;
        return NULL;
    }
    return Open("romfs:/", relativePath, "rb");
}

FILE *CtrFs_OpenData(const char *relativePath, const char *mode)
{
    return Open("sdmc:/3ds/emerald3ds/", relativePath, mode);
}
