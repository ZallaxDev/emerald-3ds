#ifndef CTR_FIELD_TERRAIN_H
#define CTR_FIELD_TERRAIN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The 2D field's terrain: the metatiles the field camera draws on BG1-BG3
 * (field_camera.c) and the 2D tile banks of the neighbours drawn with other
 * tilesets (fieldmap.c). Only the 2D compositor reads it (3ds_video.c); the
 * voxel world draws the map from the map grid and its own tilesets.
 *
 * While the voxel world is drawn alone, nothing shows that terrain, and the
 * camera leaves it undrawn: its window keeps following the picture, the doors
 * it draws are remembered, the terrain is marked stale and nothing else is
 * done - no metatile drawn, no tilemap copied, no tile bank loaded. Before a
 * frame can show the 2D field again it is drawn again, whole, in the VBlank
 * (FieldCamera_SyncTerrain), so that frame's tilemap copies carry it.
 */

/* Port side (3ds_field_view.c). */
/* By the present, every top-screen frame it draws: whether that frame showed
 * the voxel world alone, with no swoop bringing the 2D field in. */
void CtrFieldTerrain_NoteFrame(bool unseen);
/* Whether the terrain has to be drawn now: anything but the voxel world drawn
 * alone last frame, its option still on, in the overworld. */
bool CtrFieldTerrain_Wanted(void);

/* Game side (field_camera.c). */
/* Some of the terrain was left undrawn and has not been drawn again since,
 * and BG1-BG3 show the field's tilemaps: a 2D field drawn now would show it
 * stale (the present draws black instead, for the frame that takes). */
bool FieldCamera_TerrainStale(void);
/* In the VBlank, before the game's handler: draws the whole view again and
 * queues the three tilemaps for its DMA, if the terrain is stale and wanted. */
void FieldCamera_SyncTerrain(void);

/* For the profiler: what was left undrawn and what was drawn again. */
typedef struct
{
    uint32_t viewsSkipped;   /* whole views (crossings, scripts) not drawn */
    uint32_t slicesSkipped;  /* columns and rows of the window not drawn */
    uint32_t cellsSkipped;   /* single metatiles and door frames not drawn */
    uint32_t banksDeferred;  /* crossings that kept no outgoing tileset bank */
    uint32_t resyncs;        /* whole views drawn again for a 2D frame */
    float lastResyncMs;      /* the last of those redraws, queued copies included */
} CtrFieldTerrainStats;

/* Counters since boot (the profiler takes differences). */
void FieldCamera_GetTerrainStats(CtrFieldTerrainStats *stats);

#endif
