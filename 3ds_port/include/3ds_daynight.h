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

typedef struct
{
    float rgb[3];
    float daylight;
    float glow;
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
