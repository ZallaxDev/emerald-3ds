/*
 * The stereoscopic Pokémon planes, native side: the two planes files
 * (3ds_stereo_mon.h), their headers and indexes resident, an entry's planes
 * read on request. The battle (3ds_stereo_battle.c) composes and draws them.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "3ds_platform.h"
#include "3ds_log.h"
#include "3ds_stereo_mon.h"

/* A planes file: its header and index, resident. The Pokémon's fronts,
 * and the battle's other pictures (the backs file). */
typedef struct
{
    const char *path;
    uint8_t index[CTR_STEREO_MON_HEADER_BYTES + CTR_STEREO_MON_INDEX_BYTES * CTR_STEREO_MON_MAX_COUNT];
    uint32_t indexBytes, fileSize;
    CtrStereoMonHeader header;
    bool loaded, tried;
} PlanesFile;
static PlanesFile sFront = {.path = CTR_STEREO_MON_FILE}, sBacks = {.path = CTR_STEREO_BACKS_FILE};
/* Shorthands for the fronts file. */
#define sIndex sFront.index
#define sIndexBytes sFront.indexBytes
#define sHeader sFront.header
#define sLoaded sFront.loaded
static bool LoadFile(PlanesFile *pf)
{
    FILE *f;
    long size;

    if (pf->tried) return pf->loaded;
    pf->tried = true;
    f = CtrFs_OpenAsset(pf->path);
    if (f == NULL)
    {
        CtrLog_Write(CTR_LOG_VIDEO, "stereo: no %s", pf->path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    pf->indexBytes = (uint32_t)fread(pf->index, 1, sizeof(pf->index), f);
    fclose(f);
    if (size > 0 && CtrStereoMon_ParseHeader(pf->index, pf->indexBytes, &pf->header)
        && pf->header.count <= CTR_STEREO_MON_MAX_COUNT)
    {
        /* A build without a plane source writes the header alone: no
         * planes, nothing to say. */
        if (pf->header.count == 0)
        {
            CtrLog_Write(CTR_LOG_VIDEO, "stereo: %s holds no planes", pf->path);
            return false;
        }
        pf->fileSize = (uint32_t)size;
        pf->loaded = true;
        CtrLog_Write(CTR_LOG_VIDEO, "stereo: %s: %u entries, %u planes, up to %u frames (%ld bytes)", pf->path,
                     pf->header.count, pf->header.planes, pf->header.frames, size);
    }
    else
        CtrLog_Write(CTR_LOG_ERROR, "stereo: %s is not a planes file (%ld bytes)", pf->path, size);
    return pf->loaded;
}

bool CtrStereoMon_Load(void) { return LoadFile(&sFront); }

unsigned CtrStereoMon_Count(void) { return sLoaded ? sHeader.count : 0; }

uint16_t CtrStereoMon_NationalAt(unsigned index)
{
    uint16_t national = 0;

    if (sLoaded) CtrStereoMon_IndexEntry(sIndex, sIndexBytes, &sHeader, index, &national, NULL);
    return national;
}

bool CtrStereoMon_Has(uint16_t national)
{
    return sLoaded && CtrStereoMon_Find(sIndex, sIndexBytes, &sHeader, national) >= 0;
}

const CtrStereoMonHeader *CtrStereoMon_Header(void) { return sLoaded ? &sHeader : NULL; }
const CtrStereoMonHeader *CtrStereoMon_BacksHeader(void) { return LoadFile(&sBacks) ? &sBacks.header : NULL; }

/* One entry's planes from a file into `out` (`size` bytes), with its frames. */
static bool ReadPlanesInto(PlanesFile *pf, unsigned index, uint8_t *out, uint32_t size, unsigned *frames)
{
    uint32_t offset, bytes;
    unsigned entryFrames = CtrStereoMon_EntryFrames(pf->index, pf->indexBytes, &pf->header, index);
    FILE *f;
    bool ok;

    bytes = CtrStereoMon_FramesBytes(&pf->header, entryFrames);
    if (entryFrames == 0 || !CtrStereoMon_IndexEntry(pf->index, pf->indexBytes, &pf->header, index, NULL, &offset)
        || offset > pf->fileSize || (uint64_t)offset + bytes > pf->fileSize || bytes > size)
        return false;
    f = CtrFs_OpenAsset(pf->path);
    if (f == NULL) return false;
    ok = fseek(f, (long)offset, SEEK_SET) == 0 && fread(out, 1, bytes, f) == bytes;
    fclose(f);
    if (ok && frames) *frames = entryFrames;
    return ok;
}

bool CtrStereoMon_PlanesFor(uint16_t national, uint8_t *out, uint32_t size)
{
    int index;

    if (!CtrStereoMon_Load()) return false;
    index = CtrStereoMon_Find(sIndex, sIndexBytes, &sHeader, national);
    return index >= 0 && ReadPlanesInto(&sFront, (unsigned)index, out, size, NULL);
}

bool CtrStereoMon_HasBack(uint16_t id)
{
    return LoadFile(&sBacks) && CtrStereoMon_Find(sBacks.index, sBacks.indexBytes, &sBacks.header, id) >= 0;
}

bool CtrStereoMon_BackPlanesFor(uint16_t id, uint8_t *out, uint32_t size, unsigned *frames)
{
    int index;

    if (!LoadFile(&sBacks)) return false;
    index = CtrStereoMon_Find(sBacks.index, sBacks.indexBytes, &sBacks.header, id);
    return index >= 0 && ReadPlanesInto(&sBacks, (unsigned)index, out, size, frames);
}
