#ifndef VOXEL_VERTEX_PACK_H
#define VOXEL_VERTEX_PACK_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "voxel_mesh_builder.h"

/*
 * The builder works in VoxelVertex (six floats, 24 bytes). What the GPU reads
 * is this: the texture coordinates stay float, because the building pages are
 * not powers of two and their texel edges must stay exact, while position and
 * shade become shorts - positions in 1/512 tile relative to the draw's own
 * origin (a chunk corner, or the camera for the billboards), shade in 1/16384.
 * voxel.v.pica undoes both scales.
 *
 * A third less of everything: VRAM per chunk, staging, GPU copies and vertex
 * fetch. 1/512 of a tile is 1/32 of a pixel, and the origin a vertex is
 * measured from is always a whole tile, so two chunks quantise a shared edge
 * to exactly the same place.
 */
typedef struct
{
    float u, v;
    int16_t x, y, z, shade;
} VoxelGpuVertex;

#define VOXEL_POS_SCALE 512.0f
#define VOXEL_SHADE_SCALE 16384.0f

_Static_assert(sizeof(VoxelGpuVertex) == 16 && offsetof(VoxelGpuVertex, x) == 8,
               "GPU vertex layout must match the shader");

typedef struct
{
    unsigned errors;
    float worst;
    char axis; /* first out-of-range attribute: position or shade */
} VoxelPackStats;

static inline int16_t VoxelPack_Quantise(float value, float scale, VoxelPackStats *stats)
{
    float scaled = value * scale + 0.5f;
    int whole;

    /*
     * Round half up, never half away from zero: that is what keeps the
     * result of a shared edge independent of the origin it is measured from.
     * floor() by truncation and one correction, not floorf: ARMv6 has no
     * rounding instruction and the library call was a third of a chunk build.
     */
    if (scaled >= 32767.0f || scaled < -32768.0f)
    {
        if (scaled >= 32768.0f || scaled < -32768.0f)
        {
            ++stats->errors;
            if (fabsf(value) > fabsf(stats->worst))
                stats->worst = value;
        }
        return scaled >= 0.0f ? 32767 : -32768;
    }
    whole = (int)scaled;
    if ((float)whole > scaled)
        --whole;
    return (int16_t)whole;
}

static inline void VoxelPack_Vertices(const VoxelVertex *src, unsigned count, VoxelGpuVertex *dst,
                                      VoxelPackStats *stats)
{
    for (unsigned i = 0; i < count; ++i)
    {
        dst[i].u = src[i].u;
        dst[i].v = src[i].v;
        unsigned before = stats->errors;

        dst[i].x = VoxelPack_Quantise(src[i].x, VOXEL_POS_SCALE, stats);
        dst[i].y = VoxelPack_Quantise(src[i].y, VOXEL_POS_SCALE, stats);
        dst[i].z = VoxelPack_Quantise(src[i].z, VOXEL_POS_SCALE, stats);
        if (stats->errors != before && stats->axis == 0)
            stats->axis = 'p';
        dst[i].shade = VoxelPack_Quantise(src[i].shade, VOXEL_SHADE_SCALE, stats);
        if (stats->errors != before && stats->axis == 0)
            stats->axis = 's';
    }
}

typedef struct
{
    int32_t lo[3], hi[3];
} VoxelPackBounds;

static inline void VoxelPack_GrowBounds(VoxelPackBounds *bounds, const VoxelGpuVertex *out)
{
    int32_t q[3] = {out->x, out->y, out->z};

    for (int a = 0; a < 3; ++a)
    {
        if (q[a] < bounds->lo[a]) bounds->lo[a] = q[a];
        if (q[a] > bounds->hi[a]) bounds->hi[a] = q[a];
    }
}

#endif
