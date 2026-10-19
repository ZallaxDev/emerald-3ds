/*
 * The free-roam camera of the voxel overworld: 1ST (first person) and 3RD
 * (third person), and the camera-relative walk that goes with them.
 *
 * Ported from the voxel mod's lib/FirstPerson.lua, lib/ThirdPerson.lua and
 * lib/FreeMove.lua (see NOTICE.md). The three modules are one rig there and
 * one here: the eye stands in the player's head and looks where the player
 * steers it (1ST); on 3RD the same eye is pulled back onto a boom behind the
 * player's shoulder, shortened by whatever the boom would pass through.
 * Everything is pure state and pure math - no GPU, no game types - so it is
 * unit-tested on the host (tests/voxel_freecam_test.c); voxel_camera.c feeds
 * it the player and the world and reads the placed camera back.
 *
 * Units are world tiles (the Lua's world pixels over 16) and radians, the
 * Lua's conventions throughout: yaw is the bearing the head looks along, 0
 * towards the south (+Z, the way a resting sprite faces) and pi/2 east;
 * pitch is below the horizon, positive looking down.
 *
 * FreeMove.lua walks the player continuously and slides it along walls. The
 * console's game keeps the grid (a step is the decompilation's own, with
 * its collision, ledges, warps and encounters), so what is ported of it is
 * the part that decides where the player WANTS to go: the stick is rotated
 * by the camera's yaw into a world direction (VoxelFreeCam_Steer), and a
 * standing player turns to face where the head looks, so that A talks to
 * what you are looking at (VoxelFreeCam_TurnTap).
 */
#ifndef CTR_VOXEL_FREECAM_H
#define CTR_VOXEL_FREECAM_H

#include <stdbool.h>
#include <stdint.h>

/* The rig's numbers, from FirstPerson.lua and ThirdPerson.lua. The Lua's
 * characters are 16 pixels tall and Emerald's are 32, so the lengths that
 * stand for the player's size (the eye's height, the boom) are scaled by
 * VOXEL_FC_BODY: the eye stays near the top of the sprite and the boom
 * frames the player at the same share of the screen. */
#define VOXEL_FC_BODY        1.75f
#define VOXEL_FC_EYE_HEIGHT  (13.0f / 16.0f * VOXEL_FC_BODY)   /* above the ground */
#define VOXEL_FC_FOV         60.0f  /* degrees, vertical (the Lua's 65 on a 16:9 window; the top screen is 5:3) */
#define VOXEL_FC_FOCUS_DIST  1.5f   /* the aim point's distance: near plane scale */
#define VOXEL_FC_PITCH_DOWN  (70.0f * 3.14159265358979f / 180.0f)
#define VOXEL_FC_PITCH_UP    (-50.0f * 3.14159265358979f / 180.0f)
#define VOXEL_FC_PITCH_REST  (10.0f * 3.14159265358979f / 180.0f)
#define VOXEL_FC_BLEND_TIME  0.45f  /* seconds: the dive between the orbit and the head */

/* Look input, from FirstPerson.lua: radians per second at full deflection,
 * with a squared response so small deflections aim and full ones turn. */
#define VOXEL_FC_STICK_YAW   3.5f
#define VOXEL_FC_STICK_PITCH 2.4f
#define VOXEL_FC_STICK_DEAD  0.18f
#define VOXEL_FC_MOVE_DEAD   0.25f

/* ThirdPerson.lua: the boom. */
#define VOXEL_FC_BOOM        (48.0f / 16.0f * VOXEL_FC_BODY)   /* tiles behind the pivot */
#define VOXEL_FC_PIVOT_LIFT  (4.0f / 16.0f * VOXEL_FC_BODY)
#define VOXEL_FC_SHOULDER    (4.0f / 16.0f * VOXEL_FC_BODY)
#define VOXEL_FC_BOOM_TIME   0.35f
#define VOXEL_FC_ZOOM_MIN    0.45f
#define VOXEL_FC_ZOOM_MAX    2.4f
#define VOXEL_FC_ZOOM_TIME   0.18f
#define VOXEL_FC_STEP        0.25f  /* collision march, tiles */
#define VOXEL_FC_REFINE      4
#define VOXEL_FC_PAD         0.35f  /* clearance kept off whatever stopped the boom (> the near plane) */
#define VOXEL_FC_CLEAR       1.25f  /* how high over a blocked cell the eye passes it */
#define VOXEL_FC_RETURN      9.375f /* tiles per second the boom may grow back */
/* The boom is shown (the player's card drawn) from this length. */
#define VOXEL_FC_SHOW_AT     (14.0f / 16.0f)

typedef struct
{
    float yaw, pitch;
    /* 0 on the orbit .. 1 in the head (VOXEL_FC_BLEND_TIME). */
    float blend;
    /* The boom: `out` 0 in the head, 1 fully out; `len` its length after
     * the world has had its say; `want` what it asked for. */
    float out, len, want;
    /* The player's zoom of the boom, eased (`zoom`) towards `zoomGoal`. */
    float zoom, zoomGoal;
    bool engagedBefore;
    /* The look input this frame: right stick (rates, -1..1) and the
     * shoulder buttons' turn (-1..1), held between samples. */
    float stickX, stickY, turn;
    /* Whether the player's own input drives the camera right now (the field
     * is on top and nothing owns the buttons). */
    bool driving;
} VoxelFreeCam;

/*
 * Whether the world refuses the eye at a point (world tiles, y up): the
 * ground, a building, a blocked cell. ThirdPerson.lua's `occupied`.
 */
typedef bool (*VoxelFreeCamBlocked)(void *context, float x, float y, float z);

void VoxelFreeCam_Init(VoxelFreeCam *fc);

/* A look delta, radians: every input funnels through here, so the clamps
 * live once (FirstPerson.lookBy). */
void VoxelFreeCam_LookBy(VoxelFreeCam *fc, float dyaw, float dpitch);

/* The look input of this frame: the right stick's axes and the shoulder
 * turn, each -1..1, +x to the right, +y up on the stick. */
void VoxelFreeCam_SetLook(VoxelFreeCam *fc, float stickX, float stickY, float turn);

/* ThirdPerson.stepZoom / scaleZoom: the boom's zoom, multiplied. */
void VoxelFreeCam_SetZoom(VoxelFreeCam *fc, float zoom);

/*
 * One frame's tick (FirstPerson.update and ThirdPerson.update): the blend,
 * the boom's slide and recovery, the look rates. `engaged` is 1ST or 3RD
 * selected, `third` 3RD, `facingYaw` the bearing the player faces now (the
 * head starts looking that way on entering), `ready` whether there is a
 * world to dive into.
 */
void VoxelFreeCam_Tick(VoxelFreeCam *fc, float dt, bool engaged, bool third,
                       float facingYaw, bool ready);

/* The unit look direction. */
void VoxelFreeCam_LookDir(const VoxelFreeCam *fc, float *lx, float *ly, float *lz);

/* The eased blend, 0 on the orbit and 1 in the head. */
float VoxelFreeCam_Eased(const VoxelFreeCam *fc);

/* The boom's eased extension: 0 in the head, 1 out. */
float VoxelFreeCam_Extension(const VoxelFreeCam *fc);

/* ThirdPerson.showsPlayer: the boom is far enough out that the player's own
 * card belongs in the frame. */
bool VoxelFreeCam_ShowsPlayer(const VoxelFreeCam *fc);

/* FirstPerson.hidePlayer: deep enough in the head that the card would fill
 * the lens from inside. */
bool VoxelFreeCam_HidePlayer(const VoxelFreeCam *fc);

/* The camera this frame as the rig places it, mixed with the orbit's by the
 * blend. All of it is what CameraMatrices needs. */
typedef struct
{
    float eye[3];
    float focus[3];
    float fov;      /* degrees */
    /* The distance from the eye to the plane of zero parallax, tiles. */
    float focusDistance;
    /* The pitch a card is sized for, degrees (voxel_entities.c). */
    float cardPitch;
    /* How far the cards have turned from the orbit's plane to face the eye. */
    float cardBlend;
    /* The eased blend itself, 0 on the orbit .. 1 in the head: what the
     * near plane, the haze and the view's reach are mixed by. */
    float eased;
} VoxelFreeCamView;

/*
 * FirstPerson.frame with ThirdPerson.place: `head` is where the player's
 * eyes are in the world (tiles, the ground already counted in), `orbit*` the
 * orbit camera's eye, aim and field of view (what the blend starts from).
 * `blocked` may be NULL (an empty world).
 */
void VoxelFreeCam_Frame(VoxelFreeCam *fc, const float head[3],
                        const float orbitEye[3], const float orbitFocus[3], float orbitFov,
                        VoxelFreeCamBlocked blocked, void *context, VoxelFreeCamView *view);

/* ThirdPerson.reach, exposed for the host test: how far back along
 * (bx, by, bz) from `pivot` the eye can stand, up to `want`. */
float VoxelFreeCam_Reach(const float pivot[3], float bx, float by, float bz, float want,
                         VoxelFreeCamBlocked blocked, void *context);

/* The yaw that turns a card's south-facing normal towards the eye, as
 * voxel_entities.c's yaw (the bearing from the card to the eye). */
float VoxelFreeCam_CardYaw(const float eye[3], float cardX, float cardZ);

/* ------- the walk (FreeMove.lua) ------- */

/* The four directions of the grid, by the decompilation's DIR_* numbers. */
enum { VOXEL_FC_DIR_NONE = 0, VOXEL_FC_DIR_SOUTH = 1, VOXEL_FC_DIR_NORTH = 2,
       VOXEL_FC_DIR_WEST = 3, VOXEL_FC_DIR_EAST = 4 };

/* FirstPerson.compassFacing: the head's flat bearing as one of the four
 * facings (45-degree boundaries). */
int VoxelFreeCam_CompassFacing(float yaw);

/* The bearing of a facing, the Lua's FACING_ANGLE. */
float VoxelFreeCam_FacingAngle(int dir);

/* FirstPerson.moveWorld: a camera-space move (strafe right, advance) as a
 * world-space one. */
void VoxelFreeCam_MoveWorld(float yaw, float right, float forward, float *wx, float *wz);

/*
 * The direction the stick asks for, with the camera turned to `yaw`:
 * `right` and `forward` in the units of the Circle Pad (about -156..156;
 * the D-pad is 128), `previous` the direction asked last frame (for the
 * hysteresis the Circle Pad's own decoding has, 3ds_input_decode.c: a
 * direction is entered at CTR_CIRCLE_ENTER, left at CTR_CIRCLE_LEAVE, and
 * kept near a diagonal). Returns a VOXEL_FC_DIR_*.
 */
int VoxelFreeCam_Steer(int previous, float yaw, float right, float forward);

/*
 * Turning a standing player to face where the head looks, as the Lua sets
 * p.facing every tick. The grid game turns on a tap of the direction: a
 * press against its facing turns it without a step, and the tap is one
 * frame, then quiet for a few so the turn can finish.
 */
typedef struct
{
    int cooldown;
} VoxelFreeCamTap;

/* The direction to tap this frame (VOXEL_FC_DIR_NONE for none). `free` is
 * whether a tap would be taken as one: the player standing still at the
 * centre of a tile with the field's controls free and nothing held. */
int VoxelFreeCam_TurnTap(VoxelFreeCamTap *tap, bool free, int facing, int wanted);

#endif
