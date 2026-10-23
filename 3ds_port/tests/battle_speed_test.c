/* The battle scene's own speed (CtrSettings_BattleSpeedHalves): it follows
 * the overworld's SPEED until L or R sets one, then keeps it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <3ds.h>
#include "../src/3ds_settings.c"

void CtrDayNight_Update(void) {}
FILE *CtrFs_OpenData(const char *name, const char *mode) { (void)name; (void)mode; return NULL; }
void CtrLog_Write(CtrLogCategory category, const char *fmt, ...) { (void)category; (void)fmt; }

int main(void)
{
    char text[1024];

    CtrSettings_Freeze(); /* nothing is written */
    CtrSettings_Defaults();

    /* A new card: the battle follows the overworld, whatever it is set to. */
    assert(CtrSettings_BattleSpeedHalves() == 2);
    CtrSettings_ApplyLine("speed=3");
    assert(CtrSettings_SpeedHalves() == 6 && CtrSettings_BattleSpeedHalves() == 6);
    CtrSettings_ApplyLine("speed=1");

    /* At 1x, L has nowhere to go and sets nothing: still following. */
    CtrSettings_StepBattleSpeed(-1, false);
    assert(CtrSettings_GetInt("battle_speed", 0) == 0);

    /* R steps 1x, 1.5x, 2x, 3x, 4x and stops there; the overworld stays. */
    static const int halves[] = {3, 4, 6, 8, 8, 8};
    for (unsigned i = 0; i < sizeof(halves) / sizeof(halves[0]); ++i)
    {
        CtrSettings_StepBattleSpeed(1, false);
        assert(CtrSettings_BattleSpeedHalves() == halves[i]);
    }
    assert(CtrSettings_SpeedHalves() == 2);

    /* It keeps its own speed when the overworld changes, and L steps down. */
    CtrSettings_ApplyLine("speed=4");
    assert(CtrSettings_BattleSpeedHalves() == 8);
    CtrSettings_StepBattleSpeed(-1, false);
    assert(CtrSettings_BattleSpeedHalves() == 6);
    CtrSettings_StepBattleSpeed(-1, false);
    CtrSettings_StepBattleSpeed(-1, false);
    CtrSettings_StepBattleSpeed(-1, false);
    assert(CtrSettings_BattleSpeedHalves() == 2);
    CtrSettings_StepBattleSpeed(-1, false);
    assert(CtrSettings_BattleSpeedHalves() == 2);

    /* settings.txt round trip, and a value that is not a speed falls back to
     * the overworld's. */
    CtrSettings_ApplyLine("speed=2");
    CtrSettings_ApplyLine("battle_speed=6");
    assert(CtrSettings_BattleSpeedHalves() == 6);
    assert(CtrSettings_Text(text, sizeof(text)) > 0 && strstr(text, "battle_speed=6\n"));
    CtrSettings_ApplyLine("battle_speed=5");
    assert(CtrSettings_BattleSpeedHalves() == 4);

    /* With L=A, R alone goes round: 1x, 1.5x, 2x, 3x, 4x, 1x ... */
    CtrSettings_ApplyLine("battle_speed=6");
    static const int round[] = {8, 2, 3, 4, 6};
    for (unsigned i = 0; i < sizeof(round) / sizeof(round[0]); ++i)
    {
        CtrSettings_StepBattleSpeed(1, true);
        assert(CtrSettings_BattleSpeedHalves() == round[i]);
    }

    /* A scenario starts from the defaults: following again. */
    CtrSettings_Defaults();
    assert(CtrSettings_BattleSpeedHalves() == 2);
    puts("PASS battle speed: follows the overworld, own steps 1x-4x, going round, kept, round trip, defaults");
    return 0;
}
