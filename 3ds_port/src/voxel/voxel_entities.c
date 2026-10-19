/*
 * Player and NPC billboards. See voxel_entities.h and NOTICE.md.
 */

/* Before global.h, which redefines abs() as a macro over stdlib's prototype. */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "global.h"
#include "fieldmap.h"
#include "main.h"
#include "sprite.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "constants/field_effects.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "gba/io_reg.h"
#include "port_platform.h"
#include "3ds_log.h"

#include "3ds_video.h"
#include "voxel_building.h"
#include "voxel_entities.h"
#include "voxel_freemove.h"
#include "voxel_grade.h"
#include "voxel_relief.h"
#include "voxel_world.h"
#include "voxel_lighting.h"

/* 16 pixels of sprite is one world tile, as in the reference renderer. */
#define VOXEL_PIXELS_PER_TILE 16.0f

/* Largest sprite this atlas can hold, 8bpp: 64*64 bytes of source tiles. */
#define VOXEL_SPRITE_MAX_BYTES (VOXEL_SPRITE_SLOT_DIM * VOXEL_SPRITE_SLOT_DIM)

/* Step durations, matching sStepTimes[] in event_object_movement.c. */
static const int sStepFrames[] = { 16, 8, 6, 4, 2 };

/*
 * What a slot was last decoded from. The animation of a walking object event
 * rewrites the tiles *behind* a fixed tileNum, so the descriptor alone cannot
 * detect a frame change: the source bytes and the palette are kept and
 * compared. That is ~2 KiB of memcmp per object per frame, which is far
 * cheaper than re-decoding one.
 */
typedef struct
{
    bool valid;
    u16 tileNum, paletteNum;
    /* The ext palette it was drawn with (sprite.c), 0xFF for paletteNum. */
    u8 extPalette;
    u8 shape, size;
    bool flipX, flipY, color256;
    int width, height;
    u32 sourceBytes;
    u8 source[VOXEL_SPRITE_MAX_BYTES];
    u16 palette[256];
    u32 paletteBytes;
    /* Extent last written into the atlas cell, so a shrinking sprite clears
     * exactly what it used to cover instead of the whole 64x64 slot. */
    int drawnWidth, drawnHeight;
    /* Empty rows under the feet of the standing pose: the art's feet sit this
     * far up the cell, and the card leaves them out to put them on the
     * ground (VisibleRows). Measured once per picture table (StandingFootPad). */
    int footPad;
    const struct SpriteFrameImage *footPadImages;
    /* Measured on the standing pose itself; else, until that picture is in
     * memory, on the frame the object first showed (StandingFootPad). */
    bool footPadExact;
} VoxelSpriteSlot;

static VoxelSpriteSlot sSlots[VOXEL_SPRITE_SLOTS];
/* The field effect sprite a slot of an unused object event shows, plus one;
 * 0 for none (VoxelEntities_Emit). */
static u8 sSlotEffect[VOXEL_SPRITE_SLOTS];
static int sPlayerVertexFirst = -1;

int VoxelEntities_PlayerVertexFirst(void) { return sPlayerVertexFirst; }

int VoxelEntities_PlayerFacing(void)
{
    const struct ObjectEvent *obj;

    if (gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
        return 0;
    obj = &gObjectEvents[gPlayerAvatar.objectEventId];
    /* DIR_SOUTH..DIR_EAST are 1..4; the diagonals and none are not a facing. */
    return obj->active && obj->facingDirection >= DIR_SOUTH && obj->facingDirection <= DIR_EAST
         ? obj->facingDirection : 0;
}

/*
 * The axes a card at (cx, cz) is drawn along: `right` along its width, scaled
 * by `narrow`, and `toward` the camera along the ground. On the orbit every
 * card shares the camera's yaw; in the free-roam rig each turns to face the
 * eye, which a head that stands among them (and looks anywhere) needs.
 */
static void CardAxes(const VoxelCamera *camera, float cx, float cz, float narrow,
                     float *rightX, float *rightZ, float *towardX, float *towardZ)
{
    float yawRad = VoxelCamera_CardYaw(camera, cx, cz) * (3.14159265358979323846f / 180.0f);

    *rightX = cosf(yawRad) * narrow;
    *rightZ = -sinf(yawRad) * narrow;
    *towardX = sinf(yawRad);
    *towardZ = cosf(yawRad);
}

void VoxelEntities_Reset(void)
{
    for (unsigned i = 0; i < VOXEL_SPRITE_SLOTS; ++i)
    {
        sSlots[i].valid = false;
        sSlotEffect[i] = 0;
    }
}

/* ── OAM geometry ───────────────────────────────────────────────────────── */

static void GetSpriteDimensions(u8 shape, u8 size, int *w, int *h)
{
    static const u8 square[4][2] = {{8,8},{16,16},{32,32},{64,64}};
    static const u8 wide[4][2]   = {{16,8},{32,8},{32,16},{64,32}};
    static const u8 tall[4][2]   = {{8,16},{8,32},{16,32},{32,64}};
    const u8 (*table)[2];

    if (shape == ST_OAM_SQUARE) table = square;
    else if (shape == ST_OAM_H_RECTANGLE) table = wide;
    else if (shape == ST_OAM_V_RECTANGLE) table = tall;
    else { *w = 16; *h = 32; return; }
    *w = table[size & 3][0];
    *h = table[size & 3][1];
}

/* ── Movement interpolation ─────────────────────────────────────────────── */

/*
 * Sub-tile progress in [0,1]: 0 at previousCoords, 1 at currentCoords.
 *
 * sprite->x/y are screen-space and already carry the camera offset, so they
 * cannot be used for a world position. The step timer can.
 */
/*
 * A jump keeps other things in the sprite data a step keeps its speed and
 * timer in (event_object_movement.c: sDistance, sJumpType, and the timer in
 * data[6]). Read as a step, a ledge's jump stood still at 0 and the jumper
 * went from tile to tile as the coordinates did: at the start, half way and
 * on landing.
 */
static bool IsJumpAction(u8 action)
{
    return (action >= MOVEMENT_ACTION_JUMP_2_DOWN && action <= MOVEMENT_ACTION_JUMP_2_RIGHT)
        || (action >= MOVEMENT_ACTION_JUMP_SPECIAL_DOWN && action <= MOVEMENT_ACTION_JUMP_SPECIAL_RIGHT)
        || (action >= MOVEMENT_ACTION_JUMP_DOWN && action <= MOVEMENT_ACTION_JUMP_IN_PLACE_RIGHT_LEFT)
        || (action >= MOVEMENT_ACTION_ACRO_WHEELIE_HOP_FACE_DOWN
         && action <= MOVEMENT_ACTION_ACRO_WHEELIE_JUMP_RIGHT);
}

/* In the air: the action has set the jump up (sActionFuncId) and not landed. */
static bool IsJumping(const struct ObjectEvent *obj, const struct Sprite *sprite)
{
    return (obj->singleMovementActive || obj->heldMovementActive)
        && IsJumpAction(obj->movementActionId) && sprite->data[2] == 1;
}

/*
 * A jump moves a pixel a frame, a special one a pixel every other frame, and
 * one of two tiles moves its coordinates on by the second tile half way
 * (UpdateJumpAnim): past 16 pixels it is that far into the second tile.
 */
static float GetJumpProgress(const struct ObjectEvent *obj, const struct Sprite *sprite)
{
    float pixels = (float)(u16)sprite->data[6];

    if (obj->movementActionId >= MOVEMENT_ACTION_JUMP_SPECIAL_DOWN
     && obj->movementActionId <= MOVEMENT_ACTION_JUMP_SPECIAL_RIGHT)
        pixels *= 0.5f;
    if (pixels >= VOXEL_PIXELS_PER_TILE)
        pixels -= VOXEL_PIXELS_PER_TILE;
    return pixels < VOXEL_PIXELS_PER_TILE ? pixels / VOXEL_PIXELS_PER_TILE : 1.0f;
}

static float GetMovementProgress(const struct ObjectEvent *obj, const struct Sprite *sprite)
{
    int speed, timer, stepLen;
    float t;

    /* Idle: ShiftStillObjectEventCoords has made previous == current. */
    if (!obj->singleMovementActive && !obj->heldMovementActive)
        return 1.0f;
    if (IsJumpAction(obj->movementActionId))
        return IsJumping(obj, sprite) ? GetJumpProgress(obj, sprite) : 1.0f;

    speed = (int)(u16)sprite->data[4];
    timer = (int)(u16)sprite->data[5];
    if (speed < 0 || speed >= (int)ARRAY_COUNT(sStepFrames))
        return 1.0f;
    stepLen = sStepFrames[speed];
    if (stepLen <= 0)
        return 1.0f;

    t = (float)timer / (float)stepLen;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

static bool IsPlayerObject(const struct ObjectEvent *obj)
{
    return gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT
        && obj == &gObjectEvents[gPlayerAvatar.objectEventId];
}

/* The game moves the player itself: a jump over a ledge, with its arc. */
static bool GameOwnsPlayer(const struct ObjectEvent *obj)
{
    return (obj->singleMovementActive || obj->heldMovementActive) && IsJumpAction(obj->movementActionId);
}

static bool GetObjectWorldPos(const struct ObjectEvent *obj, const struct Sprite *sprite,
                              float *outX, float *outZ)
{
    float prevX = (float)(obj->previousCoords.x - MAP_OFFSET);
    float prevZ = (float)(obj->previousCoords.y - MAP_OFFSET);
    float curX = (float)(obj->currentCoords.x - MAP_OFFSET);
    float curZ = (float)(obj->currentCoords.y - MAP_OFFSET);
    float t = GetMovementProgress(obj, sprite);

    *outX = prevX + (curX - prevX) * t;
    *outZ = prevZ + (curZ - prevZ) * t;
    if (IsPlayerObject(obj))
    {
        /*
         * Free walking (voxel_freemove.h): the player stands where the
         * stick has taken it, not where a grid step has got to. Everything
         * placed from the player - the card, what it stands in, the camera -
         * follows from here.
         */
        VoxelFreeMove *move = VoxelFreeMove_Get();

        VoxelFreeMove_Sync(move, obj->currentCoords.x, obj->currentCoords.y);
        if (move->blend > 0.0f && !GameOwnsPlayer(obj))
        {
            *outX += ((curX + move->ox) - *outX) * move->blend;
            *outZ += ((curZ + move->oz) - *outZ) * move->blend;
        }
    }
    return true;
}

/*
 * How high a bridge's deck lifts an object standing at world (x, z) of its
 * tile: the Seaside Cycling Road (voxel_bridge.py) carries whoever is up at
 * the deck's elevation, and lets the ones on the path or the sea pass under.
 * Tiles; 0 off any deck.
 */
static float ObjectDeck(const struct ObjectEvent *obj, float worldX, float worldZ)
{
    float cx = worldX + 0.5f, cz = worldZ + 0.5f, lift;
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt((int)floorf(cx), (int)floorf(cz));

    return VoxelBuildings_DeckAt(inst, cx, cz, obj->currentElevation, &lift) ? lift : 0.0f;
}

float VoxelEntities_PlayerDeck(void)
{
    const struct ObjectEvent *obj = &gObjectEvents[gPlayerAvatar.objectEventId];
    float x = 0.0f, z = 0.0f;

    if (!obj->active || obj->spriteId >= MAX_SPRITES)
        return 0.0f;
    GetObjectWorldPos(obj, &gSprites[obj->spriteId], &x, &z);
    if (VoxelWorld_InstanceCount() > 0)
    {
        x += (float)VoxelWorld_Instance(0)->originX;
        z += (float)VoxelWorld_Instance(0)->originY;
    }
    return ObjectDeck(obj, x, z);
}

void VoxelEntities_GetPlayerWorldPos(float *worldX, float *worldZ)
{
    const struct ObjectEvent *obj = &gObjectEvents[gPlayerAvatar.objectEventId];
    float x = 0.0f, z = 0.0f;

    if (!obj->active || obj->spriteId >= MAX_SPRITES)
    {
        /* That helper already resolves to world space on its own. */
        VoxelWorld_GetPlayerWorldCoords(worldX, worldZ);
        return;
    }
    GetObjectWorldPos(obj, &gSprites[obj->spriteId], &x, &z);

    if (VoxelWorld_InstanceCount() > 0)
    {
        const VoxelMapInstance *inst = VoxelWorld_Instance(0);

        x += (float)inst->originX;
        z += (float)inst->originY;
    }
    /* The camera follows the player where the relief puts it, and up on
     * a bridge's deck as far south as it is high (the 45-degree rule). */
    z += VoxelRelief_ShiftAt(x + 0.5f, z + 0.5f) + ObjectDeck(obj, x, z);
    if (worldX != NULL) *worldX = x;
    if (worldZ != NULL) *worldZ = z;
}

/* ── Slot decoding ──────────────────────────────────────────────────────── */

/*
 * Gathers the source tiles of a sprite into `dest`, in reading order, using
 * the same address arithmetic the 2D compositor uses for OBJ: 1D or 2D
 * character mapping per DISPCNT bit 6, two VRAM units per tile at 8bpp.
 */
static u32 GatherSource(const struct Sprite *sprite, int w, int h, bool color256, u8 *dest)
{
    const u8 *vram = VRAM_ + 0x10000;
    bool mapping1d = (REG_DISPCNT & DISPCNT_OBJ_1D_MAP) != 0;
    unsigned bytesPerTile = color256 ? 64u : 32u;
    u32 written = 0;

    for (int ty = 0; ty < h / 8; ++ty)
    {
        for (int tx = 0; tx < w / 8; ++tx)
        {
            unsigned tile = CtrVideo_ObjTile(sprite->oam.tileNum, (unsigned)tx, (unsigned)ty,
                                             (unsigned)w, color256, mapping1d);
            unsigned offset = tile * 32;

            /* OBJ character data is the last 32 KiB of the 96 KiB VRAM bank. */
            if (offset + bytesPerTile > 0x8000)
                offset = 0;
            memcpy(dest + written, vram + offset, bytesPerTile);
            written += bytesPerTile;
        }
    }
    return written;
}

/* The atlas texel of the 8x8 block (bx, by), a block of 64 in Morton order. */
static uint16_t *AtlasBlock(uint16_t *atlas, unsigned bx, unsigned by)
{
    return atlas + (by * (VOXEL_SPRITE_ATLAS_W / 8) + bx) * 64u;
}

/*
 * Writes the gathered tiles into the slot's 64x64 cell of the atlas. A
 * sprite's tiles are whole 8x8 blocks of the atlas, flipped or not, so each
 * goes to one block; the colours are graded once each into a table, not per
 * pixel out of the 64 KiB grading table (a cache miss a pixel on an ARM11).
 * The same texels as a pixel at a time through CtrVideo_Texel.
 */
static void DecodeSlot(VoxelSpriteSlot *slot, unsigned index, uint16_t *atlas)
{
    static uint8_t morton[64];
    static bool mortonReady;
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned bytesPerTile = slot->color256 ? 64u : 32u, colours = slot->color256 ? 256u : 16u;
    int tilesX = slot->width / 8, tilesY = slot->height / 8;
    /* Clearing only what was drawn before, union the new extent, keeps a
     * walking sprite's per-frame cost at its own size rather than 64x64. */
    int clearW = slot->drawnWidth > slot->width ? slot->drawnWidth : slot->width;
    int clearH = slot->drawnHeight > slot->height ? slot->drawnHeight : slot->height;
    uint16_t graded[256];

    if (!mortonReady)
    {
        for (unsigned i = 0; i < 64; ++i)
            morton[i] = (uint8_t)CtrVideo_Texel(i & 7, i / 8, 8);
        mortonReady = true;
    }
    for (int by = 0; by < (clearH + 7) / 8; ++by)
        for (int bx = 0; bx < (clearW + 7) / 8; ++bx)
            memset(AtlasBlock(atlas, baseX / 8 + (unsigned)bx, baseY / 8 + (unsigned)by), 0,
                   64 * sizeof(uint16_t));
    slot->drawnWidth = slot->width;
    slot->drawnHeight = slot->height;
    for (unsigned c = 1; c < colours; ++c)
        graded[c] = VoxelGrade_RGBA5551(slot->palette[c]);

    for (int ty = 0; ty < tilesY; ++ty)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            const u8 *tile = slot->source + (unsigned)(ty * tilesX + tx) * bytesPerTile;
            unsigned ox = (unsigned)(slot->flipX ? tilesX - 1 - tx : tx);
            unsigned oy = (unsigned)(slot->flipY ? tilesY - 1 - ty : ty);
            uint16_t *block = AtlasBlock(atlas, baseX / 8 + ox, baseY / 8 + oy);
            unsigned flipX = slot->flipX ? 7u : 0u, flipY = slot->flipY ? 7u : 0u;

            for (unsigned py = 0; py < 8; ++py)
            {
                const uint8_t *row = morton + ((py ^ flipY) * 8);

                for (unsigned px = 0; px < 8; ++px)
                {
                    unsigned index8 = py * 8 + px;
                    unsigned colorIdx = slot->color256
                        ? tile[index8]
                        : ((tile[index8 / 2] >> ((px & 1) * 4)) & 0xF);

                    /* Colour index 0 is transparent; the alpha bit carries it. */
                    if (colorIdx != 0)
                        block[row[px ^ flipX]] = graded[colorIdx];
                }
            }
        }
    }
}

/*
 * Empty rows under the feet of an object's standing pose: frame 0 of its
 * picture table, facing south. Not of the frame showing: the art moves the
 * whole body between frames, a walk's steps a row lower than the standing
 * pose and a run's standing pose a row higher than its strides, and that
 * row is the bob of the walk. Standing on the ground, the pose seen most
 * meets its shadow; every other frame keeps its offset from it, as on the
 * GBA (VisibleRows).
 */
/* Object event pictures are 4bpp, tiles in rows (1D mapping). Bottom up,
 * the first row with an opaque pixel is the feet. */
static int FootPadOf(const u8 *tiles, int width, int height)
{
    int tilesX = width / 8;

    for (int y = height - 1; y >= 0; --y)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            const u8 *row = tiles + (u32)((y / 8) * tilesX + tx) * TILE_SIZE_4BPP + (y % 8) * 4;

            if (row[0] | row[1] | row[2] | row[3])
                return height - 1 - y;
        }
    }
    return 0;
}

/*
 * The standing pose is read only if it is in memory already. Its picture is
 * an asset of its own, and resolving one that is not loaded reads it off the
 * card - on the render thread, in the middle of the frame: every object that
 * came into view with a picture the game had not shown yet cost 10-20 ms on
 * hardware. Until it is, the pad is measured once on the frame the object
 * first showed (in VRAM already, the slot's own copy): the standing pose,
 * nearly always, and at worst a row off for as long as it takes the game to
 * load the picture - it is looked for again every frame.
 */
static int StandingFootPad(const struct ObjectEventGraphicsInfo *info,
                           const VoxelSpriteSlot *slot, bool *exact)
{
    const struct SpriteFrameImage *frame = &info->images[0];
    int tilesX = info->width / 8, tilesY = info->height / 8;
    u32 size = Port_GetSpriteFrameSize(frame->data, frame->size);
    const u8 *tiles = Port_PeekSpriteFramePointer(frame->data, size, frame->offset);

    *exact = tiles != NULL;
    if (tilesX <= 0 || tilesY <= 0)
        return 0;
    if (tiles == NULL)
    {
        if (slot->color256 || slot->width != info->width || slot->height != info->height
         || slot->sourceBytes < (u32)(tilesX * tilesY) * TILE_SIZE_4BPP)
            return 0;
        return FootPadOf(slot->source, info->width, info->height);
    }
    if (size < (u32)(tilesX * tilesY) * TILE_SIZE_4BPP)
        return 0;
    return FootPadOf(tiles, info->width, info->height);
}

/*
 * The size of a sprite's picture: its OAM's, or an object's own where a table
 * of subsprites draws it bigger than that - the moving truck is a 32x32 OAM
 * shown as 48x48 (sOamTable_48x48, its tiles row-major six wide), and its OAM
 * alone gave the card the truck's top-left corner (#36). The S.S. Tidal and
 * the submarine's shadow are wider than a slot: they keep their OAM's.
 */
static void PictureSize(const struct Sprite *sprite, const struct ObjectEvent *obj, int *w, int *h)
{
    const struct ObjectEventGraphicsInfo *info;

    GetSpriteDimensions(sprite->oam.shape, sprite->oam.size, w, h);
    if (obj == NULL || sprite->subspriteTables == NULL)
        return;
    info = GetObjectEventGraphicsInfo(obj->graphicsId);
    if ((info->width > *w || info->height > *h)
     && info->width <= (int)VOXEL_SPRITE_SLOT_DIM && info->height <= (int)VOXEL_SPRITE_SLOT_DIM)
    {
        *w = info->width;
        *h = info->height;
    }
}

/*
 * A picture to show in place of the one the game has in its VRAM: the frame
 * of the object's own picture table that faces the camera the way the object
 * faces in the world (ApparentFrame).
 */
typedef struct
{
    const u8 *tiles;
    u32 bytes;
    bool flipX, flipY;
} FrameOverride;

/*
 * Brings a slot up to date with its sprite (an object's, or NULL for a field
 * effect). Returns true if it was re-decoded, which only happens when
 * something the picture depends on actually changed. `frame` replaces the
 * picture in the game's VRAM with another of the object's frames.
 */
static bool RefreshSlot(unsigned index, const struct Sprite *sprite, const struct ObjectEvent *obj,
                        uint16_t *atlas, const FrameOverride *frame)
{
    VoxelSpriteSlot *slot = &sSlots[index];
    /* Static, not automatic: this runs in the VBlank handler and 4.5 KiB of
     * stack frame there is not worth the convenience. */
    static u8 source[VOXEL_SPRITE_MAX_BYTES];
    static u16 palette[256];
    int w, h;
    bool color256 = sprite->oam.bpp != 0;
    bool flipX = sprite->oam.affineMode == ST_OAM_AFFINE_OFF
              && (sprite->oam.matrixNum & 0x08) != 0;
    bool flipY = sprite->oam.affineMode == ST_OAM_AFFINE_OFF
              && (sprite->oam.matrixNum & 0x10) != 0;
    u32 sourceBytes, paletteBytes;
    u8 extPalette = color256 ? 0xFF : GetSpriteExtPalette(sprite);
    bool changed;

    PictureSize(sprite, obj, &w, &h);
    if (w > (int)VOXEL_SPRITE_SLOT_DIM || h > (int)VOXEL_SPRITE_SLOT_DIM)
        return false;

    if (frame != NULL && frame->tiles != NULL && !color256 && frame->bytes == (u32)(w * h / 2)
     && frame->bytes <= VOXEL_SPRITE_MAX_BYTES)
    {
        memcpy(source, frame->tiles, frame->bytes);
        sourceBytes = frame->bytes;
        flipX = frame->flipX;
        flipY = frame->flipY;
    }
    else
        sourceBytes = GatherSource(sprite, w, h, color256, source);
    paletteBytes = color256 ? 256 * 2 : 16 * 2;
    /* OBJ palettes are the second half of PLTT; 8bpp uses it as one bank.
     * One given an ext palette shows that, as the 2D compositor does. */
    if (extPalette != 0xFF)
        memcpy(palette, CtrVideo_ExtPalette(extPalette), paletteBytes);
    else
        memcpy(palette, PLTT + 0x200 + (color256 ? 0 : sprite->oam.paletteNum * 32), paletteBytes);

    changed = !slot->valid
           || slot->width != w || slot->height != h
           || slot->color256 != color256
           || slot->flipX != flipX || slot->flipY != flipY
           || slot->tileNum != sprite->oam.tileNum
           || slot->paletteNum != sprite->oam.paletteNum
           || slot->extPalette != extPalette
           || slot->sourceBytes != sourceBytes
           || memcmp(slot->source, source, sourceBytes) != 0
           || memcmp(slot->palette, palette, paletteBytes) != 0;
    if (!changed)
        return false;

    slot->valid = true;
    slot->width = w;
    slot->height = h;
    slot->color256 = color256;
    slot->flipX = flipX;
    slot->flipY = flipY;
    slot->tileNum = sprite->oam.tileNum;
    slot->paletteNum = sprite->oam.paletteNum;
    slot->extPalette = extPalette;
    slot->shape = sprite->oam.shape;
    slot->size = sprite->oam.size;
    slot->sourceBytes = sourceBytes;
    slot->paletteBytes = paletteBytes;
    memcpy(slot->source, source, sourceBytes);
    memcpy(slot->palette, palette, paletteBytes);
    DecodeSlot(slot, index, atlas);
    return true;
}

/* ── Billboards ─────────────────────────────────────────────────────────── */

/*
 * Rows of the sprite the card shows: all but the standing pose's empty rows
 * under the feet, so the card ends at the feet and they are on the ground.
 * Cut off rather than sunk under the ground: a step's feet, a row lower, would
 * be hidden by the terrain there, and the player's x-ray pass (ctr_voxel.c)
 * draws exactly what terrain hides.
 */
static int VisibleRows(const VoxelSpriteSlot *slot)
{
    int pad = slot->footPad < slot->height ? slot->footPad : slot->height - 1;

    return slot->height - pad;
}

/*
 * How far a card standing on (cx, cz) must come towards the camera so the
 * relief behind it stays behind it. A point of the drawing north of the feet,
 * dv tiles up the card, lands at depth v + h: on a flight of stairs (h rising
 * one pixel per pixel) that is exactly the card's own plane, and anything
 * steeper stands in front of it, so the ground drew over the walker. The card
 * is then moved along the line of sight (as far up as forward), which leaves
 * it where it was on screen and only changes what hides what. Flat ground and
 * walls (a south face lands behind the feet) give nothing.
 */
#define VOXEL_CARD_CLEAR (1.0f / VOXEL_PIXELS_PER_TILE)

static float CardPush(float cx, float cz, float halfW, float height)
{
    const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt((int)floorf(cx), (int)floorf(cz));
    float feet, worst = -1.0f;
    int x0 = (int)floorf(cx - halfW), x1 = (int)floorf(cx + halfW);
    int y0 = (int)floorf(cz - height), y1 = (int)floorf(cz);
    bool relief = false;

    for (int y = y0; y <= y1 && !relief; ++y)
        for (int x = x0; x <= x1 && !relief; ++x)
            relief = VoxelRelief_IsSlope(VoxelRelief_Cell(VoxelWorld_GetInstanceAt(x, y), x, y));
    if (inst == NULL || !relief)
        return 0.0f;
    feet = VoxelRelief_LiftAt(cx, cz);
    for (int i = -1; i <= 1; ++i)
    {
        float x = cx + i * halfW * 0.75f;

        for (float dv = VOXEL_CARD_CLEAR; dv <= height; dv += 2.0f * VOXEL_CARD_CLEAR)
        {
            float over = VoxelRelief_LiftAt(x, cz - dv) - feet - dv;

            if (over > worst)
                worst = over;
        }
    }
    return worst > -VOXEL_CARD_CLEAR ? worst + VOXEL_CARD_CLEAR : 0.0f;
}

/*
 * Furniture against a room's back wall is modelled from its drawing, and the
 * drawing comes forward of the wall: half a cell, to the middle of the cell
 * before it, where a walker's card stands - or further, a Pokemon Center's
 * PC two pixels past it. Card and model front then share a depth, or the
 * card is behind, and the model, drawn first, hid the walker in front of it.
 * The card comes along the line of sight to a pixel in front of the model's
 * face there, as on the GBA. Under either edge of the card, not only its
 * middle: walking along the furniture, half the card is over a model's cell
 * before its middle is, and beside a machine a sliver of it is.
 */
#define VOXEL_MODEL_PUSH_MAX 0.5f      /* further than that is not furniture's front */
#define VOXEL_MODEL_PUSH_HEIGHT 0.25f  /* where on the card the face is looked for */

static float ModelPush(float cx, float cz, float halfW)
{
    int y = (int)floorf(cz);
    float left = cx - halfW + VOXEL_CARD_CLEAR, right = cx + halfW - VOXEL_CARD_CLEAR;
    int x0 = (int)floorf(left), x1 = (int)floorf(right);
    float push = 0.0f;

    for (int x = x0; x <= x1; ++x)
    {
        const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt(x, y);
        float front, need = 0.0f;

        if (inst == NULL)
            continue;
        /* A model's face in the cell, whether or not the cell is the
         * model's: the PC's foot is drawn on the floor the walker uses it
         * from, in a cell that is the counter's. */
        if (VoxelBuildings_FrontAt(inst, x, y, left, right, VOXEL_MODEL_PUSH_HEIGHT, &front)
         && front + VOXEL_CARD_CLEAR > cz && front - cz + VOXEL_CARD_CLEAR <= VOXEL_MODEL_PUSH_MAX)
            need = front - cz + VOXEL_CARD_CLEAR;
        else if (VoxelBuildings_CellAt(inst, x, y, NULL, NULL))
            need = VOXEL_CARD_CLEAR;
        if (need > push)
            push = need;
    }
    return push;
}

/*
 * A card standing on the ground at (cx, cz), moved (offX, offZ) from there
 * and raised by `rise`: an object stands on the centre of its tile, feet on
 * the ground; a field effect that belongs to an object is drawn in that
 * object's place, moved from it as it is on the GBA screen.
 */
/* A sprite drawn on an OAM matrix (a visible wild Pokemon growing in,
 * wild_encounter.c) is that much smaller or larger; the matrix maps the
 * screen onto the picture, so its size is one over the matrix's. */
static float SpriteScale(const struct Sprite *sprite)
{
    s16 a;

    if (!(sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK))
        return 1.0f;
    a = (s16)gOamMatrices[sprite->oam.matrixNum & (OAM_MATRIX_COUNT - 1)].a;
    return a > 0 ? 256.0f / a : 1.0f;
}

static void EmitBillboard(VoxelBuilder *builder, const VoxelSpriteSlot *slot, unsigned index,
                          float cx, float cz, float offX, float offZ, float rise, float deck,
                          float rightX, float rightZ, float stretch, float shade, float scale)
{
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_W;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_W;
    int rows = VisibleRows(slot);
    /* Row 0 in memory is v=1, the convention the whole port uses. */
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_H;
    float v1 = 1.0f - (baseY + rows) / (float)VOXEL_SPRITE_ATLAS_H;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.5f * scale;
    float height = rows / VOXEL_PIXELS_PER_TILE * stretch * scale;
    /* On relief the sprite stands where its cell was lifted to, and rides
     * the lattice between cells, so a flight of stairs is climbed. */
    float lift = VoxelRelief_LiftAt(cx, cz) + rise + deck;
    float shift = VoxelRelief_ShiftAt(cx, cz) + deck;
    /* Up on a deck nothing of the ground or of a model's front is before
     * it: the deck is under its feet. */
    float push = deck > 0.0f ? 0.0f : CardPush(cx, cz, halfW, height);
    float model = deck > 0.0f ? 0.0f : ModelPush(cx, cz, halfW);

    if (model > push)
        push = model;
    /* Towards the camera: the right vector turned a quarter, unit length. */
    float along = push > 0.0f ? push / sqrtf(rightX * rightX + rightZ * rightZ) : 0.0f;
    float px = cx + offX - rightZ * along, pz = cz + offZ + shift + rightX * along;

    lift += push;
    float ax = px - rightX * halfW, az = pz - rightZ * halfW;
    float bx = px + rightX * halfW, bz = pz + rightZ * halfW;

    VoxelBuilder_Quad(builder,
        &(VoxelVertex){ax, lift,          az, u0, v1, shade},
        &(VoxelVertex){bx, lift,          bz, u1, v1, shade},
        &(VoxelVertex){bx, lift + height, bz, u1, v0, shade},
        &(VoxelVertex){ax, lift + height, az, u0, v0, shade});
}

/*
 * A mark on the ground (a ripple, footprints, tyre tracks) lies on it: a
 * card standing up would be a wall of footprints. The top of the picture is
 * north, as on the GBA screen.
 */
#define VOXEL_DECAL_LIFT 0.025f   /* over the ground, under cast shadows (0.03) */

static void EmitDecal(VoxelBuilder *builder, const VoxelSpriteSlot *slot, unsigned index,
                      float cx, float cz, float lift, float shade)
{
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_W;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_W;
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_H;
    float v1 = 1.0f - (baseY + slot->height) / (float)VOXEL_SPRITE_ATLAS_H;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.5f;
    float halfH = slot->height / VOXEL_PIXELS_PER_TILE * 0.5f;
    float y = VoxelRelief_LiftAt(cx, cz) + VOXEL_DECAL_LIFT + lift;
    float z = cz + VoxelRelief_ShiftAt(cx, cz);

    VoxelBuilder_Quad(builder,
        &(VoxelVertex){cx - halfW, y, z + halfH, u0, v1, shade},
        &(VoxelVertex){cx + halfW, y, z + halfH, u1, v1, shade},
        &(VoxelVertex){cx + halfW, y, z - halfH, u1, v0, shade},
        &(VoxelVertex){cx - halfW, y, z - halfH, u0, v0, shade});
}

/*
 * A door standing open. The game draws its frames on the tilemap, which this
 * view does not show: the walker went through a door that stayed shut, card
 * and all. Here the doorway is a black opening that widens as the door does -
 * on the wall the door is drawn on, or lying on the door's own cell where the
 * building is not modelled and its drawing lies flat - and whoever steps
 * through it is gone into the dark, as on the GBA they are behind the wall.
 */
bool8 PortDoor_Get(s16 *x, s16 *y, u8 *width, u8 *level);   /* field_door.c */

#define VOXEL_DOOR_DEPTH 0.03f    /* the opening, in front of its wall */
#define VOXEL_DOOR_HEIGHT 1.25f   /* the door's own metatile and the 4 pixels drawn over it */
#define VOXEL_DOOR_DUSK 0.5f      /* tiles before it over which a walker darkens */
#define VOXEL_DOOR_DARK 0.35f

typedef struct
{
    bool on, upright;
    int x, y, width;        /* world cells */
    float z;                /* the wall, or the cell's south edge */
    float open;             /* 0-1 */
} VoxelDoor;

static void FindDoor(VoxelDoor *door)
{
    /* The wall last found: looking for it reads the whole model. */
    static int lastLayout = -1, lastX, lastY;
    static bool lastUpright;
    static float lastZ;
    const VoxelMapInstance *inst;
    s16 x, y;
    u8 width, level;

    door->on = false;
    if (VoxelWorld_InstanceCount() == 0 || !PortDoor_Get(&x, &y, &width, &level))
        return;
    inst = VoxelWorld_Instance(0);
    door->on = true;
    door->x = x - MAP_OFFSET + inst->originX;
    door->y = y - MAP_OFFSET + inst->originY;
    door->width = width;
    door->open = level >= 3 ? 1.0f : level / 3.0f;
    if (lastLayout != inst->layoutId || lastX != door->x || lastY != door->y)
    {
        lastLayout = inst->layoutId;
        lastX = door->x;
        lastY = door->y;
        lastZ = (float)door->y + 1.0f;
        lastUpright = VoxelBuildings_CellAt(inst, door->x, door->y, NULL, NULL);
        if (lastUpright)
            VoxelBuildings_DoorWall(inst, door->x, door->y, &lastZ);
    }
    door->upright = lastUpright;
    door->z = lastZ;
}

/* In the doorway's column: 0 through it, up to 1 a little way before it. */
static float DoorLight(const VoxelDoor *door, float cx, float cz)
{
    float t;

    if (!door->on || cx < (float)door->x || cx >= (float)(door->x + door->width)
     || cz < (float)door->y || cz >= door->z + VOXEL_DOOR_DEPTH + VOXEL_DOOR_DUSK)
        return 1.0f;
    if (cz <= door->z + VOXEL_DOOR_DEPTH)
        return 0.0f;
    t = (cz - door->z - VOXEL_DOOR_DEPTH) / VOXEL_DOOR_DUSK;
    return VOXEL_DOOR_DARK + (1.0f - VOXEL_DOOR_DARK) * t;
}

/* The atlas has no black of its own: any opaque texel, drawn at no light. */
static bool OpaqueTexel(const uint16_t *atlas, float *u, float *v)
{
    for (unsigned s = 0; s < VOXEL_SPRITE_SLOTS; ++s)
    {
        unsigned baseX = (s % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
        unsigned baseY = (s / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;

        if (!sSlots[s].valid)
            continue;
        for (int y = 0; y < sSlots[s].height; ++y)
            for (int x = 0; x < sSlots[s].width; ++x)
                if (atlas[CtrVideo_Texel(baseX + (unsigned)x, baseY + (unsigned)y,
                                         VOXEL_SPRITE_ATLAS_W)] & 1)
                {
                    *u = (baseX + x + 0.5f) / (float)VOXEL_SPRITE_ATLAS_W;
                    *v = 1.0f - (baseY + y + 0.5f) / (float)VOXEL_SPRITE_ATLAS_H;
                    return true;
                }
    }
    return false;
}

static void EmitDoor(VoxelBuilder *builder, const uint16_t *atlas, const VoxelDoor *door)
{
    float cx, half, lift, shift, u, v;

    if (!door->on || !OpaqueTexel(atlas, &u, &v))
        return;
    cx = (float)door->x + door->width * 0.5f;
    half = door->width * 0.5f * door->open;
    lift = VoxelRelief_LiftAt(cx, (float)door->y + 0.5f);
    shift = VoxelRelief_ShiftAt(cx, (float)door->y + 0.5f);
    if (door->upright)
    {
        float z = door->z + VOXEL_DOOR_DEPTH + shift;

        VoxelBuilder_Quad(builder,
            &(VoxelVertex){cx - half, lift,                     z, u, v, 0.0f},
            &(VoxelVertex){cx + half, lift,                     z, u, v, 0.0f},
            &(VoxelVertex){cx + half, lift + VOXEL_DOOR_HEIGHT, z, u, v, 0.0f},
            &(VoxelVertex){cx - half, lift + VOXEL_DOOR_HEIGHT, z, u, v, 0.0f});
    }
    else
    {
        float y = lift + VOXEL_DECAL_LIFT + 0.01f, z = (float)door->y + shift;

        VoxelBuilder_Quad(builder,
            &(VoxelVertex){cx - half, y, z + 1.0f, u, v, 0.0f},
            &(VoxelVertex){cx + half, y, z + 1.0f, u, v, 0.0f},
            &(VoxelVertex){cx + half, y, z,        u, v, 0.0f},
            &(VoxelVertex){cx - half, y, z,        u, v, 0.0f});
    }
}

#if CTR_VOXEL_LIGHTING
/*
 * The billboard's own picture laid on the ground along the sun: the feet stay
 * where they are and every row of the sprite slides away from the sun by its
 * height, which is exactly where the fixed sun would put the shadow of a flat
 * card standing there. Same texture and texels as the sprite, so it walks,
 * turns and animates with it; one quad per object and no clipping work.
 *
 * The strength goes out in `shade`, which the shadow pass reads as alpha
 * (ctr_voxel.c). An object already in the scenery's shadow has no sun to
 * cast with, so its shadow fades out with the light it receives.
 */
#define VOXEL_CAST_SHADOW_ALPHA 0.36f
#define VOXEL_CAST_SHADOW_LIFT 0.03f    /* over decals (0.02) */
#define VOXEL_CAST_SHADOW_LIT 0.70f     /* sample at or below: no sun */

static void EmitCastShadow(VoxelBuilder *shadows, const VoxelSpriteSlot *slot, unsigned index,
                           float worldX, float worldZ, float rightX, float rightZ, float stretch,
                           float light, float lift, float deck)
{
    if (gVoxelSun.strength == 0.0f)
        return;
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_W;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_W;
    /* The rows the card shows (VisibleRows), from the feet: they meet their
     * own shadow. */
    int rows = VisibleRows(slot);
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_H;
    float v1 = 1.0f - (baseY + rows) / (float)VOXEL_SPRITE_ATLAS_H;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.5f;
    float height = rows / VOXEL_PIXELS_PER_TILE * stretch;
    /* Off the ground by `lift` (riding), the feet's shadow falls that much
     * further along the sun. */
    float cx = worldX + 0.5f + VOXEL_SUN_DX * lift, cz = worldZ + 0.5f + VOXEL_SUN_DZ * lift;
    float ax = cx - rightX * halfW, az = cz - rightZ * halfW;
    float bx = cx + rightX * halfW, bz = cz + rightZ * halfW;
    float sx = VOXEL_SUN_DX * height, sz = VOXEL_SUN_DZ * height;
    /* Each corner on the ground under it: at a face's foot the ground under
     * the shadow's far end is not the ground under the feet. */
    /* On a deck, on the deck: as high, and as far south (the 45-degree
     * rule, as the walker). */
    float ya = VoxelRelief_LiftAt(ax, az) + VOXEL_CAST_SHADOW_LIFT + deck;
    float yb = VoxelRelief_LiftAt(bx, bz) + VOXEL_CAST_SHADOW_LIFT + deck;
    float yc = VoxelRelief_LiftAt(bx + sx, bz + sz) + VOXEL_CAST_SHADOW_LIFT + deck;
    float yd = VoxelRelief_LiftAt(ax + sx, az + sz) + VOXEL_CAST_SHADOW_LIFT + deck;
    float za = VoxelRelief_ShiftAt(ax, az) + deck, zb = VoxelRelief_ShiftAt(bx, bz) + deck;
    float zc = VoxelRelief_ShiftAt(bx + sx, bz + sz) + deck;
    float zd = VoxelRelief_ShiftAt(ax + sx, az + sz) + deck;
    float strength = (light - VOXEL_CAST_SHADOW_LIT) / (1.0f - VOXEL_CAST_SHADOW_LIT);

    if (strength <= 0.0f)
        return;
    if (strength > 1.0f)
        strength = 1.0f;
    strength *= VOXEL_CAST_SHADOW_ALPHA * gVoxelSun.strength;
    VoxelBuilder_Quad(shadows,
        &(VoxelVertex){ax,      ya, az + za,      u0, v1, strength},
        &(VoxelVertex){bx,      yb, bz + zb,      u1, v1, strength},
        &(VoxelVertex){bx + sx, yc, bz + sz + zc, u1, v0, strength},
        &(VoxelVertex){ax + sx, yd, az + sz + zd, u0, v0, strength});
}

/*
 * The shadow of something lying flat a little over the ground - the surf mon
 * on the water: its own picture again on the ground, moved along the sun by
 * how high it is. Mostly under it, it shows as a dark rim on the far side,
 * which is what sets it on the water.
 */
static void EmitDecalShadow(VoxelBuilder *shadows, const VoxelSpriteSlot *slot, unsigned index,
                            float cx, float cz, float height, float light)
{
    if (gVoxelSun.strength == 0.0f)
        return;
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_W;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_W;
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_H;
    float v1 = 1.0f - (baseY + slot->height) / (float)VOXEL_SPRITE_ATLAS_H;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.5f;
    float halfH = slot->height / VOXEL_PIXELS_PER_TILE * 0.5f;
    float x = cx + VOXEL_SUN_DX * height, z0 = cz + VOXEL_SUN_DZ * height;
    float y = VoxelRelief_LiftAt(x, z0) + VOXEL_CAST_SHADOW_LIFT;
    float z = z0 + VoxelRelief_ShiftAt(x, z0);
    float strength = (light - VOXEL_CAST_SHADOW_LIT) / (1.0f - VOXEL_CAST_SHADOW_LIT);

    if (strength <= 0.0f)
        return;
    if (strength > 1.0f)
        strength = 1.0f;
    strength *= VOXEL_CAST_SHADOW_ALPHA * gVoxelSun.strength;
    VoxelBuilder_Quad(shadows,
        &(VoxelVertex){x - halfW, y, z + halfH, u0, v1, strength},
        &(VoxelVertex){x + halfW, y, z + halfH, u1, v1, strength},
        &(VoxelVertex){x + halfW, y, z - halfH, u1, v0, strength},
        &(VoxelVertex){x - halfW, y, z - halfH, u0, v0, strength});
}
#endif

/* Require every map cell under the actual, relief-shifted quad to be
 * reflective. Checking only the first south cell let a long reflection spill
 * across shorelines into grass. The small AABB is conservative when yawed. */
static bool IsReflectiveFootprint(float ax, float az, float bx, float bz, float length)
{
    float minX = fminf(ax, bx), maxX = fmaxf(ax, bx);
    float minZ = fminf(az, bz), maxZ = fmaxf(az, bz) + length;
    int firstX = (int)floorf(minX + 0.0001f);
    int lastX = (int)floorf(maxX - 0.0001f);
    int firstZ = (int)floorf(minZ + 0.0001f);
    int lastZ = (int)floorf(maxZ - 0.0001f);

    for (int z = firstZ; z <= lastZ; ++z)
    {
        for (int x = firstX; x <= lastX; ++x)
        {
            if (!VoxelWorld_IsVisibleReflectiveSurface(x, z))
                return false;
        }
    }
    return true;
}

/* A reflected object's quad. Grow only through cells that the world marks as
 * water or ice; the original ground effect merely says water is nearby. It is
 * cut across the card in VOXEL_REFLECTION_STRIPS strips, each grown over the
 * water under it alone: a step onto a one-tile puddle has the reflection come
 * in with the walker, as the GBA's shows through the water's own pixels,
 * instead of none until the card is wholly over the puddle and then all of it
 * at once. Neighbouring strips of one length are one quad. */
static void EmitReflection(VoxelBuilder *reflections, const VoxelSpriteSlot *slot,
                           unsigned index, float worldX, float worldZ,
                           float rightX, float rightZ, float stretch)
{
    unsigned baseX = (index % VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    unsigned baseY = (index / VOXEL_SPRITE_COLUMNS) * VOXEL_SPRITE_SLOT_DIM;
    float u0 = baseX / (float)VOXEL_SPRITE_ATLAS_W;
    float u1 = (baseX + slot->width) / (float)VOXEL_SPRITE_ATLAS_W;
    float v0 = 1.0f - baseY / (float)VOXEL_SPRITE_ATLAS_H;
    float v1 = 1.0f - (baseY + slot->height) / (float)VOXEL_SPRITE_ATLAS_H;
    float halfW = slot->width / VOXEL_PIXELS_PER_TILE * 0.5f;
    float cardLength = slot->height / VOXEL_PIXELS_PER_TILE * stretch;
    float fullLength = cardLength;
    float lengths[VOXEL_REFLECTION_STRIPS];
    float cx = worldX + 0.5f, nearZ = worldZ + 1.0f;
    float shift = VoxelRelief_ShiftAt(cx, nearZ + 0.5f);
    float y = VoxelRelief_LiftAt(cx, nearZ + 0.5f) + 0.035f;
    const unsigned strips = VOXEL_REFLECTION_STRIPS;

    if (fullLength > 1.75f) fullLength = 1.75f;
    for (unsigned s = 0; s < strips; ++s)
    {
        /* across the card: t = 0 at its left edge (u0), 1 at its right (u1) */
        float a = halfW * (2.0f * s / strips - 1.0f), b = halfW * (2.0f * (s + 1) / strips - 1.0f);
        float ax = cx + rightX * a, az = nearZ + rightZ * a + shift;
        float bx = cx + rightX * b, bz = nearZ + rightZ * b + shift;

        lengths[s] = 0.0f;
        for (float next = 0.25f; next <= fullLength + 0.0001f; next += 0.25f)
        {
            if (!IsReflectiveFootprint(ax, az, bx, bz, next))
                break;
            lengths[s] = next;
        }
    }
    for (unsigned s = 0, e; s < strips; s = e)
    {
        float length = lengths[s];

        for (e = s + 1; e < strips && lengths[e] == length; ++e)
            ;
        if (length < 0.25f)
            continue;
        float t0 = (float)s / strips, t1 = (float)e / strips;
        float a = halfW * (2.0f * t0 - 1.0f), b = halfW * (2.0f * t1 - 1.0f);
        float ax = cx + rightX * a, az = nearZ + rightZ * a + shift;
        float bx = cx + rightX * b, bz = nearZ + rightZ * b + shift;
        float ua = u0 + (u1 - u0) * t0, ub = u0 + (u1 - u0) * t1;
        float vFar = v1 + (v0 - v1) * (length / cardLength);

        VoxelBuilder_Quad(reflections,
            &(VoxelVertex){ax, y, az, ua, v1, 1.0f},
            &(VoxelVertex){bx, y, bz, ub, v1, 1.0f},
            &(VoxelVertex){bx, y, bz + length, ub, vFar, 1.0f},
            &(VoxelVertex){ax, y, az + length, ua, vFar, 1.0f});
    }
}

/* ── Field effects ──────────────────────────────────────────────────────── */

/*
 * Every other sprite on the map: the Pokémon the player surfs on, grass
 * shaking under a step, a trainer's "!", splashes, sand piles. The GBA draws
 * them over the map at a screen position; here each one is a card like the
 * objects' own, in an atlas slot no object event is using.
 *
 * One drawn over an object belongs to it (the surf mon, the grass, the icon
 * over a head) and stands in that object's place, moved from it by as much as
 * on the GBA screen, just in front of it or just behind it as the GBA orders
 * the two. One drawn behind the object and below its feet is what the object
 * rides - the surf mon - and lifts it: the rider sits on it as on the GBA, and
 * the mon floats on the water instead of sinking under it. A sprite on its own
 * stands where its feet are drawn, placed from the player; a mark on the
 * ground lies on it (EmitDecal).
 *
 * Not here: object shadows and reflections, which this renderer draws its own
 * way; the weather, drawn over the view (ComposeVoxelOverlay); and sprites
 * placed on the screen rather than on the map (the mon shown for a field
 * move, the bird of Fly and its rider), drawn over the view too.
 */
bool8 CtrSprite_IsVoxelWeather(const struct Sprite *sprite);   /* sprite.c */

#define VOXEL_EFFECTS_MAX VOXEL_SPRITE_SLOTS
/* How far an effect card sits in front of or behind the object it belongs to. */
#define VOXEL_EFFECT_DEPTH 0.04f
/* The surf mon lies on the water (the GBA draws it from above, an oval) and
 * its rider sits on its back this much over it, tiles. */
#define VOXEL_SURF_SEAT 0.04f
/* How high the mon's back stands over the water, for its shadow, tiles. */
#define VOXEL_SURF_BODY 0.12f

typedef struct
{
    bool drawn;
    float worldX, worldZ;    /* the tile, world */
    int screenX;             /* GBA screen: centre, */
    int top, base, feet;     /* top edge, bottom edge, feet (no y2) */
    const struct Sprite *sprite;
    float rise;              /* onto what it rides */
    float deck;              /* up on a bridge's deck, tiles */
    float shade;
    bool outdoor;            /* casts a shadow */
} VoxelObjectCard;

typedef struct
{
    const struct Sprite *sprite;
    unsigned slot;
    int owner;               /* object index, or -1 */
    bool front, decal, tile;
    bool surf;               /* the surf mon: lying on the water, ridden */
} VoxelEffectCard;

static bool IsTemplate(const struct SpriteTemplate *template, unsigned first, unsigned last)
{
    for (unsigned i = first; i <= last; ++i)
        if (template == gFieldEffectObjectTemplatePointers[i])
            return true;
    return false;
}

/* Grass that rustles where it was stepped on keeps that tile in its data
 * (field_effect_helpers.c: sX, sY) and stays there while its object walks on:
 * placed from the object, it floated in the object's plane, off its tile. */
static bool IsTileEffect(const struct SpriteTemplate *template)
{
    return template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_TALL_GRASS]
        || template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_LONG_GRASS];
}

static bool IsDecal(const struct SpriteTemplate *template)
{
    return template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_RIPPLE]
        || template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_SAND_FOOTPRINTS]
        || template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_DEEP_SAND_FOOTPRINTS]
        || template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_BIKE_TIRE_TRACKS];
}

/* wild_encounter.c, from the optional visible wild Pokemon (patch 0056);
 * without it no sprite is one. */
bool8 VisibleWild_IsSprite(u8 spriteId);

bool8 __attribute__((weak)) VisibleWild_IsSprite(u8 spriteId)
{
    (void)spriteId;
    return FALSE;
}

/*
 * A slot for field effect sprite `id`: the one it had, if no object has taken
 * it back, or else a free one - past the objects' first (an object that comes
 * into use takes its own slot back, and the effect's picture is decoded
 * again elsewhere), then one whose object event is not in use. -1 when every
 * slot is taken.
 */
static int EffectSlot(unsigned id, bool claimed[VOXEL_SPRITE_SLOTS])
{
    int found = -1;

    for (unsigned s = 0; s < VOXEL_SPRITE_SLOTS; ++s)
    {
        if (claimed[s]) continue;
        if (sSlotEffect[s] == id + 1) { found = (int)s; break; }
        if (sSlotEffect[s] == 0 && (found < 0 || (found < OBJECT_EVENTS_COUNT && s >= OBJECT_EVENTS_COUNT)))
            found = (int)s;
    }
    if (found < 0)
        return -1;
    if (sSlotEffect[found] != id + 1)
    {
        sSlotEffect[found] = (u8)(id + 1);
        sSlots[found].valid = false;
        sSlots[found].footPad = 0;
        sSlots[found].footPadImages = NULL;
    }
    claimed[found] = true;
    return found;
}

/* Drawn over `object` on the GBA screen: sprites come first in OAM, and so
 * on top, by priority and then subpriority. */
static bool DrawnOver(const struct Sprite *sprite, const struct Sprite *object)
{
    if (sprite->oam.priority != object->oam.priority)
        return sprite->oam.priority < object->oam.priority;
    return sprite->subpriority <= object->subpriority;
}

static int SpriteHeight(const struct Sprite *sprite)
{
    int w, h;

    GetSpriteDimensions(sprite->oam.shape, sprite->oam.size, &w, &h);
    return h;
}

/* The object an effect is drawn over, or -1: its centre inside the object's
 * picture, the nearest to the object's middle. */
static int EffectOwner(const struct Sprite *sprite, const VoxelObjectCard *objects)
{
    int x = sprite->x + sprite->x2;
    int y = sprite->y + sprite->y2 + sprite->centerToCornerVecY + SpriteHeight(sprite) / 2;
    int best = -1, bestDistance = 0x7fff;

    for (unsigned i = 0; i < OBJECT_EVENTS_COUNT; ++i)
    {
        const VoxelObjectCard *object = &objects[i];
        int y2, distance;

        if (!object->drawn || abs(x - object->screenX) > 8)
            continue;
        y2 = object->sprite->y2;
        if (y < object->top + y2 || y > object->base + y2)
            continue;
        distance = abs(y - (object->top + object->base) / 2 - y2);
        if (distance < bestDistance)
        {
            best = (int)i;
            bestDistance = distance;
        }
    }
    return best;
}

/* ── Facing the camera ───────────────────────────────────────────────────── */

/*
 * Object pictures are one frame per way an object faces (and a few more to
 * walk by), the game choosing by the direction it faces in the world. From a
 * camera that looks any way, the picture to show is the one for the way the
 * object faces *across the camera*: facing the camera, the front; away from
 * it, the back; to the camera's left or right, that side. On the orbit
 * camera (looking north) this is the picture the game chose, so only the
 * free-roam camera needs it.
 *
 * It is taken from the object's own animation table, which keeps the walk
 * cycle: the same position in the same animation, of the facing the camera
 * sees. The standard table (every NPC that walks) and the player's lay their
 * animations out in groups of four, south, north, west, east, which is what
 * makes this a matter of changing the number's last two bits. Any other
 * table (a Pokémon, a boulder, a door) is shown as the game has it.
 */
#define VOXEL_ANIM_STD_COUNT 20
#define VOXEL_ANIM_PLAYER_COUNT 24
#define VOXEL_PI 3.14159265358979323846f

static const union AnimCmd *const *sStandardAnims;
static const union AnimCmd *const *sPlayerAnims;

static void FindAnimTables(void)
{
    if (sStandardAnims == NULL)
        sStandardAnims = GetObjectEventGraphicsInfo(OBJ_EVENT_GFX_MAN_1)->anims;
    if (sPlayerAnims == NULL)
        sPlayerAnims = GetObjectEventGraphicsInfo(OBJ_EVENT_GFX_BRENDAN_NORMAL)->anims;
}

/* The bearing (yaw: 0 south, pi/2 east) of a group's member. */
static float AnimFacingAngle(unsigned member)
{
    static const float angle[4] = {0.0f, VOXEL_PI, -0.5f * VOXEL_PI, 0.5f * VOXEL_PI};

    return angle[member & 3u];
}

/* The member (south, north, west, east) an object facing `facing` shows to a
 * camera that sees it from `toCamera` (both bearings). */
static unsigned ApparentMember(float facing, float toCamera)
{
    float delta = facing - toCamera;

    while (delta > VOXEL_PI) delta -= 2.0f * VOXEL_PI;
    while (delta <= -VOXEL_PI) delta += 2.0f * VOXEL_PI;
    if (delta >= -0.25f * VOXEL_PI && delta <= 0.25f * VOXEL_PI)
        return 0; /* towards the camera: the front */
    if (delta > 0.25f * VOXEL_PI && delta <= 0.75f * VOXEL_PI)
        return 3; /* across it to the right: east's picture */
    if (delta < -0.25f * VOXEL_PI && delta >= -0.75f * VOXEL_PI)
        return 2;
    return 1;
}

/* The frame to show for an object card at (cx, cz), or false to show the
 * game's own. */
static bool ApparentFrame(const struct ObjectEvent *obj, const struct Sprite *sprite,
                          const VoxelCamera *camera, float cx, float cz, int w, int h,
                          FrameOverride *out)
{
    const struct ObjectEventGraphicsInfo *info;
    const struct SpriteFrameImage *image;
    const union AnimCmd *cmd;
    VoxelFreeMove *move = VoxelFreeMove_Get();
    unsigned num, base, target, index;
    bool player = IsPlayerObject(obj), walking = false;
    float facing, toCamera;
    u32 size;

    if (camera->cardBlend <= 0.0f || sprite->anims == NULL || sprite->oam.bpp != 0)
        return false;
    FindAnimTables();
    num = sprite->animNum;
    if (sprite->anims == sPlayerAnims)
    {
        if (!player && num >= VOXEL_ANIM_STD_COUNT)
            return false;
        if (num >= VOXEL_ANIM_PLAYER_COUNT)
            return false;
    }
    else if (sprite->anims == sStandardAnims)
    {
        if (num >= VOXEL_ANIM_STD_COUNT)
            return false;
    }
    else
        return false;
    info = GetObjectEventGraphicsInfo(obj->graphicsId);
    if (info == NULL || info->images == NULL)
        return false;

    base = num & ~3u;
    facing = AnimFacingAngle(num);
    if (player && move->blend > 0.0f && !GameOwnsPlayer(obj))
    {
        /* Walking free, the player faces where it walks, and walks while it
         * moves, with the cycle by the distance it has gone. */
        if (VoxelFreeMove_Moving(move))
        {
            facing = move->heading;
            walking = true;
            base = num >= VOXEL_ANIM_STD_COUNT ? VOXEL_ANIM_STD_COUNT : 4u;
        }
    }
    toCamera = VoxelCamera_CardYaw(camera, cx, cz) * (VOXEL_PI / 180.0f);
    target = base + ApparentMember(facing, toCamera);
    index = sprite->animCmdIndex;
    if (walking)
    {
        float cycle = move->travel - floorf(move->travel);

        index = (unsigned)(cycle * 4.0f) & 3u;
    }
    cmd = &sprite->anims[target][index];
    if (cmd->type < 0)
    {
        cmd = &sprite->anims[target][0];
        if (cmd->type < 0)
            return false;
    }
    image = &info->images[cmd->frame.imageValue];
    size = Port_GetSpriteFrameSize(image->data, image->size);
    if (size != (u32)(w * h / 2))
        return false;
    out->tiles = Port_PeekSpriteFramePointer(image->data, size, image->offset);
    out->bytes = size;
    out->flipX = cmd->frame.hFlip != 0;
    out->flipY = cmd->frame.vFlip != 0;
    return out->tiles != NULL;
}

void VoxelEntities_WarmPlayerFrames(void)
{
    const struct ObjectEvent *obj;
    const struct ObjectEventGraphicsInfo *info;
    const struct Sprite *sprite;
    unsigned seen[32], seenCount = 0;

    if (gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
        return;
    obj = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (!obj->active || obj->spriteId >= MAX_SPRITES)
        return;
    sprite = &gSprites[obj->spriteId];
    FindAnimTables();
    if (sprite->anims != sPlayerAnims || sprite->oam.bpp != 0)
        return;
    info = GetObjectEventGraphicsInfo(obj->graphicsId);
    if (info == NULL || info->images == NULL)
        return;
    /* Every picture the player's animations show, read now - on the game's
     * thread, a frame at a time as the game does for the one it plays -
     * rather than on the render thread the first time it is turned to. */
    for (unsigned anim = 0; anim < VOXEL_ANIM_PLAYER_COUNT; ++anim)
    {
        const union AnimCmd *cmds = sprite->anims[anim];

        for (unsigned c = 0; cmds[c].type >= 0 && c < 8; ++c)
        {
            unsigned value = (unsigned)cmds[c].frame.imageValue, i;
            const struct SpriteFrameImage *image;
            u32 size;

            for (i = 0; i < seenCount && seen[i] != value; ++i)
                ;
            if (i < seenCount || seenCount >= ARRAY_COUNT(seen))
                continue;
            seen[seenCount++] = value;
            image = &info->images[value];
            size = Port_GetSpriteFrameSize(image->data, image->size);
            (void)Port_ResolveSpriteFramePointer(image->data, size, image->offset);
        }
    }
}

unsigned VoxelEntities_Emit(VoxelBuilder *builder, uint16_t *atlas, const VoxelCamera *camera,
                            VoxelBuilder *shadows, VoxelBuilder *reflections)
{
    /* The billboards turn about the vertical axis to face the camera. With the
     * default yaw of 0 this is (1,0,0), the same plane the reference uses.
     * The card stands upright and the camera looks down on it, so on screen
     * its height comes out cos(pitch) short. That is made up half on each
     * side: the width comes down by sqrt(cos(pitch)) and the height goes up
     * by as much, so on screen both are sqrt(cos(pitch)) of the sprite's
     * (0.875 at 40 degrees). The proportions are the sprite's, and so is the
     * area on screen of the plain card. All of it off the width left the
     * characters small, all of it on the height big. Still upright, so it
     * sorts against walls as before. Shadow and reflection are the card's
     * own, so they take the same size. */
    float narrow = sqrtf(cosf(VoxelCamera_CardPitch(camera) * (3.14159265358979323846f / 180.0f)));
    float stretch = 1.0f / narrow;
    float pixel = stretch / VOXEL_PIXELS_PER_TILE;
    /* Static, not automatic: this runs in the VBlank handler. */
    static VoxelObjectCard objects[OBJECT_EVENTS_COUNT];
    static VoxelEffectCard effects[VOXEL_EFFECTS_MAX];
    static bool objectSprite[MAX_SPRITES];
    static u16 objectTiles[OBJECT_EVENTS_COUNT];
    bool claimed[VOXEL_SPRITE_SLOTS] = {false};
    unsigned objectTileCount = 0, effectCount = 0;
    unsigned updates = 0;
    VoxelDoor door;
    sPlayerVertexFirst = -1;

    /* The lighting caches are the chunks' own, valid until the world
     * changes (ctr_voxel.c resets them then), so they are not reset here. */
#if !CTR_VOXEL_LIGHTING
    (void)shadows;
#endif
    memset(objectSprite, 0, sizeof(objectSprite));
    for (unsigned i = 0; i < OBJECT_EVENTS_COUNT; ++i)
    {
        const struct ObjectEvent *obj = &gObjectEvents[i];
        const struct Sprite *sprite;
        VoxelObjectCard *card = &objects[i];
        float worldX = 0.0f, worldZ = 0.0f;
        float shade = 1.0f;
        bool outdoor = false;

        card->drawn = false;
        if (!obj->active)
            continue;
        /* An object in use takes its slot back from any field effect. */
        claimed[i] = true;
        if (sSlotEffect[i] != 0)
        {
            sSlotEffect[i] = 0;
            sSlots[i].valid = false;
            sSlots[i].footPadImages = NULL;
        }
        if (obj->spriteId >= MAX_SPRITES)
            continue;
        sprite = &gSprites[obj->spriteId];
        objectSprite[obj->spriteId] = true;
        objectTiles[objectTileCount++] = sprite->oam.tileNum;
        /*
         * Not sprite->invisible: the game also sets that for an object that
         * has left the 2D field of view (offScreen), and the 3D camera sees
         * twice as far north. Only the object's own flag hides it here.
         */
        if (obj->invisible)
            continue;
        /* Carried on the screen instead of the map - the rider of Fly's
         * bird - it is drawn with the screen's sprites, over the view. */
        if (!sprite->coordOffsetEnabled)
            continue;

        GetObjectWorldPos(obj, sprite, &worldX, &worldZ);
        /* Object coordinates are map-local; the mesh is in world space. */
        if (VoxelWorld_InstanceCount() > 0)
        {
            const VoxelMapInstance *inst = VoxelWorld_Instance(0);

            worldX += (float)inst->originX;
            worldZ += (float)inst->originY;
        }
        /* Active events may be dozens of tiles beyond the camera, including
         * across map connections. They cannot appear in this view, and packing
         * them relative to the camera's tile overflows the 16-bit format. */
        if (fabsf(worldX - camera->targetX) > 24.0f
         || fabsf(worldZ - camera->targetZ) > 24.0f)
            continue;
        {
            FrameOverride frame;
            int pictureW, pictureH;
            bool facingCamera;

            PictureSize(sprite, obj, &pictureW, &pictureH);
            frame.tiles = NULL;
            facingCamera = ApparentFrame(obj, sprite, camera, worldX + 0.5f, worldZ + 0.5f,
                                         pictureW, pictureH, &frame);
            if (RefreshSlot(i, sprite, obj, atlas, facingCamera ? &frame : NULL))
                ++updates;
        }
        if (!sSlots[i].valid)
            continue;
        {
            const struct ObjectEventGraphicsInfo *info = GetObjectEventGraphicsInfo(obj->graphicsId);

            bool changed = sSlots[i].footPadImages != info->images;

            if (changed || !sSlots[i].footPadExact)
            {
                bool exact = true;
                int pad = info->images != NULL ? StandingFootPad(info, &sSlots[i], &exact) : 0;

                sSlots[i].footPadImages = info->images;
                /* A provisional pad is taken once, never from each frame of a
                 * walk: that would undo its bob. */
                if (changed || exact)
                    sSlots[i].footPad = pad;
                sSlots[i].footPadExact = exact;
            }
        }
        card->deck = ObjectDeck(obj, worldX, worldZ);
#if CTR_VOXEL_LIGHTING
        {
            const VoxelMapInstance *inst = VoxelWorld_GetInstanceAt((int)floorf(worldX + 0.5f),
                                                                   (int)floorf(worldZ + 0.5f));
            outdoor = inst != NULL && !inst->indoor;
            /* up on a bridge's deck, the light over the deck, not under it */
            if (outdoor)
                shade = VoxelLighting_Sample(worldX + 0.5f, 0.75f + card->deck,
                                             worldZ + 0.5f + card->deck);
        }
#endif
        card->drawn = true;
        card->worldX = worldX;
        card->worldZ = worldZ;
        card->screenX = sprite->x + sprite->x2;
        /* A picture bigger than its OAM (PictureSize) is centred on the
         * sprite as its subsprites are: half the difference higher. */
        card->top = sprite->y + sprite->centerToCornerVecY
                  - (sSlots[i].height - SpriteHeight(sprite)) / 2;
        card->base = card->top + sSlots[i].height;
        card->feet = card->base - (sSlots[i].height - VisibleRows(&sSlots[i]));
        card->sprite = sprite;
        card->rise = 0.0f;
        card->shade = shade;
        card->outdoor = outdoor;
    }

    for (unsigned id = 0; id < MAX_SPRITES && effectCount < VOXEL_EFFECTS_MAX; ++id)
    {
        const struct Sprite *sprite = &gSprites[id];
        VoxelEffectCard *effect = &effects[effectCount];
        unsigned t;
        int w, h, slot;

        if (!sprite->inUse || sprite->invisible || !sprite->coordOffsetEnabled || objectSprite[id])
            continue;
        if (IsTemplate(sprite->template, FLDEFFOBJ_SHADOW_S, FLDEFFOBJ_SHADOW_XL)
         || CtrSprite_IsVoxelWeather(sprite))
            continue;
        /* A reflection is a copy of its object's picture. */
        for (t = 0; t < objectTileCount && objectTiles[t] != sprite->oam.tileNum; ++t)
            ;
        if (t < objectTileCount)
            continue;
        GetSpriteDimensions(sprite->oam.shape, sprite->oam.size, &w, &h);
        if (w > (int)VOXEL_SPRITE_SLOT_DIM || h > (int)VOXEL_SPRITE_SLOT_DIM)
            continue;
        slot = EffectSlot(id, claimed);
        if (slot < 0)
        {
            /* Not drawn this frame: logged, at most every ten seconds. */
            static uint32_t sLastFull;

            if (sLastFull == 0 || gMain.vblankCounter1 - sLastFull >= 600)
            {
                sLastFull = gMain.vblankCounter1 ? gMain.vblankCounter1 : 1;
                CtrLog_Write(CTR_LOG_VIDEO, "VOXEL sprite slots full: %u effects drawn, the rest left out",
                             effectCount);
            }
            break;
        }
        effect->sprite = sprite;
        effect->slot = (unsigned)slot;
        effect->surf = sprite->template == gFieldEffectObjectTemplatePointers[FLDEFFOBJ_SURF_BLOB];
        effect->decal = IsDecal(sprite->template);
        effect->tile = IsTileEffect(sprite->template);
        /* A visible wild Pokemon stands on its own tile, never on an object:
         * a tile south of the player, owned by it, it sank under the ground. */
        effect->owner = effect->decal || effect->tile || VisibleWild_IsSprite((u8)id)
                      ? -1 : EffectOwner(sprite, objects);
        effect->front = effect->owner < 0 || DrawnOver(sprite, objects[effect->owner].sprite);
        /* Behind its object and below its feet: what the object rides. Only
         * the surf mon is ridden; grass behind a walker's feet, as it steps
         * onto or off a tile of it, lifted the walker a moment. The mon is
         * seen from above, an oval on the water: stood up as a card it was a
         * wall with the rider perched on top of it, in the air. It lies on
         * the water instead (a decal placed from the player, bobbing as the
         * GBA bobs it) and the rider sits just over its back. */
        if (effect->surf)
        {
            if (effect->owner >= 0 && !effect->front && VOXEL_SURF_SEAT > objects[effect->owner].rise)
                objects[effect->owner].rise = VOXEL_SURF_SEAT;
            effect->decal = true;
            effect->owner = -1;
            effect->front = true;
        }
        ++effectCount;
    }
    /* Slots neither an object nor an effect wants this frame. */
    for (unsigned s = 0; s < VOXEL_SPRITE_SLOTS; ++s)
    {
        if (claimed[s]) continue;
        sSlotEffect[s] = 0;
        sSlots[s].valid = false;
    }

    FindDoor(&door);
    EmitDoor(builder, atlas, &door);
    for (unsigned i = 0; i < OBJECT_EVENTS_COUNT; ++i)
    {
        VoxelObjectCard *card = &objects[i];
        float rise, dusk;
        float rightX, rightZ, towardX, towardZ;

        if (!card->drawn)
            continue;
        (void)towardX; (void)towardZ;
        CardAxes(camera, card->worldX + 0.5f, card->worldZ + 0.5f, narrow,
                 &rightX, &rightZ, &towardX, &towardZ);
        /* Through an open door: gone, shadow and all. */
        dusk = DoorLight(&door, card->worldX + 0.5f, card->worldZ + 0.5f);
        if (dusk <= 0.0f)
            continue;
        /* Riding: up and down with what it rides, as the GBA bobs both. */
        rise = card->rise > 0.0f ? card->rise - card->sprite->y2 * pixel : 0.0f;
        /* Jumping: the arc the GBA draws, over a shadow left on the ground. */
        if (IsJumping(&gObjectEvents[i], card->sprite))
            rise -= card->sprite->y2 * pixel;
#if CTR_VOXEL_LIGHTING
        /* Riding, it is off the ground: its shadow falls from its feet,
         * moved along the sun by how high they are, onto the water past
         * the mon's back. */
        if (shadows != NULL && card->outdoor)
            EmitCastShadow(shadows, &sSlots[i], i, card->worldX, card->worldZ, rightX, rightZ,
                           stretch, card->shade, card->rise > 0.0f ? rise : 0.0f, card->deck);
#endif
        /* In the head of the player (1ST) the card would fill the lens from
         * inside: its cast shadow stays, the card and its reflection go. */
        if (camera->hidePlayer && i == gPlayerAvatar.objectEventId)
            continue;
        if (reflections != NULL && gObjectEvents[i].hasReflection)
            EmitReflection(reflections, &sSlots[i], i, card->worldX, card->worldZ,
                           rightX, rightZ, stretch);
        unsigned first = builder->count;
        EmitBillboard(builder, &sSlots[i], i, card->worldX + 0.5f, card->worldZ + 0.5f,
                      0.0f, 0.0f, rise, card->deck, rightX, rightZ, stretch, card->shade * dusk, 1.0f);
        if (i == gPlayerAvatar.objectEventId && builder->count == first + 6)
            sPlayerVertexFirst = (int)first;
    }

    for (unsigned e = 0; e < effectCount; ++e)
    {
        const VoxelEffectCard *effect = &effects[e];
        const struct Sprite *sprite = effect->sprite;
        const VoxelObjectCard *reference;
        int h, x, base;

        if (RefreshSlot(effect->slot, sprite, NULL, atlas, NULL))
            ++updates;
        if (!sSlots[effect->slot].valid)
            continue;
        h = sSlots[effect->slot].height;
        x = sprite->x + sprite->x2;
        base = sprite->y + sprite->centerToCornerVecY + h;
        if (effect->tile)
        {
            /* Lying on its own tile over the ground's drawing of it, which it
             * animates: stood up as a card it read as a flat picture on top
             * of the tile instead of the tile itself. */
            float cx = (float)(sprite->data[1] - MAP_OFFSET) + 0.5f;
            float cz = (float)(sprite->data[2] - MAP_OFFSET) + 0.5f;
            float shade = 1.0f;

            if (gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT
             && objects[gPlayerAvatar.objectEventId].drawn)
                shade = objects[gPlayerAvatar.objectEventId].shade;

            if (VoxelWorld_InstanceCount() > 0)
            {
                cx += (float)VoxelWorld_Instance(0)->originX;
                cz += (float)VoxelWorld_Instance(0)->originY;
            }
            if (fabsf(cx - camera->targetX) > 24.0f || fabsf(cz - camera->targetZ) > 24.0f)
                continue;
            EmitDecal(builder, &sSlots[effect->slot], effect->slot, cx, cz, 0.0f, shade);
            continue;
        }
        if (effect->owner >= 0)
        {
            const VoxelObjectCard *object = &objects[effect->owner];
            float depth = effect->front ? VOXEL_EFFECT_DEPTH : -VOXEL_EFFECT_DEPTH;
            float along = (x - object->screenX) / VOXEL_PIXELS_PER_TILE;
            /* The owner's feet on the ground, lifted by what it rides. */
            float rise = object->rise + (object->feet - base - sprite->y2) * pixel;
            float rightX, rightZ, towardX, towardZ;

            CardAxes(camera, object->worldX + 0.5f, object->worldZ + 0.5f, narrow,
                     &rightX, &rightZ, &towardX, &towardZ);
            EmitBillboard(builder, &sSlots[effect->slot], effect->slot,
                          object->worldX + 0.5f, object->worldZ + 0.5f,
                          rightX * along + towardX * depth, rightZ * along + towardZ * depth,
                          rise, object->deck, rightX, rightZ, stretch, object->shade, SpriteScale(sprite));
            continue;
        }
        /* On its own: placed from the player, whose feet are on the centre of
         * its tile and whose picture ends at the tile's bottom edge. */
        reference = gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT
                  ? &objects[gPlayerAvatar.objectEventId] : NULL;
        if (reference == NULL || !reference->drawn)
            continue;
        {
            float cx = reference->worldX + 0.5f + (x - reference->screenX) / VOXEL_PIXELS_PER_TILE;
            float cz;

            if (effect->decal)
            {
                /* Its middle on the ground: a tile's middle is 8 px above
                 * its bottom edge. */
                cz = reference->worldZ + 0.5f
                   + (base - h / 2 - (reference->base - 8)) / VOXEL_PIXELS_PER_TILE;
                if (fabsf(cx - camera->targetX) > 24.0f || fabsf(cz - camera->targetZ) > 24.0f)
                    continue;
                if (effect->surf)
                {
                    /* Up and down with the GBA's bob, its rider with it. */
                    float lift = sprite->y2 < 0 ? -sprite->y2 * pixel : 0.0f;

                    /* Under its rider as the GBA draws the two: the rider's
                     * feet a little in front of the oval's middle. Placed as
                     * ground, it lay 8 px south of that - the rider's card
                     * stands on its tile's middle, where the GBA draws feet
                     * at the tile's bottom edge - and the rider sat on its
                     * back rim instead. */
                    cz -= 8.0f / VOXEL_PIXELS_PER_TILE;

                    EmitDecal(builder, &sSlots[effect->slot], effect->slot, cx, cz, lift,
                              reference->shade);
#if CTR_VOXEL_LIGHTING
                    if (shadows != NULL && reference->outdoor)
                        EmitDecalShadow(shadows, &sSlots[effect->slot], effect->slot, cx, cz,
                                        lift + VOXEL_SURF_BODY, reference->shade);
#endif
                    continue;
                }
                EmitDecal(builder, &sSlots[effect->slot], effect->slot, cx, cz, 0.0f, reference->shade);
                continue;
            }
            float rightX, rightZ, towardX, towardZ;

            cz = reference->worldZ + 0.5f + (base - reference->base) / VOXEL_PIXELS_PER_TILE;
            if (fabsf(cx - camera->targetX) > 24.0f || fabsf(cz - camera->targetZ) > 24.0f)
                continue;
            CardAxes(camera, cx, cz, narrow, &rightX, &rightZ, &towardX, &towardZ);
            (void)towardX; (void)towardZ;
            EmitBillboard(builder, &sSlots[effect->slot], effect->slot, cx, cz, 0.0f, 0.0f,
                          -sprite->y2 * pixel, reference->deck, rightX, rightZ, stretch,
                          reference->shade, SpriteScale(sprite));
        }
    }
    return updates;
}
