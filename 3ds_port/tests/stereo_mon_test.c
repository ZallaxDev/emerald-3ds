/* Host test of the stereoscopic viewer's file decoding and composition. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "3ds_stereo_mon.h"
#include "3ds_video.h"

static int sFailures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++sFailures; } } while (0)

static void Put16(uint8_t *p, unsigned v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static void Put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 255; }

/* A file of two Pokémon, 4 planes, 2 frames: #384's plane is x / 16 in frame
 * 0 and y / 16 in frame 1, with pixel (0, 0) transparent; #150 is all plane 3. */
static uint8_t *MakeFile(uint32_t *size)
{
    const unsigned entryBytes = 2 * 64 * 64 / 2;
    uint32_t total = 16 + 2 * 8 + 2 * entryBytes;
    uint8_t *f = calloc(total, 1);

    memcpy(f, "EM3DSTM1", 8);
    Put16(f + 8, 2); f[10] = 4; f[11] = 2; Put16(f + 12, 64); Put16(f + 14, 64);
    Put16(f + 16, 384); Put32(f + 20, 32);
    Put16(f + 24, 150); Put32(f + 28, 32 + entryBytes);
    for (unsigned frame = 0; frame < 2; ++frame)
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 64; ++x)
            {
                unsigned i = frame * 4096 + y * 64 + x;
                unsigned v = frame == 0 ? x / 16 : y / 16;

                if (x == 0 && y == 0) v = 15;
                f[32 + (i >> 1)] |= v << ((i & 1) * 4);
                f[32 + entryBytes + (i >> 1)] |= 3u << ((i & 1) * 4);
            }
    *size = total;
    return f;
}

static int ReadFileTest(const char *path)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *data;
    long size;
    CtrStereoMonHeader h;
    unsigned shown = 0;

    if (!fp) { printf("cannot open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END); size = ftell(fp); fseek(fp, 0, SEEK_SET);
    data = malloc(size);
    if (fread(data, 1, size, fp) != (size_t)size) { printf("short read\n"); return 1; }
    fclose(fp);
    CHECK(CtrStereoMon_ParseHeader(data, (uint32_t)size, &h));
    printf("%s: %u pictures, %u planes, up to %u frames\n", path, h.count, h.planes, h.frames);
    for (unsigned i = 0; i < h.count; ++i)
    {
        uint16_t national;
        const uint8_t *planes;
        unsigned used[CTR_STEREO_MON_MAX_PLANES + 1] = {0}, outline = 0;
        CtrStereoMonHeader eh = h;

        CHECK(CtrStereoMon_Entry(data, (uint32_t)size, &h, i, &national, &planes));
        CHECK(CtrStereoMon_Find(data, (uint32_t)size, &h, national) == (int)i);
        /* The entry's own frames (a backs file's entries have their own). */
        eh.frames = CtrStereoMon_EntryFrames(data, (uint32_t)size, &h, i);
        /* A pack carries depth only: every pixel of every frame has a plane
         * and none is transparent (15). There is no outline in the file;
         * the game's own picture says which pixels are drawn. */
        for (unsigned frame = 0; frame < eh.frames; ++frame)
            for (unsigned y = 0; y < 64; ++y)
                for (unsigned x = 0; x < 64; ++x)
                {
                    unsigned p = CtrStereoMon_Plane(planes, &eh, frame, x, y);

                    ++used[p];
                    outline += p == CTR_STEREO_MON_NONE;
                }
        CHECK(outline == 0);
        unsigned distinct = 0;
        for (unsigned p = 0; p < h.planes; ++p) distinct += used[p] != 0;
        CHECK(distinct >= 1);
        if (outline != 0 && shown < 3) printf("  #%04x: %u transparent pixels\n", national, outline);
        if (shown++ < 3) printf("  #%04x: %u frames, %u planes in use\n", national, eh.frames, distinct);
    }
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t size;
    uint8_t *f = MakeFile(&size);
    CtrStereoMonHeader h;
    uint16_t national;
    const uint8_t *planes;
    static uint16_t picture[64 * 64], texture[256 * 256];

    CHECK(CtrStereoMon_ParseHeader(f, size, &h));
    CHECK(h.count == 2 && h.planes == 4 && h.frames == 2 && h.entryBytes == 4096);
    CHECK(!CtrStereoMon_ParseHeader(f, 15, NULL));
    CHECK(!CtrStereoMon_ParseHeader(f, 20, NULL));
    f[0] = 'X';
    CHECK(!CtrStereoMon_ParseHeader(f, size, NULL));
    f[0] = 'E';
    f[10] = 1;
    CHECK(!CtrStereoMon_ParseHeader(f, size, NULL));
    f[10] = 4;

    CHECK(CtrStereoMon_Find(f, size, &h, 384) == 0);
    /* The index alone, as the viewer keeps it: entries are found, planes are not. */
    {
        uint32_t offset = 0;
        CHECK(CtrStereoMon_Find(f, 32, &h, 150) == 1);
        CHECK(CtrStereoMon_IndexEntry(f, 32, &h, 1, &national, &offset) && national == 150 && offset == 32 + 4096);
        CHECK(!CtrStereoMon_IndexEntry(f, 24, &h, 1, NULL, NULL));
        CHECK(!CtrStereoMon_Entry(f, 32, &h, 1, NULL, NULL));
    }
    CHECK(CtrStereoMon_Find(f, size, &h, 150) == 1);
    CHECK(CtrStereoMon_Find(f, size, &h, 151) == -1);
    CHECK(!CtrStereoMon_Entry(f, size, &h, 2, NULL, NULL));
    CHECK(CtrStereoMon_Entry(f, size, &h, 0, &national, &planes));
    CHECK(national == 384);
    CHECK(CtrStereoMon_Plane(planes, &h, 0, 0, 0) == CTR_STEREO_MON_NONE);
    CHECK(CtrStereoMon_Plane(planes, &h, 0, 17, 5) == 1);
    CHECK(CtrStereoMon_Plane(planes, &h, 0, 63, 63) == 3);
    CHECK(CtrStereoMon_Plane(planes, &h, 1, 63, 5) == 0);
    CHECK(CtrStereoMon_Plane(planes, &h, 1, 5, 40) == 2);
    /* A third frame repeats the last one. */
    CHECK(CtrStereoMon_Plane(planes, &h, 2, 5, 40) == 2);
    CHECK(CtrStereoMon_Plane(planes, &h, 0, 64, 0) == CTR_STEREO_MON_NONE);
    /* An entry past the end of the file is refused. */
    CHECK(!CtrStereoMon_Entry(f, size - 1, &h, 1, NULL, NULL));
    /* Frames of an entry's own: the index's reserved word, else the file's. */
    CHECK(CtrStereoMon_EntryFrames(f, size, &h, 0) == 2);
    Put16(f + 16 + 2, 1);
    CHECK(CtrStereoMon_EntryFrames(f, size, &h, 0) == 1 && CtrStereoMon_FramesBytes(&h, 1) == 2048);
    Put16(f + 16 + 2, 9);
    CHECK(CtrStereoMon_EntryFrames(f, size, &h, 0) == 2);
    Put16(f + 16 + 2, 0);

    /* The picture's own transparent pixel (3, 20) lies on plane 1: the
     * planes have no outline, the picture's 0 is what stays transparent. */
    for (unsigned i = 0; i < 64 * 64; ++i) picture[i] = (uint16_t)(0x8000 | i | 1);
    picture[3 * 64 + 20] = 0;
    memset(texture, 0xaa, sizeof(texture));
    CtrStereoMon_ComposeTile(picture, planes, &h, 0, 4, 1, texture, 256, 64, 128, 0);
    /* Plane 1 is columns 16..31 of frame 0. */
    CHECK(texture[CtrVideo_Texel(64 + 20, 128 + 7, 256)] == picture[7 * 64 + 20]);
    CHECK(texture[CtrVideo_Texel(64 + 20, 128 + 3, 256)] == 0);
    CHECK(texture[CtrVideo_Texel(64 + 15, 128 + 7, 256)] == 0);
    CHECK(texture[CtrVideo_Texel(64 + 32, 128 + 7, 256)] == 0);
    CHECK(texture[CtrVideo_Texel(64 + 31, 128 + 63, 256)] == picture[63 * 64 + 31]);
    /* Two groups of the four planes: group 1 is planes 2 and 3, columns 32..63. */
    CHECK(CtrStereoMon_Group(0, 4, 2) == 0 && CtrStereoMon_Group(1, 4, 2) == 0);
    CHECK(CtrStereoMon_Group(2, 4, 2) == 1 && CtrStereoMon_Group(3, 4, 2) == 1);
    CHECK(CtrStereoMon_Group(9, 10, 3) == 2 && CtrStereoMon_Group(0, 10, 3) == 0 && CtrStereoMon_Group(4, 10, 3) == 1);
    CHECK(CtrStereoMon_Group(CTR_STEREO_MON_NONE, 4, 2) == CTR_STEREO_MON_NONE);
    memset(texture, 0xaa, sizeof(texture));
    CtrStereoMon_ComposeTile(picture, planes, &h, 0, 2, 1, texture, 256, 0, 0, 0);
    CHECK(texture[CtrVideo_Texel(32, 7, 256)] == picture[7 * 64 + 32]);
    CHECK(texture[CtrVideo_Texel(63, 7, 256)] == picture[7 * 64 + 63]);
    CHECK(texture[CtrVideo_Texel(31, 7, 256)] == 0);
    memset(texture, 0xaa, sizeof(texture));
    CtrStereoMon_ComposeTile(picture, planes, &h, 0, 4, 1, texture, 256, 64, 128, 0);
    /* Nothing outside the tile is touched. */
    CHECK(texture[CtrVideo_Texel(0, 0, 256)] == 0xaaaa);
    CHECK(texture[CtrVideo_Texel(128, 128, 256)] == 0xaaaa);
    CHECK(texture[CtrVideo_Texel(70, 127, 256)] == 0xaaaa);
    /* A skirt of 2: plane 1's card goes on under the nearer plane 2's
     * columns 32 and 33 with the colours of its own nearest pixels (column
     * 31), not under column 34 (too far) nor under the farther plane 0's
     * column 15; a transparent pixel stays transparent. */
    memset(texture, 0xaa, sizeof(texture));
    CtrStereoMon_ComposeTile(picture, planes, &h, 0, 4, 1, texture, 256, 0, 0, 2);
    CHECK(texture[CtrVideo_Texel(32, 7, 256)] == picture[7 * 64 + 31]);
    CHECK(texture[CtrVideo_Texel(33, 7, 256)] == picture[7 * 64 + 31]);
    CHECK(texture[CtrVideo_Texel(34, 7, 256)] == 0);
    CHECK(texture[CtrVideo_Texel(15, 7, 256)] == 0);
    CHECK(texture[CtrVideo_Texel(20, 3, 256)] == 0);
    CHECK(texture[CtrVideo_Texel(0, 0, 256)] == 0);

    /*
     * Full no-parallax reconstruction, synthetic front sprites in both
     * animation frames. Compositing the far-to-near depth cards at exactly
     * the same pixel coordinates must yield every original RGBA5551 pixel
     * byte-for-byte. This includes arbitrary palette colors, holes in the
     * silhouette and the 2px skirts that conceal gaps between cards.
     * It guards faded/tinted/shiny palettes and transparency without a ROM.
     */
    {
        static uint16_t reconstructed[64 * 64];
        static uint16_t tinted[64 * 64];
        const unsigned groupCounts[] = {2, 3, 4};
        for (unsigned frame = 0; frame < 2; ++frame)
            for (unsigned tint = 0; tint < 2; ++tint)
            {
                for (unsigned y = 0; y < 64; ++y)
                    for (unsigned x = 0; x < 64; ++x)
                    {
                        unsigned i = y * 64 + x;
                        unsigned colour = tint ? ((i * 31 + 7) ^ 0x4d21u)
                                               : (i * 71 + x * 17 + y * 3);
                        /* RGB5551: alpha is the top bit in the port's
                         * texture convention; zero is fully transparent. */
                        tinted[i] = ((x == 0 && y == 0) || (x + y * 3 + tint * 7) % 11 == 0)
                                     ? 0 : (uint16_t)(0x8000u | (colour & 0x7fffu));
                    }
                for (unsigned ci = 0; ci < sizeof(groupCounts) / sizeof(groupCounts[0]); ++ci)
                {
                    unsigned groups = groupCounts[ci];

                    memset(texture, 0, sizeof(texture));
                    memset(reconstructed, 0, sizeof(reconstructed));
                    for (unsigned g = 0; g < groups; ++g)
                    {
                        /* Unique 64px tile per plane; no card overwrites
                         * another before final composition. */
                        CtrStereoMon_ComposeTile(tinted, planes, &h, frame,
                                                 groups, g, texture, 256,
                                                 g * 64, 0, 2);
                    }
                    for (unsigned g = 0; g < groups; ++g)
                        for (unsigned y = 0; y < 64; ++y)
                            for (unsigned x = 0; x < 64; ++x)
                            {
                                uint16_t v = texture[CtrVideo_Texel(g * 64 + x, y, 256)];
                                if (v != 0) reconstructed[y * 64 + x] = v;
                            }
                    CHECK(memcmp(reconstructed, tinted, sizeof(reconstructed)) == 0);
                }
            }
    }
    free(f);

    for (int a = 1; a < argc; ++a)
        if (ReadFileTest(argv[a]))
            ++sFailures;
    printf(sFailures ? "stereo_mon_test: %d FAILED\n" : "stereo_mon_test: PASS\n", sFailures);
    return sFailures != 0;
}
