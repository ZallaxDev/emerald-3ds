#include "3ds_daynight.h"
#include "3ds_platform.h"
#include <math.h>

static const unsigned sSpeeds[CTR_DAYNIGHT_SPEED_COUNT] = {1, 6, 12, 24, 60, 120};
static CtrDayNightClock sClock;
static CtrDayNightLight sLight = {{1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};
static CtrDayNightSun sSun = {0.85f, 0.55f, 1.0f, 0};
static unsigned sLocalSeconds;
static uint64_t sSampleMs;
static bool sSampled, sEnabled, sResume, sValid;
static int64_t sWallSeconds;

unsigned CtrDayNight_Speed(int index)
{
    return index >= 0 && index < CTR_DAYNIGHT_SPEED_COUNT ? sSpeeds[index] : 1;
}

void CtrDayNight_Advance(CtrDayNightClock *clock, uint64_t nowMs, unsigned localSeconds, int speed)
{
    unsigned rate = CtrDayNight_Speed(speed);

    if (!clock->ready || (rate == 1 && !clock->shifted))
        clock->timeMs = (localSeconds % 86400u) * 1000u;
    if (clock->ready && nowMs >= clock->lastMs)
    {
        uint64_t elapsed = nowMs - clock->lastMs;
        uint64_t room = CTR_DAYNIGHT_BONUS_LIMIT_MS - clock->berryBonusMs;

        if (rate != 1 || clock->shifted)
            clock->timeMs = (uint32_t)((clock->timeMs + (elapsed % CTR_DAYNIGHT_DAY_MS) * rate)
                                      % CTR_DAYNIGHT_DAY_MS);
        if (rate > 1)
            clock->berryBonusMs += elapsed > room / (rate - 1u) ? room : elapsed * (rate - 1u);
    }
    if (rate > 1)
        clock->shifted = true;
    clock->lastMs = nowMs;
    clock->ready = true;
}

void CtrDayNight_Sync(CtrDayNightClock *clock, uint64_t nowMs, unsigned localSeconds, int speed)
{
    if (clock->ready)
        CtrDayNight_Advance(clock, nowMs, localSeconds, speed);
    clock->timeMs = (localSeconds % 86400u) * 1000u;
    clock->shifted = false;
    clock->lastMs = nowMs;
    clock->ready = true;
}

CtrDayNightLight CtrDayNight_At(unsigned timeMs)
{
    static const struct
    {
        unsigned minute;
        CtrDayNightLight light;
    } keys[] = {
        {0,    {{0.66f, 0.74f, 0.92f}, 0.0f, 1.0f}},
        {120,  {{0.66f, 0.74f, 0.92f}, 0.0f, 1.0f}},
        {240,  {{0.96f, 0.94f, 0.88f}, 0.75f, 0.0f}},
        {480,  {{0.96f, 0.94f, 0.88f}, 0.75f, 0.0f}},
        {600,  {{1.00f, 1.00f, 1.00f}, 1.0f, 0.0f}},
        {900,  {{1.00f, 1.00f, 1.00f}, 1.0f, 0.0f}},
        {1020, {{1.00f, 0.80f, 0.65f}, 0.35f, 0.25f}},
        {1080, {{1.00f, 0.80f, 0.65f}, 0.35f, 0.25f}},
        {1200, {{0.66f, 0.74f, 0.92f}, 0.0f, 1.0f}},
        {1440, {{0.66f, 0.74f, 0.92f}, 0.0f, 1.0f}},
    };
    unsigned current = timeMs % CTR_DAYNIGHT_DAY_MS;

    for (unsigned index = 1; index < sizeof(keys) / sizeof(keys[0]); ++index)
        if (current < keys[index].minute * 60000u)
        {
            CtrDayNightLight light = keys[index - 1].light;
            float blend = (float)(current - keys[index - 1].minute * 60000u)
                        / ((keys[index].minute - keys[index - 1].minute) * 60000u);

            blend = blend * blend * (3.0f - 2.0f * blend);
            for (unsigned channel = 0; channel < 3; ++channel)
                light.rgb[channel] += (keys[index].light.rgb[channel] - light.rgb[channel]) * blend;
            light.daylight += (keys[index].light.daylight - light.daylight) * blend;
            light.glow += (keys[index].light.glow - light.glow) * blend;
            return light;
        }
    return keys[0].light;
}

CtrDayNightBlend CtrDayNight_Moonlight(const CtrDayNightLight *light, float fade,
                                     const float fadeRgb[3])
{
    CtrDayNightBlend blend = {{70.0f / 255.0f, 92.0f / 255.0f, 136.0f / 255.0f}, 0.0f};
    float moon = 0.06f * light->glow * light->glow;
    float remaining = moon * (1.0f - fade);

    blend.amount = remaining + fade;
    if (blend.amount > 0.0f)
        for (unsigned channel = 0; channel < 3; ++channel)
            blend.rgb[channel] = (blend.rgb[channel] * remaining + fadeRgb[channel] * fade)
                               / blend.amount;
    return blend;
}

static unsigned SunKey(unsigned timeMs)
{
    unsigned minute = (timeMs % CTR_DAYNIGHT_DAY_MS) / 60000u;

    return minute >= 240u && minute < 1200u ? minute / 15u + 2u : 1u;
}

CtrDayNightSun CtrDayNight_SunAt(unsigned timeMs)
{
    unsigned key = SunKey(timeMs);
    CtrDayNightSun sun = {0.0f, 0.55f, 0.0f, key};

    if (key != 1u)
    {
        float progress = ((float)((key - 2u) * 15u) - 240.0f) / 960.0f;
        float altitude = sinf(progress * 3.141592654f);

        sun.dx = -1.35f * cosf(progress * 3.141592654f);
        sun.dz = 0.35f + 0.20f * altitude;
        sun.strength = fminf(1.0f, altitude * 2.0f);
    }
    return sun;
}

void CtrDayNight_Resume(void)
{
    sResume = true;
    sSampled = false;
}

void CtrDayNight_Update(void)
{
    uint64_t now = CtrPlatform_Milliseconds();
    bool enabled = CtrSettings_GetInt("day_night", 0) == 1;
    bool active = enabled || CtrSettings_GetInt("timed_encounters", 0) == 1
                          || CtrSettings_GetInt("show_clock", 0) == 1
                          || sClock.shifted || sClock.berryBonusMs != 0;
    int speed = CtrSettings_GetInt("clock_speed", 0);
    CtrDayNightSun previousSun = sSun;

    sLight = (CtrDayNightLight){{1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};
    sSun = (CtrDayNightSun){0.85f, 0.55f, 1.0f, 0};
    sValid = false;
    if (!active && CtrDayNight_Speed(speed) == 1)
    {
        sEnabled = false;
        sClock.ready = false;
        return;
    }
    if (!sSampled || !sEnabled || now < sSampleMs || now - sSampleMs >= 1000)
    {
        int64_t wall = CtrPlatform_WallSeconds();

        if (!CtrGame_LocalTimeSeconds(&sLocalSeconds))
        {
            sSampled = false;
            sResume = true;
            return;
        }
        if (sResume && sClock.ready && sClock.shifted && sWallSeconds > 0 && wall > sWallSeconds)
            sClock.timeMs = (uint32_t)((sClock.timeMs
                         + (uint64_t)(wall - sWallSeconds) % 86400u * 1000u) % CTR_DAYNIGHT_DAY_MS);
        sWallSeconds = wall;
        sSampleMs = now;
        sSampled = true;
    }
    if (sResume)
        sClock.lastMs = now;
    sResume = false;
    CtrDayNight_Advance(&sClock, now, sLocalSeconds, speed);
    sEnabled = true;
    sValid = true;
    if (enabled && CtrGame_IsOutdoor())
    {
        sLight = CtrDayNight_At(sClock.timeMs);
        sSun = previousSun.key == SunKey(sClock.timeMs)
             ? previousSun : CtrDayNight_SunAt(sClock.timeMs);
    }
}

bool CtrDayNight_Time(unsigned *seconds, unsigned *rate)
{
    CtrDayNight_Update();
    if (!sValid)
        return false;
    *seconds = sClock.timeMs / 1000u;
    *rate = CtrDayNight_Speed(CtrSettings_GetInt("clock_speed", 0));
    return true;
}

/* Off (every clock option off, 1X, nothing owed) the game keeps the GBA's
 * own timing. */
bool CtrDayNight_Active(void)
{
    CtrDayNight_Update();
    return sValid;
}

const CtrDayNightLight *CtrDayNight_Get(void)
{
    return &sLight;
}

const CtrDayNightSun *CtrDayNight_GetSun(void)
{
    return &sSun;
}

bool CtrDayNight_BerryDue(void)
{
    CtrDayNight_Update();
    return sValid && sClock.berryBonusMs >= 60000u;
}

unsigned CtrDayNight_TakeBerryMinutes(void)
{
    CtrDayNight_Update();
    if (!sValid)
        return 0;
    unsigned minutes = (unsigned)(sClock.berryBonusMs / 60000u);

    sClock.berryBonusMs %= 60000u;
    return minutes;
}

void CtrDayNight_Reset(void)
{
    sClock = (CtrDayNightClock){0};
    sSampled = sEnabled = sResume = sValid = false;
    sWallSeconds = 0;
    sLight = (CtrDayNightLight){{1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};
    sSun = (CtrDayNightSun){0.85f, 0.55f, 1.0f, 0};
}

/* The game's daily events, lastBerryTreeUpdate and the RTC are not touched:
 * only the port's accelerated phase goes. The berry bonus owed so far stays,
 * including the time since the last update at the speed that was selected. */
bool CtrDayNight_SyncToConsole(void)
{
    uint64_t now = CtrPlatform_Milliseconds();
    int speed = CtrSettings_GetInt("clock_speed", 0);
    unsigned local;

    if (!CtrGame_LocalTimeSeconds(&local))
        return false;
    /* Suspended or not yet resampled: that time was owed at 1X, not at speed. */
    if (sResume)
        sClock.ready = false;
    if (sClock.ready)
        CtrDayNight_Advance(&sClock, now, local, speed);
    /* The setter updates the clock at the old rate. Clear its shifted phase
     * afterwards, or that update would immediately undo the sync. */
    CtrSettings_SetInt("clock_speed", 0);
    CtrDayNight_Sync(&sClock, now, local, 0);
    sLocalSeconds = local;
    sSampleMs = now;
    sSampled = true;
    sResume = false;
    sWallSeconds = CtrPlatform_WallSeconds();
    /* Light, sun and encounters see the local clock from now. */
    CtrDayNight_Update();
    return true;
}

bool CtrDayNight_Snapshot(CtrDayNightSnapshot *snapshot)
{
    CtrDayNight_Update();
    if (!sValid || snapshot == NULL)
        return false;
    *snapshot = (CtrDayNightSnapshot){sClock.timeMs, sClock.berryBonusMs,
                                    CtrPlatform_WallSeconds(), sClock.shifted};
    return snapshot->wallSeconds > 0;
}

bool CtrDayNight_Restore(const CtrDayNightSnapshot *snapshot)
{
    int64_t wall = CtrPlatform_WallSeconds();

    if (snapshot == NULL || snapshot->timeMs >= CTR_DAYNIGHT_DAY_MS
     || snapshot->berryBonusMs > CTR_DAYNIGHT_BONUS_LIMIT_MS || snapshot->wallSeconds <= 0 || wall <= 0)
        return false;
    CtrDayNight_Reset();
    sClock.timeMs = snapshot->timeMs;
    if (snapshot->shifted && wall > snapshot->wallSeconds)
        sClock.timeMs = (uint32_t)((sClock.timeMs
                         + (uint64_t)(wall - snapshot->wallSeconds) % 86400u * 1000u) % CTR_DAYNIGHT_DAY_MS);
    sClock.berryBonusMs = snapshot->berryBonusMs;
    sClock.shifted = snapshot->shifted;
    sClock.ready = true;
    sClock.lastMs = CtrPlatform_Milliseconds();
    sWallSeconds = wall;
    sResume = true;
    return true;
}
