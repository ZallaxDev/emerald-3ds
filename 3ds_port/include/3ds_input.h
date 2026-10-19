#ifndef CTR_INPUT_H
#define CTR_INPUT_H
#include <stdbool.h>
#include <stdint.h>

/* First ten bits exactly match GBA. X/Y are native extras, never game bits. */
enum {
    CTR_KEY_A = 1 << 0, CTR_KEY_B = 1 << 1,
    CTR_KEY_SELECT = 1 << 2, CTR_KEY_START = 1 << 3,
    CTR_KEY_RIGHT = 1 << 4, CTR_KEY_LEFT = 1 << 5,
    CTR_KEY_UP = 1 << 6, CTR_KEY_DOWN = 1 << 7,
    CTR_KEY_R = 1 << 8, CTR_KEY_L = 1 << 9,
    CTR_KEY_X = 1 << 10, CTR_KEY_Y = 1 << 11,
    CTR_KEY_DPAD = 0xf0, CTR_KEY_GAME = 0x3ff,
    CTR_CIRCLE_ENTER = 40, CTR_CIRCLE_LEAVE = 28,
    CTR_CIRCLE_AXIS_HYSTERESIS = 12
};

typedef struct {
    uint16_t buttons;
    int16_t circleX, circleY;
    bool touchActive;
    uint16_t touchX, touchY;
} CtrInputSample;

typedef struct {
    uint16_t held, down, up; /* GBA buttons + cardinal Circle Pad. */
    uint16_t physicalHeld, physicalDown, physicalUp;
    uint16_t lastDown, lastUp, circleDirection;
    uint32_t presses[12];
    bool touchActive, touchDown, touchUp;
    uint16_t touchX, touchY; /* bottom pixels: 0..319, 0..239 */
    float touchNormX, touchNormY; /* 0..1, zero when not touching */
    int16_t circleX, circleY;
} CtrInput;

/* Pure conversion shared by HID and host regression tests. */
void CtrInput_Update(CtrInput *state, const CtrInputSample *sample);
void CtrInput_Scan(void);
void CtrInput_Clear(void);
/* Stops what the scan started (the ir:rst service of the second stick). */
void CtrInput_Shutdown(void);
const CtrInput *CtrInput_Get(void);
#endif
