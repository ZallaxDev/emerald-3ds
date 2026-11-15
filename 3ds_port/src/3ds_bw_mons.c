/*
 * The BW pictures, native side (3ds_bw_mons.h): the files read from the
 * card, one per slot (a battler, the Pokédex), and their frames decoded.
 *
 * A file is read by a thread of its own, never inside a frame: a picture of
 * two megabytes held an Old 3DS's frame for most of a second (13 FPS as a
 * Pokémon was sent out). CtrBwPic_Request asks for one as soon as the game
 * knows which (the battler's graphics loaded, the Pokédex's cursor moved);
 * CtrBwPic_Get hands it over once it is read, and NULL until then - the
 * game's own picture is shown meanwhile. The thread only reads into a
 * picture of its own; the main thread swaps it in and frees what it held.
 *
 * The pictures come from one file, CTR_BW_PAK, when it is there: an index
 * read once, then a seek and a read a picture. A folder of a thousand files
 * on the card's FAT took the better part of a second to open one in (the
 * Pokédex showed the game's picture that long). Loose files under CTR_BW_DIR
 * are read where there is no pack.
 *
 * The pack: "BWPK", u32 count, count entries of { char name[20] (as
 * "front/001_f_s", NUL-padded), u32 offset, u32 size } sorted by name, then
 * the pictures (BWS1) at their offsets from the start of the file.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "3ds_log.h"
#include "3ds_platform.h"
#include "3ds_bw_mons.h"

struct CtrBwPic
{
    char path[64];
    unsigned w, h, frames, anchorX2, feetY, topY, colours;
    uint32_t loopTicks;
    uint32_t palette[256];
    uint16_t *ticks;
    uint32_t *offsets;
    const uint8_t *data;
    uint32_t dataSize;
    uint8_t *file;
};

/* The main thread's: the picture each slot shows. */
static CtrBwPic sSlots[CTR_BW_SLOTS];
static bool sHeld[CTR_BW_SLOTS];

/* Shared with the reader, under sLock: the file each slot wants, the
 * picture read for it, and a file that could not be (not asked again). */
static LightLock sLock;
static LightEvent sWake;
static bool sStarted;
static char sWant[CTR_BW_SLOTS][64];
static CtrBwPic *sRead[CTR_BW_SLOTS];
static char sFailed[CTR_BW_SLOTS][64];

static unsigned U16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t U32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

typedef struct
{
    char name[20];
    uint32_t offset, size;
} PakEntry;

/* The pack's index, read once (PakIndex); the reader's own handle on it. */
static PakEntry *sPakIndex;
static unsigned sPakCount;
static bool sPakTried;
static FILE *sPakFile;

static void PakIndex(void)
{
    FILE *f;
    uint8_t head[8];

    if (sPakTried) return;
    sPakTried = true;
    f = fopen(CTR_BW_PAK, "rb");
    if (!f) return;
    if (fread(head, 1, 8, f) == 8 && !memcmp(head, "BWPK", 4))
    {
        unsigned count = U32(head + 4);
        uint8_t *raw = count && count < 65536 ? malloc(28u * count) : NULL;

        sPakIndex = raw ? malloc(sizeof(PakEntry) * count) : NULL;
        if (raw && sPakIndex && fread(raw, 28, count, f) == count)
        {
            for (unsigned i = 0; i < count; ++i)
            {
                memcpy(sPakIndex[i].name, raw + 28 * i, 20);
                sPakIndex[i].name[19] = 0;
                sPakIndex[i].offset = U32(raw + 28 * i + 20);
                sPakIndex[i].size = U32(raw + 28 * i + 24);
            }
            sPakCount = count;
            CtrLog_Write(CTR_LOG_FS, "bw: %u pictures in %s", count, CTR_BW_PAK);
        }
        else
        {
            free(sPakIndex);
            sPakIndex = NULL;
        }
        free(raw);
    }
    fclose(f);
}

/* A path under CTR_BW_DIR as the pack names it: "front/001_s". */
static bool PakKey(const char *path, char key[20])
{
    size_t dir = strlen(CTR_BW_DIR), n;

    if (strncmp(path, CTR_BW_DIR, dir)) return false;
    path += dir;
    n = strlen(path);
    if (n > 4 && !strcmp(path + n - 4, ".bin")) n -= 4;
    if (n >= 20) return false;
    memcpy(key, path, n);
    key[n] = 0;
    return true;
}

static const PakEntry *PakFind(const char *path)
{
    char key[20];
    unsigned lo = 0, hi = sPakCount;

    if (!sPakCount || !PakKey(path, key)) return NULL;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(sPakIndex[mid].name, key);

        if (!c) return &sPakIndex[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

bool CtrBwPic_Exists(const char *path)
{
    struct stat st;

    PakIndex();
    if (sPakCount) return PakFind(path) != NULL;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void Free(CtrBwPic *pic)
{
    free(pic->file);
    free(pic->ticks);
    free(pic->offsets);
    memset(pic, 0, sizeof(*pic));
}

static bool Parse(CtrBwPic *pic, uint8_t *file, long size)
{
    const uint8_t *p = file;
    long need;

    if (size < 16 || memcmp(p, "BWS1", 4)) return false;
    pic->w = U16(p + 4);
    pic->h = U16(p + 6);
    pic->frames = U16(p + 8);
    pic->anchorX2 = U16(p + 10);
    pic->feetY = U16(p + 12);
    pic->colours = U16(p + 14);
    if (!pic->w || !pic->h || pic->w > CTR_BW_MAX_DIM || pic->h > CTR_BW_MAX_DIM || !pic->frames
        || !pic->colours || pic->colours > 256)
        return false;
    need = 16 + 4L * pic->colours + 2L * pic->frames + 4L * (pic->frames + 1);
    if (size < need) return false;
    p += 16;
    for (unsigned i = 0; i < pic->colours; ++i, p += 4)
        pic->palette[i] = i ? U32(p) : 0;
    pic->ticks = malloc(sizeof(uint16_t) * pic->frames);
    pic->offsets = malloc(sizeof(uint32_t) * (pic->frames + 1));
    if (!pic->ticks || !pic->offsets) return false;
    pic->loopTicks = 0;
    for (unsigned i = 0; i < pic->frames; ++i, p += 2)
    {
        pic->ticks[i] = (uint16_t)U16(p);
        if (!pic->ticks[i]) pic->ticks[i] = 1;
        pic->loopTicks += pic->ticks[i];
    }
    for (unsigned i = 0; i <= pic->frames; ++i, p += 4)
        pic->offsets[i] = U32(p);
    pic->data = p;
    pic->dataSize = (uint32_t)(size - need);
    for (unsigned i = 0; i < pic->frames; ++i)
        if (pic->offsets[i] > pic->offsets[i + 1] || pic->offsets[i + 1] > pic->dataSize) return false;
    return true;
}

/* The first frame's highest opaque row, from its colour indices. */
static unsigned TopRow(const CtrBwPic *pic)
{
    const uint8_t *p = pic->data + pic->offsets[0], *end = pic->data + pic->offsets[1];
    unsigned n = 0, total = pic->w * pic->h;

    while (n < total && p < end)
    {
        unsigned c = *p++;

        if (c < 128)
        {
            for (unsigned k = 0; k <= c && p < end; ++k, ++n)
                if (*p++) return n / pic->w;
        }
        else if (p < end)
        {
            if (*p++) return n / pic->w;
            n += c - 126;
        }
    }
    return 0;
}

/* The reader's own: the file into a picture of its own, or NULL. */
static CtrBwPic *Read(const char *path)
{
    CtrBwPic *pic = calloc(1, sizeof(*pic));
    FILE *f;
    long size;

    if (!pic) return NULL;
    if (sPakCount)
    {
        const PakEntry *entry = PakFind(path);

        if (!entry) goto missing;
        if (!sPakFile) sPakFile = fopen(CTR_BW_PAK, "rb");
        if (!sPakFile) goto missing;
        size = (long)entry->size;
        pic->file = size > 0 && size < 8L * 1024 * 1024 ? malloc((size_t)size) : NULL;
        if (!pic->file || fseek(sPakFile, (long)entry->offset, SEEK_SET)
            || fread(pic->file, 1, (size_t)size, sPakFile) != (size_t)size || !Parse(pic, pic->file, size))
            goto broken;
    }
    else
    {
        f = fopen(path, "rb");
        if (!f) goto missing;
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        pic->file = size > 0 && size < 8L * 1024 * 1024 ? malloc((size_t)size) : NULL;
        if (!pic->file || fread(pic->file, 1, (size_t)size, f) != (size_t)size || !Parse(pic, pic->file, size))
        {
            fclose(f);
            goto broken;
        }
        fclose(f);
    }
    snprintf(pic->path, sizeof(pic->path), "%s", path);
    pic->topY = TopRow(pic);
    return pic;

broken:
    CtrLog_Write(CTR_LOG_ERROR, "bw: %s cannot be read as a BWS1 picture", path);
missing:
    Free(pic);
    free(pic);
    return NULL;
}

static void Reader(void *arg)
{
    (void)arg;
    for (;;)
    {
        LightEvent_Wait(&sWake);
        for (;;)
        {
            char path[64];
            int slot = -1;
            CtrBwPic *pic;

            LightLock_Lock(&sLock);
            for (int i = 0; i < CTR_BW_SLOTS && slot < 0; ++i)
                if (sWant[i][0] && strcmp(sWant[i], sFailed[i])
                    && !(sRead[i] && !strcmp(sRead[i]->path, sWant[i])))
                {
                    slot = i;
                    memcpy(path, sWant[i], sizeof(path));
                }
            LightLock_Unlock(&sLock);
            if (slot < 0) break;
            pic = Read(path);
            LightLock_Lock(&sLock);
            if (!pic)
                memcpy(sFailed[slot], path, sizeof(path));
            else if (!strcmp(sWant[slot], path))
            {
                if (sRead[slot])
                {
                    Free(sRead[slot]);
                    free(sRead[slot]);
                }
                sRead[slot] = pic;
                pic = NULL;
            }
            LightLock_Unlock(&sLock);
            if (pic)
            {
                Free(pic);
                free(pic);
            }
        }
    }
}

static bool Start(void)
{
    static bool tried;

    if (sStarted || tried) return sStarted;
    tried = true;
    LightLock_Init(&sLock);
    LightEvent_Init(&sWake, RESET_ONESHOT);
    PakIndex();
    /* On the system core where it can: the game's own frame never waits
     * for it, nor it for the game's. */
    sStarted = CtrPlatform_StartThread(Reader, NULL, 16 * 1024, 1);
    if (!sStarted) CtrLog_Write(CTR_LOG_ERROR, "bw: no thread to read the pictures with");
    return sStarted;
}

void CtrBwPic_Request(unsigned slot, const char *path)
{
    if (slot >= CTR_BW_SLOTS || !path || !path[0] || !Start()) return;
    if (sHeld[slot] && !strcmp(sSlots[slot].path, path)) return;
    LightLock_Lock(&sLock);
    if (strcmp(sWant[slot], path)) snprintf(sWant[slot], sizeof(sWant[slot]), "%s", path);
    LightLock_Unlock(&sLock);
    LightEvent_Signal(&sWake);
}

const CtrBwPic *CtrBwPic_Get(unsigned slot, const char *path)
{
    CtrBwPic *pic = NULL;

    if (slot >= CTR_BW_SLOTS || !path || !path[0]) return NULL;
    if (sHeld[slot] && !strcmp(sSlots[slot].path, path)) return &sSlots[slot];
    if (!Start()) return NULL;
    LightLock_Lock(&sLock);
    if (sRead[slot] && !strcmp(sRead[slot]->path, path))
    {
        pic = sRead[slot];
        sRead[slot] = NULL;
    }
    LightLock_Unlock(&sLock);
    if (!pic)
    {
        CtrBwPic_Request(slot, path);
        return NULL;
    }
    if (sHeld[slot]) Free(&sSlots[slot]);
    sSlots[slot] = *pic;
    free(pic);
    sHeld[slot] = true;
    return &sSlots[slot];
}

void CtrBwPic_Release(unsigned slot)
{
    if (slot >= CTR_BW_SLOTS) return;
    if (sHeld[slot]) Free(&sSlots[slot]);
    sHeld[slot] = false;
    if (!sStarted) return;
    LightLock_Lock(&sLock);
    sWant[slot][0] = 0;
    sFailed[slot][0] = 0;
    if (sRead[slot])
    {
        Free(sRead[slot]);
        free(sRead[slot]);
        sRead[slot] = NULL;
    }
    LightLock_Unlock(&sLock);
}

void CtrBwPic_Size(const CtrBwPic *pic, unsigned *w, unsigned *h, unsigned *anchorX2, unsigned *feetY)
{
    *w = pic->w;
    *h = pic->h;
    *anchorX2 = pic->anchorX2;
    *feetY = pic->feetY;
}

unsigned CtrBwPic_TopY(const CtrBwPic *pic)
{
    return pic->topY;
}

unsigned CtrBwPic_FrameAt(const CtrBwPic *pic, uint32_t tick)
{
    tick %= pic->loopTicks;
    for (unsigned i = 0; i < pic->frames; ++i)
    {
        if (tick < pic->ticks[i]) return i;
        tick -= pic->ticks[i];
    }
    return 0;
}

void CtrBwPic_Decode(const CtrBwPic *pic, unsigned frame, uint32_t *out, unsigned stride)
{
    const uint8_t *p = pic->data + pic->offsets[frame], *end = pic->data + pic->offsets[frame + 1];
    const unsigned w = pic->w, rows = pic->h;
    unsigned x = 0, y = 0;
    uint32_t *row = out;

    /* Row by row, without a division a pixel: the runs cross rows. */
    while (y < rows && p < end)
    {
        unsigned c = *p++, run;
        bool literal = c < 128;
        uint32_t colour = 0;

        run = literal ? c + 1 : c - 126;
        if (!literal)
        {
            if (p >= end) break;
            colour = pic->palette[*p++];
        }
        while (run && y < rows)
        {
            unsigned n = w - x < run ? w - x : run;

            if (literal)
            {
                if ((unsigned)(end - p) < n) n = (unsigned)(end - p);
                if (!n) break;
                for (unsigned k = 0; k < n; ++k)
                    row[x + k] = pic->palette[p[k]];
                p += n;
            }
            else
                for (unsigned k = 0; k < n; ++k)
                    row[x + k] = colour;
            x += n;
            run -= n;
            if (x == w)
            {
                x = 0;
                ++y;
                row += stride;
            }
        }
        if (literal && run) break;
    }
    /* What the data did not reach is transparent. */
    for (; y < rows; ++y, row += stride, x = 0)
        for (; x < w; ++x)
            row[x] = 0;
}
