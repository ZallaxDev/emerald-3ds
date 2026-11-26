/* Immutable region roles, resident at four bits per cell. No SD access from
 * classification or shadow rays, and no cache eviction at map crossings. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "port_log.h"
#include "voxel_regions.h"
#include "voxel_who.h"
#include "voxel_file.h"
#ifndef VOXEL_REGIONS_PATH
#define VOXEL_REGIONS_PATH "voxel/regions.bin"
#endif
#define VOXEL_REGIONS_MAX_BYTES (2u * 1024u * 1024u)
#if VOXEL_ROLE_COUNT > 16
#error Region roles no longer fit in four bits
#endif
struct RegionIndex {
    uint16_t layoutId, width, height;
    uint32_t roleOffset;
};
static struct RegionIndex *sIndex;
static unsigned sIndexCount;
static const struct RegionIndex *sRecentOf[VOXEL_THREADS];
#define sRecent (sRecentOf[VOXEL_WHO])
static uint8_t *sRoles;
static unsigned Read16(const uint8_t *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static uint32_t Read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void VoxelRegions_Shutdown(void) {
    free(sRoles); free(sIndex);
    sRoles = NULL; sIndex = NULL; sIndexCount = 0; memset(sRecentOf, 0, sizeof(sRecentOf));
}
bool VoxelRegions_Init(void) {
    FILE *file;
    uint8_t *raw = NULL;
    uint32_t packed = 0, table;
    long length;
    bool ok = false;
    VoxelRegions_Shutdown();
    file = VoxelFile_Open(VOXEL_REGIONS_PATH);
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 8
     || length > (long)VOXEL_REGIONS_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) goto done;
    raw = malloc((size_t)length);
    if (!raw || fread(raw, 1, (size_t)length, file) != (size_t)length
     || memcmp(raw, "VXR5", 4) != 0) goto done;
    sIndexCount = Read16(raw + 4);
    table = 8u + 12u * sIndexCount;
    if (!sIndexCount || table > (uint32_t)length) goto done;
    sIndex = malloc(sIndexCount * sizeof(*sIndex));
    if (!sIndex) goto done;
    for (unsigned i = 0; i < sIndexCount; ++i) {
        const uint8_t *row = raw + 8u + 12u * i;
        struct RegionIndex *entry = &sIndex[i];
        uint32_t offset = Read32(row + 8), cells, bytes;
        entry->layoutId = (uint16_t)Read16(row);
        entry->width = (uint16_t)Read16(row + 2);
        entry->height = (uint16_t)Read16(row + 4);
        cells = (uint32_t)entry->width * entry->height;
        if (!entry->layoutId || !cells || (i && entry->layoutId <= sIndex[i - 1].layoutId)
         || offset < table || offset > (uint32_t)length || cells > (uint32_t)length - offset) goto done;
        bytes = (cells + 1u) / 2u;
        if (bytes > VOXEL_REGIONS_MAX_BYTES - packed) goto done;
        entry->roleOffset = packed; packed += bytes;
    }
    sRoles = calloc(packed, 1);
    if (!sRoles) goto done;
    for (unsigned i = 0; i < sIndexCount; ++i) {
        const struct RegionIndex *entry = &sIndex[i];
        const uint8_t *roles = raw + Read32(raw + 8u + 12u * i + 8u);
        uint32_t cells = (uint32_t)entry->width * entry->height;
        for (uint32_t k = 0; k < cells; ++k) {
            if (roles[k] >= VOXEL_ROLE_COUNT) goto done;
            sRoles[entry->roleOffset + k / 2u] |= (uint8_t)(roles[k] << ((k & 1u) * 4u));
        }
    }
    PORT_LOG("[VIDEO] VOXEL regions: %u layouts resident, %lu packed bytes\n", sIndexCount, (unsigned long)packed);
    ok = true;
done:
    free(raw); fclose(file);
    if (!ok) {
        PORT_LOG("[ERROR] VOXEL regions: invalid data or insufficient heap\n");
        VoxelRegions_Shutdown();
    }
    return ok;
}
static const struct RegionIndex *FindLayout(unsigned layoutId) {
    unsigned low = 0, high = sIndexCount;
    while (low < high) {
        unsigned mid = (low + high) / 2;
        if (sIndex[mid].layoutId == layoutId) return &sIndex[mid];
        if (sIndex[mid].layoutId < layoutId) low = mid + 1;
        else high = mid;
    }
    return NULL;
}
unsigned VoxelRegions_RoleAt(unsigned layoutId, int localX, int localY) {
    const struct RegionIndex *entry = sRecent;
    uint32_t cell;
    if (!entry || entry->layoutId != layoutId) sRecent = entry = FindLayout(layoutId);
    if (!entry || localX < 0 || localY < 0 || localX >= entry->width || localY >= entry->height)
        return VOXEL_ROLE_FLOOR;
    cell = (uint32_t)localY * entry->width + (unsigned)localX;
    return (sRoles[entry->roleOffset + cell / 2u] >> ((cell & 1u) * 4u)) & 15u;
}
