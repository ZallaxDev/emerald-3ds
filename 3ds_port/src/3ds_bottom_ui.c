/*
 * The bottom screen, game side: what it shows and what a touch does.
 *
 * It replaces the START menu. A header on top (Y, the section's title, the
 * Repel, the clock, RUN), a dock at the bottom with one button a section
 * (MAP, the default, POKéDEX, POKéMON, BAG, POKéNAV, the trainer card, SAVE,
 * OPTIONS, and NUZLOCKE while the tracker is on) and the content area
 * between them; everything happens here while the top screen keeps the
 * world. The game's own screens (the party menu, the bag, the Pokédex, the
 * PokéNav, the boxes) are its 240x160 picture 1:1 in a window in the middle
 * (CtrVideo_BottomInUse, CTR_BOTTOM_WIN_X/Y), drawn by the compositor; a tap
 * on them goes to the game in pixels of that picture (CtrBag_Touch,
 * CtrPokedex_Touch, CtrMenu_PostTap, CtrStorage_Tap, the PokéNav's taps).
 * Opened from a battle, a shop or the PC, the game's screen has the whole
 * screen and there is no dock.
 *
 * The map, the trainer card, SAVE, OPTIONS and NUZLOCKE are drawn and run
 * here directly (3ds_bottom_field.inc, 3ds_bottom_options.inc). While one of
 * the game's screens is open, or on its way (a START entry asked for,
 * CtrBottom_KeepWorld), the top screen holds the world (CtrVideo_HoldTop,
 * the sessions below); the game still decides everything.
 *
 * The frame is the port's own design: a backdrop (romfs bottom/menu.bin,
 * scripts/gen_menu_art.py) and plates rendered at run time
 * (3ds_bottom_plate.inc, 3ds_bottom_shell.inc), a chosen or pressed one
 * emerald, with the game's item and mon icons on them and a few new
 * pictures (3ds_bottom_art.h, from assets/artwork/bottom). Everything else
 * is the game's own graphics, decoded from the same RomFS files the game
 * loads: the Hoenn region map, mon/item/type/status icons, front pictures,
 * badges and the game's fonts. Texts are the game's strings where it has
 * them. X walks the header and the dock with the buttons (ProcessKeys); the
 * 3DS's Y is SELECT (3ds_game_bridge.c).
 *
 * Cost model, chosen for an Old 3DS whose frame the top screen already fills:
 *   - every frame: a snapshot of the few values on screen (a memcmp of a few
 *     hundred bytes) and the touch state; no drawing;
 *   - when the snapshot changes: the canvas is recomposed by the CPU from
 *     pre-rendered backgrounds (a memcpy) plus the live parts, and copied to
 *     the framebuffer; a plate's picture is made once and kept;
 *   - RomFS is never read inside a redraw: icons and pictures are fetched one
 *     per frame beforehand, everything else is decoded once at boot.
 * No GPU time, no VRAM, no linear memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "global.h"
#include "3ds_locale.h"
#include "3ds_daynight.h"
#include "3ds_encounters.h"
#include "3ds_locale_text.h"
#include "main.h"
#include "money.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_controllers.h"
#include "battle_main.h"
#include "battle_message.h"
#include "contest_util.h"
#include "contest_effect.h"
#include "data.h"
#include "event_data.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "fonts.h"
#include "graphics.h"
#include "item.h"
#include "item_icon.h"
#include "item_menu.h"
#include "menu.h"
#include "new_game.h"
#include "overworld.h"
#include "palette.h"
#include "party_menu.h"
#include "pokedex.h"
#include "pokemon.h"
#include "pokemon_icon.h"
#include "pokemon_summary_screen.h"
#include "pokenav.h"
#include "region_map.h"
#include "safari_zone.h"
#include "save.h"
#include "script.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "strings.h"
#include "task.h"
#include "text.h"
#include "text_window.h"
#include "util.h"
#include "wild_encounter.h"
#include "constants/items.h"
#include "constants/maps.h"
#include "constants/vars.h"
#include "constants/map_types.h"
#include "constants/party_menu.h"
#include "constants/region_map_sections.h"
#include "constants/rgb.h"
#include "constants/songs.h"
#include "constants/trainers.h"
#include "port_platform.h"

#include "3ds_data.h"
#include "3ds_bottom.h"
#include "3ds_extras.h"
#include "3ds_bottom_art.h"
#include "3ds_clock_store.h"
#include "3ds_input.h"
#include "3ds_nuzlocke_ui.h"
#include "3ds_game_views.h"
#include "3ds_log.h"
#include "3ds_platform.h"
#include "3ds_video.h"

/* Exported by the game under PLATFORM_3DS, or not exported by its headers. */
void CB2_BagMenuRun(void);
/* party_menu.c: the dock's buttons close the game's party menu. */
bool8 CtrParty_Close(bool8 leaving);
/* menu.c: a tap on a game screen shown here, for what waits for input. */
void CtrMenu_PostTap(s16 x, s16 y);
void CtrStartMenu_Request(u8 action);
bool8 CtrStartMenu_Pending(void);
bool8 CtrStartMenu_Available(void);
bool8 CtrStartMenu_Busy(void);
bool8 CtrPokenav_IsOpen(void);
u32 CtrPokenav_Screen(bool8 *ready);
int CtrPokenavMenu_Options(int *cursor);
void CtrPokenavMenu_Rows(int *yStart, int *deltaY);
bool8 CtrPokenavList_View(u8 *x, u8 *y, u8 *width, u16 *top, u16 *selected, u16 *shown, u16 *count);
u8 CtrPokenavMatchCall_Input(u16 *cursor, u16 *count);
bool8 CtrPokenavRibbons_Summary(u16 *selected, u16 *normal, u16 *gift, u16 *giftStart, bool8 *expanded);
bool8 CtrRegionMap_Cursor(s16 *x, s16 *y, bool8 *zoomed, bool8 *moving);
bool8 CtrMonMarkings_Menu(s8 *cursor, s16 *x, s16 *y);
bool8 CtrPokenavCondition_Marking(void);
void CtrPokenavMenu_SetCursor(int cursor);
void CtrPokenavList_SetSelected(u16 selected);
void CtrPokenavMatchCall_SetOption(u16 cursor);
void CtrMonMarkings_SetCursor(s8 cursor);
bool8 CtrStorage_IsOpen(void);
int CtrTitleScreen_RayquazaBg(void);
void CtrStorage_Tap(s16 x, s16 y);
void CtrSummary_Tap(s16 x, s16 y);
/* trade.c: the trade menu is up (on the whole screen); it takes taps as the
 * party menu does (CtrMenu_PostTap). */
bool8 CtrTrade_IsOpen(void);
/* item_menu.c: the bag's touches, in pixels of its picture. */
enum { BAG_TOUCH_DOWN, BAG_TOUCH_MOVE, BAG_TOUCH_UP, BAG_TOUCH_CANCEL };
void CtrBag_Touch(u8 phase, s16 x, s16 y);
bool8 CtrBag_Close(void);
/* pokedex.c: the Pokédex's touches, in pixels of its picture, as the bag's. */
bool8 CtrPokedex_IsOpen(void);
void CtrPokedex_Touch(u8 phase, s16 x, s16 y);
bool8 CtrPokedex_Close(bool8 leave);
/* shop.c: the BUY screen's touches, in pixels of its picture, as the bag's.
 * It has the whole screen (CTR_CENTRED_SHOP), so it runs as MODE_STORAGE. */
bool8 CtrShop_IsOpen(void);
void CtrShop_Touch(u8 phase, s16 x, s16 y);
void SetPokemonCryStereo(u32 val);
extern const struct PokedexEntry gPokedexEntries[];

/* start_menu.c's MENU_ACTION_* (the enum is private to that file). */
enum { START_POKEDEX, START_POKEMON, START_BAG, START_POKENAV, START_PLAYER, START_NONE = 0xFF };
bool8 CtrTrainerCard_FieldOpen(void);
u8 CtrTrainerCard_Side(void);
bool8 CtrBottom_NativeMenus(void);

#define W CTR_BOTTOM_WIDTH
#define H CTR_BOTTOM_HEIGHT
/* The game's picture, 240 wide: centred on the whole screen in a battle. */
#define CW 240

/* ------------------------------------------------------------------------ */
/* Canvas                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * The canvas, in framebuffer layout, is what gets copied to the screen. The
 * big static pictures behind each view (the backdrop, the Hoenn map in its
 * window, the game's window) are made once into caches of the same layout, so a redraw
 * starts with a memcpy instead of re-decoding tens of thousands of pixels.
 * Views draw in content coordinates; sOX moves them (battle menus centre the
 * 240-wide views in the whole screen).
 */
static u16 sCanvas[W * H] __attribute__((aligned(32)));
static u16 *sDst = sCanvas;
static int sOX;

/* The menu's backdrop; the party menu's background, under a battle without
 * its art; the backdrop with the map's window; with the game's window; and
 * CACHE_BATTLE, the battle art's backdrop itself, in this layout already. */
enum { CACHE_MENU, CACHE_WIDE, CACHE_MAP, CACHE_GAME, CACHE_BATTLE, CACHE_COUNT };
static u16 *sCache[CACHE_COUNT];

/*
 * A redraw that changes one part of the screen - a button pressed, a cursor
 * moved, an HP bar draining, an option's value - draws only that part: every
 * writer below keeps inside this clip (screen pixels, after sOX), the cache
 * is copied in only for it, and only it is sent to the screen. The whole
 * screen is the default.
 */
static int sClipX0, sClipY0, sClipX1 = W, sClipY1 = H;

static bool8 ClipIsFull(void)
{
    return sClipX0 == 0 && sClipY0 == 0 && sClipX1 == W && sClipY1 == H;
}

static void CopyCache(int which)
{
    if (ClipIsFull())
    {
        if (sCache[which])
            memcpy(sCanvas, sCache[which], sizeof(sCanvas));
        else
            memset(sCanvas, 0, sizeof(sCanvas));
        return;
    }
    /* Whole columns side by side are one run: a single copy (a page of the
     * options redraws the whole content area). */
    if (sClipY0 == 0 && sClipY1 == H && sClipX0 < sClipX1)
    {
        if (sCache[which])
            memcpy(sCanvas + sClipX0 * H, sCache[which] + sClipX0 * H, (size_t)(sClipX1 - sClipX0) * H * sizeof(u16));
        else
            memset(sCanvas + sClipX0 * H, 0, (size_t)(sClipX1 - sClipX0) * H * sizeof(u16));
        return;
    }
    /* A column runs bottom-to-top: rows [y0, y1) are one contiguous run. */
    for (int x = sClipX0; x < sClipX1; ++x)
    {
        u16 *dst = sCanvas + x * H + (H - sClipY1);

        if (sCache[which])
            memcpy(dst, sCache[which] + x * H + (H - sClipY1), (size_t)(sClipY1 - sClipY0) * sizeof(u16));
        else
            memset(dst, 0, (size_t)(sClipY1 - sClipY0) * sizeof(u16));
    }
}

static inline void Put(int x, int y, u16 c)
{
    x += sOX;
    if (x < sClipX0 || x >= sClipX1 || y < sClipY0 || y >= sClipY1)
        return;
    sDst[x * H + (H - 1 - y)] = c;
}

static void FillRect(int x, int y, int w, int h, u16 c)
{
    int x0 = x + sOX, x1 = x0 + w, y0 = y, y1 = y + h;

    if (x0 < sClipX0) x0 = sClipX0;
    if (x1 > sClipX1) x1 = sClipX1;
    if (y0 < sClipY0) y0 = sClipY0;
    if (y1 > sClipY1) y1 = sClipY1;
    for (int cx = x0; cx < x1; ++cx)
    {
        u16 *p = sDst + cx * H + (H - y1);
        for (int n = y1 - y0; n > 0; --n)
            *p++ = c;
    }
}

/* Black at half opacity over what is there: each RGB565 channel halved. */
static void ShadeRect(int x, int y, int w, int h)
{
    int x0 = x + sOX, x1 = x0 + w, y0 = y, y1 = y + h;

    if (x0 < sClipX0) x0 = sClipX0;
    if (x1 > sClipX1) x1 = sClipX1;
    if (y0 < sClipY0) y0 = sClipY0;
    if (y1 > sClipY1) y1 = sClipY1;
    for (int cx = x0; cx < x1; ++cx)
    {
        u16 *p = sDst + cx * H + (H - y1);
        for (int n = y1 - y0; n > 0; --n, ++p)
            *p = (*p >> 1) & 0x7BEF;
    }
}

static u16 Rgb565(u16 bgr)
{
    u16 r = bgr & 31, g = (bgr >> 5) & 31, b = (bgr >> 10) & 31;
    return (r << 11) | (((g << 1) | (g >> 4)) << 5) | b;
}


typedef struct { u16 c[16]; } Pal;

static void ToPals(Pal *dst, const u16 *src, int count)
{
    for (int p = 0; p < count; ++p)
        for (int i = 0; i < 16; ++i)
            dst[p].c[i] = Rgb565(src[p * 16 + i]);
}

/*
 * A 4bpp 8x8 tile; colour 0 is transparent, as on the GBA. This is where a
 * redraw spends its time, so a tile fully on screen is written straight into
 * its columns: pixel (x, y) is canvas[x * H + H - 1 - y], so one step right
 * is +H and one step down is -1.
 */
static void DrawTile(const u8 *tile, int x, int y, const u16 *pal, bool8 hflip, bool8 vflip)
{
    int sx0 = x + sOX;

    if (sx0 >= sClipX1 || y >= sClipY1 || sx0 + 8 <= sClipX0 || y + 8 <= sClipY0)
        return;
    if (sx0 >= sClipX0 && y >= sClipY0 && sx0 + 8 <= sClipX1 && y + 8 <= sClipY1)
    {
        u16 *origin = sDst + sx0 * H + (H - 1 - y);
        for (int py = 0; py < 8; ++py)
        {
            const u8 *row = tile + (vflip ? 7 - py : py) * 4;
            u32 bits = row[0] | (row[1] << 8) | (row[2] << 16) | ((u32)row[3] << 24);
            u16 *p = origin - py;

            if (!bits)
                continue;
            for (int px = 0; px < 8; ++px, bits >>= 4)
            {
                u8 v = bits & 15;
                if (v)
                    p[(hflip ? 7 - px : px) * H] = pal[v];
            }
        }
        return;
    }
    for (int py = 0; py < 8; ++py)
    {
        const u8 *row = tile + (vflip ? 7 - py : py) * 4;
        for (int px = 0; px < 8; ++px)
        {
            int sx = hflip ? 7 - px : px;
            u8 v = (row[sx >> 1] >> ((sx & 1) * 4)) & 15;
            if (v)
                Put(x + px, y + py, pal[v]);
        }
    }
}

/* A sprite in the GBA's one-dimensional tile layout. */
static void DrawSprite(const u8 *tiles, int wt, int ht, int x, int y, const u16 *pal)
{
    for (int ty = 0; ty < ht; ++ty)
        for (int tx = 0; tx < wt; ++tx)
            DrawTile(tiles + (ty * wt + tx) * 32, x + tx * 8, y + ty * 8, pal, FALSE, FALSE);
}

/* A text background tilemap entry: tile, flips and palette bank. */
static void DrawMapEntry(const u8 *tiles, u32 tileCount, u16 e, int x, int y, const Pal *pals)
{
    u32 tile = e & 0x3FF;

    if (tiles && tile < tileCount)
        DrawTile(tiles + tile * 32, x, y, pals[e >> 12].c, (e >> 10) & 1, (e >> 11) & 1);
}

static void DrawTypeIcon(u8 type, int x, int y);
static void DrawCategoryIcon(u16 move, int x, int y);

/* ------------------------------------------------------------------------ */
/* Resources                                                                */
/* ------------------------------------------------------------------------ */

static void *ReadRomfs(const char *path, u32 *outSize)
{
    char full[128];
    void *data;

    snprintf(full, sizeof(full), "graphics/%s", path);
    data = CtrData_Load(full, outSize);
    if (!data)
        CtrLog_Write(CTR_LOG_ERROR, "bottom: %s missing", full);
    return data;
}

/*
 * LZ77 data, whether a game symbol (an asset stub, resolved from RomFS) or a
 * buffer read here. The size comes from the stream's own header.
 */
static void *Unlz(const void *src, u32 *outSize)
{
    const u8 *res = src ? Port_ResolveAssetPointer(src) : NULL;
    u32 header, size;
    void *dst;

    if (!res)
        return NULL;
    header = res[0] | (res[1] << 8) | (res[2] << 16) | ((u32)res[3] << 24);
    size = header >> 8;
    if ((header & 0xFF) != 0x10 || size == 0 || size > 0x10000)
        return NULL;
    dst = malloc(size);
    if (!dst)
        return NULL;
    LZ77UnCompWram((const u32 *)res, dst);
    if (outSize)
        *outSize = size;
    return dst;
}

static void *UnlzFile(const char *path, u32 *outSize)
{
    void *packed = ReadRomfs(path, NULL);
    void *data = Unlz(packed, outSize);

    free(packed);
    return data;
}

static bool8 PalFile(const char *path, Pal *dst, int count, bool8 compressed)
{
    u32 size = 0;
    u16 *raw = compressed ? UnlzFile(path, &size) : ReadRomfs(path, &size);

    if (!raw)
        return FALSE;
    if ((int)(size / 32) < count)
        count = size / 32;
    ToPals(dst, raw, count);
    free(raw);
    return TRUE;
}

/* An uncompressed game palette; the pointer may be an asset stub. */
static void PalSymbol(const void *src, Pal *dst, int count)
{
    const u16 *res = src ? Port_ResolveAssetPointer(src) : NULL;

    if (res)
        ToPals(dst, res, count);
}

/* The sections, the dock's buttons left to right; NUZLOCKE only while the
 * tracker is on. */
enum { SCR_MAP, SCR_POKEDEX, SCR_POKEMON, SCR_BAG, SCR_POKENAV, SCR_CARD, SCR_SAVE, SCR_OPTION, SCR_NUZLOCKE,
       SCR_COUNT };
/* The header's buttons, after the dock's for the focus: the registered item
 * (Y), the title (the wild list's toggle) and RUN. */
enum { COL_Y = SCR_COUNT, COL_TITLE, COL_RUN, COL_ITEMS };

/*
 * The game's six options, then the port's own, off by default and kept in
 * settings.txt rather than in the save (3ds_settings.c): the FPS counter and
 * the voxel overworld. Only a build with the counter has its row, and only
 * one with the voxel renderer has the voxel rows.
 */
#ifndef CTR_SHOW_FPS
#define CTR_SHOW_FPS 1
#endif
enum { OPT_TEXT_SPEED, OPT_BATTLE_SCENE, OPT_BATTLE_STYLE, OPT_SOUND, OPT_BUTTON_MODE, OPT_FRAME,
       OPT_FPS, OPT_VOXEL, OPT_VOXEL_PITCH, OPT_VOXEL_ZOOM, OPT_VOXEL_BLUR, OPT_VOXEL_BATTLE,
       OPTION_ROWS };
#if CTR_VOXEL_ENABLED
#define OPTION_SHOWN OPTION_ROWS
#else
#define OPTION_SHOWN OPT_VOXEL
#endif

typedef struct
{
    u8 *tiles;
    u8 size;        /* 3 for a 24x24 item icon, 4 for 32x32 */
    Pal pal;
} Icon;

static struct
{
    bool8 ready;
    Pal text;                        /* the message box text palette */
    /* Party menu. */
    u8 *partyTiles;
    u32 partyTileCount;
    u16 partyRaw[16 * 11];
    Pal partyPal[11];
    /* Icons: the dock's, and the Repel's in the header. */
    Icon column[SCR_COUNT];
    Icon repel;
    u8 *typeTiles;
    Pal typePal[3];
    const u8 *categoryTiles;
    Pal categoryPal;
    /* The mon icons' palettes (the game has three): the party's, the wild list's. */
    Pal monIconPal[3];
    /* Region map. */
    u8 *mapTiles;
    u32 mapTileCount;
    u8 *mapMap;
    u16 mapPal[32];
    u8 *playerIcon[2];
    Pal playerIconPal[2];
    u8 *cursorTiles;
    Pal cursorPal;
    /* Trainer card. */
    Pal badgePal;
    u8 *badgeTiles;
    u8 *trainerPic[2];
    Pal trainerPicPal[2];
} sRes;

static void LoadItemIcon(Icon *icon, const void *tiles, const void *pal)
{
    u16 *raw = Unlz(pal, NULL);

    icon->tiles = Unlz(tiles, NULL);
    icon->size = 3;
    if (raw)
    {
        ToPals(&icon->pal, raw, 1);
        free(raw);
    }
}

/* Both players' trainer picture and bag, so nothing is decoded in a frame. */
static void LoadGenderResources(void)
{
    for (u8 gender = MALE; gender <= FEMALE; ++gender)
    {
        u16 pic = gFacilityClassToPicIndex[gender == FEMALE ? FACILITY_CLASS_MAY : FACILITY_CLASS_BRENDAN];
        u16 *pal = Unlz(gTrainerFrontPicPaletteTable[pic].data, NULL);

        sRes.trainerPic[gender] = Unlz(gTrainerFrontPicTable[pic].data, NULL);
        if (pal)
        {
            ToPals(&sRes.trainerPicPal[gender], pal, 1);
            free(pal);
        }
    }
}

static void LoadBattleArt(void);
static void BuildRayquaza(void);
static void BuildTypeColours(void);
static void InitLook(void);

static void LoadResources(void)
{
    u32 size = 0;   /* UnlzFile leaves it alone when it fails */

    PalSymbol(GetOverworldTextboxPalettePtr(), &sRes.text, 1);
    InitLook();

    sRes.partyTiles = UnlzFile("party_menu/bg.4bpp.lz", &size);
    sRes.partyTileCount = size / 32;
    {
        u16 *raw = UnlzFile("party_menu/bg.gbapal.lz", &size);
        if (raw)
        {
            memcpy(sRes.partyRaw, raw, size < sizeof(sRes.partyRaw) ? size : sizeof(sRes.partyRaw));
            free(raw);
        }
        ToPals(sRes.partyPal, sRes.partyRaw, 11);
    }

    /* The dock's icons: the game's own item icons, and the PokéNav's. */
    LoadItemIcon(&sRes.column[SCR_MAP], gItemIcon_TownMap, gItemIconPalette_TownMap);
    LoadItemIcon(&sRes.column[SCR_POKEMON], gItemIcon_PokeBall, gItemIconPalette_PokeBall);
    LoadItemIcon(&sRes.column[SCR_BAG], gItemIcon_BerryPouch, gItemIconPalette_BerryPouch);
    LoadItemIcon(&sRes.column[SCR_CARD], gItemIcon_ContestPass, gItemIconPalette_ContestPass);
    /* Emerald has no Pokédex item icon; port art (Platinum menu Pokédex). */
    sRes.column[SCR_POKEDEX].tiles = (u8 *)sArtPokedex;
    sRes.column[SCR_POKEDEX].size = 3;
    ToPals(&sRes.column[SCR_POKEDEX].pal, sArtPalette, 1);
    LoadItemIcon(&sRes.column[SCR_SAVE], GetItemIconPicOrPalette(ITEM_LETTER, 0), GetItemIconPicOrPalette(ITEM_LETTER, 1));
    LoadItemIcon(&sRes.column[SCR_OPTION], gItemIcon_TeachyTV, gItemIconPalette_TeachyTV);
    sRes.column[SCR_POKENAV].tiles = UnlzFile("pokenav/nav_icon.4bpp.lz", NULL);
    sRes.column[SCR_POKENAV].size = 4;
    PalFile("pokenav/nav_icon.gbapal", &sRes.column[SCR_POKENAV].pal, 1, FALSE);
    /* Port art: a Poké Ball with a cross on it. */
    sRes.column[SCR_NUZLOCKE].tiles = (u8 *)sArtNuzlocke;
    sRes.column[SCR_NUZLOCKE].size = 3;
    ToPals(&sRes.column[SCR_NUZLOCKE].pal, sArtPalette2, 1);
    LoadItemIcon(&sRes.repel, GetItemIconPicOrPalette(ITEM_REPEL, 0), GetItemIconPicOrPalette(ITEM_REPEL, 1));

    sRes.typeTiles = UnlzFile("types/move_types.4bpp.lz", NULL);
    PalFile("types/move_types.gbapal.lz", sRes.typePal, 3, TRUE);
    {
        const u16 *pal;

        sRes.categoryTiles = GetMoveCategoryIconGfx(&pal);
        ToPals(&sRes.categoryPal, pal, 1);
    }
    for (int i = 0; i < (int)ARRAY_COUNT(sRes.monIconPal); ++i)
        PalSymbol(gMonIconPaletteTable[i].data, &sRes.monIconPal[i], 1);

    sRes.mapTiles = UnlzFile("pokenav/region_map/map.8bpp.lz", &size);
    sRes.mapTileCount = size / 64;
    sRes.mapMap = UnlzFile("pokenav/region_map/map.bin.lz", NULL);
    {
        u16 *raw = ReadRomfs("pokenav/region_map/map.gbapal", &size);
        if (raw)
        {
            for (u32 i = 0; i < 32 && i < size / 2; ++i)
                sRes.mapPal[i] = Rgb565(raw[i]);
            free(raw);
        }
    }
    sRes.playerIcon[MALE] = ReadRomfs("pokenav/region_map/brendan_icon.4bpp", NULL);
    sRes.playerIcon[FEMALE] = ReadRomfs("pokenav/region_map/may_icon.4bpp", NULL);
    PalFile("pokenav/region_map/brendan_icon.gbapal", &sRes.playerIconPal[MALE], 1, FALSE);
    PalFile("pokenav/region_map/may_icon.gbapal", &sRes.playerIconPal[FEMALE], 1, FALSE);
    sRes.cursorTiles = UnlzFile("pokenav/region_map/cursor_small.4bpp.lz", NULL);
    PalFile("pokenav/region_map/cursor.gbapal", &sRes.cursorPal, 1, FALSE);

    PalFile("trainer_card/badges.gbapal", &sRes.badgePal, 1, FALSE);
    sRes.badgeTiles = UnlzFile("trainer_card/badges.4bpp.lz", NULL);

    LoadGenderResources();
    LoadBattleArt();
    BuildTypeColours();
    BuildRayquaza();

    for (int i = 0; i < CACHE_BATTLE; ++i)
        sCache[i] = malloc(sizeof(sCanvas));
    sRes.ready = sRes.partyTiles && sCache[CACHE_MENU] && sCache[CACHE_WIDE];
    if (!sRes.ready)
        CtrLog_Write(CTR_LOG_ERROR, "bottom: party menu graphics or canvas caches missing, screen stays off");
}

/*
 * Icons are read from RomFS, which on hardware is the slowest thing a frame
 * can do. Drawing only ever uses what is already cached; Prefetch loads at
 * most one missing icon per frame, and the screen is redrawn once they are in.
 */
static bool8 sIconBudget;
static u32 sIconClock;

/* Mon icons, one of their two 32x32 frames each: the party (SAVE,
 * NUZLOCKE, the party menu's, which move) and the wild list's page. */
#define MON_ICON_SLOTS 20
static struct
{
    u16 key;
    u32 age;
    u8 tiles[512];
} sMonIcons[MON_ICON_SLOTS];

static const u8 *MonIconFrame(u16 iconSpecies, bool8 deoxysForm, u8 frame)
{
    u16 key = iconSpecies | (deoxysForm ? 0x8000 : 0) | (frame ? 0x4000 : 0);
    int victim = 0;
    const u8 *src;

    for (int i = 0; i < MON_ICON_SLOTS; ++i)
    {
        if (sMonIcons[i].key == key && key != 0)
        {
            sMonIcons[i].age = ++sIconClock;
            return sMonIcons[i].tiles;
        }
        if (sMonIcons[i].age < sMonIcons[victim].age)
            victim = i;
    }
    if (iconSpecies >= SPECIES_EGG + 28 || iconSpecies == SPECIES_NONE || !sIconBudget)
        return NULL;
    sIconBudget = FALSE;
    src = Port_ResolveAssetPointer(gMonIconTable[iconSpecies]);
    if (!src)
        return NULL;
    /* Deoxys keeps its alternate form in the same file, 0x400 in. */
    memcpy(sMonIcons[victim].tiles, src + (deoxysForm ? 0x400 : 0) + (frame ? 0x200 : 0), 512);
    sMonIcons[victim].key = key;
    sMonIcons[victim].age = ++sIconClock;
    return sMonIcons[victim].tiles;
}

static const u8 *MonIcon(u16 iconSpecies, bool8 deoxysForm)
{
    return MonIconFrame(iconSpecies, deoxysForm, 0);
}

/* Item icons: 24x24 with their own palette. */
#define ITEM_ICON_SLOTS 24
static struct
{
    u16 item;
    u32 age;
    bool8 valid, loaded;
    u8 tiles[9 * 32];
    Pal pal;
} sItemIcons[ITEM_ICON_SLOTS];

static int ItemIcon(u16 item)
{
    int victim = 0;
    u32 size = 0;
    u8 *tiles;
    u16 *pal;

    for (int i = 0; i < ITEM_ICON_SLOTS; ++i)
    {
        if (sItemIcons[i].loaded && sItemIcons[i].item == item)
        {
            sItemIcons[i].age = ++sIconClock;
            return sItemIcons[i].valid ? i : -1;
        }
        if (sItemIcons[i].age < sItemIcons[victim].age)
            victim = i;
    }
    if (!sIconBudget)
        return -1;
    sIconBudget = FALSE;
    tiles = Unlz(GetItemIconPicOrPalette(item, 0), &size);
    pal = Unlz(GetItemIconPicOrPalette(item, 1), NULL);
    sItemIcons[victim].valid = tiles && pal && size >= sizeof(sItemIcons[victim].tiles);
    if (sItemIcons[victim].valid)
    {
        memcpy(sItemIcons[victim].tiles, tiles, sizeof(sItemIcons[victim].tiles));
        ToPals(&sItemIcons[victim].pal, pal, 1);
    }
    free(tiles);
    free(pal);
    sItemIcons[victim].item = item;
    sItemIcons[victim].loaded = TRUE;
    sItemIcons[victim].age = ++sIconClock;
    return sItemIcons[victim].valid ? victim : -1;
}

/* Front pictures (the summary's mon): the first of their frames, 64x64,
 * and the palette, normal or shiny. */
#define FRONT_SLOTS 2
static struct
{
    u16 species;
    bool8 shiny, valid, loaded;
    u32 age;
    u8 tiles[64 * 64 / 2];
    Pal pal;
} sFronts[FRONT_SLOTS];

static int FrontPic(u16 species, bool8 shiny)
{
    int victim = 0;
    u32 size = 0;
    u8 *tiles;
    u16 *pal;

    for (int i = 0; i < FRONT_SLOTS; ++i)
    {
        if (sFronts[i].loaded && sFronts[i].species == species && sFronts[i].shiny == shiny)
        {
            sFronts[i].age = ++sIconClock;
            return sFronts[i].valid ? i : -1;
        }
        if (sFronts[i].age < sFronts[victim].age)
            victim = i;
    }
    if (!sIconBudget || species == SPECIES_NONE || species >= NUM_SPECIES + 28)
        return -1;
    sIconBudget = FALSE;
    tiles = Unlz(gMonFrontPicTable[species].data, &size);
    pal = Unlz((shiny ? gMonShinyPaletteTable : gMonPaletteTable)[species].data, NULL);
    sFronts[victim].valid = tiles && pal && size >= sizeof(sFronts[victim].tiles);
    if (sFronts[victim].valid)
    {
        memcpy(sFronts[victim].tiles, tiles, sizeof(sFronts[victim].tiles));
        ToPals(&sFronts[victim].pal, pal, 1);
    }
    free(tiles);
    free(pal);
    sFronts[victim].species = species;
    sFronts[victim].shiny = shiny;
    sFronts[victim].loaded = TRUE;
    sFronts[victim].age = ++sIconClock;
    return sFronts[victim].valid ? victim : -1;
}

/* The bag's own picture, the frame of a pocket, male or female. */
static struct
{
    bool8 female, valid, loaded;
    u8 frame;
    u8 tiles[64 * 64 / 2];
    Pal pal;
} sBagPic;

static int BagPic(bool8 female, u8 frame)
{
    u32 size = 0;
    u8 *tiles;
    u16 *pal;

    if (sBagPic.loaded && sBagPic.female == female && sBagPic.frame == frame)
        return sBagPic.valid ? 0 : -1;
    if (!sIconBudget)
        return -1;
    sIconBudget = FALSE;
    tiles = Unlz(female ? gBagFemaleTiles : gBagMaleTiles, &size);
    pal = Unlz(gBagPalette, NULL);
    sBagPic.valid = tiles && pal && size >= (frame + 1) * sizeof(sBagPic.tiles);
    if (sBagPic.valid)
    {
        memcpy(sBagPic.tiles, tiles + frame * sizeof(sBagPic.tiles), sizeof(sBagPic.tiles));
        ToPals(&sBagPic.pal, pal, 1);
    }
    free(tiles);
    free(pal);
    sBagPic.female = female;
    sBagPic.frame = frame;
    sBagPic.loaded = TRUE;
    return sBagPic.valid ? 0 : -1;
}

static void DrawItemIcon(u16 item, int x, int y)
{
    int slot = ItemIcon(item);

    if (slot >= 0)
        DrawSprite(sItemIcons[slot].tiles, 3, 3, x, y, sItemIcons[slot].pal.c);
}

/* ------------------------------------------------------------------------ */
/* Battle art                                                               */
/* ------------------------------------------------------------------------ */

/*
 * The battle menus' backdrop and plates are the port's own pictures,
 * drawn smooth by scripts/gen_battle_art.py into bottom/battle.bin. A plate
 * takes its colour here - FIGHT's red, a move's type - so each pixel is
 * stored as what it adds over whatever lies under it:
 *
 *     out = under * (255 - alpha) / 255 + colour * A / 255 + ring * R / 255 + C
 *
 * (R only where a focused plate has its ring). Ids as the script's.
 */
enum
{
    BTA_BACKDROP = 0,
    BTA_FIGHT_WIDE = 16, BTA_FIGHT_FULL = 19, BTA_BALL = 22, BTA_BOTTOM = 25, BTA_MOVE = 28, BTA_CANCEL = 31,
    BTA_TARGET = 34,
    BTA_CLIP_FIGHT_WIDE = 40, BTA_CLIP_FIGHT_FULL = 41,
    BTA_ICON_BAG = 48, BTA_ICON_RUN, BTA_ICON_NEAR, BTA_ICON_BLOCK,
    BTA_PARTY_OK = 56, BTA_PARTY_STATUS, BTA_PARTY_FAINT, BTA_PARTY_EMPTY,
    BTA_COUNT = 64,
};
/* A plate's id plus its state. */
enum { BTA_NORMAL, BTA_FOCUS, BTA_PRESSED };
/* How far a pressed plate sinks. */
#define BTA_SINK 2

typedef struct
{
    s16 x, y;          /* the box, from the element's anchor */
    u16 w, h;
    u8 flags;
    const u8 *alpha, *a, *r, *c;   /* c: RGB565, little endian */
} BtaElem;

static struct
{
    u8 *file;
    BtaElem e[BTA_COUNT];
} sBta;

static u8 sExpand5[32], sExpand6[64];

static void LoadBattleArt(void)
{
    FILE *f = fopen("romfs:/bottom/battle.bin", "rb");
    long size;
    const u8 *p, *end;
    u16 count;

    for (int i = 0; i < 32; ++i)
        sExpand5[i] = i * 255 / 31;
    for (int i = 0; i < 64; ++i)
        sExpand6[i] = i * 255 / 63;
    if (!f)
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: bottom/battle.bin missing, battle menus stay plain");
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    sBta.file = size > 10 ? malloc(size) : NULL;
    if (!sBta.file || fread(sBta.file, 1, size, f) != (size_t)size || memcmp(sBta.file, "EM3DBTA1", 8) != 0)
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: bottom/battle.bin unreadable");
        free(sBta.file);
        sBta.file = NULL;
        fclose(f);
        return;
    }
    fclose(f);
    p = sBta.file + 8;
    end = sBta.file + size;
    count = p[0] | (p[1] << 8);
    p += 2;
    for (u16 n = 0; n < count && p + 10 <= end; ++n)
    {
        u8 id = p[0];
        BtaElem e = {
            .flags = p[1],
            .x = (s16)(p[2] | (p[3] << 8)), .y = (s16)(p[4] | (p[5] << 8)),
            .w = p[6] | (p[7] << 8), .h = p[8] | (p[9] << 8),
        };
        u32 area = (u32)e.w * e.h;

        p += 10;
        if (e.flags & 8)
        {
            e.c = p;
            p += area * 2;
        }
        else
        {
            e.alpha = p;
            p += area;
            if (!(e.flags & 4))
            {
                e.a = p;
                e.c = p + area;
                p += area * 3;
                if (e.flags & 2)
                {
                    e.r = p;
                    p += area;
                }
            }
        }
        if (p > end)
            break;
        if (id < BTA_COUNT)
            sBta.e[id] = e;
    }
    /* The backdrop is in the canvas' layout already: it is a cache. */
    sCache[CACHE_BATTLE] = (u16 *)sBta.e[BTA_BACKDROP].c;
}

typedef struct { u8 r, g, b; } Rgb;

static inline Rgb RgbOf(u16 c)
{
    return (Rgb){sExpand5[c >> 11], sExpand6[(c >> 5) & 63], sExpand5[c & 31]};
}

static inline u16 PackRgb(int r, int g, int b)
{
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* x / 255 for x up to 255 * 255 * 3, rounded. */
static inline int Div255(int x)
{
    return (x + 128 + ((x + 128) >> 8)) >> 8;
}

/* An element at its anchor, in `colour`, its ring (if any) in `ring`. */
static void DrawBta(u8 id, int ax, int ay, Rgb colour, Rgb ring)
{
    const BtaElem *e = id < BTA_COUNT ? &sBta.e[id] : NULL;
    int x0, y0, i0, i1, j0, j1;

    if (!e || !e->alpha || !e->a)
        return;
    x0 = ax + e->x + sOX;
    y0 = ay + e->y;
    i0 = sClipX0 - x0 > 0 ? sClipX0 - x0 : 0;
    j0 = sClipY0 - y0 > 0 ? sClipY0 - y0 : 0;
    i1 = sClipX1 - x0 < e->w ? sClipX1 - x0 : e->w;
    j1 = sClipY1 - y0 < e->h ? sClipY1 - y0 : e->h;
    for (int i = i0; i < i1; ++i)
    {
        u16 *dst = sDst + (x0 + i) * H + (H - 1 - (y0 + j0));

        for (int j = j0; j < j1; ++j, --dst)
        {
            u32 q = (u32)j * e->w + i;
            int al = e->alpha[q], A = e->a[q], R = e->r ? e->r[q] : 0;
            u16 c = e->c[q * 2] | (e->c[q * 2 + 1] << 8);
            Rgb under, add;

            if (!al && !A && !R && !c)
                continue;
            under = RgbOf(*dst);
            add = RgbOf(c);
            *dst = PackRgb(Div255(under.r * (255 - al) + colour.r * A + ring.r * R) + add.r,
                           Div255(under.g * (255 - al) + colour.g * A + ring.g * R) + add.g,
                           Div255(under.b * (255 - al) + colour.b * A + ring.b * R) + add.b);
        }
    }
}

/* An alpha mask's value at a point from its anchor (0 outside it). */
static u8 BtaAlpha(u8 id, int dx, int dy)
{
    const BtaElem *e = &sBta.e[id];

    dx -= e->x;
    dy -= e->y;
    if (!e->alpha || dx < 0 || dy < 0 || dx >= e->w || dy >= e->h)
        return 0;
    return e->alpha[dy * e->w + dx];
}

/* A coverage map (0-255 per pixel, w x h) in one colour, times a clip. */
static void DrawCoverage(const u8 *cover, int w, int h, int x, int y, Rgb colour, int opacity, u8 clip, int clipX,
                         int clipY)
{
    for (int i = 0; i < w; ++i)
    {
        int sx = x + i + sOX;

        if (sx < sClipX0 || sx >= sClipX1)
            continue;
        for (int j = 0; j < h; ++j)
        {
            int sy = y + j, al = cover[j * w + i];
            u16 *dst;
            Rgb under;

            if (!al || sy < sClipY0 || sy >= sClipY1)
                continue;
            if (clip != BTA_COUNT)
                al = Div255(al * BtaAlpha(clip, x + i - clipX, y + j - clipY));
            al = Div255(al * opacity);
            if (!al)
                continue;
            dst = sDst + sx * H + (H - 1 - sy);
            under = RgbOf(*dst);
            *dst = PackRgb(Div255(under.r * (255 - al) + colour.r * al), Div255(under.g * (255 - al) + colour.g * al),
                           Div255(under.b * (255 - al) + colour.b * al));
        }
    }
}

/* An item icon's shadow: its shape in black at a quarter. */
static void DrawItemIconShadow(u16 item, int x, int y)
{
    int slot = ItemIcon(item);
    static u8 cover[24 * 24];

    if (slot < 0)
        return;
    for (int t = 0; t < 9; ++t)
        for (int py = 0; py < 8; ++py)
            for (int px = 0; px < 8; ++px)
            {
                u8 v = (sItemIcons[slot].tiles[t * 32 + py * 4 + px / 2] >> ((px & 1) * 4)) & 15;

                cover[((t / 3) * 8 + py) * 24 + (t % 3) * 8 + px] = v ? 255 : 0;
            }
    DrawCoverage(cover, 24, 24, x, y, (Rgb){0, 0, 0}, 64, BTA_COUNT, 0, 0);
}

/*
 * FIGHT's watermark: Rayquaza, from its own front picture, as a smooth
 * silhouette 1.1 times its size, with its darkest pixels (its outline and
 * inner lines) as a second mask. Each screen pixel takes 4x4 samples of the
 * picture, softened by a [1 2 1] blur and read bilinearly, against a
 * threshold: an edge that follows the shape instead of its pixel steps.
 */
#define RAY_SIZE 70
static u8 sRayBody[RAY_SIZE * RAY_SIZE], sRayLines[RAY_SIZE * RAY_SIZE];

static float RaySample(const float *m, float u, float v)
{
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0, s = 0;

    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
            int x = x0 + dx, y = y0 + dy;
            float wgt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);

            if (x >= 0 && y >= 0 && x < 64 && y < 64)
                s += wgt * m[y * 64 + x];
        }
    return s;
}

static void BuildRayquaza(void)
{
    static float body[64 * 64], lines[64 * 64], soft[64 * 64];
    u8 *tiles = Unlz(gMonFrontPicTable[SPECIES_RAYQUAZA].data, NULL);
    u16 *pal = Unlz(gMonPaletteTable[SPECIES_RAYQUAZA].data, NULL);

    if (tiles && pal)
    {
        for (int t = 0; t < 64; ++t)
            for (int py = 0; py < 8; ++py)
                for (int px = 0; px < 8; ++px)
                {
                    u8 v = (tiles[t * 32 + py * 4 + px / 2] >> ((px & 1) * 4)) & 15;
                    int x = (t % 8) * 8 + px, y = (t / 8) * 8 + py;
                    u16 c = pal[v];
                    int sum = ((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)) * 255 / 31;

                    body[y * 64 + x] = v ? 1.0f : 0.0f;
                    lines[y * 64 + x] = v && sum < 200 ? 1.0f : 0.0f;
                }
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
            {
                float s = 0, n = 0;

                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        float wgt = (dx ? 1.0f : 2.0f) * (dy ? 1.0f : 2.0f);

                        if (x + dx >= 0 && y + dy >= 0 && x + dx < 64 && y + dy < 64)
                            s += wgt * body[(y + dy) * 64 + x + dx];
                        n += wgt;
                    }
                soft[y * 64 + x] = s / n;
            }
        for (int Y = 0; Y < RAY_SIZE; ++Y)
            for (int X = 0; X < RAY_SIZE; ++X)
            {
                int inBody = 0, inLines = 0;

                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                    {
                        float u = (X + (sx + 0.5f) / 4) * (64.0f / 70.4f) - 0.5f;
                        float v = (Y + (sy + 0.5f) / 4) * (64.0f / 70.4f) - 0.5f;

                        inBody += RaySample(soft, u, v) >= 0.47f;
                        inLines += RaySample(lines, u, v) >= 0.43f;
                    }
                sRayBody[Y * RAY_SIZE + X] = inBody * 255 / 16;
                sRayLines[Y * RAY_SIZE + X] = inLines * 255 / 16;
            }
    }
    free(tiles);
    free(pal);
}

/* Which of the three type label palettes each type uses
 * (pokemon_summary_screen.c's sMoveTypeToOamPaletteNum, minus 13). */
static const u8 sTypeIconPal[NUMBER_OF_MON_TYPES] = {
    [TYPE_NORMAL] = 0, [TYPE_FIGHTING] = 0, [TYPE_FLYING] = 1, [TYPE_POISON] = 1,
    [TYPE_GROUND] = 0, [TYPE_ROCK] = 0, [TYPE_BUG] = 2, [TYPE_GHOST] = 1,
    [TYPE_STEEL] = 0, [TYPE_MYSTERY] = 2, [TYPE_FIRE] = 0, [TYPE_WATER] = 1,
    [TYPE_GRASS] = 2, [TYPE_ELECTRIC] = 0, [TYPE_PSYCHIC] = 1, [TYPE_ICE] = 1,
    [TYPE_DRAGON] = 2, [TYPE_DARK] = 0,
};

/* A move plate's colour: the main colour of the type's own label, its most
 * used one that is neither its white nor its dark outline. */
static Rgb sTypeColour[NUMBER_OF_MON_TYPES];

static void BuildTypeColours(void)
{
    for (int type = 0; type < NUMBER_OF_MON_TYPES; ++type)
    {
        const u16 *pal = sRes.typePal[sTypeIconPal[type]].c;
        int count[16] = {0}, best = -1;

        sTypeColour[type] = (Rgb){150, 150, 150};
        if (!sRes.typeTiles)
            continue;
        for (int i = 0; i < 8 * 32; ++i)
        {
            u8 byte = sRes.typeTiles[type * 8 * 32 + i];

            ++count[byte & 15];
            ++count[byte >> 4];
        }
        for (int v = 1; v < 16; ++v)
        {
            Rgb c = RgbOf(pal[v]);

            if ((c.r > 230 && c.g > 230 && c.b > 230) || c.r + c.g + c.b <= 120)
                continue;
            if (best < 0 || count[v] > count[best])
                best = v;
        }
        if (best >= 0)
            sTypeColour[type] = RgbOf(pal[best]);
    }
}

/* ------------------------------------------------------------------------ */
/* Text                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct
{
    const u16 *glyphs;
    const u8 *widths;
    u8 height, lineHeight;
} Font;

/* The game's two fonts, and their narrow cuts for long translations. */
static Font sSmall, sNormal, sSmallNarrow, sNarrow;

static const u16 *ResolveFontBase(const u16 *glyphs)
{
    const void *resolved = Port_ResolveFontPointer(glyphs);

    if (resolved == glyphs)
        resolved = Port_ResolveAssetPointer(glyphs);
    return resolved;
}

/* The glyph payloads live in the asset cache; resolve them per redraw. */
static void ResolveFonts(void)
{
    sSmall.glyphs = ResolveFontBase(gFontSmallLatinGlyphs);
    sSmall.widths = gFontSmallLatinGlyphWidths;
    sSmall.height = 13;
    sSmall.lineHeight = 13;
    sNormal.glyphs = ResolveFontBase(gFontNormalLatinGlyphs);
    sNormal.widths = gFontNormalLatinGlyphWidths;
    sNormal.height = 16;
    sNormal.lineHeight = 16;
    sSmallNarrow.glyphs = ResolveFontBase(gFontSmallNarrowLatinGlyphs);
    sSmallNarrow.widths = gFontSmallNarrowLatinGlyphWidths;
    sSmallNarrow.height = 13;
    sSmallNarrow.lineHeight = 13;
    sNarrow.glyphs = ResolveFontBase(gFontNarrowLatinGlyphs);
    sNarrow.widths = gFontNarrowLatinGlyphWidths;
    sNarrow.height = 16;
    sNarrow.lineHeight = 16;
}

/* Walks a game string: returns the next glyph, 0xFFFE for a line break or
 * 0xFFFF at the end. */
static u16 NextGlyph(const u8 **str)
{
    for (;;)
    {
        u8 c = *(*str)++;

        switch (c)
        {
        case EOS:
            --*str;
            return 0xFFFF;
        case CHAR_NEWLINE:
        case CHAR_PROMPT_SCROLL:
        case CHAR_PROMPT_CLEAR:
            return 0xFFFE;
        case EXT_CTRL_CODE_BEGIN:
            if (**str == EOS)
                return 0xFFFF;
            *str += GetExtCtrlCodeLength(**str);
            break;
        case PLACEHOLDER_BEGIN:
        case CHAR_DYNAMIC:
        case CHAR_KEYPAD_ICON:
            if (**str != EOS)
                ++*str;
            break;
        case CHAR_EXTRA_SYMBOL:
            if (**str == EOS)
                return 0xFFFF;
            return 0x100 | *(*str)++;
        default:
            return c;
        }
    }
}

static int StrWidth(const Font *font, const u8 *str)
{
    int width = 0, best = 0;

    if (!str)
        return 0;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
    {
        if (g == 0xFFFE)
        {
            if (width > best) best = width;
            width = 0;
            continue;
        }
        width += font->widths[g];
    }
    return width > best ? width : best;
}

static int DrawGlyph(const Font *font, u16 glyph, int x, int y, u16 fg, u16 shadow)
{
    const u16 *base = font->glyphs + glyph * 0x20;
    int width = font->widths[glyph];
    int sx = x + sOX;
    bool8 inside = sx >= sClipX0 && y >= sClipY0 && sx + width <= sClipX1 && y + font->height <= sClipY1;
    u16 *origin = sDst + (inside ? sx * H + (H - 1 - y) : 0);

    /* Wholly outside the clip: nothing to write. */
    if (sx >= sClipX1 || y >= sClipY1 || sx + width <= sClipX0 || y + font->height <= sClipY0)
        return font->widths[glyph];
    if (width > 16)
        width = 16;
    for (int row = 0; row < font->height; ++row)
    {
        for (int half = 0; half < 2 && half * 8 < width; ++half)
        {
            u16 bits = base[(row >= 8 ? 0x10 : 0) + half * 8 + (row & 7)];

            if (!bits)
                continue;
            for (int k = 0; k < 8 && half * 8 + k < width; ++k)
            {
                u8 byte = k < 4 ? bits >> 8 : bits & 0xFF;
                u8 v = (byte >> (6 - 2 * (k & 3))) & 3;
                int px = half * 8 + k;

                if (v != 1 && v != 2)
                    continue;
                if (inside)
                    origin[px * H - row] = v == 1 ? fg : shadow;
                else
                    Put(x + px, y + row, v == 1 ? fg : shadow);
            }
        }
    }
    return font->widths[glyph];
}

/* Draws a game string; returns the x where it ended. */
static int DrawStr(const Font *font, const u8 *str, int x, int y, u16 fg, u16 shadow)
{
    int left = x;

    if (!font->glyphs || !str)
        return x;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
    {
        if (g == 0xFFFE)
        {
            x = left;
            y += font->lineHeight;
            continue;
        }
        x += DrawGlyph(font, g, x, y, fg, shadow);
    }
    return x;
}

static void DrawStrRight(const Font *font, const u8 *str, int right, int y, u16 fg, u16 shadow)
{
    DrawStr(font, str, right - StrWidth(font, str), y, fg, shadow);
}

static void DrawStrCentered(const Font *font, const u8 *str, int cx, int y, u16 fg, u16 shadow)
{
    DrawStr(font, str, cx - StrWidth(font, str) / 2, y, fg, shadow);
}

/* Whether a glyph paints (glyph or shadow) the pixel at column px of row. */
static bool8 GlyphInk(const Font *font, u16 glyph, int px, int row)
{
    u16 bits = font->glyphs[glyph * 0x20 + (row >= 8 ? 0x10 : 0) + (px / 8) * 8 + (row & 7)];
    u8 byte = (px & 7) < 4 ? bits >> 8 : bits & 0xFF;
    u8 v = (byte >> (6 - 2 * (px & 3))) & 3;

    return v == 1 || v == 2;
}

/* The columns a one-line string really paints, [*x0, *x1) from its origin;
 * *x1 <= *x0 when it paints none. */
static void InkColumns(const Font *font, const u8 *str, int *x0, int *x1)
{
    int x = 0;

    *x0 = 0x7FFF;
    *x1 = 0;
    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF && g != 0xFFFE;)
    {
        int width = font->widths[g] > 16 ? 16 : font->widths[g];

        for (int px = 0; px < width; ++px)
            for (int row = 0; row < font->height; ++row)
                if (GlyphInk(font, g, px, row))
                {
                    if (x + px < *x0) *x0 = x + px;
                    if (x + px + 1 > *x1) *x1 = x + px + 1;
                    break;
                }
        x += font->widths[g];
    }
}

/* The rows a capital paints, [*top, *bottom): every label is centred on the
 * same band, so one with an accent (POKéMON) keeps the others' baseline. */
static void CapRows(const Font *font, int *top, int *bottom)
{
    *top = font->height;
    *bottom = 0;
    for (int row = 0; row < font->height; ++row)
        for (int px = 0; px < font->widths[CHAR_H]; ++px)
            if (GlyphInk(font, CHAR_H, px, row))
            {
                if (row < *top) *top = row;
                if (row + 1 > *bottom) *bottom = row + 1;
                break;
            }
}

/* A one-line string centred in [x0, x1) x [y0, y1): across by the pixels it
 * paints, down by the capitals' band. */
static void DrawStrIn(const Font *font, const u8 *str, int x0, int x1, int y0, int y1, u16 fg, u16 shadow)
{
    int ix0, ix1, top, bottom;

    if (!font->glyphs || !str)
        return;
    InkColumns(font, str, &ix0, &ix1);
    if (ix1 <= ix0)
        return;
    CapRows(font, &top, &bottom);
    DrawStr(font, str, x0 + ((x1 - x0) - (ix1 - ix0)) / 2 - ix0, y0 + ((y1 - y0) - (bottom - top)) / 2 - top, fg,
            shadow);
}

/* Latin labels for the few words the game has no standalone string for. */
static const u8 *Ascii(const char *text)
{
    static u8 ring[8][40];
    static u8 next;
    u8 *out = ring[next++ & 7];
    CtrLocale_ToGame(out, sizeof(ring[0]), text);
    return out;
}

static const u8 *Number(u32 value, int digits, enum StringConvertMode mode)
{
    static u8 ring[8][12];
    static u8 next;
    u8 *out = ring[next++ & 7];

    ConvertIntToDecimalStringN(out, value, mode, digits);
    return out;
}

/*
 * The battle plates' labels: the game's glyphs, enlarged by 1, 1.5, 2 or
 * 2.5 (k = 4, 6, 8, 10 quarter pixels a glyph pixel), with a smooth round
 * outline and, for some, a soft drop shadow, as gen_battle_art.py's mockups
 * draw them. Built at 4x - glyphs, a blurred-and-hardened outline merged with
 * a dilation, the outline moved down and blurred for the shadow - then
 * averaged down to screen pixels. A label is built once and kept.
 */
#define SMOOTH_PAD 4    /* glyph pixels around the text, for outline and shadow */
#define SMOOTH_SLOTS 28

typedef struct
{
    u32 age;
    u8 key[40], font, k;
    Rgb fg, outline, shadow;
    bool8 hasShadow;
    u16 w, h;
    u8 *px;            /* per pixel: coverage, then premultiplied r, g, b */
} SmoothText;

static SmoothText sSmooth[SMOOTH_SLOTS];
static u32 sSmoothClock;
/* How many labels or plates a redraw may still build (0xFF: any). The warm
 * up while the turn plays out builds one a frame (WarmBattleMenus). */
static u8 sBuildBudget = 0xFF;
static bool8 sCompBad;   /* a plate picture being made hit something not built yet */

static bool8 TakeBuildBudget(void)
{
    if (sBuildBudget == 0)
    {
        sCompBad = TRUE;
        return FALSE;
    }
    if (sBuildBudget != 0xFF)
        --sBuildBudget;
    return TRUE;
}

/*
 * Three box blurs in a row approximate a Gaussian. Each sweep is its own
 * step so a label can be built over several frames (SmoothJob). Neither
 * sweep divides: this CPU has no divide instruction, and a division per
 * pixel per sweep was most of what a label cost. The mean is a multiply by
 * the span's reciprocal (rounded up, so a full window stays 255).
 */
static void BlurRows(const u8 *src, u8 *dst, int w, int h, int radius, int y0, int y1)
{
    int span = 2 * radius + 1;

    (void)h;
    u32 inv = (65536u + span - 1) / span;

    for (int y = y0; y < y1; ++y)
    {
        const u8 *row = src + y * w;
        u8 *out = dst + y * w;
        int sum = 0, x = 0;

        for (int i = -radius; i <= radius; ++i)
            sum += row[i < 0 ? 0 : (i >= w ? w - 1 : i)];
        /* The left edge and the right edge clamp; the middle needs no test. */
        for (; x < w && x < radius; ++x)
        {
            int add = x + radius + 1;

            out[x] = (u8)(((u32)sum * inv) >> 16);
            sum += row[add >= w ? w - 1 : add] - row[0];
        }
        for (; x < w && x + radius + 1 < w; ++x)
        {
            out[x] = (u8)(((u32)sum * inv) >> 16);
            sum += row[x + radius + 1] - row[x - radius];
        }
        for (; x < w; ++x)
        {
            int sub = x - radius;

            out[x] = (u8)(((u32)sum * inv) >> 16);
            sum += row[w - 1] - row[sub < 0 ? 0 : sub];
        }
    }
}

/* Down the columns, a row at a time: the sums of every column are kept in
 * `sums`, so memory is read in order instead of a column per stride. */
static void BlurColumns(const u8 *src, u8 *dst, int w, int h, int radius, int *sums, int x0, int x1)
{
    int span = 2 * radius + 1;
    u32 inv = (65536u + span - 1) / span;

    memset(sums + x0, 0, (size_t)(x1 - x0) * sizeof(int));
    for (int i = -radius; i <= radius; ++i)
    {
        const u8 *row = src + (i < 0 ? 0 : (i >= h ? h - 1 : i)) * w;

        for (int x = x0; x < x1; ++x)
            sums[x] += row[x];
    }
    for (int y = 0; y < h; ++y)
    {
        u8 *out = dst + y * w;
        int add = y + radius + 1, sub = y - radius;
        const u8 *addRow = src + (add >= h ? h - 1 : add) * w, *subRow = src + (sub < 0 ? 0 : sub) * w;

        for (int x = x0; x < x1; ++x)
        {
            out[x] = (u8)(((u32)sums[x] * inv) >> 16);
            sums[x] += addRow[x] - subRow[x];
        }
    }
}

static int GaussRadius(float sigma)
{
    return (int)((sqrtf(4.0f * sigma * sigma + 1.0f) - 1.0f) / 2.0f + 0.5f);
}

/* A square dilation of a 0/255 mask, separable: a running count of the set
 * pixels in the window, so its cost does not grow with the radius. Rows of
 * the first sweep and columns of the second are independent: a band at a time. */
static void DilateRows(const u8 *src, u8 *tmp, int w, int radius, int y0, int y1)
{
    for (int y = y0; y < y1; ++y)
    {
        const u8 *row = src + y * w;
        int count = 0;

        for (int x = 0; x < radius && x < w; ++x)
            count += row[x] != 0;
        for (int x = 0; x < w; ++x)
        {
            if (x + radius < w)
                count += row[x + radius] != 0;
            if (x - radius - 1 >= 0)
                count -= row[x - radius - 1] != 0;
            tmp[y * w + x] = count ? 255 : 0;
        }
    }
}

static void DilateColumns(const u8 *tmp, u8 *dst, int w, int h, int radius, int x0, int x1)
{
    for (int x = x0; x < x1; ++x)
    {
        int count = 0;

        for (int y = 0; y < radius && y < h; ++y)
            count += tmp[y * w + x] != 0;
        for (int y = 0; y < h; ++y)
        {
            if (y + radius < h)
                count += tmp[(y + radius) * w + x] != 0;
            if (y - radius - 1 >= 0)
                count -= tmp[(y - radius - 1) * w + x] != 0;
            dst[y * w + x] = count ? 255 : 0;
        }
    }
}

/*
 * A label's build, in steps. In the frames the battle's turn plays out the
 * next menus' labels are made ready (WarmBattleMenus) and a big one takes
 * tens of milliseconds at once: so it runs a step or several at a time up
 * to sSliceMs milliseconds after sSliceStart (0: no limit) and the rest
 * comes next frame. The job keeps its buffers between frames.
 */
enum
{
    SJ_GLYPHS,
    SJ_RING_BLUR,       /* six sweeps: three box blurs, rows then columns each */
    SJ_RING_HARDEN = SJ_RING_BLUR + 6,
    SJ_DILATE_ROWS,     /* the sweeps of a blur or a dilation are SJ_PARTS steps each */
    SJ_DILATE_COLUMNS,
    SJ_RING_MERGE,
    SJ_SHADOW_SHIFT,
    SJ_SHADOW_BLUR,     /* six sweeps */
    SJ_SHADOW_FADE = SJ_SHADOW_BLUR + 6,
    SJ_DOWN,            /* screen pixels, a band of rows a step */
    SJ_DONE,
};

/* Steps of a few tenths of a millisecond on an Old 3DS at the largest k
 * (a label of ~100k quarter pixels): a band of 24 rows down, or a whole
 * image's pass, was 1-3 ms in one step, past any slice. */
#define SJ_BAND 4
#define SJ_PARTS 8
#define SJ_GLYPHS_A_STEP 3

typedef struct
{
    SmoothText *t;
    const Font *font;
    u8 stage, k, part;
    u16 row;
    u16 strAt;          /* SJ_GLYPHS: the next glyph's byte in the key (0: the first step) */
    int glyphX;         /* ... and where it goes */
    int w4, h4, ringRadius, shadowRadius;
    u8 *glyph, *ring, *shadow, *tmp;
    int *sums;
} SmoothJob;

static SmoothJob sJob;
static uint64_t sSliceStart;
static float sSliceMs;   /* 0: no limit */

static void AbortSmoothJob(void)
{
    if (sJob.t)
    {
        sJob.t->age = 0;
        free(sJob.t->px);
        sJob.t->px = NULL;
    }
    free(sJob.glyph);
    free(sJob.sums);
    memset(&sJob, 0, sizeof(sJob));
}

static bool8 StartSmoothJob(SmoothText *t, const Font *font)
{
    int textW = StrWidth(font, t->key), k = t->k;
    int r = k > 4 ? k : 4;

    AbortSmoothJob();
    free(t->px);
    t->px = NULL;
    sJob.t = t;
    sJob.font = font;
    sJob.k = k;
    sJob.w4 = (textW + 2 * SMOOTH_PAD) * k;
    sJob.h4 = (font->height + 2 * SMOOTH_PAD) * k;
    sJob.ringRadius = GaussRadius(r * 0.55f);
    sJob.shadowRadius = GaussRadius(3.2f);
    sJob.glyph = calloc((size_t)sJob.w4 * sJob.h4, 4);
    sJob.sums = malloc((size_t)sJob.w4 * sizeof(int));
    if (!sJob.glyph || !sJob.sums)
    {
        free(sJob.glyph);
        free(sJob.sums);
        memset(&sJob, 0, sizeof(sJob));
        t->age = 0;
        return FALSE;
    }
    sJob.ring = sJob.glyph + sJob.w4 * sJob.h4;
    sJob.shadow = sJob.ring + sJob.w4 * sJob.h4;
    sJob.tmp = sJob.shadow + sJob.w4 * sJob.h4;
    t->age = 0xFFFFFFFF;   /* never the slot the next build takes over */
    return TRUE;
}

static void StepSmoothJob(void)
{
    SmoothText *t = sJob.t;
    int w4 = sJob.w4, h4 = sJob.h4, k = sJob.k;
    u8 *glyph = sJob.glyph, *ring = sJob.ring, *shadow = sJob.shadow, *tmp = sJob.tmp;

    if (sJob.stage == SJ_GLYPHS)
    {
        const Font *font = sJob.font;
        const u8 *str = t->key + sJob.strAt;
        int x = sJob.strAt ? sJob.glyphX : SMOOTH_PAD;

        for (int n = 0;; ++n)
        {
            const u8 *at = str;
            u16 g = NextGlyph(&str);

            if (g == 0xFFFF || g == 0xFFFE)
                break;
            if (n == SJ_GLYPHS_A_STEP)
            {
                /* The rest next step, from this glyph on. */
                sJob.strAt = (u16)(at - t->key);
                sJob.glyphX = x;
                return;
            }
            const u16 *base = font->glyphs + g * 0x20;
            int width = font->widths[g] > 16 ? 16 : font->widths[g];

            for (int row = 0; row < font->height; ++row)
                for (int px = 0; px < width; ++px)
                {
                    u16 bits = base[(row >= 8 ? 0x10 : 0) + (px / 8) * 8 + (row & 7)];
                    u8 byte = (px & 7) < 4 ? bits >> 8 : bits & 0xFF;

                    if (((byte >> (6 - 2 * (px & 3))) & 3) != 1)
                        continue;
                    for (int dy = 0; dy < k; ++dy)
                        memset(glyph + ((SMOOTH_PAD + row) * k + dy) * w4 + (x + px) * k, 255, k);
                }
            x += font->widths[g];
        }
        /* The outline: blurred and hardened, and never thinner than a dilation. */
        memcpy(ring, glyph, (size_t)w4 * h4);
    }
    else if (sJob.stage >= SJ_RING_BLUR && sJob.stage < SJ_RING_HARDEN)
    {
        int p = sJob.part;

        if ((sJob.stage - SJ_RING_BLUR) & 1)
            BlurColumns(tmp, ring, w4, h4, sJob.ringRadius, sJob.sums, w4 * p / SJ_PARTS, w4 * (p + 1) / SJ_PARTS);
        else
            BlurRows(ring, tmp, w4, h4, sJob.ringRadius, h4 * p / SJ_PARTS, h4 * (p + 1) / SJ_PARTS);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_RING_HARDEN)
    {
        int p = sJob.part, end = w4 * h4 * (p + 1) / SJ_PARTS;

        for (int i = w4 * h4 * p / SJ_PARTS; i < end; ++i)
            ring[i] = ring[i] > 18 ? 255 : ring[i] * 14;
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_DILATE_ROWS)
    {
        int p = sJob.part;

        DilateRows(glyph, tmp, w4, (int)((k > 4 ? k : 4) * 0.7f + 0.5f), h4 * p / SJ_PARTS, h4 * (p + 1) / SJ_PARTS);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_DILATE_COLUMNS)
    {
        int p = sJob.part;

        DilateColumns(tmp, shadow, w4, h4, (int)((k > 4 ? k : 4) * 0.7f + 0.5f), w4 * p / SJ_PARTS, w4 * (p + 1) / SJ_PARTS);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_RING_MERGE)
    {
        int p = sJob.part, begin = w4 * h4 * p / SJ_PARTS, end = w4 * h4 * (p + 1) / SJ_PARTS;

        for (int i = begin; i < end; ++i)
            if (shadow[i] > ring[i])
                ring[i] = shadow[i];
        memset(shadow + begin, 0, (size_t)(end - begin));
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_SHADOW_SHIFT)
    {
        if (t->hasShadow)
        {
            int drop = (int)(4.8f * (k > 5 ? k / 5.0f : 1.0f) + 0.5f);

            for (int y = drop; y < h4; ++y)
                memcpy(shadow + y * w4, ring + (y - drop) * w4, w4);
        }
        else
            sJob.stage = SJ_SHADOW_FADE;   /* nothing to blur or fade: the ++ below leaves it */
    }
    else if (sJob.stage >= SJ_SHADOW_BLUR && sJob.stage < SJ_SHADOW_FADE)
    {
        int p = sJob.part;

        if ((sJob.stage - SJ_SHADOW_BLUR) & 1)
            BlurColumns(tmp, shadow, w4, h4, sJob.shadowRadius, sJob.sums, w4 * p / SJ_PARTS, w4 * (p + 1) / SJ_PARTS);
        else
            BlurRows(shadow, tmp, w4, h4, sJob.shadowRadius, h4 * p / SJ_PARTS, h4 * (p + 1) / SJ_PARTS);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_SHADOW_FADE)
    {
        int p = sJob.part, end = w4 * h4 * (p + 1) / SJ_PARTS;

        /* x * 6 / 10 for x < 256, without a divide (the ARM11 has none). */
        for (int i = w4 * h4 * p / SJ_PARTS; i < end; ++i)
            shadow[i] = (u8)((shadow[i] * 39322u) >> 16);
        if (++sJob.part < SJ_PARTS)
            return;
    }
    else if (sJob.stage == SJ_DOWN)
    {
        /* Down to screen pixels: coverage and premultiplied colour. */
        if (!sJob.row)
        {
            t->w = (w4 + 3) / 4;
            t->h = (h4 + 3) / 4;
            t->px = calloc((size_t)t->w * t->h, 4);
            if (!t->px)
            {
                AbortSmoothJob();
                return;
            }
        }
        for (int oy = sJob.row; oy < t->h && oy < sJob.row + SJ_BAND; ++oy)
            for (int ox = 0; ox < t->w; ++ox)
            {
                int a = 0, cr = 0, cg = 0, cb = 0;

                for (int sy = oy * 4; sy < oy * 4 + 4 && sy < h4; ++sy)
                    for (int sx = ox * 4; sx < ox * 4 + 4 && sx < w4; ++sx)
                    {
                        int i = sy * w4 + sx, ri = ring[i], al = ri > shadow[i] ? ri : shadow[i];
                        Rgb c;

                        if (!al)
                            continue;
                        if (glyph[i])
                            c = t->fg;
                        else
                            c = (Rgb){(u8)Div255(t->shadow.r * (255 - ri) + t->outline.r * ri),
                                      (u8)Div255(t->shadow.g * (255 - ri) + t->outline.g * ri),
                                      (u8)Div255(t->shadow.b * (255 - ri) + t->outline.b * ri)};
                        a += al;
                        cr += c.r * al;
                        cg += c.g * al;
                        cb += c.b * al;
                    }
                t->px[(oy * t->w + ox) * 4 + 0] = a / 16;
                t->px[(oy * t->w + ox) * 4 + 1] = cr / (255 * 16);
                t->px[(oy * t->w + ox) * 4 + 2] = cg / (255 * 16);
                t->px[(oy * t->w + ox) * 4 + 3] = cb / (255 * 16);
            }
        sJob.row += SJ_BAND;
        if (sJob.row < t->h)
            return;   /* the next band, same stage */
    }
    sJob.part = 0;
    ++sJob.stage;
}

/* Runs the job until it is done or the slice is over; TRUE once t->px is
 * ready. A job that cannot go on is dropped and its slot left empty. */
static bool8 RunSmoothJob(void)
{
    while (sJob.t && sJob.stage < SJ_DONE)
    {
        StepSmoothJob();
        if (sJob.t && sJob.stage < SJ_DONE && sSliceMs > 0.0f && CtrPlatform_TickMs(CtrPlatform_Ticks() - sSliceStart) >= sSliceMs)
            return FALSE;
    }
    if (!sJob.t)
        return FALSE;
    free(sJob.glyph);
    free(sJob.sums);
    memset(&sJob, 0, sizeof(sJob));
    return TRUE;
}

/*
 * A whole redraw (a new mode: the action menu, the field's column) builds
 * the labels it lacks for up to sRedrawLabelMs from sRedrawStart; past it a
 * missing label is drawn flat (DrawFlatStr) for now and sRefineLabels asks
 * for the redraw again next frame, which carries the half-built label on.
 * All of them at once was 6-13 ms in one frame on an Old 3DS. 0: no limit.
 */
static float sRedrawLabelMs;
static uint64_t sRedrawStart;
static bool8 sRefineLabels;
/* This redraw has built some of a label. Each redraw builds at least one
 * slice, however long its plates took: on an Old 3DS the move menu's took
 * the whole 2 ms before its first label, every label stayed flat, and the
 * menu was drawn again every frame for as long as it was open (12 ms a
 * frame, 30 fps). */
static bool8 sRedrawLabelWorked;
/* Labels found built, and built or left flat, since the last report. */
static u32 sLabelHits, sLabelMisses;
#define REDRAW_LABEL_MS 2.0f

/* A label's stand-in: its glyphs in their colour over a one-pixel outline,
 * no blur, no shadow, at the size the built one has. */
static void DrawFlatStr(const Font *font, const u8 *str, int x, int y, int k, Rgb fg, Rgb outline)
{
    u16 colours[2] = {PackRgb(outline.r, outline.g, outline.b), PackRgb(fg.r, fg.g, fg.b)};

    for (int pass = 0; pass < 2; ++pass)
    {
        const u8 *s = str;
        int gx = 0;

        for (u16 g; (g = NextGlyph(&s)) != 0xFFFF && g != 0xFFFE;)
        {
            const u16 *base = font->glyphs + g * 0x20;
            int width = font->widths[g] > 16 ? 16 : font->widths[g];

            for (int row = 0; row < font->height; ++row)
                for (int px = 0; px < width; ++px)
                {
                    u16 bits = base[(row >= 8 ? 0x10 : 0) + (px / 8) * 8 + (row & 7)];
                    u8 byte = (px & 7) < 4 ? bits >> 8 : bits & 0xFF;
                    int x0 = x + ((gx + px) * k >> 2), y0 = y + (row * k >> 2);
                    int x1 = x + ((gx + px + 1) * k >> 2), y1 = y + ((row + 1) * k >> 2);

                    if (((byte >> (6 - 2 * (px & 3))) & 3) != 1)
                        continue;
                    if (pass == 0)
                        FillRect(x0 - 1, y0 - 1, x1 - x0 + 2, y1 - y0 + 2, colours[0]);
                    else
                        FillRect(x0, y0, x1 - x0, y1 - y0, colours[1]);
                }
            gx += font->widths[g];
        }
    }
}

/* Draws a label with its glyph origin at (x, y); k quarter pixels a glyph pixel. */
static void DrawSmoothStr(const Font *font, const u8 *str, int x, int y, int k, Rgb fg, Rgb outline,
                          const Rgb *shadow)
{
    SmoothText *t = NULL, *victim = &sSmooth[0];
    int n = 0;

    if (!font->glyphs || !str)
        return;
    while (str[n] != EOS && n < (int)sizeof(t->key) - 1)
        ++n;
    for (int i = 0; i < SMOOTH_SLOTS && !t; ++i)
    {
        SmoothText *s = &sSmooth[i];

        if (s->px && s != sJob.t && s->font == (font == &sSmall) && s->k == k && memcmp(s->key, str, n) == 0 && s->key[n] == EOS
         && !memcmp(&s->fg, &fg, sizeof(fg)) && !memcmp(&s->outline, &outline, sizeof(outline))
         && s->hasShadow == (shadow != NULL) && (!shadow || !memcmp(&s->shadow, shadow, sizeof(*shadow))))
            t = s;
        else if (s->age < victim->age)
            victim = s;
    }
    if (!t)
    {
        SmoothText *job = sJob.t;
        /* A whole redraw's own build, within its time (sRedrawLabelMs); never
         * into a cache kept beyond the frame, which would keep it flat. */
        bool8 timed = sRedrawLabelMs > 0.0f && sBuildBudget == 0xFF && sDst == sCanvas;

        bool8 late = timed && CtrPlatform_TickMs(CtrPlatform_Ticks() - sRedrawStart) >= sRedrawLabelMs;

        ++sLabelMisses;
        if (late && sRedrawLabelWorked)
        {
            DrawFlatStr(font, str, x, y, k, fg, outline);
            sRefineLabels = TRUE;
            return;
        }
        if (!TakeBuildBudget())
            return;
        /* The label an earlier slice left half built goes on; any other
         * label drops it and starts over. */
        if (job && job->font == (font == &sSmall) && job->k == k && memcmp(job->key, str, n) == 0 && job->key[n] == EOS
         && !memcmp(&job->fg, &fg, sizeof(fg)) && !memcmp(&job->outline, &outline, sizeof(outline))
         && job->hasShadow == (shadow != NULL) && (!shadow || !memcmp(&job->shadow, shadow, sizeof(*shadow))))
            t = job;
        else
        {
            t = victim;
            memcpy(t->key, str, n);
            t->key[n] = EOS;
            t->font = font == &sSmall;
            t->k = k;
            t->fg = fg;
            t->outline = outline;
            t->hasShadow = shadow != NULL;
            t->shadow = shadow ? *shadow : outline;
            if (!StartSmoothJob(t, font))
                return;
        }
        if (timed)
        {
            float slice = sSliceMs;
            uint64_t sliceStart = sSliceStart;
            bool8 done;

            /* Past the redraw's time, still this one slice of its own. */
            sSliceMs = sRedrawLabelMs;
            sSliceStart = late ? CtrPlatform_Ticks() : sRedrawStart;
            sRedrawLabelWorked = TRUE;
            done = RunSmoothJob();
            sSliceMs = slice;
            sSliceStart = sliceStart;
            if (!done)
            {
                if (sJob.t)
                {
                    DrawFlatStr(font, str, x, y, k, fg, outline);
                    sRefineLabels = TRUE;
                }
                return;
            }
        }
        else if (!RunSmoothJob())
            return;
    }
    else
        ++sLabelHits;
    t->age = ++sSmoothClock;
    /* The bitmap starts SMOOTH_PAD glyph pixels up and left: k screen pixels. */
    x -= k;
    y -= k;
    for (int i = 0; i < t->w; ++i)
    {
        int sx = x + i + sOX;

        if (sx < sClipX0 || sx >= sClipX1)
            continue;
        for (int j = 0; j < t->h; ++j)
        {
            const u8 *p = t->px + (j * t->w + i) * 4;
            int sy = y + j;
            u16 *dst;
            Rgb under;

            if (!p[0] || sy < sClipY0 || sy >= sClipY1)
                continue;
            dst = sDst + sx * H + (H - 1 - sy);
            under = RgbOf(*dst);
            *dst = PackRgb(Div255(under.r * (255 - p[0])) + p[1], Div255(under.g * (255 - p[0])) + p[2],
                           Div255(under.b * (255 - p[0])) + p[3]);
        }
    }
}

/* The width a label's pixels span, enlarged k quarter pixels a glyph pixel. */
static int SmoothInkWidth(const Font *font, const u8 *str, int k)
{
    int x0, x1;

    InkColumns(font, str, &x0, &x1);
    return x1 > x0 ? (x1 - x0) * k / 4 : 0;
}

/* Text colours from the message box palette. */
#define TXT(i) (sRes.text.c[(i)])
#define TXT_WHITE TXT(TEXT_COLOR_WHITE)
#define TXT_DARK TXT(TEXT_COLOR_DARK_GRAY)
#define TXT_LIGHT TXT(TEXT_COLOR_LIGHT_GRAY)
#define TXT_RED TXT(TEXT_COLOR_RED)
#define TXT_LRED TXT(TEXT_COLOR_LIGHT_RED)
#define TXT_BLUE TXT(TEXT_COLOR_BLUE)
#define TXT_LBLUE TXT(TEXT_COLOR_LIGHT_BLUE)

/* ------------------------------------------------------------------------ */
/* Frames and backgrounds                                                   */
/* ------------------------------------------------------------------------ */

/*
 * The party menu background, framed on all four sides: the GBA screen leaves
 * its right edge open because the picture runs off it, here the left border
 * is mirrored instead.
 */
static void DrawPartyBackground(int cols, int rows)
{
    FillRect(0, 0, cols * 8, rows * 8, sRes.partyPal[0].c[0]);
    for (int ty = 0; ty < rows; ++ty)
    {
        for (int tx = 0; tx < cols; ++tx)
        {
            int edge = tx < cols / 2 ? tx : cols - 1 - tx;
            u16 flip = tx < cols / 2 ? 0 : 0x400, t;

            if (edge < 2) t = 0x002, flip = 0;
            else if (edge > 2) t = ty == 0 ? 0x005 : ty == rows - 1 ? 0x008 : 0x00E, flip = 0;
            else if (ty == 0) t = 0x004;
            else if (ty == 1) t = 0x006;
            else if (ty == rows - 2) t = 0x009;
            else if (ty == rows - 1) t = 0x007;
            else t = 0x00E;
            DrawMapEntry(sRes.partyTiles, sRes.partyTileCount, 0x1000 | flip | t, tx * 8, ty * 8, sRes.partyPal);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* The menu's look                                                          */
/* ------------------------------------------------------------------------ */

#include "3ds_bottom_plate.inc"

/* The port's art palettes (3ds_bottom_art.h). */
static Pal sArtPal, sArtPal2;

static void InitLook(void)
{
    ToPals(&sArtPal, sArtPalette, 1);
    ToPals(&sArtPal2, sArtPalette2, 1);
    InitStyles();
}

/* n / 2 rounded half to even, as the mockups' layout rounds. */
static int Half(int n)
{
    int q = n >= 0 ? n / 2 : -((1 - n) / 2);

    if ((n & 1) && (q & 1))
        ++q;
    return q;
}

/* The box of a sprite's visible pixels, [x0, x1) x [y0, y1). */
static void SpriteBox(const u8 *tiles, int wt, int ht, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = wt * 8;
    *y0 = ht * 8;
    *x1 = *y1 = 0;
    for (int y = 0; y < ht * 8; ++y)
        for (int x = 0; x < wt * 8; ++x)
        {
            const u8 *tile = tiles + ((y / 8) * wt + x / 8) * 32;
            int px = x & 7, py = y & 7;

            if (!((tile[py * 4 + px / 2] >> ((px & 1) * 4)) & 15))
                continue;
            if (x < *x0) *x0 = x;
            if (y < *y0) *y0 = y;
            if (x + 1 > *x1) *x1 = x + 1;
            if (y + 1 > *y1) *y1 = y + 1;
        }
}

/* A sprite with its visible pixels centred on the point whose doubled
 * coordinates are (cx2, cy2). */
static void DrawSpriteCentred(const u8 *tiles, int wt, int ht, int cx2, int cy2, const u16 *pal)
{
    int x0, y0, x1, y1;

    SpriteBox(tiles, wt, ht, &x0, &y0, &x1, &y1);
    if (x1 > x0)
        DrawSprite(tiles, wt, ht, Half(cx2 - x0 - x1), Half(cy2 - y0 - y1), pal);
}

/* ------------------------------------------------------------------------ */
/* View state                                                               */
/* ------------------------------------------------------------------------ */

enum
{
    MODE_OFF,
    MODE_FIELD,
    MODE_PARTY_MENU,
    MODE_BAG_MENU,
    /* The PokéNav, drawn by the compositor in the game screen's window. */
    MODE_POKENAV,
    /* The PC's boxes, drawn by the compositor over the whole screen. */
    MODE_STORAGE,
    /* The game's Pokédex, drawn by the compositor in its window. */
    MODE_POKEDEX,
    /* The summary opened from the field's party menu, drawn here. */
    MODE_SUMMARY,
    /* The game's trainer card (classic menus), drawn by the compositor in
     * the game screen's window. */
    MODE_CARD,
    MODE_BATTLE_INFO,
    MODE_BATTLE_ACTION,
    MODE_BATTLE_MOVE,
    MODE_BATTLE_TARGET,
};

typedef struct
{
    u16 species;      /* SPECIES_NONE: empty slot */
    u16 iconSpecies;
    u8 deoxys, isEgg, level, gender, ailment, fainted;
    u16 hp, maxHp;
    u8 nick[POKEMON_NAME_LENGTH + 2];
} MonView;

/* A row of the map's wild Pokemon list (map extras): a species of one
 * method's table, its slots' levels and chances added up. */
#define ENC_ROWS 12   /* a method's rows, at most */
typedef struct
{
    u16 species;
    u8 minLevel, maxLevel, percent, seen;
} EncRow;

/* The wild list's rows a page. */
#define ENC_SHOWN 6
/* The OPTIONS group's rows, at most. */
#define OPT_ROWS_SHOWN 6
/* What the NUZLOCKE section writes out: an area's name, short lines. */
#define NUZ_TEXT 40

/* Everything a redraw reads. Zeroed before every snapshot: memcmp-safe. */
typedef struct
{
    u8 mode, screen, pressed, gender, inBattle;
    u8 title;                         /* MODE_OFF on the title screen */
    u16 enabled;                      /* bit per section */
    /* Whether the dock and the header are on screen (not under the game's
     * screen opened from a battle, a shop or the PC). */
    u8 dock;
    /* Party: SAVE's and NUZLOCKE's icons. */
    MonView party[PARTY_SIZE];
    /* Region map, and the header's clock. */
    u8 mapsec, cursorX, cursorY, pickMapsec, pickX, pickY;
    u8 clockShown, clockValid, clockRate, clockPhase;
    u16 clockMinute;
    /* Map extras (3ds_field_qol_extras.inc), all zero with them off: the
     * list toggle on, the list shown instead of the map, its page (of
     * encPages, 0 with no table here; ENC_SHOWN rows a page, each method
     * its own pages) and method, its rows; the Repel's plate and steps; the
     * clock-not-restored banner. */
    u8 encOn, encView, encPage, encPages, encMethod, encCount;
    u8 repelShown, clockNotice;
    u16 repelSteps;
    EncRow enc[ENC_SHOWN];
    /* Bag: which of the game's is on show, BAG_VIEW_*. */
    u8 bagView;
    /* Trainer card, its back with cardBack. */
    u8 name[PLAYER_NAME_LENGTH + 1];
    u8 hasDex, stars, badges, minutes, cardBack, hofMinutes;
    u16 id, dex, hours, hofHours, linkWins, linkLosses, trades, pokeblocks, contests;
    u32 money;
    /* Save and options. */
    u8 saveStep, canSave;
    u8 options[OPTION_ROWS];
    /* The OPTIONS category and group on show, and what their rows show. */
    u8 optPage, optSub;
    u16 rows[OPT_ROWS_SHOWN];
    /* NUZLOCKE: the area and its status (-1 none), the chosen slot, the
     * fallen ones (a bit a slot), its texts. */
    s8 nuzStatus;
    u8 nuzSlot, nuzFallen;
    u8 nuzArea[NUZ_TEXT], nuzLink[NUZ_TEXT], nuzFallenText[NUZ_TEXT], nuzFainted[NUZ_TEXT], nuzCap[NUZ_TEXT],
       nuzTotals[NUZ_TEXT];
    /* The header's Y and RUN: the registered item; bit 0 running is the
     * default, bit 1 the player has the shoes. */
    u16 registered;
    u8 run;
    /* The keyboard focus (X): a dock or header button, or FOCUS_NONE;
     * inside the options, the row. */
    u8 focus, optFocus, blink;
    /* Battle: the cursor is 4 on the quick ball (actions) or CANCEL (moves). */
    u8 isDouble, safari, cursor, battler;
    u16 quickBall, quickBallCount;    /* the ball a quick throw would use, ITEM_NONE if none */
    u8 safariBalls;
    u8 partyBalls[PARTY_SIZE];        /* PARTY_BALL_*, as the game's party indicator */
    struct ChooseMoveStruct moves4;
    u8 text[256];
    /* The game's screen drawn here from its state (3ds_bottom_game.inc),
     * NATIVE_NONE for its picture in the window; what it reads. */
    u8 native;
    struct CtrPartyView pv;
    struct CtrSummaryView sv;
    struct CtrBagView bv;
    struct CtrDexView dx;
} ViewState;

/* NATIVE_BLANK: one of those whose view is not there yet: the section's
 * backdrop alone, never the game's picture. */
enum { NATIVE_NONE, NATIVE_PARTY, NATIVE_SUMMARY, NATIVE_BAG, NATIVE_DEX, NATIVE_BLANK };

/* Which of the game's bag is on show. */
enum { BAG_VIEW_NONE, BAG_VIEW_SECTION, BAG_VIEW_WHOLE };

static bool8 BagShown(u8 mode)
{
    return mode == MODE_BAG_MENU;
}

/* Whether the shop's BUY screen is what the whole screen shows. */
static bool8 ShopShown(u8 mode)
{
    return mode == MODE_STORAGE && CtrShop_IsOpen();
}

/* Whether the game's own Pokédex is what the area shows. */
static bool8 DexShown(u8 mode)
{
    return mode == MODE_POKEDEX;
}

static ViewState sState, sShown;
static bool8 sForceRedraw = TRUE;
static bool8 sInGame;
static u8 sScreen = SCR_MAP;
/* CLASSIC MENUS (3ds_extras.c): the party menu, the summary, the bag, the
 * trainer card and the Pokédex as the game draws them, in their window. Off
 * (the default) the bottom screen draws them itself and never shows their
 * picture. Read once a frame. */
static bool8 sClassicMenus;

/* The game's menus drawn here (not the classic menus): pokedex.c skips its
 * fades, which nobody sees then. */
bool8 CtrBottom_NativeMenus(void)
{
    return !sClassicMenus;
}

static bool8 OptionExists(int row)
{
    return row < OPTION_SHOWN && !(row == OPT_FPS && !CTR_SHOW_FPS);
}

static bool8 OptionLive(int row, bool8 voxel)
{
    return OptionExists(row) && (voxel || row < OPT_VOXEL_PITCH);
}

/* An extra for the voxel overworld (3D WORLD, 3D DEPTH) is greyed with it
 * off, as the voxel camera's rows are. */
static bool8 ExtraLive(const CtrExtra *extra, bool8 voxel)
{
    return voxel || !CtrExtras_NeedsVoxel(extra);
}

/* The OPTIONS category (CAT_*) and group on show (3ds_bottom_options.inc). */
static u8 sOptPage;
static u8 sOptSub;
/* The trainer card's back is up. */
static bool8 sCardBack;

/* What a row shows, for the redraw to notice a change: its value, or for
 * one with its own text, a sum of that text. */
static u16 ExtraShownValue(const CtrExtra *extra)
{
    u16 sum = 0;

    if (!extra->text)
        return extra->step ? CtrSettings_GetInt(extra->key, extra->fallback) : CtrExtras_Value(extra);
    for (const u8 *c = extra->text(); *c != EOS; ++c)
        sum = sum * 31 + *c;
    return sum;
}

/* The keyboard focus (X) on the dock and the header, and where it is inside
 * a screen. */
#define FOCUS_NONE 0xFF
enum { INSIDE_NONE, INSIDE_OPTIONS, INSIDE_SAVE };
static u8 sFocus = FOCUS_NONE, sInside, sOptFocus;
/* Leaving the focus, the buttons stay the bottom screen's until let go:
 * the B or A that left it must not reach the game. */
static bool8 sSwallow;
static u32 sFrames;

static bool8 RectsMeet(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1);

/* Per screen state, kept while another screen is shown. */

static u8 sPickMapsec = MAPSEC_NONE, sPickX, sPickY;
/* Map extras: the wild list instead of the map, its page; the load
 * (CtrClockStore_LoadSerial) whose clock banner was dismissed. */
static bool8 sEncView;
static u8 sEncPage;
static unsigned sClockNoticeSerial;
static u8 sSaveStep;
/* Room for the longest, gText_DifferentSaveFile (some 230 bytes). */
static u8 sSaveMessage[256];

enum { SAVE_ASK, SAVE_OVERWRITE, SAVE_DONE };

/* Learned once: the party menu's running callback (static in party_menu.c). */
static MainCallback sPartyMenuCallback;

/* ------------------------------------------------------------------------ */
/* Hit zones                                                                */
/* ------------------------------------------------------------------------ */

enum
{
    HIT_NONE = 0xFF,
    HIT_COLUMN = 0x10,     /* + dock section, or COL_Y, COL_RUN */
    HIT_CANCEL = 0x30,
    HIT_OK,
    HIT_YES,
    HIT_NO,
    HIT_ROW = 0x50,        /* + visible list row */
    HIT_ACTION = 0x60,     /* + action cursor */
    HIT_MOVE = 0x70,       /* + move slot */
    HIT_TARGET_LEFT = 0x80,
    HIT_TARGET_RIGHT,
    HIT_TARGET_OK,
    HIT_QUICK_BALL,        /* throw the selected ball */
    HIT_QUICK_BALL_PREV,
    HIT_QUICK_BALL_NEXT,
    HIT_MAP = 0x90,
    HIT_MAP_INFO,          /* the title: map <-> wild list (map extras) */
    HIT_MAP_PAGE,          /* the wild list: its next page */
    HIT_CLOCK_NOTICE,      /* the clock banner: hide it */
    HIT_OPTION = 0xC0,     /* + option row; +HIT_OPTION_BACK for its back half */
    HIT_PAGE = 0xE0,       /* + OPTIONS category tab; + 8 + group tab */
};
/* More than there are option rows, so a row and a left arrow never share an id. */
#define HIT_OPTION_BACK 16

typedef struct { s16 x, y, w, h; u8 id; } Hit;
static Hit sHits[64];
static u8 sHitCount;

static void AddHit(int x, int y, int w, int h, u8 id)
{
    if (sHitCount < ARRAY_COUNT(sHits))
        sHits[sHitCount++] = (Hit){x + sOX, y, w, h, id};
}

static u8 HitTest(int x, int y)
{
    for (int i = sHitCount - 1; i >= 0; --i)
        if (x >= sHits[i].x && y >= sHits[i].y && x < sHits[i].x + sHits[i].w && y < sHits[i].y + sHits[i].h)
            return sHits[i].id;
    return HIT_NONE;
}

/* ------------------------------------------------------------------------ */
/* Game state                                                               */
/* ------------------------------------------------------------------------ */


/* The player stands in the field with nothing else going on. */
static bool8 FieldIdle(void)
{
    return gMain.callback2 == CB2_Overworld && !ArePlayerFieldControlsLocked() && !ScriptContext_IsEnabled()
        && gPlayerAvatar.tileTransitionState == T_NOT_MOVING && !gPaletteFade.active && !CtrStartMenu_Busy();
}

static u16 EnabledScreens(void)
{
    u16 mask = (1 << SCR_MAP) | (1 << SCR_BAG) | (1 << SCR_CARD) | (1 << SCR_OPTION);

    if (FlagGet(FLAG_SYS_POKEMON_GET))
        mask |= 1 << SCR_POKEMON;
    if (FlagGet(FLAG_SYS_POKEDEX_GET))
        mask |= 1 << SCR_POKEDEX;
    if (FlagGet(FLAG_SYS_POKENAV_GET) && CtrStartMenu_Available())
        mask |= 1 << SCR_POKENAV;
    if (CtrStartMenu_Available())
        mask |= 1 << SCR_SAVE;
    if (CtrNuzlockeUi_Enabled())
        mask |= 1 << SCR_NUZLOCKE;
    return mask;
}

/*
 * What the battle controllers wait for. Their input handlers call in here
 * every frame they run (CtrBattleMenu_*Input); last frame's call is what the
 * bottom screen shows.
 */
enum { ASK_NONE, ASK_ACTION, ASK_MOVE, ASK_TARGET };

typedef struct
{
    u8 kind, battler;
} BattleAsk;

static BattleAsk sAsk, sAsked;
static u8 sBattleTap = 0xFF;   /* a tap for the controller to take */
static bool8 sMoveCancel;      /* the move menu's cursor is on CANCEL */
static bool8 sQuickBallTap;    /* the ball button was tapped */
static bool8 sQuickBallFocus;  /* the D-pad is on the ball button (A throws) */

/*
 * The quick ball (battle_controller_player.c, an optional patch in the
 * public tree): the ball R or the ball button would throw now, or
 * ITEM_NONE. Without that patch no battle offers one.
 */
u16 __attribute__((weak)) CtrBattle_QuickBallItem(void)
{
    return ITEM_NONE;
}

bool8 __attribute__((weak)) CtrBattle_QuickBallCycle(s8 direction)
{
    (void)direction;
    return FALSE;
}

static u8 CurrentMode(void)
{
    if (gMain.callback2 == CB2_Overworld)
        sInGame = TRUE;
    if (!sInGame || !gSaveBlock1Ptr || !gSaveBlock2Ptr)
        return MODE_OFF;
    /* The legendaries' cutscenes have the top screen to themselves, the
     * bottom one dark under them. */
    if (CtrVideo_StageZoomed())
        return MODE_OFF;
    if (CtrPokenav_IsOpen())
        return MODE_POKENAV;
    if (CtrPokedex_IsOpen())
        return MODE_POKEDEX;
    if (CtrTrainerCard_FieldOpen())
        return MODE_CARD;
    /* The summary from the field's party menu, drawn here; a classic one has
     * the whole screen, as from the boxes (MODE_STORAGE). */
    if (!sClassicMenus && CtrSummary_FieldOpen())
        return MODE_SUMMARY;
    /* Before the boxes: the bag can be opened from them, whole screen too. */
    if (gMain.callback2 == CB2_BagMenuRun && gBagMenu)
        return MODE_BAG_MENU;
    /* A summary opened from the boxes is on the whole screen too. */
    if (CtrStorage_IsOpen() || CtrVideo_BottomWhole())
        return MODE_STORAGE;
    if (FuncIsActiveTask(Task_HandleChooseMonInput))
        sPartyMenuCallback = gMain.callback2;
    if ((sPartyMenuCallback && gMain.callback2 == sPartyMenuCallback) || CtrParty_Building())
        return MODE_PARTY_MENU;
    if (gMain.inBattle)
    {
        if (gMain.callback2 == BattleMainCB2 && !gPaletteFade.active)
        {
            if (sAsked.kind == ASK_ACTION) return MODE_BATTLE_ACTION;
            if (sAsked.kind == ASK_MOVE) return MODE_BATTLE_MOVE;
            if (sAsked.kind == ASK_TARGET) return MODE_BATTLE_TARGET;
        }
        return MODE_BATTLE_INFO;
    }
    return MODE_FIELD;
}

/* ------------------------------------------------------------------------ */
/* Hidden sessions: the game's menus running under the world              */
/* ------------------------------------------------------------------------ */

static struct
{
    bool8 active, entered, battle;
    /* Opened by CtrBottom_KeepWorld: a game screen is on its way. */
    bool8 awaitMenu;
    u16 frames, away;
} sSession;

static void BeginSession(bool8 battle)
{
    if (!sSession.active)
        CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: hidden menu session (%s)", battle ? "battle" : "field");
    sSession.active = TRUE;
    sSession.entered = FALSE;
    sSession.awaitMenu = FALSE;
    sSession.battle = battle;
    sSession.frames = sSession.away = 0;
}

/*
 * Whether the top screen holds its frame. A session holds from the first
 * press that leads to the menu (the fade out is not shown) through the menu
 * and back until the fade in is over. A screen the session did not expect -
 * an evolution, the move to forget, the fly map - is shown after a moment.
 */
static bool8 UpdateSession(u8 mode, bool8 planRunning)
{
    bool8 inMenu = mode == MODE_PARTY_MENU || mode == MODE_BAG_MENU || mode == MODE_POKENAV
                || mode == MODE_STORAGE || mode == MODE_POKEDEX || mode == MODE_SUMMARY || mode == MODE_CARD;
    bool8 home = gMain.callback2 == CB2_Overworld || gMain.callback2 == BattleMainCB2;

    if (inMenu && !sSession.active)
        BeginSession(gMain.inBattle);
    if (!sSession.active)
        return FALSE;
    ++sSession.frames;
    if (inMenu)
    {
        sSession.entered = TRUE;
        sSession.away = 0;
        return TRUE;
    }
    if (home)
    {
        sSession.away = 0;
        /* A screen asked for by the game (the shop's BUY, the bag to sell
         * from) starts a frame after the fade out: not the session's end. */
        if (sSession.awaitMenu && !sSession.entered && sSession.frames < 180)
            return TRUE;
        if ((sSession.entered || (!planRunning && sSession.frames > 30)) && !gPaletteFade.active)
        {
            sSession.active = FALSE;
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: hidden menu session over");
            return FALSE;
        }
        return TRUE;
    }
    /* A menu's setup and the map reload pass through here too: brief. */
    return ++sSession.away < 40;
}

/* Hidden menus need not wait on their fades and slow text. */
static void FastForward(void)
{
    for (int i = 0; i < 6; ++i)
    {
        if (gPaletteFade.active)
            UpdatePaletteFade();
        RunTextPrinters();
    }
}

/* ------------------------------------------------------------------------ */
/* The battle menus                                                         */
/* ------------------------------------------------------------------------ */

/*
 * The action, move and target menus are the bottom screen's: the controllers
 * do not draw theirs (the top keeps its message box) and read their keys
 * through these, which move the game's cursor in the bottom screen's order
 * and turn a tap into the key that confirms it. What each choice does stays
 * the controller's own code.
 */
bool8 CtrBattleMenu_Active(void)
{
    /* Link battles too (local wireless): the player's choices go through the
     * same controller. Drawn on the top screen, the menu ran into the wide
     * text box. Not for a recorded battle played back: nobody chooses. */
    return sRes.ready && !(gBattleTypeFlags & BATTLE_TYPE_RECORDED);
}

void CtrBattleMenu_Begin(void)
{
    sBattleTap = HIT_NONE;
    sMoveCancel = FALSE;
    sQuickBallTap = FALSE;
    sQuickBallFocus = FALSE;
}

/* The ball button, once per tap: the action handler throws the ball. */
bool8 CtrBattleMenu_TakeQuickBall(void)
{
    bool8 tapped = sQuickBallTap;

    sQuickBallTap = FALSE;
    return tapped;
}

/* "What will X do?", in the message box: the one thing left on top. */
void CtrBattleMenu_ShowPrompt(void)
{
    /* The printer reads it while it prints: a copy of its own. */
    static u8 prompt[64];
    int i;

    for (i = 0; i < (int)sizeof(prompt) - 1 && gDisplayedStringBattle[i] != EOS; ++i)
        prompt[i] = gDisplayedStringBattle[i];
    prompt[i] = EOS;
    BattlePutTextOnWindow(prompt, B_WIN_MSG);
}

/* Wally's controller picks by itself: the menu is shown, a tap is dropped. */
static void ShowBattleAsk(u8 kind)
{
    sBattleTap = HIT_NONE;
    sQuickBallFocus = FALSE;
    sAsk.kind = kind;
    sAsk.battler = gActiveBattler;
}

void CtrBattleMenu_ShowAction(void)
{
    ShowBattleAsk(ASK_ACTION);
}

void CtrBattleMenu_ShowMove(void)
{
    ShowBattleAsk(ASK_MOVE);
}

static u8 TakeBattleTap(u8 kind)
{
    u8 tap = sBattleTap;

    sBattleTap = HIT_NONE;
    sAsk.kind = kind;
    sAsk.battler = gActiveBattler;
    return tap;
}

void CtrBattleMenu_ActionInput(u8 *cursor, bool8 safari)
{
    u8 tap = TakeBattleTap(ASK_ACTION), next = *cursor;
    u16 dpad = gMain.newKeys & DPAD_ANY;
    /* FIGHT on top; BAG, POKéMON and RUN in a row under it. The ball
     * button, right of FIGHT and over RUN, is reached from both. */
    bool8 ball = !safari && CtrBattle_QuickBallItem() != ITEM_NONE;

    gMain.newKeys &= ~DPAD_ANY;
    if (!ball)
        sQuickBallFocus = FALSE;
    if (sQuickBallFocus)
    {
        if (dpad & (DPAD_LEFT | DPAD_DOWN))
        {
            sQuickBallFocus = FALSE;
            next = (dpad & DPAD_LEFT) ? 0 : 3;
            PlaySE(SE_SELECT);
            *cursor = next;
        }
        else if (dpad & DPAD_RIGHT)
        {
            if (CtrBattle_QuickBallCycle(1))
                PlaySE(SE_SELECT);
        }
        else if (gMain.newKeys & A_BUTTON)
        {
            gMain.newKeys &= ~A_BUTTON;
            sQuickBallTap = TRUE;
        }
        dpad = 0;
    }
    else if (ball && (((dpad & DPAD_RIGHT) && next == 0) || ((dpad & DPAD_UP) && next == 3)))
    {
        sQuickBallFocus = TRUE;
        PlaySE(SE_SELECT);
        dpad = 0;
    }
    if (dpad & DPAD_UP)
        next = 0;
    else if ((dpad & DPAD_DOWN) && next == 0)
        next = 2;
    else if ((dpad & DPAD_LEFT) && next > 1)
        --next;
    else if ((dpad & DPAD_RIGHT) && next != 0 && next < 3)
        ++next;
    if (next != *cursor)
    {
        PlaySE(SE_SELECT);
        *cursor = next;
    }
    if (tap == HIT_QUICK_BALL && !safari)
        sQuickBallTap = TRUE;
    else if (tap == HIT_QUICK_BALL_PREV || tap == HIT_QUICK_BALL_NEXT)
    {
        if (CtrBattle_QuickBallCycle(tap == HIT_QUICK_BALL_PREV ? -1 : 1))
            PlaySE(SE_SELECT);
        sQuickBallFocus = TRUE;
    }
    if (tap >= HIT_ACTION && tap < HIT_ACTION + 4)
    {
        sQuickBallFocus = FALSE;
        *cursor = tap - HIT_ACTION;
        gMain.newKeys |= A_BUTTON;
    }
    /* BAG and POKéMON open menus that run hidden. */
    if ((gMain.newKeys & A_BUTTON) && !safari && (*cursor == 1 || *cursor == 2))
        BeginSession(TRUE);
}

void CtrBattleMenu_MoveInput(u8 *cursor, const u16 *moves)
{
    u8 tap = TakeBattleTap(ASK_MOVE), next = *cursor, count = 0;
    u16 dpad = gMain.newKeys & DPAD_ANY;
    bool8 cancel = sMoveCancel;

    for (int i = 0; i < MAX_MON_MOVES; ++i)
        if (moves[i] != MOVE_NONE)
            ++count;
    /* The moves two by two, CANCEL under them. No reordering (SELECT). */
    gMain.newKeys &= ~(DPAD_ANY | SELECT_BUTTON);
    if (cancel)
    {
        if (dpad & DPAD_UP)
            cancel = FALSE;
    }
    else if (dpad & DPAD_UP)
    {
        if (next & 2)
            next ^= 2;
    }
    else if (dpad & DPAD_DOWN)
    {
        if (!(next & 2) && (next ^ 2) < count)
            next ^= 2;
        else
            cancel = TRUE;
    }
    else if (dpad & DPAD_LEFT)
    {
        if (next & 1)
            next ^= 1;
    }
    else if (dpad & DPAD_RIGHT)
    {
        if (!(next & 1) && (next ^ 1) < count)
            next ^= 1;
    }
    if (next != *cursor || cancel != sMoveCancel)
        PlaySE(SE_SELECT);
    *cursor = next;
    sMoveCancel = cancel;
    if (tap >= HIT_MOVE && tap < HIT_MOVE + MAX_MON_MOVES && moves[tap - HIT_MOVE] != MOVE_NONE)
    {
        *cursor = tap - HIT_MOVE;
        sMoveCancel = FALSE;
        gMain.newKeys |= A_BUTTON;
    }
    else if (tap == HIT_CANCEL)
        gMain.newKeys |= B_BUTTON;
    /* A on CANCEL is B. */
    if (sMoveCancel && (gMain.newKeys & A_BUTTON))
        gMain.newKeys = (gMain.newKeys & ~A_BUTTON) | B_BUTTON;
}

void CtrBattleMenu_TargetInput(void)
{
    u8 tap = TakeBattleTap(ASK_TARGET);

    if (tap == HIT_TARGET_LEFT) gMain.newKeys |= DPAD_LEFT;
    else if (tap == HIT_TARGET_RIGHT) gMain.newKeys |= DPAD_RIGHT;
    else if (tap == HIT_TARGET_OK) gMain.newKeys |= A_BUTTON;
    else if (tap == HIT_CANCEL) gMain.newKeys |= B_BUTTON;
}

/* ------------------------------------------------------------------------ */
/* Snapshots                                                                */
/* ------------------------------------------------------------------------ */

static u8 PlayerRegionPosition(u8 *outX, u8 *outY)
{
    /* region_map.c's InitMapBasedOnPlayerLocation, without its UI state. */
    const struct MapHeader *header = &gMapHeader;
    u16 mapWidth, mapHeight, x, y, scale;
    u8 mapsec;

    switch (GetMapTypeByGroupAndId(gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum))
    {
    case MAP_TYPE_UNDERGROUND:
    case MAP_TYPE_UNKNOWN:
        if (gMapHeader.allowEscaping)
        {
            header = Overworld_GetMapHeaderByGroupAndId(gSaveBlock1Ptr->escapeWarp.mapGroup,
                                                        gSaveBlock1Ptr->escapeWarp.mapNum);
            x = gSaveBlock1Ptr->escapeWarp.x;
            y = gSaveBlock1Ptr->escapeWarp.y;
            mapsec = header->regionMapSectionId;
        }
        else
        {
            mapsec = gMapHeader.regionMapSectionId;
            header = NULL;
            x = y = 1;
        }
        break;
    case MAP_TYPE_SECRET_BASE:
        header = Overworld_GetMapHeaderByGroupAndId(gSaveBlock1Ptr->dynamicWarp.mapGroup,
                                                    gSaveBlock1Ptr->dynamicWarp.mapNum);
        x = gSaveBlock1Ptr->dynamicWarp.x;
        y = gSaveBlock1Ptr->dynamicWarp.y;
        mapsec = header->regionMapSectionId;
        break;
    case MAP_TYPE_INDOOR:
    {
        const struct WarpData *warp = gMapHeader.regionMapSectionId != MAPSEC_DYNAMIC
                                    ? &gSaveBlock1Ptr->escapeWarp : &gSaveBlock1Ptr->dynamicWarp;
        header = Overworld_GetMapHeaderByGroupAndId(warp->mapGroup, warp->mapNum);
        mapsec = gMapHeader.regionMapSectionId != MAPSEC_DYNAMIC ? gMapHeader.regionMapSectionId
                                                                 : header->regionMapSectionId;
        x = warp->x;
        y = warp->y;
        break;
    }
    default:
        mapsec = gMapHeader.regionMapSectionId;
        x = gSaveBlock1Ptr->pos.x;
        y = gSaveBlock1Ptr->pos.y;
        break;
    }
    if (mapsec >= MAPSEC_NONE)
        return MAPSEC_NONE;
    mapWidth = header && header->mapLayout ? header->mapLayout->width : 1;
    mapHeight = header && header->mapLayout ? header->mapLayout->height : 1;
    if (gRegionMapEntries[mapsec].width && gRegionMapEntries[mapsec].height)
    {
        scale = mapWidth / gRegionMapEntries[mapsec].width;
        x /= scale ? scale : 1;
        if (x >= gRegionMapEntries[mapsec].width) x = gRegionMapEntries[mapsec].width - 1;
        scale = mapHeight / gRegionMapEntries[mapsec].height;
        y /= scale ? scale : 1;
        if (y >= gRegionMapEntries[mapsec].height) y = gRegionMapEntries[mapsec].height - 1;
    }
    else
    {
        x = y = 0;
    }
    /* 1 and 2 are region_map.c's MAPCURSOR_X_MIN / MAPCURSOR_Y_MIN. */
    *outX = gRegionMapEntries[mapsec].x + x + 1;
    *outY = gRegionMapEntries[mapsec].y + y + 2;
    return mapsec;
}

static void CopyText(u8 *dst, int size, const u8 *src)
{
    int n = 0;

    while (src && n < size - 1 && src[n] != EOS)
    {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = EOS;
}

/* trainer_card.c's GetRubyTrainerStars as Emerald fills it in. */
static u8 CardStars(void)
{
    return (GetGameStat(GAME_STAT_ENTERED_HOF) != 0) + (HasAllHoennMons() != 0)
         + (CountPlayerMuseumPaintings() >= CONTEST_CATEGORIES_COUNT);
}

static void SnapshotCard(ViewState *s)
{
    static u16 dex, frames;
    static u8 stars;

    StringCopy(s->name, gSaveBlock2Ptr->playerName);
    s->id = gSaveBlock2Ptr->playerTrainerId[0] | (gSaveBlock2Ptr->playerTrainerId[1] << 8);
    s->money = GetMoney(&gSaveBlock1Ptr->money);
    s->hours = gSaveBlock2Ptr->playTimeHours > 999 ? 999 : gSaveBlock2Ptr->playTimeHours;
    s->minutes = gSaveBlock2Ptr->playTimeMinutes > 59 ? 59 : gSaveBlock2Ptr->playTimeMinutes;
    s->hasDex = FlagGet(FLAG_SYS_POKEDEX_GET);
    /* Counting the Pokédex walks every species; twice a second is plenty. */
    if ((frames++ % 30) == 0)
    {
        dex = !s->hasDex ? 0 : IsNationalPokedexEnabled() ? GetNationalPokedexCount(FLAG_GET_CAUGHT)
                                                         : GetHoennPokedexCount(FLAG_GET_CAUGHT);
        stars = CardStars();
    }
    s->dex = dex;
    s->stars = stars;
    for (int i = 0; i < NUM_BADGES; ++i)
        if (FlagGet(FLAG_BADGE01_GET + i))
            s->badges |= 1 << i;
}

static u16 CappedStat(u8 stat, u32 cap)
{
    u32 value = GetGameStat(stat);

    return value > cap ? cap : value;
}

/* The card's back, as trainer_card.c's SetPlayerCardData fills it in. */
static void SnapshotCardBack(ViewState *s)
{
    u32 hof = GetGameStat(GAME_STAT_FIRST_HOF_PLAY_TIME);

    s->cardBack = TRUE;
    s->hofHours = hof >> 16;
    s->hofMinutes = (hof >> 8) & 0xFF;
    if (s->hofHours > 999)
    {
        s->hofHours = 999;
        s->hofMinutes = 59;
    }
    s->linkWins = CappedStat(GAME_STAT_LINK_BATTLE_WINS, 9999);
    s->linkLosses = CappedStat(GAME_STAT_LINK_BATTLE_LOSSES, 9999);
    s->trades = CappedStat(GAME_STAT_POKEMON_TRADES, 0xFFFF);
    s->pokeblocks = CappedStat(GAME_STAT_POKEBLOCKS_WITH_FRIENDS, 0xFFFF);
    s->contests = CappedStat(GAME_STAT_WON_LINK_CONTEST, 999);
}

/* The party's icons, a fainted one grey. */
static void SnapshotParty(ViewState *s)
{
    for (int i = 0; i < PARTY_SIZE && i < gPlayerPartyCount; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u16 species = GetMonData(mon, MON_DATA_SPECIES);
        MonView *view = &s->party[i];

        if (species == SPECIES_NONE)
            continue;
        view->species = species;
        view->isEgg = GetMonData(mon, MON_DATA_IS_EGG);
        view->iconSpecies = view->isEgg ? SPECIES_EGG : GetIconSpecies(species, GetMonData(mon, MON_DATA_PERSONALITY));
        view->deoxys = species == SPECIES_DEOXYS;
        view->fainted = !view->isEgg && GetMonData(mon, MON_DATA_HP) == 0;
    }
}

/* The party as its own menu shows it: names, levels, genders, HP. */
static void SnapshotPartyFull(ViewState *s)
{
    SnapshotParty(s);
    for (int i = 0; i < PARTY_SIZE && i < gPlayerPartyCount; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        MonView *view = &s->party[i];

        if (!view->species)
            continue;
        view->level = GetMonData(mon, MON_DATA_LEVEL);
        view->hp = GetMonData(mon, MON_DATA_HP);
        view->maxHp = GetMonData(mon, MON_DATA_MAX_HP);
        view->ailment = GetAilmentFromStatus(GetMonData(mon, MON_DATA_STATUS));
        GetMonData(mon, MON_DATA_NICKNAME, view->nick);
        StringGet_Nickname(view->nick);
        view->gender = view->isEgg ? MON_GENDERLESS : GetMonGender(mon);
        /* The game shows no sign by a NIDORAN still called so. */
        if ((view->species == SPECIES_NIDORAN_M || view->species == SPECIES_NIDORAN_F)
         && StringCompare(view->nick, gSpeciesNames[view->species]) == 0)
            view->gender = MON_GENDERLESS;
    }
}

/* Which native view the mode on show has, NATIVE_NONE for none. */
static u8 NativeKind(const ViewState *s)
{
    if (sClassicMenus)
        return NATIVE_NONE;
    if (s->mode == MODE_SUMMARY)
        return NATIVE_SUMMARY;
    if (s->mode == MODE_POKEDEX)
        return NATIVE_DEX;
    if (s->mode == MODE_BAG_MENU && s->bagView == BAG_VIEW_SECTION)
        return NATIVE_BAG;
    if (s->mode == MODE_PARTY_MENU && s->dock)
        return NATIVE_PARTY;
    return NATIVE_NONE;
}

/* The game's screen on show, drawn by the bottom screen itself: what it
 * reads of it. With nothing to read for a moment (the menu coming in or
 * going out, a state it has no view of) what was drawn stays; with nothing
 * drawn yet, the section's empty backdrop (NATIVE_BLANK). The game's
 * picture is never shown for these. */
static void SnapshotNative(ViewState *s)
{
    u8 kind = NativeKind(s);
    bool8 got = FALSE;

    if (kind == NATIVE_NONE)
        return;
    switch (kind)
    {
    case NATIVE_SUMMARY:
        got = CtrSummary_GetView(&s->sv) && s->sv.state != CTR_SUM_VIEW_NONE;
        break;
    case NATIVE_DEX:
        got = CtrPokedex_GetView(&s->dx) && s->dx.state != CTR_DEX_VIEW_NONE;
        break;
    case NATIVE_BAG:
        got = CtrBag_GetView(&s->bv) && s->bv.state != CTR_BAG_VIEW_NONE;
        break;
    case NATIVE_PARTY:
        got = CtrParty_GetView(&s->pv) && s->pv.state != CTR_PARTY_VIEW_NONE;
        break;
    }
    if (got)
        s->native = kind;
    else if (sShown.native == kind)
    {
        s->native = kind;
        s->pv = sShown.pv;
        s->sv = sShown.sv;
        s->bv = sShown.bv;
        s->dx = sShown.dx;
    }
    else
    {
        memset(&s->pv, 0, sizeof(s->pv));
        memset(&s->sv, 0, sizeof(s->sv));
        memset(&s->bv, 0, sizeof(s->bv));
        memset(&s->dx, 0, sizeof(s->dx));
        s->native = NATIVE_BLANK;
    }
    if (s->native == NATIVE_PARTY)
        SnapshotPartyFull(s);
}

/* The save view's file: the card's figures, where the player is, the party. */
static void SnapshotSave(ViewState *s)
{
    SnapshotCard(s);
    s->mapsec = gMapHeader.regionMapSectionId;
    SnapshotParty(s);
}

/* The NUZLOCKE section: the tracker's own figures and texts. */
static void SnapshotNuzlocke(ViewState *s)
{
    int slot = CtrNuzlockeUi_Slot();

    SnapshotParty(s);
    s->nuzStatus = CtrNuzlockeUi_AreaStatus();
    s->nuzSlot = slot < 0 ? 0xFF : slot;
    for (int i = 0; i < PARTY_SIZE; ++i)
        if (CtrNuzlockeUi_SlotFallen(i))
            s->nuzFallen |= 1 << i;
    CopyText(s->nuzArea, NUZ_TEXT, CtrNuzlockeUi_AreaText());
    CopyText(s->nuzLink, NUZ_TEXT, CtrNuzlockeUi_LinkText());
    CopyText(s->nuzFallenText, NUZ_TEXT, CtrNuzlockeUi_FallenText());
    CopyText(s->nuzFainted, NUZ_TEXT, CtrNuzlockeUi_MarkFaintedText());
    CopyText(s->nuzCap, NUZ_TEXT, CtrNuzlockeUi_CapText());
    CopyText(s->nuzTotals, NUZ_TEXT, CtrNuzlockeUi_TotalsText());
}

/* The party ball states, as battle_interface.c's party indicator has them. */
enum { PARTY_BALL_OK, PARTY_BALL_STATUS, PARTY_BALL_FAINT, PARTY_BALL_EMPTY };

static void SnapshotBattle(ViewState *s)
{
    s->isDouble = (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) != 0;
    s->safari = (gBattleTypeFlags & BATTLE_TYPE_SAFARI) != 0;
    if (s->mode == MODE_BATTLE_INFO)
        return;
    s->blink = (sFrames >> 4) & 1;
}

/* The six party balls: the mons in party order, then the empty places (an
 * egg is one, as in the game's indicator). */
static void SnapshotPartyBalls(ViewState *s)
{
    int n = 0;

    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);

        if (species == SPECIES_NONE || species == SPECIES_EGG)
            continue;
        if (GetMonData(mon, MON_DATA_HP) == 0)
            s->partyBalls[n++] = PARTY_BALL_FAINT;
        else if (GetMonData(mon, MON_DATA_STATUS) != 0)
            s->partyBalls[n++] = PARTY_BALL_STATUS;
        else
            s->partyBalls[n++] = PARTY_BALL_OK;
    }
    while (n < PARTY_SIZE)
        s->partyBalls[n++] = PARTY_BALL_EMPTY;
}


/* Every option's value as the OPTIONS screen shows it. */
static void ReadOptionValues(u8 options[OPTION_ROWS])
{
    options[0] = gSaveBlock2Ptr->optionsTextSpeed;
    options[1] = gSaveBlock2Ptr->optionsBattleSceneOff;
    options[2] = gSaveBlock2Ptr->optionsBattleStyle;
    options[3] = gSaveBlock2Ptr->optionsSound;
    options[4] = gSaveBlock2Ptr->optionsButtonMode;
    options[5] = gSaveBlock2Ptr->optionsWindowFrameType;
    options[OPT_FPS] = CtrSettings_ShowFps();
    options[OPT_VOXEL] = CtrSettings_Voxel();
    options[OPT_VOXEL_PITCH] = CtrSettings_VoxelPitch();
    options[OPT_VOXEL_ZOOM] = CtrSettings_VoxelZoom();
    options[OPT_VOXEL_BLUR] = CtrSettings_VoxelBlurStrength();
    options[OPT_VOXEL_BATTLE] = CtrSettings_VoxelBattle();
}

/*
 * Map extras (3ds_field_qol_extras.inc). The wild list reads the area's
 * normal tables as the game's encounters do: the map's header in
 * gWildMonHeaders (Altering Cave's set by VAR_ALTERING_CAVE_WILD_SET), the
 * grass table through CtrEncounters_Land, so the timed tables (WILD POKEMON:
 * TIMED) show what the hour gives. Each method with a table is a page: grass,
 * surfing, Rock Smash, and the three rods (their slots of the fishing table).
 * Not shown, as they are not in those tables: Feebas's tiles, a TV outbreak's
 * species, the roamers, the Battle Pyramid's and Pike's own tables, static and
 * scripted Pokemon. A later randomizer (CtrRandomizer_Wild) is to map the
 * species here too, once its API is final.
 */
enum { ENC_LAND, ENC_WATER, ENC_ROCK, ENC_OLD_ROD, ENC_GOOD_ROD, ENC_SUPER_ROD, ENC_METHODS };

/* The game's slot chances (wild_encounter.c), in percent; the fishing table's
 * slots are the old rod's 0-1, the good rod's 2-4, the super rod's 5-9. */
static const u8 sEncLandRates[12] = {20, 20, 10, 10, 10, 10, 5, 5, 4, 4, 1, 1};
static const u8 sEncWaterRates[5] = {60, 30, 5, 4, 1};
static const u8 sEncFishRates[10] = {70, 30, 60, 20, 20, 40, 40, 15, 4, 1};
static const u8 sEncFirst[ENC_METHODS] = {0, 0, 0, 0, 2, 5};
static const u8 sEncSlots[ENC_METHODS] = {12, 5, 5, 2, 3, 5};

/* The slots [first, first + count) as rows, one a species in slot order,
 * their chances added up and their levels' range; how many rows. */
static u8 EncCollect(EncRow *rows, const struct WildPokemon *mons, u8 first, u8 count, const u8 *rates)
{
    u8 n = 0;

    for (u8 i = 0; i < count; ++i)
    {
        const struct WildPokemon *mon = &mons[first + i];
        u8 k = 0;
        u16 species;

        if (mon->species == SPECIES_NONE)
            continue;
        species = CtrEncounters_DisplaySpecies(mon->species);
        if (species == SPECIES_NONE)
            continue;
        while (k < n && rows[k].species != species)
            ++k;
        if (k == n)
        {
            if (n == ENC_ROWS)
                continue;
            rows[n].species = species;
            rows[n].minLevel = mon->minLevel;
            rows[n].maxLevel = mon->maxLevel;
            rows[n].percent = 0;
            rows[n].seen = 0;
            ++n;
        }
        if (mon->minLevel < rows[k].minLevel)
            rows[k].minLevel = mon->minLevel;
        if (mon->maxLevel > rows[k].maxLevel)
            rows[k].maxLevel = mon->maxLevel;
        rows[k].percent += rates[first + i];
    }
    return n;
}

/* The current map's wild header, as the game finds it, or NULL. */
static const struct WildPokemonHeader *EncHeader(void)
{
    u8 group = gSaveBlock1Ptr->location.mapGroup, num = gSaveBlock1Ptr->location.mapNum;

    for (u16 i = 0; gWildMonHeaders[i].mapGroup != MAP_GROUP(MAP_UNDEFINED); ++i)
    {
        if (gWildMonHeaders[i].mapGroup != group || gWildMonHeaders[i].mapNum != num)
            continue;
        /* Altering Cave's sets follow each other; an unknown set is the first, as in the game. */
        if (group == MAP_GROUP(MAP_ALTERING_CAVE) && num == MAP_NUM(MAP_ALTERING_CAVE))
        {
            u16 set = VarGet(VAR_ALTERING_CAVE_WILD_SET), run = 0;

            while (gWildMonHeaders[i + run].mapGroup == group && gWildMonHeaders[i + run].mapNum == num)
                ++run;
            if (set < run)
                i += set;
        }
        return &gWildMonHeaders[i];
    }
    return NULL;
}

/* Each method with a table, ENC_SHOWN rows a page: the page on show. */
static void SnapshotEncounters(ViewState *s)
{
    const struct WildPokemonHeader *header = EncHeader();
    const struct WildPokemonInfo *info[ENC_METHODS] = {NULL};
    static const u8 *const rates[ENC_METHODS] = {sEncLandRates, sEncWaterRates, sEncWaterRates,
                                                 sEncFishRates, sEncFishRates, sEncFishRates};
    EncRow rows[ENC_ROWS];
    u8 counts[ENC_METHODS] = {0}, pages = 0, page;

    if (header)
    {
        info[ENC_LAND] = CtrEncounters_Land(header->landMonsInfo, gSaveBlock1Ptr->location.mapGroup,
                                            gSaveBlock1Ptr->location.mapNum);
        info[ENC_WATER] = header->waterMonsInfo;
        info[ENC_ROCK] = header->rockSmashMonsInfo;
        info[ENC_OLD_ROD] = info[ENC_GOOD_ROD] = info[ENC_SUPER_ROD] = header->fishingMonsInfo;
    }
    for (u8 m = 0; m < ENC_METHODS; ++m)
        if (info[m] && info[m]->wildPokemon)
        {
            counts[m] = EncCollect(rows, info[m]->wildPokemon, sEncFirst[m], sEncSlots[m], rates[m]);
            pages += counts[m] ? (counts[m] + ENC_SHOWN - 1) / ENC_SHOWN : 1;
        }
    s->encPages = pages;
    if (pages == 0)
        return;
    s->encPage = page = sEncPage % pages;
    for (u8 m = 0; m < ENC_METHODS; ++m)
    {
        u8 n;

        if (!info[m] || !info[m]->wildPokemon)
            continue;
        n = counts[m] ? (counts[m] + ENC_SHOWN - 1) / ENC_SHOWN : 1;
        if (page >= n)
        {
            page -= n;
            continue;
        }
        EncCollect(rows, info[m]->wildPokemon, sEncFirst[m], sEncSlots[m], rates[m]);
        s->encMethod = m;
        s->encCount = counts[m] - page * ENC_SHOWN > ENC_SHOWN ? ENC_SHOWN : counts[m] - page * ENC_SHOWN;
        memcpy(s->enc, rows + page * ENC_SHOWN, s->encCount * sizeof(EncRow));
        break;
    }
    /* Only what the Pokedex has seen is named. */
    for (u8 i = 0; i < s->encCount; ++i)
        s->enc[i].seen = GetSetPokedexFlag(SpeciesToNationalPokedexNum(s->enc[i].species), FLAG_GET_SEEN) != 0;
}

static void SnapshotMapExtras(ViewState *s)
{
    s->encOn = CtrExtras_Get("map_encounters") == 1;
    s->encView = s->encOn && sEncView;
    if (s->encView)
    {
        /* The list has no marks: walking in the area redraws nothing. */
        s->cursorX = s->cursorY = 0;
        s->pickMapsec = MAPSEC_NONE;
        s->pickX = s->pickY = 0;
        SnapshotEncounters(s);
    }
    s->clockNotice = CtrExtras_Get("clock_notice") == 1 && CtrClockStore_NeedsNotice(CtrClockStore_Status())
                  && CtrClockStore_LoadSerial() != sClockNoticeSerial;
}

/* The header's clock: the hour always; the part of the day and the clock's
 * speed only with the day/night clock or SHOW CLOCK on. Off, the clock is
 * not running: the hour is the game's own. */
static void SnapshotClock(ViewState *s)
{
    unsigned seconds = 0, rate = 1;

    s->clockShown = CtrSettings_GetInt("day_night", 0) == 1 || CtrSettings_GetInt("show_clock", 0) == 1;
    s->clockValid = CtrDayNight_Time(&seconds, &rate);
    if (!s->clockValid)
        s->clockValid = CtrGame_LocalTimeSeconds(&seconds);
    if (s->clockValid)
    {
        s->clockMinute = seconds / 60;
        s->clockRate = rate;
        s->clockPhase = CtrEncounters_Phase(seconds);
    }
}

static void SnapshotOptionRows(ViewState *s);

static void Snapshot(ViewState *s, u8 mode, u8 pressed)
{
    memset(s, 0, sizeof(*s));
    s->mode = mode;
    s->pressed = pressed;
    s->pickMapsec = MAPSEC_NONE;
    if (s->mode == MODE_OFF)
    {
        s->title = CtrTitleScreen_RayquazaBg() >= 0;
        return;
    }
    s->gender = gSaveBlock2Ptr->playerGender ? FEMALE : MALE;
    s->inBattle = gMain.inBattle;
    s->enabled = EnabledScreens();
    s->screen = sScreen;
    /* The hidden menus take over the view they belong to. */
    if (mode == MODE_PARTY_MENU)
    {
        s->screen = SCR_POKEMON;
        /* The party menu from the field is in the window; from a battle, a
         * contest or a facility it has the whole screen (CtrCentredParty). */
        s->bagView = !gMain.inBattle && gPartyMenu.menuType == PARTY_MENU_TYPE_FIELD ? BAG_VIEW_SECTION
                                                                                       : BAG_VIEW_WHOLE;
        if (s->bagView == BAG_VIEW_WHOLE)
            s->screen = SCR_COUNT;
    }
    else if (mode == MODE_BAG_MENU)
    {
        s->screen = SCR_BAG;
        /* The bag from the field is in the window; from anything else it has
         * the whole screen. */
        s->bagView = CtrBag_InWindow() ? BAG_VIEW_SECTION : BAG_VIEW_WHOLE;
        if (s->bagView == BAG_VIEW_WHOLE)
            s->screen = SCR_COUNT;
    }
    else if (mode == MODE_POKENAV)
        s->screen = SCR_POKENAV;
    else if (mode == MODE_CARD)
        s->screen = SCR_CARD;
    else if (mode == MODE_POKEDEX)
        s->screen = SCR_POKEDEX;
    else if (mode == MODE_SUMMARY)
        s->screen = SCR_POKEMON;
    else if (mode == MODE_STORAGE)
        s->screen = SCR_COUNT;   /* the boxes have the whole screen */
    if (s->screen < SCR_COUNT && !((s->enabled >> s->screen) & 1))
        s->screen = SCR_MAP;
    s->dock = mode < MODE_BATTLE_INFO && !s->inBattle && s->screen < SCR_COUNT;
    StringCopy(s->name, gSaveBlock2Ptr->playerName);
    /* The header's Y and RUN, and the focus on the dock or the header. */
    s->registered = gSaveBlock1Ptr->registeredItem;
    s->run = (CtrSettings_RunAlways() ? 1 : 0) | (FlagGet(FLAG_SYS_B_DASH) ? 2 : 0);
    s->focus = sFocus;
    s->optFocus = FOCUS_NONE;

    if (s->dock)
    {
        /* The header: the area's name and the map extras' toggle with it,
         * the Repel, the clock. */
        s->mapsec = PlayerRegionPosition(&s->cursorX, &s->cursorY);
        if (s->screen == SCR_MAP)
        {
            s->pickMapsec = sPickMapsec;
            s->pickX = sPickX;
            s->pickY = sPickY;
            SnapshotMapExtras(s);
        }
        else
            s->cursorX = s->cursorY = 0;
        /* Read only: the step counter is the game's (UpdateRepelCounter). */
        if (CtrExtras_Get("repel_steps") == 1)
        {
            s->repelShown = TRUE;
            s->repelSteps = VarGet(VAR_REPEL_STEP_COUNT);
        }
        SnapshotClock(s);
    }

    SnapshotNative(s);
    switch (s->mode)
    {
    case MODE_FIELD:
        switch (s->screen)
        {
        case SCR_CARD:
            SnapshotCard(s);
            if (sCardBack)
                SnapshotCardBack(s);
            break;
        case SCR_SAVE:
            SnapshotSave(s);
            s->saveStep = sSaveStep;
            s->canSave = FieldIdle();
            CopyText(s->text, sizeof(s->text), sSaveMessage);
            break;
        case SCR_OPTION:
            ReadOptionValues(s->options);
            s->optPage = sOptPage;
            s->optSub = sOptSub;
            SnapshotOptionRows(s);
            if (sInside == INSIDE_OPTIONS)
                s->optFocus = sOptFocus;
            break;
        case SCR_NUZLOCKE:
            SnapshotNuzlocke(s);
            break;
        }
        break;
    case MODE_BATTLE_ACTION:
    {
        u8 b = sAsked.battler;
        SnapshotBattle(s);
        SnapshotPartyBalls(s);
        s->battler = b;
        s->cursor = sQuickBallFocus ? 4 : gActionSelectionCursor[b];
        if (s->safari)
            s->safariBalls = gNumSafariBalls;
        else
            s->quickBall = CtrBattle_QuickBallItem();
        if (s->quickBall != ITEM_NONE)
            s->quickBallCount = CountTotalItemQuantityInBag(s->quickBall);
        break;
    }
    case MODE_BATTLE_MOVE:
    case MODE_BATTLE_TARGET:
    {
        u8 b = sAsked.battler;
        SnapshotBattle(s);
        s->battler = b;
        s->cursor = sMoveCancel ? MAX_MON_MOVES : gMoveSelectionCursor[b];
        memcpy(&s->moves4, &gBattleBufferA[b][4], sizeof(s->moves4));
        break;
    }
    case MODE_BATTLE_INFO:
        SnapshotBattle(s);
        break;
    }
}

/* Loads one icon or picture the snapshot needs; TRUE if it did. */
/* The picture the summary shows: an egg's, an UNOWN's letter. */
static u16 SummaryPicSpecies(const struct CtrSummaryView *v)
{
    if (v->isEgg)
        return SPECIES_EGG;
    if (v->species == SPECIES_UNOWN)
    {
        u16 letter = GetUnownLetterByPersonality(v->pid);

        return letter ? SPECIES_UNOWN_B + letter - 1 : SPECIES_UNOWN;
    }
    return v->species;
}

static bool8 Prefetch(const ViewState *s)
{
    sIconBudget = TRUE;
    for (int i = 0; i < PARTY_SIZE && sIconBudget; ++i)
        if (s->party[i].species)
            MonIcon(s->party[i].iconSpecies, s->party[i].deoxys);
    /* The party menu's icons move: their second frames too. */
    for (int i = 0; i < PARTY_SIZE && sIconBudget && s->native == NATIVE_PARTY; ++i)
        if (s->party[i].species)
            MonIconFrame(s->party[i].iconSpecies, s->party[i].deoxys, 1);
    for (int i = 0; i < s->encCount && sIconBudget; ++i)
        if (s->enc[i].seen)
            MonIcon(GetIconSpecies(s->enc[i].species, 0), FALSE);
    if (sIconBudget && s->registered != ITEM_NONE && s->mode < MODE_BATTLE_INFO)
        ItemIcon(s->registered);
    if (sIconBudget && s->mode == MODE_BATTLE_ACTION && s->quickBall != ITEM_NONE)
        ItemIcon(s->quickBall);
    if (sIconBudget && s->mode == MODE_BATTLE_ACTION && s->safari)
        ItemIcon(ITEM_SAFARI_BALL);
    if (s->native == NATIVE_SUMMARY && !s->sv.loading)
    {
        if (sIconBudget)
            FrontPic(SummaryPicSpecies(&s->sv), s->sv.shiny);
        if (sIconBudget && s->sv.ball)
            ItemIcon(s->sv.ball);
        if (sIconBudget && s->sv.item)
            ItemIcon(s->sv.item);
    }
    if (s->native == NATIVE_DEX)
    {
        if (sIconBudget && s->dx.species)
            FrontPic(s->dx.species, FALSE);
        for (int i = 0; i < CTR_DEX_ROWS && sIconBudget; ++i)
            if (s->dx.rows[i].species)
                MonIcon(GetIconSpecies(s->dx.rows[i].species, 0), FALSE);
    }
    if (s->native == NATIVE_BAG)
    {
        if (sIconBudget)
            BagPic(s->bv.female, s->bv.bagFrame);
        if (sIconBudget)
            ItemIcon(s->bv.item != ITEM_NONE ? s->bv.item : ITEM_LIST_END);
    }
    if (sIconBudget)
    {
        sIconBudget = FALSE;
        return FALSE;
    }
    return TRUE;
}

#include "3ds_bottom_shell.inc"
#include "3ds_bottom_field.inc"
#include "3ds_bottom_options.inc"
#include "3ds_bottom_game.inc"

/* ------------------------------------------------------------------------ */
/* Drawing: battle                                                          */
/* ------------------------------------------------------------------------ */

/*
 * The battle menus, over the whole screen on the battle backdrop (see
 * "Battle art"): FIGHT and the quick ball on top, BAG, POKéMON and RUN under
 * them; the four moves two by two with CANCEL; the target choice. While the
 * turn plays out, the backdrop alone: the top screen shows the battle and its
 * message. Labels are the game's strings and glyphs (DrawSmoothStr), item
 * and type icons the game's; FIGHT's watermark is Rayquaza's own picture.
 */

static const Rgb sHueFight = {230, 52, 56}, sHueBag = {240, 178, 36}, sHueMon = {36, 172, 96},
                 sHueRun = {40, 112, 226}, sHueBall = {140, 76, 200}, sHueCancel = {88, 104, 130},
                 sHueEmpty = {150, 150, 158};
static const Rgb sCream = {252, 245, 234}, sWhite = {255, 255, 255};
static const Rgb sFocusRing[2] = {{255, 120, 40}, {255, 206, 72}};

/* A colour at keep/100 of its brightness. */
static Rgb Shade(Rgb c, int keep)
{
    return (Rgb){(u8)(c.r * keep / 100), (u8)(c.g * keep / 100), (u8)(c.b * keep / 100)};
}

/* A plate's labels: outlined in its dark tone, shadowed darker still. */
#define PLATE_DARK(c) Shade(c, 38)
#define PLATE_SHADOW(c) Shade(c, 27)

/* A move's category (physical, special, status), 32x16, as the summary shows
 * it: only with ENHANCEMENTS' PHYS/SPEC SPLIT on. */
static void DrawCategoryIcon(u16 move, int x, int y)
{
    if (sRes.categoryTiles && move < MOVES_COUNT && MoveSplitEnabled())
        DrawSprite(sRes.categoryTiles + gMoveCategories[move] * 8 * 32, 4, 2, x, y, sRes.categoryPal.c);
}

static void DrawTypeIcon(u8 type, int x, int y)
{
    if (sRes.typeTiles && type < NUMBER_OF_MON_TYPES)
        DrawSprite(sRes.typeTiles + type * 8 * 32, 4, 2, x, y, sRes.typePal[sTypeIconPal[type]].c);
}

static u8 PlateState(const ViewState *s, u8 hit, bool8 focused)
{
    if ((s->pressed == hit || (hit == HIT_QUICK_BALL
         && (s->pressed == HIT_QUICK_BALL_PREV || s->pressed == HIT_QUICK_BALL_NEXT))) && hit != HIT_NONE)
        return BTA_PRESSED;
    return focused ? BTA_FOCUS : BTA_NORMAL;
}

/*
 * A plate in a colour, worked out once: the same plate in the same colour
 * (and ring) is then copied. Stored a column at a time in the canvas' own
 * order (bottom to top), each pixel as its coverage and the colour it adds;
 * the run of each column the plate covers whole is copied with memcpy, only
 * its edges and shadow are blended. FIGHT's has Rayquaza in it already.
 */
#define TINT_SLOTS 56

typedef struct
{
    u32 age;
    u8 id;
    Rgb colour, ring;
    s16 x, y;
    u16 w, h;
    u16 *p;          /* premultiplied RGB565, w columns of h */
    u8 *a;           /* coverage, the same way */
    u8 *run;         /* per column: the opaque run's first and end index */
    u16 done;        /* columns built so far: a build can span frames (sSliceMs) */
} Tint;

static Tint sTints[TINT_SLOTS];
static u32 sTintClock;

static bool8 SameRgb(Rgb a, Rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* 0: failed, 1: built, 2: more columns to go (the slice was over). */
static int BuildTint(Tint *t, u8 id, Rgb colour, Rgb ring, bool8 resume)
{
    const BtaElem *e = &sBta.e[id];
    bool8 wide = id >= BTA_FIGHT_WIDE && id < BTA_FIGHT_WIDE + 3, full = id >= BTA_FIGHT_FULL && id < BTA_FIGHT_FULL + 3;
    u8 clip = wide ? BTA_CLIP_FIGHT_WIDE : BTA_CLIP_FIGHT_FULL;
    int sink = (wide && id - BTA_FIGHT_WIDE == BTA_PRESSED) || (full && id - BTA_FIGHT_FULL == BTA_PRESSED) ? BTA_SINK : 0;
    int plateW = wide ? 224 : 284;
    const Rgb body = Shade(sHueFight, 82), lines = Shade(sHueFight, 66);

    if (!resume)
    {
        free(t->p);
        t->p = malloc((size_t)e->w * e->h * 3 + e->w * 2);
        if (!t->p)
            return 0;
        t->a = (u8 *)(t->p + e->w * e->h);
        t->run = t->a + e->w * e->h;
        t->id = id;
        t->colour = colour;
        t->ring = ring;
        t->x = e->x;
        t->y = e->y;
        t->w = e->w;
        t->h = e->h;
        t->done = 0;
    }
    for (int i = t->done; i < e->w; ++i)
    {
        int best0 = 0, best1 = 0, start = -1;

        for (int k = 0; k <= e->h; ++k)
        {
            int al = 0;

            if (k < e->h)
            {
                int j = e->h - 1 - k, o = i * e->h + k;
                u32 q = (u32)j * e->w + i;
                int A = e->a[q], R = e->r ? e->r[q] : 0;
                Rgb add = RgbOf(e->c[q * 2] | (e->c[q * 2 + 1] << 8));
                int r = Div255(colour.r * A + ring.r * R) + add.r, g = Div255(colour.g * A + ring.g * R) + add.g,
                    b = Div255(colour.b * A + ring.b * R) + add.b;

                al = e->alpha[q];
                /* FIGHT's watermark, over the colour, inside it. */
                if (wide || full)
                {
                    int px = e->x + i, py = e->y + j, rx = px - (plateW - 84), ry = py - (sink - 1);

                    if (rx >= 0 && ry >= 0 && rx < RAY_SIZE && ry < RAY_SIZE)
                    {
                        int c = BtaAlpha(clip, px, py - sink);
                        int cb = Div255(Div255(sRayBody[ry * RAY_SIZE + rx] * c) * 217);
                        int cl = Div255(Div255(sRayLines[ry * RAY_SIZE + rx] * c) * 153);

                        r = Div255(r * (255 - cb) + body.r * cb);
                        g = Div255(g * (255 - cb) + body.g * cb);
                        b = Div255(b * (255 - cb) + body.b * cb);
                        r = Div255(r * (255 - cl) + lines.r * cl);
                        g = Div255(g * (255 - cl) + lines.g * cl);
                        b = Div255(b * (255 - cl) + lines.b * cl);
                    }
                }
                t->p[o] = PackRgb(r, g, b);
                t->a[o] = al;
            }
            if (k < e->h && al == 255)
            {
                if (start < 0)
                    start = k;
            }
            else if (start >= 0)
            {
                if (k - start > best1 - best0)
                    best0 = start, best1 = k;
                start = -1;
            }
        }
        /* Columns are at most a plate's height: the run fits a byte each. */
        t->run[i * 2] = best0;
        t->run[i * 2 + 1] = best1;
        t->done = i + 1;
        if (sSliceMs > 0.0f && i + 1 < e->w && CtrPlatform_TickMs(CtrPlatform_Ticks() - sSliceStart) >= sSliceMs)
            return 2;
    }
    return 1;
}

static const Tint *GetTint(u8 id, Rgb colour, Rgb ring)
{
    Tint *victim = &sTints[0];
    int state;

    if (id >= BTA_COUNT || !sBta.e[id].alpha || !sBta.e[id].a || sBta.e[id].h > 255)
        return NULL;
    for (int i = 0; i < TINT_SLOTS; ++i)
    {
        Tint *t = &sTints[i];

        /* The ring only shows on a focused plate. */
        if (t->p && t->id == id && SameRgb(t->colour, colour) && (!sBta.e[id].r || SameRgb(t->ring, ring)))
        {
            /* One a slice began and did not finish goes on. */
            if (t->done < t->w)
            {
                int state;

                if (!TakeBuildBudget())
                    return NULL;
                state = BuildTint(t, id, colour, ring, TRUE);
                if (state != 1)
                {
                    t->age = state == 2 ? 0xFFFFFFFF : 0;
                    return NULL;
                }
            }
            t->age = ++sTintClock;
            return t;
        }
        if (t->age < victim->age)
            victim = t;
    }
    if (!TakeBuildBudget())
        return NULL;
    state = BuildTint(victim, id, colour, ring, FALSE);
    if (state != 1)
    {
        /* Unfinished: kept out of the way of the next victim, and of any drawing. */
        victim->age = state == 2 ? 0xFFFFFFFF : 0;
        return NULL;
    }
    victim->age = ++sTintClock;
    return victim;
}

static void DrawTint(const Tint *t, int ax, int ay)
{
    int x0 = ax + t->x + sOX, y0 = ay + t->y;
    int i0 = sClipX0 - x0 > 0 ? sClipX0 - x0 : 0, i1 = sClipX1 - x0 < t->w ? sClipX1 - x0 : t->w;
    /* Rows y0 .. y0 + h - 1 are column indices h - 1 .. 0 (bottom up). */
    int k0 = y0 + t->h - sClipY1, k1 = y0 + t->h - sClipY0;

    if (k0 < 0) k0 = 0;
    if (k1 > t->h) k1 = t->h;
    for (int i = i0; i < i1; ++i)
    {
        u16 *col = sDst + (x0 + i) * H + (H - y0 - t->h);
        const u16 *p = t->p + i * t->h;
        const u8 *a = t->a + i * t->h;
        int r0 = t->run[i * 2], r1 = t->run[i * 2 + 1];

        if (r0 < k0) r0 = k0;
        if (r1 > k1) r1 = k1;
        if (r0 < r1)
            memcpy(col + r0, p + r0, (r1 - r0) * sizeof(u16));
        else
            r0 = r1 = k1;
        for (int k = k0; k < k1; ++k)
        {
            Rgb under, add;
            int al;

            if (k == r0)
            {
                k = r1 - 1;
                continue;
            }
            al = a[k];
            if (!al && !p[k])
                continue;
            if (al == 255)
            {
                col[k] = p[k];
                continue;
            }
            under = RgbOf(col[k]);
            add = RgbOf(p[k]);
            col[k] = PackRgb(Div255(under.r * (255 - al)) + add.r, Div255(under.g * (255 - al)) + add.g,
                             Div255(under.b * (255 - al)) + add.b);
        }
    }
}

/* Whether a plate and what is on it lie outside a partial redraw's clip: its
 * labels then need no drawing (the plate itself and its hit still run). The
 * warm-up (a build budget, an empty clip) walks everything. */
static bool8 PlateOutsideClip(int x, int y, int w, int h)
{
    return sBuildBudget == 0xFF && !ClipIsFull() && !RectsMeet(x - 4, y - 4, x + w + 4, y + h + 8, sClipX0, sClipY0, sClipX1, sClipY1);
}

/* A plate, its hit added; returns the y its contents start at (lower when
 * pressed). Without the art, a flat box in its colour. */
static int DrawBattlePlate(u8 id, int x, int y, int w, int h, Rgb colour, u8 state, u8 blink, u8 hit)
{
    const Tint *t = GetTint(id + state, colour, sFocusRing[blink & 1]);

    if (t)
        DrawTint(t, x, y);
    else if (!sBta.e[id + state].alpha)
        FillRect(x, y, w, h, PackRgb(colour.r, colour.g, colour.b));
    if (hit != HIT_NONE)
        AddHit(x, y, w, h, hit);
    return y + (state == BTA_PRESSED ? BTA_SINK : 0);
}

/* The index-th of the four words of the game's action menu text, split where
 * the game moves on to the next ({CLEAR_TO} or a new line). */
static const u8 *ActionLabel(bool8 safari, int index)
{
    static u8 out[4][24];
    const u8 *src = safari ? gText_SafariZoneMenu : gText_BattleMenu;
    int part = 0, n = 0;

    while (*src != EOS && part <= index)
    {
        if (*src == EXT_CTRL_CODE_BEGIN || *src == CHAR_NEWLINE)
        {
            if (*src++ == EXT_CTRL_CODE_BEGIN && *src != EOS)
                src += GetExtCtrlCodeLength(*src);
            if (n || part < index)
                ++part;
            continue;
        }
        if (part == index && n < (int)sizeof(out[0]) - 1)
            out[index][n++] = *src;
        ++src;
    }
    out[index][n] = EOS;
    return out[index];
}

/* "×N", the game's way of counting items. */
static const u8 *Times(u32 n)
{
    static u8 out[2][8];
    static u8 next;
    u8 *text = out[next++ & 1];

    text[0] = CHAR_MULT_SIGN;
    StringCopy(text + 1, Number(n, 3, STR_CONV_MODE_LEFT_ALIGN));
    return text;
}

#define ACT_Y 32
#define ACT_H 74
#define ROW_Y 138
#define ROW_W 100
#define ROW_H 72

/*
 * Plate composites. A plate with its label and icons is the same picture
 * every time it is in the same look (normal, focused in either blink tone,
 * pressed) and shows the same things (the stamp: moves and their PP, the
 * ball and its count, the party's balls). While the turn plays out each
 * look is drawn once into a picture of its own - the backdrop under it
 * included (WarmBattleMenus) - and moving the cursor, a blink or a press is
 * then a copy of two such pictures: no blending, no label, no icon.
 */
enum { CP_FIGHT, CP_BALL, CP_BAG, CP_MON, CP_RUN, CP_MOVE0, CP_CANCEL = CP_MOVE0 + MAX_MON_MOVES, CP_COUNT };
#define CP_LOOKS 4

typedef struct
{
    u16 *px;               /* the rectangle's columns, in the canvas' own order */
    s16 x, y;
    u16 w, h;
    u32 stamp;
    bool8 valid;
} Comp;

typedef void (*PlateBody)(const ViewState *s, int arg);

static Comp sComps[CP_COUNT][CP_LOOKS];
static bool8 sCompBuild, sCompCapture;
static u8 sCompBudget;
static unsigned sCompFailed;
static bool8 sCompAny;

static u32 Mix(u32 h, u32 v)
{
    return (h ^ v) * 16777619u;
}

static void FreeComps(void)
{
    for (int i = 0; i < CP_COUNT; ++i)
        for (int k = 0; k < CP_LOOKS; ++k)
        {
            free(sComps[i][k].px);
            memset(&sComps[i][k], 0, sizeof(sComps[i][k]));
        }
}

/* The picture over the canvas, inside the clip. */
static void CompCopy(const Comp *c)
{
    int x0 = c->x > sClipX0 ? c->x : sClipX0, x1 = c->x + c->w < sClipX1 ? c->x + c->w : sClipX1;
    int y0 = c->y > sClipY0 ? c->y : sClipY0, y1 = c->y + c->h < sClipY1 ? c->y + c->h : sClipY1;

    if (x0 >= x1 || y0 >= y1)
        return;
    for (int x = x0; x < x1; ++x)
        memcpy(sCanvas + x * H + (H - y1), c->px + (x - c->x) * c->h + (c->h - (y1 - c->y)),
               (size_t)(y1 - y0) * sizeof(u16));
}

/* Every icon the snapshot needs is in (Prefetch loaded none this frame): a
 * plate drawn now is whole, and its picture may be kept. */
static bool8 sIconsReady;
/* Every icon the screen needs is loaded (this frame's sIconsReady stays FALSE
 * on the frame they come in, for the captures' sake). */
static bool8 sIconsIn;
/* Plates kept on screen this frame: one. All of them in the redraw that opens
 * the action menu was 17-18 ms on an Old 3DS; the rest follow on the blinks. */
static u8 sShowCaptures;

/* Draws a plate: from its picture if it has one, else by its body. In the
 * warm-up (sCompBuild) a missing picture is made, one a frame. On screen a
 * look the warm-up has not reached yet is kept as it is drawn, when the
 * redraw holds the whole plate: the ring's next blink in that tone, or the
 * next press, is then a copy. In a battle over the voxel world the warm-up
 * gets little of the frame, and every blink of the action menu drew its
 * plates again, 5-10 ms on an Old 3DS (DROP bottom=9). */
static void DrawCached(int pid, u8 state, u32 stamp, int ax, int ay, u8 tintBase, int hw, int hh, u8 hit,
                       PlateBody body, const ViewState *s, int arg)
{
    u8 look = state == BTA_PRESSED ? 3 : state == BTA_FOCUS ? 1 + (s->blink & 1) : 0;
    Comp *c = &sComps[pid][look];
    const BtaElem *e = &sBta.e[tintBase + state];

    if (!e->alpha || !e->w || !e->h)
    {
        body(s, arg);
        return;
    }
    if (c->valid && c->stamp == stamp)
    {
        CompCopy(c);
        if (hit != HIT_NONE)
            AddHit(ax, ay, hw, hh, hit);
        return;
    }
    bool8 warm = sCompBuild && sCompBudget && !sCompCapture;
    int rx = ax + e->x, ry = ay + e->y;
    bool8 show = !warm && !sCompBuild && !sCompCapture && sBuildBudget == 0xFF && sDst == sCanvas && sIconsReady
              && sShowCaptures && rx >= sClipX0 && ry >= sClipY0 && rx + e->w <= sClipX1 && ry + e->h <= sClipY1;

    if (warm || show)
    {
        int clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};
        u8 budget = sBuildBudget;
        bool8 icons = sIconBudget, refine = sRefineLabels;
        int bad = 0;

        if (rx < 0 || ry < 0 || rx + e->w > W || ry + e->h > H)
        {
            if (show)
                body(s, arg);
            return;
        }
        if (warm)
            --sCompBudget;
        else
            --sShowCaptures;
        sClipX0 = rx;
        sClipY0 = ry;
        sClipX1 = rx + e->w;
        sClipY1 = ry + e->h;
        if (warm)
        {
            sBuildBudget = 0;           /* nothing may be built meanwhile: a gap would be kept */
            sIconBudget = TRUE;
        }
        sCompBad = FALSE;
        sRefineLabels = FALSE;      /* a label drawn flat for now is not kept either */
        sCompCapture = TRUE;
        CopyCache(sCache[CACHE_BATTLE] ? CACHE_BATTLE : CACHE_WIDE);
        body(s, arg);
        sCompCapture = FALSE;
        bad = sCompBad || sRefineLabels;
        sRefineLabels |= refine;
        if (!bad && (c->px == NULL || c->w != e->w || c->h != e->h))
        {
            free(c->px);
            c->px = malloc((size_t)e->w * e->h * sizeof(u16));
        }
        if (!bad && c->px)
        {
            for (int x = 0; x < e->w; ++x)
                memcpy(c->px + x * e->h, sCanvas + (rx + x) * H + (H - ry - e->h), (size_t)e->h * sizeof(u16));
            c->x = rx;
            c->y = ry;
            c->w = e->w;
            c->h = e->h;
            c->stamp = stamp;
            c->valid = TRUE;
            sCompAny = TRUE;
        }
        else if (warm)
            ++sCompFailed;
        sClipX0 = clip[0];
        sClipY0 = clip[1];
        sClipX1 = clip[2];
        sClipY1 = clip[3];
        sBuildBudget = budget;
        sIconBudget = icons;
        return;
    }
    body(s, arg);
}

static void FightBody(const ViewState *s, int wide)
{
    int x = wide ? 6 : 18, w = wide ? 224 : 284;
    u8 state = PlateState(s, HIT_ACTION + 0, s->cursor == 0);
    int oy = DrawBattlePlate(wide ? BTA_FIGHT_WIDE : BTA_FIGHT_FULL, x, ACT_Y, w, ACT_H, sHueFight, state, s->blink,
                             HIT_ACTION + 0);
    Rgb dark = PLATE_DARK(sHueFight), shadow = PLATE_SHADOW(sHueFight);
    const u8 *label = ActionLabel(s->safari, 0);

    /* Rayquaza is in the plate's tint (BuildTint). */
    if (PlateOutsideClip(x, ACT_Y, w, ACT_H))
        return;
    if (s->safari)
    {
        DrawItemIcon(ITEM_SAFARI_BALL, x + w / 2 - 34, oy + ACT_H / 2 - 15);
        DrawSmoothStr(&sNormal, label, x + w / 2 - 4, oy + ACT_H / 2 - 16, 8, sCream, dark, &shadow);
        DrawSmoothStr(&sSmall, Times(s->safariBalls), x + w / 2 + 52, oy + ACT_H / 2 + 4, 4, sWhite, dark, NULL);
        return;
    }
    DrawSmoothStr(&sNormal, label, x + (60 + w - 64) / 2 - SmoothInkWidth(&sNormal, label, 10) / 2,
                  oy + ACT_H / 2 - 21, 10, sCream, dark, &shadow);
}

static void DrawFightPlate(const ViewState *s, bool8 wide)
{
    int x = wide ? 6 : 18, w = wide ? 224 : 284;
    u8 state = PlateState(s, HIT_ACTION + 0, s->cursor == 0);
    u32 stamp = Mix(Mix(Mix(1, wide), s->safari), s->safariBalls);

    DrawCached(CP_FIGHT, state, stamp, x, ACT_Y, wide ? BTA_FIGHT_WIDE : BTA_FIGHT_FULL, w, ACT_H, HIT_ACTION + 0,
               FightBody, s, wide);
}

/* The quick ball plate's name and count, its contents starting at oy. */
static void QuickBallLabels(u16 ball, u16 count, int oy)
{
    int x = 236, w = 78;
    Rgb dark = PLATE_DARK(sHueBall), shadow = PLATE_SHADOW(sHueBall);
    const u8 *name = GetItemName(ball), *times = Times(count);

    DrawSmoothStr(&sSmall, name, x + w / 2 - SmoothInkWidth(&sSmall, name, 4) / 2, oy + 6, 4, sWhite, dark, NULL);
    DrawSmoothStr(&sNormal, times, x + w / 2 - SmoothInkWidth(&sNormal, times, 4) / 2, oy + ACT_H - 16, 4,
                  sCream, dark, &shadow);
}

static void QuickBallBody(const ViewState *s, int unused)
{
    int x = 236, w = 78;

    (void)unused;
    u8 state = PlateState(s, HIT_QUICK_BALL, s->cursor == 4);
    int oy = DrawBattlePlate(BTA_BALL, x, ACT_Y, w, ACT_H, sHueBall, state, s->blink, HIT_QUICK_BALL);

    if (PlateOutsideClip(x, ACT_Y, w, ACT_H))
        return;
    DrawItemIconShadow(s->quickBall, x + 27, oy + ACT_H / 2 - 11);
    DrawItemIcon(s->quickBall, x + 26, oy + ACT_H / 2 - 13);
    QuickBallLabels(s->quickBall, s->quickBallCount, oy);
    /* The cycle arrows: translucent, the plate shows through. */
    for (int row = 0; row < 13; ++row)
    {
        int halfWidth = (row < 6 ? row : 12 - row) + 1;

        ShadeRect(x + 7 + 4 - halfWidth, oy + ACT_H / 2 - 6 + row, halfWidth + 1, 1);
        ShadeRect(x + w - 8 - 4, oy + ACT_H / 2 - 6 + row, halfWidth + 1, 1);
    }
}

static void DrawQuickBall(const ViewState *s)
{
    u8 state = PlateState(s, HIT_QUICK_BALL, s->cursor == 4);
    u32 stamp = Mix(Mix(2, s->quickBall), s->quickBallCount);

    DrawCached(CP_BALL, state, stamp, 236, ACT_Y, BTA_BALL, 78, ACT_H, HIT_QUICK_BALL, QuickBallBody, s, 0);
    AddHit(236, ACT_Y, 16, ACT_H, HIT_QUICK_BALL_PREV);
    AddHit(298, ACT_Y, 16, ACT_H, HIT_QUICK_BALL_NEXT);
}

static void ActionRowBody(const ViewState *s, int k)
{
    static const s16 rowX[3] = {6, 110, 214};
    const Rgb hues[3] = {sHueBag, sHueMon, sHueRun};
    int x = rowX[k], idx = k + 1;
    u8 state = PlateState(s, HIT_ACTION + idx, s->cursor == idx);
    int oy = DrawBattlePlate(BTA_BOTTOM, x, ROW_Y, ROW_W, ROW_H, hues[k], state, s->blink, HIT_ACTION + idx);
    const u8 *label = ActionLabel(s->safari, idx);

    if (PlateOutsideClip(x, ROW_Y, ROW_W, ROW_H))
        return;
    if (k == 1 && !s->safari)
        for (int b = 0; b < PARTY_SIZE; ++b)
            DrawBta(BTA_PARTY_OK + s->partyBalls[b], x + ROW_W - 82 + b * 14, oy + 18, hues[k], hues[k]);
    else if (s->safari)
        DrawBta(k == 0 ? BTA_ICON_BLOCK : k == 1 ? BTA_ICON_NEAR : BTA_ICON_RUN, x, oy, hues[k], hues[k]);
    else
        DrawBta(k == 0 ? BTA_ICON_BAG : BTA_ICON_RUN, x, oy, hues[k], hues[k]);
    DrawSmoothStr(&sNormal, label, x + 9, oy + ROW_H - 28, 6, PLATE_DARK(hues[k]), sCream, NULL);
}

static void DrawBattleActions(const ViewState *s)
{
    static const s16 rowX[3] = {6, 110, 214};
    bool8 ball = !s->safari && s->quickBall != ITEM_NONE;

    DrawFightPlate(s, ball);
    if (ball)
        DrawQuickBall(s);
    for (int k = 0; k < 3; ++k)
    {
        int idx = k + 1;
        u32 stamp = Mix(Mix(3 + k, s->safari), 0);

        if (k == 1)
            for (int b = 0; b < PARTY_SIZE; ++b)
                stamp = Mix(stamp, s->partyBalls[b]);
        DrawCached(CP_BAG + k, PlateState(s, HIT_ACTION + idx, s->cursor == idx), stamp, rowX[k], ROW_Y, BTA_BOTTOM,
                   ROW_W, ROW_H, HIT_ACTION + idx, ActionRowBody, s, k);
    }
}

/* The PP's colour as the game warns: at 0 red, then orange to a quarter,
 * yellow to half. */
static void PpColours(u8 pp, u8 maxPp, u16 *fg, u16 *shadow)
{
    if (pp == 0)
        *fg = TXT_RED, *shadow = TXT_LRED;
    else if (pp <= maxPp / 4)
        *fg = PackRgb(224, 112, 32), *shadow = PackRgb(248, 200, 152);
    else if (pp <= maxPp / 2)
        *fg = PackRgb(200, 160, 16), *shadow = PackRgb(248, 232, 152);
    else
        *fg = TXT_DARK, *shadow = TXT_LIGHT;
}

#define MOVE_W 150
#define MOVE_H 80

static void MoveBody(const ViewState *s, int i)
{
    int x = i & 1 ? 164 : 6, y = i & 2 ? 106 : 20, oy;
    u16 move = s->moves4.moves[i], fg, sh;
    const struct BattleMove *data = &gBattleMoves[move];
    Rgb colour, dark, shadow;
    u8 text[20];

    if (move == MOVE_NONE)
    {
        DrawBattlePlate(BTA_MOVE, x, y, MOVE_W, MOVE_H, sHueEmpty, BTA_NORMAL, 0, HIT_NONE);
        return;
    }
    colour = data->type < NUMBER_OF_MON_TYPES ? sTypeColour[data->type] : sHueEmpty;
    dark = PLATE_DARK(colour);
    shadow = PLATE_SHADOW(colour);
    oy = DrawBattlePlate(BTA_MOVE, x, y, MOVE_W, MOVE_H, colour, PlateState(s, HIT_MOVE + i, s->cursor == i),
                         s->blink, HIT_MOVE + i);
    if (PlateOutsideClip(x, y, MOVE_W, MOVE_H))
        return;
    DrawSmoothStr(&sNormal, gMoveNames[move], x + 10, oy + 5, 4, sWhite, dark, &shadow);
    DrawTypeIcon(data->type, x + 11, oy + 33);
    DrawCategoryIcon(move, x + 46, oy + 33);
    StringCopy(text, gText_MoveInterfacePP);
    StringAppend(text, Number(s->moves4.currentPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
    StringAppend(text, gText_Slash);
    StringAppend(text, Number(s->moves4.maxPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
    PpColours(s->moves4.currentPp[i], s->moves4.maxPp[i], &fg, &sh);
    DrawStrRight(&sNormal, text, x + MOVE_W - 12, oy + 32, fg, sh);
    {
        int tx = DrawStr(&sSmall, Ascii(CTR_TEXT("POW ", "POT. ", "PUISS. ", "POT. ", "STÄRKE ", "POD. ")), x + 12, oy + 56, TXT_DARK, TXT_LIGHT);

        DrawStr(&sSmall, data->power > 1 ? Number(data->power, 3, STR_CONV_MODE_LEFT_ALIGN) : Ascii("---"), tx,
                oy + 56, TXT_DARK, TXT_LIGHT);
        StringCopy(text, Ascii(CTR_TEXT("ACC ", "PREC. ", "PRÉC. ", "PREC. ", "GENAU. ", "PREC. ")));
        StringAppend(text, data->accuracy ? Number(data->accuracy, 3, STR_CONV_MODE_LEFT_ALIGN) : Ascii("---"));
        DrawStrRight(&sSmall, text, x + MOVE_W - 12, oy + 56, TXT_DARK, TXT_LIGHT);
    }
}

static void DrawMovePlate(const ViewState *s, int i)
{
    int x = i & 1 ? 164 : 6, y = i & 2 ? 106 : 20;
    u16 move = s->moves4.moves[i];
    /* The category icon follows ENHANCEMENTS' PHYS/SPEC SPLIT. */
    u32 stamp = Mix(Mix(Mix(Mix(Mix(4, move), s->moves4.currentPp[i]), s->moves4.maxPp[i]), i), MoveSplitEnabled());

    if (move == MOVE_NONE)
        MoveBody(s, i);
    else
        DrawCached(CP_MOVE0 + i, PlateState(s, HIT_MOVE + i, s->cursor == i), stamp, x, y, BTA_MOVE, MOVE_W, MOVE_H,
                   HIT_MOVE + i, MoveBody, s, i);
}

static void CancelBody(const ViewState *s, int unused)
{
    int oy = DrawBattlePlate(BTA_CANCEL, 84, 192, 152, 26, sHueCancel,
                             PlateState(s, HIT_CANCEL, s->cursor == MAX_MON_MOVES), s->blink, HIT_CANCEL);

    (void)unused;
    DrawSmoothStr(&sNormal, gText_Cancel2, 160 - SmoothInkWidth(&sNormal, gText_Cancel2, 4) / 2, oy + 5, 4, sWhite,
                  PLATE_DARK(sHueCancel), NULL);
}

static void DrawBattleMoves(const ViewState *s)
{
    for (int i = 0; i < MAX_MON_MOVES; ++i)
        DrawMovePlate(s, i);
    DrawCached(CP_CANCEL, PlateState(s, HIT_CANCEL, s->cursor == MAX_MON_MOVES), 5, 84, 192, BTA_CANCEL, 152, 26,
               HIT_CANCEL, CancelBody, s, 0);
}

/* Left and right move the game's target cursor; OK is what A does. */
static void DrawBattleTarget(const ViewState *s)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS}, ok[] = {CHAR_O, CHAR_K, EOS};
    const struct { s16 x, y; Rgb hue; const u8 *label; u8 hit; } plates[4] = {
        {6, ACT_Y, sHueRun, left, HIT_TARGET_LEFT},
        {164, ACT_Y, sHueRun, right, HIT_TARGET_RIGHT},
        {6, ROW_Y, sHueFight, ok, HIT_TARGET_OK},
        {164, ROW_Y, sHueCancel, gText_Cancel2, HIT_CANCEL},
    };

    for (int i = 0; i < 4; ++i)
    {
        Rgb dark = PLATE_DARK(plates[i].hue), shadow = PLATE_SHADOW(plates[i].hue);
        int oy = DrawBattlePlate(BTA_TARGET, plates[i].x, plates[i].y, MOVE_W, ROW_H, plates[i].hue,
                                 PlateState(s, plates[i].hit, plates[i].hit == HIT_TARGET_OK), s->blink, plates[i].hit);

        DrawSmoothStr(&sNormal, plates[i].label, plates[i].x + 85 - SmoothInkWidth(&sNormal, plates[i].label, 8) / 2,
                      oy + 22, 8, sCream, dark, &shadow);
    }
}

/*
 * While the turn plays out (the backdrop alone) the next menus are made
 * ready, one plate or label a frame: the action menu as it will come and the
 * moves of the mon on the left, with the focus ring's two blink tones. The
 * menus are walked with an empty clip, so nothing is drawn: only the one
 * piece missing is built. When they come they are copied together instead
 * of worked out in the frame they appear in. A pressed plate is built when
 * it is first pressed.
 */
static u32 sWarmKey = 0xFFFFFFFF;
/* The menus the walk below is on, and how far it is: with the pictures freed
 * it starts again, even for the same menus (the next battle, same moves). */
static u32 sWarmIdxKey = 0xFFFFFFFF;
static bool8 sWarmDone;
#define WARM_SLICE_MS 1.5f
/*
 * The slice is what the frame leaves, not a fixed 1.5 ms: a 3D battle's frame
 * is 15-17 ms of work on an Old 3DS before any of this, and a fixed slice on
 * top of it dropped the frame (DROP bottom=6-7). Read off the last frame's
 * work less its bottom screen, with WARM_MARGIN_MS to spare; under
 * WARM_MIN_MS nothing is warmed, but after WARM_STARVED_FRAMES such frames
 * WARM_MIN_MS is taken anyway, so the menus still get made.
 */
#define WARM_FRAME_MS 16.7f
#define WARM_MARGIN_MS 1.0f
#define WARM_MIN_MS 0.4f
#define WARM_STARVED_FRAMES 20u
/* What a plate's picture (DrawCached in the warm-up) cost when last made. */
static float sWarmCompMs = 1.0f;
/*
 * A picture is made whole in its pass, and costs more than WARM_SLICE_MS: it
 * may take what the frame leaves, up to WARM_COMP_ROOM_MS. Held to the slice
 * it waited WARM_STARVED_FRAMES for each, and the action menu came up with
 * most of its looks missing.
 */
#define WARM_COMP_ROOM_MS 5.0f

/* What the next menus will show, as the game will hand it to Snapshot. */
static void WarmView(ViewState *v, u8 battler)
{
    memset(v, 0, sizeof(*v));
    v->safari = (gBattleTypeFlags & BATTLE_TYPE_SAFARI) != 0;
    v->quickBall = v->safari ? ITEM_NONE : CtrBattle_QuickBallItem();
    v->pressed = HIT_NONE;
    if (v->safari)
        v->safariBalls = gNumSafariBalls;
    if (v->quickBall != ITEM_NONE)
        v->quickBallCount = CountTotalItemQuantityInBag(v->quickBall);
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        v->moves4.moves[i] = gBattleMons[battler].moves[i];
        v->moves4.currentPp[i] = gBattleMons[battler].pp[i];
        v->moves4.maxPp[i] = CalculatePPWithBonus(gBattleMons[battler].moves[i], gBattleMons[battler].ppBonuses, i);
    }
    SnapshotPartyBalls(v);
}

static void WarmBattleMenus(void)
{
    static ViewState v;
    uint64_t start;
    float slice;
    u8 battler = GetBattlerAtPosition(B_POSITION_PLAYER_LEFT);
    int ox = sOX, hits = sHitCount, clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};
    u32 key = 1;
    float room;

    if (!sBta.file || battler >= MAX_BATTLERS_COUNT || sBuildBudget != 0xFF)
        return;
    WarmView(&v, battler);
    key = Mix(key, v.safari);
    key = Mix(key, v.quickBall);
    key = Mix(key, v.quickBallCount);
    key = Mix(key, v.safariBalls);
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        key = Mix(key, v.moves4.moves[i]);
        key = Mix(key, v.moves4.currentPp[i]);
        key = Mix(key, v.moves4.maxPp[i]);
    }
    for (int i = 0; i < PARTY_SIZE; ++i)
        key = Mix(key, v.partyBalls[i]);
    if (key == sWarmKey)
        return;
    {
        static unsigned starved;
        const CtrTiming *timing = CtrPlatform_GetTiming();

        slice = WARM_FRAME_MS - (timing->workMs - timing->bottomMs) - WARM_MARGIN_MS;
        room = slice < WARM_COMP_ROOM_MS ? slice : WARM_COMP_ROOM_MS;
        if (slice > WARM_SLICE_MS)
            slice = WARM_SLICE_MS;
        if (slice < WARM_MIN_MS)
        {
            if (++starved < WARM_STARVED_FRAMES)
                return;
            slice = WARM_MIN_MS;
        }
        starved = 0;
        if (room < slice)
            room = slice;
    }
    ResolveFonts();
    sOX = 0;
    sClipX0 = sClipY0 = sClipX1 = sClipY1 = 0;
    sBuildBudget = 1;
    /* A label is built a few milliseconds a frame, not all at once. */
    start = CtrPlatform_Ticks();
    sSliceStart = start;
    sSliceMs = slice;
    /* Every look a plate can have, in the order it is likely to be wanted: the
     * focus on each plate in both blink tones, then each plate pressed. First
     * the tints and labels (phase 0), then each look's picture (phase 1,
     * DrawCached): moving the cursor onto a plate that was never lit built its
     * tint in the frame it was lit, and drawing one is still blending. */
    {
        static int idx;
        int nA = v.quickBall != ITEM_NONE ? 5 : 4, per = nA + MAX_MON_MOVES + 1, total = per * 3;

        if (sWarmIdxKey != key)
        {
            sWarmIdxKey = key;
            idx = 0;
            sCompFailed = 0;
        }
        static unsigned compWait;
        bool8 first = TRUE;

        while (idx < total * 2 && sBuildBudget == 1
            && CtrPlatform_TickMs(CtrPlatform_Ticks() - start) < (idx >= total ? room : slice))
        {
            int c = idx % total, r = c % per;
            bool8 action = r < nA;
            int n = action ? r : r - nA;
            uint64_t passStart = CtrPlatform_Ticks();

            /* A plate's picture is made whole in its pass: only when what it
             * cost last fits, or, waited for long enough, as the frame's first. */
            if (idx >= total && CtrPlatform_TickMs(passStart - start) + sWarmCompMs > room)
            {
                if (!first || ++compWait < WARM_STARVED_FRAMES)
                    break;
            }
            compWait = 0;
            first = FALSE;

            sCompBuild = idx >= total;
            sCompBudget = 1;
            v.mode = action ? MODE_BATTLE_ACTION : MODE_BATTLE_MOVE;
            v.blink = c < per * 2 ? c / per : 0;
            v.cursor = 0;
            v.pressed = HIT_NONE;
            if (c >= per * 2)
                v.pressed = action ? (n == 4 ? HIT_QUICK_BALL : HIT_ACTION + n) : (n == MAX_MON_MOVES ? HIT_CANCEL : HIT_MOVE + n);
            else
                v.cursor = n;
            if (action)
                DrawBattleActions(&v);
            else
                DrawBattleMoves(&v);
            sCompBuild = FALSE;
            if (idx >= total && sCompBudget == 0)
            {
                float ms = CtrPlatform_TickMs(CtrPlatform_Ticks() - passStart);

                sWarmCompMs = ms > sWarmCompMs ? ms : sWarmCompMs * 0.8f + ms * 0.2f;
            }
            /* A walk that built something is made again next frame, to be sure. */
            if (sBuildBudget == 1 && sCompBudget == 1)
                ++idx;
            else if (sCompFailed > 8)
                idx = total * 2;   /* the pictures will not go in: the menus draw as before */
        }
        sWarmDone = idx >= total * 2;
    }
    sSliceMs = 0.0f;
    /* A pass that built nothing: all of it is ready. */
    if (sBuildBudget == 1 && !sJob.t && sWarmDone)
        sWarmKey = key;
    sBuildBudget = 0xFF;
    sOX = ox;
    sHitCount = hits;
    sClipX0 = clip[0];
    sClipY0 = clip[1];
    sClipX1 = clip[2];
    sClipY1 = clip[3];
}

/*
 * What an idle field frame may spend making a cache ahead: the slice
 * WarmBattleMenus takes, what the last frame's work leaves, at most
 * WARM_SLICE_MS. FALSE while that leaves less than WARM_MIN_MS, until the
 * warm-up has been starved WARM_STARVED_FRAMES frames: then WARM_MIN_MS.
 */
static bool8 IdleSlice(unsigned *starved, float *slice)
{
    const CtrTiming *timing = CtrPlatform_GetTiming();

    *slice = WARM_FRAME_MS - (timing->workMs - timing->bottomMs) - WARM_MARGIN_MS;
    if (*slice > WARM_SLICE_MS)
        *slice = WARM_SLICE_MS;
    if (*slice < WARM_MIN_MS)
    {
        if (++*starved < WARM_STARVED_FRAMES)
            return FALSE;
        *slice = WARM_MIN_MS;
    }
    *starved = 0;
    return TRUE;
}

/* The Poke Balls pocket slot of the first stack of `ball`, or the capacity. */
static u32 FirstBallStack(u16 ball)
{
    u32 i = 0;

    while (i < gBagPockets[BALLS_POCKET].capacity && gBagPockets[BALLS_POCKET].itemSlots[i].itemId != ball)
        ++i;
    return i;
}

/* The ball a press of the plate's arrow would choose: the next one in the
 * Poke Balls pocket that the bag holds, either way round, from the first
 * stack of the current one, as CtrBattle_QuickBallCycle walks it (every ball
 * counts once, at its first stack). ITEM_NONE if none is. */
static u16 NeighbourBall(u16 current, s8 direction)
{
    u32 capacity = gBagPockets[BALLS_POCKET].capacity, at = FirstBallStack(current);

    if (at == capacity)
        return ITEM_NONE;
    for (u32 step = 1; step < capacity; ++step)
    {
        u32 slot = (at + (direction > 0 ? step : capacity - step)) % capacity;
        u16 ball = gBagPockets[BALLS_POCKET].itemSlots[slot].itemId;

        if (ball >= FIRST_BALL && ball <= LAST_BALL && FirstBallStack(ball) == slot && CheckBagHasItem(ball, 1))
            return ball;
    }
    return ITEM_NONE;
}

/* The ball WarmQuickBalls made ready the neighbours of (ITEM_NONE: none). */
static u16 sQuickWarmBall = ITEM_NONE;

/*
 * While the action menu waits, the plates of the balls either arrow would
 * bring are made ready: their icons (one a frame) and their name and count,
 * with the slice an idle frame leaves. A new ball is then its plate drawn
 * from what is ready, not built in the frame it is chosen. Once all of it
 * is, nothing more is asked until another ball is chosen.
 */
static void WarmQuickBalls(const ViewState *s)
{
    static unsigned starved;
    static u16 count, balls[2], counts[2];
    static bool8 ready;
    int ox = sOX, hits = sHitCount, clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};
    bool8 icons = TRUE;
    float slice;

    if (s->mode != MODE_BATTLE_ACTION || s->safari || s->quickBall == ITEM_NONE || sBuildBudget != 0xFF)
        return;
    if (sQuickWarmBall != s->quickBall || count != s->quickBallCount)
    {
        sQuickWarmBall = s->quickBall;
        count = s->quickBallCount;
        for (int i = 0; i < 2; ++i)
        {
            balls[i] = NeighbourBall(sQuickWarmBall, i == 0 ? 1 : -1);
            counts[i] = balls[i] != ITEM_NONE ? CountTotalItemQuantityInBag(balls[i]) : 0;
        }
        ready = FALSE;
    }
    if (ready)
        return;
    sIconBudget = TRUE;
    for (int i = 0; i < 2; ++i)
        if (balls[i] != ITEM_NONE && ItemIcon(balls[i]) < 0 && !sIconBudget)
            icons = FALSE;
    sIconBudget = FALSE;
    if (!IdleSlice(&starved, &slice))
        return;
    ResolveFonts();
    sOX = 0;
    sClipX0 = sClipY0 = sClipX1 = sClipY1 = 0;
    sBuildBudget = 1;
    sSliceStart = CtrPlatform_Ticks();
    sSliceMs = slice;
    for (int i = 0; i < 2 && sBuildBudget == 1; ++i)
        if (balls[i] != ITEM_NONE)
            QuickBallLabels(balls[i], counts[i], ACT_Y);
    /* Nothing was left to build, and the icons were in before this frame. */
    ready = sBuildBudget == 1 && icons;
    sSliceMs = 0.0f;
    sBuildBudget = 0xFF;
    sOX = ox;
    sHitCount = hits;
    sClipX0 = clip[0];
    sClipY0 = clip[1];
    sClipX1 = clip[2];
    sClipY1 = clip[3];
}

/* ------------------------------------------------------------------------ */
/* Redraw and present                                                       */
/* ------------------------------------------------------------------------ */

static void BuildBackgroundCaches(void)
{
    sOX = 0;
    /* The menu's backdrop; the map's window on it; the game's window on it,
     * black inside until the game's picture is there. A battle without its
     * art keeps the party menu's background. */
    LoadMenuBackdrop(sCache[CACHE_MENU]);
    sDst = sCache[CACHE_WIDE];
    DrawPartyBackground(W / 8, H / 8);
    BuildMapCache();
    if (sCache[CACHE_GAME])
    {
        memcpy(sCache[CACHE_GAME], sCache[CACHE_MENU], sizeof(sCanvas));
        sDst = sCache[CACHE_GAME];
        DrawChamferFrame(GAME_WIN_X, GAME_WIN_Y, GAME_WIN_W, GAME_WIN_H, 4, 2);
        FillRect(CTR_BOTTOM_WIN_X, CTR_BOTTOM_WIN_Y, 240, 160, 0);
    }
    sDst = sCanvas;
}

/* The section's backdrop: the map's window, the game's, or the plain one. */
static int SectionCache(const ViewState *s)
{
    if (s->native)
        return CACHE_MENU;
    switch (s->screen)
    {
    case SCR_MAP:
        return s->encView ? CACHE_MENU : CACHE_MAP;
    case SCR_POKEDEX:
    case SCR_POKEMON:
    case SCR_BAG:
        /* Drawn here without the classic menus: the plain backdrop while
         * they open, never the game's black window. */
        if (!sClassicMenus)
            return CACHE_MENU;
        /* fall through */
    case SCR_POKENAV:
        /* Still in the field, on its way: the backdrop and its icon. */
        if (s->mode == MODE_FIELD)
            return CACHE_MENU;
        return sCache[CACHE_GAME] ? CACHE_GAME : CACHE_MENU;
    case SCR_CARD:
        /* The game's card in the window, with the classic menus. */
        if (s->mode == MODE_CARD)
            return sCache[CACHE_GAME] ? CACHE_GAME : CACHE_MENU;
        break;
    }
    return CACHE_MENU;
}

/* A part of the canvas to the screen; with the game's picture in its window,
 * everything round the window and nothing of it. */
static void BlitAroundWindow(int x0, int y0, int x1, int y1)
{
    const int wx0 = CTR_BOTTOM_WIN_X, wy0 = CTR_BOTTOM_WIN_Y, wx1 = wx0 + 240, wy1 = wy0 + 160;
    int my0, my1;

    CtrVideo_BottomStrips(sCanvas);
    if (!CtrVideo_BottomInUse() || sState.native || x1 <= wx0 || x0 >= wx1 || y1 <= wy0 || y0 >= wy1)
    {
        if (x0 == 0 && y0 == 0 && x1 == W && y1 == H)
            CtrBottom_Blit(sCanvas, 0, W);
        else
            CtrBottom_BlitRect(sCanvas, x0, y0, x1, y1);
        return;
    }
    my0 = y0 > wy0 ? y0 : wy0;
    my1 = y1 < wy1 ? y1 : wy1;
    if (y0 < wy0)
        CtrBottom_BlitRect(sCanvas, x0, y0, x1, wy0);
    if (y1 > wy1)
        CtrBottom_BlitRect(sCanvas, x0, wy1, x1, y1);
    if (x0 < wx0)
        CtrBottom_BlitRect(sCanvas, x0, my0, wx0, my1);
    if (x1 > wx1)
        CtrBottom_BlitRect(sCanvas, wx1, my0, x1, my1);
}

/* The battle's entrance (BattleIntro, below). */
static struct
{
    u8 phase, step;
    bool8 defer;
} sIntro;

/*
 * Under the title screen: the developer's logo in the bottom-left corner and
 * the build's version (CTR_APP_VERSION, the Makefile's APP_VERSION) in the
 * bottom-right one, both in the logo's own grey over black.
 */
#ifndef CTR_APP_VERSION
#define CTR_APP_VERSION "dev"
#endif
#define TITLE_MARGIN_X 16
#define TITLE_MARGIN_Y 6
#define TITLE_GREY 100   /* the logo's coverage at its strongest */

static void DrawTitleCredit(void)
{
    int x0 = TITLE_MARGIN_X, y0 = H - TITLE_MARGIN_Y - ART_LOGO_H;
    u16 grey = PackRgb(TITLE_GREY, TITLE_GREY, TITLE_GREY);

    for (int y = 0; y < ART_LOGO_H; ++y)
        for (int x = 0; x < ART_LOGO_W; ++x)
        {
            u8 a = sArtLogo[y * ART_LOGO_W + x];

            if (a)
                Put(x0 + x, y0 + y, PackRgb(a, a, a));
        }
    /* Its capitals level with the logo's lettering (the logo's rows 8 to 20). */
    {
        const u8 *str = Ascii("v" CTR_APP_VERSION);
        int ix0, ix1, top, bottom;

        if (!sNormal.glyphs)
            return;
        InkColumns(&sNormal, str, &ix0, &ix1);
        CapRows(&sNormal, &top, &bottom);
        DrawStr(&sNormal, str, W - TITLE_MARGIN_X - ix1, y0 + 8 + (12 - (bottom - top)) / 2 - top, grey, 0);
    }
}

/*
 * The sections drawn here (map, card, save, options, Nuzlocke) drawn whole, kept with
 * the state they were drawn from. Back from a battle - or from anything that
 * leaves the screen as it was - it is put back instead of drawn again: 10-15 ms in one frame on an Old 3DS, a dropped frame
 * at the end of every battle (F0). Only a screen with nothing animated, every
 * label finished and every icon in, so the copy is what Render would draw;
 * CTR_BOTTOM_CACHE_AUDIT draws it anyway and logs a difference.
 *
 * The copy is taken as a battle opens (MapCacheTake), not after each drawing:
 * on an Old 3DS copying the whole screen at every redraw made the first visit
 * to a screen 3 ms slower than it saved coming back.
 */
static struct
{
    bool8 valid;
    ViewState state;
    u16 canvas[W * H];
    Hit hits[ARRAY_COUNT(sHits)];
    u8 hitCount;
} sMapCache;
/* The canvas is a whole drawing of a cacheable screen, nothing animated, every
 * label and icon in: set by the last whole Render, kept by the partial ones
 * (they keep the canvas what a whole drawing of the new state would be). */
static bool8 sMapShotOk;

static bool8 MapCacheable(const ViewState *s)
{
    return s->mode == MODE_FIELD && s->dock
        && (s->screen == SCR_MAP || s->screen == SCR_CARD || s->screen == SCR_SAVE || s->screen == SCR_OPTION
            || s->screen == SCR_NUZLOCKE)
        && !CtrVideo_BottomInUse() && !CtrVideo_BottomWhole() && ClipIsFull();
}

/* The screen on show, kept for when the battle that opens now is over. */
static void MapCacheTake(void)
{
    if (!sMapShotOk || !MapCacheable(&sShown) || sRefineLabels || sForceRedraw)
        return;
    sMapCache.state = sShown;
    memcpy(sMapCache.canvas, sCanvas, sizeof(sCanvas));
    memcpy(sMapCache.hits, sHits, sizeof(sHits));
    sMapCache.hitCount = sHitCount;
    sMapCache.valid = TRUE;
}

/*
 * A whole redraw over CTR_RENDER_SLOW_MS, phase by phase, for the console's
 * log: Azahar copies memory far faster than an Old 3DS, so where a 10 ms
 * redraw goes can only be read there.
 */
#define CTR_RENDER_SLOW_MS 4.0f
#define CTR_RENDER_SLOW_LINES 60

static void Render(const ViewState *s)
{
    bool8 cacheable = MapCacheable(s), blitted = FALSE;
    uint64_t phase[6] = {CtrPlatform_Ticks()};

    ResolveFonts();
    if (cacheable && sMapCache.valid && memcmp(&sMapCache.state, s, sizeof(*s)) == 0)
    {
#ifdef CTR_BOTTOM_CACHE_AUDIT
        static u16 kept[W * H];
        static Hit keptHits[ARRAY_COUNT(sHits)];
        u8 keptCount = sMapCache.hitCount;
        static unsigned hits;

        memcpy(kept, sMapCache.canvas, sizeof(kept));
        memcpy(keptHits, sMapCache.hits, sizeof(keptHits));
        sMapCache.valid = FALSE;
        Render(s);
        if (memcmp(kept, sCanvas, sizeof(kept)) != 0 || sHitCount != keptCount
            || memcmp(sHits, keptHits, sHitCount * sizeof(Hit)) != 0)
            CtrLog_Write(CTR_LOG_ERROR, "CONTRACT bottom map cache differs from a redraw");
        else
            CtrLog_Write(CTR_LOG_VIDEO, "bottom map cache: same as a redraw (%u)", ++hits);
        return;
#else
        memcpy(sCanvas, sMapCache.canvas, sizeof(sCanvas));
        memcpy(sHits, sMapCache.hits, sizeof(sHits));
        sHitCount = sMapCache.hitCount;
        sDst = sCanvas;
        sOX = 0;
        if (!sIntro.defer)
            CtrBottom_Blit(sCanvas, 0, W);
        sMapShotOk = TRUE;
        return;
#endif
    }
    sHitCount = 0;
    sDst = sCanvas;
    sOX = 0;

    if (s->mode == MODE_OFF)
    {
        memset(sCanvas, 0, sizeof(sCanvas));
        if (s->title)
            DrawTitleCredit();
    }
    else if (s->mode >= MODE_BATTLE_INFO)
    {
        CopyCache(sCache[CACHE_BATTLE] ? CACHE_BATTLE : CACHE_WIDE);
        if (s->mode == MODE_BATTLE_ACTION)
            DrawBattleActions(s);
        else if (s->mode == MODE_BATTLE_MOVE)
            DrawBattleMoves(s);
        else if (s->mode == MODE_BATTLE_TARGET)
            DrawBattleTarget(s);
    }
    else if (!s->dock)
    {
        /* The game's screen opened from a battle, a shop or the PC has the
         * whole screen: black until it is drawn there. */
        FillRect(0, 0, W, H, 0);
        phase[1] = phase[2] = CtrPlatform_Ticks();
    }
    else
    {
        CopyCache(SectionCache(s));
        phase[1] = CtrPlatform_Ticks();
        if (s->native)
            DrawGameView(s);
        else switch (s->screen)
        {
        case SCR_MAP: DrawRegionMap(s); break;
        case SCR_CARD:
            if (s->mode != MODE_CARD)
                DrawTrainerCard(s);
            break;
        case SCR_POKEDEX:
        case SCR_POKEMON:
        case SCR_BAG:
        case SCR_POKENAV:
            if ((!sClassicMenus && s->screen != SCR_POKENAV) || s->mode == MODE_FIELD)
                DrawSectionWait(s);
            break;
        case SCR_SAVE: DrawSave(s); break;
        case SCR_OPTION: DrawOptions(s); break;
        case SCR_NUZLOCKE: DrawNuzlocke(s); break;
        }
        /* The game's party menu, bag, Pokédex and PokéNav are drawn in their
         * window by the compositor; black until they are, as they fade in
         * from black, and while the field is on its way to opening them
         * (OpenAsked): the cache's. */
        phase[2] = CtrPlatform_Ticks();
        DrawHeader(s);
        DrawDock(s);
    }
    phase[3] = CtrPlatform_Ticks();
    phase[4] = CtrPlatform_Ticks();
    /* The game's window is the compositor's while it draws there, and the
     * whole screen the boxes'. */
    if (!ClipIsFull())
    {
        BlitAroundWindow(sClipX0, sClipY0, sClipX1, sClipY1);
        blitted = TRUE;
    }
    else if (!CtrVideo_BottomWhole() && !sIntro.defer)
    {
        BlitAroundWindow(0, 0, W, H);
        blitted = TRUE;
    }
    phase[5] = CtrPlatform_Ticks();
    if (ClipIsFull())
        sMapShotOk = cacheable && !sRefineLabels && sIconsIn;
    /* Whole redraws and partial ones alike (a page of the options is one):
     * the backdrop's copy (with the card's cache finished in it), and the
     * present split into its copy to the screen and its cache flush. */
    {
        static unsigned lines;
        float total = CtrPlatform_TickMs(phase[5] - phase[0]);
        uint64_t copy = 0, flush = 0;

        if (total > CTR_RENDER_SLOW_MS && lines < CTR_RENDER_SLOW_LINES && s->mode == MODE_FIELD)
        {
            if (blitted)
                CtrBottom_LastBlitTicks(&copy, &flush);
            ++lines;
            CtrLog_Write(CTR_LOG_VIDEO, "bottom render f=%lu screen=%u page=%u part=%d,%d-%d,%d total=%.2f: "
                         "backdrop=%.2f screen=%.2f frame=%.2f icons=%.2f blit=%.2f "
                         "(copy=%.2f flush=%.2f) ms",
                         (unsigned long)sFrames, s->screen, s->optPage, sClipX0, sClipY0, sClipX1, sClipY1, total,
                         CtrPlatform_TickMs(phase[1] - phase[0]),
                         CtrPlatform_TickMs(phase[2] - phase[1]), CtrPlatform_TickMs(phase[3] - phase[2]),
                         CtrPlatform_TickMs(phase[4] - phase[3]), CtrPlatform_TickMs(phase[5] - phase[4]),
                         CtrPlatform_TickMs(copy), CtrPlatform_TickMs(flush));
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Partial redraws                                                          */
/* ------------------------------------------------------------------------ */

typedef struct { int x0, y0, x1, y1; } Rect;

static void RectInit(Rect *r)
{
    r->x0 = r->y0 = W;
    r->x1 = r->y1 = 0;
}

static void RectAdd(Rect *r, int x0, int y0, int x1, int y1)
{
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}

/* A button's rectangle, as the last redraw laid it out, with the room its
 * plate's ring and shadow take; every hit of the id counts. FALSE when there
 * is none. */
static bool8 RectAddHitOf(Rect *r, u8 id)
{
    bool8 found = FALSE;

    for (int i = 0; i < sHitCount; ++i)
        if (sHits[i].id == id)
        {
            RectAdd(r, sHits[i].x - PLATE_ML, sHits[i].y - PLATE_MT, sHits[i].x + sHits[i].w + PLATE_MR,
                    sHits[i].y + sHits[i].h + PLATE_MB);
            found = TRUE;
        }
    return found;
}

static bool8 RectAddHit(Rect *r, u8 id)
{
    if (id == HIT_NONE)
        return TRUE;
    /* An option row is lit whole, whichever of its two halves is touched. */
    if (id >= HIT_OPTION && id < HIT_OPTION + 2 * HIT_OPTION_BACK)
    {
        u8 row = (id - HIT_OPTION) % HIT_OPTION_BACK;

        /* An action row has no back half: its one hit is enough. */
        bool8 forward = RectAddHitOf(r, HIT_OPTION + row);
        bool8 backward = RectAddHitOf(r, HIT_OPTION + HIT_OPTION_BACK + row);

        return forward || backward;
    }
    /* A dock button held names its section on the title. */
    if (id >= HIT_COLUMN && id < HIT_COLUMN + SCR_COUNT && sShown.dock)
    {
        int x0, y0, x1, y1;

        TitleRect(&sShown, &x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
    }
    return RectAddHitOf(r, id);
}

/* A battle plate's hit, with the room its ring and shadow take. */
static bool8 RectAddPlate(Rect *r, u8 id)
{
    Rect plate;

    RectInit(&plate);
    if (id == HIT_QUICK_BALL_PREV || id == HIT_QUICK_BALL_NEXT)
        id = HIT_QUICK_BALL;
    if (!RectAddHitOf(&plate, id))
        return FALSE;
    RectAdd(r, plate.x0 - 4, plate.y0 - 4, plate.x1 + 4, plate.y1 + 6);
    return TRUE;
}

/* The battle cursor's plate: the action (or the quick ball, 4), the move
 * (or CANCEL), or OK when choosing a target. */
static u8 BattleCursorHit(const ViewState *s, u8 cursor)
{
    if (s->mode == MODE_BATTLE_ACTION)
        return cursor == 4 ? HIT_QUICK_BALL : HIT_ACTION + cursor;
    if (s->mode == MODE_BATTLE_MOVE)
        return cursor == MAX_MON_MOVES ? HIT_CANCEL : HIT_MOVE + cursor;
    return HIT_TARGET_OK;
}

/* The content area, its plates' shadows and rings included. */
static void AreaRect(Rect *r)
{
    RectAdd(r, AREA_X - PLATE_ML, AREA_Y - PLATE_MT, AREA_X + AREA_W + PLATE_MR, AREA_Y + AREA_H + PLATE_MB);
}

/* What a focus on the dock or the header lights. FALSE when it cannot be told. */
static bool8 RectAddFocus(Rect *r, const ViewState *s, u8 focus)
{
    int x0, y0, x1, y1;

    if (focus == FOCUS_NONE)
        return TRUE;
    if (focus == COL_TITLE)
    {
        TitleRect(s, &x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
        return TRUE;
    }
    if (focus < SCR_COUNT)
    {
        DockButton(s, focus, &x0, &x1);
        PlateBounds(x0, DOCK_TOP, x1, DOCK_H, &x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
        return TRUE;
    }
    return RectAddHit(r, HIT_COLUMN + focus);
}

/*
 * The clip a redraw can be limited to, when the new view differs from the
 * shown one only in things that touch one part of the screen: the button lit
 * by a press or the battle cursor, the clock, the Repel's steps, the
 * options' panel (a value, the focus), the options' tabs. FALSE when anything
 * else differs, or the part cannot be told.
 */
static bool8 DirtyRect(const ViewState *now, const ViewState *shown, Rect *r)
{
    static ViewState probe;
    bool8 battle = now->mode >= MODE_BATTLE_INFO;

    if (now->mode != shown->mode || now->screen != shown->screen || now->dock != shown->dock)
        return FALSE;
    if (!battle && !now->dock)
        return FALSE;
    probe = *now;
    probe.pressed = shown->pressed;
    if (battle)
    {
        probe.cursor = shown->cursor;
        probe.blink = shown->blink;
        /* Another ball chosen: its plate alone (both draw one, of one size). */
        if (now->mode == MODE_BATTLE_ACTION && !now->safari && now->quickBall != ITEM_NONE
         && shown->quickBall != ITEM_NONE)
        {
            probe.quickBall = shown->quickBall;
            probe.quickBallCount = shown->quickBallCount;
        }
        if (memcmp(&probe, shown, sizeof(probe)) != 0)
            return FALSE;
        RectInit(r);
        /* A plate pressed or let go, the cursor moved, its ring blinked. */
        if (now->pressed != shown->pressed
         && !((now->pressed == HIT_NONE || RectAddPlate(r, now->pressed))
              && (shown->pressed == HIT_NONE || RectAddPlate(r, shown->pressed))))
            return FALSE;
        if ((now->cursor != shown->cursor || now->blink != shown->blink)
         && !(RectAddPlate(r, BattleCursorHit(now, now->cursor)) && RectAddPlate(r, BattleCursorHit(now, shown->cursor))))
            return FALSE;
        if ((now->quickBall != shown->quickBall || now->quickBallCount != shown->quickBallCount)
         && !RectAddPlate(r, HIT_QUICK_BALL))
            return FALSE;
        return r->x0 < r->x1 && r->y0 < r->y1;
    }

    /* The header's clock and Repel. */
    probe.clockShown = shown->clockShown;
    probe.clockValid = shown->clockValid;
    probe.clockRate = shown->clockRate;
    probe.clockPhase = shown->clockPhase;
    probe.clockMinute = shown->clockMinute;
    probe.repelSteps = shown->repelSteps;
    probe.focus = shown->focus;
    /* The options' panel, and their tabs. */
    if (now->screen == SCR_OPTION)
    {
        memcpy(probe.options, shown->options, sizeof(probe.options));
        memcpy(probe.rows, shown->rows, sizeof(probe.rows));
        probe.optFocus = shown->optFocus;
        probe.optPage = shown->optPage;
        probe.optSub = shown->optSub;
    }
    /* A native view of the game's screen: its area. */
    if (now->native && now->native == shown->native)
    {
        probe.pv = shown->pv;
        probe.sv = shown->sv;
        probe.bv = shown->bv;
        probe.dx = shown->dx;
        memcpy(probe.party, shown->party, sizeof(probe.party));
    }
    if (memcmp(&probe, shown, sizeof(probe)) != 0)
        return FALSE;
    RectInit(r);
    /* The party menu's icons moving, and nothing else of it: theirs alone. */
    if (now->native == NATIVE_PARTY && memcmp(&now->pv, &shown->pv, offsetof(struct CtrPartyView, iconFrame)) == 0
     && memcmp(now->party, shown->party, sizeof(now->party)) == 0)
    {
        for (int i = 0; i < PARTY_SIZE; ++i)
        {
            int x0, y0, x1, y1;

            if (now->pv.iconFrame[i] == shown->pv.iconFrame[i] && now->pv.iconLift[i] == shown->pv.iconLift[i])
                continue;
            if (PartyIconRect(now, i, &x0, &y0, &x1, &y1))
                RectAdd(r, x0, y0, x1, y1);
        }
    }
    else if (now->native && (memcmp(&now->pv, &shown->pv, sizeof(now->pv)) != 0
                        || memcmp(&now->sv, &shown->sv, sizeof(now->sv)) != 0
                        || memcmp(&now->bv, &shown->bv, sizeof(now->bv)) != 0
                        || memcmp(&now->dx, &shown->dx, sizeof(now->dx)) != 0
                        || memcmp(now->party, shown->party, sizeof(now->party)) != 0))
        AreaRect(r);
    if (now->clockShown != shown->clockShown || now->clockValid != shown->clockValid
     || now->clockRate != shown->clockRate || now->clockPhase != shown->clockPhase
     || now->clockMinute != shown->clockMinute)
    {
        int x0, y0, x1, y1;

        ClockRect(&x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
    }
    if (now->repelSteps != shown->repelSteps)
    {
        int x0, y0, x1, y1;

        RepelRect(&x0, &y0, &x1, &y1);
        RectAdd(r, x0, y0, x1, y1);
    }
    if (now->pressed != shown->pressed && !(RectAddHit(r, now->pressed) && RectAddHit(r, shown->pressed)))
        return FALSE;
    if (now->focus != shown->focus && !(RectAddFocus(r, now, now->focus) && RectAddFocus(r, shown, shown->focus)))
        return FALSE;
    if (now->screen == SCR_OPTION)
    {
        if (now->optPage != shown->optPage || now->optSub != shown->optSub)
            AreaRect(r);
        else if (memcmp(now->options, shown->options, sizeof(now->options)) != 0
              || memcmp(now->rows, shown->rows, sizeof(now->rows)) != 0 || now->optFocus != shown->optFocus)
        {
            int x0, y0, x1, y1;

            OptionsPanelRect(&x0, &y0, &x1, &y1);
            RectAdd(r, x0, y0, x1, y1);
        }
    }
    return r->x0 < r->x1 && r->y0 < r->y1;
}

static void RenderPart(const ViewState *s, int x0, int y0, int x1, int y1);

/* Draws one part of the screen. The canvas outside it keeps what is shown. */
static void RenderPart(const ViewState *s, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1)
        return;
    sClipX0 = x0;
    sClipY0 = y0;
    sClipX1 = x1;
    sClipY1 = y1;
    Render(s);
    sClipX0 = sClipY0 = 0;
    sClipX1 = W;
    sClipY1 = H;
}

/* ------------------------------------------------------------------------ */
/* Pressing buttons for the player: one press, or a START entry             */
/* ------------------------------------------------------------------------ */

enum
{
    PLAN_NONE,
    PLAN_PRESS,      /* one press of `keys` */
    PLAN_START,      /* open start menu entry `target` */
};

typedef struct
{
    u8 kind, steps, wait, tries;
    bool8 release;
    s16 target;
    u16 keys;
} Plan;

static Plan sPlan;
static u16 sInjected;

static Plan MakePlan(u8 kind, s16 target)
{
    Plan p;

    memset(&p, 0, sizeof(p));
    p.kind = kind;
    p.target = target;
    return p;
}

static void StartPlan(u8 kind, s16 target)
{
    sPlan = MakePlan(kind, target);
}

/* One press of keys, as the player's own: the header's Y is SELECT. */
static void PressOnce(u16 keys)
{
    StartPlan(PLAN_PRESS, 0);
    sPlan.keys = keys;
}

static void FinishPlan(void)
{
    sPlan.kind = PLAN_NONE;
}

static void CancelPlan(void)
{
    if (sPlan.kind == PLAN_START)
        CtrStartMenu_Request(START_NONE);
    sPlan.kind = PLAN_NONE;
}

/* START with a request pending opens that entry (see start_menu.c). */
static void RunStartPlan(void)
{
    if (sPlan.tries && !CtrStartMenu_Pending())
    {
        FinishPlan();                             /* served */
        return;
    }
    if (++sPlan.wait < 8 && sPlan.tries)
        return;
    /* START only registers with the player free to move; try a few times. */
    if (sPlan.tries >= 6 || gMain.callback2 != CB2_Overworld || CtrStartMenu_Busy())
    {
        if (sPlan.tries >= 6 || ++sPlan.steps > 90)
            CancelPlan();
        return;
    }
    CtrStartMenu_Request(sPlan.target);
    sInjected = START_BUTTON;
    sPlan.release = TRUE;
    sPlan.wait = 0;
    ++sPlan.tries;
}

static void RunPlan(void)
{
    if (sPlan.kind == PLAN_NONE)
        return;
    /* The player's own buttons always win; not those the bottom screen holds back
     * (CtrBottom_FilterKeys), as the A that chose BAG while it opens. */
    if (CtrBottom_FilterKeys(CtrInput_Get()->held) & CTR_KEY_GAME)
    {
        CancelPlan();
        return;
    }
    /* A press only registers as new after a frame with the key up. */
    if (sPlan.release)
    {
        sPlan.release = FALSE;
        return;
    }
    switch (sPlan.kind)
    {
    case PLAN_PRESS:
        sInjected = sPlan.keys;
        FinishPlan();
        sPlan.release = TRUE;
        return;
    case PLAN_START:
        RunStartPlan();
        return;
    }
}

uint16_t CtrBottom_InjectedKeys(void)
{
    return sInjected;
}

/* ------------------------------------------------------------------------ */
/* The PokéNav by touch                                                     */
/* ------------------------------------------------------------------------ */

/*
 * The PokéNav runs as it is, drawn in the game screen's window by the compositor; a
 * tap on one of its screens becomes the buttons that screen reads, pressed
 * only while the PokéNav waits for input (CtrPokenav_Screen): an option or a
 * list entry is reached with one press of the D-pad, the cursor put next to
 * it first (NavNext), and chosen with A; a place on the map
 * is walked to, a ribbon is picked. The POKéNAV button of the dock is B,
 * and any other button of it leaves the PokéNav for its own screen.
 */
enum
{
    NAV_NONE,
    NAV_PRESS,    /* keys, once */
    NAV_MENU,     /* walk the menu cursor to target */
    NAV_LIST,     /* walk the list selection to target */
    NAV_OPTION,   /* walk Match Call's options cursor to target */
    NAV_PARTY,    /* walk the condition screen's mon to target */
    NAV_MAP,      /* dx, dy presses on the map */
    NAV_RIBBON,   /* walk the ribbon cursor to target */
    NAV_MARK,     /* walk the markings menu's cursor to target */
    NAV_LEAVE,    /* B until the PokéNav closes */
};

static struct
{
    u8 kind, steps, wait;
    bool8 release;
    s16 target, dx, dy;
    u16 keys, finish;
} sNav;

/* Condition graph: the party's balls down the right edge, CANCEL below. */
#define NAV_BALL_X 212
#define NAV_BALL_TOP 8
#define NAV_BALL_STEP 20
/* Ribbon summary: 16x16 cells from here, RIBBONS_PER_ROW to a row. */
#define NAV_RIBBON_X 88
#define NAV_RIBBON_Y 32
#define NAV_RIBBONS_PER_ROW 9
/* Match Call's options: rows of its info box, left of the list. */
#define NAV_OPTION_Y 72
#define NAV_OPTION_W 88

/* Stops what is under way. A NAV_LEAVE given up - a key held, the PokéNav not
 * closing, or another plan for it - drops the screen chosen from the dock:
 * that one is not to open later, when the player closes the PokéNav. */
static void NavStop(void)
{
    if (sNav.kind == NAV_LEAVE)
        sScreen = SCR_MAP;
    sNav.kind = NAV_NONE;
}

static void NavStart(u8 kind, s16 target, u16 finish)
{
    NavStop();
    memset(&sNav, 0, sizeof(sNav));
    sNav.kind = kind;
    sNav.target = target;
    sNav.finish = finish;
}

static void NavPress(u16 keys)
{
    NavStart(NAV_PRESS, 0, 0);
    sNav.keys = keys;
}

static struct PokenavMonList *NavMonList(void)
{
    return GetSubstructPtr(POKENAV_SUBSTRUCT_MON_LIST);
}

/*
 * The press that takes a cursor from cursor to target in one step: the
 * cursor is put right next to the target first (set), so the game's own
 * move - its sound, the option sliding out - happens once, for the target.
 */
static u16 NavNext(int cursor, int target, void (*set)(int))
{
    if (target > cursor + 1) set(target - 1);
    else if (target < cursor - 1) set(target + 1);
    return target > cursor ? DPAD_DOWN : DPAD_UP;
}

static void NavSetMenu(int cursor) { CtrPokenavMenu_SetCursor(cursor); }
static void NavSetList(int cursor) { CtrPokenavList_SetSelected((u16)cursor); }
static void NavSetOption(int cursor) { CtrPokenavMatchCall_SetOption((u16)cursor); }
static void NavSetMark(int cursor) { CtrMonMarkings_SetCursor((s8)cursor); }

/* One step of the plan, or 0 while the cursor is not known. */
static u16 NavStep(bool8 *done)
{
    int cursor = 0, count;
    u16 top, selected, shown, total, option, options, normal, gift, giftStart;
    u8 x, y, width;
    bool8 zoomed, moving, expanded;
    s16 cx, cy;
    struct PokenavMonList *mons;

    *done = FALSE;
    switch (sNav.kind)
    {
    case NAV_PRESS:
        *done = TRUE;
        return sNav.keys;
    case NAV_MENU:
        count = CtrPokenavMenu_Options(&cursor);
        if (sNav.target >= count) break;
        if (cursor == sNav.target) { *done = TRUE; return sNav.finish; }
        return NavNext(cursor, sNav.target, NavSetMenu);
    case NAV_LIST:
        if (!CtrPokenavList_View(&x, &y, &width, &top, &selected, &shown, &total) || sNav.target >= total) break;
        if (selected == sNav.target) { *done = TRUE; return sNav.finish; }
        return NavNext(selected, sNav.target, NavSetList);
    case NAV_OPTION:
        if (CtrPokenavMatchCall_Input(&option, &options) != 1 || sNav.target >= options) break;
        if (option == sNav.target) { *done = TRUE; return sNav.finish; }
        return NavNext(option, sNav.target, NavSetOption);
    case NAV_PARTY:
        if (!(mons = NavMonList()) || sNav.target >= mons->listCount) break;
        if (mons->currIndex == sNav.target) { *done = TRUE; return sNav.finish; }
        return sNav.target > mons->currIndex ? DPAD_DOWN : DPAD_UP;
    case NAV_MAP:
        if (!CtrRegionMap_Cursor(&cx, &cy, &zoomed, &moving) || moving) return 0;
        if (sNav.dx > 0) { --sNav.dx; return DPAD_RIGHT; }
        if (sNav.dx < 0) { ++sNav.dx; return DPAD_LEFT; }
        if (sNav.dy > 0) { --sNav.dy; return DPAD_DOWN; }
        if (sNav.dy < 0) { ++sNav.dy; return DPAD_UP; }
        *done = TRUE;
        return sNav.finish;
    case NAV_RIBBON:
        if (!CtrPokenavRibbons_Summary(&selected, &normal, &gift, &giftStart, &expanded)) break;
        if (!expanded) return A_BUTTON;
        if (selected == sNav.target) { *done = TRUE; return 0; }
        if (selected / NAV_RIBBONS_PER_ROW != sNav.target / NAV_RIBBONS_PER_ROW)
            return sNav.target > selected ? DPAD_DOWN : DPAD_UP;
        return sNav.target > selected ? DPAD_RIGHT : DPAD_LEFT;
    case NAV_MARK:
    {
        s8 mark;
        s16 mx, my;

        if (!CtrPokenavCondition_Marking() || !CtrMonMarkings_Menu(&mark, &mx, &my)) break;
        if (mark == sNav.target) { *done = TRUE; return sNav.finish; }
        return NavNext(mark, sNav.target, NavSetMark);
    }
    case NAV_LEAVE:
        return B_BUTTON;
    }
    /* What the plan was for is gone. */
    sNav.kind = NAV_NONE;
    return 0;
}

static void RunNav(u8 mode)
{
    bool8 ready, done = FALSE;
    u16 keys;

    if (sNav.kind == NAV_NONE)
        return;
    if (mode != MODE_POKENAV)
    {
        sNav.kind = NAV_NONE;
        return;
    }
    /* As in RunPlan: not the keys the bottom screen holds back, such as the A that
     * chose another screen from it and started this NAV_LEAVE. */
    if (CtrBottom_FilterKeys(CtrInput_Get()->held) & CTR_KEY_GAME)
    {
        NavStop();
        return;
    }
    /* A press only registers as new after a frame with the key up. */
    if (sNav.release)
    {
        sNav.release = FALSE;
        return;
    }
    /* A single press also answers what waits inside a task: a call's text. */
    CtrPokenav_Screen(&ready);
    keys = ready || sNav.kind == NAV_PRESS ? NavStep(&done) : 0;
    /* Finished with no final key (a ribbon view, a party or map move with
     * no finish): over now, not left to steer for 240 frames. */
    if (done && !keys)
    {
        sNav.kind = NAV_NONE;
        return;
    }
    if (!keys)
    {
        /* Waiting for the PokéNav to take input, or a move to end. */
        if (++sNav.wait > 240)
            NavStop();
        return;
    }
    if (done && sNav.kind != NAV_LEAVE)
        sNav.kind = NAV_NONE;
    if (++sNav.steps > 64)
        NavStop();
    sInjected = keys;
    sNav.release = TRUE;
    sNav.wait = 0;
}

/* A tap at (x, y) of the PokéNav's picture. */
static void NavTap(int x, int y)
{
    bool8 ready, zoomed, moving, expanded;
    u32 screen = CtrPokenav_Screen(&ready);
    int yStart, deltaY, cursor, count, row;
    u16 top, selected, shown, total, option, options, normal, gift, giftStart;
    u8 lx, ly, width;
    s16 cx, cy;
    struct PokenavMonList *mons;

    if (y < 0 || y >= 160)
        return;
    switch (screen)
    {
    case POKENAV_MAIN_MENU:
    case POKENAV_MAIN_MENU_CURSOR_ON_MAP:
    case POKENAV_CONDITION_MENU:
    case POKENAV_CONDITION_SEARCH_MENU:
    case POKENAV_MAIN_MENU_CURSOR_ON_MATCH_CALL:
    case POKENAV_MAIN_MENU_CURSOR_ON_RIBBONS:
        /* No menu substruct yet (or any more): Rows would read through NULL. */
        if ((count = CtrPokenavMenu_Options(&cursor)) <= 0)
            break;
        CtrPokenavMenu_Rows(&yStart, &deltaY);
        row = (y - yStart + deltaY / 2 + deltaY) / deltaY - 1;
        if (x >= 112 && row >= 0 && row < count)
            NavStart(NAV_MENU, row, A_BUTTON);
        else if (x < 88 && y >= 16 && y < 40)
            NavPress(B_BUTTON);         /* the header: back */
        else
            NavPress(A_BUTTON);         /* a message waiting (no ribbons yet) */
        break;
    case POKENAV_REGION_MAP:
        if (y >= 144)
        {
            /* The help bar: "A ZOOM", then "B CANCEL". */
            if (x < 40) NavPress(A_BUTTON);
            else if (x < 112) NavPress(B_BUTTON);
            break;
        }
        if (!CtrRegionMap_Cursor(&cx, &cy, &zoomed, &moving))
            break;
        {
            int step = zoomed ? 16 : 8;
            int dx = x - cx, dy = y - cy;

            NavStart(NAV_MAP, 0, 0);
            sNav.dx = (dx + (dx >= 0 ? step / 2 : -step / 2)) / step;
            sNav.dy = (dy + (dy >= 0 ? step / 2 : -step / 2)) / step;
            /* On the cursor: the place is chosen, zoom in or out on it. */
            if (!sNav.dx && !sNav.dy)
                sNav.finish = A_BUTTON;
        }
        break;
    case POKENAV_CONDITION_GRAPH_PARTY:
    case POKENAV_CONDITION_GRAPH_SEARCH:
        mons = NavMonList();
        {
            s8 mark;
            s16 mx, my;

            /* The markings menu, open over the graph: a row, or out of it. */
            if (CtrPokenavCondition_Marking() && CtrMonMarkings_Menu(&mark, &mx, &my))
            {
                row = (y - my - 8) / 16;
                if (x >= mx && x < mx + 64 && y >= my + 8 && row < 6)
                    NavStart(NAV_MARK, row, A_BUTTON);
                else
                    NavPress(B_BUTTON);
                break;
            }
        }
        if (x < 88 && y < 40)
            NavPress(B_BUTTON);
        else if (screen == POKENAV_CONDITION_GRAPH_PARTY && x >= NAV_BALL_X && mons)
        {
            row = (y - NAV_BALL_TOP + NAV_BALL_STEP / 2 + NAV_BALL_STEP) / NAV_BALL_STEP - 1;
            if (row == PARTY_SIZE)
                NavStart(NAV_PARTY, mons->listCount - 1, A_BUTTON);   /* CANCEL */
            else if (row >= 0 && row < mons->listCount - 1)
                NavStart(NAV_PARTY, row, 0);
        }
        else if (x < 80 && y >= 56)
            NavPress(y < 100 ? DPAD_UP : DPAD_DOWN);   /* the picture: previous, next */
        else if (screen == POKENAV_CONDITION_GRAPH_SEARCH)
            NavPress(A_BUTTON);                       /* markings */
        break;
    case POKENAV_CONDITION_SEARCH_RESULTS:
    case POKENAV_MATCH_CALL:
    case POKENAV_RIBBONS_MON_LIST:
        if (screen == POKENAV_MATCH_CALL)
        {
            switch (CtrPokenavMatchCall_Input(&option, &options))
            {
            case 1:
                row = (y - NAV_OPTION_Y) / 16;
                if (x < NAV_OPTION_W && y >= NAV_OPTION_Y && row < options)
                    NavStart(NAV_OPTION, row, A_BUTTON);
                else
                    NavPress(B_BUTTON);
                return;
            case 2:
                NavPress(B_BUTTON);
                return;
            case 3:
                NavPress(A_BUTTON);
                return;
            }
        }
        if (x < 88 && y < 32)
        {
            NavPress(B_BUTTON);
            break;
        }
        if (!CtrPokenavList_View(&lx, &ly, &width, &top, &selected, &shown, &total) || x < lx || x >= lx + width)
            break;
        if (y < ly)
            NavPress(DPAD_LEFT);        /* a page up */
        else if (y >= ly + 16 * shown)
            NavPress(DPAD_RIGHT);       /* a page down */
        else if (top + (y - ly) / 16 < total)
            NavStart(NAV_LIST, top + (y - ly) / 16, A_BUTTON);
        break;
    case POKENAV_RIBBONS_SUMMARY_SCREEN:
        if (!CtrPokenavRibbons_Summary(&selected, &normal, &gift, &giftStart, &expanded))
            break;
        if (x >= NAV_RIBBON_X && x < NAV_RIBBON_X + 16 * NAV_RIBBONS_PER_ROW && y >= NAV_RIBBON_Y)
        {
            int pos = (y - NAV_RIBBON_Y) / 16 * NAV_RIBBONS_PER_ROW + (x - NAV_RIBBON_X) / 16;

            if (pos < normal || (pos >= giftStart && pos < giftStart + gift))
            {
                NavStart(NAV_RIBBON, pos, 0);
                break;
            }
        }
        if (expanded)
            NavPress(B_BUTTON);
        else if (x < 88 && y < 32)
            NavPress(B_BUTTON);
        else if (x < 80 && y >= 64)
            NavPress(y < 104 ? DPAD_UP : DPAD_DOWN);   /* the picture: previous, next */
        break;
    }
}

/* A drag across the PokéNav's picture: the next or previous page or mon. */
static void NavSwipe(int dy)
{
    bool8 ready;
    u32 screen = CtrPokenav_Screen(&ready);
    bool8 next = dy < 0;

    switch (screen)
    {
    case POKENAV_CONDITION_SEARCH_RESULTS:
    case POKENAV_MATCH_CALL:
    case POKENAV_RIBBONS_MON_LIST:
        NavPress(next ? DPAD_RIGHT : DPAD_LEFT);
        break;
    case POKENAV_CONDITION_GRAPH_PARTY:
    case POKENAV_CONDITION_GRAPH_SEARCH:
    case POKENAV_RIBBONS_SUMMARY_SCREEN:
        NavPress(next ? DPAD_DOWN : DPAD_UP);
        break;
    }
}


/* ------------------------------------------------------------------------ */
/* What a tap does                                                          */
/* ------------------------------------------------------------------------ */

static struct
{
    bool8 active, dragged;
    s16 startX, startY, lastX, lastY;
    u8 pressed;
    bool8 bag;   /* on the game's bag or Pokédex, which take it themselves */
} sTouch;

/* The save, done here as start_menu.c's SaveDoSaveCallback does it. */
static void DoSave(void)
{
    u8 status;

    SaveMapView();
    IncrementGameStat(GAME_STAT_SAVED_GAME);
    if (gDifferentSaveFile == TRUE)
    {
        status = TrySavingData(SAVE_OVERWRITE_DIFFERENT_FILE);
        gDifferentSaveFile = FALSE;
    }
    else
    {
        status = TrySavingData(SAVE_NORMAL);
    }
    StringExpandPlaceholders(sSaveMessage, status == SAVE_STATUS_OK ? gText_PlayerSavedGame : gText_SaveError);
    if (status == SAVE_STATUS_OK)
        PlaySE(SE_SAVE);
    sSaveStep = SAVE_DONE;
}

static void OpenSave(void)
{
    sSaveStep = SAVE_ASK;
    StringExpandPlaceholders(sSaveMessage, gText_ConfirmSave);
}

/* A column screen chosen while one of the game's is on show, for when the
 * area is the bottom screen's again. sScreen may still be SAVE from before
 * that screen opened: SAVE opens on its question all the same. */
/* A screen to go to once the summary is back in the party menu. */
static u8 sLeaveVia = SCR_COUNT, sLeaveWait;

static void LeaveForScreen(u8 screen)
{
    if (screen == SCR_SAVE)
        OpenSave();
    sScreen = screen;
}

/* Touches use the displayed snapshot and only act on enabled controls. */
static void MapExtrasTap(u8 id)
{
    if (id == HIT_MAP_INFO && sShown.encOn && sShown.screen == SCR_MAP)
    {
        sEncView = !sShown.encView;
        sEncPage = 0;
        sPickMapsec = MAPSEC_NONE;
        PlaySE(SE_SELECT);
    }
    else if (id == HIT_MAP_PAGE && sShown.encView && sShown.encPages > 1)
    {
        sEncPage = (sShown.encPage + 1) % sShown.encPages;
        PlaySE(SE_SELECT);
    }
    else if (id == HIT_CLOCK_NOTICE && sShown.clockNotice)
        sClockNoticeSerial = CtrClockStore_LoadSerial();
}

/*
 * The game's bag, party menu or Pokédex for BAG, POKéMON or POKéDEX, opened from the field as the
 * start menu opens it. Chosen while another screen was up - the Pokédex, the
 * bag, the party menu, the PokéNav - it opens once that one has closed and
 * the field is idle again (OpenAsked); until then the area is black.
 */
static bool8 OpenGameScreen(u8 screen)
{
    if (!FieldIdle() || !CtrStartMenu_Available())
        return FALSE;
    StartPlan(PLAN_START, screen == SCR_BAG ? START_BAG : screen == SCR_POKEMON ? START_POKEMON
                        : screen == SCR_POKENAV ? START_POKENAV : screen == SCR_CARD ? START_PLAYER : START_POKEDEX);
    BeginSession(FALSE);
    return TRUE;
}

/* The screens the game draws in the window, opened by OpenAsked. */
static bool8 AskedScreen(u8 screen)
{
    return screen == SCR_BAG || screen == SCR_POKEDEX || screen == SCR_POKEMON || screen == SCR_POKENAV
        || (screen == SCR_CARD && sClassicMenus);
}

static void OpenAsked(u8 mode)
{
    static u8 lastMode = MODE_OFF;
    static u16 waited;

    /* Closed by its own button or B: back to the map, not opened again. */
    if (mode != lastMode)
    {
        if ((lastMode == MODE_BAG_MENU && sScreen == SCR_BAG) || (lastMode == MODE_POKEDEX && sScreen == SCR_POKEDEX)
         || (lastMode == MODE_PARTY_MENU && sScreen == SCR_POKEMON) || (lastMode == MODE_POKENAV && sScreen == SCR_POKENAV)
         || (lastMode == MODE_CARD && sScreen == SCR_CARD && sClassicMenus))
            sScreen = SCR_MAP;
        lastMode = mode;
        waited = 0;
    }
    if (mode != MODE_FIELD || !AskedScreen(sScreen) || sSession.active || sPlan.kind != PLAN_NONE)
        return;
    if (!(EnabledScreens() & (1 << sScreen)))
        sScreen = SCR_MAP;
    else if (OpenGameScreen(sScreen))
        waited = 0;
    /* Two seconds without the field coming back idle: given up. */
    else if (++waited > 120)
        sScreen = SCR_MAP;
}

static void Activate(u8 id, u8 mode)
{
    if (id == HIT_NONE)
        return;
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: tap %02x (mode %u screen %u)", id, mode, sShown.screen);
    if (mode >= MODE_BATTLE_INFO)
    {
        /* The controller waiting for it takes it on its next frame. */
        if (mode != MODE_BATTLE_INFO)
            sBattleTap = id;
        return;
    }

    /* Y: SELECT in the field, the registered item. RUN: running by default. */
    if (id == HIT_COLUMN + COL_Y)
    {
        if (mode == MODE_FIELD && FieldIdle() && gSaveBlock1Ptr->registeredItem != ITEM_NONE)
            PressOnce(SELECT_BUTTON);
        return;
    }
    if (id == HIT_COLUMN + COL_RUN)
    {
        if (FlagGet(FLAG_SYS_B_DASH))
        {
            CtrSettings_SetRunAlways(!CtrSettings_RunAlways());
            PlaySE(SE_SELECT);
        }
        return;
    }
    if (id >= HIT_COLUMN && id < HIT_COLUMN + SCR_COUNT)
    {
        u8 screen = id - HIT_COLUMN;

        if (mode == MODE_POKENAV)
        {
            /* Its own button is its B; any other leaves it for that screen. */
            if (screen == SCR_POKENAV)
                NavPress(B_BUTTON);
            else
            {
                /* First: NavStart drops the screen of a leave under way. */
                NavStart(NAV_LEAVE, 0, 0);
                LeaveForScreen(screen);
            }
            return;
        }
        /* The game's card: its own button closes it (B on its front, A on
         * its back); any other closes it for that screen. */
        if (mode == MODE_CARD)
        {
            u8 side = CtrTrainerCard_Side();

            if (side != 2)
            {
                PressOnce(side == 0 ? B_BUTTON : A_BUTTON);
                if (screen != SCR_CARD)
                    LeaveForScreen(screen);
            }
            return;
        }
        /* The bag on show: its own button closes it, as B does; any other
         * closes it for that screen. */
        if (mode == MODE_BAG_MENU)
        {
            if (CtrBag_Close() && screen != SCR_BAG)
                LeaveForScreen(screen);
            return;
        }
        /* The Pokédex's is its B; any other leaves it for that screen, B
         * after B, as the PokéNav's do. */
        if (mode == MODE_POKEDEX)
        {
            if (screen == SCR_POKEDEX)
                CtrPokedex_Close(FALSE);
            else if (CtrPokedex_Close(TRUE))
                LeaveForScreen(screen);
            return;
        }
        /* The game's party menu on show: its own button is B, as the bag's
         * and the Pokédex's are; any other closes it for that screen. */
        /* The summary opened from the party menu: POKéMON is its B, back to
         * the menu; any other goes back and leaves the menu for its screen. */
        if (mode == MODE_SUMMARY)
        {
            CtrSummary_NativeTap(CTR_SUM_TAP_BACK, 0);
            if (screen != SCR_POKEMON)
            {
                sLeaveVia = screen;
                sLeaveWait = 0;
            }
            return;
        }
        if (mode == MODE_PARTY_MENU)
        {
            if (screen == SCR_POKEMON)
                CtrParty_Close(FALSE);
            else if (CtrParty_Close(TRUE))
                LeaveForScreen(screen);
            return;
        }
        if (mode != MODE_FIELD)
            return;
        /* The PokéNav takes the area when it opens; the top keeps the world
         * from the moment it is asked for, fade included. */
        if (screen == SCR_POKENAV)
        {
            if (FieldIdle())
            {
                StartPlan(PLAN_START, START_POKENAV);
                BeginSession(FALSE);
            }
            return;
        }
        /* The game's card with the classic menus, as the PokéNav. */
        if (screen == SCR_CARD && sClassicMenus)
        {
            if (FieldIdle())
            {
                StartPlan(PLAN_START, START_PLAYER);
                BeginSession(FALSE);
            }
            return;
        }
        /* The game's bag and Pokédex, as the PokéNav: they take the area
         * when they open. */
        if (screen == SCR_BAG || screen == SCR_POKEDEX || screen == SCR_POKEMON)
        {
            OpenGameScreen(screen);
            return;
        }
        if (screen == SCR_SAVE && sScreen != SCR_SAVE)
            OpenSave();
        if (screen == SCR_CARD && sScreen != SCR_CARD)
            sCardBack = FALSE;
        if (screen != sScreen)
            PlaySE(SE_SELECT);
        sScreen = screen;
        sPickMapsec = MAPSEC_NONE;
        return;
    }
    /* The title: the wild list's toggle, on the map. */
    if (id == HIT_MAP_INFO)
    {
        MapExtrasTap(id);
        return;
    }

    /* The game's screen drawn here: the tap its own code takes. */
    if (sShown.native)
    {
        GameViewTap(&sShown, id);
        return;
    }
    switch (sShown.screen)
    {
    case SCR_MAP:
        if (id == HIT_MAP)
            PickMapCell(sTouch.lastX, sTouch.lastY);
        else
            MapExtrasTap(id);
        break;
    case SCR_CARD:
        if (id == HIT_OK)
        {
            sCardBack = !sCardBack;
            PlaySE(SE_SELECT);
        }
        break;
    case SCR_NUZLOCKE:
        NuzlockeTap(id);
        break;
    case SCR_SAVE:
        if (id == HIT_YES && FieldIdle())
        {
            if (sSaveStep == SAVE_ASK && gSaveFileStatus != SAVE_STATUS_EMPTY && gSaveFileStatus != SAVE_STATUS_CORRUPT)
            {
                sSaveStep = SAVE_OVERWRITE;
                StringExpandPlaceholders(sSaveMessage, gDifferentSaveFile ? gText_DifferentSaveFile
                                                                         : gText_AlreadySavedFile);
            }
            else
                DoSave();
        }
        else if (id == HIT_NO || id == HIT_OK)
        {
            OpenSave();
            sScreen = SCR_MAP;
        }
        break;
    case SCR_OPTION:
        ActivateOption(id);
        break;
    }
}

#ifdef CTR_TEST_SWOOP
/* Azahar aid (3ds_game_full.c): the OPTIONS screen opened on the bottom, on
 * WORLD > 3D, then VOXEL 3D tapped there, as the player does it (bottom
 * redraws between). */
void CtrBottom_TestSwoop(unsigned step)
{
    if (step == 0)
    {
        sScreen = SCR_OPTION;
        sOptPage = CAT_WORLD;
        sOptSub = 0;
    }
    else
        Activate(HIT_OPTION, MODE_FIELD); /* the group's first row */
}
#endif

/* Whether a point is on the game's picture in its window (or, with the whole
 * screen, in the middle of it: the same place). */
static bool8 InGameWindow(int x, int y)
{
    return x >= CTR_BOTTOM_WIN_X && x < CTR_BOTTOM_WIN_X + 240 && y >= CTR_BOTTOM_WIN_Y
        && y < CTR_BOTTOM_WIN_Y + 160;
}

/* Returns the id to show as pressed. */
static u8 ProcessTouch(u8 mode)
{
    const CtrInput *in = CtrInput_Get();

    if (mode != sShown.mode)
    {
        /* A touch that began on another screen does not act on this one. */
        if (sTouch.bag)
        {
            CtrBag_Touch(BAG_TOUCH_CANCEL, 0, 0);
            CtrPokedex_Touch(BAG_TOUCH_CANCEL, 0, 0);
            CtrShop_Touch(BAG_TOUCH_CANCEL, 0, 0);
        }
        sTouch.active = FALSE;
        sTouch.bag = FALSE;
        return HIT_NONE;
    }
    /* The shop's BUY screen: its picture in the middle of the whole screen,
     * the touch in pixels of it, as it goes (drags scroll its list). */
    if ((in->touchDown && ShopShown(mode)) || (sTouch.bag && sTouch.active && ShopShown(mode)))
    {
        int ox = CTR_BOTTOM_WIN_X, oy = CTR_BOTTOM_WIN_Y;

        if (in->touchDown)
        {
            sTouch.active = sTouch.bag = TRUE;
            CtrShop_Touch(BAG_TOUCH_DOWN, in->touchX - ox, in->touchY - oy);
        }
        else if (in->touchActive)
            CtrShop_Touch(BAG_TOUCH_MOVE, in->touchX - ox, in->touchY - oy);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrShop_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    /* The bag drawn here: a touch on its list's rows goes to the bag's own
     * list touch, at the same row of its list, as it goes. */
    if ((in->touchDown && BagShown(mode) && sShown.native == NATIVE_BAG
         && BagListTouch(&sShown, BAG_TOUCH_DOWN, in->touchX, in->touchY, TRUE))
     || (sTouch.bag && sTouch.active && BagShown(mode) && sShown.native == NATIVE_BAG))
    {
        if (in->touchDown)
            sTouch.active = sTouch.bag = TRUE;
        else if (in->touchActive)
            BagListTouch(&sShown, BAG_TOUCH_MOVE, in->touchX, in->touchY, FALSE);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrBag_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    /* The game's bag: its picture in its window, the touch in pixels of it,
     * as it goes. With the whole screen, anywhere. */
    if ((in->touchDown && BagShown(mode) && !sShown.native
         && (sShown.bagView == BAG_VIEW_WHOLE || InGameWindow(in->touchX, in->touchY)))
     || (sTouch.bag && sTouch.active && BagShown(mode)))
    {
        int ox = CTR_BOTTOM_WIN_X, oy = CTR_BOTTOM_WIN_Y;

        if (in->touchDown)
        {
            sTouch.active = sTouch.bag = TRUE;
            CtrBag_Touch(BAG_TOUCH_DOWN, in->touchX - ox, in->touchY - oy);
        }
        else if (in->touchActive)
            CtrBag_Touch(BAG_TOUCH_MOVE, in->touchX - ox, in->touchY - oy);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrBag_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    /* The Pokédex drawn here: a touch on its list or over an entry goes to
     * its own touch, at the same place of its picture, as it goes. */
    if ((in->touchDown && DexShown(mode) && sShown.native == NATIVE_DEX
         && DexTouch(&sShown, BAG_TOUCH_DOWN, in->touchX, in->touchY, TRUE))
     || (sTouch.bag && sTouch.active && DexShown(mode) && sShown.native == NATIVE_DEX))
    {
        if (in->touchDown)
            sTouch.active = sTouch.bag = TRUE;
        else if (in->touchActive)
            DexTouch(&sShown, BAG_TOUCH_MOVE, in->touchX, in->touchY, FALSE);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            DexTouch(&sShown, BAG_TOUCH_UP, 0, 0, FALSE);
        }
        return HIT_NONE;
    }
    /* The game's Pokédex, the same way. */
    if ((in->touchDown && DexShown(mode) && !sShown.native && InGameWindow(in->touchX, in->touchY))
     || (sTouch.bag && sTouch.active && DexShown(mode)))
    {
        int ox = CTR_BOTTOM_WIN_X, oy = CTR_BOTTOM_WIN_Y;

        if (in->touchDown)
        {
            sTouch.active = sTouch.bag = TRUE;
            CtrPokedex_Touch(BAG_TOUCH_DOWN, in->touchX - ox, in->touchY - oy);
        }
        else if (in->touchActive)
            CtrPokedex_Touch(BAG_TOUCH_MOVE, in->touchX - ox, in->touchY - oy);
        else
        {
            sTouch.active = sTouch.bag = FALSE;
            CtrPokedex_Touch(BAG_TOUCH_UP, 0, 0);
        }
        return HIT_NONE;
    }
    if (in->touchDown)
    {
        sTouch.active = TRUE;
        sTouch.dragged = FALSE;
        sTouch.startX = sTouch.lastX = in->touchX;
        sTouch.startY = sTouch.lastY = in->touchY;
        sTouch.pressed = HitTest(in->touchX, in->touchY);
    }
    else if (in->touchActive && sTouch.active)
    {
        int dy = in->touchY - sTouch.startY, dx = in->touchX - sTouch.startX;

        sTouch.lastX = in->touchX;
        sTouch.lastY = in->touchY;
        if (!sTouch.dragged && (dy > 8 || dy < -8 || dx > 8 || dx < -8))
            sTouch.dragged = TRUE;
    }
    else if (in->touchUp && sTouch.active)
    {
        sTouch.active = FALSE;
        /* The game's party menu, its picture in its window: the tap goes to
         * whatever waits for input in it (a mon, the buttons, its menu, a
         * question, a message), in pixels of that picture. The header's and
         * the dock's buttons are ours. */
        if (mode == MODE_PARTY_MENU && !sShown.native
         && (sShown.bagView == BAG_VIEW_WHOLE || InGameWindow(sTouch.startX, sTouch.startY)))
        {
            int ox = CTR_BOTTOM_WIN_X, oy = CTR_BOTTOM_WIN_Y;

            if (!sTouch.dragged)
                CtrMenu_PostTap(sTouch.lastX - ox, sTouch.lastY - oy);
            return HIT_NONE;
        }
        /* The boxes have the whole screen, their picture in the middle of it,
         * and take the tap themselves, in pixels of that picture. */
        if (mode == MODE_STORAGE)
        {
            int x = sTouch.lastX - CTR_BOTTOM_WIN_X, y = sTouch.lastY - CTR_BOTTOM_WIN_Y;

            /* Or a summary opened from them, the same way. So does the
             * trade menu. */
            if (!sTouch.dragged && CtrStorage_IsOpen())
                CtrStorage_Tap(x, y);
            else if (!sTouch.dragged && CtrTrade_IsOpen())
                CtrMenu_PostTap(x, y);
            else if (!sTouch.dragged)
                CtrSummary_Tap(x, y);
            return HIT_NONE;
        }
        if (mode == MODE_CARD && InGameWindow(sTouch.startX, sTouch.startY))
        {
            u8 side = CtrTrainerCard_Side();

            if (!sTouch.dragged && side != 2)
                PressOnce(side == 0 ? A_BUTTON : B_BUTTON);
            return HIT_NONE;
        }
        if (mode == MODE_POKENAV && InGameWindow(sTouch.startX, sTouch.startY))
        {
            if (!sTouch.dragged)
                NavTap(sTouch.lastX - CTR_BOTTOM_WIN_X, CtrVideo_BottomPictureY(sTouch.lastY));
            else if (sTouch.lastY - sTouch.startY > 24 || sTouch.lastY - sTouch.startY < -24)
                NavSwipe(sTouch.lastY - sTouch.startY);
            return HIT_NONE;
        }
        /* While the turn plays out the screen is the backdrop: a tap on it is
         * A, on with the text. */
        if (mode == MODE_BATTLE_INFO)
        {
            if (!sTouch.dragged)
                PressOnce(A_BUTTON);
            return HIT_NONE;
        }
        if ((!sTouch.dragged || sTouch.pressed == HIT_MAP) && HitTest(sTouch.lastX, sTouch.lastY) == sTouch.pressed)
            Activate(sTouch.pressed, mode);
        return HIT_NONE;
    }
    if (!sTouch.active || sTouch.dragged)
        return HIT_NONE;
    return sTouch.pressed;
}

/* ------------------------------------------------------------------------ */
/* The dock and the header by the buttons (X)                              */
/* ------------------------------------------------------------------------ */

/*
 * X gives the dock a focus: LEFT and RIGHT walk its buttons, UP goes to the
 * header (Y, the title while it is the wild list's toggle, RUN) and DOWN
 * back; A does what a tap would, B or X again gives the buttons back to the
 * game. While the dock has them the game sees none, so the player stands
 * still. Opened with A, the options take the focus into their panel (up and
 * down through the rows, left and right or A to change one, L and R through
 * the groups, B back to the dock) and the save screen takes A for YES and B
 * for NO; the game's own screens (party, bag, Pokédex, PokéNav) take the
 * buttons themselves, and X inside them brings the focus back to the dock
 * to go somewhere else.
 */
uint16_t CtrBottom_FilterKeys(uint16_t held)
{
    return sFocus != FOCUS_NONE || sInside != INSIDE_NONE || sSwallow ? 0 : held;
}

/* Whether the dock is on screen: not in battle, not under the boxes or the
 * bag or party menu that a battle, a shop or the PC opens. */
static bool8 ColumnShown(u8 mode)
{
    if (gMain.inBattle)
        return FALSE;
    switch (mode)
    {
    case MODE_FIELD:
    case MODE_POKENAV:
    case MODE_POKEDEX:
    case MODE_SUMMARY:
    case MODE_CARD:
        return TRUE;
    case MODE_PARTY_MENU:
        return gPartyMenu.menuType == PARTY_MENU_TYPE_FIELD;
    case MODE_BAG_MENU:
        return CtrBag_InWindow();
    }
    return FALSE;
}

static bool8 ColumnItemAvailable(u8 item)
{
    if (item < SCR_COUNT)
        return (EnabledScreens() >> item) & 1;
    if (item == COL_Y)
        return gSaveBlock1Ptr->registeredItem != ITEM_NONE;
    if (item == COL_TITLE)
        return TitleToggles(&sShown);
    return FlagGet(FLAG_SYS_B_DASH);
}

/* The next available button left (dir < 0) or right on the focus's row - the
 * dock's, or the header's - or `from`. */
static u8 ColumnStep(u8 from, int dir)
{
    static const u8 header[] = {COL_Y, COL_TITLE, COL_RUN};

    if (from >= SCR_COUNT)
    {
        int i = 0;

        while (i < (int)ARRAY_COUNT(header) && header[i] != from)
            ++i;
        for (i += dir; i >= 0 && i < (int)ARRAY_COUNT(header); i += dir)
            if (ColumnItemAvailable(header[i]))
                return header[i];
        return from;
    }
    for (int i = from + dir; i >= 0 && i < SCR_COUNT; i += dir)
        if (ColumnItemAvailable(i))
            return i;
    return from;
}

/* UP from the dock: the header's middle (the title), or what of it there is. */
static u8 HeaderFocus(void)
{
    if (ColumnItemAvailable(COL_TITLE))
        return COL_TITLE;
    if (ColumnItemAvailable(COL_Y))
        return COL_Y;
    if (ColumnItemAvailable(COL_RUN))
        return COL_RUN;
    return FOCUS_NONE;
}

static void MoveFocus(u8 *focus, u8 to)
{
    if (to != *focus)
    {
        *focus = to;
        PlaySE(SE_SELECT);
    }
}

/* Gives the buttons back to the game once they are let go. */
static void LeaveFocus(void)
{
    sFocus = FOCUS_NONE;
    sInside = INSIDE_NONE;
    sSwallow = TRUE;
}

/* A on a dock or header button: what a tap on it does, and where the focus
 * goes. */
static void ChooseColumnItem(u8 mode, u8 item)
{
    bool8 gameScreen = item == SCR_POKEMON || item == SCR_BAG || item == SCR_POKEDEX || item == SCR_POKENAV
                    || (item == SCR_CARD && sClassicMenus);

    if (item == COL_RUN)
    {
        Activate(HIT_COLUMN + COL_RUN, mode);
        return;
    }
    if (item == COL_TITLE)
    {
        Activate(HIT_MAP_INFO, mode);
        return;
    }
    if (item == COL_Y)
    {
        LeaveFocus();
        Activate(HIT_COLUMN + COL_Y, mode);
        return;
    }
    /* The game's screen on show already: back to it. */
    if (gameScreen && item == sShown.screen && mode != MODE_FIELD)
    {
        LeaveFocus();
        return;
    }
    Activate(HIT_COLUMN + item, mode);
    if (gameScreen)
        LeaveFocus();
    else if (item == SCR_OPTION || item == SCR_SAVE)
    {
        sFocus = FOCUS_NONE;
        sInside = item == SCR_OPTION ? INSIDE_OPTIONS : INSIDE_SAVE;
        if (item == SCR_OPTION)
            sOptFocus = OptionStep(0xFF, 1) == 0xFF ? 0 : OptionStep(0xFF, 1);
    }
}

static void SaveKeys(u8 mode, u16 down)
{
    if (down & A_BUTTON)
        Activate(sSaveStep == SAVE_DONE ? HIT_OK : HIT_YES, mode);
    else if (down & B_BUTTON)
        Activate(sSaveStep == SAVE_DONE ? HIT_OK : HIT_NO, mode);
    /* Done or declined, the screen goes back to the map: so does the focus,
     * to the dock. */
    if (sScreen != SCR_SAVE)
    {
        sInside = INSIDE_NONE;
        sFocus = sScreen;
    }
}

static void ProcessKeys(u8 mode)
{
    const CtrInput *in = CtrInput_Get();
    u16 down = in->down;
    bool8 x = (in->physicalDown & CTR_KEY_X) != 0;

    if (sSwallow && !(in->held & CTR_KEY_GAME) && !(in->physicalHeld & (CTR_KEY_X | CTR_KEY_Y)))
        sSwallow = FALSE;
    if (!ColumnShown(mode))
    {
        if (sFocus != FOCUS_NONE || sInside != INSIDE_NONE)
            LeaveFocus();
        return;
    }
    /* A tap took the screen elsewhere. */
    if ((sInside == INSIDE_OPTIONS && sScreen != SCR_OPTION) || (sInside == INSIDE_SAVE && sScreen != SCR_SAVE))
    {
        sInside = INSIDE_NONE;
        sFocus = sScreen;
    }
    if (sInside != INSIDE_NONE)
    {
        if (x)
        {
            sInside = INSIDE_NONE;
            sFocus = sScreen;
            PlaySE(SE_SELECT);
        }
        else if (sInside == INSIDE_OPTIONS)
            OptionKeys(down);
        else
            SaveKeys(mode, down);
        return;
    }
    if (sFocus == FOCUS_NONE)
    {
        /* Not in the middle of a script or a field effect. */
        if (!x || (mode == MODE_FIELD && (gMain.callback2 != CB2_Overworld || ArePlayerFieldControlsLocked()
                                          || ScriptContext_IsEnabled())))
            return;
        sFocus = sShown.screen < SCR_COUNT && ColumnItemAvailable(sShown.screen) ? sShown.screen : SCR_MAP;
        PlaySE(SE_SELECT);
        return;
    }
    if (!ColumnItemAvailable(sFocus))
        sFocus = sFocus >= SCR_COUNT && HeaderFocus() != FOCUS_NONE ? HeaderFocus() : SCR_MAP;
    if (x || (down & B_BUTTON))
    {
        LeaveFocus();
        PlaySE(SE_SELECT);
    }
    else if (down & DPAD_LEFT)
        MoveFocus(&sFocus, ColumnStep(sFocus, -1));
    else if (down & DPAD_RIGHT)
        MoveFocus(&sFocus, ColumnStep(sFocus, 1));
    else if ((down & DPAD_UP) && sFocus < SCR_COUNT && HeaderFocus() != FOCUS_NONE)
        MoveFocus(&sFocus, HeaderFocus());
    else if ((down & DPAD_DOWN) && sFocus >= SCR_COUNT)
        MoveFocus(&sFocus, sShown.screen < SCR_COUNT && ColumnItemAvailable(sShown.screen) ? sShown.screen : SCR_MAP);
    else if (down & A_BUTTON)
        ChooseColumnItem(mode, sFocus);
}

/* ------------------------------------------------------------------------ */
/* Entry points                                                             */
/* ------------------------------------------------------------------------ */

/* field_player_avatar.c: B walks instead of running while RUN is on. */
bool8 CtrPlayer_RunAlways(void)
{
    return CtrSettings_RunAlways();
}

void CtrBottom_Init(void)
{
    uint64_t start = CtrPlatform_Ticks();

    LoadResources();
    if (sRes.ready)
        BuildBackgroundCaches();
    sShown.mode = 0xFF;
    CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: resources %s in %.1f ms", sRes.ready ? "ready" : "MISSING",
                 CtrPlatform_TickMs(CtrPlatform_Ticks() - start));
}

/* The PC's boxes, the shop's BUY screen or the bag to sell from are about to
 * open: the top keeps the world, fade included, until the field is back. */
void CtrBottom_KeepWorld(void)
{
    /* Only CtrBottom_Frame lets the top go again, and without the bottom
     * screen's resources it never runs: the world would stay frozen. */
    if (!sRes.ready)
        return;
    BeginSession(FALSE);
    sSession.awaitMenu = TRUE;
    CtrVideo_HoldTop(TRUE);
}

/*
 * Walking moves the player's mark on the MAP screen, and nothing else on it:
 * every move used to redraw the whole screen - 5-15 ms on an Old 3DS, in the
 * frame the move fell in. Now the mark's old square is restored from the
 * map's cache, the mark drawn on its new one, and those two squares sent to
 * the screen.
 */
static bool8 MapCursorOnlyMoved(const ViewState *now, const ViewState *shown)
{
    ViewState moved;

    if (now->screen != SCR_MAP || now->mode != MODE_FIELD || !now->dock || !shown->dock
     || now->mapsec == MAPSEC_NONE || shown->mapsec == MAPSEC_NONE
     /* The wild list has no marks to move; the clock banner lies over them. */
     || now->encView || shown->encView || now->clockNotice || shown->clockNotice
     || (now->cursorX == shown->cursorX && now->cursorY == shown->cursorY
         && now->mapsec == shown->mapsec))
        return FALSE;
    moved = *now;
    moved.cursorX = shown->cursorX;
    moved.cursorY = shown->cursorY;
    moved.mapsec = shown->mapsec;
    /* A step that moves the mark also takes one off a Repel: MoveMapCursor
     * redraws its plate for it. */
    moved.repelSteps = shown->repelSteps;
    return memcmp(&moved, shown, sizeof(moved)) == 0;
}

static bool8 RectsMeet(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1)
{
    return ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1;
}

/* False, with nothing touched, where a whole redraw is needed after all. */
static bool8 MoveMapCursor(const ViewState *from, const ViewState *to)
{
    const u8 *icon = sRes.playerIcon[to->gender];
    int r[2][4];

    if (sCache[CACHE_MAP] == NULL || icon == NULL || CtrVideo_BottomInUse() || CtrVideo_BottomWhole())
        return FALSE;
    MarkRect(from->cursorX, from->cursorY, &r[0][0], &r[0][1], &r[0][2], &r[0][3]);
    MarkRect(to->cursorX, to->cursorY, &r[1][0], &r[1][1], &r[1][2], &r[1][3]);
    /* The old square as the map cache has it (the canvas is its layout). */
    for (int x = r[0][0]; x < r[0][2]; ++x)
        memcpy(sCanvas + x * H + (H - r[0][3]), sCache[CACHE_MAP] + x * H + (H - r[0][3]),
               (size_t)(r[0][3] - r[0][1]) * sizeof(u16));
    sDst = sCanvas;
    sOX = 0;
    /* The mark, and a picked cell's cursor over it, as Render draws them. */
    DrawMapMarks(to);
    for (int k = 0; k < 2; ++k)
        CtrBottom_BlitRect(sCanvas, r[k][0], r[k][1], r[k][2], r[k][3]);
    /* Crossing a region changes only the title; a step, the Repel's steps:
     * those plates alone. */
    if (to->mapsec != from->mapsec && to->pickMapsec == MAPSEC_NONE)
    {
        int x0, y0, x1, y1;

        TitleRect(to, &x0, &y0, &x1, &y1);
        RenderPart(to, x0, y0, x1, y1);
    }
    if (to->repelSteps != from->repelSteps)
    {
        int x0, y0, x1, y1;

        RepelRect(&x0, &y0, &x1, &y1);
        RenderPart(to, x0, y0, x1, y1);
    }
    return TRUE;
}

/*
 * A part redrawn builds the labels it lacks within the time a whole redraw
 * has (sRedrawLabelMs); one left flat has that part drawn again next frame,
 * not the whole screen. Built in one go, a new quick ball's name and count
 * were a spike; left to the whole redraw's refining, each new ball was ~15
 * whole redraws (4-6 ms each in Azahar, a drop on an Old 3DS).
 */
static bool8 sRefinePart;
static Rect sRefineRect;

static void TimedRenderPart(const ViewState *s, Rect r)
{
    if (sRefinePart)
        RectAdd(&r, sRefineRect.x0, sRefineRect.y0, sRefineRect.x1, sRefineRect.y1);
    sRefinePart = FALSE;
    sRedrawStart = CtrPlatform_Ticks();
    sRedrawLabelMs = REDRAW_LABEL_MS;
    sRefineLabels = FALSE;
    sRedrawLabelWorked = FALSE;
    RenderPart(s, r.x0, r.y0, r.x1, r.y1);
    sRedrawLabelMs = 0.0f;
    if (sRefineLabels)
    {
        sRefinePart = TRUE;
        sRefineRect = r;
    }
}

static bool8 PartialRedraw(const ViewState *now)
{
    Rect r;

    if (DirtyRect(now, &sShown, &r))
    {
        TimedRenderPart(now, r);
        return TRUE;
    }
    return FALSE;
}

/*
 * Entering a battle: the bottom screen fades out what the field had, stays
 * dark a moment (the top screen's own intro is going on, and the battle
 * menus are warmed up), then the backdrop is drawn once, off screen, and
 * fades in. A fade step is the canvas copied to the screen at 3/4, 1/2, 1/4
 * of its brightness (CtrBottom_BlitDim: two bit operations per two pixels),
 * so it costs about what a redraw's copy does, and nothing is drawn.
 */
#define BATTLE_INTRO_OUT 8      /* frames, a level every two */
#define BATTLE_INTRO_HOLD 12
#define BATTLE_INTRO_IN 8

enum { INTRO_NONE, INTRO_OUT, INTRO_HOLD, INTRO_RENDER, INTRO_IN };

/* TRUE while the intro owns the screen: the rest of the frame is skipped. */
static bool8 BattleIntro(u8 mode)
{
    static u8 prev = MODE_OFF;
    bool8 enter = mode == MODE_BATTLE_INFO && prev == MODE_FIELD;

    prev = mode;
    if (enter && sIntro.phase == INTRO_NONE)
    {
        MapCacheTake();
        sIntro.phase = INTRO_OUT;
        sIntro.step = 0;
    }
    if (mode < MODE_BATTLE_INFO && sIntro.phase != INTRO_NONE)
    {
        /* Left the battle meanwhile: the field draws itself again. */
        sIntro.phase = INTRO_NONE;
        sIntro.defer = FALSE;
        sForceRedraw = TRUE;
        return FALSE;
    }
    switch (sIntro.phase)
    {
    case INTRO_OUT:
        if (!(sIntro.step & 1))
            CtrBottom_BlitDim(sCanvas, 3 - sIntro.step / 2);
        if (++sIntro.step >= BATTLE_INTRO_OUT)
        {
            sIntro.phase = INTRO_HOLD;
            sIntro.step = 0;
        }
        return TRUE;
    case INTRO_HOLD:
        WarmBattleMenus();
        if (++sIntro.step >= BATTLE_INTRO_HOLD)
        {
            sIntro.phase = INTRO_RENDER;
            sIntro.step = 0;
            sIntro.defer = TRUE;
            sForceRedraw = TRUE;
            return FALSE;
        }
        return TRUE;
    case INTRO_RENDER:
        /* The full redraw below ends it; a frame that did not draw it
         * (the menu moved on) must not keep the screen dark. */
        if (++sIntro.step > 3)
        {
            sIntro.phase = INTRO_NONE;
            sIntro.defer = FALSE;
            sForceRedraw = TRUE;
        }
        return FALSE;
    case INTRO_IN:
        if (!(sIntro.step & 1))
            CtrBottom_BlitDim(sCanvas, 1 + sIntro.step / 2);
        if (++sIntro.step >= BATTLE_INTRO_IN)
            sIntro.phase = INTRO_NONE;
        return TRUE;
    }
    return FALSE;
}

static void BottomProfile(u32 frame, u8 mode, const uint64_t ticks[3], unsigned kind)
{
    static u32 last;
    uint64_t end = CtrPlatform_Ticks();
    float total = CtrPlatform_TickMs(end - ticks[0]);
    if (total < 2.0f || (last && frame - last < 120)) return;
    last = frame;
    CtrLog_Write(CTR_LOG_VIDEO,
                 "bottom slice frame=%lu mode=%u kind=%u total=%.2f control=%.2f snapshot=%.2f draw=%.2f ms "
                 "labels hit=%lu missed=%lu",
                 (unsigned long)frame, mode, kind, total, CtrPlatform_TickMs(ticks[1] - ticks[0]),
                 CtrPlatform_TickMs(ticks[2] - ticks[1]), CtrPlatform_TickMs(end - ticks[2]),
                 (unsigned long)sLabelHits, (unsigned long)sLabelMisses);
    sLabelHits = sLabelMisses = 0;
}

/*
 * The game's soft reset (CtrGame_SoftReset) puts back the game's data, not
 * the port's: what the bottom screen knows of the game it was in goes too.
 * Until the field is reached again it is the title's (MODE_OFF), not a field
 * screen whose OPTIONS and CHEATS would write into the save being loaded.
 */
void CtrBottom_SoftReset(void)
{
    sInGame = FALSE;
    memset(&sSession, 0, sizeof(sSession));
    memset(&sPlan, 0, sizeof(sPlan));
    sInjected = 0;
    memset(&sNav, 0, sizeof(sNav));
    memset(&sTouch, 0, sizeof(sTouch));
    sFocus = FOCUS_NONE;
    sInside = INSIDE_NONE;
    sSwallow = FALSE;
    sScreen = SCR_MAP;
    sOptPage = CAT_GENERAL;
    sOptSub = 0;
    sOptFocus = 0;
    sCardBack = FALSE;
    sPickMapsec = MAPSEC_NONE;
    sEncView = FALSE;
    sEncPage = 0;
    sSaveStep = SAVE_ASK;
    sAsk.kind = sAsked.kind = ASK_NONE;
    sBattleTap = HIT_NONE;
    sMoveCancel = sQuickBallTap = sQuickBallFocus = FALSE;
    sIntro.phase = INTRO_NONE;
    sIntro.defer = FALSE;
    sMapCache.valid = FALSE;
    /* A session's hold, or one CtrBottom_KeepWorld took this very frame. */
    CtrVideo_HoldTop(FALSE);
    sShown.mode = 0xFF;
    sForceRedraw = TRUE;
}

void CtrBottom_Frame(void)
{
    static u32 frames;
    static bool8 iconsPending, held;
    u8 mode, pressed;
    bool8 hold;
    uint64_t ticks[3];

    if (!sRes.ready)
        return;
    ++frames;
    /* Last frame's press is over, also on the frames the battle's entrance
     * skips RunPlan and RunNav: a press left in would stay held through it. */
    sInjected = 0;
    ticks[0] = CtrPlatform_Ticks();
    sAsked = sAsk;
    sAsk.kind = ASK_NONE;
    if (sAsked.kind == ASK_NONE)
        sBattleTap = HIT_NONE;

    sFrames = frames;
    sClassicMenus = CtrExtras_Get("classic_menus") == 1;
    mode = CurrentMode();
    /* Away from the action menu its labels may give way: looked at again. */
    if (mode != MODE_BATTLE_ACTION)
        sQuickWarmBall = ITEM_NONE;
    if (mode < MODE_BATTLE_INFO && sCompAny)
    {
        /* The plate pictures are only for a battle. */
        FreeComps();
        sCompAny = FALSE;
        sWarmKey = sWarmIdxKey = 0xFFFFFFFF;
    }
    if (BattleIntro(mode))
        return;
    /* Left the summary for another screen: the party menu after it too. */
    if (sLeaveVia < SCR_COUNT && mode != MODE_SUMMARY)
    {
        if (mode == MODE_PARTY_MENU && CtrParty_Close(TRUE))
        {
            LeaveForScreen(sLeaveVia);
            sLeaveVia = SCR_COUNT;
        }
        else if (mode != MODE_PARTY_MENU && ++sLeaveWait > 60)
            sLeaveVia = SCR_COUNT;
    }
    pressed = ProcessTouch(mode);
    ProcessKeys(mode);
    OpenAsked(mode);
    RunPlan();
    RunNav(mode);

    /* The hidden menus: the top screen keeps the world meanwhile. */
    hold = UpdateSession(mode, sPlan.kind != PLAN_NONE);
    if (hold != held)
    {
        CtrVideo_HoldTop(hold);
        held = hold;
    }
    if (hold && mode != MODE_POKENAV && mode != MODE_STORAGE && mode != MODE_PARTY_MENU && mode != MODE_SUMMARY
     && mode != MODE_CARD
     && !BagShown(mode)
     && !DexShown(mode))   /* on show */
        FastForward();

    /* The game's last picture stays in its window until repainted. */
    {
        static bool navDrawn;
        bool drawn = CtrVideo_BottomInUse();

        if (drawn != navDrawn)
            sForceRedraw = TRUE;
        navDrawn = drawn;
    }
    ticks[1] = CtrPlatform_Ticks();
    Snapshot(&sState, mode, pressed);
    CtrVideo_SetBottomNative(sState.native != NATIVE_NONE);
    CtrVideo_SetNativeMenus(!sClassicMenus);

    /* One RomFS read at most, and a single redraw once the icons are in. */
    sIconsReady = FALSE;
    sIconsIn = FALSE;
    sShowCaptures = 1;
    if (Prefetch(&sState))
        iconsPending = TRUE;
    else if (iconsPending)
    {
        iconsPending = FALSE;
        sForceRedraw = TRUE;
        sIconsIn = TRUE;
    }
    else
        sIconsReady = sIconsIn = TRUE;

    ticks[2] = CtrPlatform_Ticks();
    if (!sForceRedraw && MapCursorOnlyMoved(&sState, &sShown) && MoveMapCursor(&sShown, &sState))
    {
        sShown = sState;
        BottomProfile(frames, mode, ticks, 1);
        return;
    }
    /* A press, the focus, a cursor, the clock, an option's value: only the
     * part that changes is drawn. */
    if (!sForceRedraw && !CtrVideo_BottomWhole() && sShown.mode != 0xFF
     && memcmp(&sState, &sShown, sizeof(sState)) != 0 && PartialRedraw(&sState))
    {
        sShown = sState;
        BottomProfile(frames, mode, ticks, 3);
        return;
    }
    /* A part left with a flat label: that part again, the label further on. */
    if (sRefinePart && !sForceRedraw && !CtrVideo_BottomWhole()
     && memcmp(&sState, &sShown, sizeof(sState)) == 0)
    {
        Rect r;

        RectInit(&r);
        TimedRenderPart(&sShown, r);
        BottomProfile(frames, mode, ticks, 3);
        return;
    }
    if (sForceRedraw || memcmp(&sState, &sShown, sizeof(sState)) != 0)
    {
        uint64_t start = CtrPlatform_Ticks();
        static float peak;
        float ms;

        if (sState.mode != sShown.mode)
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: mode %u", sState.mode);
        sShown = sState;
        sForceRedraw = FALSE;
        sRefinePart = FALSE;
        sRedrawStart = start;
        sRedrawLabelMs = REDRAW_LABEL_MS;
        sRefineLabels = FALSE;
        sRedrawLabelWorked = FALSE;
        Render(&sShown);
        sRedrawLabelMs = 0.0f;
        /* A label left flat: drawn again next frame, built a bit further. */
        if (sRefineLabels)
            sForceRedraw = TRUE;
        if (sIntro.defer)
        {
            sIntro.defer = FALSE;
            sIntro.phase = INTRO_IN;
            sIntro.step = 0;
        }
        ms = CtrPlatform_TickMs(CtrPlatform_Ticks() - start);
        if (ms > peak + 0.25f)
        {
            peak = ms;
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: redraw peak %.2f ms (mode %u screen %u)", ms, sShown.mode,
                         sShown.screen);
        }
        BottomProfile(frames, mode, ticks, 2);
        return;
    }
    if (mode == MODE_BATTLE_INFO)
        WarmBattleMenus();
    else if (mode == MODE_BATTLE_ACTION)
        WarmQuickBalls(&sShown);
    BottomProfile(frames, mode, ticks, 0);
}

