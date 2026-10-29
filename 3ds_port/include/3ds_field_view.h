#ifndef CTR_FIELD_VIEW_H
#define CTR_FIELD_VIEW_H

/*
 * The part of the overworld the top screen shows, in metatiles from the
 * player's own: which map objects have to exist (event_object_movement.c).
 *
 * The game has room for 16 of them, a size the save keeps. The original
 * spawns those within a fixed window around the camera; the 3DS screen is
 * wider than the GBA's, and the voxel camera looks north across many more
 * rows than it does south, as far as its pitch and zoom take it. A window
 * large enough for that holds more people than there is room for, and the
 * ones that came first kept the room: people standing on screen never came
 * into existence while others out of sight did. The view says how far the
 * window reaches and which of the people in it come first.
 *
 * The 2D field is a rectangle. The voxel view is a trapezoid over the ground,
 * narrow at the player's feet and wide at its far (northern) edge.
 *
 * Both are centred on the player, unless the camera has stopped at the edge of
 * the map (CtrFieldView_Limit): the view is then `shiftX` columns east and
 * `shiftY` rows south of where it would be, and so are the people it reaches.
 */
typedef struct
{
    float north;         /* rows seen up the screen from the player's */
    float south;         /* rows seen down it */
    float farHalfWidth;  /* columns either side at the northern edge */
    float nearHalfWidth; /* columns either side at the southern edge */
    int shiftX, shiftY;  /* the camera stopped at an edge, tiles east and south */
} CtrFieldView;

/* The 2D field: the whole top screen around the player. */
void CtrFieldView_Flat(CtrFieldView *view);
/* The voxel camera at this pitch (degrees) and zoom (percent). */
void CtrFieldView_Voxel(CtrFieldView *view, float pitchDegrees, float zoomPercent);
/* Tiles by which the tile (dx, dy) from the player's (east and south
 * positive) lies outside the view; 0 within it. */
float CtrFieldView_Outside(const CtrFieldView *view, int dx, int dy);
/* Which of the people at these tiles come first: smaller first. Tiles out of
 * sight count double, then the distance from the player. */
float CtrFieldView_Score(const CtrFieldView *view, int dx, int dy);
/* The rectangle around the view, whole metatiles from the player's, each side
 * `margin` more. */
void CtrFieldView_Window(const CtrFieldView *view, int margin, int *west, int *east, int *north, int *south);

/*
 * The edge of the map. The 3DS screen shows more of the map than the GBA's,
 * and past a map's edge there is only its border tiled over and over, or
 * nothing: where no other map joins it, the camera stops at that edge and the
 * player walks on alone.
 *
 * One axis, in any unit: the camera would put the view at `at`, from which it
 * reaches `before` towards `low` and `after` towards `high`, the two ends of
 * the map. `closed` says which of them has no map beyond it
 * (CTR_FIELD_EDGE_LOW, CTR_FIELD_EDGE_HIGH). Returns where the view goes
 * instead: as far as the closed ends let it, or in the middle of a map
 * smaller than the view, closed at both.
 */
#define CTR_FIELD_EDGE_LOW 1u
#define CTR_FIELD_EDGE_HIGH 2u
float CtrFieldView_Limit(float at, float before, float after, float low, float high, unsigned closed);
/* The four edges of a map as two of those pairs: west and east are the low
 * and high ends of the columns, north and south (shifted by 2) of the rows. */
#define CTR_FIELD_EDGE_WEST CTR_FIELD_EDGE_LOW
#define CTR_FIELD_EDGE_EAST CTR_FIELD_EDGE_HIGH
#define CTR_FIELD_EDGE_NORTH (CTR_FIELD_EDGE_LOW << 2)
#define CTR_FIELD_EDGE_SOUTH (CTR_FIELD_EDGE_HIGH << 2)

/* What the overworld shows with the player's current options, and a number
 * that changes whenever that does (device only: it reads the settings). */
const CtrFieldView *CtrFieldView_Current(void);
unsigned CtrFieldView_Generation(void);
/* Where each camera has stopped short of the player at an edge, tiles east
 * and south: the 2D field's (field_camera.c) and the voxel world's
 * (voxel_camera.c). The view of the option in use takes its own. */
void CtrFieldView_SetFlatShift(int x, int y);
void CtrFieldView_SetVoxelShift(int x, int y);

#endif
