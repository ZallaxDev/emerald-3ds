/* Clock-driven sun, baked into chunk colours; no extra terrain GPU pass. */
#ifndef CTR_VOXEL_LIGHTING_H
#define CTR_VOXEL_LIGHTING_H

#include "voxel_mesh_builder.h"

/* Standalone host consumers retain the original mesh unless explicitly enabled. */
#ifndef CTR_VOXEL_LIGHTING
#define CTR_VOXEL_LIGHTING 0
#endif

#if CTR_VOXEL_LIGHTING
/* The legacy sun is northwest; AUTO follows the clock. Y is height. */
typedef struct
{
    float dx, dz, strength;
    uint32_t key;
} VoxelSun;
extern VoxelSun gVoxelSun;
#define VOXEL_SUN_DX (gVoxelSun.dx)
#define VOXEL_SUN_DZ (gVoxelSun.dz)
#define VOXEL_LIGHT_REACH 8
/* The light of what the sun does not reach: a cast shadow, and a face turned
 * away from the sun. The same number, so a wall and the shadow it casts on
 * the grass read as the same shade. */
#define VOXEL_AMBIENT 0.70f

/* Forgets every cached caster and sample. Required whenever the world they
 * were read from changes - a live tile, the set of maps or their origins -
 * and needed at no other time: the caches outlive builds and frames. */
void VoxelLighting_Reset(void);
bool VoxelLighting_SetSun(float dx, float dz, float strength, uint32_t key);
uint32_t VoxelLighting_Hash(int x0, int z0, int x1, int z1);
float VoxelLighting_Sample(float x, float y, float z);
/* The same answer for a point lit on its own, every frame (a sprite's card):
 * without the blocks' bound, which pays for itself over a build's rays but
 * classifies some four hundred cells the first time a lone ray needs it. */
float VoxelLighting_SampleAlone(float x, float y, float z);
/* Ahead of the ground of cells x0..x1, z0..z1: the casters' bounds its rays
 * will read, `budget` new ones a call from `*cursor` (0 to begin). True once
 * all are read. The same shadows either way; it only spreads their cost. */
bool VoxelLighting_Warm(int x0, int z0, int x1, int z1, unsigned *cursor, unsigned budget);
/* Rays cast so far (samples not found in the cache), for the build logs. */
unsigned VoxelLighting_Rays(void);
/* Ray points tested, and tests against a relief's surface (rays and contact
 * AO), so far: where the rays' time goes, for the same logs. */
unsigned VoxelLighting_Steps(void);
unsigned VoxelLighting_SurfaceTests(void);
/* The light a face gets from the sun for its facing alone, VOXEL_AMBIENT for
 * a face turned away. The normal need not be unit length. */
float VoxelLighting_Face(float nx, float ny, float nz);
void VoxelLighting_Quad(VoxelBuilder *builder, const VoxelVertex *a,
                         const VoxelVertex *b, const VoxelVertex *c,
                         const VoxelVertex *d);
/* A relief cell's lattice of VOXEL_RELIEF_SIDE x VOXEL_RELIEF_SIDE points,
 * row major, each with its face term as its shade: its shadows read ahead,
 * fewer rays, for the quads lit through VoxelLighting_Quad after it. */
void VoxelLighting_Lattice(VoxelBuilder *builder, const VoxelVertex *lattice);
/* The light each of those points gets, written to `shade`, as the quads lit
 * through VoxelLighting_Quad give it to them. */
void VoxelLighting_LatticeShades(VoxelBuilder *builder, const VoxelVertex *lattice, float *shade);
/* The triangle a b c, lit as VoxelLighting_Quad(a, b, c, c) lights it. */
void VoxelLighting_Tri(VoxelBuilder *builder, const VoxelVertex *a,
                       const VoxelVertex *b, const VoxelVertex *c);
/*
 * A modelled building's triangle. `drawnShade` is the face shade the model
 * was written with (voxel_building.py's SHADE_*): the side the face was drawn
 * for, since models are not wound consistently. It is replaced by the
 * sun's facing; models take no cast shadow.
 */
/* A model triangle's lit shade (sun facing only, see ModelTri). */
float VoxelLighting_ModelShade(const VoxelVertex *a, const VoxelVertex *b,
                               const VoxelVertex *c, float drawnShade);
void VoxelLighting_ModelTri(VoxelBuilder *builder, const VoxelVertex *a,
                            const VoxelVertex *b, const VoxelVertex *c, float drawnShade);
#endif

#endif
