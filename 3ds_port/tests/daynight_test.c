#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "3ds_daynight.h"

static uint64_t sNow;
static unsigned sLocal = 12 * 3600;
static int sEnabled, sSpeed, sShowClock, sTimedEncounters;
static bool sOutdoor = true, sClockValid = true;
static int64_t sWall = 1700000000;

uint64_t CtrPlatform_Milliseconds(void) { return sNow; }
int64_t CtrPlatform_WallSeconds(void) { return sWall; }
bool CtrGame_IsOutdoor(void) { return sOutdoor; }
bool CtrGame_LocalTimeSeconds(unsigned *seconds)
{
    *seconds = sLocal;
    return sClockValid;
}
int CtrSettings_GetInt(const char *key, int fallback)
{
    if (strcmp(key, "day_night") == 0) return sEnabled;
    if (strcmp(key, "clock_speed") == 0) return sSpeed;
    if (strcmp(key, "show_clock") == 0) return sShowClock;
    if (strcmp(key, "timed_encounters") == 0) return sTimedEncounters;
    return fallback;
}
static unsigned sSetCalls;
void CtrSettings_SetInt(const char *key, int value)
{
    ++sSetCalls;
    if (strcmp(key, "clock_speed") == 0 && sSpeed != value)
    {
        CtrDayNight_Update();
        sSpeed = value;
    }
}

static void Near(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.00001f);
}

static void ClockTests(void)
{
    CtrDayNightClock clock = {0};
    const unsigned rates[] = {1, 6, 12, 24, 60, 120};

    for (unsigned index = 0; index < CTR_DAYNIGHT_SPEED_COUNT; ++index)
    {
        clock = (CtrDayNightClock){0};
        assert(CtrDayNight_Speed(index) == rates[index]);
        CtrDayNight_Advance(&clock, 100, 86399, index);
        CtrDayNight_Advance(&clock, 1100, 0, index);
        assert(clock.timeMs == (index == 0 ? 0 : (rates[index] - 1) * 1000));
    }
    assert(CtrDayNight_Speed(-1) == 1 && CtrDayNight_Speed(999) == 1);
    CtrDayNight_Advance(&clock, 2100, 0, 1);
    assert(clock.timeMs == 125000);
    CtrDayNight_Advance(&clock, 2200, 123, 0);
    assert(clock.timeMs == 125100);
    CtrDayNight_Advance(&clock, 2300, 123, 5);
    assert(clock.timeMs == 137100);
    CtrDayNight_Advance(&clock, 2300, 123, 5);
    assert(clock.timeMs == 137100);
    CtrDayNight_Advance(&clock, 1, 123, 5);
    assert(clock.timeMs == 137100);
    CtrDayNight_Advance(&clock, UINT64_MAX, 123, 5);
    assert(clock.timeMs < CTR_DAYNIGHT_DAY_MS);

    CtrDayNightClock slow = {0}, fast = {0};
    CtrDayNight_Advance(&slow, 0, 0, 4);
    CtrDayNight_Advance(&fast, 0, 0, 4);
    for (unsigned index = 1; index <= 1000; ++index)
        CtrDayNight_Advance(&fast, index, 0, 4);
    CtrDayNight_Advance(&slow, 1000, 0, 4);
    assert(slow.timeMs == fast.timeMs && slow.timeMs == 60000);
}

static void LightTests(void)
{
    const unsigned boundaries[] = {0, 150, 255, 315, 390, 480, 600, 900, 990, 1050, 1110, 1155, 1200, 1290};
    CtrDayNightLight day = CtrDayNight_At(12 * 3600000), night = CtrDayNight_At(0);
    CtrDayNightLight neutral = CtrDayNight_At(10 * 3600000);

    for (unsigned channel = 0; channel < 3; ++channel)
    {
        /* Noon is the original look, exactly, in every part of it. */
        assert(day.rgb[channel] == 1.0f && day.sun[channel] == 1.0f);
        assert(day.shade[channel] == 1.0f && day.haze[channel] == 1.0f);
        assert(day.rayRgb[channel] == 1.0f);
    }
    assert(day.daylight == 1.0f && day.glow == 0.0f);
    assert(day.hazeAmount == 1.0f && day.bloom == 1.0f);
    assert(neutral.rgb[0] == 1.0f && neutral.daylight == 1.0f);
    Near(night.daylight, 0.0f);
    Near(night.glow, 1.0f);
    assert(night.rgb[2] > night.rgb[0] && night.rays == 0.0f);
    for (unsigned index = 0; index < sizeof(boundaries) / sizeof(boundaries[0]); ++index)
    {
        unsigned time = boundaries[index] * 60000;
        CtrDayNightLight before = CtrDayNight_At(time + CTR_DAYNIGHT_DAY_MS - 1);
        CtrDayNightLight after = CtrDayNight_At(time);

        for (unsigned channel = 0; channel < 3; ++channel)
        {
            Near(before.rgb[channel], after.rgb[channel]);
            Near(before.sun[channel], after.sun[channel]);
            Near(before.shade[channel], after.shade[channel]);
        }
        Near(before.daylight, after.daylight);
        Near(before.glow, after.glow);
    }
    /* The day closes on itself: midnight has no seam, in value or slope. */
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        CtrDayNightLight behind = CtrDayNight_At(CTR_DAYNIGHT_DAY_MS - 60000);
        CtrDayNightLight at = CtrDayNight_At(0), ahead = CtrDayNight_At(60000);

        assert(fabsf((ahead.sun[channel] - at.sun[channel]) - (at.sun[channel] - behind.sun[channel])) < 0.01f);
    }
    for (unsigned minute = 0; minute < 1440; ++minute)
    {
        CtrDayNightLight light = CtrDayNight_At(minute * 60000);
        CtrDayNightLight next = CtrDayNight_At((minute + 1) * 60000);
        float lum;

        for (unsigned channel = 0; channel < 3; ++channel)
        {
            assert(light.rgb[channel] > 0 && light.rgb[channel] <= 1);
            /* The sun's tint stays under the ceiling the 1.30 texture saturation allows. */
            assert(light.sun[channel] > 0 && light.sun[channel] <= 1.04f + 0.0001f);
            assert(light.shade[channel] > 0 && light.shade[channel] <= 1.04f + 0.0001f);
            assert(light.haze[channel] >= 0 && isfinite(light.haze[channel]));
            assert(light.rayRgb[channel] > 0 && light.rayRgb[channel] <= 1);
            /* No visible step at 120X (a game minute is half a real second). */
            assert(fabsf(next.sun[channel] - light.sun[channel]) < 0.02f);
            assert(fabsf(next.shade[channel] - light.shade[channel]) < 0.02f);
            assert(fabsf(next.rgb[channel] - light.rgb[channel]) < 0.02f);
            assert(fabsf(next.haze[channel] - light.haze[channel]) < 0.25f);
        }
        assert(light.daylight >= 0 && light.daylight <= 1);
        assert(light.glow >= 0 && light.glow <= 1);
        assert(fabsf(next.glow - light.glow) < 0.02f && fabsf(next.daylight - light.daylight) < 0.02f);
        assert(light.rays >= 0 && light.rays <= 2.0f && fabsf(next.rays - light.rays) < 0.05f);
        assert(light.hazeAmount >= 1.0f && light.bloom >= 1.0f);
        /* obj.inc tints sprites when daylight is not 1, tiles.inc the world when rgb is not. */
        if (light.rgb[0] != 1.0f || light.rgb[1] != 1.0f || light.rgb[2] != 1.0f)
            assert(light.daylight != 1.0f);
        /* Night stays readable: the 2D tint's luminance never goes under the floor. */
        lum = 0.299f * light.rgb[0] + 0.587f * light.rgb[1] + 0.114f * light.rgb[2];
        assert(lum >= 0.50f);
        lum = 0.299f * light.shade[0] + 0.587f * light.shade[1] + 0.114f * light.shade[2];
        assert(lum >= 0.50f);
        /* The sun's rays come from the top edge, leaning the way the light
         * travels; the moon's fan (20:00-04:00) is narrower than any sun's. */
        assert(light.rayX > -0.1f && light.rayX < 1.1f && light.rayLength > 0.0f);
        assert(minute >= 1200u || minute < 240u ? light.rayWidth > 0.5f : light.rayWidth >= 1.0f);
    }
    /* Lamps come on before the dark and go off after dawn begins. */
    assert(CtrDayNight_At(17 * 3600000u + 30 * 60000u).glow > 0.0f);
    assert(CtrDayNight_At(19 * 3600000u).glow > 0.5f);
    assert(CtrDayNight_At(4 * 3600000u).glow == 1.0f);
    assert(CtrDayNight_At(5 * 3600000u).glow > 0.5f);
    assert(CtrDayNight_At(8 * 3600000u).glow == 0.0f);
    /* Golden hour is warmer than noon and its shadows cooler than its light. */
    CtrDayNightLight golden = CtrDayNight_At(17 * 3600000u + 30 * 60000u);

    assert(golden.sun[0] > golden.sun[2] + 0.3f && golden.shade[2] > golden.shade[0]);
    assert(golden.rays > day.rays);
    /* Sunrise light is a different colour from sunset's, and the rays' source
     * moves from the right (morning, sun in the east) to the left (evening). */
    CtrDayNightLight seven = CtrDayNight_At(7 * 3600000u), noon = CtrDayNight_At(12 * 3600000u);
    CtrDayNightLight seventeen = CtrDayNight_At(17 * 3600000u);

    assert(seven.rayX > 0.8f && seven.rayAngle < -30.0f);
    Near(noon.rayX, 0.5f);
    Near(noon.rayAngle, 0.0f);
    assert(noon.rayWidth > seven.rayWidth);
    assert(seventeen.rayX < 0.2f && seventeen.rayAngle > 30.0f);
    for (unsigned minute = 241; minute < 1200; ++minute)
        assert(CtrDayNight_At(minute * 60000u).rayX < CtrDayNight_At((minute - 1) * 60000u).rayX);
    /* Rays gone with the sun. */
    assert(CtrDayNight_At(21 * 3600000u).rays == 0.0f && CtrDayNight_At(2 * 3600000u).rays == 0.0f);
    /* The moon's faint shafts: only at night, never with the sun's (so the
     * source can move with nothing showing), fainter than the noon sun's,
     * cool, from the top right, and without a step. */
    assert(day.moonRays == 0.0f && neutral.moonRays == 0.0f);
    assert(night.moonRays > 0.0f && night.moonRays < 3.0f * day.rays);
    /* a narrower, longer fan than any of the sun's, and a deeper blue */
    assert(night.rayWidth < 1.0f && night.rayLength > 1.2f);
    assert(night.rayRgb[0] < 0.5f && night.rayRgb[1] < 0.7f && night.rayRgb[2] == 1.0f);
    assert(night.rayRgb[2] > night.rayRgb[0] && night.rayX > 0.6f && night.rayAngle < -20.0f);
    for (unsigned minute = 0; minute < 1440; ++minute)
    {
        CtrDayNightLight light = CtrDayNight_At(minute * 60000u);
        CtrDayNightLight next = CtrDayNight_At((minute + 1) * 60000u);

        assert(light.moonRays >= 0.0f && fabsf(next.moonRays - light.moonRays) < 0.05f);
        assert(light.rays == 0.0f || light.moonRays == 0.0f);
        /* within a quarter of an hour of the sun's, both are nothing */
        if (minute >= 1185 && minute < 1215)
            assert(light.moonRays == 0.0f);
        if (minute >= 240 && minute < 270)
            assert(light.moonRays == 0.0f);
    }
}

static void MoonlightTests(void)
{
    CtrDayNightLight night = CtrDayNight_At(0), day = CtrDayNight_At(12u * 3600000u);
    const float black[3] = {0.0f, 0.0f, 0.0f};
    const float white[3] = {1.0f, 1.0f, 1.0f};
    CtrDayNightBlend moon = CtrDayNight_Moonlight(&night, 0.0f, black);

    Near(moon.amount, 0.10f);
    assert(moon.rgb[2] > moon.rgb[1] && moon.rgb[1] > moon.rgb[0]);
    Near(CtrDayNight_Moonlight(&day, 0.0f, black).amount, 0.0f);
    const float originalNight[3] = {0.432f, 0.54f, 0.828f}; /* the night tint under the moon blend */
    for (unsigned colour = 0; colour < 32768u; ++colour)
        for (unsigned channel = 0; channel < 3; ++channel)
        {
            float source = ((colour >> (channel * 5u)) & 31u) / 31.0f;
            float result = source * night.rgb[channel] * (1.0f - moon.amount)
                         + moon.rgb[channel] * moon.amount;

            assert(result >= source * originalNight[channel] && result <= 1.0f);
        }
    for (unsigned minute = 0; minute < 1440u; ++minute)
    {
        CtrDayNightLight light = CtrDayNight_At(minute * 60000u);
        CtrDayNightLight previous = CtrDayNight_At(minute * 60000u + CTR_DAYNIGHT_DAY_MS - 1u);
        CtrDayNightBlend blend = CtrDayNight_Moonlight(&light, 0.0f, black);
        CtrDayNightBlend before = CtrDayNight_Moonlight(&previous, 0.0f, black);

        assert(blend.amount >= 0.0f && blend.amount <= 0.10f);
        Near(before.amount, blend.amount);
        for (unsigned step = 0; step <= 10u; ++step)
            for (unsigned target = 0; target < 2u; ++target)
            {
                float fade = step / 10.0f;
                const float *fadeRgb = target == 0 ? black : white;
                CtrDayNightBlend combined = CtrDayNight_Moonlight(&light, fade, fadeRgb);

                for (unsigned channel = 0; channel < 3u; ++channel)
                {
                    float source = 0.37f * light.rgb[channel];
                    float expected = (source * (1.0f - blend.amount)
                                   + blend.rgb[channel] * blend.amount) * (1.0f - fade)
                                   + fadeRgb[channel] * fade;
                    float actual = source * (1.0f - combined.amount)
                                 + combined.rgb[channel] * combined.amount;

                    Near(actual, expected);
                    if (step == 10u)
                        Near(actual, fadeRgb[channel]);
                }
            }
    }
}

static void SunTests(void)
{
    CtrDayNightSun morning = CtrDayNight_SunAt(6u * 3600000u);
    CtrDayNightSun noon = CtrDayNight_SunAt(12u * 3600000u);
    CtrDayNightSun evening = CtrDayNight_SunAt(18u * 3600000u);

    assert(morning.dx < -1.0f && evening.dx > 1.0f);
    Near(morning.dx, -evening.dx);
    Near(noon.dx, 0.0f);
    Near(noon.strength, 1.0f);
    assert(hypotf(noon.dx, noon.dz) < hypotf(morning.dx, morning.dz));
    assert(CtrDayNight_SunAt(20u * 3600000u).strength == 0.0f);
    assert(CtrDayNight_SunAt(4u * 3600000u).strength == 0.0f);
    assert(CtrDayNight_SunAt(240u * 60000u - 1u).key == 1u);
    assert(CtrDayNight_SunAt(6u * 3600000u + 900000u).key != morning.key);
    /* The moon casts at night: from its shafts' and its glitter's place
     * (up and to the right), faint, well under the sun's. */
    CtrDayNightSun midnight = CtrDayNight_SunAt(0);
    CtrDayNightLight midnightLight = CtrDayNight_At(0);

    Near(midnight.dx, CTR_DAYNIGHT_MOON_DX);
    Near(midnight.dz, CTR_DAYNIGHT_MOON_DZ);
    assert(midnight.dx < 0.0f && midnight.dz > 0.0f);
    assert(midnight.strength > 0.15f && midnight.strength < 0.45f);
    assert(midnight.strength < 0.5f * noon.strength);
    assert(midnight.key >= 100u && midnight.key != morning.key);
    /* the shafts come from the same place: the rays' source moves with dx
     * as the sun's does (0.38 screen widths per unit, RayGeometry) */
    assert(midnightLight.moonRays > 0.0f);
    Near(midnightLight.rayX, 1.0f); /* the moon's glow: from the right corner, along its shadows */
    /* its shadow a touch bluer than the moonlit ground, which keeps the
     * night's tint (and the 2D tint the night's own); the day untouched */
    assert(midnightLight.shade[0] < midnightLight.sun[0] && midnightLight.shade[1] < midnightLight.sun[1]);
    Near(midnightLight.shade[2], midnightLight.sun[2]);
    assert(midnightLight.shade[0] > 0.85f * midnightLight.sun[0]);
    Near(midnightLight.rgb[0], 0.48f);
    /* (the blue hour's key, before the moon: as it was) */
    for (unsigned channel = 0; channel < 3; ++channel)
        Near(CtrDayNight_At(1200u * 60000u).shade[channel], CtrDayNight_At(1200u * 60000u).sun[channel]);
    Near(CtrDayNight_At(12u * 3600000u).shade[0], 1.0f);
    /* the same strength all through the full night, one key: no rebuilds */
    for (unsigned minute = 1305; minute < 1440 + 150; ++minute)
    {
        CtrDayNightSun moon = CtrDayNight_SunAt(minute % 1440u * 60000u);

        assert(moon.key == midnight.key);
        Near(moon.strength, midnight.strength);
    }
    for (unsigned minute = 0; minute < 1440; ++minute)
    {
        CtrDayNightSun sun = CtrDayNight_SunAt(minute * 60000u);
        CtrDayNightSun wrapped = CtrDayNight_SunAt(minute * 60000u + CTR_DAYNIGHT_DAY_MS);

        assert(isfinite(sun.dx) && isfinite(sun.dz) && isfinite(sun.strength));
        assert(fabsf(sun.dx) <= 1.35f && sun.dz >= 0.35f && sun.dz <= 0.55f);
        assert(sun.strength >= 0.0f && sun.strength <= 1.0f);
        assert(sun.key == wrapped.key);
        Near(sun.dx, wrapped.dx);
    }
    assert(CtrDayNight_SunAt(6u * 3600000u + 899999u).key == morning.key);
    /*
     * The switches. The light is the sun's (keys 2..81) or the moon's (1,
     * 100+); wherever one gives way to the other the moon casts nothing,
     * and the sun's last step at dusk is its faintest (it goes from there to
     * nothing at 20:00, as it always has), with a whole quarter of an hour
     * of nothing cast between the two: the place changes with no shadow
     * drawn from it. Within a step the key, the place and the strength never
     * change; a new strength is a new key, at most every quarter of an hour.
     */
    {
        unsigned nightKeys = 0, lastSun = 0, firstSun = 1440;

        for (unsigned minute = 0; minute < 1440; ++minute)
        {
            CtrDayNightSun sun = CtrDayNight_SunAt(minute * 60000u);
            CtrDayNightSun next = CtrDayNight_SunAt((minute + 1u) % 1440u * 60000u);
            bool moon = sun.key == 1u || sun.key >= 100u, nextMoon = next.key == 1u || next.key >= 100u;

            if (moon != nextMoon)
                assert((moon ? sun : next).strength == 0.0f
                       && (moon ? next : sun).strength < 0.1f);
            if (moon)
            {
                Near(sun.dx, CTR_DAYNIGHT_MOON_DX);
                Near(sun.dz, CTR_DAYNIGHT_MOON_DZ);
                assert(sun.strength <= midnight.strength);
                /* never in the sun's dark minutes either side */
                assert(minute < 240u || minute >= 1200u);
            }
            else if (sun.strength > 0.0f)
            {
                if (minute < firstSun)
                    firstSun = minute;
                lastSun = minute;
            }
            if (sun.key == next.key)
            {
                Near(sun.strength, next.strength);
                Near(sun.dx, next.dx);
            }
            else
            {
                assert((minute + 1u) % 15u == 0u);
                if (moon || nextMoon)
                    ++nightKeys;
            }
            /* the moon's shadows inside the gap the sun's leave */
            if (moon && sun.strength > 0.0f)
                assert((minute >= 1200u + 30u) || (minute < 240u - 15u));
        }
        /* sun in at 04:15, out after 19:59; moon from 20:30 to 03:44 */
        assert(firstSun == 255u && lastSun == 1199u);
        assert(CtrDayNight_SunAt(1229u * 60000u).strength == 0.0f);
        assert(CtrDayNight_SunAt(1230u * 60000u).strength > 0.0f);
        assert(CtrDayNight_SunAt(224u * 60000u).strength > 0.0f);
        assert(CtrDayNight_SunAt(225u * 60000u).strength == 0.0f);
        /* 20:00 (sun out), 20:30..21:45 (six in), 02:30..03:45 (six out),
         * 04:00 (sun in) */
        assert(nightKeys == 14u);
    }
}

static void RuntimeTests(void)
{
    unsigned seconds, rate;

    CtrDayNight_Reset();
    CtrDayNight_Update();
    Near(CtrDayNight_Get()->rgb[0], 1);
    assert(CtrDayNight_GetSun()->key == 0);
    sEnabled = 1;
    sLocal = 0;
    CtrDayNight_Update();
    Near(CtrDayNight_Get()->rgb[0], 0.48f);
    /* midnight: the moon casts */
    assert(CtrDayNight_GetSun()->key == CtrDayNight_SunAt(0).key && CtrDayNight_GetSun()->key >= 100u);
    assert(CtrDayNight_GetSun()->strength > 0.0f && CtrDayNight_GetSun()->strength < 0.5f);
    sOutdoor = false;
    CtrDayNight_Update();
    Near(CtrDayNight_Get()->rgb[0], 1);
    Near(CtrDayNight_Get()->glow, 0);
    assert(CtrDayNight_GetSun()->key == 0 && CtrDayNight_GetSun()->strength == 1);
    sOutdoor = true;
    sSpeed = 5;
    sNow = 360000;
    CtrDayNight_Update();
    Near(CtrDayNight_Get()->rgb[0], 1);
    sNow += 3600000;
    CtrDayNight_Resume();
    CtrDayNight_Update();
    Near(CtrDayNight_Get()->rgb[0], 1);
    sSpeed = 0;
    sNow += 1000;
    CtrDayNight_Update();
    Near(CtrDayNight_Get()->rgb[0], 1.0f);
    sEnabled = 99;
    CtrDayNight_Update();
    Near(CtrDayNight_Get()->rgb[0], 1);
    Near(CtrDayNight_Get()->glow, 0);
    sEnabled = 1;
    sClockValid = false;
    sNow += 1000;
    CtrDayNight_Update();
    Near(CtrDayNight_Get()->rgb[0], 1);
    Near(CtrDayNight_Get()->glow, 0);
    sClockValid = true;
    sSpeed = -1;
    CtrDayNight_Update();
    Near(CtrDayNight_Get()->rgb[0], 1.0f);
    sEnabled = 0;
    CtrDayNight_Reset();
    CtrDayNight_Update();
    assert(!CtrDayNight_Time(&seconds, &rate));
    assert(!CtrDayNight_Active());
    sShowClock = 1;
    sLocal = 9 * 3600;
    sSpeed = 5;
    assert(CtrDayNight_Time(&seconds, &rate));
    assert(seconds == sLocal && rate == 120);
    assert(CtrDayNight_Active());
    Near(CtrDayNight_Get()->rgb[0], 1);
    sNow += 30000;
    assert(CtrDayNight_Time(&seconds, &rate));
    assert(seconds == 10 * 3600);
    sShowClock = 0;
    sTimedEncounters = 1;
    sNow += 30000;
    assert(CtrDayNight_Time(&seconds, &rate));
    assert(seconds == 11 * 3600);
    Near(CtrDayNight_Get()->rgb[0], 1);
    sNow += 3600000;
    CtrDayNight_Resume();
    assert(CtrDayNight_Time(&seconds, &rate));
    assert(seconds == 11 * 3600);
    sClockValid = false;
    sNow += 1000;
    assert(!CtrDayNight_Time(&seconds, &rate));
    sNow += 30000;
    sClockValid = true;
    assert(CtrDayNight_Time(&seconds, &rate));
    assert(seconds == 11 * 3600);
    sTimedEncounters = 0;
    sShowClock = 99;
    assert(CtrDayNight_Time(&seconds, &rate));
    sShowClock = 1;
    assert(CtrDayNight_Time(&seconds, &rate));
    assert(seconds == 11 * 3600);
    sSpeed = 0;
    sLocal = 21 * 3600;
    sNow += 1000;
    assert(CtrDayNight_Time(&seconds, &rate));
    assert(seconds == 11 * 3600 + 1 && rate == 1);
}

static void RuntimeSunTests(void)
{
    CtrDayNight_Reset();
    sSpeed = 0;
    sEnabled = sShowClock = sTimedEncounters = 0;
    sNow += 1000;
    CtrDayNight_Update();
    sEnabled = sShowClock = 1;
    sOutdoor = sClockValid = true;
    sSpeed = 5;
    sLocal = 6u * 3600u;
    CtrDayNight_Update();
    CtrDayNightSun morning = *CtrDayNight_GetSun();

    Near(morning.dx, CtrDayNight_SunAt(6u * 3600000u).dx);
    sNow += 7499;
    CtrDayNight_Update();
    assert(CtrDayNight_GetSun()->key == morning.key);
    sNow += 1;
    CtrDayNight_Update();
    assert(CtrDayNight_GetSun()->key == morning.key + 1);
    unsigned key = CtrDayNight_GetSun()->key;
    sNow += 3600000;
    CtrDayNight_Resume();
    CtrDayNight_Update();
    assert(CtrDayNight_GetSun()->key == key);
    sEnabled = 0;
    CtrDayNight_Update();
    assert(CtrDayNight_GetSun()->key == 0);
    Near(CtrDayNight_GetSun()->dx, 0.85f);
    sEnabled = 1;
    sOutdoor = false;
    CtrDayNight_Update();
    assert(CtrDayNight_GetSun()->key == 0);
    sOutdoor = true;
    sClockValid = false;
    sNow += 1000;
    CtrDayNight_Update();
    assert(CtrDayNight_GetSun()->key == 0);
}

static void BerryClockTests(void)
{
    CtrDayNightSnapshot snapshot;
    unsigned seconds, rate;

    CtrDayNight_Reset();
    sEnabled = sShowClock = sTimedEncounters = 0;
    sOutdoor = sClockValid = true;
    sSpeed = 5;
    sLocal = 23u * 3600u;
    sNow = 0;
    assert(CtrDayNight_Time(&seconds, &rate));
    assert(seconds == sLocal && rate == 120);
    sNow = 60000;
    assert(CtrDayNight_BerryDue());
    assert(CtrDayNight_TakeBerryMinutes() == 119);
    assert(CtrDayNight_TakeBerryMinutes() == 0);
    assert(CtrDayNight_Time(&seconds, &rate) && seconds == 3600);
    sNow += 250;
    assert(CtrDayNight_Snapshot(&snapshot));
    assert(snapshot.berryBonusMs == 29750);
    sSpeed = 1;
    sNow += 1000;
    assert(CtrDayNight_Snapshot(&snapshot));
    assert(snapshot.berryBonusMs == 34750);
    assert(snapshot.timeMs == 3636000);
    sSpeed = 0;
    sNow += 1000;
    assert(CtrDayNight_Snapshot(&snapshot));
    assert(snapshot.timeMs == 3637000 && snapshot.berryBonusMs == 34750);
    sWall += 3 * 86400 + 60;
    assert(CtrDayNight_Restore(&snapshot));
    assert(CtrDayNight_Time(&seconds, &rate) && seconds == 3697 && rate == 1);
    assert(CtrDayNight_Snapshot(&snapshot) && snapshot.berryBonusMs == 34750);
    sWall += 3600;
    sNow += 3600000;
    CtrDayNight_Resume();
    assert(CtrDayNight_Time(&seconds, &rate) && seconds == 7297);
    assert(CtrDayNight_Snapshot(&snapshot) && snapshot.berryBonusMs == 34750);
    sSpeed = 5;
    sNow += 1000;
    sClockValid = false;
    assert(CtrDayNight_TakeBerryMinutes() == 0);
    sNow += 60000;
    sClockValid = true;
    assert(CtrDayNight_TakeBerryMinutes() == 0);
    sNow += 1000;
    assert(CtrDayNight_TakeBerryMinutes() == 2);
    CtrDayNightClock clock = {0};
    CtrDayNight_Advance(&clock, 0, 0, 5);
    CtrDayNight_Advance(&clock, 2u * CTR_DAYNIGHT_DAY_MS, 0, 5);
    assert(clock.berryBonusMs == (uint64_t)2u * CTR_DAYNIGHT_DAY_MS * 119u);
    assert(clock.timeMs == 0);
    CtrDayNight_Advance(&clock, UINT64_MAX, 0, 5);
    assert(clock.berryBonusMs == CTR_DAYNIGHT_BONUS_LIMIT_MS);
    snapshot.timeMs = CTR_DAYNIGHT_DAY_MS;
    assert(!CtrDayNight_Restore(&snapshot));
    CtrDayNight_Reset();
    sSpeed = 0;
    assert(!CtrDayNight_BerryDue());
    assert(CtrDayNight_TakeBerryMinutes() == 0);
}

static void SyncHelperTests(void)
{
    CtrDayNightClock clock = {0}, fresh = {0};

    CtrDayNight_Advance(&clock, 0, 3600, 1);
    CtrDayNight_Advance(&clock, 1000, 3600, 1);
    assert(clock.shifted && clock.timeMs == 3606000 && clock.berryBonusMs == 5000);
    /* The last second is still credited at 6X, then the phase goes. */
    CtrDayNight_Sync(&clock, 2000, 7200, 1);
    assert(!clock.shifted && clock.ready && clock.lastMs == 2000);
    assert(clock.timeMs == 7200000 && clock.berryBonusMs == 10000);
    CtrDayNight_Advance(&clock, 3000, 7201, 0);
    assert(clock.timeMs == 7201000 && clock.berryBonusMs == 10000);
    /* Monotonic time going backwards credits nothing. */
    CtrDayNight_Sync(&clock, 1000, 86400 + 5, 5);
    assert(clock.timeMs == 5000 && clock.berryBonusMs == 10000 && clock.lastMs == 1000);
    CtrDayNight_Sync(&fresh, 500, 86399, 5);
    assert(fresh.ready && !fresh.shifted && fresh.timeMs == 86399000 && fresh.berryBonusMs == 0);
}

static void SyncRuntimeTests(void)
{
    CtrDayNightSnapshot snapshot;
    unsigned seconds, rate, calls;

    CtrDayNight_Reset();
    sEnabled = 1;
    sShowClock = sTimedEncounters = 0;
    sOutdoor = sClockValid = true;
    sSpeed = 5;
    sLocal = 10u * 3600u;
    sNow = 0;
    assert(CtrDayNight_Time(&seconds, &rate) && seconds == 10u * 3600u && rate == 120);
    sNow = 30000;
    assert(CtrDayNight_Time(&seconds, &rate) && seconds == 11u * 3600u);
    Near(CtrDayNight_Get()->rgb[0], 1.0f);

    /* An unreadable RTC leaves clock and settings as they were. */
    sClockValid = false;
    sNow = 31000;
    calls = sSetCalls;
    assert(!CtrDayNight_SyncToConsole());
    assert(sSpeed == 5 && sSetCalls == calls);
    sClockValid = true;
    assert(CtrDayNight_Snapshot(&snapshot) && snapshot.shifted);
    assert(snapshot.timeMs == 39720000u && snapshot.berryBonusMs == 3689000u);

    /* Sync just before midnight: the last second still earns bonus at 120X. */
    sNow = 32000;
    sLocal = 23u * 3600u + 59u * 60u + 30u;
    assert(CtrDayNight_SyncToConsole());
    assert(sSpeed == 0 && sSetCalls == calls + 1);
    Near(CtrDayNight_Get()->rgb[0], 0.48f);
    assert(CtrDayNight_Snapshot(&snapshot) && !snapshot.shifted);
    assert(snapshot.timeMs == 86370000u && snapshot.berryBonusMs == 3808000u);

    /* From then on it follows the local clock: midnight, then backwards. */
    sNow = 62000;
    sLocal = 30;
    assert(CtrDayNight_Time(&seconds, &rate) && seconds == 30 && rate == 1);
    sNow = 63000;
    sLocal = 5u * 3600u;
    assert(CtrDayNight_Time(&seconds, &rate) && seconds == 5u * 3600u);
    assert(CtrDayNight_Snapshot(&snapshot) && !snapshot.shifted);
    assert(snapshot.berryBonusMs == 3808000u);

    /* Saved and loaded unshifted: still the local clock, bonus intact. */
    sWall += 7200;
    assert(CtrDayNight_Restore(&snapshot));
    sLocal = 6u * 3600u;
    sNow = 64000;
    assert(CtrDayNight_Time(&seconds, &rate) && seconds == 6u * 3600u && rate == 1);
    assert(CtrDayNight_Snapshot(&snapshot) && !snapshot.shifted && snapshot.berryBonusMs == 3808000u);
    assert(CtrDayNight_TakeBerryMinutes() == 63);

    /* Time asleep is not credited at the old speed by a sync on resume. */
    sSpeed = 5;
    sNow = 65000;
    assert(CtrDayNight_Time(&seconds, &rate) && rate == 120);
    assert(CtrDayNight_Snapshot(&snapshot) && snapshot.shifted && snapshot.berryBonusMs == 147000u);
    CtrDayNight_Resume();
    sNow += 3600000;
    sLocal = 7u * 3600u;
    assert(CtrDayNight_SyncToConsole() && sSpeed == 0);
    assert(CtrDayNight_Snapshot(&snapshot) && !snapshot.shifted);
    assert(snapshot.timeMs == 7u * 3600000u && snapshot.berryBonusMs == 147000u);
    assert(CtrDayNight_Time(&seconds, &rate) && seconds == 7u * 3600u && rate == 1);

    /* With every clock option off the bonus alone keeps the clock running
     * until the field takes it. */
    sEnabled = 0;
    sNow += 1000;
    assert(CtrDayNight_Active());
    CtrDayNight_Reset();
    sNow += 1000;
    assert(!CtrDayNight_Active());
}

int main(void)
{
    ClockTests();
    LightTests();
    MoonlightTests();
    SunTests();
    RuntimeTests();
    RuntimeSunTests();
    BerryClockTests();
    SyncHelperTests();
    SyncRuntimeTests();
    puts("PASS day/night: rates, rollover, continuity, moonlight, all 32768 colours, fade composition, solar path, bonus berry minutes, fractions, restore, offline 1X, resume, invalid settings, clock sync");
    return 0;
}
