/*
 * Force-included into the translation units staged as a whole GBA picture
 * (full.mk, CTR_GBA_STAGE_SRCS): the intro, the title screen, the credits and
 * the cable car.
 *
 * Those screens are one 240x160 composition each, so they are compiled with
 * the GBA geometry (include/gba/defines.h) and the compositor has to know when
 * one of them is on screen to present it as a stage. Their VBlank callbacks
 * are the marker: every one they install goes through the bridge, which
 * remembers it, and the stage lasts while one of those is the active callback.
 * Renaming the call here keeps the game sources untouched; main.h declares the
 * renamed function, so the prototype still matches.
 */
#ifndef CTR_GBA_STAGE_H
#define CTR_GBA_STAGE_H

#if defined(CTR_STAGE_ZOOMED)
/* The legendaries' cutscenes: magnified to fit the screen (3ds_video.c,
 * RenderEye). */
void CtrStageZoomed_SetVBlankCallback(void (*callback)(void));
#define SetVBlankCallback CtrStageZoomed_SetVBlankCallback
#elif defined(CTR_STAGE_COVERED)
/* The cable car: scaled up until its picture covers the whole top screen, the
 * rows that do not fit cut off, so no margin is invented (3ds_video.c). */
void CtrStageCovered_SetVBlankCallback(void (*callback)(void));
#define SetVBlankCallback CtrStageCovered_SetVBlankCallback
#elif defined(CTR_STAGE_FADED)
/* The cable car: its scrolling layers too fade into the margins, like the
 * intro's still ones (3ds_video.c, DrawStageBg). */
void CtrStageFaded_SetVBlankCallback(void (*callback)(void));
#define SetVBlankCallback CtrStageFaded_SetVBlankCallback
#else
#define SetVBlankCallback CtrStage_SetVBlankCallback
#endif

#endif
