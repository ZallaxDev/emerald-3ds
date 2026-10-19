#include <stdlib.h>
#include <math.h>
#include "global.h"
#include "main.h"
#include "overworld.h"
#include "event_object_movement.h"
#include "fieldmap.h"
#include "field_player_avatar.h"
#include "script.h"
#include "palette.h"
#include "event_data.h"
#include "bike.h"
#include "constants/flags.h"
#include "constants/field_effects.h"
#include "3ds_platform.h"
#include "3ds_freecam.h"
#if CTR_VOXEL_ENABLED
#include "voxel/voxel_freecam.h"
#include "voxel/voxel_freemove.h"
/* voxel_entities.h: reads the player's pictures in, on this thread. */
void VoxelEntities_WarmPlayerFrames(void);
#endif
#include "3ds_audio.h"
#include "3ds_emu_regs.h"
#include "3ds_bottom.h"
#include "siirtc.h"
#include "gba/flash_internal.h"

static time_t sClockOffset;
static u8 sRtcStatus = SIIRTCINFO_24HOUR;

void CtrEmu_Reset(void)
{
    memset(REG_BASE, 0, 0x400);
    memset(PLTT, 0, PLTT_SIZE);
    memset(VRAM_, 0, VRAM_SIZE);
    memset(OAM, 0, OAM_SIZE);
    REG_IME = 1;
    REG_KEYINPUT = KEYS_MASK;
    INTR_CHECK = 0;
    INTR_VECTOR = NULL;
}

void CtrEmu_BeginVBlank(void)
{
    REG_VCOUNT = 160;
    REG_DISPSTAT |= DISPSTAT_VBLANK;
    REG_IF |= INTR_FLAG_VBLANK;
}

void CtrEmu_EndVBlank(void)
{
    REG_DISPSTAT &= ~DISPSTAT_VBLANK;
    REG_IF &= ~INTR_FLAG_VBLANK;
    REG_VCOUNT = 0;
}

#if CTR_VOXEL_ENABLED
/*
 * OPTIONS' CAMERA on 1ST or 3RD (voxel_freecam.h): the Circle Pad and the
 * D-pad mean forward, back, left and right of where the camera looks, and
 * a player left standing is tapped round to face where the head looks, so
 * that A speaks to what is in front of the lens. Everything here is the
 * input of the field with nothing else going on: a script, a menu, a
 * battle, a measured scenario or a fade are left exactly as they were.
 *
 * Two ways to walk. With OPTIONS' MOVE on FREE and the player on foot, the
 * walk is free (voxel_freemove.h): the stick moves the player in any
 * direction over walkable tiles, and the game's tile follows without a grid
 * walk. A wall, a ledge or a warp still asks the game by pressing. Otherwise
 * (on a bike, surfing, MOVE on GRID) the stick's direction, turned by the
 * camera's yaw, is the one of the four the game is pressed.
 */

static bool sFreeWalkTookStep;

bool CtrFreeMove_TookStep(void)
{
    bool took = sFreeWalkTookStep;

    sFreeWalkTookStep = false;
    return took;
}

/* The game's own answer to a step from the player's tile, for the free walk:
 * refused by a wall, an object, a ledge. */
static int FreeWalkBlocked(void *context, int dir)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    s16 x = player->currentCoords.x, y = player->currentCoords.y;

    (void)context;
    MoveCoords((u8)dir, &x, &y);
    if (GetCollisionAtCoords(player, x, y, (u32)dir) == COLLISION_NONE)
        return 0;
    return 1;
}

/* Put the game's tile under P without starting a walk animation. */
static void FreeWalkCommit(int dir)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    s16 x = player->currentCoords.x, y = player->currentCoords.y;
    s16 dx = 0, dy = 0;

    MoveCoords((u8)dir, &x, &y);
    if (player->heldMovementActive)
        ObjectEventClearHeldMovement(player);
    ShiftObjectEventCoords(player, x, y);
    ShiftStillObjectEventCoords(player);
    player->currentMetatileBehavior = MapGridGetMetatileBehaviorAt(x, y);
    ObjectEventUpdateElevation(player);
    SetObjectEventDirection(player, (u8)dir);
    switch (dir)
    {
    case DIR_SOUTH: dy = 1; break;
    case DIR_NORTH: dy = -1; break;
    case DIR_WEST:  dx = -1; break;
    case DIR_EAST:  dx = 1; break;
    default: break;
    }
    if (dx != 0 || dy != 0)
    {
        CameraMove(dx, dy);
        UpdateObjectEventsForCameraUpdate(dx, dy);
    }
    gPlayerAvatar.runningState = MOVING;
    sFreeWalkTookStep = true;
}

/* Whether the player runs this step: B (or always, OPTIONS' RUN) with the
 * running shoes, where the game lets it. */
static bool FreeWalkRuns(u16 held)
{
    const struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    bool b = (held & B_BUTTON) != 0;

    if (CtrPlayer_RunAlways())
        b = !b;
    return b && FlagGet(FLAG_SYS_B_DASH) && !IsRunningDisallowed(player->currentMetatileBehavior);
}

static u16 FreeCamKeysOnce(const CtrInput *input, u16 held)
{
    static int sPrevious;
    static unsigned sWarm;
    bool engaged = CtrFreeCam_Engaged() && !CtrScenario_Active();
    bool field = engaged && gMain.callback2 == CB2_Overworld && !gMain.inBattle
              && !ArePlayerFieldControlsLocked() && !ScriptContext_IsEnabled()
              && !gPaletteFade.active;
    const struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    VoxelFreeMove *move = VoxelFreeMove_Get();
    bool freeWalk;
    float right = 0.0f, forward = 0.0f;
    int dir;

    CtrFreeCam_SetDriving(field);
    freeWalk = field && CtrFreeCam_FreeWalk() && (gPlayerAvatar.flags & PLAYER_AVATAR_FLAG_ON_FOOT)
            && !(gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_SURFING | PLAYER_AVATAR_FLAG_UNDERWATER
                                        | PLAYER_AVATAR_FLAG_FORCED_MOVE))
            && player->active;
    if (!freeWalk)
        VoxelFreeMove_Idle(move, player->currentCoords.x, player->currentCoords.y);
    if (!field)
    {
        sPrevious = 0;
        return held;
    }
    if ((sWarm++ & 31u) == 0)
        VoxelEntities_WarmPlayerFrames();
    /* L and R turn the head (the C-Stick is the New 3DS's own, 3ds_input.c). */
    held &= ~(CTR_KEY_L | CTR_KEY_R);
    if (input->physicalHeld & CTR_KEY_DPAD)
    {
        right = ((input->physicalHeld & CTR_KEY_RIGHT) ? 128.0f : 0.0f)
              - ((input->physicalHeld & CTR_KEY_LEFT) ? 128.0f : 0.0f);
        forward = ((input->physicalHeld & CTR_KEY_UP) ? 128.0f : 0.0f)
                - ((input->physicalHeld & CTR_KEY_DOWN) ? 128.0f : 0.0f);
    }
    else
    {
        right = (float)input->circleX;
        forward = (float)input->circleY;
    }
    if (freeWalk)
    {
        float magnitude = sqrtf(right * right + forward * forward), speed = 0.0f;
        float wx = 0.0f, wz = 0.0f;

        sPrevious = 0;
        if (magnitude >= (float)CTR_CIRCLE_ENTER || ((input->physicalHeld & CTR_KEY_DPAD) && magnitude > 0.0f))
        {
            /* The D-pad is a full push in its direction; the Circle Pad's
             * throw is the speed, from a crawl to a full walk. */
            float throw_ = (input->physicalHeld & CTR_KEY_DPAD) ? 1.0f : magnitude / 130.0f;

            if (throw_ > 1.0f)
                throw_ = 1.0f;
            VoxelFreeCam_MoveWorld(CtrFreeCam_Yaw(), right / magnitude, forward / magnitude, &wx, &wz);
            speed = throw_ * (FreeWalkRuns(held) ? 1.0f / 8.0f : 1.0f / 16.0f);
        }
        dir = VoxelFreeMove_Step(move, player->currentCoords.x, player->currentCoords.y,
                                 wx * speed, wz * speed, FreeWalkBlocked, NULL);
        if (dir != 0 && !FreeWalkBlocked(NULL, dir))
        {
            FreeWalkCommit(dir);
            VoxelFreeMove_Sync(move, gObjectEvents[gPlayerAvatar.objectEventId].currentCoords.x,
                               gObjectEvents[gPlayerAvatar.objectEventId].currentCoords.y);
            dir = 0;
        }
        if (dir == 0 && speed == 0.0f)
        {
            /* Standing at rest: turn to the look. */
            bool free = gPlayerAvatar.tileTransitionState == T_NOT_MOVING && !(held & CTR_KEY_A);

            dir = CtrFreeCam_TurnTap(free, player->facingDirection);
        }
        else
            CtrFreeCam_TurnTap(false, 0);
    }
    else
    {
        dir = CtrFreeCam_Steer(sPrevious, right, forward);
        sPrevious = dir;
        /* Standing at a tile's centre with nothing held: turn to the look. */
        if (dir == 0)
        {
            bool free = gPlayerAvatar.tileTransitionState == T_NOT_MOVING
                     && !(gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE))
                     && right > -CTR_CIRCLE_LEAVE && right < CTR_CIRCLE_LEAVE
                     && forward > -CTR_CIRCLE_LEAVE && forward < CTR_CIRCLE_LEAVE && !(held & CTR_KEY_A);

            dir = CtrFreeCam_TurnTap(free, player->facingDirection);
        }
        else
            CtrFreeCam_TurnTap(false, 0);
    }
    held &= ~CTR_KEY_DPAD;
    switch (dir)
    {
    case 1: held |= CTR_KEY_DOWN; break;
    case 2: held |= CTR_KEY_UP; break;
    case 3: held |= CTR_KEY_LEFT; break;
    case 4: held |= CTR_KEY_RIGHT; break;
    default: break;
    }
    return held;
}

/* The game reads the keys once a frame, but a wait for a release (soft reset)
 * reads them again in between: the free walk must step once a game frame, or
 * the player would walk twice as fast. A second read in the same frame gets
 * the first's answer. */
static u16 FreeCamKeys(const CtrInput *input, u16 held)
{
    static uint64_t sFrame = ~(uint64_t)0;
    static u16 sDpad;
    static bool sTouched;
    uint64_t now = CtrPlatform_FrameCount();

    if (now != sFrame)
    {
        u16 out = FreeCamKeysOnce(input, held);

        sFrame = now;
        sTouched = out != held;
        sDpad = out & CTR_KEY_DPAD;
        return out;
    }
    if (sTouched)
        held = (held & ~(CTR_KEY_L | CTR_KEY_R | CTR_KEY_DPAD)) | sDpad;
    return held;
}
#else
bool CtrFreeMove_TookStep(void)
{
    return false;
}
#endif

u16 Platform_GetKeyInput(void)
{
    const CtrInput *input = CtrInput_Get();
    u16 held = input->held;

    /* Y is the 3DS's own SELECT: the registered item, as the bottom screen's
     * Y button says. */
    if (input->physicalHeld & CTR_KEY_Y)
        held |= SELECT_BUTTON;
#if CTR_VOXEL_ENABLED
    held = FreeCamKeys(input, held);
#endif
    /* The bottom screen's column takes the buttons while X has its focus;
     * its touch controls press buttons through here. */
    held = CtrBottom_FilterKeys(held) | CtrBottom_InjectedKeys();
    REG_KEYINPUT = held ^ KEYS_MASK;
    return held;
}

/* AgbMain owns the loop: this is the game's frame boundary. */
void CtrGame_WaitFrame(void);
void VBlankIntrWait(void) { CtrGame_WaitFrame(); }
/* At once, as on the GBA: DoSoftReset expects SoftReset not to return. */
void SoftReset(u32 flags)
{
    (void)flags;
    CtrGame_SoftReset();
}

static u8 Bcd(unsigned value) { return ((value / 10) << 4) | (value % 10); }
static unsigned FromBcd(u8 value) { return (value >> 4) * 10 + (value & 15); }

void Platform_GetStatus(struct SiiRtcInfo *rtc) { rtc->status = sRtcStatus; }
void Platform_SetStatus(struct SiiRtcInfo *rtc) { sRtcStatus = rtc->status; }

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    /* A measured scenario's own clock: the game seeds its random numbers
     * from it (3ds_scenario.c). */
    int64_t pinned;
    time_t now = (CtrScenario_Clock(&pinned) ? (time_t)pinned : time(NULL)) + sClockOffset;
    struct tm value;
    if (!localtime_r(&now, &value))
        CtrPlatform_Fatal("RTC conversion failed");
    memset(rtc, 0, sizeof(*rtc));
    rtc->status = sRtcStatus;
    rtc->year = Bcd((value.tm_year + 1900) % 100);
    rtc->month = Bcd(value.tm_mon + 1);
    rtc->day = Bcd(value.tm_mday);
    rtc->dayOfWeek = Bcd(value.tm_wday);
    rtc->hour = Bcd(value.tm_hour);
    rtc->minute = Bcd(value.tm_min);
    rtc->second = Bcd(value.tm_sec);
}

void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
    struct tm value = {0};
    value.tm_year = 100 + FromBcd(rtc->year);
    value.tm_mon = FromBcd(rtc->month) - 1;
    value.tm_mday = FromBcd(rtc->day);
    value.tm_hour = FromBcd(rtc->hour);
    value.tm_min = FromBcd(rtc->minute);
    value.tm_sec = FromBcd(rtc->second);
    value.tm_isdst = -1;
    time_t requested = mktime(&value);
    if (requested == (time_t)-1)
        CtrPlatform_Fatal("invalid logical RTC date");
    sClockOffset = requested - time(NULL);
    CtrLog_Write(CTR_LOG_GAME, "logical RTC updated (session only)");
}

void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    struct SiiRtcInfo full;
    Platform_GetDateTime(&full);
    rtc->hour = full.hour;
    rtc->minute = full.minute;
    rtc->second = full.second;
}

void Platform_SetTime(struct SiiRtcInfo *rtc)
{
    struct SiiRtcInfo full;
    Platform_GetDateTime(&full);
    full.hour = rtc->hour;
    full.minute = rtc->minute;
    full.second = rtc->second;
    Platform_SetDateTime(&full);
}

void Platform_SetAlarm(u8 *alarmData)
{
    (void)alarmData;
    CTR_UNIMPLEMENTED("RTC_ALARM: hardware alarm not supported");
}

/*
 * Nothing to flush: Port_WriteFlash commits each sector to the file as the game
 * writes it, so the save on SD is already current by the time a save finishes.
 * save.c only calls this outside the port bridge anyway.
 */
void Platform_StoreSaveFile(void)
{
}

void Platform_ReadFlash(u16 sector, u32 offset, u8 *dest, u32 size)
{
    u32 address = ((u32)sector << 12) + offset;

    if (address >= sizeof(FLASH_BASE) || size > sizeof(FLASH_BASE) - address)
    {
        CtrLog_Write(CTR_LOG_ERROR, "flash read out of range: %lu+%lu",
                     (unsigned long)address, (unsigned long)size);
        return;
    }
    memcpy(dest, FLASH_BASE + address, size);
}

void Platform_QueueAudio(s32 *samples, s32 count)
{
    /*
     * m4aSoundVSync hands over one mixer frame per VBlank, measured in bytes
     * because SDL's queue is. Four bytes per sample, two samples per frame.
     */
    CtrAudio_Queue(samples, count / (s32)(2 * sizeof(s32)));
}
