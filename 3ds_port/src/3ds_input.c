#include <3ds.h>
#include <string.h>
#include "3ds_platform.h"
#include "3ds_freecam.h"

static CtrInput sInput;

#if CTR_VOXEL_ENABLED
/*
 * The second stick: the New 3DS's C-Stick and the Old 3DS's Circle Pad Pro
 * are both read through the ir:rst service, which has to be started (without
 * it the read is always zero - the Circle Pad Pro "did nothing"). On a console
 * with neither it reads zero. A New 3DS C-Stick that rehid
 * (github.com/hax0kartik/rehid) or another HID replacement maps to keys gives
 * KEY_CSTICK_* held instead, which is used when the analog read is at rest.
 */
static bool sIrrst, sIrrstTried;

static void IrrstStart(void)
{
    if (sIrrstTried)
        return;
    sIrrstTried = true;
    sIrrst = R_SUCCEEDED(irrstInit());
}

void CtrInput_Shutdown(void)
{
    if (sIrrst)
        irrstExit();
    sIrrst = false;
    sIrrstTried = false;
}
#else
void CtrInput_Shutdown(void) {}
#endif

void CtrInput_Clear(void)
{
    memset(&sInput, 0, sizeof(sInput));
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
    circlePosition circle;
    hidCircleRead(&circle);
    sample.circleX = circle.dx;
    sample.circleY = circle.dy;
#if CTR_VOXEL_ENABLED
    {
        /* The free-roam camera's look (OPTIONS' CAMERA, 3ds_freecam.h): the
         * C-Stick aims it, L and R turn it where there is none. Not through
         * the sample, which a scenario records and the game would see. */
        circlePosition stick = {0, 0};
        float turn = ((raw & KEY_R) ? 1.0f : 0.0f) - ((raw & KEY_L) ? 1.0f : 0.0f);

        IrrstStart();
        if (sIrrst)
        {
            irrstScanInput();
            irrstCstickRead(&stick);
        }
        /* Keys for the stick's four directions, where the analog read is at
         * rest: a full push each. */
        if (stick.dx == 0 && stick.dy == 0)
        {
            stick.dx = ((raw & KEY_CSTICK_RIGHT) ? 156 : 0) - ((raw & KEY_CSTICK_LEFT) ? 156 : 0);
            stick.dy = ((raw & KEY_CSTICK_UP) ? 156 : 0) - ((raw & KEY_CSTICK_DOWN) ? 156 : 0);
        }
        if (CtrScenario_Active())
            stick.dx = stick.dy = 0, turn = 0.0f;
        CtrFreeCam_SetLook((float)stick.dx / 156.0f, (float)stick.dy / 156.0f, turn);
    }
#endif
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
