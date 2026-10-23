#ifndef GUARD_PORT_PLATFORM_H
#define GUARD_PORT_PLATFORM_H

/*
 * ARM11 compatibility header for the portable bridge API.
 *
 * The shared game tree (`src/`, `include/`) carries small hooks behind
 * `#ifdef PORT_BRIDGE` for everything that lives outside the executable on
 * this target: asset stubs, script/text/event pointers, map payloads and save
 * storage. Game translation units are compiled with -DPORT_BRIDGE and this
 * header; the ARM11 backend implements the API (3ds_assets.c,
 * 3ds_map_loader.c, 3ds_script_loader.c, 3ds_compat.c).
 *
 * The native 3DS translation units (3ds_platform.c, 3ds_video.c, ...) are
 * compiled without PORT_BRIDGE and without this header.
 */

#include <stdbool.h>
#include "global.h"

/* ── Platform lifecycle hooks called from the shared main loop ───────────── */
void Port_InitVramBanks(void);
void Port_SyncVramToHardware(void);
void Port_AudioSyncPlayerState(void);
void Port_RecordMainLoopIteration(void);

/* ── External resources ─────────────────────────────────────────────────── */
/* Hardware profiling of a stretch of game code: logs the time since the last
 * mark when it is a millisecond or more (rate limited). */
void Port_ProfileMark(const char *label);
void Port_ProfBegin(void);
void Port_ProfAcc(const char *name);
void Port_AssetPreload(void);
bool Port_IsAssetStub(const void *ptr);
const char *Port_GetAssetPath(const void *ptr);
u32 Port_GetAssetSize(const void *ptr);
u32 Port_GetAssetSizeExact(const void *base);
u32 Port_GetAssetSizeSized(const void *ptr, u32 size);
bool Port_LoadAssetToBuffer(const char *path, void *dest, u32 maxSize);
bool Port_LoadAssetPointerToBuffer(const void *ptr, void *dest, u32 maxSize);
bool Port_LoadAssetPointerToBufferSized(const void *ptr, void *dest, u32 size);
bool Port_LoadAssetRangeToBuffer(const void *base, u32 offset, void *dest, u32 size);
bool Port_LoadAssetPointerToHeap(const void *ptr, u8 **outBuf, u32 *outSize);
const void *Port_ResolveAssetPointer(const void *ptr);
const void *Port_ResolveAssetPointerSized(const void *ptr, u32 size);
const void *Port_ResolveAssetPointerInContainingAsset(const void *ptr, u32 size);
u32 Port_GetSpriteFrameSize(const void *base, u32 declaredSize);
const void *Port_ResolveSpriteFramePointer(const void *base, u32 size, u32 offset);
/* The same, only if the frame is already in memory: NULL rather than a read. */
const void *Port_PeekSpriteFramePointer(const void *base, u32 size, u32 offset);
const void *Port_ResolveFontPointer(const void *ptr);
/* 3ds_icons_ptbr.c: redraws a graphic with English words as it is read. */
void Port_TransformAsset(const char *path, unsigned char *data, unsigned size);
void Port_TransformDecompressed(const void *source, void *data, unsigned size);
const unsigned char *Port_LatinGlyph(unsigned font, unsigned char code, int *width);
void Port_PreloadLatinFonts(void);
u32 Port_GetDecompressedAssetSize(const void *ptr);
const void *Port_ResolveEventPointer(const void *ptr);
const u8 *Port_ResolveScriptPointer(const u8 *ptr);
const u8 *Port_ResolveTextPointer(const u8 *ptr);

const struct MapLayout *Port_GetMapLayoutById(u16 mapLayoutId);
const struct MapHeader *Port_GetMapHeaderByGroupAndId(u16 mapGroup, u16 mapNum);
const void *Port_ResolveMapAssetPointer(const void *ptr);

/* ── Audio ───────────────────────────────────────────────────────────────
 * The sound bank is linked, so the mixer reads it in place; what is left here
 * is the debug channel below, which the mixer uses to mark its own frame.
 */
/* ── Save storage ────────────────────────────────────────────────────────── */
void Port_SaveInit(void);
bool Port_SaveIsAvailable(void);
u16 Port_WriteFlash(u32 offset, const void *data, u32 size);

/* ── Reclaimable resource cache ─────────────────────────────────────────── */
void Port_VisitGameHeap(void (*visit)(const void *, u32));

/* ── Debug channels ─────────────────────────────────────────────────────
 * Marks the game code leaves at points of interest (the Birch intro, DMA,
 * assets, the field's graphics). Nothing on this target reads them, and some
 * are on hot paths (MapGridGetMetatileIdAt, RunTasks): they are empty inline
 * here, so the calls cost nothing; the arguments are still evaluated. The
 * audio channel is real: the mixer's marks time its frame (3ds_compat.c).
 */
void Port_SetAudioDebugEx(u16 tag, u16 a, u16 b, u16 c, u16 d);

/* Two callers see no prototype (implicitly declared, src/pokeball.c and
 * src/trainer_pokemon_sprites.c): 3ds_compat.c keeps these two out of line,
 * empty, for them. */
#ifdef PORT_BIRCH_DEBUG_OUT_OF_LINE
void Port_SetBirchDebug(u16 tag, u16 a, u16 b, u16 c);
void Port_SetBirchDebugEx(u16 tag, u16 a, u16 b, u16 c, u16 d);
#else
static inline void Port_SetBirchDebugEx(u16 tag, u16 a, u16 b, u16 c, u16 d)
{
    (void)tag; (void)a; (void)b; (void)c; (void)d;
}
static inline void Port_SetBirchDebug(u16 tag, u16 a, u16 b, u16 c)
{
    (void)tag; (void)a; (void)b; (void)c;
}
#endif
static inline void Port_SetDmaDebugEx(u16 tag, u16 a, u16 b, u16 c, u16 d)
{
    (void)tag; (void)a; (void)b; (void)c; (void)d;
}
static inline void Port_SetDmaDebug(u16 tag, u16 a, u16 b, u16 c)
{
    (void)tag; (void)a; (void)b; (void)c;
}
static inline void Port_SetDmaDebug2(u16 tag, u16 a, u16 b, u16 c, u16 d)
{
    (void)tag; (void)a; (void)b; (void)c; (void)d;
}
static inline void Port_SetAssetDebugEx(u16 tag, u16 a, u16 b, u16 c, u16 d)
{
    (void)tag; (void)a; (void)b; (void)c; (void)d;
}
static inline void Port_SetAssetDebug(u16 tag, u16 a, u16 b, u16 c)
{
    (void)tag; (void)a; (void)b; (void)c;
}
static inline void Port_SetGfxTilesetDebug(u8 secondary, u16 size, u8 nonzero)
{
    (void)secondary; (void)size; (void)nonzero;
}
static inline void Port_SetGfxPaletteDebug(u8 secondary, u8 nonzero, u8 resolved)
{
    (void)secondary; (void)nonzero; (void)resolved;
}
static inline void Port_SetGfxMetatileDebug(u16 id, u8 resolved, u16 first, u16 second)
{
    (void)id; (void)resolved; (void)first; (void)second;
}

/* The soft reset: L+R+START+SELECT, as in the 3DS games, in place of the
 * GBA's A+B+START+SELECT (src/main.c, patch 0080; 3ds_game_full.c). */
#define CTR_SOFT_RESET_KEYS (L_BUTTON | R_BUTTON | START_BUTTON | SELECT_BUTTON)

#endif
