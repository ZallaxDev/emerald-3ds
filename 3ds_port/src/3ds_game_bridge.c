#include <stdlib.h>
#include "global.h"
#include "3ds_platform.h"
#include "3ds_audio.h"
#include "3ds_emu_regs.h"
#include "3ds_bottom.h"
#ifdef CTR_TEST_SWOOP
#include "main.h"
#include "overworld.h"
#endif
#include "siirtc.h"
#include "gba/flash_internal.h"

static time_t sClockOffset;
static u8 sRtcStatus = SIIRTCINFO_24HOUR;

void CtrEmu_Reset(void)
{
    memset(REG_BASE, 0, 0x400);
    memset(PLTT, 0, PLTT_SIZE);
    memset(VRAM_, 0, VRAM_SIZE);
    memset(OAM, 0, OAM_SIZE);
    REG_IME = 1;
    REG_KEYINPUT = KEYS_MASK;
    INTR_CHECK = 0;
    INTR_VECTOR = NULL;
}

void CtrEmu_BeginVBlank(void)
{
    REG_VCOUNT = 160;
    REG_DISPSTAT |= DISPSTAT_VBLANK;
    REG_IF |= INTR_FLAG_VBLANK;
}

void CtrEmu_EndVBlank(void)
{
    REG_DISPSTAT &= ~DISPSTAT_VBLANK;
    REG_IF &= ~INTR_FLAG_VBLANK;
    REG_VCOUNT = 0;
}

u16 Platform_GetKeyInput(void)
{
    const CtrInput *input = CtrInput_Get();
    u16 held = input->held;

    /* Y is the 3DS's own SELECT: the registered item, as the bottom screen's
     * Y button says. */
    if (input->physicalHeld & CTR_KEY_Y)
        held |= SELECT_BUTTON;
    /* In the battle scene L slows and R speeds the battle up, and the game
     * sees neither. With the game's L=A button mode L stays A, and R alone
     * goes round the speeds, 4x back to 1x. Not while START and SELECT are
     * down: L+R+START+SELECT is the soft reset. Once per frame, whatever the
     * number of reads. */
    if (CtrPlatform_BattleSpeedKeys()
        && (held & (START_BUTTON | SELECT_BUTTON)) != (START_BUTTON | SELECT_BUTTON))
    {
        static uint64_t sSteppedAt = ~0ull;
        /* NULL from a soft reset until the intro sets it. */
        bool lIsA = gSaveBlock2Ptr != NULL && gSaveBlock2Ptr->optionsButtonMode == OPTIONS_BUTTON_MODE_L_EQUALS_A;
        uint64_t frame = CtrPlatform_FrameCount();

        if (frame != sSteppedAt)
        {
            if (input->physicalDown & CTR_KEY_R)
            {
                sSteppedAt = frame;
                CtrPlatform_StepBattleSpeed(1, lIsA);
            }
            else if (!lIsA && (input->physicalDown & CTR_KEY_L))
            {
                sSteppedAt = frame;
                CtrPlatform_StepBattleSpeed(-1, false);
            }
        }
        held &= lIsA ? ~R_BUTTON : ~(L_BUTTON | R_BUTTON);
    }
    /* The bottom screen's column takes the buttons while X has its focus;
     * its touch controls press buttons through here. */
    held = CtrBottom_FilterKeys(held) | CtrBottom_InjectedKeys();
#ifdef CTR_TEST_SWOOP
    /* Emulator aid (3ds_game_full.c): A tapped through the intro, the title
     * and the menu until the field is up, for a keyboard-less run. */
    if (gMain.callback2 != CB2_Overworld && gMain.vblankCounter1 % 40 == 0)
        held |= A_BUTTON;
#endif
    REG_KEYINPUT = held ^ KEYS_MASK;
    return held;
}

/* AgbMain owns the loop: this is the game's frame boundary. */
void CtrGame_WaitFrame(void);
void VBlankIntrWait(void) { CtrGame_WaitFrame(); }
/* At once, as on the GBA: DoSoftReset expects SoftReset not to return. */
void SoftReset(u32 flags)
{
    (void)flags;
    CtrGame_SoftReset();
}

static u8 Bcd(unsigned value) { return ((value / 10) << 4) | (value % 10); }
static unsigned FromBcd(u8 value) { return (value >> 4) * 10 + (value & 15); }

void Platform_GetStatus(struct SiiRtcInfo *rtc) { rtc->status = sRtcStatus; }
void Platform_SetStatus(struct SiiRtcInfo *rtc) { sRtcStatus = rtc->status; }

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    /* A measured scenario's own clock: the game seeds its random numbers
     * from it (3ds_scenario.c). */
    int64_t pinned;
    time_t now = (CtrScenario_Clock(&pinned) ? (time_t)pinned : time(NULL)) + sClockOffset;
    struct tm value;
    if (!localtime_r(&now, &value))
        CtrPlatform_Fatal("RTC conversion failed");
    memset(rtc, 0, sizeof(*rtc));
    rtc->status = sRtcStatus;
    rtc->year = Bcd((value.tm_year + 1900) % 100);
    rtc->month = Bcd(value.tm_mon + 1);
    rtc->day = Bcd(value.tm_mday);
    rtc->dayOfWeek = Bcd(value.tm_wday);
    rtc->hour = Bcd(value.tm_hour);
    rtc->minute = Bcd(value.tm_min);
    rtc->second = Bcd(value.tm_sec);
}

void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
    struct tm value = {0};
    value.tm_year = 100 + FromBcd(rtc->year);
    value.tm_mon = FromBcd(rtc->month) - 1;
    value.tm_mday = FromBcd(rtc->day);
    value.tm_hour = FromBcd(rtc->hour);
    value.tm_min = FromBcd(rtc->minute);
    value.tm_sec = FromBcd(rtc->second);
    value.tm_isdst = -1;
    time_t requested = mktime(&value);
    if (requested == (time_t)-1)
        CtrPlatform_Fatal("invalid logical RTC date");
    sClockOffset = requested - time(NULL);
    CtrLog_Write(CTR_LOG_GAME, "logical RTC updated (session only)");
}

void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    struct SiiRtcInfo full;
    Platform_GetDateTime(&full);
    rtc->hour = full.hour;
    rtc->minute = full.minute;
    rtc->second = full.second;
}

void Platform_SetTime(struct SiiRtcInfo *rtc)
{
    struct SiiRtcInfo full;
    Platform_GetDateTime(&full);
    full.hour = rtc->hour;
    full.minute = rtc->minute;
    full.second = rtc->second;
    Platform_SetDateTime(&full);
}

void Platform_SetAlarm(u8 *alarmData)
{
    (void)alarmData;
    CTR_UNIMPLEMENTED("RTC_ALARM: hardware alarm not supported");
}

/*
 * Nothing to flush: Port_WriteFlash commits each sector to the file as the game
 * writes it, so the save on SD is already current by the time a save finishes.
 * save.c only calls this outside the port bridge anyway.
 */
void Platform_StoreSaveFile(void)
{
}

void Platform_ReadFlash(u16 sector, u32 offset, u8 *dest, u32 size)
{
    u32 address = ((u32)sector << 12) + offset;

    if (address >= sizeof(FLASH_BASE) || size > sizeof(FLASH_BASE) - address)
    {
        CtrLog_Write(CTR_LOG_ERROR, "flash read out of range: %lu+%lu",
                     (unsigned long)address, (unsigned long)size);
        return;
    }
    memcpy(dest, FLASH_BASE + address, size);
}

void Platform_QueueAudio(s32 *samples, s32 count)
{
    /*
     * m4aSoundVSync hands over one mixer frame per VBlank, measured in bytes
     * because SDL's queue is. Four bytes per sample, two samples per frame.
     */
    CtrAudio_Queue(samples, count / (s32)(2 * sizeof(s32)));
}
