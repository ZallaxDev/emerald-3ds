/*
 * Player and NPC billboards for the voxel overworld.
 *
 * Object events are drawn as camera-facing quads sampled from a sprite atlas
 * that this module decodes out of the game's own logical OBJ VRAM, so the
 * sprites are whatever the game is actually showing this frame - animation,
 * palette fades and all - with no second asset path.
 *
 * Like the rest of the voxel modules below ctr_voxel.c, nothing here touches
 * the GPU: the atlas is written into a caller-owned pixel buffer and the
 * billboards into a caller-owned vertex array.
 *
 * Derived from the object-event handling of the MIT-licensed voxel_renderer.c
 * of pokeemerald-multiplatform; see NOTICE.md.
 */
#ifndef CTR_VOXEL_ENTITIES_H
#define CTR_VOXEL_ENTITIES_H

#include <stdbool.h>
#include <stdint.h>

#include "voxel_camera.h"
#include "voxel_mesh_builder.h"

/*
 * 32 slots of 64x64 in a 512x256 RGBA5551 texture (256 KiB of linear memory):
 * the first 16 one per gObjectEvents[], the rest for what is not an object -
 * field effects, the follower, the visible wild Pokemon - which also take an
 * unused object's slot. With 16 in all, a route full of trainers left those
 * too few: grass stepped on did not rustle and the wild ones blinked in and
 * out as they took turns.
 */
#define VOXEL_SPRITE_ATLAS_W    512u
#define VOXEL_SPRITE_ATLAS_H    256u
#define VOXEL_SPRITE_SLOT_DIM   64u
#define VOXEL_SPRITE_COLUMNS    (VOXEL_SPRITE_ATLAS_W / VOXEL_SPRITE_SLOT_DIM)
#define VOXEL_SPRITE_SLOTS      32u
#define VOXEL_SPRITE_PIXELS     (VOXEL_SPRITE_ATLAS_W * VOXEL_SPRITE_ATLAS_H)
#define VOXEL_CAST_SHADOW_VERTICES 6u
/* A reflection is cut across its card into strips, each over the water under
 * it (EmitReflection): at most one quad a strip. */
#define VOXEL_REFLECTION_STRIPS 4u
#define VOXEL_REFLECTION_VERTICES (6u * VOXEL_REFLECTION_STRIPS)

/* Forgets every cached slot, so the next update re-decodes from scratch. */
void VoxelEntities_Reset(void);

/*
 * The player's world position with sub-tile interpolation applied, which is
 * what the camera has to follow: VoxelWorld_GetPlayerWorldCoords reports whole
 * tiles, and a follow camera fed a target that jumps a tile at a time lurches
 * once per step no matter how smooth its damping is.
 */
void VoxelEntities_GetPlayerWorldPos(float *worldX, float *worldZ);
/* How high a bridge's deck carries the player, tiles (0 off one). */
float VoxelEntities_PlayerDeck(void);
/* First vertex of the emitted player quad, or -1 when invisible/not emitted. */
int VoxelEntities_PlayerVertexFirst(void);
/* The way the player faces, as the decompilation's DIR_* (1 south, 2 north,
 * 3 west, 4 east), or 0 when there is none to speak of (no player yet, a
 * diagonal): the free-roam camera starts its look from it. */
int VoxelEntities_PlayerFacing(void);
/* Reads every picture the player's animations show, so that the render
 * thread, which shows the one that faces the camera (free-roam camera), finds
 * them in memory. Call from the game's thread. */
void VoxelEntities_WarmPlayerFrames(void);

/*
 * Re-decodes the slots whose sprite changed and appends one billboard per
 * visible object event to the builder. `atlas` is VOXEL_SPRITE_PIXELS
 * uint16_t in PICA order. Optional `shadows` receives each outdoor object's
 * cast shadow: one quad (VOXEL_CAST_SHADOW_VERTICES), alpha in `shade`.
 * Returns the number of slots re-decoded this frame.
 */
unsigned VoxelEntities_Emit(VoxelBuilder *builder, uint16_t *atlas,
                            const VoxelCamera *camera, VoxelBuilder *shadows,
                            VoxelBuilder *reflections);

#endif
