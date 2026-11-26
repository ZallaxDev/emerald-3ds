#ifndef CTR_LOG_H
#define CTR_LOG_H
#include <stdbool.h>

typedef enum
{
    CTR_LOG_BOOT, CTR_LOG_VIDEO, CTR_LOG_INPUT, CTR_LOG_FS,
    CTR_LOG_AUDIO, CTR_LOG_GAME, CTR_LOG_ERROR
} CtrLogCategory;

void CtrLog_Init(void);
void CtrLog_Close(void);
/* The last drain of the log to the card: how long it took and how long ago it
 * ended, in ms (0 and a large age before the first). */
void CtrLog_LastDrain(float *ms, float *agoMs);
void CtrLog_ShowFatal(const char *reason);
/* Whether there is a log to write to (debug.txt on the SD card). */
bool CtrLog_Enabled(void);
void CtrLog_Write(CtrLogCategory category, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
/* Records for perf.bin (beside port.log, same debug.txt condition): copied
 * into a ring of their own and written by the log's thread. A record that
 * does not fit is dropped whole and counted in port.log. */
void CtrLog_WriteBinary(const void *data, unsigned size);

/*
 * The hang watch (with the log on, nothing without it): a thread above the
 * game's priority that looks twice a second at whether the main loop still
 * turns. When no frame has started for three seconds it writes where the
 * main thread last said it was, and sees that the line reaches the card even
 * if the main thread is spinning (the writer is put above it). A frozen
 * console - black screen, no HOME - then leaves in port.log the stage it
 * froze in: a wait for the GPU, a draw, the game's own code.
 *
 * CtrLog_Beat: once a frame, from the main loop. CtrLog_Mark: where the main
 * thread is from now on, a string literal (kept by pointer); NULL where a
 * long wait is in order (APT: the HOME menu, sleep), which the watch skips.
 */
void CtrLog_Beat(void);
void CtrLog_Mark(const char *where);

#endif
