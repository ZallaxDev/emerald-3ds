#include "3ds_input.h"

static uint16_t CircleDirection(int x, int y, uint16_t previous)
{
    unsigned ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    unsigned threshold = previous ? CTR_CIRCLE_LEAVE : CTR_CIRCLE_ENTER;
    if (ax * ax + ay * ay < threshold * threshold) return 0;
    bool horizontal = ax > ay;
    /* Keep the selected axis near diagonals; opposite sign still changes
     * direction immediately. Positive HID Y points up. */
    if (previous & (CTR_KEY_LEFT | CTR_KEY_RIGHT))
        horizontal = ay <= ax + CTR_CIRCLE_AXIS_HYSTERESIS;
    else if (previous & (CTR_KEY_UP | CTR_KEY_DOWN))
        horizontal = ax > ay + CTR_CIRCLE_AXIS_HYSTERESIS;
    return horizontal ? (x < 0 ? CTR_KEY_LEFT : CTR_KEY_RIGHT)
                      : (y < 0 ? CTR_KEY_DOWN : CTR_KEY_UP);
}

void CtrInput_Update(CtrInput *state, const CtrInputSample *sample)
{
    uint16_t previous = state->held;
    uint16_t buttons = sample->buttons & 0xfff;
    state->physicalDown = buttons & ~state->physicalHeld;
    state->physicalUp = state->physicalHeld & ~buttons;
    state->physicalHeld = buttons;
    for (unsigned i = 0; i < 12; ++i)
        if (state->physicalDown & (1u << i)) ++state->presses[i];
    state->circleX = sample->circleX;
    state->circleY = sample->circleY;
    state->circleDirection = CircleDirection(sample->circleX, sample->circleY, state->circleDirection);
    state->held = buttons & CTR_KEY_GAME;
    /* Explicit D-pad input takes precedence over the stick. This avoids
     * manufacturing a diagonal/opposite pair by combining both sources. */
    if (!(buttons & CTR_KEY_DPAD)) state->held |= state->circleDirection;
    state->down = state->held & ~previous;
    state->up = previous & ~state->held;
    state->touchDown = sample->touchActive && !state->touchActive;
    state->touchUp = state->touchActive && !sample->touchActive;
    state->touchActive = sample->touchActive;
    state->touchX = state->touchActive ? (sample->touchX < 320 ? sample->touchX : 319) : 0;
    state->touchY = state->touchActive ? (sample->touchY < 240 ? sample->touchY : 239) : 0;
    state->touchNormX = state->touchX / 319.0f;
    state->touchNormY = state->touchY / 239.0f;
}
