/*
 * The stereoscopic Pokémon planes file and composition, SDK-free: parsed
 * on the console and in the host test (tests/stereo_mon_test.c). The format
 * is described in scripts/gen_stereo_mons.py.
 */
#include <string.h>
#include "3ds_stereo_mon.h"
#include "3ds_video.h"

#define HEADER_BYTES CTR_STEREO_MON_HEADER_BYTES
#define INDEX_BYTES CTR_STEREO_MON_INDEX_BYTES

static uint16_t Read16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t Read32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

bool CtrStereoMon_ParseHeader(const uint8_t *file, uint32_t size, CtrStereoMonHeader *out)
{
    CtrStereoMonHeader h;

    if (file == NULL || size < HEADER_BYTES || memcmp(file, CTR_STEREO_MON_MAGIC, 8) != 0)
        return false;
    h.count = Read16(file + 8);
    h.planes = file[10];
    h.frames = file[11];
    h.width = Read16(file + 12);
    h.height = Read16(file + 14);
    if (h.planes < 2 || h.planes > CTR_STEREO_MON_MAX_PLANES
        || h.frames < 1 || h.frames > CTR_STEREO_MON_FILE_MAX_FRAMES
        || h.width != CTR_STEREO_MON_DIM || h.height != CTR_STEREO_MON_DIM)
        return false;
    h.entryBytes = (uint32_t)h.frames * h.width * h.height / 2;
    if ((uint64_t)HEADER_BYTES + (uint64_t)h.count * INDEX_BYTES > size)
        return false;
    if (out) *out = h;
    return true;
}

bool CtrStereoMon_IndexEntry(const uint8_t *file, uint32_t size, const CtrStereoMonHeader *header,
                             unsigned index, uint16_t *national, uint32_t *offset)
{
    const uint8_t *entry;

    if (file == NULL || header == NULL || index >= header->count
        || (uint64_t)HEADER_BYTES + (uint64_t)(index + 1) * INDEX_BYTES > size)
        return false;
    entry = file + HEADER_BYTES + index * INDEX_BYTES;
    if (national) *national = Read16(entry);
    if (offset) *offset = Read32(entry + 4);
    return true;
}

unsigned CtrStereoMon_EntryFrames(const uint8_t *file, uint32_t size, const CtrStereoMonHeader *header,
                                  unsigned index)
{
    unsigned frames;

    if (!CtrStereoMon_IndexEntry(file, size, header, index, NULL, NULL)) return 0;
    frames = Read16(file + HEADER_BYTES + index * INDEX_BYTES + 2);
    if (frames == 0 || frames > header->frames) frames = header->frames;
    return frames;
}

uint32_t CtrStereoMon_FramesBytes(const CtrStereoMonHeader *header, unsigned frames)
{
    return header == NULL ? 0 : (uint32_t)frames * header->width * header->height / 2;
}

bool CtrStereoMon_Entry(const uint8_t *file, uint32_t size, const CtrStereoMonHeader *header,
                        unsigned index, uint16_t *national, const uint8_t **planes)
{
    uint32_t offset, bytes;

    if (!CtrStereoMon_IndexEntry(file, size, header, index, national, &offset))
        return false;
    bytes = CtrStereoMon_FramesBytes(header, CtrStereoMon_EntryFrames(file, size, header, index));
    if (offset > size || (uint64_t)offset + bytes > size)
        return false;
    if (planes) *planes = file + offset;
    return true;
}

int CtrStereoMon_Find(const uint8_t *file, uint32_t size, const CtrStereoMonHeader *header,
                      uint16_t national)
{
    if (header == NULL) return -1;
    for (unsigned i = 0; i < header->count; ++i)
    {
        uint16_t n;

        if (CtrStereoMon_IndexEntry(file, size, header, i, &n, NULL) && n == national)
            return (int)i;
    }
    return -1;
}

unsigned CtrStereoMon_Plane(const uint8_t *planes, const CtrStereoMonHeader *header,
                            unsigned frame, unsigned x, unsigned y)
{
    unsigned i, v;

    if (planes == NULL || header == NULL || x >= header->width || y >= header->height)
        return CTR_STEREO_MON_NONE;
    if (frame >= header->frames) frame = header->frames - 1;
    i = frame * header->width * header->height + y * header->width + x;
    v = (planes[i >> 1] >> ((i & 1) * 4)) & 15;
    return v >= header->planes ? CTR_STEREO_MON_NONE : v;
}

unsigned CtrStereoMon_Group(unsigned plane, unsigned planes, unsigned groups)
{
    if (plane >= planes || planes == 0) return CTR_STEREO_MON_NONE;
    if (groups >= planes) return plane;
    return plane * groups / planes;
}

/* Whether (x, y) is an opaque pixel of `group`. */
static bool InGroup(const uint16_t *picture, const uint8_t *planes, const CtrStereoMonHeader *header,
                    unsigned frame, unsigned groups, unsigned group, int x, int y)
{
    if (x < 0 || y < 0 || x >= CTR_STEREO_MON_DIM || y >= CTR_STEREO_MON_DIM) return false;
    return picture[y * CTR_STEREO_MON_DIM + x] != 0
        && CtrStereoMon_Group(CtrStereoMon_Plane(planes, header, frame, (unsigned)x, (unsigned)y),
                              header->planes, groups) == group;
}

void CtrStereoMon_ComposeTile(const uint16_t *picture, const uint8_t *planes,
                              const CtrStereoMonHeader *header, unsigned frame, unsigned groups,
                              unsigned group, uint16_t *texture, unsigned texWidth,
                              unsigned tileX, unsigned tileY, unsigned skirt)
{
    for (unsigned y = 0; y < CTR_STEREO_MON_DIM; ++y)
        for (unsigned x = 0; x < CTR_STEREO_MON_DIM; ++x)
        {
            uint16_t pixel = picture[y * CTR_STEREO_MON_DIM + x];
            unsigned plane = CtrStereoMon_Plane(planes, header, frame, x, y);
            unsigned g = CtrStereoMon_Group(plane, header->planes, groups);
            bool on = pixel != 0 && g == group;

            /* The skirt: under a pixel of a nearer group this card goes on
             * with the colour of its own nearest pixel within `skirt`, so
             * that the gap a nearer card's shift opens shows this card's
             * colours at this card's depth, and the nearer card stays clean. */
            if (!on && pixel != 0 && g != CTR_STEREO_MON_NONE && g > group && skirt != 0)
            {
                int best = (int)(skirt * skirt) * 2 + 1;

                for (int dy = -(int)skirt; dy <= (int)skirt; ++dy)
                    for (int dx = -(int)skirt; dx <= (int)skirt; ++dx)
                        if (dx * dx + dy * dy < best
                            && InGroup(picture, planes, header, frame, groups, group, (int)x + dx, (int)y + dy))
                        {
                            best = dx * dx + dy * dy;
                            pixel = picture[(y + dy) * CTR_STEREO_MON_DIM + x + dx];
                            on = true;
                        }
            }
            texture[CtrVideo_Texel(tileX + x, tileY + y, texWidth)] = on ? pixel : 0;
        }
}
