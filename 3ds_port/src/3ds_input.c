#include <3ds.h>
#include <string.h>
#include "3ds_platform.h"

static CtrInput sInput;

void CtrInput_Clear(void)
{
    memset(&sInput, 0, sizeof(sInput));
}

static uint16_t sInjected;

void CtrInput_Inject(uint16_t buttons)
{
    sInjected |= buttons;
}

void CtrInput_Scan(void)
{
    static const uint32_t keys[] =
    {
        KEY_A, KEY_B, KEY_SELECT, KEY_START, KEY_DRIGHT,
        KEY_DLEFT, KEY_DUP, KEY_DDOWN, KEY_R, KEY_L, KEY_X, KEY_Y
    };
    CtrInputSample sample = {0};
    hidScanInput();
    uint32_t raw = hidKeysHeld();
    /* New 3DS: ZR and ZL step the fast-forward speed (OPTIONS has it too). */
    uint32_t down = hidKeysDown();
    /* Not while a measured scenario runs: it is recorded and played at 1x. */
    if ((down & KEY_ZR) && !CtrScenario_Active())
        CtrSettings_StepSpeed(1, false);
    if ((down & KEY_ZL) && !CtrScenario_Active())
        CtrSettings_StepSpeed(-1, false);
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (raw & keys[i])
            sample.buttons |= 1u << i;
    sample.buttons |= sInjected;
    sInjected = 0;
    circlePosition circle;
    hidCircleRead(&circle);
    sample.circleX = circle.dx;
    sample.circleY = circle.dy;
    sample.touchActive = (raw & KEY_TOUCH) != 0;
    if (sample.touchActive)
    {
        touchPosition touch;
        hidTouchRead(&touch);
        sample.touchX = touch.px;
        sample.touchY = touch.py;
    }
    CtrScenario_Input(&sample);
    CtrCycle_Input(&sample);
    CtrInput_Update(&sInput, &sample);
}

const CtrInput *CtrInput_Get(void)
{
    return &sInput;
}
