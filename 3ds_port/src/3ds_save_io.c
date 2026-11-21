#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "3ds_data.h"
#include "3ds_platform.h"
#include "3ds_save_io.h"

static bool DataPath(char *path, size_t size, const char *name)
{
    int length = snprintf(path, size, "%s%s", CTR_DATA_DIR, name);
    return length >= 0 && (size_t)length < size;
}

bool CtrSave_Replace(const char *name, const char *temp, const char *backup,
                     const void *data, size_t size)
{
    char targetPath[128], tempPath[128], backupPath[128];
    FILE *file;
    bool ok;

    if (!DataPath(targetPath, sizeof(targetPath), name)
     || !DataPath(tempPath, sizeof(tempPath), temp)
     || !DataPath(backupPath, sizeof(backupPath), backup))
        return false;

    file = CtrFs_OpenData(temp, "wb");
    if (file == NULL)
        return false;
    ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
    if (fclose(file) != 0)
        ok = false;
    if (!ok)
    {
        remove(tempPath);
        return false;
    }
    if (rename(tempPath, targetPath) == 0)
        return true;

    /* Some FAT implementations refuse to replace an existing destination. */
    file = CtrFs_OpenData(name, "rb");
    if (file == NULL)
        return false;  /* a complete temp file remains available for recovery */
    fclose(file);
    remove(backupPath);
    if (rename(targetPath, backupPath) == 0)
    {
        if (rename(tempPath, targetPath) == 0)
            return true;
        if (rename(backupPath, targetPath) != 0)
            return false;  /* preserve both recovery candidates */
    }
    remove(tempPath);
    return false;
}

bool CtrFlash_WriteRange(const char *path, void *flash, size_t flashSize, size_t trailerMax,
                        size_t offset, const void *data, size_t size)
{
    unsigned char *mirror = flash;
    FILE *file;
    long fileSize;
    bool ok;

    if (path == NULL || flash == NULL || data == NULL || offset > LONG_MAX || offset > flashSize || size > flashSize - offset)
        return false;
    file = fopen(path, "r+b");
    if (file == NULL && errno == ENOENT)
        file = fopen(path, "w+b");
    if (file == NULL)
        return false;

    fileSize = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    ok = fileSize >= 0 && ((size_t)fileSize <= flashSize
                         || (size_t)fileSize - flashSize <= trailerMax);
    if (ok && (size_t)fileSize < flashSize)
    {
        size_t remaining = flashSize - (size_t)fileSize;
        ok = fwrite(mirror + fileSize, 1, remaining, file) == remaining;
    }
    if (ok)
        ok = fseek(file, (long)offset, SEEK_SET) == 0 && fwrite(data, 1, size, file) == size;
    if (ok)
        ok = fflush(file) == 0;
    if (fclose(file) != 0)
        ok = false;
    if (ok)
        memmove(mirror + offset, data, size);
    return ok;
}
