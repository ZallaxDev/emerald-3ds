#ifndef CTR_DEX_TOP_H
#define CTR_DEX_TOP_H

/*
 * The Pokédex on the top screen: while the Pokédex is open on the bottom
 * screen, the top shows the device itself - its red shell, the hinge, the
 * lens and the lights, a screen window - with the Pokémon the list's cursor
 * or the info page has in the window, its front picture at 2x over the
 * battle terrain of the place it is met.
 *
 * The game side (3ds_dex_top_game.c) follows the Pokédex (patch 0058) and
 * decodes the picture and the terrain from the game's own data into the
 * buffers here; the native side (3ds_dex_top.c) puts them into its textures
 * inside the Citro3D frame, before it draws, so the GPU never reads a
 * texture that is being written.
 */
#include <stdbool.h>
#include <stdint.h>

/* A front picture's frames: 64x64 each, at most two. */
#define CTR_DEX_TOP_DIM 64
#define CTR_DEX_TOP_FRAMES 2
#define CTR_DEX_TOP_NAME 16
/* The backdrop: the left 256x128 of a battle terrain. */
#define CTR_DEX_TOP_BACKDROP_W 256
#define CTR_DEX_TOP_BACKDROP_H 128

/* ── Game side (3ds_dex_top_game.c) ───────────────────────────────────── */

/* Once a frame, after the bottom screen's own frame. */
void CtrDexTopGame_Frame(void);

/* ── Native side (3ds_dex_top.c) ──────────────────────────────────────── */

/*
 * The Pokédex is open: the device is drawn. `national` 0 is an empty window
 * (a page with no Pokémon on it); otherwise `picture` holds `frames` 64x64
 * RGBA5551 frames (rows top to bottom, 0 = transparent), copied here, and
 * `name` the species' name in ASCII.
 */
void CtrDexTop_Show(uint16_t national, const uint16_t *picture, unsigned frames, const char *name);
/* The backdrop for the Pokémon just shown, row by row (RGBA5551), after
 * Begin; used once all its rows are in. Show keeps the last one: Begin
 * drops it (a Pokémon of another terrain). */
void CtrDexTop_BackdropBegin(void);
void CtrDexTop_BackdropRow(unsigned y, const uint16_t *row);
/* Whether every row of the backdrop kept is in. */
bool CtrDexTop_BackdropComplete(void);
/* The Pokédex is closed. */
void CtrDexTop_Hide(void);
bool CtrDexTop_Active(void);

/*
 * Inside the frame (3ds_video.c): Upload once, after C3D_FrameBegin has
 * waited for the GPU to finish the last frame, then DrawEye for each eye
 * into the target the caller has begun. `sign` is +1 for the left eye, -1
 * for the right, 0 flat; `slider` 0..1.
 */
void CtrDexTop_Upload(void);
void CtrDexTop_DrawEye(float sign, float slider);
/* Not shown this frame: the textures are given back. */
void CtrDexTop_Idle(void);

#endif /* CTR_DEX_TOP_H */
