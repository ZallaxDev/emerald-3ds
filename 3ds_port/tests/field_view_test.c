/* Host test for the part of the overworld on screen (src/3ds_field_view.c). */
#include "3ds_field_view.h"
#include <stdio.h>

static int sFailures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++sFailures; } } while (0)

static const int sPitches[] = {34, 37, 40, 43, 46};
static const int sZooms[] = {90, 100, 110, 120};

int main(void)
{
    CtrFieldView view;

    CtrFieldView_Flat(&view);
    /* The player's own tile and the corners of the screen are in view; a
     * row past the top edge and two past the bottom are not. */
    CHECK(CtrFieldView_Outside(&view, 0, 0) == 0.0f);
    CHECK(CtrFieldView_Outside(&view, -12, -6) == 0.0f);
    CHECK(CtrFieldView_Outside(&view, 12, 8) == 0.0f);
    CHECK(CtrFieldView_Outside(&view, 0, -7) > 0.0f);
    CHECK(CtrFieldView_Outside(&view, 0, 10) > 0.0f);
    CHECK(CtrFieldView_Outside(&view, 14, 0) > 0.0f);

    /* The default camera (40 degrees, 100%): it used to be thought to see
     * 14 rows north; the far edge is wider than the near one. */
    CtrFieldView_Voxel(&view, 40.0f, 100.0f);
    printf("voxel 40/100: north %.1f south %.1f far %.1f near %.1f\n",
           view.north, view.south, view.farHalfWidth, view.nearHalfWidth);
    CHECK(view.north >= 13.0f && view.north <= 18.0f);
    CHECK(view.south >= 5.0f && view.south <= 9.0f);
    CHECK(view.farHalfWidth > view.nearHalfWidth);
    CHECK(CtrFieldView_Outside(&view, 0, -13) == 0.0f);
    CHECK(CtrFieldView_Outside(&view, 12, -13) == 0.0f);
    /* At the player's feet the picture is narrow. */
    CHECK(CtrFieldView_Outside(&view, 12, 5) > 0.0f);

    /* Every option: flatter or further out sees further north, and nothing
     * goes past what the voxel world draws the cards for. */
    for (unsigned p = 0; p < sizeof(sPitches) / sizeof(sPitches[0]); ++p)
        for (unsigned z = 0; z < sizeof(sZooms) / sizeof(sZooms[0]); ++z)
        {
            CtrFieldView a, b;

            CtrFieldView_Voxel(&a, (float)sPitches[p], (float)sZooms[z]);
            printf("voxel %d/%d: north %.1f south %.1f far %.1f near %.1f\n", sPitches[p], sZooms[z],
                   a.north, a.south, a.farHalfWidth, a.nearHalfWidth);
            CHECK(a.north > 9.0f && a.north <= 22.0f);
            CHECK(a.south > 3.0f && a.south <= 22.0f);
            CHECK(a.farHalfWidth <= 22.0f && a.nearHalfWidth >= 1.0f);
            if (p > 0)
            {
                CtrFieldView_Voxel(&b, (float)sPitches[p - 1], (float)sZooms[z]);
                CHECK(b.north >= a.north);
            }
            if (z > 0)
            {
                CtrFieldView_Voxel(&b, (float)sPitches[p], (float)sZooms[z - 1]);
                CHECK(b.north >= a.north);
            }
        }

    /* Further outside is a larger number, whichever way. */
    CtrFieldView_Voxel(&view, 40.0f, 100.0f);
    CHECK(CtrFieldView_Outside(&view, 0, -30) > CtrFieldView_Outside(&view, 0, -20));
    CHECK(CtrFieldView_Outside(&view, 30, -5) > CtrFieldView_Outside(&view, 20, -5));

    /* The camera stopped at an edge: the view, and the window round it, move
     * with it. */
    {
        int west, east, north, south;

        CtrFieldView_Flat(&view);
        view.shiftX = 5;
        view.shiftY = -3;
        CHECK(CtrFieldView_Outside(&view, 17, 0) == 0.0f);
        CHECK(CtrFieldView_Outside(&view, -12, 0) > 0.0f);
        CHECK(CtrFieldView_Outside(&view, 0, -9) == 0.0f);
        CHECK(CtrFieldView_Outside(&view, 0, 8) > 0.0f);
        CtrFieldView_Window(&view, 2, &west, &east, &north, &south);
        CHECK(west == 10 && east == 20 && north == 11 && south == 8);
    }

    /* The edge of the map: a 400px view over a map from 112 to 432 (20
     * metatiles). Closed at both ends it is centred; at one, it stops there;
     * open, it goes where the camera does. */
    CHECK(CtrFieldView_Limit(0.0f, 0.0f, 400.0f, 112.0f, 432.0f, CTR_FIELD_EDGE_LOW | CTR_FIELD_EDGE_HIGH) == 72.0f);
    CHECK(CtrFieldView_Limit(0.0f, 0.0f, 400.0f, 112.0f, 432.0f, CTR_FIELD_EDGE_LOW) == 112.0f);
    CHECK(CtrFieldView_Limit(200.0f, 0.0f, 400.0f, 112.0f, 432.0f, CTR_FIELD_EDGE_LOW) == 200.0f);
    CHECK(CtrFieldView_Limit(200.0f, 0.0f, 400.0f, 112.0f, 432.0f, CTR_FIELD_EDGE_HIGH) == 32.0f);
    CHECK(CtrFieldView_Limit(-50.0f, 0.0f, 400.0f, 112.0f, 432.0f, 0) == -50.0f);
    /* A wide map: the view moves freely between its ends. */
    CHECK(CtrFieldView_Limit(500.0f, 0.0f, 400.0f, 112.0f, 1712.0f, 3) == 500.0f);
    CHECK(CtrFieldView_Limit(1400.0f, 0.0f, 400.0f, 112.0f, 1712.0f, 3) == 1312.0f);
    /* A view centred on its point, seeing further one way (the voxel camera,
     * north): centred on a small map, the point is off its middle. */
    CHECK(CtrFieldView_Limit(5.0f, 15.0f, 6.0f, 0.0f, 9.0f, 3) == 9.0f);
    CHECK(CtrFieldView_Limit(5.0f, 15.0f, 6.0f, 0.0f, 40.0f, 3) == 15.0f);
    CHECK(CtrFieldView_Limit(38.0f, 15.0f, 6.0f, 0.0f, 40.0f, 3) == 34.0f);

    if (sFailures == 0)
        printf("field view: all checks passed\n");
    return sFailures != 0;
}
