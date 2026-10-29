/*
 * Host test of voxel_sprite_slots.c on synthetic pictures and slot tables.
 *
 *   cc -std=c99 -Wall -Wextra -I3ds_port/src/voxel \
 *      3ds_port/tests/voxel_sprite_slots_test.c 3ds_port/src/voxel/voxel_sprite_slots.c \
 *      -o /tmp/voxel_sprite_slots_test && /tmp/voxel_sprite_slots_test
 */
#include <stdio.h>
#include <string.h>

#include "voxel_sprite_slots.h"

#define SLOT_DIM 64u
#define MAX_BYTES (SLOT_DIM * SLOT_DIM)
#define SLOTS 32u
#define OBJECTS 16u

static int sFailures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++sFailures; } } while (0)

/* ── Fitting ─────────────────────────────────────────────────────────────── */

static void TestAtlasWidth(void)
{
    CHECK(VoxelSlots_AtlasWidth(16, SLOT_DIM) == 16);
    CHECK(VoxelSlots_AtlasWidth(48, SLOT_DIM) == 48);   /* the truck, as before */
    CHECK(VoxelSlots_AtlasWidth(64, SLOT_DIM) == 64);
    CHECK(VoxelSlots_AtlasWidth(96, SLOT_DIM) == 48);   /* S.S. Tidal */
    CHECK(VoxelSlots_AtlasWidth(88, SLOT_DIM) == 44);   /* submarine shadow */
    CHECK(VoxelSlots_AtlasWidth(128, SLOT_DIM) == 64);
    CHECK(VoxelSlots_AtlasWidth(136, SLOT_DIM) == 0);
    CHECK(VoxelSlots_AtlasWidth(0, SLOT_DIM) == 0);
}

static void TestFits(void)
{
    /* Everything an OAM can be still fits, 8bpp included. */
    static const unsigned oam[][2] = {
        {8,8},{16,16},{32,32},{64,64},{16,8},{32,8},{32,16},{64,32},{8,16},{8,32},{16,32},{32,64}};

    for (unsigned i = 0; i < sizeof(oam) / sizeof(oam[0]); ++i)
    {
        CHECK(VoxelSlots_Fits(oam[i][0], oam[i][1], false, SLOT_DIM, MAX_BYTES));
        CHECK(VoxelSlots_Fits(oam[i][0], oam[i][1], true, SLOT_DIM, MAX_BYTES));
    }
    CHECK(VoxelSlots_Fits(96, 40, false, SLOT_DIM, MAX_BYTES));
    CHECK(VoxelSlots_Fits(88, 32, false, SLOT_DIM, MAX_BYTES));
    CHECK(VoxelSlots_Fits(48, 48, false, SLOT_DIM, MAX_BYTES));
    CHECK(VoxelSlots_Fits(128, 64, false, SLOT_DIM, MAX_BYTES));
    CHECK(!VoxelSlots_Fits(128, 64, true, SLOT_DIM, MAX_BYTES));   /* 8 KiB of tiles */
    CHECK(!VoxelSlots_Fits(96, 72, false, SLOT_DIM, MAX_BYTES));   /* too tall */
    CHECK(!VoxelSlots_Fits(136, 8, false, SLOT_DIM, MAX_BYTES));   /* too wide */
    CHECK(!VoxelSlots_Fits(12, 8, false, SLOT_DIM, MAX_BYTES));    /* not whole tiles */
}

/* Every picture accepted fits the slot it is given: at most 64 texels across
 * and down, its tiles in the 4 KiB source buffer, every column it samples
 * inside the picture; and its world width is its own (the caller's halfW
 * comes from the picture's width, the atlas's u range from AtlasWidth). */
static void TestFitsInvariants(void)
{
    unsigned accepted = 0;

    for (unsigned w = 8; w <= 256; w += 8)
        for (unsigned h = 8; h <= 128; h += 8)
            for (int c = 0; c < 2; ++c)
            {
                bool color256 = c != 0;
                unsigned aw, bytes = w * h / (color256 ? 1u : 2u);

                if (!VoxelSlots_Fits(w, h, color256, SLOT_DIM, MAX_BYTES))
                    continue;
                ++accepted;
                aw = VoxelSlots_AtlasWidth(w, SLOT_DIM);
                CHECK(aw > 0 && aw <= SLOT_DIM);
                CHECK(h <= SLOT_DIM);
                CHECK(bytes <= MAX_BYTES);
                CHECK(w <= SLOT_DIM ? aw == w : aw * 2 == w);
                CHECK(VoxelSlots_SourceColumn(aw - 1, aw, w, false) < w);
                CHECK(VoxelSlots_SourceColumn(0, aw, w, true) < w);
            }
    CHECK(accepted > 0);
}

/* A synthetic 4bpp picture in rows of tiles, colour of (x, y) given. */
static unsigned Pattern(unsigned x, unsigned y)
{
    return ((x * 7u) ^ (y * 3u)) & 0xFu;
}

static void Encode4bpp(uint8_t *tiles, unsigned width, unsigned height)
{
    memset(tiles, 0, width * height / 2u);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            unsigned tile = (y / 8u) * (width / 8u) + x / 8u;
            uint8_t *byte = &tiles[tile * 32u + (y & 7u) * 4u + (x & 7u) / 2u];

            *byte |= (uint8_t)(Pattern(x, y) << ((x & 1u) * 4u));
        }
}

static void TestTilePixel(void)
{
    static uint8_t tiles[96 * 40 / 2];
    static uint8_t tiles8[16 * 16];

    Encode4bpp(tiles, 96, 40);
    for (unsigned y = 0; y < 40; ++y)
        for (unsigned x = 0; x < 96; ++x)
            CHECK(VoxelSlots_TilePixel(tiles, 96, x, y, false) == Pattern(x, y));

    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            tiles8[((y / 8) * 2 + x / 8) * 64 + (y & 7) * 8 + (x & 7)] = (uint8_t)(y * 16 + x);
    CHECK(VoxelSlots_TilePixel(tiles8, 16, 0, 0, true) == 0);
    CHECK(VoxelSlots_TilePixel(tiles8, 16, 9, 3, true) == 3 * 16 + 9);
    CHECK(VoxelSlots_TilePixel(tiles8, 16, 15, 15, true) == 255);
}

/* The S.S. Tidal's 96x40 picture into a 48-texel cell: every second column,
 * from the picture's left edge to its right, not an 8x8 corner (#72). */
static void TestWideSampling(void)
{
    static uint8_t tiles[96 * 40 / 2];
    unsigned aw = VoxelSlots_AtlasWidth(96, SLOT_DIM);
    unsigned covered = 0;
    bool seen[96] = {false};

    Encode4bpp(tiles, 96, 40);
    CHECK(aw == 48);
    for (unsigned ax = 0; ax < aw; ++ax)
    {
        unsigned sx = VoxelSlots_SourceColumn(ax, aw, 96, false);

        CHECK(sx == 2 * ax);
        CHECK(sx < 96);
        seen[sx] = true;
        for (unsigned y = 0; y < 40; ++y)
            CHECK(VoxelSlots_TilePixel(tiles, 96, sx, y, false) == Pattern(2 * ax, y));
    }
    for (unsigned x = 0; x < 96; ++x)
        covered += seen[x];
    CHECK(covered == 48);
    CHECK(seen[0] && seen[94]);           /* both ends of the hull */

    /* Mirrored: the cell's left column shows the picture's right. */
    CHECK(VoxelSlots_SourceColumn(0, aw, 96, true) == 94);
    CHECK(VoxelSlots_SourceColumn(47, aw, 96, true) == 0);
    /* 88 wide: 44 texels reaching column 86. */
    CHECK(VoxelSlots_SourceColumn(43, 44, 88, false) == 86);
    /* A picture that fits is copied column for column. */
    for (unsigned ax = 0; ax < 32; ++ax)
    {
        CHECK(VoxelSlots_SourceColumn(ax, 32, 32, false) == ax);
        CHECK(VoxelSlots_SourceColumn(ax, 32, 32, true) == 31 - ax);
    }
}

/* ── Assigning ───────────────────────────────────────────────────────────── */

static void ObjectsClaim(bool *claimed)
{
    memset(claimed, 0, SLOTS * sizeof(bool));
    for (unsigned s = 0; s < OBJECTS; ++s)
        claimed[s] = true;
}

static VoxelSlotRequest Request(unsigned id, unsigned priority, bool inRange)
{
    VoxelSlotRequest r = {(uint8_t)id, (uint8_t)priority, inRange, -1};

    return r;
}

/* Twenty rustling grass tiles come first in sprite order, two wild Pokemon
 * after: the wild ones still get slots, and keep them frame after frame. */
static void TestWildBeforeGrass(void)
{
    uint8_t owner[SLOTS] = {0};
    bool claimed[SLOTS], reset[SLOTS];
    VoxelSlotRequest req[22];
    int wildSlot[2];
    unsigned dropped, used = 0;

    for (unsigned i = 0; i < 20; ++i)
        req[i] = Request(i, VOXEL_SLOT_PRIORITY_DECOR, true);
    req[20] = Request(40, VOXEL_SLOT_PRIORITY_WILD, true);
    req[21] = Request(41, VOXEL_SLOT_PRIORITY_WILD, true);

    ObjectsClaim(claimed);
    memset(reset, 0, sizeof(reset));
    dropped = VoxelSlots_Assign(req, 22, owner, claimed, reset, SLOTS, OBJECTS);
    CHECK(dropped == 6);
    CHECK(req[20].slot >= (int)OBJECTS && req[21].slot >= (int)OBJECTS);
    CHECK(req[20].slot != req[21].slot);
    for (unsigned i = 0; i < 22; ++i)
        used += req[i].slot >= 0;
    CHECK(used == 16);
    /* The grass left out is the last in order. */
    for (unsigned i = 14; i < 20; ++i)
        CHECK(req[i].slot < 0);
    for (unsigned s = 0; s < SLOTS; ++s)
        CHECK(claimed[s]);
    wildSlot[0] = req[20].slot;
    wildSlot[1] = req[21].slot;
    CHECK(owner[wildSlot[0]] == 41 && owner[wildSlot[1]] == 42);

    /* The next frame, the same sprites: the same slots, nothing to decode. */
    ObjectsClaim(claimed);
    memset(reset, 0, sizeof(reset));
    dropped = VoxelSlots_Assign(req, 22, owner, claimed, reset, SLOTS, OBJECTS);
    CHECK(dropped == 6);
    CHECK(req[20].slot == wildSlot[0] && req[21].slot == wildSlot[1]);
    for (unsigned s = 0; s < SLOTS; ++s)
        CHECK(!reset[s]);
}

/* Every free slot held by last frame's grass: a wild Pokemon that appears
 * takes one at once, from the grass last in order, and no other grass moves. */
static void TestWildDisplacesGrass(void)
{
    uint8_t owner[SLOTS] = {0};
    bool claimed[SLOTS], reset[SLOTS];
    VoxelSlotRequest req[17];
    int before[16];

    for (unsigned i = 0; i < 16; ++i)
        req[i] = Request(i, VOXEL_SLOT_PRIORITY_DECOR, true);
    ObjectsClaim(claimed);
    memset(reset, 0, sizeof(reset));
    CHECK(VoxelSlots_Assign(req, 16, owner, claimed, reset, SLOTS, OBJECTS) == 0);
    for (unsigned i = 0; i < 16; ++i)
        before[i] = req[i].slot;

    req[16] = Request(50, VOXEL_SLOT_PRIORITY_WILD, true);
    ObjectsClaim(claimed);
    memset(reset, 0, sizeof(reset));
    CHECK(VoxelSlots_Assign(req, 17, owner, claimed, reset, SLOTS, OBJECTS) == 1);
    CHECK(req[16].slot == before[15]);
    CHECK(req[15].slot < 0);
    CHECK(owner[before[15]] == 51);
    CHECK(reset[before[15]]);
    for (unsigned i = 0; i < 15; ++i)
    {
        CHECK(req[i].slot == before[i]);
        CHECK(!reset[before[i]]);
    }
}

/* Out of sight: no slot, and none taken from one in sight; the slot it had
 * goes back. */
static void TestOutOfRange(void)
{
    uint8_t owner[SLOTS] = {0};
    bool claimed[SLOTS], reset[SLOTS];
    VoxelSlotRequest req[17];
    int farSlot;

    req[0] = Request(30, VOXEL_SLOT_PRIORITY_WILD, true);
    ObjectsClaim(claimed);
    memset(reset, 0, sizeof(reset));
    VoxelSlots_Assign(req, 1, owner, claimed, reset, SLOTS, OBJECTS);
    farSlot = req[0].slot;
    CHECK(farSlot >= (int)OBJECTS);

    /* It wanders off; sixteen grass tiles in sight fill every slot. */
    req[0].inRange = false;
    for (unsigned i = 1; i < 17; ++i)
        req[i] = Request(i, VOXEL_SLOT_PRIORITY_DECOR, true);
    ObjectsClaim(claimed);
    memset(reset, 0, sizeof(reset));
    CHECK(VoxelSlots_Assign(req, 17, owner, claimed, reset, SLOTS, OBJECTS) == 0);
    CHECK(req[0].slot < 0);
    for (unsigned i = 1; i < 17; ++i)
        CHECK(req[i].slot >= 0);
    CHECK(owner[farSlot] != 31);
    CHECK(reset[farSlot]);
}

/* A field effect before grass, ties in order; free slots past the objects'
 * first, then an unused object's. */
static void TestPriorityAndPlacement(void)
{
    uint8_t owner[SLOTS] = {0};
    bool claimed[SLOTS], reset[SLOTS];
    VoxelSlotRequest req[3];

    /* Only two slots free: 20 and 3 (an object not in use). */
    for (unsigned s = 0; s < SLOTS; ++s)
        claimed[s] = s != 20 && s != 3;
    memset(reset, 0, sizeof(reset));
    req[0] = Request(5, VOXEL_SLOT_PRIORITY_DECOR, true);
    req[1] = Request(6, VOXEL_SLOT_PRIORITY_EFFECT, true);
    req[2] = Request(7, VOXEL_SLOT_PRIORITY_EFFECT, true);
    CHECK(VoxelSlots_Assign(req, 3, owner, claimed, reset, SLOTS, OBJECTS) == 1);
    CHECK(req[0].slot < 0);
    CHECK(req[1].slot == 20);
    CHECK(req[2].slot == 3);
    CHECK(claimed[20] && claimed[3]);
}

/* ── A card before a model's face ────────────────────────────────────────── */

#define CLEAR (1.0f / 16.0f)
#define FACE_MAX 0.5f

static bool Near(float a, float b)
{
    float d = a - b;

    return d < 1e-5f && d > -1e-5f;
}

static void TestModelFacePush(void)
{
    float push;

    /* The seated walker: feet at 5.5, the chair's front on the cell's south
     * edge at 6.0. The face is half a tile on; it gets its pixel past. */
    push = -1.0f;
    CHECK(VoxelSlots_ModelFacePush(6.0f, 5.5f, CLEAR, FACE_MAX, &push));
    CHECK(Near(push, 0.5625f));

    /* What was pushed before still is, by as much: a PC two pixels past
     * the cell's middle, furniture whose face is the middle itself. */
    CHECK(VoxelSlots_ModelFacePush(5.625f, 5.5f, CLEAR, FACE_MAX, &push));
    CHECK(Near(push, 0.1875f));
    CHECK(VoxelSlots_ModelFacePush(5.5f, 5.5f, CLEAR, FACE_MAX, &push));
    CHECK(Near(push, CLEAR));
    /* A face a little behind the feet, closer than the clearance. */
    CHECK(VoxelSlots_ModelFacePush(5.47f, 5.5f, CLEAR, FACE_MAX, &push));
    CHECK(Near(push, 0.0325f));
    /* Walking south through the cell, the edge close ahead. */
    CHECK(VoxelSlots_ModelFacePush(6.0f, 5.9f, CLEAR, FACE_MAX, &push));
    CHECK(Near(push, 0.1625f));

    /* Already in front: nothing, and `push` left alone. */
    push = 123.0f;
    CHECK(!VoxelSlots_ModelFacePush(5.4f, 5.5f, CLEAR, FACE_MAX, &push));
    CHECK(!VoxelSlots_ModelFacePush(5.5f - CLEAR, 5.5f, CLEAR, FACE_MAX, &push));
    CHECK(!VoxelSlots_ModelFacePush(3.0f, 5.5f, CLEAR, FACE_MAX, &push));
    CHECK(push == 123.0f);

    /* Past the walker's own cell - a wall, a model further south - never
     * carries the card across. */
    CHECK(!VoxelSlots_ModelFacePush(6.0625f, 5.5f, CLEAR, FACE_MAX, &push));
    CHECK(!VoxelSlots_ModelFacePush(7.0f, 5.5f, CLEAR, FACE_MAX, &push));
    CHECK(!VoxelSlots_ModelFacePush(6.6f, 5.9f, CLEAR, FACE_MAX, &push));
    CHECK(push == 123.0f);

    /* Across every face from a tile behind to two ahead: a push only for a
     * face within half a tile on and not a clearance behind, always just
     * clearing it, never more than half a tile and a pixel. */
    for (int i = -64; i <= 128; ++i)
    {
        float front = 5.5f + i / 64.0f;
        bool expect = front + CLEAR > 5.5f && front - 5.5f <= FACE_MAX;
        bool got;

        push = 0.0f;
        got = VoxelSlots_ModelFacePush(front, 5.5f, CLEAR, FACE_MAX, &push);
        CHECK(got == expect);
        if (!got)
            continue;
        CHECK(push > 0.0f);
        CHECK(push <= FACE_MAX + CLEAR + 1e-5f);
        CHECK(Near(5.5f + push, front + CLEAR));
    }
}

/* ── Doors ───────────────────────────────────────────────────────────────── */

/* A door's frame as field_door.c copies it to VRAM: tile t filled with colour
 * t % 16, so the pixel names the tile it came from. */
static void TestDoorPixel(void)
{
    static uint8_t tiles[16 * 32];
    static const uint8_t palettes[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    unsigned palette = 0;

    for (unsigned t = 0; t < 16; ++t)
        memset(tiles + t * 32, (int)((t % 16) * 0x11), 32);
    /* One column: the top metatile's TL TR BL BR, then the bottom one's. */
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 0, 0, &palette) == 0 && palette == 10);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 8, 0, &palette) == 1 && palette == 11);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 7, 8, &palette) == 2 && palette == 12);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 15, 15, &palette) == 3 && palette == 13);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 0, 16, &palette) == 4 && palette == 14);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 8, 24, &palette) == 7 && palette == 17);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 15, 31, &palette) == 7 && palette == 17);
    /* A door two cells wide: the second column's 8 tiles, the same palettes. */
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 16, 0, &palette) == 8 && palette == 10);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 24, 16, &palette) == 13 && palette == 15);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 31, 31, &palette) == 15 && palette == 17);

    /* 4bpp: the left pixel of a pair in the low nibble. */
    memset(tiles, 0, sizeof(tiles));
    tiles[5 * 32 + 3 * 4 + 2] = 0x9A;   /* tile 5, row 3, pixels 4 and 5 */
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 12, 19, &palette) == 0xA && palette == 15);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 13, 19, &palette) == 0x9);
    CHECK(VoxelSlots_DoorPixel(tiles, palettes, 14, 19, &palette) == 0);
}

/* Every row lands where the GBA's projection put it, (u, h, v + h): up the
 * wall north of it, on the ground south of it, the two meeting at its foot. */
static void TestDoorRow(void)
{
    float h, z;

    VoxelSlots_DoorRow(8.0f, 10.0f, &h, &z);
    CHECK(h == 2.0f && z == 10.0f);
    VoxelSlots_DoorRow(9.25f, 10.0f, &h, &z);
    CHECK(h == 0.75f && z == 10.0f);
    VoxelSlots_DoorRow(10.0f, 10.0f, &h, &z);
    CHECK(h == 0.0f && z == 10.0f);
    VoxelSlots_DoorRow(10.5f, 10.0f, &h, &z);
    CHECK(h == 0.0f && z == 10.5f);
    for (float v = 6.0f; v <= 12.0f; v += 0.125f)
    {
        VoxelSlots_DoorRow(v, 9.5f, &h, &z);
        CHECK(h >= 0.0f && z >= 9.5f && z - h == v);
    }
}

int main(void)
{
    TestAtlasWidth();
    TestFits();
    TestFitsInvariants();
    TestModelFacePush();
    TestTilePixel();
    TestWideSampling();
    TestWildBeforeGrass();
    TestWildDisplacesGrass();
    TestOutOfRange();
    TestPriorityAndPlacement();
    TestDoorPixel();
    TestDoorRow();
    if (sFailures)
    {
        fprintf(stderr, "voxel_sprite_slots_test: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("voxel_sprite_slots_test: ok\n");
    return 0;
}
