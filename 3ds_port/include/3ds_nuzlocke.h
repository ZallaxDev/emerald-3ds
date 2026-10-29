#ifndef CTR_NUZLOCKE_H
#define CTR_NUZLOCKE_H

/*
 * The Nuzlocke tracker's hooks in the game's save code (src/save.c,
 * src/new_game.c; src/3ds_nuzlocke_game.c). SDK-free, no game types.
 * `save` is the save image as the flash holds it after the load or the write.
 */
#include <stddef.h>

/* The game's save was loaded: this save's record, or an empty tracker. */
void CtrNuzlocke_Loaded(const void *save, size_t size);
/* The game's save was written without error: the record follows it. */
void CtrNuzlocke_Saved(const void *save, size_t size);
/* A new game: an empty tracker, whose record the first save writes. */
void CtrNuzlocke_NewGame(void);

#endif
