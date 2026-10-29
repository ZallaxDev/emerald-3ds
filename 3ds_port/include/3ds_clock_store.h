#ifndef CTR_CLOCK_STORE_H
#define CTR_CLOCK_STORE_H

#include <stdbool.h>
#include <stddef.h>

bool CtrClockStore_Load(const void *save, size_t size);
bool CtrClockStore_Save(const void *save, size_t size);

/*
 * What the last CtrClockStore_Load found, for a notice (3ds_bottom_ui.c). It
 * only reports: the files, their format and the order they are tried in
 * (main, temp, backup) are unchanged, and so is the fallback to the local RTC.
 * SDK-free.
 */
typedef enum
{
    CTR_CLOCK_STORE_NONE,       /* no load yet, or no save to bind one to (a new game) */
    CTR_CLOCK_STORE_RESTORED,   /* the main file matched and was restored */
    CTR_CLOCK_STORE_RECOVERED,  /* the temp or backup file matched and was restored */
    CTR_CLOCK_STORE_MISSING,    /* no clock file at all: nothing was saved to restore */
    CTR_CLOCK_STORE_INVALID,    /* a file was there but unreadable, malformed or out of range */
    CTR_CLOCK_STORE_MISMATCH,   /* a well-formed file was there, bound to another save */
} CtrClockStoreStatus;

CtrClockStoreStatus CtrClockStore_Status(void);
/* Counts the loads, so a notice dismissed for one comes back after another. */
unsigned CtrClockStore_LoadSerial(void);
/* An existing save has no usable saved clock: the local RTC is in use.
 * NONE suppresses the notice for a new game without a save to bind. */
bool CtrClockStore_NeedsNotice(CtrClockStoreStatus status);

#endif
