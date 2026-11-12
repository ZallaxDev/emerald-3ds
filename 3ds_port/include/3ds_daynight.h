#ifndef CTR_DAYNIGHT_H
#define CTR_DAYNIGHT_H

#include <stdbool.h>
#include <stdint.h>

#define CTR_DAYNIGHT_SPEED_COUNT 6
#define CTR_DAYNIGHT_DAY_MS 86400000u
#define CTR_DAYNIGHT_BONUS_LIMIT_MS ((uint64_t)2147483647u * 60000u)

typedef struct
{
    uint64_t lastMs;
    uint32_t timeMs;
    uint64_t berryBonusMs;
    bool ready;
    bool shifted;
} CtrDayNightClock;

typedef struct
{
    uint32_t timeMs;
    uint64_t berryBonusMs;
    int64_t wallSeconds;
    bool shifted;
} CtrDayNightSnapshot;

/*
 * The hour's light. rgb, daylight and glow are what the 2D renderer, the
 * sprites and the lamps read; the rest is voxel-only (LightFor, DrawRays in
 * ctr_voxel.c), all of it uniforms and texture-environment constants, so a
 * later restriction to one console model is a matter of not reading it.
 *
 *   rgb       the single multiply tint 2D can express: the split look below
 *             folded to one colour, never above 1
 *   daylight  how much hard sun there is (dapples, dust); below 1 whenever
 *             rgb is not neutral, because the 2D sprites are tinted by that test
 *   glow      how much the lamps and windows are lit, and the moonlight's size
 *   sun       what the lit ground and walls are multiplied by, on top of the
 *             weather's own sun tint
 *   shade     the same for shadow: sky light, complementary to the sun's
 *   haze      what the distance fades to, as a multiple of the clear haze colour
 *   hazeAmount, bloom  multiples of the weather's own
 *   rays      the sun rays' strength, its own curve (not daylight's)
 *   rayRgb    a multiple of the rays' colour
 *   rayX, rayAngle, rayWidth, rayLength  where the rays come from and how:
 *             their source along the top edge (0 left, 1 right, screen units),
 *             the angle they travel at off the vertical (degrees, positive
 *             towards the right), and the fan's scales across and along them.
 *             rayWidth 0 is the fixed corner fan of the clear, untimed look.
 *   sunDx, sunDz, sunAlt  the sun's own path at this minute, continuous (the
 *             CtrDayNightSun of the 15-minute keys is the same path): its
 *             direction as gVoxelSun has it, positive towards -x / -z, and its
 *             height, 0 at the horizon to 1 at noon. The sun's glitter on the
 *             water follows them (GlitterShapeFor in ctr_voxel.c); untimed
 *             they are the legacy sun's, top left.
 */
typedef struct
{
    float rgb[3];
    float daylight;
    float glow;
    float sun[3], shade[3], haze[3];
    float hazeAmount, bloom;
    float rays, rayRgb[3];
    float rayX, rayAngle, rayWidth, rayLength;
    float sunDx, sunDz, sunAlt;
} CtrDayNightLight;

typedef struct
{
    float rgb[3];
    float amount;
} CtrDayNightBlend;

typedef struct
{
    float dx, dz, strength;
    uint32_t key;
} CtrDayNightSun;

unsigned CtrDayNight_Speed(int index);
void CtrDayNight_Advance(CtrDayNightClock *clock, uint64_t nowMs, unsigned localSeconds, int speed);
/* Pure: credits the time since the last advance at `speed` (berry bonus
 * included), then puts the clock back on localSeconds, unshifted. */
void CtrDayNight_Sync(CtrDayNightClock *clock, uint64_t nowMs, unsigned localSeconds, int speed);
CtrDayNightLight CtrDayNight_At(unsigned timeMs);
CtrDayNightBlend CtrDayNight_Moonlight(const CtrDayNightLight *light, float fade,
                                     const float fadeRgb[3]);
CtrDayNightSun CtrDayNight_SunAt(unsigned timeMs);
void CtrDayNight_Update(void);
void CtrDayNight_Resume(void);
const CtrDayNightLight *CtrDayNight_Get(void);
const CtrDayNightSun *CtrDayNight_GetSun(void);
bool CtrDayNight_Time(unsigned *seconds, unsigned *rate);
bool CtrDayNight_Active(void);
bool CtrDayNight_BerryDue(void);
unsigned CtrDayNight_TakeBerryMinutes(void);
void CtrDayNight_Reset(void);
/* SYNC CLOCK (OPTIONS): back to the game's local clock and clock_speed 1X,
 * keeping the pending berry bonus. False, with nothing changed, when the
 * local time cannot be read. Kept on disk with the next in-game save. */
bool CtrDayNight_SyncToConsole(void);
bool CtrDayNight_Snapshot(CtrDayNightSnapshot *snapshot);
bool CtrDayNight_Restore(const CtrDayNightSnapshot *snapshot);

#endif
