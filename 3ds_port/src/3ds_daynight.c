#include "3ds_daynight.h"
#include "3ds_platform.h"
#include <math.h>

/* No cycle: every multiple is 1 and the rays are the legacy corner fan (the
 * 35 degrees of VOXEL_RAY_ANGLE, rayWidth 0), so nothing changes with
 * DAY / NIGHT off, indoors or without a clock. */
#define CTR_DAYNIGHT_NEUTRAL \
    {{1.0f, 1.0f, 1.0f}, 1.0f, 0.0f, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, \
     1.0f, 1.0f, 1.0f, {1.0f, 1.0f, 1.0f}, 0.0f, 35.0f, 0.0f, 1.0f, \
     0.85f, 0.55f, 0.55f}

static const unsigned sSpeeds[CTR_DAYNIGHT_SPEED_COUNT] = {1, 6, 12, 24, 60, 120};
static CtrDayNightClock sClock;
static CtrDayNightLight sLight = CTR_DAYNIGHT_NEUTRAL;
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

/*
 * The day's palette. Each key is a mood, held by the colours it carries; the
 * hours between are cubic Hermite curves through them with monotone tangents
 * (no overshoot, so a flat stretch stays exactly flat and no channel leaves
 * the range of its two keys), and the day closes on itself, so midnight has
 * no seam. Every value is a multiple or a colour the voxel light and the 2D
 * tint are made from at run time: nothing here costs the GPU anything.
 *
 * The sun and the shade are tinted apart, complementary: warm light over cool
 * sky-lit shadow, and at night, where no sun is left, the same blue for both.
 * "haze" is the colour the distance fades to, written as the colour itself
 * and turned into a multiple of the clear haze (CTR_DAYNIGHT_HAZE_CLEAR, the
 * same as VOXEL_HAZE_COLOUR in ctr_voxel.c) so the weather's own haze still
 * composes with it: a grey rain over a sunset goes red-grey, not clear.
 */
#define CTR_DAYNIGHT_HAZE_CLEAR {210.0f / 255.0f, 230.0f / 255.0f, 250.0f / 255.0f}
/* How much of the lit and of the shaded colour the single 2D tint takes: the
 * light covers about two thirds of what is on the screen. */
#define CTR_DAYNIGHT_TINT_SHADE 0.35f

enum
{
    K_SUN = 0, K_SHADE = 3, K_HAZE = 6, K_HAZE_AMOUNT = 9, K_BLOOM = 10,
    K_DAYLIGHT = 11, K_GLOW = 12, K_RAYS = 13, K_RAY_RGB = 14, K_COUNT = 17
};

typedef struct
{
    unsigned minute;
    float v[K_COUNT];
} CtrDayNightKey;

/*
 * minute | sun rgb | shade rgb | haze rgb | haze x | bloom x | daylight | glow | rays | ray rgb
 *
 * glow rises through the golden hour and sunset, well before the dark, and
 * falls from the first light of dawn to the morning sun: lamps and windows
 * never jump. daylight stays under 1 wherever the tint is not neutral
 * (obj.inc tints sprites by that test).
 */
#define K(minute, sr, sg, sb, hr, hg, hb, zr, zg, zb, hazeX, bloomX, day, glowV, raysV, rr, rg, rb) \
    {minute, {sr, sg, sb, hr, hg, hb, zr, zg, zb, hazeX, bloomX, day, glowV, raysV, rr, rg, rb}}
static const CtrDayNightKey sKeys[] = {
    /* night: moonlit, blue, the greens a little greyed */
    K(0,    0.48f, 0.60f, 0.92f,  0.48f, 0.60f, 0.92f,  0.05f, 0.08f, 0.17f,  1.60f, 2.00f, 0.00f, 1.00f, 0.00f,  1.00f, 1.00f, 1.00f),
    K(150,  0.44f, 0.56f, 0.90f,  0.44f, 0.56f, 0.90f,  0.04f, 0.07f, 0.15f,  1.70f, 2.10f, 0.00f, 1.00f, 0.00f,  1.00f, 1.00f, 1.00f),
    /* pre-dawn: deep blue lifting, the horizon violet */
    K(255,  0.50f, 0.61f, 0.94f,  0.50f, 0.61f, 0.94f,  0.10f, 0.12f, 0.26f,  1.50f, 1.80f, 0.00f, 1.00f, 0.00f,  1.00f, 1.00f, 1.00f),
    /* dawn: rose and peach light, cool violet shadow, the lamps still on */
    K(315,  1.00f, 0.78f, 0.66f,  0.58f, 0.62f, 0.88f,  0.95f, 0.60f, 0.58f,  1.40f, 1.50f, 0.45f, 0.80f, 1.10f,  1.00f, 0.80f, 0.66f),
    /* early morning: gold turning to white */
    K(390,  1.02f, 0.88f, 0.74f,  0.80f, 0.84f, 0.98f,  0.98f, 0.80f, 0.72f,  1.20f, 1.25f, 0.85f, 0.15f, 1.50f,  1.00f, 0.88f, 0.72f),
    /* clear morning: cool and fresh */
    K(480,  0.96f, 0.985f, 1.00f, 0.90f, 0.95f, 1.00f,  0.80f, 0.90f, 1.00f,  1.05f, 1.05f, 0.97f, 0.00f, 1.10f,  0.96f, 0.98f, 1.00f),
    /* noon: the original colours, untouched */
    K(600,  1.00f, 1.00f, 1.00f,  1.00f, 1.00f, 1.00f,  210.0f / 255.0f, 230.0f / 255.0f, 250.0f / 255.0f,  1.00f, 1.00f, 1.00f, 0.00f, 0.90f,  1.00f, 1.00f, 1.00f),
    K(900,  1.00f, 1.00f, 1.00f,  1.00f, 1.00f, 1.00f,  210.0f / 255.0f, 230.0f / 255.0f, 250.0f / 255.0f,  1.00f, 1.00f, 1.00f, 0.00f, 0.90f,  1.00f, 1.00f, 1.00f),
    /* late afternoon: warm */
    K(990,  1.02f, 0.94f, 0.82f,  0.88f, 0.92f, 1.00f,  0.95f, 0.86f, 0.76f,  1.10f, 1.15f, 0.95f, 0.00f, 1.15f,  1.00f, 0.93f, 0.78f),
    /* golden hour: strong orange key, long cool violet shadows */
    K(1050, 1.04f, 0.86f, 0.62f,  0.70f, 0.74f, 0.98f,  1.00f, 0.70f, 0.48f,  1.25f, 1.45f, 0.75f, 0.15f, 1.70f,  1.00f, 0.82f, 0.55f),
    /* sunset: red-orange light, magenta shadow, the horizon burning */
    K(1110, 1.00f, 0.64f, 0.46f,  0.66f, 0.58f, 0.90f,  0.95f, 0.46f, 0.42f,  1.45f, 1.60f, 0.35f, 0.55f, 1.60f,  1.00f, 0.66f, 0.48f),
    /* the sun gone: red-violet embers over sky-lit blue */
    K(1155, 0.82f, 0.52f, 0.56f,  0.54f, 0.55f, 0.88f,  0.52f, 0.32f, 0.52f,  1.50f, 1.70f, 0.10f, 0.88f, 0.50f,  1.00f, 0.58f, 0.52f),
    /* blue hour: all the light the sky has left */
    K(1200, 0.55f, 0.65f, 0.96f,  0.55f, 0.65f, 0.96f,  0.18f, 0.20f, 0.40f,  1.50f, 1.80f, 0.00f, 0.97f, 0.00f,  1.00f, 1.00f, 1.00f),
    K(1290, 0.48f, 0.60f, 0.92f,  0.48f, 0.60f, 0.92f,  0.05f, 0.08f, 0.17f,  1.60f, 2.00f, 0.00f, 1.00f, 0.00f,  1.00f, 1.00f, 1.00f),
};
#undef K
#define CTR_DAYNIGHT_KEYS (sizeof(sKeys) / sizeof(sKeys[0]))

/* The sun's bounded path: east at 04:00, west at 20:00, always to the north
 * (the camera looks north), as a function of its progress along it. */
#define CTR_DAYNIGHT_SUN_DX 1.35f
static void SunPath(float progress, float *dx, float *dz, float *altitude)
{
    float alt = sinf(progress * 3.141592654f);

    *dx = -CTR_DAYNIGHT_SUN_DX * cosf(progress * 3.141592654f);
    *dz = 0.35f + 0.20f * alt;
    *altitude = alt;
}

/*
 * The rays' geometry for the sun at `progress`. Positive dx is a sun towards
 * -x (west, screen left); the morning's is negative, so the source starts
 * off the top-right corner and travels left across the top edge to the
 * top-left by evening, the shafts leaning the way the light travels: at 07:00
 * they run from the top right down and to the left, the way the shadows on
 * the ground fall.
 *
 *   angle  the light's direction projected on the screen, as the tip of a
 *          shadow sees it: across dx, down cos(pitch) + dz sin(pitch)
 *          (the camera's own pitch is 40 degrees; the user's few degrees
 *          either way move the shafts by less than two)
 *   width  the fan opens at high sun, to near enough vertical shafts from the
 *          top centre, soft and wide, where it is a narrow one from a corner
 *   length the shafts reach further as the sun lowers
 */
#define CTR_DAYNIGHT_RAY_PITCH 40.0f
#define CTR_DAYNIGHT_RAY_TRAVEL 0.38f   /* screen widths per unit of dx */
#define CTR_DAYNIGHT_RAY_WIDEN 0.40f    /* extra width at noon */
#define CTR_DAYNIGHT_RAY_LONG 1.20f     /* length at the horizon... */
#define CTR_DAYNIGHT_RAY_SHORT 0.45f    /* ...less this at noon */
static void RayGeometry(CtrDayNightLight *light, float progress)
{
    float dx, dz, altitude, pitch = CTR_DAYNIGHT_RAY_PITCH * 3.141592654f / 180.0f;
    float down;

    if (progress < 0.0f)
        progress = 0.0f;
    if (progress > 1.0f)
        progress = 1.0f;
    SunPath(progress, &dx, &dz, &altitude);
    down = cosf(pitch) + dz * sinf(pitch);
    light->rayX = 0.5f - dx * CTR_DAYNIGHT_RAY_TRAVEL;
    light->rayAngle = atan2f(dx, down) * 180.0f / 3.141592654f;
    light->rayWidth = 1.0f + CTR_DAYNIGHT_RAY_WIDEN * altitude * altitude * altitude;
    light->rayLength = CTR_DAYNIGHT_RAY_LONG - CTR_DAYNIGHT_RAY_SHORT * altitude;
    light->sunDx = dx;
    light->sunDz = dz;
    light->sunAlt = altitude;
}

/* The tangent of one channel at key `index`: the weighted harmonic mean of
 * the two slopes around it, and flat wherever they differ in sign or either
 * is flat (Fritsch-Carlson). Minutes; the day is periodic. */
static float KeyTangent(unsigned index, unsigned channel)
{
    unsigned previous = (index + CTR_DAYNIGHT_KEYS - 1u) % CTR_DAYNIGHT_KEYS;
    unsigned next = (index + 1u) % CTR_DAYNIGHT_KEYS;
    float h0 = (float)sKeys[index].minute - (float)sKeys[previous].minute;
    float h1 = (float)sKeys[next].minute - (float)sKeys[index].minute;
    float d0, d1, w0, w1;

    if (h0 <= 0.0f)
        h0 += 1440.0f;
    if (h1 <= 0.0f)
        h1 += 1440.0f;
    d0 = (sKeys[index].v[channel] - sKeys[previous].v[channel]) / h0;
    d1 = (sKeys[next].v[channel] - sKeys[index].v[channel]) / h1;
    if (d0 * d1 <= 0.0f)
        return 0.0f;
    w0 = 2.0f * h1 + h0;
    w1 = h1 + 2.0f * h0;
    return (w0 + w1) / (w0 / d0 + w1 / d1);
}

CtrDayNightLight CtrDayNight_At(unsigned timeMs)
{
    static const float haze[3] = CTR_DAYNIGHT_HAZE_CLEAR;
    CtrDayNightLight light = CTR_DAYNIGHT_NEUTRAL;
    float minute = (float)(timeMs % CTR_DAYNIGHT_DAY_MS) / 60000.0f;
    float value[K_COUNT];
    unsigned index = 0, next;
    float h, t, t2, t3;
    bool neutral;

    while (index + 1u < CTR_DAYNIGHT_KEYS && (float)sKeys[index + 1u].minute <= minute)
        ++index;
    next = (index + 1u) % CTR_DAYNIGHT_KEYS;
    h = (float)sKeys[next].minute - (float)sKeys[index].minute;
    if (h <= 0.0f)
        h += 1440.0f;
    t = (minute - (float)sKeys[index].minute) / h;
    t2 = t * t;
    t3 = t2 * t;
    for (unsigned channel = 0; channel < K_COUNT; ++channel)
    {
        float y0 = sKeys[index].v[channel], y1 = sKeys[next].v[channel];

        if (y0 == y1)
        {
            value[channel] = y0;
            continue;
        }
        value[channel] = (2.0f * t3 - 3.0f * t2 + 1.0f) * y0
                       + (t3 - 2.0f * t2 + t) * h * KeyTangent(index, channel)
                       + (-2.0f * t3 + 3.0f * t2) * y1
                       + (t3 - t2) * h * KeyTangent(next, channel);
    }
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        light.sun[channel] = value[K_SUN + channel];
        light.shade[channel] = value[K_SHADE + channel];
        light.haze[channel] = value[K_HAZE + channel] / haze[channel];
        light.rayRgb[channel] = value[K_RAY_RGB + channel];
        light.rgb[channel] = light.sun[channel]
                           + (light.shade[channel] - light.sun[channel]) * CTR_DAYNIGHT_TINT_SHADE;
        if (light.rgb[channel] > 1.0f)
            light.rgb[channel] = 1.0f;
    }
    light.hazeAmount = value[K_HAZE_AMOUNT];
    light.bloom = value[K_BLOOM];
    light.daylight = value[K_DAYLIGHT];
    light.glow = value[K_GLOW];
    light.rays = value[K_RAYS];
    neutral = light.rgb[0] == 1.0f && light.rgb[1] == 1.0f && light.rgb[2] == 1.0f;
    if (!neutral && light.daylight >= 1.0f)
        light.daylight = nextafterf(1.0f, 0.0f);
    RayGeometry(&light, (minute - 240.0f) / 960.0f);
    return light;
}

/* A little more than before, and bluer: it lifts the darkest surfaces to a
 * sky-lit blue at night while the greens, scaled by the night's own tint,
 * stay readable. */
#define CTR_DAYNIGHT_MOON_AMOUNT 0.10f
#define CTR_DAYNIGHT_MOON_RGB {56.0f / 255.0f, 84.0f / 255.0f, 150.0f / 255.0f}

CtrDayNightBlend CtrDayNight_Moonlight(const CtrDayNightLight *light, float fade,
                                     const float fadeRgb[3])
{
    CtrDayNightBlend blend = {CTR_DAYNIGHT_MOON_RGB, 0.0f};
    float moon = CTR_DAYNIGHT_MOON_AMOUNT * light->glow * light->glow;
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
        float altitude;

        SunPath(progress, &sun.dx, &sun.dz, &altitude);
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

    sLight = (CtrDayNightLight)CTR_DAYNIGHT_NEUTRAL;
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
    sLight = (CtrDayNightLight)CTR_DAYNIGHT_NEUTRAL;
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
