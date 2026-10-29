#include <stddef.h>
/*
 * The sprite atlas's slots, without the game or the SDK. See
 * voxel_sprite_slots.h.
 */
#include "voxel_sprite_slots.h"

#define VOXEL_SLOT_REQUESTS_MAX 256u

unsigned VoxelSlots_AtlasWidth(unsigned width, unsigned slotDim)
{
    if (width == 0 || slotDim == 0)
        return 0;
    if (width <= slotDim)
        return width;
    if (width <= 2u * slotDim)
        return (width + 1u) / 2u;
    return 0;
}

bool VoxelSlots_Fits(unsigned width, unsigned height, bool color256,
                     unsigned slotDim, unsigned maxBytes)
{
    unsigned bytes;

    if (width == 0 || height == 0 || (width & 7u) != 0 || (height & 7u) != 0)
        return false;
    if (height > slotDim || VoxelSlots_AtlasWidth(width, slotDim) == 0)
        return false;
    bytes = width * height;
    if (!color256)
        bytes /= 2u;
    return bytes <= maxBytes;
}

unsigned VoxelSlots_SourceColumn(unsigned atlasX, unsigned atlasWidth, unsigned width, bool flipX)
{
    unsigned step, column;

    if (atlasWidth == 0 || atlasX >= atlasWidth)
        return 0;
    step = width > atlasWidth ? width / atlasWidth : 1u;
    if (step == 0)
        step = 1;
    column = (flipX ? atlasWidth - 1u - atlasX : atlasX) * step;
    return column < width ? column : width - 1u;
}

unsigned VoxelSlots_TilePixel(const uint8_t *tiles, unsigned width, unsigned x, unsigned y,
                              bool color256)
{
    unsigned tile = (y / 8u) * (width / 8u) + x / 8u;
    unsigned px = x & 7u, py = y & 7u;

    if (color256)
        return tiles[tile * 64u + py * 8u + px];
    return (tiles[tile * 32u + py * 4u + px / 2u] >> ((px & 1u) * 4u)) & 0xFu;
}

unsigned VoxelSlots_DoorPixel(const uint8_t *tiles, const uint8_t *palettes,
                              unsigned x, unsigned y, unsigned *palette)
{
    unsigned column = x / 16u;
    unsigned tile = (y / 16u) * 4u + ((y / 8u) & 1u) * 2u + ((x / 8u) & 1u);
    unsigned px = x & 7u, py = y & 7u;
    const uint8_t *data = tiles + (column * 8u + tile) * 32u;

    *palette = palettes[tile];
    return (data[py * 4u + px / 2u] >> ((px & 1u) * 4u)) & 0xFu;
}

void VoxelSlots_DoorRow(float v, float wall, float *height, float *depth)
{
    if (v >= wall)
    {
        *height = 0.0f;
        *depth = v;
    }
    else
    {
        *height = wall - v;
        *depth = wall;
    }
}

/* Float slack on the face's bound: a face computed on the cell's edge is not
 * rejected for an ulp (1/256 tile, far below a pixel's 1/16). */
#define VOXEL_FACE_SLACK (1.0f / 256.0f)

bool VoxelSlots_ModelFacePush(float front, float cz, float clear, float maxFace, float *push)
{
    float ahead = front - cz;

    if (front + clear <= cz)
        return false;                       /* already in front of it */
    if (ahead > maxFace + VOXEL_FACE_SLACK)
        return false;                       /* past the walker's own cell */
    if (push != NULL)
        *push = ahead + clear;
    return true;
}

unsigned VoxelSlots_Assign(VoxelSlotRequest *requests, unsigned count, uint8_t *owner,
                           bool *claimed, bool *reset, unsigned slots, unsigned firstShared)
{
    /* Static: voxel_entities.c calls this from the VBlank handler. */
    static uint16_t order[VOXEL_SLOT_REQUESTS_MAX];
    static bool selected[VOXEL_SLOT_REQUESTS_MAX];
    static bool used[256];
    unsigned available = 0, taken = 0, dropped = 0, n = 0;

    if (count > VOXEL_SLOT_REQUESTS_MAX)
        count = VOXEL_SLOT_REQUESTS_MAX;
    if (slots > 256u)
        slots = 256u;
    for (unsigned s = 0; s < slots; ++s)
    {
        used[s] = false;
        if (!claimed[s])
            ++available;
    }

    /* In range only, by priority, stable: an insertion sort of at most a
     * screenful of sprites. */
    for (unsigned k = 0; k < count; ++k)
    {
        requests[k].slot = -1;
        selected[k] = false;
        if (!requests[k].inRange)
            continue;
        {
            unsigned j = n;

            while (j > 0 && requests[order[j - 1]].priority > requests[k].priority)
            {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = (uint16_t)k;
            ++n;
        }
    }
    for (unsigned i = 0; i < n; ++i)
    {
        if (taken < available)
        {
            selected[order[i]] = true;
            ++taken;
        }
        else
        {
            ++dropped;
        }
    }

    /* Still wanted: the slot it had. */
    for (unsigned k = 0; k < count; ++k)
    {
        if (!selected[k])
            continue;
        for (unsigned s = 0; s < slots; ++s)
        {
            if (!claimed[s] && !used[s] && owner[s] == (unsigned)requests[k].id + 1u)
            {
                requests[k].slot = (int)s;
                used[s] = true;
                break;
            }
        }
    }
    /* No longer wanted (gone, out of sight, or given way): let go. */
    for (unsigned s = 0; s < slots; ++s)
    {
        if (!claimed[s] && !used[s] && owner[s] != 0)
        {
            owner[s] = 0;
            reset[s] = true;
        }
    }
    /* New: a free slot, past the objects' own first. */
    for (unsigned k = 0; k < count; ++k)
    {
        int found = -1;

        if (!selected[k] || requests[k].slot >= 0)
            continue;
        for (unsigned s = firstShared; s < slots && found < 0; ++s)
            if (!claimed[s] && !used[s])
                found = (int)s;
        for (unsigned s = 0; s < firstShared && s < slots && found < 0; ++s)
            if (!claimed[s] && !used[s])
                found = (int)s;
        if (found < 0)
        {
            ++dropped;   /* not reached: as many were selected as are free */
            continue;
        }
        requests[k].slot = found;
        used[found] = true;
        owner[found] = (uint8_t)(requests[k].id + 1u);
        reset[found] = true;
    }
    for (unsigned k = 0; k < count; ++k)
        if (requests[k].slot >= 0)
            claimed[requests[k].slot] = true;
    return dropped;
}
