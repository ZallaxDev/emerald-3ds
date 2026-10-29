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
    const unsigned boundaries[] = {0, 120, 240, 480, 600, 900, 1020, 1080, 1200, 1440};
    CtrDayNightLight day = CtrDayNight_At(12 * 3600000), night = CtrDayNight_At(0);

    Near(day.rgb[0], 1.0f);
    Near(day.rgb[1], 1.0f);
    Near(day.rgb[2], 1.0f);
    Near(day.daylight, 1.0f);
    Near(night.daylight, 0.0f);
    Near(day.glow, 0.0f);
    Near(night.glow, 1.0f);
    assert(night.rgb[2] > night.rgb[0]);
    for (unsigned index = 0; index < sizeof(boundaries) / sizeof(boundaries[0]); ++index)
    {
        unsigned time = boundaries[index] * 60000;
        CtrDayNightLight before = CtrDayNight_At(time + CTR_DAYNIGHT_DAY_MS - 1);
        CtrDayNightLight after = CtrDayNight_At(time);

        for (unsigned channel = 0; channel < 3; ++channel)
            Near(before.rgb[channel], after.rgb[channel]);
        Near(before.daylight, after.daylight);
        Near(before.glow, after.glow);
        CtrDayNightLight ahead = CtrDayNight_At(time + 1000);
        CtrDayNightLight behind = CtrDayNight_At(time + CTR_DAYNIGHT_DAY_MS - 1000);

        for (unsigned channel = 0; channel < 3; ++channel)
            assert(fabsf(ahead.rgb[channel] - behind.rgb[channel]) < 0.000001f);
    }
    for (unsigned minute = 0; minute < 1440; ++minute)
    {
        CtrDayNightLight light = CtrDayNight_At(minute * 60000);

        for (unsigned channel = 0; channel < 3; ++channel)
            assert(light.rgb[channel] > 0 && light.rgb[channel] <= 1);
        assert(light.daylight >= 0 && light.daylight <= 1);
        assert(light.glow >= 0 && light.glow <= 1);
    }
    CtrDayNightLight early = CtrDayNight_At(121 * 60000);
    CtrDayNightLight middle = CtrDayNight_At(180 * 60000);

    assert(early.rgb[0] - night.rgb[0] < 0.001f);
    Near(middle.rgb[0], (night.rgb[0] + 0.96f) * 0.5f);
    Near(middle.glow, 0.5f);
}

static void MoonlightTests(void)
{
    CtrDayNightLight night = CtrDayNight_At(0), day = CtrDayNight_At(12u * 3600000u);
    const float black[3] = {0.0f, 0.0f, 0.0f};
    const float white[3] = {1.0f, 1.0f, 1.0f};
    CtrDayNightBlend moon = CtrDayNight_Moonlight(&night, 0.0f, black);

    Near(moon.amount, 0.06f);
    assert(moon.rgb[2] > moon.rgb[1] && moon.rgb[1] > moon.rgb[0]);
    Near(CtrDayNight_Moonlight(&day, 0.0f, black).amount, 0.0f);
    const float originalNight[3] = {0.55f, 0.62f, 0.82f};
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

        assert(blend.amount >= 0.0f && blend.amount <= 0.06f);
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
    assert(CtrDayNight_SunAt(0).strength == 0.0f);
    assert(CtrDayNight_SunAt(20u * 3600000u).strength == 0.0f);
    assert(CtrDayNight_SunAt(4u * 3600000u).strength == 0.0f);
    assert(CtrDayNight_SunAt(240u * 60000u - 1u).key == 1u);
    assert(CtrDayNight_SunAt(6u * 3600000u + 900000u).key != morning.key);
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
    Near(CtrDayNight_Get()->rgb[0], 0.66f);
    assert(CtrDayNight_GetSun()->key == 1 && CtrDayNight_GetSun()->strength == 0);
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
    Near(CtrDayNight_Get()->rgb[0], 0.66f);
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
