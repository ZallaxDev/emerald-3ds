#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "3ds_save_io.h"

#undef fwrite
size_t fwrite(const void *, size_t, size_t, FILE *);

static bool failWrite;

size_t CtrTestWrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (failWrite && count > 0)
        --count;
    return fwrite(data, size, count, file);
}

/* CtrSave_Replace shares the production module; this test uses the flash API. */
FILE *CtrFs_OpenData(const char *name, const char *mode)
{
    return fopen(name, mode);
}

int main(int argc, char **argv)
{
    unsigned char flash[16], disk[18];
    static const unsigned char update[] = {0x17, 0x23};
    FILE *file;
    assert(argc == 2);
    const char *path = argv[1];

    memset(flash, 0xFF, sizeof(flash));
    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(flash, 1, sizeof(flash), file) == sizeof(flash));
    assert(fwrite("RT", 1, 2, file) == 2);
    assert(fclose(file) == 0);

    failWrite = true;
    assert(!CtrFlash_WriteRange(path, flash, sizeof(flash), 2, 8, update, sizeof(update)));
    for (size_t i = 0; i < sizeof(flash); i++)
        assert(flash[i] == 0xFF);
    file = fopen(path, "rb");
    assert(file != NULL);
    assert(fread(disk, 1, sizeof(disk), file) == sizeof(disk));
    assert(fclose(file) == 0);
    assert(memcmp(disk, flash, 8) == 0);
    assert(memcmp(disk + 16, "RT", 2) == 0);

    failWrite = false;
    assert(CtrFlash_WriteRange(path, flash, sizeof(flash), 2, 8, update, sizeof(update)));
    assert(!CtrFlash_WriteRange(path, flash, sizeof(flash), 2, 15, update, sizeof(update)));
    file = fopen(path, "rb");
    assert(file != NULL);
    assert(fread(disk, 1, sizeof(disk), file) == sizeof(disk));
    assert(fgetc(file) == EOF && fclose(file) == 0);
    assert(memcmp(flash, disk, sizeof(flash)) == 0);
    assert(memcmp(disk + 8, update, sizeof(update)) == 0);
    assert(memcmp(disk + 16, "RT", 2) == 0);
    remove(path);

    /* Newly created or short images are extended from the mirror. */
    memset(flash, 0xFF, sizeof(flash));
    file = fopen(path, "wb");
    assert(file != NULL && fwrite("OLD", 1, 3, file) == 3);
    assert(fclose(file) == 0);
    assert(CtrFlash_WriteRange(path, flash, sizeof(flash), 2, 8, update, sizeof(update)));
    file = fopen(path, "rb");
    assert(file != NULL && fread(disk, 1, sizeof(flash), file) == sizeof(flash));
    assert(fgetc(file) == EOF && fclose(file) == 0);
    assert(memcmp(disk, "OLD", 3) == 0);
    assert(memcmp(disk + 3, flash + 3, sizeof(flash) - 3) == 0);
    remove(path);
    puts("PASS flash write: short write, unchanged mirror and older sectors, retry, and trailer preservation");
    return 0;
}
