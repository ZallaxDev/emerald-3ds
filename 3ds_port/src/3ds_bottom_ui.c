/*
 * The bottom screen, game side: what it shows and what a touch does.
 *
 * It replaces the START menu. A column of buttons on the right holds every
 * entry the menu had, plus the Hoenn map: MAP (the default), POKéMON, BAG,
 * the trainer card, POKéDEX, POKéNAV, SAVE and OPTION. The 240x240 area on
 * the left shows the chosen one; everything happens here while the top screen
 * keeps the world. The PokéNav is the game's own, run as it is: the
 * compositor draws its screens into that area (CtrVideo_BottomInUse) and a
 * tap on them becomes the buttons the PokéNav reads. The PC's boxes are
 * drawn there too, and a tap on them acts in the game directly
 * (pokemon_storage_system.c, CtrStorage_Tap). So is the bag (item_menu.c,
 * CtrBag_Touch): BAG opens the game's own, left of the column; opened from a
 * battle, a shop or the PC it has the whole screen. And the Pokédex
 * (pokedex.c, CtrPokedex_Touch), left of the column.
 *
 * Map, trainer card, save and options are drawn and run here directly. The
 * party menu is the game's own too (party_menu.c, CTR_CENTRED_PARTY), drawn
 * left of the column and taking a tap itself (CtrMenu_PostTap). While one of
 * the game's screens is open, or on its way (a START entry asked for,
 * CtrBottom_KeepWorld), the top screen holds the world (CtrVideo_HoldTop, the
 * sessions below); the game still decides everything.
 *
 * The column is the port's own design (an emerald rail, plates, a focus
 * ring; see "The column's look"), with the game's item icons on it and a few
 * new pictures: the RUN shoe, the Y badge and the Pokédex icon
 * (3ds_bottom_art.h, from assets/artwork/bottom). Everything else is the
 * game's own graphics,
 * decoded from the same RomFS files the game loads: the party menu
 * background, the Hoenn region map, the trainer
 * card, mon/item/type/status icons, front pictures, the bag sprite, the
 * window frames and the game's fonts. Texts are the game's strings where it
 * has them. X walks the column with the buttons (ProcessKeys); the 3DS's Y
 * is SELECT (3ds_game_bridge.c).
 *
 * Cost model, chosen for an Old 3DS whose frame the top screen already fills:
 *   - every frame: a snapshot of the few values on screen (a memcmp of a few
 *     hundred bytes) and the touch state; no drawing;
 *   - when the snapshot changes: the canvas is recomposed by the CPU from
 *     pre-rendered backgrounds (a memcpy) plus the live parts, and copied to
 *     the framebuffer;
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
#include "match_call.h"
#include "menu_specialized.h"
#include "money.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_controllers.h"
#include "battle_main.h"
#include "battle_message.h"
#include "contest_effect.h"
#include "contest_util.h"
#include "decompress.h"
#include "data.h"
#include "event_data.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "fonts.h"
#include "graphics.h"
#include "item.h"
#include "item_icon.h"
#include "berry.h"
#include "item_menu.h"
#include "item_menu_icons.h"
#include "item_use.h"
#include "mail.h"
#include "menu.h"
#include "menu_helpers.h"
#include "new_game.h"
#include "overworld.h"
#include "palette.h"
#include "party_menu.h"
#include "pokedex.h"
#include "pokemon.h"
#include "pokemon_icon.h"
#include "pokeball.h"
#include "pokemon_storage_system.h"
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
#include "tv.h"
#include "util.h"
#include "constants/items.h"
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
#include "3ds_input.h"
#include "3ds_log.h"
#include "3ds_platform.h"
#include "3ds_video.h"

/* Exported by the game under PLATFORM_3DS, or not exported by its headers. */
void CB2_BagMenuRun(void);
/* party_menu.c: the column's buttons close the game's party menu. */
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
enum { START_POKEDEX, START_POKEMON, START_BAG, START_POKENAV, START_NONE = 0xFF };

#define W CTR_BOTTOM_WIDTH
#define H CTR_BOTTOM_HEIGHT
/* The content area left of the button column. */
#define CW 240
#define COL_X CW

/* ------------------------------------------------------------------------ */
/* Canvas                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * The canvas, in framebuffer layout, is what gets copied to the screen. The
 * big static pictures behind each view (party menu background, Hoenn map,
 * trainer card) are decoded once into caches of the same layout, so a redraw
 * starts with a memcpy instead of re-decoding tens of thousands of pixels.
 * Views draw in content coordinates; sOX moves them (battle menus centre the
 * 240-wide views in the whole screen).
 */
static u16 sCanvas[W * H] __attribute__((aligned(32)));
static u16 *sDst = sCanvas;
static int sOX;

/* CACHE_BATTLE is the battle art's backdrop itself, in this layout already. */
enum { CACHE_MENU, CACHE_WIDE, CACHE_MAP, CACHE_CARD, CACHE_BATTLE, CACHE_COUNT };
static u16 *sCache[CACHE_COUNT];
static int sCardCacheKey = -1;

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
     * options redraws the whole area left of the column). */
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

/* The screens of the button column, top to bottom. */
enum { SCR_MAP, SCR_POKEMON, SCR_BAG, SCR_CARD, SCR_POKEDEX, SCR_POKENAV, SCR_SAVE, SCR_OPTION, SCR_COUNT };
/* Below the screens' buttons: the registered item (Y) and RUN. */
enum { COL_Y = SCR_COUNT, COL_RUN, COL_ITEMS };

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
    /* The party menu's slot and CANCEL tilemaps (party_menu.c). */
    u8 *slotMain, *slotMainNoHp, *slotWide, *slotWideNoHp, *slotWideEmpty;
    u16 *cancelMap;
    /* The summary screen: its tiles, its eight palettes and the PP text's as
     * the ninth, the page tilemaps (PSS_PAGE_* order, then the egg's info),
     * the STATUS and move effect boxes, the move selector. */
    u8 *sumTiles;
    u32 sumTileCount;
    Pal sumPal[9];
    u16 *sumPage[5];
    const u16 *sumStatus, *sumEffectBattle, *sumEffectContest;
    u8 *moveSelectTiles;
    Pal moveSelectPal;
    /* The bag screen (item_menu.c): its tiles and tilemap, its two palettes
     * for each player, the bag picture per player and its palette, the HM
     * and SELECT marks, the scroll arrows and their red palette. */
    u8 *bagTiles;
    u32 bagTileCount;
    u16 *bagMap;
    Pal bagPal[2][2];
    u8 *bagSprite[2];
    Pal bagSpritePal;
    const u8 *hmIcon;
    u8 *selectIcon, *scrollTiles;
    Pal scrollPal;
    /* The PokéNav (pokenav_*.c): each BG layer's tiles, tilemap and the
     * palette in the slot its entries name (navPal: 0 header, 1 message or
     * call window, 2 device outline or Match Call UI, 3 dots or list, 4 call
     * frame, 5 the rematch Poké Ball); the sprites and their palettes. */
    u8 *navHeaderTiles, *navMessageTiles, *navDeviceTiles, *navDotsTiles;
    u32 navHeaderCount, navMessageCount, navDeviceCount, navDotsCount;
    u16 *navHeaderMap, *navMessageMap, *navDeviceMap, *navDotsMap;
    Pal navPal[6], navMcPal[6];
    u8 *navOptionTiles, *navLeftMain, *navLeftMatchCall, *navBlueLight;
    Pal navOptionPal[5], navLeftPal[5], navBlueLightPal;
    u8 *mcUiTiles, *mcPokeball, *mcFrame, *mcArrows, *mcCursor;
    u32 mcUiCount;
    u16 *mcUiMap;
    Pal mcArrowPal, mcCursorPal;
    /* CONDITION: the graph's art and palette (slot 1), the text palette, the
     * party's balls, the empty places, CANCEL (two palettes), the sheen's
     * sparkles, the CONDITION and PARTY headers. */
    u8 *condTiles, *condBall, *condPlace, *condCancel, *condSparkle, *navLeftCondition, *navLeftParty;
    u32 condCount;
    u16 *condMap;
    Pal condPal[6], condTextPal, condCancelPal[2], condSparklePal;
    /* RIBBONS: the list's art (slot 1) and its window's palette (slot 2);
     * the summary's art (slot 1), the icons (slots 2-6), the name window's
     * palette (slot 10); the big icons; the RIBBONS header. */
    u8 *ribListTiles, *ribSumTiles, *ribIcons, *ribBigIcons, *navLeftRibbons;
    u32 ribListCount, ribSumCount;
    u16 *ribListMap, *ribSumMap;
    Pal ribListPal[3], ribSumPal[2], ribIconPal[5], ribInfoPal;
    /* SEARCH: the results' frame (slot 1) and list window (slot 2), the
     * option patch on the condition screen (options.bin, BG3), the SEARCH
     * header and the categories' headers. */
    u8 *searchTiles, *navLeftSearch, *navLeftCategory[CONTEST_CATEGORIES_COUNT];
    u32 searchCount;
    u16 *searchMap, *searchPatch;
    Pal searchPal[3];
    /* Icons. */
    Icon column[SCR_COUNT];
    u8 *typeTiles;
    Pal typePal[3];
    const u8 *categoryTiles;
    Pal categoryPal;
    /* The party's icons (SAVE, POKéMON); the status and held item marks of
     * the game's party menu. */
    Pal monIconPal[3];
    u8 *statusTiles, *holdTiles;
    Pal statusPal, holdPal;
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
    u8 *cardTiles;
    u32 cardTileCount;
    u16 *cardFront, *cardBg;
    Pal cardPal[5][3];
    Pal cardFemaleBg, badgePal, starPal;
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
static void BuildBall(void);

static void LoadResources(void)
{
    static const char *const cardPals[5] = {
        "trainer_card/green.gbapal", "trainer_card/bronze.gbapal", "trainer_card/copper.gbapal",
        "trainer_card/silver.gbapal", "trainer_card/gold.gbapal",
    };
    u32 size = 0;   /* UnlzFile leaves it alone when it fails */

    PalSymbol(GetOverworldTextboxPalettePtr(), &sRes.text, 1);
    InitLook();
    BuildBall();

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

    sRes.slotMain = ReadRomfs("party_menu/slot_main.bin", NULL);
    sRes.slotMainNoHp = ReadRomfs("party_menu/slot_main_no_hp.bin", NULL);
    sRes.slotWide = ReadRomfs("party_menu/slot_wide.bin", NULL);
    sRes.slotWideNoHp = ReadRomfs("party_menu/slot_wide_no_hp.bin", NULL);
    sRes.slotWideEmpty = ReadRomfs("party_menu/slot_wide_empty.bin", NULL);
    sRes.cancelMap = ReadRomfs("party_menu/cancel_button.bin", NULL);
    sRes.sumTiles = Unlz(gSummaryScreen_Gfx, &size);
    sRes.sumTileCount = size / 32;
    {
        u16 *raw = Unlz(gSummaryScreen_Pal, NULL);
        const u16 *pp = Port_ResolveAssetPointer(gPPTextPalette);

        if (raw)
        {
            ToPals(sRes.sumPal, raw, 8);
            free(raw);
        }
        /* LoadPalette(&gPPTextPalette, BG_PLTT_ID(8) + 1, ...): colours 1-15. */
        for (int i = 1; pp && i < 16; ++i)
            sRes.sumPal[8].c[i] = Rgb565(pp[i - 1]);
    }
    sRes.sumPage[0] = Unlz(gSummaryPage_Info_Tilemap, NULL);
    sRes.sumPage[1] = Unlz(gSummaryPage_Skills_Tilemap, NULL);
    sRes.sumPage[2] = Unlz(gSummaryPage_BattleMoves_Tilemap, NULL);
    sRes.sumPage[3] = Unlz(gSummaryPage_ContestMoves_Tilemap, NULL);
    sRes.sumPage[4] = Unlz(gSummaryPage_InfoEgg_Tilemap, NULL);
    sRes.sumStatus = ReadRomfs("summary_screen/status_tilemap.bin", NULL);
    sRes.sumEffectBattle = Port_ResolveAssetPointer(gSummaryScreen_MoveEffect_Battle_Tilemap);
    sRes.sumEffectContest = Port_ResolveAssetPointer(gSummaryScreen_MoveEffect_Contest_Tilemap);
    sRes.moveSelectTiles = Unlz(gSummaryMoveSelect_Gfx, NULL);
    sRes.bagTiles = Unlz(gBagScreen_Gfx, &size);
    sRes.bagTileCount = size / 32;
    sRes.bagMap = Unlz(gBagScreen_GfxTileMap, NULL);
    {
        const u32 *pals[2] = {gBagScreenMale_Pal, gBagScreenFemale_Pal};
        u16 *raw;

        for (int g = 0; g < 2; ++g)
            if ((raw = Unlz(pals[g], NULL)) != NULL)
            {
                ToPals(sRes.bagPal[g], raw, 2);
                free(raw);
            }
        sRes.bagSprite[MALE] = Unlz(gBagMaleTiles, NULL);
        sRes.bagSprite[FEMALE] = Unlz(gBagFemaleTiles, NULL);
        if ((raw = Unlz(gBagPalette, NULL)) != NULL)
        {
            ToPals(&sRes.bagSpritePal, raw, 1);
            free(raw);
        }
    }
    sRes.hmIcon = Port_ResolveAssetPointer(gBagMenuHMIcon_Gfx);
    sRes.selectIcon = ReadRomfs("bag/select_button.4bpp", NULL);
    sRes.scrollTiles = UnlzFile("interface/scroll_indicator.4bpp.lz", NULL);
    PalFile("interface/red.gbapal", &sRes.scrollPal, 1, FALSE);
    sRes.navHeaderTiles = Unlz(gPokenavHeader_Gfx, &size);
    sRes.navHeaderCount = size / 32;
    sRes.navHeaderMap = Unlz(gPokenavHeader_Tilemap, NULL);
    sRes.navMessageTiles = Unlz(gPokenavMessageBox_Gfx, &size);
    sRes.navMessageCount = size / 32;
    sRes.navMessageMap = Unlz(gPokenavMessageBox_Tilemap, NULL);
    sRes.navDeviceTiles = UnlzFile("pokenav/device_outline.4bpp.lz", &size);
    sRes.navDeviceCount = size / 32;
    sRes.navDeviceMap = UnlzFile("pokenav/device_outline_map.bin.lz", NULL);
    sRes.navDotsTiles = UnlzFile("pokenav/bg_dots.4bpp.lz", &size);
    sRes.navDotsCount = size / 32;
    sRes.navDotsMap = UnlzFile("pokenav/bg_dots.bin.lz", NULL);
    PalFile("pokenav/header.gbapal", &sRes.navPal[0], 1, FALSE);
    PalFile("pokenav/message.gbapal", &sRes.navPal[1], 1, FALSE);
    PalFile("pokenav/device_outline.gbapal", &sRes.navPal[2], 1, FALSE);
    PalFile("pokenav/bg_dots.gbapal", &sRes.navPal[3], 1, FALSE);
    sRes.navOptionTiles = Unlz(gPokenavOptions_Gfx, NULL);
    PalSymbol(gPokenavOptions_Pal, sRes.navOptionPal, 5);
    sRes.navLeftMain = UnlzFile("pokenav/left_headers/main_menu.4bpp.lz", NULL);
    sRes.navLeftMatchCall = UnlzFile("pokenav/left_headers/match_call.4bpp.lz", NULL);
    PalFile("pokenav/left_headers/palette.gbapal", sRes.navLeftPal, 5, FALSE);
    sRes.navBlueLight = UnlzFile("pokenav/blue_light.4bpp.lz", NULL);
    PalFile("pokenav/blue_light.gbapal", &sRes.navBlueLightPal, 1, FALSE);
    /* Match Call: slot 0 the header's, 1 the call window, 2 its UI, 3 the
     * list window, 4 the call frame, 5 the rematch ball's first palette. */
    sRes.navMcPal[0] = sRes.navPal[0];
    PalFile("pokenav/match_call/call_window.gbapal", &sRes.navMcPal[1], 1, FALSE);
    PalFile("pokenav/match_call/ui.gbapal", &sRes.navMcPal[2], 1, FALSE);
    PalFile("pokenav/match_call/list_window.gbapal", &sRes.navMcPal[3], 1, FALSE);
    PalFile("pokenav/match_call/window.gbapal", &sRes.navMcPal[4], 1, FALSE);
    PalFile("pokenav/match_call/pokeball.gbapal", &sRes.navMcPal[5], 1, FALSE);
    sRes.mcUiTiles = UnlzFile("pokenav/match_call/ui.4bpp.lz", &size);
    sRes.mcUiCount = size / 32;
    sRes.mcUiMap = UnlzFile("pokenav/match_call/ui.bin.lz", NULL);
    sRes.mcPokeball = UnlzFile("pokenav/match_call/pokeball.4bpp.lz", NULL);
    sRes.mcFrame = ReadRomfs("pokenav/match_call/window.4bpp", NULL);
    sRes.mcArrows = UnlzFile("pokenav/list_arrows.4bpp.lz", NULL);
    PalFile("pokenav/list_arrows.gbapal", &sRes.mcArrowPal, 1, FALSE);
    sRes.mcCursor = UnlzFile("pokenav/match_call/options_cursor.4bpp.lz", NULL);
    sRes.condTiles = Unlz(gPokenavCondition_Gfx, &size);
    sRes.condCount = size / 32;
    sRes.condMap = Unlz(gPokenavCondition_Tilemap, NULL);
    sRes.condPal[0] = sRes.navPal[0];
    PalFile("pokenav/condition/graph.gbapal", &sRes.condPal[1], 1, FALSE);
    PalFile("pokenav/condition/text.gbapal", &sRes.condTextPal, 1, FALSE);
    sRes.condBall = ReadRomfs("pokenav/condition/pokeball.4bpp", NULL);
    sRes.condPlace = ReadRomfs("pokenav/condition/pokeball_placeholder.4bpp", NULL);
    sRes.condCancel = ReadRomfs("pokenav/condition/cancel.4bpp", NULL);
    PalFile("pokenav/condition/cancel.gbapal", sRes.condCancelPal, 2, FALSE);
    sRes.condSparkle = ReadRomfs("pokenav/condition/sparkle.4bpp", NULL);
    PalFile("pokenav/condition/sparkle.gbapal", &sRes.condSparklePal, 1, FALSE);
    sRes.navLeftCondition = UnlzFile("pokenav/left_headers/condition.4bpp.lz", NULL);
    sRes.navLeftParty = UnlzFile("pokenav/left_headers/party.4bpp.lz", NULL);
    sRes.ribListTiles = UnlzFile("pokenav/ribbons/list_bg.4bpp.lz", &size);
    sRes.ribListCount = size / 32;
    sRes.ribListMap = UnlzFile("pokenav/ribbons/list_bg.bin.lz", NULL);
    sRes.ribListPal[0] = sRes.navPal[0];
    PalFile("pokenav/ribbons/list_bg.gbapal", &sRes.ribListPal[1], 1, FALSE);
    PalFile("pokenav/ribbons/list_ui.gbapal", &sRes.ribListPal[2], 1, FALSE);
    sRes.ribSumTiles = Unlz(gPokenavRibbonsSummaryBg_Gfx, &size);
    sRes.ribSumCount = size / 32;
    sRes.ribSumMap = Unlz(gPokenavRibbonsSummaryBg_Tilemap, NULL);
    sRes.ribSumPal[0] = sRes.navPal[0];
    PalSymbol(gPokenavRibbonsSummaryBg_Pal, &sRes.ribSumPal[1], 1);
    sRes.ribIcons = UnlzFile("pokenav/ribbons/icons.4bpp.lz", NULL);
    sRes.ribBigIcons = UnlzFile("pokenav/ribbons/icons_big.4bpp.lz", NULL);
    for (int i = 0; i < 5; ++i)
    {
        char name[40];

        snprintf(name, sizeof(name), "pokenav/ribbons/icons%d.gbapal", i + 1);
        PalFile(name, &sRes.ribIconPal[i], 1, FALSE);
    }
    PalFile("pokenav/ribbons/mon_info.gbapal", &sRes.ribInfoPal, 1, FALSE);
    sRes.navLeftRibbons = UnlzFile("pokenav/left_headers/ribbons.4bpp.lz", NULL);
    sRes.searchTiles = UnlzFile("pokenav/condition/search_results.4bpp.lz", &size);
    sRes.searchCount = size / 32;
    sRes.searchMap = UnlzFile("pokenav/condition/search_results.bin.lz", NULL);
    sRes.searchPal[0] = sRes.navPal[0];
    PalFile("pokenav/condition/search_results.gbapal", &sRes.searchPal[1], 1, FALSE);
    PalFile("pokenav/condition/search_results_list.gbapal", &sRes.searchPal[2], 1, FALSE);
    sRes.searchPatch = ReadRomfs("pokenav/options/options.bin", NULL);
    sRes.navLeftSearch = UnlzFile("pokenav/left_headers/search.4bpp.lz", NULL);
    {
        static const char *const names[CONTEST_CATEGORIES_COUNT] = {"cool", "beauty", "cute", "smart", "tough"};

        for (int i = 0; i < CONTEST_CATEGORIES_COUNT; ++i)
        {
            char name[48];

            snprintf(name, sizeof(name), "pokenav/left_headers/%s.4bpp.lz", names[i]);
            sRes.navLeftCategory[i] = UnlzFile(name, NULL);
        }
    }
    PalFile("pokenav/match_call/options_cursor.gbapal", &sRes.mcCursorPal, 1, FALSE);
    {
        u16 *raw = Unlz(gSummaryMoveSelect_Pal, NULL);

        if (raw)
        {
            ToPals(&sRes.moveSelectPal, raw, 1);
            free(raw);
        }
    }

    /* The column's icons: the game's own item icons, and the PokéNav's. */
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

    sRes.typeTiles = UnlzFile("types/move_types.4bpp.lz", NULL);
    PalFile("types/move_types.gbapal.lz", sRes.typePal, 3, TRUE);
    {
        const u16 *pal;

        sRes.categoryTiles = GetMoveCategoryIconGfx(&pal);
        ToPals(&sRes.categoryPal, pal, 1);
    }
    for (int i = 0; i < 3; ++i)
        PalSymbol(gMonIconPaletteTable[i].data, &sRes.monIconPal[i], 1);
    sRes.statusTiles = Unlz(gStatusGfx_Icons, NULL);
    {
        u16 *raw = Unlz(gStatusPal_Icons, NULL);

        if (raw)
        {
            ToPals(&sRes.statusPal, raw, 1);
            free(raw);
        }
    }
    sRes.holdTiles = ReadRomfs("party_menu/hold_icons.4bpp", NULL);
    PalFile("party_menu/hold_icons.gbapal", &sRes.holdPal, 1, FALSE);

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

    sRes.cardTiles = UnlzFile("trainer_card/tiles.4bpp.lz", &size);
    sRes.cardTileCount = size / 32;
    sRes.cardFront = UnlzFile("trainer_card/front.bin.lz", NULL);
    sRes.cardBg = UnlzFile("trainer_card/bg.bin.lz", NULL);
    for (int i = 0; i < 5; ++i)
        PalFile(cardPals[i], sRes.cardPal[i], 3, FALSE);
    PalFile("trainer_card/female_bg.gbapal", &sRes.cardFemaleBg, 1, FALSE);
    PalFile("trainer_card/badges.gbapal", &sRes.badgePal, 1, FALSE);
    PalFile("trainer_card/star.gbapal", &sRes.starPal, 1, FALSE);
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

/* Mon icons, the first of their two 32x32 frames: the SAVE view's party. */
static struct
{
    u16 key;
    u32 age;
    u8 tiles[512];
} sMonIcons[PARTY_SIZE];

static const u8 *MonIcon(u16 iconSpecies, bool8 deoxysForm)
{
    u16 key = iconSpecies | (deoxysForm ? 0x8000 : 0);
    int victim = 0;
    const u8 *src;

    for (int i = 0; i < PARTY_SIZE; ++i)
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
    memcpy(sMonIcons[victim].tiles, src + (deoxysForm ? 0x400 : 0), 512);
    sMonIcons[victim].key = key;
    sMonIcons[victim].age = ++sIconClock;
    return sMonIcons[victim].tiles;
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

static Font sSmall, sNormal, sNarrow;

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
    /* The bag's item list (FONT_NARROW): the normal font's glyph layout. */
    sNarrow.glyphs = ResolveFontBase(gFontNarrowLatinGlyphs);
    sNarrow.widths = gFontNarrowLatinGlyphWidths;
    sNarrow.height = 15;
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
/* The column's look, and the sections' light green                         */
/* ------------------------------------------------------------------------ */

/*
 * The column is the port's own: an emerald rail with a diamond facet, the
 * buttons on it as plates (outline with clipped corners, a highlight row,
 * two shade rows, a tinted socket for the game's icon), a keyboard focus
 * ring, and below a groove the Y and RUN buttons. Native screens on the left
 * (map, save, options) and the party menu's and bag's backgrounds sit on one
 * light green with a thin frame line and a faint Poké Ball. The only new
 * pictures, the RUN shoe and the Y badge, are in 3ds_bottom_art.h.
 */

/* A plate's colours: outline, highlight row, body, shade rows, the icon's
 * socket and the socket's shade. */
typedef struct { u16 o, h, f, s, t, u; } PlateLook;

enum { RAIL_DEEP, RAIL_SHADE, RAIL_BODY, RAIL_FACET, RAIL_LIGHT, RAIL_COUNT };
enum { SECTION_BASE, SECTION_LINE, SECTION_LIGHT, SECTION_BALL, SECTION_BALL_TOP, SECTION_COUNT };
enum { LED_EDGE, LED_OFF, LED_OFF_LIGHT, LED_ON, LED_ON_LIGHT, LED_COUNT };

static struct
{
    u16 rail[RAIL_COUNT];
    u16 section[SECTION_COUNT];
    /* Normal, chosen or pressed, not available, and RUN turned on. */
    PlateLook plate, chosen, off, run;
    /* The focus ring's two blink frames: outer ring, inner line. */
    u16 ring[2], ringIn[2];
    u16 chosenShadow, offText, offShadow;
    u16 led[LED_COUNT];
    Pal art;
} sLook;

#define LOOK(r, g, b) Rgb565(RGB(r, g, b))

static void InitLook(void)
{
    static const u16 section[SECTION_COUNT] = {
        CTR_SECTION_BASE, CTR_SECTION_LINE, CTR_SECTION_LIGHT, CTR_SECTION_BALL, CTR_SECTION_BALL_TOP,
    };

    sLook.rail[RAIL_DEEP] = LOOK(3, 9, 7);
    sLook.rail[RAIL_SHADE] = LOOK(5, 14, 10);
    sLook.rail[RAIL_BODY] = LOOK(6, 17, 12);
    sLook.rail[RAIL_FACET] = LOOK(9, 21, 15);
    sLook.rail[RAIL_LIGHT] = LOOK(20, 28, 23);
    for (int i = 0; i < SECTION_COUNT; ++i)
        sLook.section[i] = Rgb565(section[i]);
    sLook.plate = (PlateLook){LOOK(7, 9, 13), LOOK(31, 31, 31), LOOK(30, 30, 31), LOOK(25, 26, 28), LOOK(26, 30, 27),
                              LOOK(20, 26, 22)};
    sLook.chosen = (PlateLook){LOOK(3, 11, 7), LOOK(21, 30, 23), LOOK(11, 24, 16), LOOK(7, 19, 12), LOOK(8, 21, 13),
                               LOOK(6, 17, 11)};
    sLook.off = (PlateLook){LOOK(17, 18, 20), LOOK(29, 29, 29), LOOK(27, 27, 27), LOOK(24, 24, 25), LOOK(25, 25, 25),
                            LOOK(23, 23, 23)};
    sLook.run = sLook.plate;
    sLook.run.h = LOOK(29, 31, 29);
    sLook.run.f = sLook.plate.t;
    sLook.run.s = sLook.plate.u;
    sLook.ring[0] = LOOK(31, 13, 6);
    sLook.ring[1] = LOOK(31, 25, 9);
    sLook.ringIn[0] = LOOK(31, 22, 15);
    sLook.ringIn[1] = LOOK(31, 30, 20);
    sLook.chosenShadow = LOOK(4, 14, 9);
    sLook.offText = LOOK(18, 18, 19);
    sLook.offShadow = LOOK(29, 29, 29);
    sLook.led[LED_EDGE] = LOOK(2, 6, 5);
    sLook.led[LED_OFF] = LOOK(12, 14, 13);
    sLook.led[LED_OFF_LIGHT] = LOOK(19, 21, 20);
    sLook.led[LED_ON] = LOOK(8, 29, 12);
    sLook.led[LED_ON_LIGHT] = LOOK(26, 31, 26);
    ToPals(&sLook.art, sArtPalette, 1);
}

/*
 * A plate: 1px outline with its corners cut and its inner corners filled, a
 * highlight row, two shade rows, and the first `socket` columns tinted for an
 * icon (0: none). Pressed, the highlight goes and the shade starts a row
 * lower, so the face looks pushed in.
 */
static void DrawPlate(int x, int y, int w, int h, const PlateLook *p, int socket, bool8 pressed)
{
    int shade = h - 3 + (pressed ? 1 : 0);

    FillRect(x + 1, y, w - 2, 1, p->o);
    FillRect(x + 1, y + h - 1, w - 2, 1, p->o);
    for (int j = 1; j < h - 1; ++j)
    {
        bool8 top = j == 1 && !pressed, low = j >= shade;
        u16 body = top ? p->h : low ? p->s : p->f;

        Put(x, y + j, p->o);
        Put(x + w - 1, y + j, p->o);
        if (socket > 0)
        {
            FillRect(x + 1, y + j, socket - 2, 1, top ? p->h : low ? p->u : p->t);
            Put(x + socket - 1, y + j, p->u);
            FillRect(x + socket, y + j, w - 1 - socket, 1, body);
        }
        else
        {
            FillRect(x + 1, y + j, w - 2, 1, body);
        }
    }
    Put(x + 1, y + 1, p->o);
    Put(x + w - 2, y + 1, p->o);
    Put(x + 1, y + h - 2, p->o);
    Put(x + w - 2, y + h - 2, p->o);
}

/* The keyboard focus: the plate's edge turns warm and a ring stands 1px
 * outside it, in the gap between plates. */
static void DrawRing(int x, int y, int w, int h, u8 frame)
{
    u16 a = sLook.ring[frame & 1], b = sLook.ringIn[frame & 1];

    FillRect(x + 1, y - 1, w - 2, 1, a);
    FillRect(x + 1, y + h, w - 2, 1, a);
    FillRect(x - 1, y + 1, 1, h - 2, a);
    FillRect(x + w, y + 1, 1, h - 2, a);
    FillRect(x + 1, y, w - 2, 1, b);
    FillRect(x + 1, y + h - 1, w - 2, 1, b);
    FillRect(x, y + 1, 1, h - 2, b);
    FillRect(x + w - 1, y + 1, 1, h - 2, b);
    Put(x, y, a);
    Put(x + w - 1, y, a);
    Put(x, y + h - 1, a);
    Put(x + w - 1, y + h - 1, a);
}

/* The column's geometry: eight plates, then a groove and the Y and RUN
 * buttons. A plate's hit is its whole 24px row of the column. */
#define PLATE_X (COL_X + 4)
#define PLATE_W 74
#define PLATE_H 22
#define PLATE_Y(i) (3 + (i) * 24)
#define PLATE_SOCKET 25
#define GROOVE_Y 196
#define SQUARE_Y 201
#define SQUARE_SIZE 36
#define SQUARE_X(k) (COL_X + 4 + (k) * 38)

/* The rail: the diamond facet over the whole column, the seam that cuts it
 * from whatever the section draws, and the groove above Y and RUN. */
static void DrawColumnBackground(void)
{
    for (int x = COL_X; x < W; ++x)
        for (int y = 0; y < H; ++y)
        {
            int d = abs(2 * (x & 7) - 7) + abs(2 * (y & 7) - 7);
            Put(x, y, sLook.rail[d == 8 ? RAIL_FACET : RAIL_BODY]);
        }
    FillRect(COL_X, 0, 1, H, sLook.rail[RAIL_DEEP]);
    FillRect(COL_X + 1, 0, 1, H, sLook.rail[RAIL_LIGHT]);
    FillRect(COL_X + 2, 0, 1, H, sLook.rail[RAIL_SHADE]);
    FillRect(COL_X + 3, GROOVE_Y, W - COL_X - 3, 1, sLook.rail[RAIL_DEEP]);
    FillRect(COL_X + 3, GROOVE_Y + 1, W - COL_X - 3, 1, sLook.rail[RAIL_LIGHT]);
}

/*
 * The faint Poké Ball in the middle of the light green: 2px rim, 2px band,
 * the button's ring with nothing inside, the bottom half - the white one on a
 * real ball - half a step lighter.
 * Symmetric about the pixel grid: distances from pixel centres, doubled so
 * they stay integers. 0 nothing, 1 the line colour, 2 the bottom half.
 */
#define BALL_R CTR_SECTION_BALL_RADIUS
static u8 sBall[2 * BALL_R * 2 * BALL_R];

static void BuildBall(void)
{
    for (int y = 0; y < 2 * BALL_R; ++y)
        for (int x = 0; x < 2 * BALL_R; ++x)
        {
            int dx = 2 * x + 1 - 2 * BALL_R, dy = 2 * y + 1 - 2 * BALL_R, d2 = dx * dx + dy * dy;
            u8 v = 0;

            if (d2 >= 4 * BALL_R * BALL_R)
                v = 0;
            else if (d2 >= 4 * (BALL_R - 2) * (BALL_R - 2))
                v = 1;
            else if (d2 < 4 * 15 * 15)
                v = (d2 >= 4 * 13 * 13 || (d2 >= 4 * 7 * 7 && d2 < 4 * 9 * 9)) ? 1 : 0;
            else if (dy > -4 && dy < 4)
                v = 1;
            else if (dy > 0)
                v = 2;
            sBall[y * 2 * BALL_R + x] = v;
        }
}

const uint8_t *CtrBottom_BallMask(void)
{
    return sBall;
}

/* The light green of a section over [x0, x1) x [y0, y1): flat, a frame line
 * one pixel in (and its light line), the Poké Ball in the middle. */
static void DrawSection(int x0, int y0, int x1, int y1)
{
    int w = x1 - x0, h = y1 - y0, bx = (x0 + x1) / 2 - BALL_R, by = (y0 + y1) / 2 - BALL_R;

    FillRect(x0, y0, w, h, sLook.section[SECTION_BASE]);
    FillRect(x0 + 1, y0 + 1, w - 2, 1, sLook.section[SECTION_LINE]);
    FillRect(x0 + 1, y1 - 2, w - 2, 1, sLook.section[SECTION_LINE]);
    FillRect(x0 + 1, y0 + 2, w - 2, 1, sLook.section[SECTION_LIGHT]);
    FillRect(x0 + 1, y1 - 1, w - 2, 1, sLook.section[SECTION_LIGHT]);
    FillRect(x0 + 1, y0 + 1, 1, h - 2, sLook.section[SECTION_LINE]);
    FillRect(x1 - 2, y0 + 1, 1, h - 2, sLook.section[SECTION_LINE]);
    FillRect(x0 + 2, y0 + 1, 1, h - 2, sLook.section[SECTION_LIGHT]);
    for (int y = 0; y < 2 * BALL_R; ++y)
        for (int x = 0; x < 2 * BALL_R; ++x)
        {
            u8 v = sBall[y * 2 * BALL_R + x];

            if (v)
                Put(bx + x, by + y, sLook.section[v == 1 ? SECTION_BALL : SECTION_BALL_TOP]);
        }
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
    /* The PokéNav, drawn by the compositor left of the column. */
    MODE_POKENAV,
    /* The PC's boxes, drawn by the compositor over the whole screen. */
    MODE_STORAGE,
    /* The game's Pokédex, drawn by the compositor left of the column. */
    MODE_POKEDEX,
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
    u16 hp, maxHp, item;
    u8 nick[POKEMON_NAME_LENGTH + 2];
} MonView;

/* Everything a redraw reads. Zeroed before every snapshot: memcmp-safe. */
typedef struct
{
    u8 mode, screen, pressed, gender, inBattle;
    u8 title;                         /* MODE_OFF on the title screen */
    u8 enabled;                       /* bit per column screen */
    /* Party, for the SAVE screen's icons and the POKéMON screen. */
    MonView party[PARTY_SIZE];
    /* POKéMON: the list or partySel's summary, partySwap the one being moved
     * (PARTY_NO_SWAP), whether the party may change now; the summary. */
    u8 partyView, partySel, partySwap, canEdit;
    /* Its actions box, message, YES/NO or Soft-Boiled's target (PACT_*): the
     * actions (PACTION_*), the message. */
    u8 partyAct, partyActCount, partyActions[MAX_MON_MOVES + 4], partyMore;
    u8 partyMsg[160];
    /* Using an item from the bag: each slot's text in place of its HP (a TM's
     * ABLE...), the move picker's (PICK_*), the level-up stats and page. */
    const u8 *partyDesc[PARTY_SIZE];
    s16 partyStats[NUM_STATS * 2];
    u8 partyPick, partyStatsPage;
    /* The MAIL read (PARTY_MAIL), or the bag's blank one. */
    struct CtrMailView mail;
    u8 types[2], nature, ability;
    u16 stats[NUM_STATS];
    u16 moves[MAX_MON_MOVES];
    u8 pp[MAX_MON_MOVES], maxPp[MAX_MON_MOVES];
    /* The summary: its page (SUM_*), the move whose details show
     * (SUM_NO_MOVE), and what the game's summary screen shows. */
    u8 sumPage, sumMove, sumMoveSwap, shiny, ball, otGender, ribbons, expTicks, sumOk;
    u16 dexNum, otId;
    u32 personality, exp, expNext;
    u8 otName[PLAYER_NAME_LENGTH + 10];
    u8 memo[200], eggState[80];
    /* BAG: the pocket, its first row on show, its stacks, the row picked
     * (BAG_NO_ROW; the stacks' count is CANCEL), the one being moved, and the
     * rows on show. */
    u8 bagPocket, bagTop, bagCount, bagSel, bagSwap, bagOk, bagGive;
    u8 bagMsg[96];
    /* The tray's work on the item picked (BACT_*, BASK_*): its actions
     * (BAG_ACT_*), how many to toss, the message (in the description's
     * place, or a box) and its page. */
    u8 bagAct, bagAsk, bagBoxDesc, bagMore, bagActions[5], bagActionCount, bagRiding;
    u16 bagTossQty;
    u8 bagBox[160];
    u8 bagTagBerry;                   /* CHECK TAG's berry (ITEM_TO_BERRY) */
    u16 bagItem[8], bagQty[8], bagSelItem;
    /* POKéNAV: the main menu or Match Call; the options shown (NAV_OPT_*
     * order), the one picked, the blue light's blink; Match Call's list
     * (each row's entry and rematch ball), its count and battles, the
     * entry picked and the place it is at, its options box, the call's page. */
    u8 navView, navOptions[6], navOptionCount, navCursor, navLight, navOk;
    u8 mcTop, mcSel, mcBox, mcBoxCursor, mcRematch, mcMapsec;
    u16 mcCount, mcEntry[8];
    u32 mcBattles;
    u8 mcCall, mcPage;
    /* CONDITION: the party's Pokémon shown (no eggs; their slots), the one
     * picked (condCount is CANCEL), its five conditions (CONDITION_* order)
     * and sheen. */
    u8 condCount, condSel, condSlots[PARTY_SIZE], conditions[CONDITION_COUNT], sheen;
    /* RIBBONS: the list's first row on show, its length, the Pokémon picked;
     * the rows on show (box << 8 | place, and their counts); the summary's
     * ribbons (normal, then gift ones), the one shown big (RIB_NONE), and
     * the Pokémon's name, gender, level. */
    u16 ribTop, ribCount, ribSel, ribRow[8];
    u8 ribRowCount[8], ribIds[25], ribIdCount, ribGift[7], ribGiftCount, ribBig, ribGender, ribLevel;
    u8 ribNick[POKEMON_NAME_LENGTH + 2];
    /* SEARCH: the category, the results' first row on show, their count,
     * the one picked; the rows on show (box << 8 | place) and their ranks;
     * on the condition screen, where the Pokémon is (a box, or RIB_PARTY_BOX)
     * and its rank. */
    u8 searchCat, searchBox;
    u16 searchTop, searchCount, searchSel, searchRow[8], searchRank[8], searchMonRank;
    /* Region map. */
    u8 mapsec, cursorX, cursorY, pickMapsec, pickX, pickY;
    u8 clockShown, clockValid, clockRate, clockPhase;
    u16 clockMinute;
    /* Bag: which of the game's is on show, BAG_VIEW_*. */
    u8 bagView;
    /* Trainer card. */
    u8 name[PLAYER_NAME_LENGTH + 1];
    u8 hasDex, stars, badges, minutes;
    u16 id, dex, hours;
    u32 money;
    /* Save and options. */
    u8 saveStep, canSave, frame;
    u8 options[OPTION_ROWS];
    /* The OPTIONS page on show (CTR_EXTRAS_*) and the values of its extras,
     * in table order. */
    u8 optPage, optSub;
    u16 extras[24];
    /* The column's Y and RUN: the registered item; bit 0 running is the
     * default, bit 1 the player has the shoes. */
    u16 registered;
    u8 run;
    /* The keyboard focus (X): the column item, or FOCUS_NONE; inside the
     * options, the option; the ring's blink frame. */
    u8 focus, optFocus, blink;
    /* Battle: the cursor is 4 on the quick ball (actions) or CANCEL (moves). */
    u8 isDouble, safari, cursor, battler;
    u16 quickBall, quickBallCount;    /* the ball a quick throw would use, ITEM_NONE if none */
    u8 safariBalls;
    u8 partyBalls[PARTY_SIZE];        /* PARTY_BALL_*, as the game's party indicator */
    struct ChooseMoveStruct moves4;
    u8 text[256];
} ViewState;

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
/*
 * The options in two columns of six: the game's own on the left in its order,
 * the port's on the right. A build without the FPS counter or the voxel
 * renderer has no cell for them; with the voxel overworld off its camera,
 * blur and battle cells stay where they are, greyed.
 */
#define OPT_CELL_W 112
#define OPT_CELL_H 32

static bool8 OptionExists(int row)
{
    return row < OPTION_SHOWN && !(row == OPT_FPS && !CTR_SHOW_FPS);
}

static bool8 OptionLive(int row, bool8 voxel)
{
    return OptionExists(row) && (voxel || row < OPT_VOXEL_PITCH);
}

/* An extra for the voxel overworld (3D WORLD, 3D DEPTH) is greyed with it
 * off, as the grid's camera and blur cells are. */
static bool8 ExtraLive(const CtrExtra *extra, bool8 voxel)
{
    return voxel || !CtrExtras_NeedsVoxel(extra);
}

/* OPTIONS has pages (3ds_extras.h) once any extra exists: a tab strip at the
 * top and the cells under it, rows a little closer. */
static u8 sOptPage;
/* The screen of the tab on show (CTR_EXTRAS_SCREEN). */
static u8 sOptSub;
#define CELLS_PER_PAGE 12

static bool8 OptionPages(void)
{
    return gCtrExtraCount > 0;
}

#define TAB_Y 2
#define TAB_H 20

/* The SETTINGS tab's first screen is the fixed grid of the game's and the
 * port's options; its later screens hold extras (3ds_extras.c) like the
 * other tabs. */
static bool8 FixedOptions(void)
{
    return sOptPage == CTR_EXTRAS_OPTIONS && sOptSub == 0;
}

static void OptionCell(int row, int *x, int *y)
{
    bool8 right = row >= OPT_FPS;

    *x = right ? 124 : 4;
    *y = OptionPages() ? TAB_Y + TAB_H + 4 + (right ? row - OPT_FPS : row) * 36
                       : 4 + (right ? row - OPT_FPS : row) * 40;
}

/* The extra on a page's cell `row` (filled column by column, six to a
 * column, as the options are), or NULL. */
/* How many screens a tab's extras take. */
static unsigned PageSubs(unsigned page)
{
    unsigned screens = 1;

    for (unsigned i = 0; i < gCtrExtraCount; ++i)
        if (CTR_EXTRAS_TAB(gCtrExtras[i].page) == page && (gCtrExtras[i].page >> 4) + 1u > screens)
            screens = (gCtrExtras[i].page >> 4) + 1;
    return screens;
}

static const CtrExtra *PageExtra(unsigned page, unsigned row)
{
    unsigned screen = CTR_EXTRAS_SCREEN(page, page == sOptPage ? sOptSub : 0);

    if (row >= CELLS_PER_PAGE)
        return NULL;
    for (unsigned i = 0, n = 0; i < gCtrExtraCount; ++i)
        if (gCtrExtras[i].page == screen && n++ == row)
            return &gCtrExtras[i];
    return NULL;
}

/* What a cell shows, for the redraw to notice a change: its value, or for
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

/* The column's keyboard focus (X), and where it is inside a screen. */
#define FOCUS_NONE 0xFF
enum { INSIDE_NONE, INSIDE_OPTIONS, INSIDE_SAVE };
static u8 sFocus = FOCUS_NONE, sInside, sOptFocus;
/* Leaving the focus, the buttons stay the column's until they are let go:
 * the B or A that left it must not reach the game. */
static bool8 sSwallow;
static u32 sFrames;

static bool8 RectsMeet(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1);

/* Per screen state, kept while another screen is shown. */

static u8 sPickMapsec = MAPSEC_NONE, sPickX, sPickY;
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
    HIT_COLUMN = 0x10,     /* + screen */
    HIT_CANCEL = 0x30,
    HIT_OK,
    HIT_YES,
    HIT_NO,
    HIT_PARTY_ACTION,      /* + party action row */
    HIT_PARTY_YES = 0x3C,
    HIT_PARTY_NO,
    HIT_PARTY_MESSAGE,
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
    HIT_SLOT = 0xA0,       /* + party slot */
    HIT_PARTY_BACK = 0xA8,
    HIT_PARTY_SWITCH,
    HIT_PARTY_PREV,
    HIT_PARTY_NEXT,
    HIT_SUM_PAGE_PREV,
    HIT_SUM_PAGE_NEXT,
    HIT_SUM_MOVE,          /* + move slot */
    HIT_BAG_POCKET_PREV = 0xB4,
    HIT_BAG_POCKET_NEXT,
    HIT_BAG_UP,
    HIT_BAG_DOWN,
    HIT_BAG_PAGE_UP,
    HIT_BAG_PAGE_DOWN,
    HIT_BAG_REGISTER,
    HIT_BAG_SWITCH,
    HIT_BAG_CANCEL,
    HIT_NAV_OPTION = 0x40, /* + PokéNav main menu row */
    HIT_NAV_UP = 0x48,
    HIT_NAV_DOWN,
    HIT_NAV_BACK,
    HIT_NAV_CALL,
    HIT_NAV_BOX_CANCEL,
    HIT_NAV_MESSAGE,
    HIT_OPTION = 0xC0,     /* + option row; +HIT_OPTION_BACK for the left arrow */
    HIT_PAGE = 0xE0,       /* + OPTIONS page tab */
    HIT_BAG_ACT = 0xF0,    /* + bag tray action */
    HIT_BAG_YES = 0xF5,
    HIT_BAG_NO,
    HIT_BAG_QTY,           /* + -10, -1, +1, +10 */
    HIT_BAG_OK = 0xFB,
    HIT_PARTY_MOVE = 0x20, /* + move slot */
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

static u8 EnabledScreens(void)
{
    u8 mask = (1 << SCR_MAP) | (1 << SCR_BAG) | (1 << SCR_CARD) | (1 << SCR_OPTION);

    if (FlagGet(FLAG_SYS_POKEMON_GET))
        mask |= 1 << SCR_POKEMON;
    if (FlagGet(FLAG_SYS_POKEDEX_GET))
        mask |= 1 << SCR_POKEDEX;
    if (FlagGet(FLAG_SYS_POKENAV_GET) && CtrStartMenu_Available())
        mask |= 1 << SCR_POKENAV;
    if (CtrStartMenu_Available())
        mask |= 1 << SCR_SAVE;
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
    /* Before the boxes: the bag can be opened from them, whole screen too. */
    if (gMain.callback2 == CB2_BagMenuRun && gBagMenu)
        return MODE_BAG_MENU;
    /* A summary opened from the boxes is on the whole screen too. */
    if (CtrStorage_IsOpen() || CtrVideo_BottomWhole())
        return MODE_STORAGE;
    if (FuncIsActiveTask(Task_HandleChooseMonInput))
        sPartyMenuCallback = gMain.callback2;
    if (sPartyMenuCallback && gMain.callback2 == sPartyMenuCallback)
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
                || mode == MODE_STORAGE || mode == MODE_POKEDEX;
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

/* The save view's file: the card's figures, where the player is, the
 * party's icons and the window frame the player chose. */
static void SnapshotSave(ViewState *s)
{
    SnapshotCard(s);
    s->mapsec = gMapHeader.regionMapSectionId;
    s->frame = gSaveBlock2Ptr->optionsWindowFrameType;
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
    }
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


/* Every option's value as the OPTIONS screen shows it: the snapshot's, and
 * the cell pictures made ahead (WarmOptionCells) read the same. */
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

/* ------------------------------------------------------------------------ */
/* POKéMON: the party, drawn here while the world goes on above            */
/* ------------------------------------------------------------------------ */

/*
 * The party is read from the save and drawn by the bottom screen itself:
 * nothing opens the game's party menu, so the top screen keeps the game
 * running. A tap on a Pokémon opens the game's actions box: SUMMARY, its
 * field moves, SWITCH, which moves it. A change to the party, or a field
 * move, waits for the field to be free of events; a field move then runs on
 * the field as the game runs it from its menu (party_menu.c's CtrParty_*).
 */
enum { PARTY_LIST, PARTY_SUMMARY, PARTY_MAIL };
#define PARTY_NO_SWAP 0xFF
static u8 sPartyView, sPartySel, sPartySwap = PARTY_NO_SWAP;
/* Over the list: nothing, the actions, a message (a tap closes it, for
 * sPartyActBack), a YES/NO question (sPartyAsk: a field move's, on
 * sPartyActMove, switching the held item for sPartyGiveItem, or taking a
 * MAIL), the Pokémon Soft-Boiled (or Milk Drink) is used on, GIVE / TAKE,
 * READ / TAKE, or the game's mail writing under way. A message goes page by
 * page (sPartyMsgPage), as the game's {PAUSE} prompts. */
enum { PACT_NONE, PACT_MENU, PACT_MESSAGE, PACT_ASK, PACT_TARGET, PACT_ITEM, PACT_MAIL, PACT_WRITING,
       /* From the bag (sPartyFromBag, back there when done): the Pokémon an
        * item is used on or given to, a move for it or to forget, the
        * level-up stats. */
       PACT_USE, PACT_GIVE, PACT_PICK_MOVE, PACT_STATS };
enum { PICK_PP, PICK_FORGET };
/* ITEM's place is MAIL's for a Pokémon holding one. */
enum { PACTION_SUMMARY, PACTION_SWITCH, PACTION_ITEM, PACTION_CANCEL, PACTION_MAIL, PACTION_FIELD_MOVE };
enum { PASK_FIELD_MOVE, PASK_SWITCH_ITEMS, PASK_MAIL_TO_PC, PASK_LOSE_MAIL, PASK_REPLACE_MOVE, PASK_STOP_LEARNING };
static u8 sPartyAct, sPartyActBack, sPartyActMove, sPartyAsk, sPartyMsgPage;
static u16 sPartyGiveItem;
static u8 sPartyMsg[256];
/* After a message, instead of sPartyActBack: the next step of a flow. */
static void (*sPartyThen)(void);
/* An item from the bag: the item, the move picker's work, the stats of a
 * level-up, the move being learned (from a TM, or the level's). */
static bool8 sPartyFromBag;
static u16 sPartyUseItem;
static u8 sPartyPick, sPartyStatsPage;
static s16 sPartyStats[NUM_STATS * 2];
static u16 sLearnMove, sLearnTm;
static bool8 sLearnLevelUp;
/* The summary's pages, PSS_PAGE_* order. */
enum { SUM_INFO, SUM_SKILLS, SUM_BATTLE, SUM_CONTEST, SUM_PAGES };
#define SUM_NO_MOVE 0xFF
static u8 sSumPage, sSumMove = SUM_NO_MOVE;
/* SWITCH on a picked move: the move waiting for the one it changes place
 * with (SUM_NO_MOVE). */
static u8 sSumMoveSwap = SUM_NO_MOVE;

/* pokemon_summary_screen.c (0057): the trainer memo, or an egg's state and
 * memo, as the summary writes them. */
void CtrSummary_MemoText(struct Pokemon *mon, u8 *memo, u32 memoSize, u8 *eggState, u32 eggStateSize);

/* The memo of the Pokémon on show, made again when it changes. Made only
 * with no script running: the memo is written with the string buffers a
 * message may be printing from (0057 puts them back all the same). */
static struct
{
    bool8 valid;
    u16 species;
    u32 personality;
    u8 memo[200], eggState[80];
} sMemo;

static void SummaryMemo(struct Pokemon *mon)
{
    u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
    u32 personality = GetMonData(mon, MON_DATA_PERSONALITY);

    if (sMemo.valid && sMemo.species == species && sMemo.personality == personality)
        return;
    sMemo.valid = FALSE;
    sMemo.memo[0] = sMemo.eggState[0] = EOS;
    if (ScriptContext_IsEnabled())
        return;
    CtrSummary_MemoText(mon, sMemo.memo, sizeof(sMemo.memo), sMemo.eggState, sizeof(sMemo.eggState));
    sMemo.species = species;
    sMemo.personality = personality;
    sMemo.valid = TRUE;
}

/* The front picture of the Pokémon on show, as the summary loads it
 * (LoadSpecialPokePic: Unown's letter, Spinda's spots), and its palette.
 * A front picture holds all its frames (two for most species): room for
 * as many as the game's own sprite buffers. The first is drawn. */
static struct
{
    bool8 valid;
    u16 species;
    u32 personality;
    u8 tiles[MON_PIC_SIZE * MAX_MON_PIC_FRAMES];
    Pal pal;
} sSumPic;

static void SummaryPicOf(u16 species, u32 otId, u32 personality)
{
    u16 *raw;

    if (sSumPic.valid && sSumPic.species == species && sSumPic.personality == personality)
        return;
    LoadSpecialPokePic(&gMonFrontPicTable[species], sSumPic.tiles, species, personality, TRUE);
    raw = Unlz(GetMonSpritePalFromSpeciesAndPersonality(species, otId, personality), NULL);
    if (raw)
    {
        ToPals(&sSumPic.pal, raw, 1);
        free(raw);
    }
    sSumPic.species = species;
    sSumPic.personality = personality;
    sSumPic.valid = TRUE;
}

static void SummaryPic(struct Pokemon *mon)
{
    SummaryPicOf(GetMonData(mon, MON_DATA_SPECIES_OR_EGG), GetMonData(mon, MON_DATA_OT_ID),
                 GetMonData(mon, MON_DATA_PERSONALITY));
}

/* The caught ball's picture (its first frame), each loaded once. */
static u8 *sBallTiles[POKEBALL_COUNT];
static Pal sBallPal[POKEBALL_COUNT];

static void SummaryBall(u8 ball)
{
    u16 *raw;

    if (ball >= POKEBALL_COUNT || sBallTiles[ball])
        return;
    sBallTiles[ball] = Unlz(gBallSpriteSheets[ball].data, NULL);
    raw = Unlz(gBallSpritePalettes[ball].data, NULL);
    if (raw)
    {
        ToPals(&sBallPal[ball], raw, 1);
        free(raw);
    }
}

/* Whether the party may change now: the player in the field, no event,
 * script or battle under way. Walking is fine. */
static bool8 PartyEditable(void)
{
    return gMain.callback2 == CB2_Overworld && !gMain.inBattle && !ScriptContext_IsEnabled()
        && !ArePlayerFieldControlsLocked() && !gPaletteFade.active;
}

static void ResetPartyScreen(void)
{
    sPartyView = PARTY_LIST;
    sPartySwap = PARTY_NO_SWAP;
    sPartyAct = PACT_NONE;
    sPartyThen = NULL;
    sPartyFromBag = FALSE;
}

/* SetPartyMonFieldSelectionActions without ITEM: an egg's has no field
 * moves (GetPartyMenuActionsType). */
static u8 PartyActions(u8 slot, u8 *actions)
{
    u8 count = 0;

    actions[count++] = PACTION_SUMMARY;
    if (!GetMonData(&gPlayerParty[slot], MON_DATA_IS_EGG))
    {
        u8 moves[MAX_MON_MOVES], n = CtrParty_FieldMoves(slot, moves);

        for (int i = 0; i < n; ++i)
            actions[count++] = PACTION_FIELD_MOVE + moves[i];
    }
    if (gPlayerPartyCount > 1)
        actions[count++] = PACTION_SWITCH;
    if (!GetMonData(&gPlayerParty[slot], MON_DATA_IS_EGG))
        actions[count++] = ItemIsMail(GetMonData(&gPlayerParty[slot], MON_DATA_HELD_ITEM)) ? PACTION_MAIL : PACTION_ITEM;
    actions[count++] = PACTION_CANCEL;
    return count;
}

/* Page `page` of a message into `out`: up to its next prompt ({PAUSE}'s
 * \p or \l), and whether one more follows. */
static bool8 MessagePage(const u8 *text, u8 page, u8 *out, u32 size)
{
    u32 n = 0;

    for (;;)
    {
        u8 c = *text;

        if (c == EOS)
            break;
        if (c == CHAR_PROMPT_CLEAR || c == CHAR_PROMPT_SCROLL)
        {
            ++text;
            if (page == 0)
                break;
            --page;
            continue;
        }
        if (c == EXT_CTRL_CODE_BEGIN && text[1] != EOS)
        {
            u8 len = 1 + GetExtCtrlCodeLength(text[1]);

            while (len-- && *text != EOS)
            {
                if (page == 0 && n + 1 < size)
                    out[n++] = *text;
                ++text;
            }
            continue;
        }
        if (page == 0 && n + 1 < size)
            out[n++] = c;
        ++text;
    }
    out[n] = EOS;
    return *text != EOS;
}

static void SnapshotPartyMons(ViewState *s)
{
    for (int i = 0; i < PARTY_SIZE && i < gPlayerPartyCount; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        MonView *view = &s->party[i];
        u16 species = GetMonData(mon, MON_DATA_SPECIES);

        if (species == SPECIES_NONE)
            continue;
        view->species = species;
        view->isEgg = GetMonData(mon, MON_DATA_IS_EGG);
        view->iconSpecies = view->isEgg ? SPECIES_EGG : GetIconSpecies(species, GetMonData(mon, MON_DATA_PERSONALITY));
        view->deoxys = species == SPECIES_DEOXYS;
        GetMonData(mon, MON_DATA_NICKNAME, view->nick);
        StringGet_Nickname(view->nick);
        view->item = GetMonData(mon, MON_DATA_HELD_ITEM);
        view->gender = MON_GENDERLESS;
        if (view->isEgg)
            continue;
        view->level = GetMonData(mon, MON_DATA_LEVEL);
        view->hp = GetMonData(mon, MON_DATA_HP);
        view->maxHp = GetMonData(mon, MON_DATA_MAX_HP);
        view->fainted = view->hp == 0;
        view->ailment = GetMonAilment(mon);
        /* As the party menu: no symbol where the name already shows it. */
        if (!((species == SPECIES_NIDORAN_F || species == SPECIES_NIDORAN_M)
              && StringCompare(view->nick, gSpeciesNames[species]) == 0))
            view->gender = GetMonGender(mon);
    }
}

static void SnapshotPartySummary(ViewState *s)
{
    static const u8 statIds[NUM_STATS] = {
        MON_DATA_MAX_HP, MON_DATA_ATK, MON_DATA_DEF, MON_DATA_SPATK, MON_DATA_SPDEF, MON_DATA_SPEED,
    };
    struct Pokemon *mon = &gPlayerParty[s->partySel];
    const MonView *view = &s->party[s->partySel];
    u8 ppBonuses;

    if (!view->species)
        return;
    SummaryMemo(mon);
    SummaryPic(mon);
    s->personality = GetMonData(mon, MON_DATA_PERSONALITY);
    s->sumPage = view->isEgg ? SUM_INFO : sSumPage;
    s->sumMove = s->sumPage >= SUM_BATTLE ? sSumMove : SUM_NO_MOVE;
    StringCopy(s->memo, sMemo.memo);
    StringCopy(s->eggState, sMemo.eggState);
    s->ball = ItemIdToBallId(GetMonData(mon, MON_DATA_POKEBALL));
    SummaryBall(s->ball);
    s->sumOk = sRes.sumTiles && sRes.sumPage[0] && sRes.sumPage[4];
    if (view->isEgg)
        return;
    s->shiny = IsMonShiny(mon);
    s->dexNum = SpeciesToPokedexNum(view->species);
    GetMonData(mon, MON_DATA_OT_NAME, s->otName);
    s->otGender = GetMonData(mon, MON_DATA_OT_GENDER);
    s->otId = GetMonData(mon, MON_DATA_OT_ID) & 0xFFFF;
    s->ribbons = GetRibbonCount(mon);
    s->types[0] = gSpeciesInfo[view->species].types[0];
    s->types[1] = gSpeciesInfo[view->species].types[1];
    for (int i = 0; i < NUM_STATS; ++i)
        s->stats[i] = GetMonData(mon, statIds[i]);
    s->nature = GetNature(mon);
    s->ability = GetMonAbility(mon);
    ppBonuses = GetMonData(mon, MON_DATA_PP_BONUSES);
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        s->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i);
        if (s->moves[i] == MOVE_NONE)
            continue;
        s->pp[i] = GetMonData(mon, MON_DATA_PP1 + i);
        s->maxPp[i] = CalculatePPWithBonus(s->moves[i], ppBonuses, i);
    }
    if (s->sumMove != SUM_NO_MOVE && s->moves[s->sumMove] == MOVE_NONE)
        s->sumMove = SUM_NO_MOVE;
    s->sumMoveSwap = s->sumMove != SUM_NO_MOVE ? sSumMoveSwap : SUM_NO_MOVE;
    /* DrawExperienceProgressBar and PrintExpPointsNextLevel. */
    s->exp = GetMonData(mon, MON_DATA_EXP);
    if (view->level < MAX_LEVEL)
    {
        const u32 *table = gExperienceTables[gSpeciesInfo[view->species].growthRate];
        u32 between = table[view->level + 1] - table[view->level], since = s->exp - table[view->level];
        u32 ticks = between ? (u64)since * 64 / between : 0;

        s->expNext = table[view->level + 1] - s->exp;
        s->expTicks = ticks == 0 && since != 0 ? 1 : ticks > 64 ? 64 : ticks;
    }
}

static void PartyMailWritten(void);

static void SnapshotParty(ViewState *s)
{
    SnapshotPartyMons(s);
    if (sPartySel >= gPlayerPartyCount)
    {
        sPartySel = 0;
        sPartyView = PARTY_LIST;
    }
    if (sPartySwap != PARTY_NO_SWAP && sPartySwap >= gPlayerPartyCount)
        sPartySwap = PARTY_NO_SWAP;
    if (sPartyView == PARTY_MAIL && GetMonData(&gPlayerParty[sPartySel], MON_DATA_MAIL) >= PARTY_SIZE)
        sPartyView = PARTY_LIST;
    if (sPartyView != PARTY_LIST)
        sPartyAct = PACT_NONE;
    if (sPartyAct == PACT_WRITING)
        PartyMailWritten();
    s->partyView = sPartyView;
    s->partySel = sPartySel;
    s->partySwap = sPartySwap;
    s->partyAct = sPartyAct;
    if (sPartyAct == PACT_MENU)
        s->partyActCount = PartyActions(sPartySel, s->partyActions);
    /* Done with the bag's item: back to the bag, as the game's menu closes. */
    if (sPartyFromBag && sPartyAct == PACT_NONE && sPartyView == PARTY_LIST)
    {
        sPartyFromBag = FALSE;
        sScreen = SCR_BAG;
    }
    if (sPartyAct == PACT_MESSAGE || sPartyAct == PACT_ASK || sPartyAct == PACT_TARGET || sPartyAct == PACT_USE
     || sPartyAct == PACT_GIVE || sPartyAct == PACT_PICK_MOVE || sPartyAct == PACT_STATS)
        s->partyMore = MessagePage(sPartyMsg, sPartyMsgPage, s->partyMsg, sizeof(s->partyMsg));
    if (sPartyAct == PACT_USE)
        for (int i = 0; i < gPlayerPartyCount && i < PARTY_SIZE; ++i)
            s->partyDesc[i] = CtrParty_ItemDescText(i, sPartyUseItem);
    if (sPartyAct == PACT_PICK_MOVE)
    {
        struct Pokemon *mon = &gPlayerParty[sPartySel];

        s->partyPick = sPartyPick;
        for (int i = 0; i < MAX_MON_MOVES; ++i)
        {
            s->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i);
            s->pp[i] = GetMonData(mon, MON_DATA_PP1 + i);
            s->maxPp[i] = s->moves[i] ? CalculatePPWithBonus(s->moves[i], GetMonData(mon, MON_DATA_PP_BONUSES), i) : 0;
        }
    }
    if (sPartyAct == PACT_STATS)
    {
        memcpy(s->partyStats, sPartyStats, sizeof(sPartyStats));
        s->partyStatsPage = sPartyStatsPage;
    }
    s->canEdit = PartyEditable();
    s->frame = gSaveBlock2Ptr->optionsWindowFrameType;
    if (sPartyView == PARTY_SUMMARY)
        SnapshotPartySummary(s);
    else if (sPartyView == PARTY_MAIL)
        CtrMail_View(&gSaveBlock1Ptr->mail[GetMonData(&gPlayerParty[sPartySel], MON_DATA_MAIL)], &s->mail);
}

/* ------------------------------------------------------------------------ */
/* BAG: the pockets, drawn here while the world goes on above              */
/* ------------------------------------------------------------------------ */

/*
 * The bag is read from the save and drawn by the bottom screen itself, as
 * the party is: nothing opens the game's bag. Each pocket keeps its first
 * row on show and its picked row, as gBagPosition keeps them for the game.
 * REGISTER and SWITCH ITEM (the game's SELECT: an item moved to another
 * place in its pocket) wait for the field to be free of events.
 */
#define BAG_ROWS 8
#define BAG_NO_ROW 0xFF
static u8 sBagPocket, sBagTop[POCKETS_COUNT], sBagSel[POCKETS_COUNT] = {BAG_NO_ROW, BAG_NO_ROW, BAG_NO_ROW, BAG_NO_ROW,
                                                                          BAG_NO_ROW};
static u8 sBagSwap = BAG_NO_ROW;

static u8 BagStacks(u8 pocket)
{
    u8 n = 0;

    while (n < gBagPockets[pocket].capacity && gBagPockets[pocket].itemSlots[n].itemId != ITEM_NONE)
        ++n;
    return n;
}

/* GIVE from the party: the Pokémon the bag picks an item for
 * (PARTY_NO_SWAP: none), and a message where the description goes. */
static u8 sBagGiveTo = PARTY_NO_SWAP;
static u8 sBagMsg[96];
/* The tray's work on the item picked: nothing (its actions), how many to
 * toss, a question (sBagAsk), a message, a blank MAIL. The message is the
 * game's: in the description's place (TOSS's) or a box under the bag
 * (DisplayItemMessage's), page by page. */
enum { BACT_NONE, BACT_QTY, BACT_ASK, BACT_MESSAGE, BACT_MAIL, BACT_TAG };
enum { BASK_TOSS, BASK_TEACH, BASK_FIELD_HM };
/* sContextMenuItems_*'s, in their order. */
enum { BAG_ACT_USE, BAG_ACT_GIVE, BAG_ACT_TOSS, BAG_ACT_REGISTER, BAG_ACT_CHECK_TAG, BAG_ACT_CHECK };
static u8 sBagAct, sBagAsk, sBagBoxDesc, sBagMsgPage, sBagHmUser;
static u16 sBagItem, sBagTossQty;
static u8 sBagBox[256];
static struct Mail sBagMail;

static void BagBox(const u8 *text, u8 act, bool8 desc)
{
    StringCopy(sBagBox, text);
    sBagAct = act;
    sBagBoxDesc = desc;
    sBagMsgPage = 0;
}

/* OpenContextMenu's for the field (ITEMMENULOCATION_FIELD), or GIVE alone
 * for a Pokémon's. */
static u8 BagActions(u8 pocket, u16 item, u8 *actions)
{
    u8 n = 0;

    if (sBagGiveTo != PARTY_NO_SWAP)
    {
        actions[n++] = BAG_ACT_GIVE;
        return n;
    }
    switch (pocket)
    {
    case ITEMS_POCKET:
        actions[n++] = ItemIsMail(item) ? BAG_ACT_CHECK : BAG_ACT_USE;
        actions[n++] = BAG_ACT_GIVE;
        actions[n++] = BAG_ACT_TOSS;
        break;
    case KEYITEMS_POCKET:
        actions[n++] = BAG_ACT_USE;
        actions[n++] = BAG_ACT_REGISTER;
        break;
    case BALLS_POCKET:
        actions[n++] = BAG_ACT_GIVE;
        actions[n++] = BAG_ACT_TOSS;
        break;
    case TMHM_POCKET:
        actions[n++] = BAG_ACT_USE;
        actions[n++] = BAG_ACT_GIVE;
        break;
    case BERRIES_POCKET:
        actions[n++] = BAG_ACT_CHECK_TAG;
        actions[n++] = BAG_ACT_USE;
        actions[n++] = BAG_ACT_GIVE;
        actions[n++] = BAG_ACT_TOSS;
        break;
    }
    return n;
}

static void ResetBagScreen(void)
{
    sBagSwap = BAG_NO_ROW;
    sBagGiveTo = PARTY_NO_SWAP;
    sBagMsg[0] = EOS;
    sBagAct = BACT_NONE;
}

static void SnapshotBag(ViewState *s)
{
    u8 count = BagStacks(sBagPocket), rows = count + 1;  /* and CANCEL */

    if (sBagSel[sBagPocket] != BAG_NO_ROW && sBagSel[sBagPocket] > count)
        sBagSel[sBagPocket] = BAG_NO_ROW;
    if (sBagSwap != BAG_NO_ROW && sBagSwap >= count)
        sBagSwap = BAG_NO_ROW;
    if (sBagTop[sBagPocket] + BAG_ROWS > rows)
        sBagTop[sBagPocket] = rows > BAG_ROWS ? rows - BAG_ROWS : 0;
    s->bagPocket = sBagPocket;
    s->bagTop = sBagTop[sBagPocket];
    s->bagCount = count;
    s->bagSel = sBagSel[sBagPocket];
    s->bagSwap = sBagSwap;
    if (sBagGiveTo != PARTY_NO_SWAP && sBagGiveTo >= gPlayerPartyCount)
        sBagGiveTo = PARTY_NO_SWAP;
    s->bagGive = sBagGiveTo;
    StringCopy(s->bagMsg, sBagMsg);
    s->frame = gSaveBlock2Ptr->optionsWindowFrameType;
    s->bagAct = sBagAct;
    if (sBagAct == BACT_TAG && sBagSel[sBagPocket] >= count)
        sBagAct = BACT_NONE;
    if (sBagAct == BACT_TAG)
        s->bagTagBerry = ITEM_TO_BERRY(BagGetItemIdByPocketPosition(sBagPocket + 1, sBagSel[sBagPocket]));
    else if (sBagAct == BACT_MAIL)
        CtrMail_View(&sBagMail, &s->mail);
    else if (sBagAct != BACT_NONE)
    {
        s->bagAsk = sBagAsk;
        s->bagBoxDesc = sBagBoxDesc;
        s->bagTossQty = sBagTossQty;
        s->bagMore = MessagePage(sBagBox, sBagMsgPage, s->bagBox, sizeof(s->bagBox));
    }
    s->canEdit = PartyEditable();
    s->bagOk = sRes.bagTiles && sRes.bagMap;
    for (int i = 0; i < BAG_ROWS && s->bagTop + i < count; ++i)
    {
        s->bagItem[i] = BagGetItemIdByPocketPosition(sBagPocket + 1, s->bagTop + i);
        s->bagQty[i] = BagGetQuantityByPocketPosition(sBagPocket + 1, s->bagTop + i);
    }
    s->bagSelItem = s->bagSel == BAG_NO_ROW ? ITEM_NONE
                  : s->bagSel == count       ? ITEM_LIST_END
                                             : BagGetItemIdByPocketPosition(sBagPocket + 1, s->bagSel);
    if (s->bagSel < count)
        s->bagActionCount = BagActions(sBagPocket, s->bagSelItem, s->bagActions);
    s->bagRiding = TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE) != 0;
}

/* ------------------------------------------------------------------------ */
/* POKéNAV: its main menu and Match Call, drawn here as the world goes on   */
/* ------------------------------------------------------------------------ */

/*
 * The PokéNav's main menu, Match Call, CONDITION's party and RIBBONS are
 * drawn by the bottom screen itself from the save, as the party and the bag
 * are, CONDITION's SEARCH too. MAP is the bottom screen's own map.
 * A call is made only with no script running: its message is written with
 * the string buffers a field message may be printing from (put back after).
 */
enum { PNAV_MENU, PNAV_MATCH_CALL, PNAV_CONDITION_MENU, PNAV_CONDITION, PNAV_RIBBONS, PNAV_RIBBON_SUMMARY,
       PNAV_SEARCH_MENU, PNAV_SEARCH_RESULTS, PNAV_SEARCH_CONDITION };
/* SEARCH's menu: the five categories, then CANCEL. */
#define SEARCH_OPT_CANCEL CONTEST_CATEGORIES_COUNT
/* The CONDITION submenu's options. */
enum { COND_OPT_PARTY, COND_OPT_SEARCH, COND_OPT_CANCEL, COND_OPT_COUNT };
enum { NAV_OPT_MAP, NAV_OPT_CONDITION, NAV_OPT_MATCH_CALL, NAV_OPT_RIBBONS, NAV_OPT_SWITCH_OFF };
#define MC_ROWS 8
#define MC_NO_ENTRY 0xFF
static u8 sNavView, sNavCursor, sCondMenuCursor, sCondSel, sMcTop, sMcSel = MC_NO_ENTRY, sMcBoxCursor;
static bool8 sMcBox, sMcCall;
static u8 sMcPage, sMcPages, sMcPageStart[24];
static u8 sMcMessage[1000];
static struct PokenavMatchCallEntry sMcEntries[MC_HEADER_COUNT + REMATCH_TABLE_ENTRIES];
static u16 sMcCount;

/* pokenav_menu_handler.c's option lists, by the flags the game reads,
 * without HOENN MAP: the column's MAP is the bottom screen's map. */
static u8 NavOptions(u8 *options)
{
    u8 n = 0;

    options[n++] = NAV_OPT_CONDITION;
    if (FlagGet(FLAG_ADDED_MATCH_CALL_TO_POKENAV))
    {
        options[n++] = NAV_OPT_MATCH_CALL;
        if (FlagGet(FLAG_SYS_RIBBON_GET))
            options[n++] = NAV_OPT_RIBBONS;
    }
    options[n++] = NAV_OPT_SWITCH_OFF;
    return n;
}

/* LoopedTask_BuildMatchCallList: the special trainers enabled, then the
 * registered rematch trainers. */
static void BuildMatchCall(void)
{
    sMcCount = 0;
    for (int i = 0; i < MC_HEADER_COUNT; ++i)
        if (MatchCall_GetEnabled(i))
            sMcEntries[sMcCount++] = (struct PokenavMatchCallEntry){TRUE, MatchCall_GetMapSec(i), i};
    for (int i = 0; i < REMATCH_TABLE_ENTRIES; ++i)
        if (!MatchCall_HasRematchId(i) && IsRematchEntryRegistered(i))
            sMcEntries[sMcCount++] = (struct PokenavMatchCallEntry){FALSE, GetMatchTableMapSectionId(i), i};
}

/* ShouldDrawRematchPokeballIcon. */
static bool8 McRematch(const struct PokenavMatchCallEntry *e)
{
    int index = e->isSpecialTrainer ? MatchCall_GetRematchTableIdx(e->headerId) : e->headerId;

    return index != REMATCH_TABLE_ENTRIES && gSaveBlock1Ptr->trainerRematches[index] != 0;
}

/* AreAnyTrainerRematchesNearby: the main menu's blue light. */
static bool8 NavRematchNearby(void)
{
    for (int i = 0; i < REMATCH_TABLE_ENTRIES; ++i)
        if (GetMatchTableMapSectionId(i) == gMapHeader.regionMapSectionId && IsRematchEntryRegistered(i)
            && gSaveBlock1Ptr->trainerRematches[i])
            return TRUE;
    return FALSE;
}

static void ResetNavScreen(void)
{
    sNavView = PNAV_MENU;
    sMcBox = sMcCall = FALSE;
    sMcSel = MC_NO_ENTRY;
}

/*
 * RIBBONS (pokenav_ribbons_list.c, pokenav_ribbons_summary.c): the party's
 * and the boxes' Pokémon with ribbons, most ribbons first (the game's
 * stable insertion), built once when the list opens; a Pokémon's ribbons
 * from MON_DATA_RIBBONS as GetMonRibbons reads them.
 */
#define RIB_PARTY_BOX TOTAL_BOXES_COUNT
#define RIB_NONE 0xFF
/* pokenav_ribbons_summary.c's grid: nine to a row, the gift ribbons on the
 * row after the normal ones' three. */
#define RIBBONS_PER_ROW 9
#define GIFT_RIBBON_ROW (1 + (FIRST_GIFT_RIBBON / RIBBONS_PER_ROW))
#define GIFT_RIBBON_START_POS (RIBBONS_PER_ROW * GIFT_RIBBON_ROW)
static struct { u8 box, place, count; } sRibList[TOTAL_BOXES_COUNT * IN_BOX_COUNT + PARTY_SIZE];
static u16 sRibListCount, sRibTop, sRibSel;
static u8 sRibBig = RIB_NONE;

static struct BoxPokemon *RibbonMon(u16 index)
{
    return sRibList[index].box == RIB_PARTY_BOX ? &gPlayerParty[sRibList[index].place].box
                                                : GetBoxedMonPtr(sRibList[index].box, sRibList[index].place);
}

static void RibbonListAdd(u8 box, u8 place, u8 count)
{
    u16 i = sRibListCount;

    /* After every entry with as many or more: ties keep their order. */
    while (i > 0 && sRibList[i - 1].count < count)
    {
        sRibList[i] = sRibList[i - 1];
        --i;
    }
    sRibList[i].box = box;
    sRibList[i].place = place;
    sRibList[i].count = count;
    ++sRibListCount;
}

static void BuildRibbonList(void)
{
    sRibListCount = 0;
    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u8 count;

        if (!GetMonData(mon, MON_DATA_SANITY_HAS_SPECIES))
            break;
        if (GetMonData(mon, MON_DATA_SANITY_IS_EGG) || GetMonData(mon, MON_DATA_SANITY_IS_BAD_EGG))
            continue;
        if ((count = GetMonData(mon, MON_DATA_RIBBON_COUNT)) != 0)
            RibbonListAdd(RIB_PARTY_BOX, i, count);
    }
    for (int box = 0; box < TOTAL_BOXES_COUNT; ++box)
        for (int place = 0; place < IN_BOX_COUNT; ++place)
        {
            u8 count;

            if (CheckBoxMonSanityAt(box, place)
             && (count = GetBoxMonData(GetBoxedMonPtr(box, place), MON_DATA_RIBBON_COUNT)) != 0)
                RibbonListAdd(box, place, count);
        }
}

static void SnapshotRibbons(ViewState *s)
{
    if (sRibTop + 8 > sRibListCount)
        sRibTop = sRibListCount > 8 ? sRibListCount - 8 : 0;
    if (sRibSel >= sRibListCount)
        sRibSel = 0;
    s->ribTop = sRibTop;
    s->ribCount = sRibListCount;
    s->ribSel = sRibSel;
    for (int i = 0; i < 8 && sRibTop + i < sRibListCount; ++i)
    {
        s->ribRow[i] = sRibList[sRibTop + i].box << 8 | sRibList[sRibTop + i].place;
        s->ribRowCount[i] = sRibList[sRibTop + i].count;
    }
    if (sNavView != PNAV_RIBBON_SUMMARY || sRibListCount == 0)
        return;
    {
        struct BoxPokemon *mon = RibbonMon(sRibSel);
        u32 ribbons = GetBoxMonData(mon, MON_DATA_RIBBONS);
        u16 species = GetBoxMonData(mon, MON_DATA_SPECIES);
        /* sRibbonData: each contest's rank count adds its ranks' ribbons. */
        static const u8 contestBase[] = {COOL_RIBBON_NORMAL, BEAUTY_RIBBON_NORMAL, CUTE_RIBBON_NORMAL,
                                         SMART_RIBBON_NORMAL, TOUGH_RIBBON_NORMAL};

        if (ribbons & 1)
            s->ribIds[s->ribIdCount++] = CHAMPION_RIBBON;
        for (int c = 0; c < 5; ++c)
            for (int n = 0; n < (int)((ribbons >> (1 + 3 * c)) & 7) && n < 4; ++n)
                s->ribIds[s->ribIdCount++] = contestBase[c] + n;
        for (int b = 0; b < 4; ++b)
            if (ribbons & (1 << (16 + b)))
                s->ribIds[s->ribIdCount++] = WINNING_RIBBON + b;
        for (int b = 0; b < 7; ++b)
            if (ribbons & (1 << (20 + b)))
                s->ribGift[s->ribGiftCount++] = FIRST_GIFT_RIBBON + b;
        s->ribBig = sRibBig;
        GetBoxMonData(mon, MON_DATA_NICKNAME, s->ribNick);
        StringGet_Nickname(s->ribNick);
        s->ribGender = GetBoxMonGender(mon);
        s->ribLevel = GetLevelFromBoxMonExp(mon);
        s->personality = GetBoxMonData(mon, MON_DATA_PERSONALITY);
        SummaryPicOf(species, GetBoxMonData(mon, MON_DATA_OT_ID), s->personality);
    }
}

/*
 * SEARCH (pokenav_conditions_search_results.c): every Pokémon of the party
 * (no eggs) and the boxes, most of the category's condition first (ties
 * after, as the game inserts them); a rank shared by equal values.
 */
static struct { u8 box, place, value; u16 rank; } sSearchList[TOTAL_BOXES_COUNT * IN_BOX_COUNT + PARTY_SIZE];
static u16 sSearchCount, sSearchTop, sSearchSel;
static u8 sSearchCat, sSearchMenuCursor;

static u8 ConditionOf(struct BoxPokemon *mon, u8 category)
{
    static const u8 data[CONTEST_CATEGORIES_COUNT] = {MON_DATA_COOL, MON_DATA_BEAUTY, MON_DATA_CUTE, MON_DATA_SMART,
                                                      MON_DATA_TOUGH};

    return GetBoxMonData(mon, data[category]);
}

static void SearchAdd(u8 box, u8 place, u8 value)
{
    u16 i = sSearchCount;

    while (i > 0 && sSearchList[i - 1].value < value)
    {
        sSearchList[i] = sSearchList[i - 1];
        --i;
    }
    sSearchList[i].box = box;
    sSearchList[i].place = place;
    sSearchList[i].value = value;
    ++sSearchCount;
}

static void BuildSearch(u8 category)
{
    sSearchCount = 0;
    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];

        if (!GetMonData(mon, MON_DATA_SANITY_HAS_SPECIES))
            break;
        if (!GetMonData(mon, MON_DATA_SANITY_IS_EGG) && !GetMonData(mon, MON_DATA_SANITY_IS_BAD_EGG))
            SearchAdd(RIB_PARTY_BOX, i, ConditionOf(&mon->box, category));
    }
    for (int box = 0; box < TOTAL_BOXES_COUNT; ++box)
        for (int place = 0; place < IN_BOX_COUNT; ++place)
            if (CheckBoxMonSanityAt(box, place))
                SearchAdd(box, place, ConditionOf(GetBoxedMonPtr(box, place), category));
    for (u16 i = 0; i < sSearchCount; ++i)
        sSearchList[i].rank = i > 0 && sSearchList[i].value == sSearchList[i - 1].value ? sSearchList[i - 1].rank : i + 1;
}

static struct BoxPokemon *SearchMon(u16 index)
{
    return sSearchList[index].box == RIB_PARTY_BOX ? &gPlayerParty[sSearchList[index].place].box
                                                   : GetBoxedMonPtr(sSearchList[index].box, sSearchList[index].place);
}

static void SnapshotSearch(ViewState *s)
{
    if (sSearchTop + 8 > sSearchCount)
        sSearchTop = sSearchCount > 8 ? sSearchCount - 8 : 0;
    if (sSearchSel >= sSearchCount)
        sSearchSel = 0;
    s->searchCat = sSearchCat;
    s->searchTop = sSearchTop;
    s->searchCount = sSearchCount;
    s->searchSel = sSearchSel;
    for (int i = 0; i < 8 && sSearchTop + i < sSearchCount; ++i)
    {
        s->searchRow[i] = sSearchList[sSearchTop + i].box << 8 | sSearchList[sSearchTop + i].place;
        s->searchRank[i] = sSearchList[sSearchTop + i].rank;
    }
    if (sNavView != PNAV_SEARCH_CONDITION || sSearchCount == 0)
        return;
    {
        /* The condition screen of the one picked, as the party's is. */
        struct BoxPokemon *mon = SearchMon(sSearchSel);
        u16 species = GetBoxMonData(mon, MON_DATA_SPECIES);

        s->searchBox = sSearchList[sSearchSel].box;
        s->searchMonRank = sSearchList[sSearchSel].rank;
        s->conditions[CONDITION_COOL] = GetBoxMonData(mon, MON_DATA_COOL);
        s->conditions[CONDITION_TOUGH] = GetBoxMonData(mon, MON_DATA_TOUGH);
        s->conditions[CONDITION_SMART] = GetBoxMonData(mon, MON_DATA_SMART);
        s->conditions[CONDITION_CUTE] = GetBoxMonData(mon, MON_DATA_CUTE);
        s->conditions[CONDITION_BEAUTY] = GetBoxMonData(mon, MON_DATA_BEAUTY);
        s->sheen = GetBoxMonData(mon, MON_DATA_SHEEN);
        GetBoxMonData(mon, MON_DATA_NICKNAME, s->ribNick);
        StringGet_Nickname(s->ribNick);
        s->ribGender = GetBoxMonGender(mon);
        if (species == SPECIES_NIDORAN_M || species == SPECIES_NIDORAN_F)
        {
            u8 name[POKEMON_NAME_LENGTH + 2];

            StringCopy(name, s->ribNick);
            if (StringCompare(name, gSpeciesNames[species]) == 0)
                s->ribGender = MON_GENDERLESS;
        }
        s->ribLevel = GetLevelFromBoxMonExp(mon);
        s->personality = GetBoxMonData(mon, MON_DATA_PERSONALITY);
        SummaryPicOf(species, GetBoxMonData(mon, MON_DATA_OT_ID), s->personality);
    }
}

/* The condition screen: the party without its eggs (as
 * pokenav_conditions.c lists it), then CANCEL. */
static void SnapshotCondition(ViewState *s)
{
    SnapshotPartyMons(s);
    s->condCount = 0;
    for (int i = 0; i < PARTY_SIZE; ++i)
        if (s->party[i].species && !s->party[i].isEgg)
            s->condSlots[s->condCount++] = i;
    if (sCondSel > s->condCount)
        sCondSel = 0;
    s->condSel = sCondSel;
    if (s->condSel < s->condCount)
    {
        struct Pokemon *mon = &gPlayerParty[s->condSlots[s->condSel]];

        SummaryPic(mon);
        s->personality = GetMonData(mon, MON_DATA_PERSONALITY);
        s->conditions[CONDITION_COOL] = GetMonData(mon, MON_DATA_COOL);
        s->conditions[CONDITION_TOUGH] = GetMonData(mon, MON_DATA_TOUGH);
        s->conditions[CONDITION_SMART] = GetMonData(mon, MON_DATA_SMART);
        s->conditions[CONDITION_CUTE] = GetMonData(mon, MON_DATA_CUTE);
        s->conditions[CONDITION_BEAUTY] = GetMonData(mon, MON_DATA_BEAUTY);
        s->sheen = GetMonData(mon, MON_DATA_SHEEN);
    }
}

static void SnapshotNav(ViewState *s)
{
    s->navOk = sRes.navHeaderTiles && sRes.navHeaderMap && sRes.navDotsMap && sRes.navDeviceMap;
    s->navView = sNavView;
    if (sNavView == PNAV_CONDITION_MENU)
    {
        s->navOptionCount = COND_OPT_COUNT;
        for (int i = 0; i < COND_OPT_COUNT; ++i)
            s->navOptions[i] = i;
        s->navCursor = sCondMenuCursor;
        return;
    }
    if (sNavView == PNAV_CONDITION)
    {
        SnapshotCondition(s);
        return;
    }
    if (sNavView == PNAV_RIBBONS || sNavView == PNAV_RIBBON_SUMMARY)
    {
        SnapshotRibbons(s);
        return;
    }
    if (sNavView == PNAV_SEARCH_MENU)
    {
        s->navOptionCount = SEARCH_OPT_CANCEL + 1;
        for (int i = 0; i <= SEARCH_OPT_CANCEL; ++i)
            s->navOptions[i] = i;
        s->navCursor = sSearchMenuCursor;
        return;
    }
    if (sNavView == PNAV_SEARCH_RESULTS || sNavView == PNAV_SEARCH_CONDITION)
    {
        SnapshotSearch(s);
        return;
    }
    s->navOptionCount = NavOptions(s->navOptions);
    if (sNavCursor >= s->navOptionCount)
        sNavCursor = 0;
    s->navCursor = sNavCursor;
    s->navLight = FlagGet(FLAG_ADDED_MATCH_CALL_TO_POKENAV) && NavRematchNearby() ? 1 + ((sFrames / 9) & 1) : 0;
    if (sNavView != PNAV_MATCH_CALL)
        return;
    BuildMatchCall();
    if (sMcSel != MC_NO_ENTRY && sMcSel >= sMcCount)
        sMcSel = MC_NO_ENTRY, sMcBox = FALSE;
    if (sMcTop + MC_ROWS > sMcCount)
        sMcTop = sMcCount > MC_ROWS ? sMcCount - MC_ROWS : 0;
    s->mcCount = sMcCount;
    s->mcTop = sMcTop;
    for (int i = 0; i < MC_ROWS && sMcTop + i < sMcCount; ++i)
    {
        const struct PokenavMatchCallEntry *e = &sMcEntries[sMcTop + i];

        s->mcEntry[i] = e->headerId | (e->isSpecialTrainer ? 0x8000 : 0);
        if (McRematch(e))
            s->mcRematch |= 1 << i;
    }
    s->mcSel = sMcSel;
    s->mcMapsec = sMcSel != MC_NO_ENTRY ? sMcEntries[sMcSel].mapSec : MAPSEC_NONE;
    s->mcBattles = GetGameStat(GAME_STAT_TRAINER_BATTLES);
    if (s->mcBattles > 99999)
        s->mcBattles = 99999;
    s->mcBox = sMcBox;
    s->mcBoxCursor = sMcBoxCursor;
    s->mcCall = sMcCall;
    s->mcPage = sMcPage;
    s->canEdit = PartyEditable();
}

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
        /* The party menu from the field is left of the column; from a
         * battle, a contest or a facility it covers it (CtrCentredParty). */
        s->bagView = !gMain.inBattle && gPartyMenu.menuType == PARTY_MENU_TYPE_FIELD ? BAG_VIEW_SECTION
                                                                                       : BAG_VIEW_WHOLE;
        if (s->bagView == BAG_VIEW_WHOLE)
            s->screen = SCR_COUNT;
    }
    else if (mode == MODE_BAG_MENU)
    {
        s->screen = SCR_BAG;
        /* The bag from the field is left of the column; from anything else
         * it covers the column. */
        s->bagView = gBagPosition.location == ITEMMENULOCATION_FIELD ? BAG_VIEW_SECTION : BAG_VIEW_WHOLE;
        if (s->bagView == BAG_VIEW_WHOLE)
            s->screen = SCR_COUNT;
    }
    else if (mode == MODE_POKENAV)
        s->screen = SCR_POKENAV;
    else if (mode == MODE_POKEDEX)
        s->screen = SCR_POKEDEX;
    else if (mode == MODE_STORAGE)
        s->screen = SCR_COUNT;   /* the boxes cover the column */
    StringCopy(s->name, gSaveBlock2Ptr->playerName);
    /* The column's Y and RUN, and the focus ring on it or in the options. */
    s->registered = gSaveBlock1Ptr->registeredItem;
    s->run = (CtrSettings_RunAlways() ? 1 : 0) | (FlagGet(FLAG_SYS_B_DASH) ? 2 : 0);
    s->focus = sFocus;
    s->optFocus = FOCUS_NONE;
    if (sFocus != FOCUS_NONE || sInside == INSIDE_OPTIONS)
        s->blink = (sFrames >> 4) & 1;

    switch (s->mode)
    {
    case MODE_FIELD:
    case MODE_PARTY_MENU:
    case MODE_BAG_MENU:
        switch (s->screen)
        {
        case SCR_MAP:
            s->mapsec = PlayerRegionPosition(&s->cursorX, &s->cursorY);
            s->pickMapsec = sPickMapsec;
            s->pickX = sPickX;
            s->pickY = sPickY;
            /* The hour is always on the bar above the map; the part of the day
             * and the clock's speed only with the day/night clock or SHOW CLOCK
             * on. Off, the clock is not running: the hour is the game's own. */
            s->clockShown = CtrSettings_GetInt("day_night", 0) == 1 || CtrSettings_GetInt("show_clock", 0) == 1;
            {
                unsigned seconds = 0, rate = 1;

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
            break;
        case SCR_POKEMON:
            /* The game's own party menu (opened by the game) is drawn by the
             * compositor; in the field the bottom screen draws the party. */
            if (s->mode == MODE_FIELD)
                SnapshotParty(s);
            break;
        case SCR_CARD:
            SnapshotCard(s);
            break;
        case SCR_BAG:
            /* The game's own bag (from a shop, the PC...) is the compositor's. */
            if (s->mode == MODE_FIELD)
                SnapshotBag(s);
            break;
        case SCR_POKENAV:
            if (s->mode == MODE_FIELD)
                SnapshotNav(s);
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
            for (unsigned row = 0; !FixedOptions() && row < ARRAY_COUNT(s->extras); ++row)
            {
                const CtrExtra *extra = PageExtra(sOptPage, row);

                if (extra)
                    s->extras[row] = ExtraShownValue(extra);
            }
            if (sInside == INSIDE_OPTIONS)
                s->optFocus = sOptFocus;
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
static bool8 Prefetch(const ViewState *s)
{
    sIconBudget = TRUE;
    for (int i = 0; i < PARTY_SIZE && sIconBudget; ++i)
        if (s->party[i].species)
            MonIcon(s->party[i].iconSpecies, s->party[i].deoxys);
    if (sIconBudget && s->registered != ITEM_NONE && s->mode < MODE_BATTLE_INFO)
        ItemIcon(s->registered);
    if (sIconBudget && s->mode == MODE_FIELD && s->screen == SCR_POKEMON && s->partyView == PARTY_SUMMARY
        && s->party[s->partySel].item != ITEM_NONE)
        ItemIcon(s->party[s->partySel].item);
    if (sIconBudget && s->mode == MODE_FIELD && s->screen == SCR_BAG && s->bagSelItem != ITEM_NONE)
        ItemIcon(s->bagSelItem);
    if (sIconBudget && s->mode == MODE_FIELD && s->screen == SCR_POKEMON && s->partyView == PARTY_MAIL
        && s->mail.iconSpecies != SPECIES_NONE)
        MonIcon(s->mail.iconSpecies, FALSE);
    if (sIconBudget && s->mode == MODE_BATTLE_ACTION && s->quickBall != ITEM_NONE)
        ItemIcon(s->quickBall);
    if (sIconBudget && s->mode == MODE_BATTLE_ACTION && s->safari)
        ItemIcon(ITEM_SAFARI_BALL);
    if (sIconBudget)
    {
        sIconBudget = FALSE;
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Drawing: the button column                                               */
/* ------------------------------------------------------------------------ */

enum { PLATE_NORMAL, PLATE_CHOSEN, PLATE_PRESSED, PLATE_OFF };

static void DrawColumnButton(const ViewState *s, int i, u8 state)
{
    const u8 *labels[SCR_COUNT] = {
        Ascii(CTR_TEXT("MAP", "MAPA", "CARTE", "MAPPA")), gText_MenuPokemon, gText_MenuBag, s->name, gText_MenuPokedex, gText_MenuPokenav,
        gText_MenuSave, gText_MenuOption,
    };
    const Icon *icon = &sRes.column[i];
    int y = PLATE_Y(i), dy = state == PLATE_PRESSED;
    bool8 on = state == PLATE_CHOSEN || state == PLATE_PRESSED;
    const PlateLook *look = state == PLATE_OFF ? &sLook.off : on ? &sLook.chosen : &sLook.plate;

    DrawPlate(PLATE_X, y, PLATE_W, PLATE_H, look, PLATE_SOCKET, dy);
    /* The icon's visible pixels centred on the socket; none on a button not
     * available yet. */
    if (icon->tiles && state != PLATE_OFF)
        DrawSpriteCentred(icon->tiles, icon->size, icon->size, 2 * PLATE_X + PLATE_SOCKET, 2 * (y + dy) + 21,
                          icon->pal.c);
    DrawStrIn(&sSmall, labels[i], PLATE_X + PLATE_SOCKET, PLATE_X + PLATE_W - 1, y + 1 + dy, y + PLATE_H - 2 + dy,
              state == PLATE_OFF ? sLook.offText : on ? TXT_WHITE : TXT_DARK,
              state == PLATE_OFF ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LIGHT);
}

/* The RUN button's lamp: lit while running is the default. */
static void DrawLed(int x, int y, bool8 on)
{
    FillRect(x + 1, y, 2, 1, sLook.led[LED_EDGE]);
    FillRect(x + 1, y + 3, 2, 1, sLook.led[LED_EDGE]);
    FillRect(x, y + 1, 1, 2, sLook.led[LED_EDGE]);
    FillRect(x + 3, y + 1, 1, 2, sLook.led[LED_EDGE]);
    FillRect(x + 1, y + 1, 2, 2, sLook.led[on ? LED_ON : LED_OFF]);
    Put(x + 2, y + 1, sLook.led[on ? LED_ON_LIGHT : LED_OFF_LIGHT]);
}

/*
 * Y, the registered item (the 3DS's Y is SELECT): the item's own icon under
 * a red badge like the bag's SEL, grey with nothing registered. RUN: the
 * shoe and its lamp; on, the face takes the socket's green and the shoe its
 * speed lines. Neither is cached: they follow the save and the settings.
 */
static void DrawSquares(const ViewState *s)
{
    for (int k = 0; k < 2; ++k)
    {
        int x = SQUARE_X(k), id = HIT_COLUMN + COL_Y + k;
        bool8 pressed = s->pressed == id;
        bool8 available = k == 0 ? s->registered != ITEM_NONE : (s->run & 2) != 0;
        bool8 on = k == 1 && (s->run & 1);
        int dy = pressed;
        const PlateLook *look = !available ? &sLook.off : pressed ? &sLook.chosen : on ? &sLook.run : &sLook.plate;

        DrawPlate(x, SQUARE_Y, SQUARE_SIZE, SQUARE_SIZE, look, 0, pressed);
        if (k == 0 && available)
        {
            int slot = ItemIcon(s->registered);

            if (slot >= 0)
                DrawSpriteCentred(sItemIcons[slot].tiles, 3, 3, 2 * x + SQUARE_SIZE, 2 * (SQUARE_Y + 23 + dy),
                                  sItemIcons[slot].pal.c);
        }
        if (k == 0)
            DrawSprite(sArtBadgeY, 2, 2, x + 2, SQUARE_Y + 2 + dy, sLook.art.c);
        if (k == 1 && available)
        {
            DrawSpriteCentred(on ? sArtShoeOn : sArtShoeOff, 3, 3, 2 * x + SQUARE_SIZE, 2 * (SQUARE_Y + 23 + dy),
                              sLook.art.c);
            DrawLed(x + SQUARE_SIZE - 7, SQUARE_Y + 3 + dy, on);
        }
        if (s->focus == COL_Y + k)
            DrawRing(x, SQUARE_Y, SQUARE_SIZE, SQUARE_SIZE, s->blink);
        /* The two halves of the column below the groove. */
        if (available)
            AddHit(k == 0 ? COL_X : SQUARE_X(1) - 1, GROOVE_Y + 2,
                   k == 0 ? SQUARE_X(1) - 1 - COL_X : W - SQUARE_X(1) + 1, H - GROOVE_Y - 2, id);
    }
}

/*
 * The column changes far less often than what is beside it: it is kept,
 * unpressed, in every background cache, and a redraw only paints the chosen,
 * pressed and focused buttons over it. The caches are repainted when which
 * entries exist changes, or the player's name does.
 *
 * Painted once, into CACHE_MENU, and copied: the column (x >= COL_X, one run
 * of (W - COL_X) * H in this layout) is the same in CACHE_MENU, CACHE_MAP and
 * CACHE_CARD. Nothing else writes there: BuildMapCache copies the whole of
 * CACHE_MENU and keeps its map and frame left of MAP_WIN_X1 + 3 (235); the
 * card's layers stay left of CW (CARD_X 0, 30 tiles, its pictures inside) and
 * its column is CACHE_MENU's copied (CardCacheStep). Same pixels under the
 * same buttons give the same column, so the copy is what painting each cache
 * gave: the first column at game start painted 32 buttons, now 16. The
 * canvas is still painted: what a section draws there is not bounded (a long
 * line of text may run past CW), so its column is not provably the caches'.
 */
#define COL_PIXELS ((size_t)(W - COL_X) * H)

#ifdef CTR_BOTTOM_CACHE_AUDIT
/* The column painted into a cache as it was before, against the copy. */
static void AuditColumnCopy(const ViewState *s, int which)
{
    static u16 painted[W * H];
    static unsigned same;
    u16 *dst = sDst;

    memcpy(painted, sCache[which], sizeof(painted));
    sDst = painted;
    for (int i = 0; i < SCR_COUNT; ++i)
        DrawColumnButton(s, i, (s->enabled >> i) & 1 ? PLATE_NORMAL : PLATE_OFF);
    sDst = dst;
    if (memcmp(painted + COL_X * H, sCache[CACHE_MENU] + COL_X * H, COL_PIXELS * sizeof(u16)) != 0)
        CtrLog_Write(CTR_LOG_ERROR, "CONTRACT bottom column copied into cache %d differs from painting it", which);
    else
        CtrLog_Write(CTR_LOG_VIDEO, "bottom column copy: same as painting cache %d (%u)", which, ++same);
}
#endif

static void DrawColumn(const ViewState *s)
{
    static int cachedMask = -1;
    static u8 cachedName[PLAYER_NAME_LENGTH + 1];

    if (cachedMask != s->enabled || memcmp(cachedName, s->name, sizeof(cachedName)) != 0)
    {
        static const u8 copies[] = {CACHE_MAP, CACHE_CARD};
        u16 *canvas = sDst;
        int clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};

        cachedMask = s->enabled;
        memcpy(cachedName, s->name, sizeof(cachedName));
        /* The caches are whole pictures, whatever part this redraw is of. */
        sClipX0 = sClipY0 = 0;
        sClipX1 = W;
        sClipY1 = H;
        if (sCache[CACHE_MENU])
        {
            sDst = sCache[CACHE_MENU];
            for (int i = 0; i < SCR_COUNT; ++i)
                DrawColumnButton(s, i, (s->enabled >> i) & 1 ? PLATE_NORMAL : PLATE_OFF);
        }
        for (unsigned k = 0; k < ARRAY_COUNT(copies); ++k)
        {
            int c = copies[k];

            if (!sCache[c])
                continue;
            if (!sCache[CACHE_MENU])
            {
                /* No column to copy from: painted as it always was. */
                sDst = sCache[c];
                for (int i = 0; i < SCR_COUNT; ++i)
                    DrawColumnButton(s, i, (s->enabled >> i) & 1 ? PLATE_NORMAL : PLATE_OFF);
                continue;
            }
#ifdef CTR_BOTTOM_CACHE_AUDIT
            /* A card cache not made yet (or half made) has no column of its
             * own: its last step copies CACHE_MENU's, so it is not compared. */
            if (c != CACHE_CARD || sCardCacheKey != -1)
                AuditColumnCopy(s, c);
#endif
            memcpy(sCache[c] + COL_X * H, sCache[CACHE_MENU] + COL_X * H, COL_PIXELS * sizeof(u16));
        }
        sClipX0 = clip[0];
        sClipY0 = clip[1];
        sClipX1 = clip[2];
        sClipY1 = clip[3];
        sDst = canvas;
        /* The canvas started from the stale column: paint all of it. */
        for (int i = 0; i < SCR_COUNT; ++i)
            DrawColumnButton(s, i, (s->enabled >> i) & 1 ? PLATE_NORMAL : PLATE_OFF);
    }
    for (int i = 0; i < SCR_COUNT; ++i)
    {
        bool8 pressed = s->pressed == HIT_COLUMN + i, chosen = s->screen == i;

        if (!((s->enabled >> i) & 1))
            continue;
        if (pressed || chosen)
            DrawColumnButton(s, i, pressed ? PLATE_PRESSED : PLATE_CHOSEN);
        if (s->focus == i)
            DrawRing(PLATE_X, PLATE_Y(i), PLATE_W, PLATE_H, s->blink);
        AddHit(COL_X, PLATE_Y(i) - 1, W - COL_X, 24, HIT_COLUMN + i);
    }
    DrawSquares(s);
}


/* ------------------------------------------------------------------------ */
/* Drawing: region map                                                    */
/* ------------------------------------------------------------------------ */

/*
 * The map sits in a recessed window on the section's green: a 3px frame
 * (the plates' outline, a white line, a shade line on its top and left).
 * The map keeps its x; the frame hides its first and last columns, the 248px
 * picture's overflow the GBA's own frame hides too. Hoenn's 150 rows are
 * centred in the window. The clock is on a bar above it, the area's name on
 * a plate below it.
 */
#define CLOCK_X 5
#define CLOCK_Y 5
#define CLOCK_W 230
#define CLOCK_H 15
#define MAP_ORIGIN_X 0
#define MAP_ORIGIN_Y 36
#define MAP_WIN_X0 8
#define MAP_WIN_Y0 28
#define MAP_WIN_X1 232
#define MAP_WIN_Y1 195
#define NAME_X 5
#define NAME_Y 203
#define NAME_W 230
#define NAME_H 32

static void DrawMapTile8(u8 tile, int x, int y)
{
    const u8 *src;

    if (!sRes.mapTiles || tile >= sRes.mapTileCount)
        return;
    src = sRes.mapTiles + tile * 64;
    for (int py = 0; py < 8; ++py)
        for (int px = 0; px < 8; ++px)
        {
            /* The map's colours are loaded at palette 7: indices 112 up. */
            u8 v = src[py * 8 + px];
            if (v >= 112 && v < 144)
                Put(x + px, y + py, sRes.mapPal[v - 112]);
        }
}

/* Draws only inside the map's window, and inside the redraw's own clip. */
static void ClipToMapWindow(int saved[4])
{
    saved[0] = sClipX0;
    saved[1] = sClipY0;
    saved[2] = sClipX1;
    saved[3] = sClipY1;
    if (sClipX0 < MAP_WIN_X0) sClipX0 = MAP_WIN_X0;
    if (sClipY0 < MAP_WIN_Y0) sClipY0 = MAP_WIN_Y0;
    if (sClipX1 > MAP_WIN_X1) sClipX1 = MAP_WIN_X1;
    if (sClipY1 > MAP_WIN_Y1) sClipY1 = MAP_WIN_Y1;
}

static void RestoreClip(const int saved[4])
{
    sClipX0 = saved[0];
    sClipY0 = saved[1];
    sClipX1 = saved[2];
    sClipY1 = saved[3];
}

/* The window's frame round [x0, x1) x [y0, y1), its outer edge. */
static void DrawMapFrame(int x0, int y0, int x1, int y1)
{
    int w = x1 - x0, h = y1 - y0;
    const PlateLook *p = &sLook.plate;

    FillRect(x0 + 1, y0, w - 2, 1, p->o);
    FillRect(x0 + 1, y1 - 1, w - 2, 1, p->o);
    FillRect(x0, y0 + 1, 1, h - 2, p->o);
    FillRect(x1 - 1, y0 + 1, 1, h - 2, p->o);
    FillRect(x0 + 1, y0 + 1, w - 2, 1, p->h);
    FillRect(x0 + 1, y1 - 2, w - 2, 1, p->h);
    FillRect(x0 + 1, y0 + 1, 1, h - 2, p->h);
    FillRect(x1 - 2, y0 + 1, 1, h - 2, p->h);
    FillRect(x0 + 2, y1 - 3, w - 4, 1, p->h);
    FillRect(x1 - 3, y0 + 3, 1, h - 5, p->h);
    FillRect(x0 + 2, y0 + 2, w - 4, 1, p->s);
    FillRect(x0 + 2, y0 + 2, 1, h - 4, p->s);
    Put(x0 + 1, y0 + 1, p->o);
    Put(x1 - 2, y0 + 1, p->o);
    Put(x0 + 1, y1 - 2, p->o);
    Put(x1 - 2, y1 - 2, p->o);
}

/* The map picture itself, once, into its cache. */
static void BuildMapCache(void)
{
    int saved[4];

    if (!sCache[CACHE_MAP])
        return;
    memcpy(sCache[CACHE_MAP], sCache[CACHE_MENU], sizeof(sCanvas));
    sDst = sCache[CACHE_MAP];
    ClipToMapWindow(saved);
    FillRect(0, 0, CW, H, sRes.mapPal[0]);
    /* The map is a 64x64 affine map; the ocean around Hoenn is tile 0. */
    for (int ty = (MAP_WIN_Y0 - MAP_ORIGIN_Y) / 8 - 1; ty * 8 + MAP_ORIGIN_Y < MAP_WIN_Y1; ++ty)
        for (int tx = 0; tx < CW / 8; ++tx)
        {
            u8 tile = (sRes.mapMap && ty >= 0 && ty < 64) ? sRes.mapMap[ty * 64 + tx] : 0;
            DrawMapTile8(tile, MAP_ORIGIN_X + tx * 8, MAP_ORIGIN_Y + ty * 8);
        }
    RestoreClip(saved);
    DrawMapFrame(MAP_WIN_X0 - 3, MAP_WIN_Y0 - 3, MAP_WIN_X1 + 3, MAP_WIN_Y1 + 3);
    sDst = sCanvas;
}

static void DrawRegionName(const ViewState *s)
{
    u8 name[32];
    bool8 picked = s->pickMapsec != MAPSEC_NONE;
    u8 mapsec = picked ? s->pickMapsec : s->mapsec;

    if (mapsec == MAPSEC_NONE)
        return;
    GetMapName(name, mapsec, 0);
    DrawPlate(NAME_X, NAME_Y, NAME_W, NAME_H, picked ? &sLook.chosen : &sLook.plate, 0, FALSE);
    DrawStrIn(&sNormal, name, NAME_X + 1, NAME_X + NAME_W - 1, NAME_Y + 1, NAME_Y + NAME_H - 2,
              picked ? TXT_WHITE : TXT_DARK, picked ? sLook.chosenShadow : TXT_LIGHT);
}

static void DrawMapClock(const ViewState *s)
{
    static const char *const phases[] = {
        CTR_TEXT("MORNING", "MAÑANA", "MATIN", "MATTINA"), CTR_TEXT("DAY", "DÍA", "JOUR", "GIORNO"),
        CTR_TEXT("EVENING", "TARDE", "SOIR", "SERA"), CTR_TEXT("NIGHT", "NOCHE", "NUIT", "NOTTE")
    };
    char time[8], speed[8];
    int top = CLOCK_Y, bottom = CLOCK_Y + CLOCK_H;

    if (s->inBattle)
        return;
    DrawPlate(CLOCK_X, top, CLOCK_W, CLOCK_H, &sLook.plate, 0, FALSE);
    if (s->clockValid)
    {
        snprintf(time, sizeof(time), "%02u:%02u", s->clockMinute / 60u, s->clockMinute % 60u);
        snprintf(speed, sizeof(speed), "%uX", s->clockRate);
    }
    else
    {
        strcpy(time, "--:--");
        strcpy(speed, "--");
    }
    DrawStrIn(&sSmall, Ascii(time), CLOCK_X + 4, CLOCK_X + 65, top, bottom, TXT_DARK, TXT_LIGHT);
    if (!s->clockShown)
        return;
    unsigned phase = s->clockPhase == CTR_TIME_NIGHT ? 3u
                   : s->clockPhase == CTR_TIME_MORNING ? 0u
                   : s->clockMinute >= 16u * 60u ? 2u : 1u;
    DrawStrIn(&sSmall, Ascii(s->clockValid ? phases[phase] : "--"),
              CLOCK_X + 66, CLOCK_X + 168, top, bottom, TXT_DARK, TXT_LIGHT);
    DrawStrIn(&sSmall, Ascii(speed), CLOCK_X + 169, CLOCK_X + CLOCK_W - 4, top, bottom, TXT_DARK, TXT_LIGHT);
}

/* The player's mark and a picked cell's cursor, inside the window. */
static void DrawMapMarks(const ViewState *s)
{
    int saved[4];

    ClipToMapWindow(saved);
    if (s->mapsec != MAPSEC_NONE && sRes.playerIcon[s->gender])
        DrawSprite(sRes.playerIcon[s->gender], 2, 2, MAP_ORIGIN_X + s->cursorX * 8 - 4,
                   MAP_ORIGIN_Y + s->cursorY * 8 - 4, sRes.playerIconPal[s->gender].c);
    if (s->pickMapsec != MAPSEC_NONE && sRes.cursorTiles)
        DrawSprite(sRes.cursorTiles, 2, 2, MAP_ORIGIN_X + s->pickX * 8 - 4, MAP_ORIGIN_Y + s->pickY * 8 - 4,
                   sRes.cursorPal.c);
    RestoreClip(saved);
}

static void DrawRegionMap(const ViewState *s)
{
    DrawMapMarks(s);
    AddHit(MAP_WIN_X0, MAP_WIN_Y0, MAP_WIN_X1 - MAP_WIN_X0, MAP_WIN_Y1 - MAP_WIN_Y0, HIT_MAP);
    DrawRegionName(s);
    DrawMapClock(s);
}

/* The section under a tapped map cell, as the region map's cursor finds it. */
static void PickMapCell(int x, int y)
{
    int cx = (x - MAP_ORIGIN_X + 64) / 8 - 8, cy = (y - MAP_ORIGIN_Y + 64) / 8 - 8;

    sPickMapsec = MAPSEC_NONE;
    for (int i = 0; i < MAPSEC_NONE; ++i)
    {
        const struct RegionMapLocation *e = &gRegionMapEntries[i];
        int ex = e->x + 1, ey = e->y + 2;
        if (e->width && cx >= ex && cy >= ey && cx < ex + e->width && cy < ey + e->height)
        {
            sPickMapsec = i;
            sPickX = cx;
            sPickY = cy;
            return;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Drawing: trainer card                                                    */
/* ------------------------------------------------------------------------ */

#define CARD_X 0
#define CARD_Y 40

static void CardPals(Pal *pals, u8 stars, u8 gender)
{
    memcpy(pals, sRes.cardPal[stars], sizeof(Pal) * 3);
    if (gender)
        pals[1] = sRes.cardFemaleBg;
    pals[3] = sRes.badgePal;
    pals[4] = sRes.starPal;
}

/* The card's background stripes and front, by star count and gender, as much
 * of them as the clip holds (sDst the card's cache, sOX 0). Only the tile
 * columns that meet the clip are walked: a tile outside it draws nothing, so
 * the layers come out as a whole drawing's would, in the same order. */
static void DrawCardLayers(u8 stars, u8 gender, const Pal *pals)
{
    int tx0 = sClipX0 / 8, tx1 = (sClipX1 + 7) / 8;
    int fx0 = (sClipX0 - CARD_X) / 8, fx1 = (sClipX1 - CARD_X + 7) / 8;

    if (tx1 > CW / 8) tx1 = CW / 8;
    if (fx0 < 0) fx0 = 0;
    if (fx1 > 30) fx1 = 30;
    FillRect(0, 0, CW, H, pals[0].c[0]);
    if (sRes.cardBg)
        for (int ty = 0; ty < H / 8; ++ty)
            for (int tx = tx0; tx < tx1; ++tx)
                DrawMapEntry(sRes.cardTiles, sRes.cardTileCount, sRes.cardBg[(ty % 20) * 30 + (tx % 30)],
                             tx * 8, ty * 8, pals);
    if (sRes.cardFront)
        for (int ty = 0; ty < 20; ++ty)
            for (int tx = fx0; tx < fx1; ++tx)
                DrawMapEntry(sRes.cardTiles, sRes.cardTileCount, sRes.cardFront[ty * 30 + tx],
                             CARD_X + tx * 8, CARD_Y + ty * 8, pals);
    if (sRes.trainerPic[gender])
        DrawSprite(sRes.trainerPic[gender], 8, 8, CARD_X + 20 * 8, CARD_Y + 5 * 8, sRes.trainerPicPal[gender].c);
    for (int i = 0; i < stars; ++i)
        DrawMapEntry(sRes.cardTiles, sRes.cardTileCount, 0x4000 | 143, CARD_X + (15 + i) * 8, CARD_Y + 7 * 8, pals);
}

/*
 * The card's cache was made in the frame the card first opened: 16.5 ms on an
 * Old 3DS, most of it a copy of the whole screen from CACHE_MENU. Left of the
 * column the flat fill paints every pixel ([0, CW) x [0, H), opaque) before
 * the layers go on, so only the column is copied now. And it is made ahead,
 * on frames the field leaves idle (WarmCardCache): a vertical strip of
 * CARD_STRIP_W columns at a time, every layer within the strip in the order a
 * whole drawing has them, then the column. Strips do not overlap, so together
 * they are the whole drawing. The card opened mid-build finishes the steps
 * left, it never starts again; sCardCacheKey is set only once all are done.
 */
#define CARD_STRIP_W 8
#define CARD_STRIPS (CW / CARD_STRIP_W)   /* then one more step: the column */
static struct
{
    int key;        /* stars * 2 + gender of the card being made, -1 none */
    u8 next;        /* its next step, 0..CARD_STRIPS; past it the card is whole */
    bool8 sliced;   /* some of its steps were taken ahead (CTR_BOTTOM_CACHE_AUDIT) */
} sCardBuild = {-1, 0, FALSE};
/* What the last opening of the card spent finishing its cache, for the log. */
static float sCardBuildMs;

static void CardCacheStep(u8 stars, u8 gender, const Pal *pals, int step)
{
    if (step >= CARD_STRIPS)
    {
        memcpy(sCache[CACHE_CARD] + COL_X * H, sCache[CACHE_MENU] + COL_X * H, (size_t)(W - COL_X) * H * sizeof(u16));
        return;
    }
    sClipX0 = step * CARD_STRIP_W;
    sClipX1 = sClipX0 + CARD_STRIP_W;
    sClipY0 = 0;
    sClipY1 = H;
    DrawCardLayers(stars, gender, pals);
}

#ifdef CTR_BOTTOM_CACHE_AUDIT
/* A card made in strips against the same card made whole, as it was made
 * before: the whole screen copied from CACHE_MENU, then every layer at once.
 * The whole one is what the cache keeps. */
static void AuditCardCache(u8 stars, u8 gender, const Pal *pals)
{
    static u16 sliced[W * H];
    static unsigned same;

    memcpy(sliced, sCache[CACHE_CARD], sizeof(sliced));
    memcpy(sCache[CACHE_CARD], sCache[CACHE_MENU], sizeof(sCanvas));
    sClipX0 = sClipY0 = 0;
    sClipX1 = W;
    sClipY1 = H;
    DrawCardLayers(stars, gender, pals);
    if (memcmp(sliced, sCache[CACHE_CARD], sizeof(sliced)) != 0)
        CtrLog_Write(CTR_LOG_ERROR, "CONTRACT bottom card cache made in strips differs from a whole build");
    else
        CtrLog_Write(CTR_LOG_VIDEO, "bottom card cache: strips same as a whole build (%u)", ++same);
}
#endif

/* Steps of the card's cache, until it is whole or, with a slice (ms from
 * start), until the slice is spent. */
static void CardCacheSteps(u8 stars, u8 gender, uint64_t start, float slice)
{
    Pal pals[5];
    u16 *dst = sDst;
    int key = stars * 2 + gender, ox = sOX, clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};

    if (sCardBuild.key != key)
    {
        /* Another card: from its first step the cache holds no card at all. */
        sCardCacheKey = -1;
        sCardBuild.key = key;
        sCardBuild.next = 0;
        sCardBuild.sliced = FALSE;
    }
    if (slice > 0.0f)
        sCardBuild.sliced = TRUE;
    CardPals(pals, stars, gender);
    sDst = sCache[CACHE_CARD];
    sOX = 0;
    while (sCardBuild.next <= CARD_STRIPS
        && (slice <= 0.0f || CtrPlatform_TickMs(CtrPlatform_Ticks() - start) < slice))
        CardCacheStep(stars, gender, pals, sCardBuild.next++);
    if (sCardBuild.next > CARD_STRIPS)
    {
#ifdef CTR_BOTTOM_CACHE_AUDIT
        if (sCardBuild.sliced)
            AuditCardCache(stars, gender, pals);
#endif
        sCardCacheKey = key;
    }
    sDst = dst;
    sOX = ox;
    sClipX0 = clip[0];
    sClipY0 = clip[1];
    sClipX1 = clip[2];
    sClipY1 = clip[3];
}

/* The card's cache for these stars and gender, whole: what is left of it. */
static void BuildCardCache(u8 stars, u8 gender)
{
    uint64_t start = CtrPlatform_Ticks();

    if (!sCache[CACHE_CARD] || sCardCacheKey == stars * 2 + gender)
        return;
    CardCacheSteps(stars, gender, start, 0.0f);
    sCardBuildMs = CtrPlatform_TickMs(CtrPlatform_Ticks() - start);
}

static void DrawTrainerCard(const ViewState *s)
{
    u8 text[32];
    int bx = CARD_X + 8, by = CARD_Y + 8; /* WIN_CARD_TEXT is at tile (1,1) */

    /* trainer_card.c: PrintNameOnCardFront, PrintIdOnCard, PrintMoneyOnCard,
     * PrintPokedexOnCard, PrintTimeOnCard, DrawStarsAndBadgesOnCard. */
    StringCopy(StringCopy(text, gText_TrainerCardName), s->name);
    DrawStr(&sNormal, text, bx + 16, by + 33, TXT_DARK, TXT_LIGHT);

    StringCopy(StringCopy(text, gText_TrainerCardIDNo), Number(s->id, 5, STR_CONV_MODE_LEADING_ZEROS));
    DrawStr(&sNormal, text, bx + 120 + (96 - StrWidth(&sNormal, text)) / 2, by + 9, TXT_DARK, TXT_LIGHT);

    DrawStr(&sNormal, gText_TrainerCardMoney, bx + 16, by + 57, TXT_DARK, TXT_LIGHT);
    text[0] = CHAR_CURRENCY;
    StringCopy(text + 1, Number(s->money, 6, STR_CONV_MODE_LEFT_ALIGN));
    DrawStrRight(&sNormal, text, bx + 128, by + 57, TXT_DARK, TXT_LIGHT);

    if (s->hasDex)
    {
        DrawStr(&sNormal, gText_TrainerCardPokedex, bx + 16, by + 73, TXT_DARK, TXT_LIGHT);
        DrawStrRight(&sNormal, Number(s->dex, 3, STR_CONV_MODE_LEFT_ALIGN), bx + 128, by + 73, TXT_DARK, TXT_LIGHT);
    }

    DrawStr(&sNormal, gText_TrainerCardTime, bx + 16, by + 89, TXT_DARK, TXT_LIGHT);
    {
        int colon = StrWidth(&sNormal, gText_Colon2), x = bx + 128 - (colon + 30);
        DrawStr(&sNormal, Number(s->hours, 3, STR_CONV_MODE_RIGHT_ALIGN), x, by + 89, TXT_DARK, TXT_LIGHT);
        DrawStr(&sNormal, gText_Colon2, x + 18, by + 89, TXT_DARK, TXT_LIGHT);
        DrawStr(&sNormal, Number(s->minutes, 2, STR_CONV_MODE_LEADING_ZEROS), x + 18 + colon, by + 89, TXT_DARK,
                TXT_LIGHT);
    }

    if (sRes.badgeTiles)
        for (int i = 0; i < NUM_BADGES; ++i)
        {
            int x = CARD_X + (4 + 3 * i) * 8, y = CARD_Y + 15 * 8;
            if (!(s->badges & (1 << i)))
                continue;
            DrawTile(sRes.badgeTiles + (2 * i) * 32, x, y, sRes.badgePal.c, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 1) * 32, x + 8, y, sRes.badgePal.c, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 16) * 32, x, y + 8, sRes.badgePal.c, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 17) * 32, x + 8, y + 8, sRes.badgePal.c, FALSE, FALSE);
        }
}

/* ------------------------------------------------------------------------ */
/* Drawing: save, options, PokéNav                                          */
/* ------------------------------------------------------------------------ */

static const u8 *OptionValue(int row, u8 value)
{
    static u8 frame[16];

    switch (row)
    {
    case 0: return value == 0 ? gText_TextSpeedSlow : value == 1 ? gText_TextSpeedMid : gText_TextSpeedFast;
    case 1: return value ? gText_BattleSceneOff : gText_BattleSceneOn;
    case 2: return value ? gText_BattleStyleSet : gText_BattleStyleShift;
    case 3: return value ? gText_SoundStereo : gText_SoundMono;
    case 4: return value == 0 ? gText_ButtonTypeNormal : value == 1 ? gText_ButtonTypeLR : gText_ButtonTypeLEqualsA;
    case OPT_FPS:
    case OPT_VOXEL: return value ? gText_BattleSceneOn : gText_BattleSceneOff;
    case OPT_VOXEL_BATTLE: return value ? gText_BattleSceneOn : gText_BattleSceneOff;
    case OPT_VOXEL_PITCH: return Number(value, 2, STR_CONV_MODE_LEFT_ALIGN);
    case OPT_VOXEL_BLUR:
    case OPT_VOXEL_ZOOM:
        /* Percentages; the blur's strength is OFF at 0 (the zoom never is). */
        if (value == 0)
            return gText_BattleSceneOff;
        StringCopy(frame, Number(value, 3, STR_CONV_MODE_LEFT_ALIGN));
        frame[StringLength(frame) + 1] = EOS;
        frame[StringLength(frame)] = CHAR_PERCENT;
        return frame;
    default:
        StringCopy(frame, gText_FrameType);
        StringAppend(frame, Number(value + 1, 2, STR_CONV_MODE_LEFT_ALIGN));
        return frame;
    }
}

/* A window frame as the game draws its message boxes, from its 3x3 tiles,
 * round [x, x + 8 wt) x [y, y + 8 ht) filled with `fill`. */
static void DrawWindowFrame(u8 type, int x, int y, int wt, int ht, u16 fill)
{
    const struct TilesPal *frame = GetWindowFrameTilesPal(type);
    const u8 *tiles = frame ? Port_ResolveAssetPointer(frame->tiles) : NULL;
    const u16 *raw = frame ? Port_ResolveAssetPointer(frame->pal) : NULL;
    Pal pal;

    if (!tiles || !raw)
        return;
    ToPals(&pal, raw, 1);
    FillRect(x, y, wt * 8, ht * 8, fill);
    for (int ty = -1; ty <= ht; ++ty)
        for (int tx = -1; tx <= wt; ++tx)
        {
            int row = ty < 0 ? 0 : ty == ht ? 2 : 1, col = tx < 0 ? 0 : tx == wt ? 2 : 1;
            if (row == 1 && col == 1)
                continue;
            DrawTile(tiles + (row * 3 + col) * 32, x + tx * 8, y + ty * 8, pal.c, FALSE, FALSE);
        }
}

/*
 * SAVE: the question in the window frame the player chose (OPTIONS), the
 * file as one big plate of the column's look - the trainer's picture in a
 * tinted socket, name, ID, where the player is, the Pokédex, the time, the
 * badges and the party's icons - and the answers as plates below.
 */
#define SAVE_MSG_X 8
#define SAVE_MSG_Y 8
#define SAVE_FILE_Y 51
#define SAVE_PIC_X 18
#define SAVE_PIC_Y 59
#define SAVE_PIC_W 72
#define SAVE_PIC_H 76
#define SAVE_INFO_X0 98
#define SAVE_INFO_X1 224
#define SAVE_PARTY_Y 151
#define SAVE_BUTTON_Y 201
#define SAVE_BUTTON_H 34

static int StrLines(const u8 *str)
{
    int lines = 1;

    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
        if (g == 0xFFFE)
            ++lines;
    return lines;
}

/* A label plate: the column's plate, green when it is the answer the game
 * expects, grey while it cannot be chosen. */
static void DrawSavePlate(int x, int w, const u8 *label, bool8 chosen, bool8 pressed, bool8 enabled, u8 id)
{
    const PlateLook *look = !enabled ? &sLook.off : chosen ? &sLook.chosen : &sLook.plate;
    u16 fg = !enabled ? sLook.offText : chosen ? TXT_WHITE : TXT_DARK;
    u16 sh = !enabled ? sLook.offShadow : chosen ? sLook.chosenShadow : TXT_LIGHT;

    DrawPlate(x, SAVE_BUTTON_Y, w, SAVE_BUTTON_H, look, 0, pressed && enabled);
    DrawStrCentered(&sNormal, label, x + w / 2, SAVE_BUTTON_Y + 9 + (pressed && enabled), fg, sh);
    if (enabled)
        AddHit(x, SAVE_BUTTON_Y, w, SAVE_BUTTON_H, id);
}

static void DrawSaveFile(const ViewState *s)
{
    u8 text[32];
    const PlateLook *p = &sLook.plate;
    int px = SAVE_PIC_X, py = SAVE_PIC_Y, pw = SAVE_PIC_W, ph = SAVE_PIC_H, x0 = SAVE_INFO_X0, x1 = SAVE_INFO_X1;
    int floorY = py + ph - 14, feetX = px + pw / 2, feetY = py + ph - 16;

    DrawPlate(8, SAVE_FILE_Y, 224, 138, p, 0, FALSE);

    /* The picture's socket, its floor and the trainer's shadow on it. */
    FillRect(px + 1, py, pw - 2, ph, p->u);
    FillRect(px, py + 1, pw, ph - 2, p->u);
    FillRect(px + 1, py + 1, pw - 2, ph - 2, p->t);
    FillRect(px + 1, floorY, pw - 2, 13, sLook.section[SECTION_BALL]);
    FillRect(px + 1, floorY, pw - 2, 1, p->u);
    for (int y = -3; y <= 3; ++y)
        for (int x = -22; x <= 22; ++x)
            if (49 * x * x + 1936 * y * y < 23716)
                Put(feetX + x, feetY + y, p->u);
    if (sRes.trainerPic[s->gender])
        DrawSprite(sRes.trainerPic[s->gender], 8, 8, px + 4, py + 2, sRes.trainerPicPal[s->gender].c);

    /* Name and ID, a rule, then where, the Pokédex and the time. */
    DrawStr(&sNormal, s->name, x0, 58, TXT_DARK, TXT_LIGHT);
    StringCopy(StringCopy(text, gText_TrainerCardIDNo), Number(s->id, 5, STR_CONV_MODE_LEADING_ZEROS));
    DrawStrRight(&sNormal, text, x1, 58, TXT_DARK, TXT_LIGHT);
    FillRect(x0, 74, x1 - x0, 1, p->s);
    if (s->mapsec != MAPSEC_NONE)
    {
        GetMapName(text, s->mapsec, 0);
        DrawStr(&sNormal, text, x0, 76, TXT_BLUE, TXT_LBLUE);
    }
    if (s->hasDex)
    {
        DrawStr(&sNormal, gText_TrainerCardPokedex, x0, 92, TXT_DARK, TXT_LIGHT);
        DrawStrRight(&sNormal, Number(s->dex, 3, STR_CONV_MODE_LEFT_ALIGN), x1, 92, TXT_DARK, TXT_LIGHT);
    }
    DrawStr(&sNormal, gText_TrainerCardTime, x0, 108, TXT_DARK, TXT_LIGHT);
    {
        int colon = StrWidth(&sNormal, gText_Colon2), x = x1 - (colon + 12);

        DrawStrRight(&sNormal, Number(s->hours, 3, STR_CONV_MODE_LEFT_ALIGN), x, 108, TXT_DARK, TXT_LIGHT);
        DrawStr(&sNormal, gText_Colon2, x, 108, TXT_DARK, TXT_LIGHT);
        DrawStr(&sNormal, Number(s->minutes, 2, STR_CONV_MODE_LEADING_ZEROS), x + colon, 108, TXT_DARK, TXT_LIGHT);
    }

    /* The badges: won ones as the card has them, the rest as sockets. */
    if (sRes.badgeTiles)
    {
        Pal socket;

        for (int i = 0; i < 16; ++i)
            socket.c[i] = p->s;
        for (int i = 0; i < NUM_BADGES; ++i)
        {
            const u16 *pal = (s->badges & (1 << i)) ? sRes.badgePal.c : socket.c;
            int x = x0 - 2 + i * 16, y = 124;

            DrawTile(sRes.badgeTiles + (2 * i) * 32, x, y, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 1) * 32, x + 8, y, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 16) * 32, x, y + 8, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 17) * 32, x + 8, y + 8, pal, FALSE, FALSE);
        }
    }

    /* The party, under a rule: each icon on a little floor; an empty place
     * is a dot. */
    FillRect(16, SAVE_PARTY_Y - 5, 208, 1, p->s);
    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        const MonView *mon = &s->party[i];
        int x = 18 + i * 34, y = SAVE_PARTY_Y;

        FillRect(x + 3, y + 26, 26, 3, p->t);
        if (mon->species)
        {
            const u8 *tiles = MonIcon(mon->iconSpecies, mon->deoxys);
            u8 pal = gMonIconPaletteIndices[mon->iconSpecies];

            if (tiles && pal < 3)
                DrawSprite(tiles, 4, 4, x, y - 2, sRes.monIconPal[pal].c);
        }
        else
        {
            for (int dy = -5; dy <= 5; ++dy)
                for (int dx = -5; dx <= 5; ++dx)
                    if (dx * dx + dy * dy <= 25)
                        Put(x + 16 + dx, y + 15 + dy, p->s);
        }
    }
}

/* The status label of the game's party menu and summary: PSN, PAR, ... FNT. */
static void DrawAilment(u8 ailment, int x, int y)
{
    if (ailment != AILMENT_NONE && ailment <= AILMENT_FNT && sRes.statusTiles)
        DrawSprite(sRes.statusTiles + (ailment - 1) * 4 * 32, 4, 1, x, y, sRes.statusPal.c);
}

static const u8 *LevelText(u8 level)
{
    static u8 text[12];

    StringCopy(StringCopy(text, gText_LevelSymbol), Number(level, 3, STR_CONV_MODE_LEFT_ALIGN));
    return text;
}

/*
 * The party list is the game's party menu, laid out as party_menu.c does on
 * the GBA's 240x160 (the SINGLE layout), PARTY_Y0 down so it sits in the
 * middle of the 240 lines: the first Pokémon's big slot on the left, five
 * wide ones on the right, the message box and CANCEL under them. Slots,
 * colours, fonts and positions are the game's.
 */
#define PARTY_Y0 40

static void SetPartyColor(Pal *pal, u8 offset, u8 id)
{
    pal->c[offset] = Rgb565(sRes.partyRaw[id]);
}

/* party_menu.c's LoadPartyBoxPalette for the states shown here: the slot
 * being moved (PARTY_PAL_SWITCHING), the one under the finger (the cursor's,
 * PARTY_PAL_SELECTED), fainted, normal, or no Pokémon. */
static void PartyBoxPalette(Pal *pal, const MonView *m, bool8 selected, bool8 switching)
{
    static const u8 offsets1[] = {4, 5, 6}, offsets2[] = {1, 7, 8};
    static const u8 normal1[] = {52, 53, 54}, normal2[] = {49, 55, 56};
    static const u8 sel1[] = {116, 117, 118}, sel2[] = {97, 103, 104};
    static const u8 faint1[] = {84, 85, 86}, faint2[] = {81, 87, 88};
    static const u8 selFaint1[] = {148, 149, 150};
    static const u8 action1[] = {100, 101, 102}, action2[] = {161, 167, 168};
    static const u8 noMon[] = {17, 27, 28}, noMonOffsets[] = {1, 11, 12};
    const u8 *ids1, *ids2;

    if (!m->species)
    {
        for (int i = 0; i < 3; ++i)
            SetPartyColor(pal, noMonOffsets[i], noMon[i]);
        return;
    }
    if (switching)
        ids1 = action1, ids2 = selected ? sel2 : action2;
    else if (m->fainted)
        ids1 = selected ? selFaint1 : faint1, ids2 = selected ? sel2 : faint2;
    else
        ids1 = selected ? sel1 : normal1, ids2 = selected ? sel2 : normal2;
    for (int i = 0; i < 3; ++i)
    {
        SetPartyColor(pal, offsets1[i], ids1[i]);
        SetPartyColor(pal, offsets2[i], ids2[i]);
    }
}

/* sPartyBoxInfoRects: nickname, level, gender, HP, max HP and HP bar, from
 * the slot's top left. */
typedef struct
{
    u8 nameX, nameY, levelX, levelY, genderX, genderY, hpX, hpY, maxHpX, maxHpY, barX, barY;
} SlotLayout;

static const SlotLayout sMainLayout = {24, 11, 32, 20, 64, 20, 38, 37, 53, 37, 24, 35};
static const SlotLayout sWideLayout = {22, 3, 30, 12, 62, 12, 102, 12, 117, 12, 88, 10};

/* sPartyMenuSpriteCoords (SINGLE): the centres of the icon, the held item
 * mark and the status. */
static const u8 sSlotSprites[PARTY_SIZE][6] = {
    {16, 40, 20, 50, 50, 52},
    {104, 18, 108, 28, 136, 27},
    {104, 42, 108, 52, 136, 51},
    {104, 66, 108, 76, 136, 75},
    {104, 90, 108, 100, 136, 99},
    {104, 114, 108, 124, 136, 123},
};

/* The party menu's HP bar: 48 pixels in the bar colours of party_menu.c. */
static void DrawPartyHpBar(int x, int y, u16 hp, u16 maxHp, const Pal *pal)
{
    static const u8 green[] = {57, 58}, yellow[] = {73, 74}, red[] = {89, 90};
    const u8 *ids;
    int fill;

    if (maxHp == 0)
        return;
    fill = hp * 48 / maxHp;
    if (hp > 0 && fill == 0)
        fill = 1;
    ids = hp * 2 > maxHp ? green : hp * 5 > maxHp ? yellow : red;
    FillRect(x, y, fill, 1, Rgb565(sRes.partyRaw[ids[1]]));
    FillRect(x, y + 1, fill, 2, Rgb565(sRes.partyRaw[ids[0]]));
    FillRect(x + fill, y, 48 - fill, 1, pal->c[0x0D]);
    FillRect(x + fill, y + 1, 48 - fill, 2, pal->c[0x02]);
}

static void DrawPartySlot(const ViewState *s, int i)
{
    const MonView *m = &s->party[i];
    bool8 main = i == 0;
    const SlotLayout *l = main ? &sMainLayout : &sWideLayout;
    const u8 *map, *sprites = sSlotSprites[i];
    int x = main ? 8 : 96, y = PARTY_Y0 + (main ? 24 : 8 + (i - 1) * 24), w = main ? 10 : 18, h = main ? 7 : 3;
    Pal pal = sRes.partyPal[main ? 3 : 4];
    u16 fg, sh;

    const u8 *desc = m->species ? s->partyDesc[i] : NULL;

    if (main)
        map = (m->isEgg || desc) && sRes.slotMainNoHp ? sRes.slotMainNoHp : sRes.slotMain;
    else if (!m->species)
        map = sRes.slotWideEmpty;
    else
        map = (m->isEgg || desc) && sRes.slotWideNoHp ? sRes.slotWideNoHp : sRes.slotWide;
    if (!map)
        return;
    PartyBoxPalette(&pal, m, s->pressed == HIT_SLOT + i || (s->partyAct != PACT_NONE && s->partyAct != PACT_USE
                                                            && s->partyAct != PACT_GIVE && s->partySel == i),
                    s->partySwap == i);
    for (int ty = 0; ty < h; ++ty)
        for (int tx = 0; tx < w; ++tx)
            DrawTile(sRes.partyTiles + map[ty * w + tx] * 32, x + tx * 8, y + ty * 8, pal.c, FALSE, FALSE);
    if (!m->species)
        return;

    /* Text as sFontColorTable's default and gender entries. */
    fg = pal.c[TEXT_COLOR_LIGHT_GRAY];
    sh = pal.c[TEXT_COLOR_DARK_GRAY];
    DrawStr(&sSmall, m->nick, x + l->nameX, y + l->nameY, fg, sh);
    if (!m->isEgg)
    {
        u8 text[16], *end;

        DrawStr(&sSmall, LevelText(m->level), x + l->levelX, y + l->levelY, fg, sh);
        if (m->gender == MON_MALE || m->gender == MON_FEMALE)
        {
            u8 color = m->gender == MON_MALE ? 59 : 75;

            DrawStr(&sSmall, m->gender == MON_MALE ? gText_MaleSymbol : gText_FemaleSymbol, x + l->genderX,
                    y + l->genderY, Rgb565(sRes.partyRaw[color]), Rgb565(sRes.partyRaw[color + 1]));
        }
        /* DisplayPartyPokemonDescriptionText, at sPartyBoxInfoRects' place. */
        if (desc)
        {
            DrawStr(&sNormal, desc, x + (main ? 12 : 77), y + (main ? 34 : 4), fg, sh);
            goto icon;
        }
        /* DisplayPartyPokemonHP and ...MaxHP both print the slash. */
        StringCopy(text, Number(m->hp, 3, STR_CONV_MODE_RIGHT_ALIGN));
        end = text + StringLength(text);
        end[0] = CHAR_SLASH;
        end[1] = EOS;
        DrawStr(&sSmall, text, x + l->hpX, y + l->hpY, fg, sh);
        StringCopy(StringCopy(text, gText_Slash), Number(m->maxHp, 3, STR_CONV_MODE_RIGHT_ALIGN));
        DrawStr(&sSmall, text, x + l->maxHpX, y + l->maxHpY, fg, sh);
        DrawPartyHpBar(x + l->barX, y + l->barY, m->hp, m->maxHp, &pal);
icon:
        DrawAilment(m->ailment, sprites[4] - 16, PARTY_Y0 + sprites[5] - 4);
    }
    /* The icon, then its held item mark, as the game's sprites. */
    {
        const u8 *tiles = MonIcon(m->iconSpecies, m->deoxys);
        u8 iconPal = gMonIconPaletteIndices[m->iconSpecies];

        if (tiles && iconPal < 3)
            DrawSprite(tiles, 4, 4, sprites[0] - 16, PARTY_Y0 + sprites[1] - 16, sRes.monIconPal[iconPal].c);
        if (m->item != ITEM_NONE && sRes.holdTiles)
            DrawTile(sRes.holdTiles + (ItemIsMail(m->item) ? 32 : 0), sprites[2] - 4, PARTY_Y0 + sprites[3] - 4,
                     sRes.holdPal.c, FALSE, FALSE);
    }
    AddHit(main ? 0 : x, y, main ? x + w * 8 : w * 8, h * 8, HIT_SLOT + i);
}

static void DrawGameButton(int x, int y, const u8 *label, bool8 pressed, bool8 enabled, u8 id);

/*
 * The actions, a message, a question or Soft-Boiled's target in the bands
 * the GBA's layout leaves free above and below the list, rather than the
 * game's boxes over it: the message in the top band (WIN_MSG's width, one
 * line or two), the actions as the CANCEL art in two rows in the bottom
 * one - the field moves (four at most, as a Pokémon's moves) in the field
 * moves' blue, then SUMMARY, SWITCH, ITEM and CANCEL.
 */
#define TRAY_ROW1_Y 182
#define TRAY_ROW2_Y 210
#define TRAY_X(i) (4 + (i) * 59)

static void DrawButtonArt(int x, int y, int wt, int h);
static void ButtonText(bool8 enabled, u16 *fg, u16 *sh);

static void DrawTrayButtonW(int x, int y, int wt, const u8 *label, bool8 pressed, bool8 enabled, bool8 move, u8 id)
{
    const Font *font = StrWidth(&sSmall, label) <= wt * 8 - 4 ? &sSmall : &sNarrow;
    u16 fg, sh;

    pressed = pressed && enabled;
    DrawButtonArt(x, y + pressed, wt, 16);
    ButtonText(enabled, &fg, &sh);
    if (move && enabled)
        fg = TXT_LBLUE, sh = TXT_BLUE;
    DrawStrCentered(font, label, x + wt * 4, y + (font == &sSmall) + pressed, fg, sh);
    if (enabled)
        AddHit(x - 1, y - 6, wt * 8 + 2, 28, id);
}

static void DrawTrayButton(int x, int y, const u8 *label, bool8 pressed, bool8 enabled, bool8 move, u8 id)
{
    DrawTrayButtonW(x, y, 7, label, pressed, enabled, move, id);
}

/* A move as a wide button: its name, and its PP at the right. */
static void DrawMoveButton(int x, int y, const ViewState *s, int i, u8 id)
{
    bool8 enabled = s->moves[i] != MOVE_NONE, pressed = enabled && s->pressed == id;
    u8 pp[12], *end;
    u16 fg, sh;

    DrawButtonArt(x, y + pressed, 14, 16);
    ButtonText(enabled, &fg, &sh);
    if (!enabled)
        return;
    DrawStr(&sSmall, gMoveNames[s->moves[i]], x + 6, y + 1 + pressed, fg, sh);
    end = StringCopy(pp, Number(s->pp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
    *end++ = CHAR_SLASH;
    StringCopy(end, Number(s->maxPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
    DrawStrRight(&sSmall, pp, x + 106, y + 1 + pressed, fg, sh);
    AddHit(x, y - 3, 112, 22, id);
}

/* The level-up stats (DrawLevelUpWindowPg1, then ...Pg2) in a box under the
 * party: what each rose by, then what it is. */
static void DrawLevelUpStats(const ViewState *s)
{
    static const u8 order[NUM_STATS] = {STAT_HP, STAT_ATK, STAT_DEF, STAT_SPATK, STAT_SPDEF, STAT_SPEED};
    const u8 *const names[NUM_STATS] = {gText_MaxHP, gText_Attack, gText_Defense, gText_SpAtk, gText_SpDef, gText_Speed};

    DrawWindowFrame(s->frame, 8, 176, 28, 6, TXT_WHITE);
    for (int i = 0; i < NUM_STATS; ++i)
    {
        int x = i < 3 ? 10 : 124, y = 177 + (i % 3) * 16;
        s16 before = s->partyStats[order[i]], after = s->partyStats[NUM_STATS + order[i]];
        u8 value[12];

        DrawStr(&sNormal, names[i], x, y, TXT_DARK, TXT_LIGHT);
        if (s->partyStatsPage == 0)
            StringCopy(StringCopy(value, after >= before ? gText_Plus : gText_Dash),
                       Number(after >= before ? after - before : before - after, 2, STR_CONV_MODE_LEFT_ALIGN));
        else
            StringCopy(value, Number(after, 3, STR_CONV_MODE_RIGHT_ALIGN));
        DrawStrRight(&sNormal, value, x + 100, y, TXT_DARK, TXT_LIGHT);
    }
}

static void DrawTopMessage(const ViewState *s, const u8 *text)
{
    bool8 two = StrLines(text) > 1;
    int y = two ? 6 : 14;

    DrawWindowFrame(s->frame, 8, y, 28, two ? 4 : 2, TXT_WHITE);
    DrawStr(&sNormal, text, 8, y + 1, TXT_DARK, TXT_LIGHT);
}

static void DrawPartyActions(const ViewState *s)
{
    static const u8 row2[] = {PACTION_SUMMARY, PACTION_SWITCH, PACTION_ITEM, PACTION_CANCEL};
    u8 text[64];
    int moves = 0;

    StringExpandPlaceholders(text, gText_DoWhatWithPokemon);
    DrawTopMessage(s, text);
    for (int i = 0; i < s->partyActCount; ++i)
    {
        u8 action = s->partyActions[i];

        if (action >= PACTION_FIELD_MOVE)
            DrawTrayButton(TRAY_X(moves++), TRAY_ROW1_Y,
                           gMoveNames[CtrParty_FieldMoveId(action - PACTION_FIELD_MOVE)],
                           s->pressed == HIT_PARTY_ACTION + i, TRUE, TRUE, HIT_PARTY_ACTION + i);
    }
    /* SWITCH needs another Pokémon; an egg holds nothing; MAIL isn't here. */
    for (int k = 0; k < 4; ++k)
    {
        int i = 0;

        while (i < s->partyActCount && s->partyActions[i] != row2[k])
            ++i;
        if (k == 2 && i == s->partyActCount)
            for (i = 0; i < s->partyActCount && s->partyActions[i] != PACTION_MAIL; ++i)
                ;
        DrawTrayButton(TRAY_X(k), TRAY_ROW2_Y,
                       k == 0   ? gText_Summary5
                       : k == 1 ? gText_Switch2
                       : k == 2 ? (i < s->partyActCount ? gText_Mail : gText_Item)
                                : gText_Cancel2,
                       s->pressed == HIT_PARTY_ACTION + i, i < s->partyActCount, FALSE, HIT_PARTY_ACTION + i);
    }
}

static void DrawPartyList(const ViewState *s)
{
    DrawPartyBackground(CW / 8, H / 8);
    for (int i = 0; i < PARTY_SIZE; ++i)
        DrawPartySlot(s, i);

    switch (s->partyAct)
    {
    case PACT_MENU:
        DrawPartyActions(s);
        return;
    case PACT_MESSAGE:
        /* A tap anywhere, or OK, closes it. */
        DrawTopMessage(s, s->partyMsg);
        AddHit(0, 0, CW, H, HIT_PARTY_MESSAGE);
        DrawTrayButton(TRAY_X(1) + 30, TRAY_ROW1_Y, gText_Ok2, s->pressed == HIT_PARTY_MESSAGE, TRUE, FALSE,
                       HIT_PARTY_MESSAGE);
        return;
    case PACT_ITEM:
    case PACT_MAIL:
        DrawTopMessage(s, s->partyAct == PACT_MAIL ? gText_DoWhatWithMail : gText_DoWhatWithItem);
        DrawTrayButton(TRAY_X(1), TRAY_ROW1_Y, s->partyAct == PACT_MAIL ? gText_Read2 : gMenuText_Give,
                       s->pressed == HIT_PARTY_YES, TRUE, FALSE, HIT_PARTY_YES);
        DrawTrayButton(TRAY_X(2), TRAY_ROW1_Y, s->partyAct == PACT_MAIL ? gText_Take2 : gText_Take,
                       s->pressed == HIT_PARTY_NO, TRUE, FALSE, HIT_PARTY_NO);
        DrawTrayButton(TRAY_X(3), TRAY_ROW2_Y, gText_Cancel2, s->pressed == HIT_PARTY_BACK, TRUE, FALSE,
                       HIT_PARTY_BACK);
        return;
    case PACT_ASK:
        DrawTopMessage(s, s->partyMsg);
        if (s->partyMore)
        {
            DrawTrayButton(TRAY_X(1) + 30, TRAY_ROW1_Y, gText_Ok2, s->pressed == HIT_PARTY_MESSAGE, TRUE, FALSE,
                           HIT_PARTY_MESSAGE);
            return;
        }
        DrawTrayButton(TRAY_X(1), TRAY_ROW1_Y, gText_Yes, s->pressed == HIT_PARTY_YES, TRUE, FALSE, HIT_PARTY_YES);
        DrawTrayButton(TRAY_X(2), TRAY_ROW1_Y, gText_No, s->pressed == HIT_PARTY_NO, TRUE, FALSE, HIT_PARTY_NO);
        return;
    case PACT_WRITING:
        return;
    case PACT_PICK_MOVE:
        DrawTopMessage(s, s->partyMsg);
        for (int i = 0; i < MAX_MON_MOVES; ++i)
            DrawMoveButton(i & 1 ? 124 : 4, 176 + (i / 2) * 22, s, i, HIT_PARTY_MOVE + i);
        DrawTrayButton(TRAY_X(3), 220, gText_Cancel2, s->pressed == HIT_PARTY_BACK, TRUE, FALSE, HIT_PARTY_BACK);
        return;
    case PACT_STATS:
        DrawTopMessage(s, s->partyMsg);
        DrawLevelUpStats(s);
        AddHit(0, 0, CW, H, HIT_PARTY_MESSAGE);
        return;
    case PACT_USE:
    case PACT_GIVE:
    case PACT_TARGET:
        DrawTopMessage(s, s->partyMsg);
        DrawTrayButton(TRAY_X(3), TRAY_ROW2_Y, gText_Cancel2, s->pressed == HIT_PARTY_BACK, TRUE, FALSE,
                       HIT_PARTY_BACK);
        return;
    }

    /* The message box (sDefaultPartyMsgWindowTemplate) in the player's frame. */
    DrawWindowFrame(s->frame, 8, PARTY_Y0 + 136, 21, 2, TXT_WHITE);
    DrawStr(&sNormal, s->partySwap != PARTY_NO_SWAP ? gText_MoveToWhere : gText_ChoosePokemon, 8, PARTY_Y0 + 137,
            TXT_DARK, TXT_LIGHT);

    /* CANCEL: the game's button at tiles (23, 18). */
    DrawGameButton(184, PARTY_Y0 + 144, gText_Cancel, s->pressed == HIT_PARTY_BACK, TRUE, HIT_PARTY_BACK);
}

/*
 * The summary is the game's summary screen as pokemon_summary_screen.c draws
 * it on the GBA's 240x160, at the top of the bottom screen: the INFO page's
 * tilemap under every page (its left panel holds the picture, the ball, the
 * name and level), the page's own over it, the text windows of
 * sSummaryTemplate and the page templates, and its sprites. Under it, the
 * party menu's background and CANCEL button art for the taps that take the
 * place of the keys: the page, the Pokémon, back, and SWITCH.
 */

/* sTextColors: background, text and shadow, in the window's palette. */
static const u8 sSumColors[][3] = {
    {0, 1, 2}, {0, 3, 4}, {0, 5, 6}, {0, 7, 8}, {0, 9, 10}, {0, 11, 12}, {0, 13, 14},
    {0, 7, 8}, {13, 15, 14}, {0, 1, 2}, {0, 3, 4}, {0, 5, 6}, {0, 7, 8},
};

/* Text as a summary window prints it, in window palette `palNum` and colour
 * `colorId`; {COLOR} and {SHADOW} codes change them as they do there. */
static int DrawSumText(const u8 *str, int x, int y, u8 palNum, u8 colorId)
{
    const u16 *pal = sRes.sumPal[palNum].c;
    u8 fg = sSumColors[colorId][1], sh = sSumColors[colorId][2];
    int left = x;

    if (!sNormal.glyphs || !str)
        return x;
    while (*str != EOS)
    {
        u16 g;

        if (*str == EXT_CTRL_CODE_BEGIN && str[1] != EOS)
        {
            u8 code = str[1];

            if (code == EXT_CTRL_CODE_COLOR)
                fg = str[2] & 15;
            else if (code == EXT_CTRL_CODE_SHADOW)
                sh = str[2] & 15;
            else if (code == EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW)
                fg = str[2] & 15, sh = str[4] & 15;
            str += 1 + GetExtCtrlCodeLength(code);
            continue;
        }
        g = NextGlyph(&str);
        if (g == 0xFFFF)
            break;
        if (g == 0xFFFE)
        {
            x = left;
            y += sNormal.lineHeight;
            continue;
        }
        x += DrawGlyph(&sNormal, g, x, y, pal[fg], pal[sh]);
    }
    return x;
}

static void DrawSumTextRight(const u8 *str, int right, int y, u8 palNum, u8 colorId)
{
    DrawSumText(str, right - StrWidth(&sNormal, str), y, palNum, colorId);
}

static void DrawSumTextCentred(const u8 *str, int x, int w, int y, u8 palNum, u8 colorId)
{
    int offset = (w - StrWidth(&sNormal, str)) / 2;

    DrawSumText(str, x + (offset > 0 ? offset : 0), y, palNum, colorId);
}

/* Entries of a summary tilemap, `stride` wide, from (sx, sy) for w x h
 * tiles to tile (dx, dy); `picBank` >= 0 gives the picture's backdrop
 * (tiles 1-8 x 4-11, SetMonPicBackgroundPalette) that bank. */
static void DrawSumMap(const u16 *map, int stride, int sx, int sy, int w, int h, int dx, int dy, int picBank)
{
    if (!map || !sRes.sumTiles)
        return;
    for (int ty = 0; ty < h; ++ty)
        for (int tx = 0; tx < w; ++tx)
        {
            u16 e = map[(sy + ty) * stride + sx + tx];
            u32 tile = e & 0x3FF, bank = e >> 12;
            int x = dx + tx, y = dy + ty;

            if (picBank >= 0 && x >= 1 && x < 9 && y >= 4 && y < 12)
                bank = picBank;
            if (tile < sRes.sumTileCount && bank < ARRAY_COUNT(sRes.sumPal))
                DrawTile(sRes.sumTiles + tile * 32, x * 8, y * 8, sRes.sumPal[bank].c, (e >> 10) & 1, (e >> 11) & 1);
        }
}

static void DrawSpriteFlipped(const u8 *tiles, int wt, int ht, int x, int y, const u16 *pal)
{
    for (int ty = 0; ty < ht; ++ty)
        for (int tx = 0; tx < wt; ++tx)
            DrawTile(tiles + (ty * wt + tx) * 32, x + (wt - 1 - tx) * 8, y + ty * 8, pal, TRUE, FALSE);
}

/* A move type's or contest category's label from the summary's sheet. */
static void DrawSumTypeIcon(u8 index, int x, int y)
{
    static const u8 contestPal[CONTEST_CATEGORIES_COUNT] = {0, 1, 1, 2, 0};

    if (!sRes.typeTiles)
        return;
    if (index < NUMBER_OF_MON_TYPES)
        DrawTypeIcon(index, x, y);
    else if (index < NUMBER_OF_MON_TYPES + CONTEST_CATEGORIES_COUNT)
        DrawSprite(sRes.typeTiles + index * 8 * 32, 4, 2, x, y,
                   sRes.typePal[contestPal[index - NUMBER_OF_MON_TYPES]].c);
}

/* One pixel row of a 4bpp tile, `row` of its eight, at (x, y). */
static void DrawTileRow(const u8 *tile, int row, int x, int y, const u16 *pal, bool8 hflip)
{
    for (int px = 0; px < 8; ++px)
    {
        int sx = hflip ? 7 - px : px;
        u8 v = (tile[row * 4 + (sx >> 1)] >> ((sx & 1) * 4)) & 15;

        if (v)
            Put(x + px, y, pal[v]);
    }
}

/*
 * The party menu's CANCEL art (cancel_button.bin, 7x2 tiles) as a button
 * `wt` tiles wide and `h` pixels high: its caps and edge tiles, the middle
 * of the art repeated for the width, and for a taller one the last row of
 * its upper half repeated down to the lower half.
 */
static void DrawButtonArt(int x, int y, int wt, int h)
{
    const u16 *map = sRes.cancelMap;

    if (!map || !sRes.partyTiles || wt < 2 || h < 16)
        return;
    for (int tx = 0; tx < wt; ++tx)
    {
        int col = tx == 0 ? 0 : tx == wt - 1 ? 6 : 1;
        u16 top = map[col], bottom = map[7 + col];
        const u8 *topTile = sRes.partyTiles + (top & 0x3FF) * 32;
        const Pal *pal = &sRes.partyPal[(top >> 12) % 11];

        if ((top & 0x3FF) >= sRes.partyTileCount || (bottom & 0x3FF) >= sRes.partyTileCount)
            return;
        DrawMapEntry(sRes.partyTiles, sRes.partyTileCount, top, x + tx * 8, y, sRes.partyPal);
        for (int py = y + 8; py < y + h - 8; ++py)
            DrawTileRow(topTile, 7, x + tx * 8, py, pal->c, (top >> 10) & 1);
        DrawMapEntry(sRes.partyTiles, sRes.partyTileCount, bottom, x + tx * 8, y + h - 8, sRes.partyPal);
    }
}

/* The label colours of the art: the party menu's default text, greyed. */
static void ButtonText(bool8 enabled, u16 *fg, u16 *sh)
{
    const Pal *pal = &sRes.partyPal[3];

    *fg = enabled ? pal->c[TEXT_COLOR_LIGHT_GRAY] : pal->c[TEXT_COLOR_DARK_GRAY];
    *sh = enabled ? pal->c[TEXT_COLOR_DARK_GRAY] : pal->c[TEXT_COLOR_LIGHT_GRAY];
}

/* The CANCEL art as a 56x16 button, its label in the small font. */
static void DrawGameButton(int x, int y, const u8 *label, bool8 pressed, bool8 enabled, u8 id)
{
    u16 fg, sh;

    pressed = pressed && enabled;
    DrawButtonArt(x, y + pressed, 7, 16);
    ButtonText(enabled, &fg, &sh);
    DrawStrCentered(&sSmall, label, x + 28, y + 1 + pressed, fg, sh);
    if (enabled)
        AddHit(x - 1, y - 6, 58, 28, id);
}

/* A tall button with a label in the middle of it, each of its lines
 * centred. */
static void DrawTallButton(int x, int y, int wt, int h, const u8 *label, bool8 pressed, bool8 enabled, u8 id)
{
    u8 line[24];
    int lines = StrLines(label), top;
    u16 fg, sh;

    pressed = pressed && enabled;
    DrawButtonArt(x, y + pressed, wt, h);
    ButtonText(enabled, &fg, &sh);
    top = y + (h - lines * sSmall.lineHeight) / 2 + pressed;
    for (int i = 0; i < lines; ++i)
    {
        int n = 0;

        while (*label != EOS && *label != CHAR_NEWLINE && n < (int)sizeof(line) - 1)
            line[n++] = *label++;
        line[n] = EOS;
        if (*label == CHAR_NEWLINE)
            ++label;
        DrawStrCentered(&sSmall, line, x + wt * 4, top + i * sSmall.lineHeight, fg, sh);
    }
    if (enabled)
        AddHit(x, y, wt * 8, h, id);
}

/* The Pokémon before or after the one on show (`step` -1 or +1), from the
 * snapshot's party, or -1 if it is the only one. */
static int SummaryNeighbour(const ViewState *s, int step)
{
    for (int i = 1; i < PARTY_SIZE; ++i)
    {
        int slot = (s->partySel + PARTY_SIZE + step * i) % PARTY_SIZE;

        if (s->party[slot].species)
            return slot;
    }
    return -1;
}

/* ◀ or ▶ to another Pokémon: its icon, its name under it, the arrow on
 * the side it is in the party. */
static void DrawMonButton(const ViewState *s, int x, int y, int wt, int h, int step, u8 id)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
    int slot = SummaryNeighbour(s, step), w = wt * 8;
    bool8 enabled = slot >= 0, pressed = enabled && s->pressed == id;
    u16 fg, sh;

    DrawButtonArt(x, y + pressed, wt, h);
    ButtonText(enabled, &fg, &sh);
    DrawStr(&sSmall, step < 0 ? left : right, step < 0 ? x + 3 : x + w - 9, y + (h - sSmall.height) / 2 + pressed, fg, sh);
    if (!enabled)
        return;
    {
        const MonView *m = &s->party[slot];
        const u8 *tiles = MonIcon(m->iconSpecies, m->deoxys);
        u8 pal = gMonIconPaletteIndices[m->iconSpecies];

        if (tiles && pal < 3)
            DrawSprite(tiles, 4, 4, x + (w - 32) / 2, y + 1 + pressed, sRes.monIconPal[pal].c);
        DrawStrCentered(&sSmall, m->nick, x + w / 2, y + h - sSmall.height - 3 + pressed, fg, sh);
    }
    AddHit(x, y, w, h, id);
}

/* The left panel: dex number, picture, ball, name, species, level and
 * gender (PrintNotEggInfo, PrintEggInfo); the STATUS box. */
static void DrawSumPortrait(const ViewState *s)
{
    const MonView *m = &s->party[s->partySel];
    u8 text[POKEMON_NAME_LENGTH + 8];

    if (sSumPic.valid)
    {
        if (m->isEgg || IsMonSpriteNotFlipped(sSumPic.species))
            DrawSprite(sSumPic.tiles, 8, 8, 8, 32, sSumPic.pal.c);
        else
            DrawSpriteFlipped(sSumPic.tiles, 8, 8, 8, 32, sSumPic.pal.c);
    }
    if (s->ball < POKEBALL_COUNT && sBallTiles[s->ball])
        DrawSprite(sBallTiles[s->ball], 2, 2, 8, 128, sBallPal[s->ball].c);
    DrawSumText(m->nick, 8, 97, 6, 1);
    if (m->isEgg)
        return;
    if (s->dexNum != 0xFFFF)
    {
        StringCopy(StringCopy(text, gText_NumberClear01), Number(s->dexNum, 3, STR_CONV_MODE_LEADING_ZEROS));
        DrawSumText(text, 8, 17, 7, s->shiny ? 7 : 1);
    }
    text[0] = CHAR_SLASH;
    StringCopy(text + 1, gSpeciesNames[m->species]);
    DrawSumText(text, 8, 113, 6, 1);
    DrawSumText(LevelText(m->level), 8 + 24, 113 + 16, 6, 1);
    if (m->species != SPECIES_NIDORAN_M && m->species != SPECIES_NIDORAN_F)
    {
        if (m->gender == MON_MALE)
            DrawSumText(gText_MaleSymbol, 8 + 57, 113 + 16, 6, 3);
        else if (m->gender == MON_FEMALE)
            DrawSumText(gText_FemaleSymbol, 8 + 57, 113 + 16, 6, 4);
    }
    if (m->ailment != AILMENT_NONE)
    {
        DrawSumMap(sRes.sumStatus, 10, 0, 0, 10, 2, 0, 18, -1);
        DrawSumText(gText_Status, 2, 145, 6, 1);
        DrawAilment(m->ailment, 48, 148);
    }
}

static void DrawSumInfo(const ViewState *s)
{
    const MonView *m = &s->party[s->partySel];
    u8 text[24];
    int x;

    if (m->isEgg)
    {
        x = DrawSumText(gText_OTSlash, 88, 33, 6, 1);
        DrawSumText(gText_FiveMarks, x, 33, 6, 1);
        StringCopy(StringCopy(text, gText_IDNumber2), gText_FiveMarks);
        DrawSumTextRight(text, 176 + 56, 33, 6, 1);
        DrawSumText(s->eggState, 88, 73, 6, 0);
        DrawSumText(s->memo, 88, 113, 6, 0);
        return;
    }
    x = DrawSumText(gText_OTSlash, 88, 33, 6, 1);
    DrawSumText(s->otName, x, 33, 6, s->otGender == MALE ? 5 : 6);
    StringCopy(StringCopy(text, gText_IDNumber2), Number(s->otId, 5, STR_CONV_MODE_LEADING_ZEROS));
    DrawSumTextRight(text, 176 + 56, 33, 6, 1);
    DrawSumText(gText_TypeSlash, 88, 49, 6, 0);
    DrawSumTypeIcon(s->types[0], 120, 48);
    if (s->types[1] != s->types[0])
        DrawSumTypeIcon(s->types[1], 160, 48);
    DrawSumText(gAbilityNames[s->ability], 88, 73, 6, 1);
    DrawSumText(gAbilityDescriptionPointers[s->ability], 88, 89, 6, 0);
    DrawSumText(s->memo, 88, 113, 6, 0);
}

/* {STR_VAR_1} of a template as `value`, without the game's buffers. */
static void ExpandVar1(u8 *dest, const u8 *src, const u8 *value)
{
    while (*src != EOS)
    {
        if (src[0] == PLACEHOLDER_BEGIN && src[1] == PLACEHOLDER_ID_STRING_VAR_1)
        {
            dest = StringCopy(dest, value);
            src += 2;
            continue;
        }
        *dest++ = *src++;
    }
    *dest = EOS;
}

static void DrawSumSkills(const ViewState *s)
{
    const MonView *m = &s->party[s->partySel];
    u8 text[ITEM_NAME_LENGTH + 20];
    const u8 *label;

    if (m->item == ITEM_NONE)
        label = gText_None;
    else
    {
        CopyItemName(m->item, text);
        label = text;
    }
    DrawSumTextCentred(label, 80 + 6, 72, 33, 6, 0);
    if (s->ribbons == 0)
        label = gText_None;
    else
    {
        ExpandVar1(text, gText_RibbonsVar1, Number(s->ribbons, 2, STR_CONV_MODE_RIGHT_ALIGN));
        label = text;
    }
    DrawSumTextCentred(label, 160 + 6, 70, 33, 6, 0);

    DrawSumTextCentred(gText_HP4, 80 + 6, 42, 57, 6, 1);
    DrawSumTextCentred(gText_Attack3, 80 + 6, 42, 73, 6, 1);
    DrawSumTextCentred(gText_Defense3, 80 + 6, 42, 89, 6, 1);
    DrawSumTextCentred(gText_SpAtk4, 176 + 2, 36, 57, 6, 1);
    DrawSumTextCentred(gText_SpDef4, 176 + 2, 36, 73, 6, 1);
    DrawSumTextCentred(gText_Speed2, 176 + 2, 36, 89, 6, 1);
    /* sStatsLeftColumnLayout and sStatsRightColumnLayout. */
    StringCopy(StringCopy(StringCopy(text, Number(m->hp, 3, STR_CONV_MODE_RIGHT_ALIGN)), gText_Slash),
               Number(s->stats[0], 3, STR_CONV_MODE_RIGHT_ALIGN));
    DrawSumText(text, 128 + 4, 57, 6, 0);
    DrawSumText(Number(s->stats[1], 7, STR_CONV_MODE_RIGHT_ALIGN), 128 + 4, 73, 6, 0);
    DrawSumText(Number(s->stats[2], 7, STR_CONV_MODE_RIGHT_ALIGN), 128 + 4, 89, 6, 0);
    for (int i = 3; i < NUM_STATS; ++i)
        DrawSumText(Number(s->stats[i], 3, STR_CONV_MODE_RIGHT_ALIGN), 216 + 2, 57 + (i - 3) * 16, 6, 0);

    DrawSumText(gText_ExpPoints, 80 + 6, 113, 6, 1);
    DrawSumText(gText_NextLv, 80 + 6, 129, 6, 1);
    DrawSumTextRight(Number(s->exp, 7, STR_CONV_MODE_RIGHT_ALIGN), 192 + 42 + 2, 113, 6, 0);
    DrawSumTextRight(Number(s->expNext, 6, STR_CONV_MODE_RIGHT_ALIGN), 192 + 42 + 2, 129, 6, 0);
    /* The EXP bar: eight tiles at (21, 18), 0x2062 + ticks, 0x206A full. */
    {
        int ticks = s->expTicks;
        u16 bar[8];

        for (int i = 0; i < 8; ++i, ticks -= 8)
            bar[i] = ticks > 7 ? 0x206A : 0x2062 + (ticks > 0 ? ticks : 0);
        DrawSumMap(bar, 8, 0, 0, 8, 1, 21, 18, -1);
    }
}

static void DrawSumMoves(const ViewState *s, bool8 contest)
{
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        u16 move = s->moves[i];
        int y = 32 + i * 16;
        u8 text[16];

        if (move == MOVE_NONE)
        {
            DrawSumText(gText_OneDash, 120, y + 1, 6, 1);
            DrawSumTextCentred(gText_TwoDashes, 192, 44, y + 1, 8, 12);
            continue;
        }
        DrawSumTypeIcon(contest ? NUMBER_OF_MON_TYPES + gContestMoves[move].contestCategory : gBattleMoves[move].type, 85, y);
        DrawSumText(gMoveNames[move], 120, y + 1, 6, 1);
        StringCopy(StringCopy(StringCopy(StringCopy(text, gText_MoveInterfacePP), Number(s->pp[i], 2, STR_CONV_MODE_RIGHT_ALIGN)),
                              gText_Slash), Number(s->maxPp[i], 2, STR_CONV_MODE_RIGHT_ALIGN));
        DrawSumTextRight(text, 192 + 44, y + 1, 8, GetCurrentPpToMaxPpState(s->pp[i], s->maxPp[i]) + 9);
        AddHit(80, y, 160, 16, HIT_SUM_MOVE + i);
    }
    if (s->sumMove == SUM_NO_MOVE)
        return;

    /* The move picked: the selector on its row, the effect box with its
     * POWER and ACCURACY (APPEAL and JAM hearts), its description. */
    if (sRes.moveSelectTiles)
        for (int i = 0; i < 10; ++i)
        {
            const u8 *tiles = sRes.moveSelectTiles + (i == 0 || i == 9 ? 16 : 20) * 32;
            int x = 81 + i * 16, y = 32 + s->sumMove * 16;

            if (i == 9)
                DrawSpriteFlipped(tiles, 2, 2, x, y, sRes.moveSelectPal.c);
            else
                DrawSprite(tiles, 2, 2, x, y, sRes.moveSelectPal.c);
        }
    {
        u16 move = s->moves[s->sumMove];

        if (!contest)
        {
            DrawSumMap(sRes.sumEffectBattle, 10, 0, 0, 10, 7, 0, 13, -1);
            DrawSumText(gText_Power, 8, 121, 6, 1);
            DrawSumText(gText_Accuracy2, 8, 137, 6, 1);
            DrawSumText(gBattleMoves[move].power < 2 ? gText_ThreeDashes
                                                     : Number(gBattleMoves[move].power, 3, STR_CONV_MODE_RIGHT_ALIGN),
                        8 + 53, 121, 6, 0);
            DrawSumText(gBattleMoves[move].accuracy == 0 ? gText_ThreeDashes
                                                         : Number(gBattleMoves[move].accuracy, 3, STR_CONV_MODE_RIGHT_ALIGN),
                        8 + 53, 137, 6, 0);
            DrawSumText(gMoveDescriptionPointers[move - 1], 80, 121, 6, 0);
        }
        else
        {
            const struct ContestEffect *effect = &gContestEffects[gContestMoves[move].effect];
            u8 appeal = effect->appeal == 0xFF ? 0xFF : effect->appeal / 10;
            u8 jam = effect->jam == 0xFF ? 0xFF : effect->jam / 10;
            u16 hearts[2][2][4];

            DrawSumMap(sRes.sumEffectContest, 10, 0, 0, 10, 7, 0, 13, -1);
            DrawSumText(gText_Appeal, 8, 121, 6, 1);
            DrawSumText(gText_Jam, 8, 137, 6, 1);
            /* DrawContestMoveHearts: two rows of four from (6, 15) and (6, 17). */
            for (int i = 0; i < 8; ++i)
            {
                hearts[0][i / 4][i & 3] = appeal != 0xFF && i < appeal ? 0x103A : 0x1039;
                hearts[1][i / 4][i & 3] = jam != 0xFF && i < jam ? 0x103C : 0x103D;
            }
            DrawSumMap(&hearts[0][0][0], 4, 0, 0, 4, 2, 6, 15, -1);
            DrawSumMap(&hearts[1][0][0], 4, 0, 0, 4, 2, 6, 17, -1);
            DrawSumText(gContestEffectDescriptionPointers[gContestMoves[move].effect], 80, 121, 6, 0);
        }
    }
}

static void DrawPartySummary(const ViewState *s)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
    static const u8 *const titles[SUM_PAGES] = {gText_PkmnInfo, gText_PkmnSkills, gText_BattleMoves, gText_ContestMoves};
    const MonView *m = &s->party[s->partySel];
    bool8 others = FALSE;

    for (int i = 0; i < PARTY_SIZE; ++i)
        if (i != s->partySel && s->party[i].species)
            others = TRUE;

    DrawPartyBackground(CW / 8, H / 8);
    if (s->sumOk)
    {
        /* The INFO page under every page (BG3), the page's own over it. */
        DrawSumMap(sRes.sumPage[m->isEgg ? 4 : 0], 32, 0, 0, 30, 20, 0, 0, m->isEgg ? -1 : s->shiny ? 5 : 0);
        if (s->sumPage != SUM_INFO)
            DrawSumMap(sRes.sumPage[s->sumPage], 32, 0, 0, 30, 20, 0, 0, -1);
        /* The title bar: the page's title alone, across it. No page dots
         * or prompt: the buttons under the summary do their work. Its
         * tiles are the bar's own, those of its left part. */
        {
            u16 bar[2][30];

            for (int tx = 0; tx < 30; ++tx)
            {
                bar[0][tx] = sRes.sumPage[0][0];
                bar[1][tx] = sRes.sumPage[0][32];
            }
            DrawSumMap(&bar[0][0], 30, 0, 0, 30, 2, 0, 0, -1);
        }
        DrawSumTextCentred(titles[s->sumPage], 0, 240, 1, 6, 1);
        DrawSumPortrait(s);
        if (s->sumPage == SUM_INFO)
            DrawSumInfo(s);
        else if (s->sumPage == SUM_SKILLS)
            DrawSumSkills(s);
        else
            DrawSumMoves(s, s->sumPage == SUM_CONTEST);
    }

    /* The keys' work by touch: the page, the Pokémon, back and SWITCH. */
    DrawGameButton(8, 166, left, s->pressed == HIT_SUM_PAGE_PREV, !m->isEgg, HIT_SUM_PAGE_PREV);
    DrawGameButton(176, 166, right, s->pressed == HIT_SUM_PAGE_NEXT, !m->isEgg, HIT_SUM_PAGE_NEXT);
    /* The Pokémon before and after, each with its icon and name. */
    DrawMonButton(s, 2, 186, 7, 52, -1, HIT_PARTY_PREV);
    DrawTallButton(62, 186, 7, 52, gText_Cancel, s->pressed == HIT_PARTY_BACK, TRUE, HIT_PARTY_BACK);
    /* SWITCH moves the move picked on a moves page (held down until the
     * move it swaps with is tapped), the Pokémon otherwise. */
    {
        bool8 move = s->sumMove != SUM_NO_MOVE;
        int moves = 0;

        for (int i = 0; i < MAX_MON_MOVES; ++i)
            moves += s->moves[i] != MOVE_NONE;
        DrawTallButton(122, 186, 7, 52,
                       Ascii(move ? CTR_TEXT("SWITCH\nMOVES", "CAMBIAR\nMOVIM.", "ÉCHANGER\nCAPAC.", "SCAMBIA\nMOSSE")
                                  : CTR_TEXT("SWITCH\nPOKéMON", "CAMBIAR\nPOKéMON", "ÉCHANGER\nPOKéMON", "SCAMBIA\nPOKéMON")),
                       s->pressed == HIT_PARTY_SWITCH || s->sumMoveSwap != SUM_NO_MOVE,
                       (move ? moves > 1 : others) && s->canEdit, HIT_PARTY_SWITCH);
    }
    DrawMonButton(s, 182, 186, 7, 52, 1, HIT_PARTY_NEXT);
}

/*
 * The bag is the game's bag screen as item_menu.c draws it on the GBA's
 * 240x160, at the top of the bottom screen: its tilemap, the bag picture of
 * the pocket, the pocket's name and its square in the row of five, the
 * pocket and list arrows, the list (FONT_NARROW: names, numbers, quantities,
 * the HM and SELECT marks, the cursor), the item's icon and description.
 * Under it, in the bag's own stripes, taps in place of the keys.
 */
static const u16 sBagFrame[POCKETS_COUNT] = {
    [ITEMS_POCKET] = 64, [BALLS_POCKET] = 192, [TMHM_POCKET] = 256, [BERRIES_POCKET] = 320, [KEYITEMS_POCKET] = 128,
};

static void DrawBagMap(const u16 *map, int stride, int sx, int sy, int w, int h, int dx, int dy, const Pal *pals)
{
    for (int ty = 0; ty < h; ++ty)
        for (int tx = 0; tx < w; ++tx)
        {
            u16 e = map[(sy + ty) * stride + sx + tx];

            if ((e >> 12) < 2)
                DrawMapEntry(sRes.bagTiles, sRes.bagTileCount, e, (dx + tx) * 8, (dy + ty) * 8, pals);
        }
}

/* A sprite with either flip, in the GBA's one-dimensional layout. */
static void DrawSpriteFlip(const u8 *tiles, int wt, int ht, int x, int y, const u16 *pal, bool8 hflip, bool8 vflip)
{
    for (int ty = 0; ty < ht; ++ty)
        for (int tx = 0; tx < wt; ++tx)
            DrawTile(tiles + (ty * wt + tx) * 32, x + (hflip ? wt - 1 - tx : tx) * 8, y + (vflip ? ht - 1 - ty : ty) * 8, pal,
                     hflip, vflip);
}

/* A list or description line: {CLEAR} and {CLEAR_TO} move on as the
 * game's text printer does (the TM, HM and berry rows' columns). */
static int DrawBagText(const Font *font, const u8 *str, int x, int y, u16 fg, u16 sh)
{
    int left = x;

    while (str && *str != EOS)
    {
        u16 g;

        if (*str == EXT_CTRL_CODE_BEGIN && (str[1] == EXT_CTRL_CODE_CLEAR || str[1] == EXT_CTRL_CODE_CLEAR_TO))
        {
            x = str[1] == EXT_CTRL_CODE_CLEAR ? x + str[2] : left + str[2];
            str += 3;
            continue;
        }
        g = NextGlyph(&str);
        if (g == 0xFFFF)
            break;
        if (g == 0xFFFE)
        {
            x = left;
            y += font->lineHeight;
            continue;
        }
        x += DrawGlyph(font, g, x, y, fg, sh);
    }
    return x;
}

/* {STR_VAR_1} and {STR_VAR_2} of a template, without the game's buffers. */
static void ExpandVars(u8 *dest, const u8 *src, const u8 *var1, const u8 *var2)
{
    while (*src != EOS)
    {
        if (src[0] == PLACEHOLDER_BEGIN && (src[1] == PLACEHOLDER_ID_STRING_VAR_1 || src[1] == PLACEHOLDER_ID_STRING_VAR_2))
        {
            dest = StringCopy(dest, src[1] == PLACEHOLDER_ID_STRING_VAR_1 ? var1 : var2);
            src += 2;
            continue;
        }
        if (src[0] == EXT_CTRL_CODE_BEGIN)
        {
            u8 n = 1 + GetExtCtrlCodeLength(src[1]);

            while (n-- && *src != EOS)
                *dest++ = *src++;
            continue;
        }
        *dest++ = *src++;
    }
    *dest = EOS;
}

/* GetItemNameFromPocket: TMs, HMs and berries numbered. */
static void BagItemName(u8 *dest, u8 pocket, u16 item)
{
    u8 number[4], name[ITEM_NAME_LENGTH + 4];

    if (pocket == TMHM_POCKET)
    {
        StringCopy(name, gMoveNames[ItemIdToBattleMoveId(item)]);
        if (item >= ITEM_HM01)
        {
            ConvertIntToDecimalStringN(number, item - ITEM_HM01 + 1, STR_CONV_MODE_LEADING_ZEROS, 1);
            ExpandVars(dest, gText_NumberItem_HM, number, name);
        }
        else
        {
            ConvertIntToDecimalStringN(number, item - ITEM_TM01 + 1, STR_CONV_MODE_LEADING_ZEROS, 2);
            ExpandVars(dest, gText_NumberItem_TMBerry, number, name);
        }
    }
    else if (pocket == BERRIES_POCKET)
    {
        ConvertIntToDecimalStringN(number, item - FIRST_BERRY_INDEX + 1, STR_CONV_MODE_LEADING_ZEROS, 2);
        CopyItemName(item, name);
        ExpandVars(dest, gText_NumberItem_TMBerry, number, name);
    }
    else
        CopyItemName(item, dest);
}

static void DrawPartyMail(const ViewState *s);

/*
 * CHECK TAG: berry_tag_screen.c's screen, at the party's height: BG3's tile
 * in the player's colour all over, the tag (BG2), the texts (BG1's windows,
 * in the screen's font palette), the flavour circles and the berry's
 * picture (OBJ, under BG0), the title (BG0) with BERRY TAG. The arrows
 * go to the berry before or after in the pocket, as UP and DOWN do.
 */
static struct
{
    bool8 tried;
    u8 *tiles, *circle, *pic;
    u16 *tag, *title;
    u32 tileCount;
    Pal pals[16], picPal;
    u8 picBerry;
} sTagGfx;

static void LoadBerryTag(u8 berry)
{
    u32 size = 0;

    if (!sTagGfx.tried)
    {
        u16 *raw;

        sTagGfx.tried = TRUE;
        sTagGfx.tiles = Unlz(gBerryCheck_Gfx, &size);
        sTagGfx.tileCount = sTagGfx.tiles ? size / 32 : 0;
        sTagGfx.tag = Unlz(gBerryTag_Gfx, NULL);
        sTagGfx.title = Unlz(gBerryTag_Tilemap, NULL);
        sTagGfx.circle = Unlz(gBerryCheckCircle_Gfx, NULL);
        raw = Unlz(gBerryCheck_Pal, &size);
        if (raw)
            ToPals(sTagGfx.pals, raw, size / 32 < 6 ? size / 32 : 6);
        free(raw);
        PalFile("bag/berry_tag_screen.gbapal", &sTagGfx.pals[15], 1, FALSE);
    }
    if (sTagGfx.picBerry != berry)
    {
        const u32 *tiles, *pal;
        u16 *raw;

        free(sTagGfx.pic);
        CtrBerry_Pic(berry - 1, &tiles, &pal);
        sTagGfx.pic = Unlz(tiles, NULL);
        raw = Unlz(pal, NULL);
        if (raw)
            ToPals(&sTagGfx.picPal, raw, 1);
        free(raw);
        sTagGfx.picBerry = berry;
    }
}

static void DrawBerryTag(const ViewState *s)
{
    static const u8 circleX[FLAVOR_COUNT] = {64, 104, 144, 184, 224};
    static const u8 up[] = {CHAR_UP_ARROW, EOS}, down[] = {CHAR_DOWN_ARROW, EOS};
    static const u8 *const firmness[] = {gBerryFirmnessString_VerySoft, gBerryFirmnessString_Soft,
                                         gBerryFirmnessString_Hard, gBerryFirmnessString_VeryHard,
                                         gBerryFirmnessString_SuperHard};
    const struct Berry *berry = GetBerryInfo(s->bagTagBerry);
    const u16 *font = sTagGfx.pals[15].c;
    u16 bg = ((s->gender == MALE ? 4 : 5) << 12) | 0x42;
    bool8 flavors[FLAVOR_COUNT];
    u8 text[64];
#if GAME_LANGUAGE == LANGUAGE_SPANISH || GAME_LANGUAGE == LANGUAGE_FRENCH
    const int descX = 24;
#else
    const int descX = 32;
#endif

    LoadBerryTag(s->bagTagBerry);
    if (!sTagGfx.tiles || !sTagGfx.tag || !sTagGfx.title)
        return;
    for (int ty = 0; ty < H / 8; ++ty)
        for (int tx = 0; tx < CW / 8; ++tx)
            DrawMapEntry(sTagGfx.tiles, sTagGfx.tileCount, bg, tx * 8, ty * 8, sTagGfx.pals);
    for (int ty = 0; ty < 20; ++ty)
        for (int tx = 0; tx < 30; ++tx)
            DrawMapEntry(sTagGfx.tiles, sTagGfx.tileCount, sTagGfx.tag[ty * 32 + tx], tx * 8, PARTY_Y0 + ty * 8,
                         sTagGfx.pals);

    /* PrintBerryNumberAndName, ...Size, ...Firmness, ...Description1 and 2. */
    ExpandVars(text, gText_NumberVar1Var2, Number(s->bagTagBerry, 2, STR_CONV_MODE_LEADING_ZEROS), berry->name);
    DrawStr(&sNormal, text, 88, PARTY_Y0 + 33, font[2], font[3]);
    DrawStr(&sNormal, gText_SizeSlash, 88, PARTY_Y0 + 57, font[2], font[3]);
    if (berry->size != 0)
    {
        u32 inches = 1000 * berry->size / 254, fraction;
        u8 whole[4];

        if (inches % 10 > 4)
            inches += 10;
        fraction = (inches % 100) / 10;
        inches /= 100;
        StringCopy(whole, Number(inches, 2, STR_CONV_MODE_LEFT_ALIGN));
        ExpandVars(text, gText_Var1DotVar2, whole, Number(fraction, 2, STR_CONV_MODE_LEFT_ALIGN));
        DrawStr(&sNormal, text, 128, PARTY_Y0 + 57, font[2], font[3]);
    }
    else
        DrawStr(&sNormal, gText_ThreeMarks, 128, PARTY_Y0 + 57, font[2], font[3]);
    DrawStr(&sNormal, gText_FirmSlash, 88, PARTY_Y0 + 73, font[2], font[3]);
    DrawStr(&sNormal, berry->firmness != 0 && berry->firmness <= ARRAY_COUNT(firmness) ? firmness[berry->firmness - 1]
                                                                                      : gText_ThreeMarks,
            128, PARTY_Y0 + 73, font[2], font[3]);
    DrawStr(&sNormal, berry->description1, descX, PARTY_Y0 + 113, font[2], font[3]);
    DrawStr(&sNormal, berry->description2, descX, PARTY_Y0 + 129, font[2], font[3]);

    /* The sprites, priority 1: the flavours it has, and the berry (48x48,
     * ArrangeBerryGfx's margin in its 64x64 sprite at (56, 64)). */
    flavors[FLAVOR_SPICY] = berry->spicy;
    flavors[FLAVOR_DRY] = berry->dry;
    flavors[FLAVOR_SWEET] = berry->sweet;
    flavors[FLAVOR_BITTER] = berry->bitter;
    flavors[FLAVOR_SOUR] = berry->sour;
    for (int i = 0; i < FLAVOR_COUNT; ++i)
        if (flavors[i] && sTagGfx.circle)
            DrawSprite(sTagGfx.circle, 8, 8, circleX[i] - 32, PARTY_Y0 + 116 - 32, sTagGfx.pals[0].c);
    if (sTagGfx.pic)
        DrawSprite(sTagGfx.pic, 6, 6, 56 - 24, PARTY_Y0 + 64 - 24, sTagGfx.picPal.c);

    /* BG0: the title, and BERRY TAG in its window (2, 0). */
    for (int ty = 0; ty < 20; ++ty)
        for (int tx = 0; tx < 30; ++tx)
            if (sTagGfx.title[ty * 32 + tx] & 0x3FF)
                DrawMapEntry(sTagGfx.tiles, sTagGfx.tileCount, sTagGfx.title[ty * 32 + tx], tx * 8, PARTY_Y0 + ty * 8,
                             sTagGfx.pals);
    FillRect(16, PARTY_Y0, 64, 16, font[15]);
    DrawStrCentered(&sNormal, gText_BerryTag, 48, PARTY_Y0 + 1, font[14], font[13]);

    DrawTrayButton(TRAY_X(0), 212, up, s->pressed == HIT_BAG_QTY, s->bagSel > 0, FALSE, HIT_BAG_QTY);
    DrawTrayButton(TRAY_X(1), 212, down, s->pressed == HIT_BAG_QTY + 1, s->bagSel + 1 < s->bagCount, FALSE,
                   HIT_BAG_QTY + 1);
    DrawTrayButton(TRAY_X(3), 212, gText_Cancel2, s->pressed == HIT_BAG_NO, TRUE, FALSE, HIT_BAG_NO);
}

static void DrawBag(const ViewState *s)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
    static const u8 up[] = {CHAR_UP_ARROW, EOS}, down[] = {CHAR_DOWN_ARROW, EOS};
    const Pal *pals = sRes.bagPal[s->gender == FEMALE];
    u16 fg = pals[1].c[TEXT_COLOR_WHITE], sh = pals[1].c[TEXT_COLOR_LIGHT_GRAY];
    u8 text[64];
    bool8 key = s->bagPocket == KEYITEMS_POCKET;
    bool8 item = s->bagSel != BAG_NO_ROW && s->bagSel < s->bagCount;
    bool8 swappable = s->bagPocket != TMHM_POCKET && s->bagPocket != BERRIES_POCKET && s->bagCount > 1;

    if (!s->bagOk)
        return;
    /* CHECK on a MAIL: the stationery, blank (CB2_CheckMail). */
    if (s->bagAct == BACT_MAIL)
    {
        DrawPartyMail(s);
        return;
    }
    if (s->bagAct == BACT_TAG)
    {
        DrawBerryTag(s);
        return;
    }
    /* The bag's backdrop, and its stripes under it for the taps. */
    DrawBagMap(sRes.bagMap, 32, 0, 0, 30, 20, 0, 0, pals);
    for (int ty = 20; ty < H / 8; ++ty)
        for (int tx = 0; tx < 30; ++tx)
            DrawBagMap(sRes.bagMap, 32, 0, 6 + (ty & 1), 1, 1, tx, ty, pals);
    /* The pocket's square in the row of five (DrawPocketIndicatorSquare). */
    for (int i = 0; i < POCKETS_COUNT; ++i)
    {
        u16 square = i == s->bagPocket ? 0x102B : 0x1017;

        DrawBagMap(&square, 1, 0, 0, 1, 1, 5 + i, 3, pals);
    }
    if (sRes.bagSprite[s->gender])
        DrawSprite(sRes.bagSprite[s->gender] + sBagFrame[s->bagPocket] * 32, 8, 8, 36, 34, sRes.bagSpritePal.c);
    DrawStrCentered(&sNormal, gPocketNamesStringsTable[s->bagPocket], 32 + 32, 9, pals[1].c[TEXT_COLOR_WHITE],
                    pals[1].c[TEXT_COLOR_RED]);
    /* The pocket arrows (sBagScrollArrowsTemplate), the list's (172: 12, 148). */
    if (sRes.scrollTiles)
    {
        if (s->bagPocket > 0)
            DrawSpriteFlip(sRes.scrollTiles, 2, 2, 20, 8, sRes.scrollPal.c, FALSE, FALSE);
        if (s->bagPocket < POCKETS_COUNT - 1)
            DrawSpriteFlip(sRes.scrollTiles, 2, 2, 92, 8, sRes.scrollPal.c, TRUE, FALSE);
        if (s->bagTop > 0)
            DrawSpriteFlip(sRes.scrollTiles + 4 * 32, 2, 2, 164, 4, sRes.scrollPal.c, FALSE, FALSE);
        if (s->bagTop + BAG_ROWS < s->bagCount + 1)
            DrawSpriteFlip(sRes.scrollTiles + 4 * 32, 2, 2, 164, 140, sRes.scrollPal.c, FALSE, TRUE);
    }
    AddHit(0, 0, 56, 32, HIT_BAG_POCKET_PREV);
    AddHit(56, 0, 56, 32, HIT_BAG_POCKET_NEXT);
    AddHit(112, 0, 128, 16, HIT_BAG_UP);
    AddHit(112, 144, 128, 16, HIT_BAG_DOWN);

    /* The list: WIN_ITEM_LIST at (14, 2), rows of 16 from y 1, names at 8. */
    for (int i = 0; i < BAG_ROWS; ++i)
    {
        int row = s->bagTop + i, y = 16 + 1 + i * 16;

        if (row > s->bagCount)
            break;
        if (row == s->bagSel || row == s->bagSwap)
        {
            const Pal *p = &pals[1];
            bool8 grey = row == s->bagSwap && row != s->bagSel;

            DrawStr(&sNormal, gText_SelectorArrow2, 112, y, grey ? p->c[TEXT_COLOR_LIGHT_GRAY] : fg,
                    grey ? p->c[TEXT_COLOR_GREEN] : sh);
        }
        AddHit(112, y - 1, 128, 16, HIT_ROW + i);
        if (row == s->bagCount)
        {
            DrawBagText(&sNarrow, gText_CloseBag, 120, y, fg, sh);
            continue;
        }
        BagItemName(text, s->bagPocket, s->bagItem[i]);
        if (s->bagItem[i] >= ITEM_HM01 && s->bagItem[i] <= ITEM_HM08 && sRes.hmIcon)
            DrawSprite(sRes.hmIcon, 2, 2, 120, y - 1, pals[1].c);
        DrawBagText(&sNarrow, text, 120, y, fg, sh);
        if (s->bagPocket == BERRIES_POCKET || (!key && GetItemImportance(s->bagItem[i]) == FALSE))
        {
            u8 qty[8];

            ExpandVar1(qty, gText_xVar1, Number(s->bagQty[i], s->bagPocket == BERRIES_POCKET ? BERRY_CAPACITY_DIGITS
                                                                                           : BAG_ITEM_CAPACITY_DIGITS,
                                                 STR_CONV_MODE_RIGHT_ALIGN));
            DrawBagText(&sNarrow, qty, 112 + 119 - StrWidth(&sNarrow, qty), y, fg, sh);
        }
        else if (s->registered != ITEM_NONE && s->registered == s->bagItem[i] && sRes.selectIcon)
            DrawSprite(sRes.selectIcon, 3, 2, 112 + 96, y - 1, pals[1].c);
    }

    /* The picked row: its icon (24, 88) and description (WIN_DESCRIPTION). */
    if (s->bagSelItem != ITEM_NONE)
    {
        DrawItemIcon(s->bagSelItem, 8, 72);
        if (s->bagAct != BACT_NONE && s->bagBoxDesc)
            DrawBagText(&sNormal, s->bagBox, 3, 104 + 1, fg, sh);
        else if (s->bagMsg[0] != EOS)
            DrawBagText(&sNormal, s->bagMsg, 3, 104 + 1, fg, sh);
        else if (s->bagSelItem == ITEM_LIST_END)
        {
            ExpandVar1(text, gText_ReturnToVar1, gBagMenu_ReturnToStrings[s->bagGive != PARTY_NO_SWAP
                                                                          ? ITEMMENULOCATION_PARTY
                                                                          : ITEMMENULOCATION_FIELD]);
            DrawBagText(&sNormal, text, 3, 104 + 1, fg, sh);
        }
        else
            DrawBagText(&sNormal, GetItemDescription(s->bagSelItem), 3, 104 + 1, fg, sh);
    }

    /* A message or a question, in the description's place or a box under
     * the bag (DisplayItemMessage's), with its buttons in the tray; TOSS's
     * how many, with the quantity box (ITEMWIN_QUANTITY) by the icon. */
    if (s->bagAct != BACT_NONE)
    {
        if (!s->bagBoxDesc)
        {
            DrawWindowFrame(s->frame, 16, 168, 26, 4, TXT_WHITE);
            DrawStr(&sNormal, s->bagBox, 16, 169, TXT_DARK, TXT_LIGHT);
        }
        if (s->bagAct == BACT_QTY)
        {
            static const u8 labels[][4] = {{CHAR_HYPHEN, CHAR_1, CHAR_0, EOS}, {CHAR_HYPHEN, CHAR_1, EOS},
                                           {CHAR_PLUS, CHAR_1, EOS}, {CHAR_PLUS, CHAR_1, CHAR_0, EOS}};

            ExpandVar1(text, gText_xVar1, Number(s->bagTossQty, BAG_ITEM_CAPACITY_DIGITS, STR_CONV_MODE_LEADING_ZEROS));
            DrawWindowFrame(s->frame, 64, 84, 5, 2, TXT_WHITE);
            DrawStrCentered(&sNormal, text, 84, 85, TXT_DARK, TXT_LIGHT);
            for (int i = 0; i < 4; ++i)
                DrawTrayButton(TRAY_X(i), 186, labels[i], s->pressed == HIT_BAG_QTY + i, TRUE, FALSE, HIT_BAG_QTY + i);
            DrawTrayButton(TRAY_X(2), 214, gText_Ok2, s->pressed == HIT_BAG_OK, TRUE, FALSE, HIT_BAG_OK);
            DrawTrayButton(TRAY_X(3), 214, gText_Cancel2, s->pressed == HIT_BAG_NO, TRUE, FALSE, HIT_BAG_NO);
        }
        else if (s->bagAct == BACT_ASK && !s->bagMore)
        {
            DrawTrayButton(TRAY_X(1), 214, gText_Yes, s->pressed == HIT_BAG_YES, TRUE, FALSE, HIT_BAG_YES);
            DrawTrayButton(TRAY_X(2), 214, gText_No, s->pressed == HIT_BAG_NO, TRUE, FALSE, HIT_BAG_NO);
        }
        else
        {
            /* A tap anywhere goes on, as A does. */
            AddHit(0, 0, CW, H, HIT_BAG_OK);
            DrawTrayButton(TRAY_X(1) + 30, 214, gText_Ok2, s->pressed == HIT_BAG_OK, TRUE, FALSE, HIT_BAG_OK);
        }
        return;
    }

    /* The keys' work by touch: pocket and scroll; then the item's actions
     * (the game's context menu), SWITCH ITEM and CANCEL. */
    DrawGameButton(2, 166, left, s->pressed == HIT_BAG_POCKET_PREV, s->bagPocket > 0, HIT_BAG_POCKET_PREV);
    DrawGameButton(62, 166, up, s->pressed == HIT_BAG_PAGE_UP, s->bagTop > 0, HIT_BAG_PAGE_UP);
    DrawGameButton(122, 166, down, s->pressed == HIT_BAG_PAGE_DOWN, s->bagTop + BAG_ROWS < s->bagCount + 1, HIT_BAG_PAGE_DOWN);
    DrawGameButton(182, 166, right, s->pressed == HIT_BAG_POCKET_NEXT, s->bagPocket < POCKETS_COUNT - 1, HIT_BAG_POCKET_NEXT);
    if (item)
        for (int i = 0; i < s->bagActionCount; ++i)
        {
            u8 action = s->bagActions[i];
            const u8 *label = action == BAG_ACT_GIVE      ? gMenuText_Give
                            : action == BAG_ACT_TOSS      ? gMenuText_Toss
                            : action == BAG_ACT_CHECK     ? gMenuText_Check
                            : action == BAG_ACT_CHECK_TAG ? gMenuText_CheckTag
                            : action == BAG_ACT_REGISTER  ? (s->registered != ITEM_NONE && s->registered == s->bagSelItem
                                                             ? gMenuText_Deselect : gMenuText_Register)
                            : (s->bagSelItem == ITEM_MACH_BIKE || s->bagSelItem == ITEM_ACRO_BIKE) && s->bagRiding
                                                          ? gMenuText_Walk
                                                          : gMenuText_Use;

            DrawTrayButton(TRAY_X(i), 190, label, s->pressed == HIT_BAG_ACT + i,
                           action == BAG_ACT_CHECK_TAG || s->canEdit, TRUE, HIT_BAG_ACT + i);
        }
    if (s->bagGive == PARTY_NO_SWAP)
    {
        u8 label[24];

        /* SWITCH ITEM on one line. */
        StringCopy(label, Ascii(CTR_TEXT("SWITCH\nITEM", "CAMBIAR\nOBJETO", "ÉCHANGER\nOBJET", "SCAMBIA\nSTRUM.")));
        for (u8 *c = label; *c != EOS; ++c)
            if (*c == CHAR_NEWLINE)
                *c = CHAR_SPACE;
        DrawTrayButtonW(4, 214, 14, label, s->pressed == HIT_BAG_SWITCH || s->bagSwap != BAG_NO_ROW,
                        swappable && item && s->canEdit, FALSE, HIT_BAG_SWITCH);
    }
    DrawTrayButton(TRAY_X(3), 214, gText_Cancel2, s->pressed == HIT_BAG_CANCEL, TRUE, FALSE, HIT_BAG_CANCEL);
}

/*
 * The PokéNav as pokenav_*.c draws it on the GBA's 240x160, at the top of
 * the bottom screen: its BG layers back to front (BG3 the dots or the list,
 * BG2 the device or Match Call's UI, BG1 the message box or the call, BG0 the
 * header), each OBJ priority's sprites between them, as the GBA shows them.
 * Under it, the dots go on, with the taps that take the keys' place.
 */
static void DrawNavLayerAt(const u8 *tiles, u32 count, const u16 *map, int stride, int sy, int rows, const Pal *pals,
                           int tileBase, int dy)
{
    if (!tiles || !map)
        return;
    for (int ty = 0; ty < rows; ++ty)
        for (int tx = 0; tx < 30; ++tx)
        {
            u16 e = map[(sy + ty) * stride + tx];
            int tile = (e & 0x3FF) - tileBase;

            if (tile >= 0 && (u32)tile < count && (e >> 12) < 6)
                DrawTile(tiles + tile * 32, tx * 8, dy + ty * 8, pals[e >> 12].c, (e >> 10) & 1, (e >> 11) & 1);
        }
}

static void DrawNavLayer(const u8 *tiles, u32 count, const u16 *map, int stride, int sy, int rows, const Pal *pals,
                         int tileBase)
{
    DrawNavLayerAt(tiles, count, map, stride, sy, rows, pals, tileBase, 0);
}

/* The description box goes down into the bottom screen's extra lines. */
#define NAV_MESSAGE_DY 64

/* The dots (BG3) under the whole screen: their 20 rows again below. */
static void DrawNavDots(void)
{
    for (int y0 = 0; y0 < H; y0 += 160)
        for (int ty = 0; ty < 20 && y0 + ty * 8 < H; ++ty)
            for (int tx = 0; tx < 30; ++tx)
            {
                u16 e = sRes.navDotsMap[ty * 32 + tx];

                if ((e & 0x3FF) < sRes.navDotsCount)
                    DrawTile(sRes.navDotsTiles + (e & 0x3FF) * 32, tx * 8, y0 + ty * 8, sRes.navPal[3].c, (e >> 10) & 1,
                             (e >> 11) & 1);
            }
}

/* A left header (two 64x32 sprites, the second from tile 32). */
static void DrawNavLeftHeader(const u8 *tiles, const Pal *pal, int y)
{
    if (!tiles)
        return;
    DrawSprite(tiles, 8, 4, 0, y, pal->c);
    DrawSprite(tiles + 32 * 32, 8, 4, 64, y, pal->c);
}

/* A palette made lighter by `level` sixteenths (BLDCNT lighten, BLDY). */
static void LightenPal(Pal *dst, const Pal *src, int level)
{
    for (int i = 0; i < 16; ++i)
    {
        u16 c = src->c[i];
        int r = c >> 11, g = (c >> 5) & 63, b = c & 31;

        r += (31 - r) * level / 16;
        g += (63 - g) * level / 16;
        b += (31 - b) * level / 16;
        dst->c[i] = (r << 11) | (g << 5) | b;
    }
}

/* An option of a PokéNav menu: its label in options.4bpp, its palette row
 * in options.gbapal, its description (pokenav_menu_handler_gfx.c). */
typedef struct
{
    u16 tile;
    u8 pal;
    const u8 *description;
} NavOption;

static const NavOption sNavMainOptions[] = {
    [NAV_OPT_MAP]        = {0x000, 0, gText_CheckMapOfHoenn},
    [NAV_OPT_CONDITION]  = {0x020, 1, gText_CheckPokemonInDetail},
    [NAV_OPT_MATCH_CALL] = {0x040, 4, gText_CallRegisteredTrainer},
    [NAV_OPT_RIBBONS]    = {0x060, 2, gText_CheckObtainedRibbons},
    [NAV_OPT_SWITCH_OFF] = {0x080, 3, gText_PutAwayPokenav},
};
static const NavOption sNavSearchOptions[] = {
    [CONTEST_CATEGORY_COOL]   = {0x0E0, 4, gText_FindCoolPokemon},
    [CONTEST_CATEGORY_BEAUTY] = {0x100, 1, gText_FindBeautifulPokemon},
    [CONTEST_CATEGORY_CUTE]   = {0x120, 2, gText_FindCutePokemon},
    [CONTEST_CATEGORY_SMART]  = {0x140, 0, gText_FindSmartPokemon},
    [CONTEST_CATEGORY_TOUGH]  = {0x160, 0, gText_FindToughPokemon},
    [SEARCH_OPT_CANCEL]       = {0x180, 3, gText_ReturnToConditionMenu},
};
/* The categories' headers' palette rows (left_headers/palette.gbapal). */
static const u8 sCategoryHeaderPal[CONTEST_CATEGORIES_COUNT] = {4, 1, 2, 0, 0};
static const NavOption sNavConditionOptions[] = {
    [COND_OPT_PARTY]  = {0x0A0, 1, gText_CheckPartyPokemonInDetail},
    [COND_OPT_SEARCH] = {0x0C0, 1, gText_CheckAllPokemonInDetail},
    [COND_OPT_CANCEL] = {0x180, 3, gText_ReturnToPokenavMenu},
};

/*
 * A PokéNav menu: the dots (purple in the CONDITION menus), the device,
 * the options from yStart every deltaY (the one picked 10 px left and lit:
 * its glow at mid strength), the blue light, the description box, the
 * left header, the top header and the PokéNav's icon.
 */
static void DrawNavOptions(const ViewState *s, const NavOption *table, int yStart, int deltaY, const u8 *leftHeader,
                           const Pal *leftPal, bool8 purple)
{
    const Pal *box = &sRes.navPal[1];
    const u8 *description = table[s->navOptions[s->navCursor]].description;
    Pal dots[6];
    int width;

    memcpy(dots, sRes.navPal, sizeof(dots));
    if (purple)
    {
        /* ChangeBgDotsColorToPurple: colours 7 and 8 as 1 and 2. */
        dots[3].c[1] = sRes.navPal[3].c[7];
        dots[3].c[2] = sRes.navPal[3].c[8];
    }
    for (int y0 = 0; y0 < H; y0 += 160)
        for (int ty = 0; ty < 20 && y0 + ty * 8 < H; ++ty)
            for (int tx = 0; tx < 30; ++tx)
            {
                u16 e = sRes.navDotsMap[ty * 32 + tx];

                if ((e & 0x3FF) < sRes.navDotsCount)
                    DrawTile(sRes.navDotsTiles + (e & 0x3FF) * 32, tx * 8, y0 + ty * 8, dots[3].c, (e >> 10) & 1, (e >> 11) & 1);
            }
    DrawNavLayer(sRes.navDeviceTiles, sRes.navDeviceCount, sRes.navDeviceMap, 32, 0, 20, sRes.navPal, 0);
    for (int i = 0; i < s->navOptionCount; ++i)
    {
        const NavOption *option = &table[s->navOptions[i]];
        bool8 picked = i == s->navCursor;
        int x = picked ? 114 : 124, y = yStart - 8 + deltaY * i;
        Pal pal = sRes.navOptionPal[option->pal];

        if (picked)
            LightenPal(&pal, &sRes.navOptionPal[option->pal], 4);
        for (int j = 0; j < 4 && sRes.navOptionTiles; ++j)
            DrawSprite(sRes.navOptionTiles + (option->tile + 8 * j) * 32, 4, 2, x + 32 * j, y + (s->pressed == HIT_NAV_OPTION + i),
                       pal.c);
        AddHit(112, y - 2, 128, deltaY, HIT_NAV_OPTION + i);
    }
    if (s->navLight == 1 && sRes.navBlueLight)
        DrawSprite(sRes.navBlueLight, 4, 2, 0, 88, sRes.navBlueLightPal.c);
    DrawNavLayerAt(sRes.navMessageTiles, sRes.navMessageCount, sRes.navMessageMap, 32, 0, 20, sRes.navPal, 0, NAV_MESSAGE_DY);
    /* The description window at (3, 17): its fill and text {6, 8, 7}. */
    FillRect(24, 136 + NAV_MESSAGE_DY, 192, 16, box->c[6]);
    width = StrWidth(&sNormal, description);
    DrawStr(&sNormal, description, 24 + (192 - width) / 2, 137 + NAV_MESSAGE_DY, box->c[8], box->c[7]);
    DrawNavLeftHeader(leftHeader, leftPal, 32);
    /* SEARCH's own header under CONDITION's (centre y 66). */
    if (table == sNavSearchOptions && sRes.navLeftSearch)
    {
        DrawSprite(sRes.navLeftSearch, 4, 2, 0, 58, sRes.navLeftPal[1].c);
        DrawSprite(sRes.navLeftSearch + 8 * 32, 4, 2, 32, 58, sRes.navLeftPal[1].c);
    }
    DrawNavLayer(sRes.navHeaderTiles, sRes.navHeaderCount, sRes.navHeaderMap, 32, 0, 20, sRes.navPal, 0);
    if (sRes.column[SCR_POKENAV].tiles)
        DrawSprite(sRes.column[SCR_POKENAV].tiles, 4, 4, 204, -4, sRes.column[SCR_POKENAV].pal.c);
}

static void DrawNavMenu(const ViewState *s)
{
    DrawNavOptions(s, sNavMainOptions, 42, 20, sRes.navLeftMain, &sRes.navLeftPal[3], FALSE);
}

/* A pixel of BG3 under the graph's polygon: BG2's green blended over it
 * (BLDALPHA 11/4), out = (11 * green + 4 * BG3) / 16 per channel. */
static void BlendGraphPixel(int x, int y)
{
    u16 *p, c;
    int r, g, b;

    x += sOX;
    if (x < sClipX0 || x >= sClipX1 || y < sClipY0 || y >= sClipY1)
        return;
    p = &sDst[x * H + (H - 1 - y)];
    c = *p;
    r = (4 * (c >> 11)) / 16;
    g = (11 * 31 + 4 * ((c >> 6) & 31)) / 16;
    b = (4 * (c & 31)) / 16;
    if (g > 31)
        g = 31;
    *p = (r << 11) | (g << 6) | b;
}

/* The polygon of the five conditions (ConditionGraph_CalcPositions): its
 * rows 56-120, each on the scanline above it as the game's windows show it. */
static void DrawConditionGraph(const u8 *conditions)
{
    static const u8 order[] = {CONDITION_COOL, CONDITION_BEAUTY, CONDITION_CUTE, CONDITION_SMART, CONDITION_TOUGH};
    struct UCoords16 v[CONDITION_COUNT];
    u8 values[CONDITION_COUNT];

    memcpy(values, conditions, sizeof(values));
    ConditionGraph_CalcPositions(values, v);
    for (int row = CONDITION_GRAPH_TOP_Y; row <= CONDITION_GRAPH_BOTTOM_Y; ++row)
    {
        int x0 = 1000, x1 = -1000;

        for (int i = 0; i < CONDITION_COUNT; ++i)
        {
            const struct UCoords16 *a = &v[order[i]], *b = &v[order[(i + 1) % CONDITION_COUNT]];
            int ay = a->y, by = b->y;

            if ((row < ay && row < by) || (row > ay && row > by))
                continue;
            if (ay == by)
            {
                x0 = min(x0, min(a->x, b->x));
                x1 = max(x1, max(a->x, b->x));
                continue;
            }
            {
                int x = a->x + ((row - ay) * ((int)b->x - a->x) + (by - ay) / 2) / (by - ay);

                x0 = min(x0, x);
                x1 = max(x1, x);
            }
        }
        for (int x = x0; x <= x1; ++x)
            BlendGraphPixel(x, row - 1);
    }
}

/*
 * The party's condition screen (pokenav_conditions_gfx.c): the graph's
 * art (BG3) with the polygon in it, the Pokémon's picture and name, its
 * sheen's sparkles, the party's balls and CANCEL on the right, the headers.
 * Under it, the dots and the taps that take the keys' place.
 */
static int DrawRibbonGender(u8 gender, u8 level, int x, int y, const Pal *pal);
static void DrawNavSubHeader(const u8 *tiles, const Pal *pal, int y);

static void DrawConditionScreen(const ViewState *s)
{
    static const u8 up[] = {CHAR_UP_ARROW, EOS}, down[] = {CHAR_DOWN_ARROW, EOS};
    static const s8 sparkles[10][2] = {
        {0, -35}, {20, -28}, {33, -10}, {33, 10}, {20, 28}, {0, 35}, {-20, 28}, {-33, 10}, {-33, -10}, {-20, -28},
    };
    bool8 search = s->navView == PNAV_SEARCH_CONDITION;
    bool8 cancel = search ? s->searchCount == 0 : s->condSel >= s->condCount;
    const Pal *text = &sRes.condTextPal;
    Pal dots[6];

    memcpy(dots, sRes.navPal, sizeof(dots));
    dots[3].c[1] = sRes.navPal[3].c[7];
    dots[3].c[2] = sRes.navPal[3].c[8];
    for (int ty = 20; ty < H / 8; ++ty)
        for (int tx = 0; tx < 30; ++tx)
        {
            u16 e = sRes.navDotsMap[(ty - 20) * 32 + tx];

            if ((e & 0x3FF) < sRes.navDotsCount)
                DrawTile(sRes.navDotsTiles + (e & 0x3FF) * 32, tx * 8, ty * 8, dots[3].c, (e >> 10) & 1, (e >> 11) & 1);
        }
    /* BG3, the graph's art; BG2's green inside the polygon. */
    DrawNavLayer(sRes.condTiles, sRes.condCount, sRes.condMap, 32, 0, 20, sRes.condPal, 0);
    /* Search mode: the options patch (options.bin, 9x4) at BG3 tile (0, 5). */
    if (search && sRes.searchPatch)
        for (int ty = 0; ty < 4; ++ty)
            for (int tx = 0; tx < 9; ++tx)
            {
                u16 e = sRes.searchPatch[ty * 9 + tx];

                if ((e & 0x3FF) < sRes.condCount && (e >> 12) < 6)
                    DrawTile(sRes.condTiles + (e & 0x3FF) * 32, tx * 8, 40 + ty * 8, sRes.condPal[e >> 12].c, (e >> 10) & 1,
                             (e >> 11) & 1);
            }
    if (!cancel)
        DrawConditionGraph(s->conditions);
    /* OBJ priority 2: the party's balls (the picked one's top half), the
     * empty places, CANCEL (lit when picked). */
    for (int i = 0; !search && i < s->condCount && sRes.condBall; ++i)
    {
        DrawSprite(sRes.condBall + (i == s->condSel ? 0 : 4) * 32, 2, 2, 218, 20 * i, sRes.condCancelPal[0].c);
        AddHit(212, 20 * i - 2, 28, 20, HIT_ROW + i);
    }
    for (int i = s->condCount; !search && i < PARTY_SIZE && sRes.condPlace; ++i)
        DrawTile(sRes.condPlace, 222, 20 * i, sRes.condCancelPal[0].c, FALSE, FALSE);
    if (!search && sRes.condCancel)
    {
        DrawSprite(sRes.condCancel, 4, 2, 214, 120, sRes.condCancelPal[cancel ? 0 : 1].c);
        AddHit(212, 118, 28, 20, HIT_ROW + s->condCount);
    }
    if (!cancel && search)
    {
        int n = s->sheen == 255 ? 9 : s->sheen / 29;
        u8 line[16];

        if (sSumPic.valid)
            DrawSprite(sSumPic.tiles, 8, 8, 6, 72, sSumPic.pal.c);
        DrawStr(&sNormal, s->ribNick, 104, 9, text->c[8], text->c[9]);
        if (s->ribGender == MON_MALE)
            DrawStr(&sNormal, gText_MaleSymbol, 104 + 60, 9, text->c[4], text->c[5]);
        else if (s->ribGender == MON_FEMALE)
            DrawStr(&sNormal, gText_FemaleSymbol, 104 + 60, 9, text->c[6], text->c[7]);
        line[0] = CHAR_SLASH;
        StringCopy(StringCopy(line + 1, gText_LevelSymbol), Number(s->ribLevel, 3, STR_CONV_MODE_LEFT_ALIGN));
        DrawStr(&sNormal, line, 104 + 68, 9, text->c[8], text->c[9]);
        /* Where it is (IN PARTY or the box), and its rank at (8, 48). */
        DrawStr(&sNormal, s->searchBox == RIB_PARTY_BOX ? gText_InParty : GetBoxNamePtr(s->searchBox), 104, 25, text->c[8],
                text->c[9]);
        DrawStr(&sNormal, gText_Number2, 8 + 4, 49, text->c[8], text->c[9]);
        DrawStrRight(&sNormal, Number(s->searchMonRank, 4, STR_CONV_MODE_RIGHT_ALIGN), 8 + 56, 49, text->c[8], text->c[9]);
        for (int k = 0; k <= n && sRes.condSparkle; ++k)
            DrawSprite(sRes.condSparkle + 8 * 32, 2, 2, 38 + sparkles[k][0] - 8, 104 + sparkles[k][1] - 8, sRes.condSparklePal.c);
    }
    if (!cancel && !search)
    {
        const MonView *m = &s->party[s->condSlots[s->condSel]];
        u8 line[16];
        int n = s->sheen == 255 ? 9 : s->sheen / 29;

        /* OBJ priority 1: the picture (not flipped here). */
        if (sSumPic.valid)
            DrawSprite(sSumPic.tiles, 8, 8, 6, 72, sSumPic.pal.c);
        /* BG1: CopyConditionMonNameGender at (104, 9), the gender at x 60. */
        DrawStr(&sNormal, m->nick, 104, 9, text->c[8], text->c[9]);
        if (m->gender == MON_MALE)
            DrawStr(&sNormal, gText_MaleSymbol, 104 + 60, 9, text->c[4], text->c[5]);
        else if (m->gender == MON_FEMALE)
            DrawStr(&sNormal, gText_FemaleSymbol, 104 + 60, 9, text->c[6], text->c[7]);
        line[0] = CHAR_SLASH;
        StringCopy(StringCopy(line + 1, gText_LevelSymbol), Number(m->level, 3, STR_CONV_MODE_LEFT_ALIGN));
        DrawStr(&sNormal, line, 104 + 68, 9, text->c[8], text->c[9]);
        /* OBJ priority 0: the sheen's sparkles round the picture. */
        for (int k = 0; k <= n && sRes.condSparkle; ++k)
            DrawSprite(sRes.condSparkle + 8 * 32, 2, 2, 38 + sparkles[k][0] - 8, 104 + sparkles[k][1] - 8, sRes.condSparklePal.c);
    }
    /* The headers: CONDITION (main) and PARTY under it, then BG0 from row 4. */
    DrawNavLeftHeader(sRes.navLeftCondition, &sRes.navLeftPal[1], 0);
    if (search)
        DrawNavSubHeader(sRes.navLeftCategory[s->searchCat], &sRes.navLeftPal[sCategoryHeaderPal[s->searchCat]], 26);
    else
        DrawNavSubHeader(sRes.navLeftParty, &sRes.navLeftPal[1], 26);
    DrawNavLayer(sRes.navHeaderTiles, sRes.navHeaderCount, sRes.navHeaderMap, 32, 4, 20, sRes.condPal, 0);
    FillRect(8, 144, 128, 16, sRes.navPal[0].c[4]);
    FillRect(8, 144, 128, 1, sRes.navPal[0].c[5]);

    /* The keys' work by touch: the Pokémon before and after, back. */
    DrawTallButton(2, 172, 7, 64, up, s->pressed == HIT_NAV_UP, TRUE, HIT_NAV_UP);
    DrawTallButton(62, 172, 7, 64, down, s->pressed == HIT_NAV_DOWN, TRUE, HIT_NAV_DOWN);
    DrawTallButton(122, 172, 14, 64, gText_Cancel, s->pressed == HIT_NAV_BACK, TRUE, HIT_NAV_BACK);
}

/* The call's message: its pages of two lines (a new page at \p, as the
 * PokéNav's call window clears). */
static void McPaginate(void)
{
    const u8 *p = sMcMessage;
    int lines = 0;

    sMcPages = 1;
    sMcPageStart[0] = 0;
    while (*p != EOS && sMcPages < ARRAY_COUNT(sMcPageStart))
    {
        u8 c = *p++;

        if (c == EXT_CTRL_CODE_BEGIN && *p != EOS)
        {
            p += GetExtCtrlCodeLength(*p);
            continue;
        }
        if (c == CHAR_PROMPT_CLEAR || ((c == CHAR_NEWLINE || c == CHAR_PROMPT_SCROLL) && ++lines == 2))
        {
            sMcPageStart[sMcPages++] = p - sMcMessage;
            lines = 0;
        }
    }
    if (sMcPageStart[sMcPages - 1] == p - sMcMessage && sMcPages > 1)
        --sMcPages;
}

static void DrawMcPage(int page, int x, int y, u16 fg, u16 sh)
{
    const u8 *p = sMcMessage + sMcPageStart[page];
    int lines = 0, left = x;

    while (*p != EOS)
    {
        u16 g;

        if (page + 1 < sMcPages && p - sMcMessage >= sMcPageStart[page + 1])
            break;
        if (*p == CHAR_PROMPT_CLEAR)
            break;
        g = NextGlyph(&p);
        if (g == 0xFFFF)
            break;
        if (g == 0xFFFE)
        {
            if (++lines == 2)
                break;
            x = left;
            y += sNormal.lineHeight;
            continue;
        }
        x += DrawGlyph(&sNormal, g, x, y, fg, sh);
    }
}

static void DrawMatchCall(const ViewState *s)
{
    static const u8 up[] = {CHAR_UP_ARROW, EOS}, down[] = {CHAR_DOWN_ARROW, EOS};
    const Pal *ui = &sRes.navMcPal[2], *list = &sRes.navMcPal[3];
    u8 text[64];

    /* BG3, the list: its window colour, the rows, the rematch balls. */
    DrawNavDots();
    FillRect(104, 0, 136, 160, list->c[1]);
    for (int i = 0; i < MC_ROWS && s->mcTop + i < s->mcCount; ++i)
    {
        struct PokenavMatchCallEntry e = {(s->mcEntry[i] & 0x8000) != 0, 0, s->mcEntry[i] & 0x7FFF};
        int y = 8 + 16 * i;

        BufferMatchCallNameAndDesc(&e, text);
        DrawBagText(&sNarrow, text, 104 + 8, y + 1, list->c[TEXT_COLOR_DARK_GRAY], list->c[TEXT_COLOR_LIGHT_GRAY]);
        if ((s->mcRematch & (1 << i)) && sRes.mcPokeball)
        {
            DrawTile(sRes.mcPokeball, 232, y, sRes.navMcPal[5].c, FALSE, FALSE);
            DrawTile(sRes.mcPokeball + 32, 232, y + 8, sRes.navMcPal[5].c, FALSE, FALSE);
        }
        AddHit(104, y, 136, 16, HIT_ROW + i);
    }
    /* BG2, the UI, and its windows: where the trainer is, the record. */
    DrawNavLayer(sRes.mcUiTiles, sRes.mcUiCount, sRes.mcUiMap, 32, 0, 20, sRes.navMcPal, 0x80);
    if (s->mcSel != MC_NO_ENTRY)
    {
        const u8 *place = text;

        if (s->mcMapsec == MAPSEC_NONE)
            place = gText_Unknown;
        else
            GetMapName(text, s->mcMapsec, 0);
        DrawStr(&sNarrow, place, (88 - StrWidth(&sNarrow, place)) / 2, 41, ui->c[TEXT_COLOR_DARK_GRAY], ui->c[TEXT_COLOR_LIGHT_GRAY]);
    }
    if (!s->mcBox)
    {
        DrawStr(&sNarrow, gText_NumberRegistered, 2, 73, ui->c[TEXT_COLOR_DARK_GRAY], ui->c[TEXT_COLOR_LIGHT_GRAY]);
        DrawStrRight(&sNarrow, Number(s->mcCount, 3, STR_CONV_MODE_LEFT_ALIGN), 86, 89, ui->c[TEXT_COLOR_DARK_GRAY],
                     ui->c[TEXT_COLOR_LIGHT_GRAY]);
        DrawStr(&sNarrow, gText_NumberOfBattles, 2, 105, ui->c[TEXT_COLOR_DARK_GRAY], ui->c[TEXT_COLOR_LIGHT_GRAY]);
        DrawStrRight(&sNarrow, Number(s->mcBattles, 5, STR_CONV_MODE_LEFT_ALIGN), 86, 121, ui->c[TEXT_COLOR_DARK_GRAY],
                     ui->c[TEXT_COLOR_LIGHT_GRAY]);
    }
    else
    {
        /* The options box: CALL and CANCEL (CHECK to come), the cursor. */
        DrawStr(&sNarrow, gText_Call, 16, 73, ui->c[TEXT_COLOR_DARK_GRAY], ui->c[TEXT_COLOR_LIGHT_GRAY]);
        DrawStr(&sNarrow, gText_Cancel6, 16, 89, ui->c[TEXT_COLOR_DARK_GRAY], ui->c[TEXT_COLOR_LIGHT_GRAY]);
        if (sRes.mcCursor)
            DrawSprite(sRes.mcCursor, 1, 2, 0, 72 + 16 * s->mcBoxCursor, sRes.mcCursorPal.c);
        AddHit(0, 72, 88, 16, HIT_NAV_CALL);
        AddHit(0, 88, 88, 16, HIT_NAV_BOX_CANCEL);
    }
    /* OBJ priority 2: the list arrows. */
    if (sRes.mcArrows)
    {
        if (s->mcSel != MC_NO_ENTRY && s->mcSel >= s->mcTop && s->mcSel < s->mcTop + MC_ROWS)
            DrawSprite(sRes.mcArrows, 1, 2, 103, 8 + 16 * (s->mcSel - s->mcTop), sRes.mcArrowPal.c);
        if (s->mcTop + MC_ROWS < s->mcCount)
            DrawSprite(sRes.mcArrows + 2 * 32, 2, 1, 156, 132, sRes.mcArrowPal.c);
        if (s->mcTop > 0)
            DrawSprite(sRes.mcArrows + 4 * 32, 2, 1, 156, 4, sRes.mcArrowPal.c);
    }
    /* BG1: the call, in its frame at rows 11-16 (match_call.c), the window at
     * (1, 12) 28x4, the PokéNav's icon at its left. */
    if (s->mcCall && sRes.mcFrame)
    {
        const Pal *frame = &sRes.navMcPal[4], *window = &sRes.navMcPal[1];

        for (int tx = 0; tx < 30; ++tx)
            for (int ty = 11; ty <= 16; ++ty)
            {
                int tile;

                if (ty == 11)
                    tile = tx == 0 ? 1 : tx == 29 ? 3 : 2;
                else if (ty == 16)
                    tile = tx == 0 ? 6 : tx == 29 ? 8 : 7;
                else if (tx == 0 || tx == 29)
                    tile = tx == 0 ? 4 : 5;
                else
                    continue;
                DrawTile(sRes.mcFrame + (tile - 1) * 32, tx * 8, ty * 8, frame->c, FALSE, FALSE);
            }
        FillRect(8, 96, 224, 32, window->c[1]);
        DrawMcPage(s->mcPage, 8 + 32, 97, window->c[TEXT_COLOR_DARK_GRAY], window->c[TEXT_COLOR_LIGHT_GRAY]);
        if (sRes.column[SCR_POKENAV].tiles)
            DrawSprite(sRes.column[SCR_POKENAV].tiles, 4, 4, 8, 96, sRes.column[SCR_POKENAV].pal.c);
        AddHit(0, 88, 240, 48, HIT_NAV_MESSAGE);
    }
    /* OBJ priority 1: the left header (match_call, y 16), then BG0 from row 4. */
    DrawNavLeftHeader(sRes.navLeftMatchCall, &sRes.navLeftPal[4], 0);
    DrawNavLayer(sRes.navHeaderTiles, sRes.navHeaderCount, sRes.navHeaderMap, 32, 4, 20, sRes.navMcPal, 0);
    /* The help bar's window at (8, 144): its fill (no keys to tell of). */
    FillRect(8, 144, 128, 16, sRes.navPal[0].c[4]);
    FillRect(8, 144, 128, 1, sRes.navPal[0].c[5]);

    /* The keys' work by touch. */
    DrawTallButton(2, 172, 7, 64, up, s->pressed == HIT_NAV_UP, s->mcTop > 0 && !s->mcCall, HIT_NAV_UP);
    DrawTallButton(62, 172, 7, 64, down, s->pressed == HIT_NAV_DOWN, s->mcTop + MC_ROWS < s->mcCount && !s->mcCall,
                   HIT_NAV_DOWN);
    DrawTallButton(122, 172, 14, 64, gText_Cancel, s->pressed == HIT_NAV_BACK, !s->mcCall, HIT_NAV_BACK);
}

/* A sub-header (two 32x16 sprites) under the main one, at y. */
static void DrawNavSubHeader(const u8 *tiles, const Pal *pal, int y)
{
    if (!tiles)
        return;
    DrawSprite(tiles, 4, 2, 0, y, pal->c);
    DrawSprite(tiles + 8 * 32, 4, 2, 32, y, pal->c);
}

/* The search's results (pokenav_conditions_search_results.c): the frame
 * (BG1) over the list's window (BG2), the rows, the rank of the one picked,
 * the cursor and the arrows, the CONDITION and category headers. */
static void DrawSearchResults(const ViewState *s)
{
    static const u8 up[] = {CHAR_UP_ARROW, EOS}, down[] = {CHAR_DOWN_ARROW, EOS};
    const Pal *list = &sRes.searchPal[2];

    FillRect(0, 0, CW, H, sRes.navPal[0].c[0]);
    DrawNavDots();
    FillRect(104, 0, 136, 160, list->c[1]);
    for (int i = 0; i < 8 && s->searchTop + i < s->searchCount; ++i)
    {
        u8 box = s->searchRow[i] >> 8, place = s->searchRow[i] & 0xFF;
        struct BoxPokemon *mon = box == RIB_PARTY_BOX ? &gPlayerParty[place].box : GetBoxedMonPtr(box, place);
        u8 nick[POKEMON_NAME_LENGTH + 2];
        int y = 9 + 16 * i;

        GetBoxMonData(mon, MON_DATA_NICKNAME, nick);
        StringGet_Nickname(nick);
        DrawStr(&sNormal, nick, 112, y, list->c[TEXT_COLOR_DARK_GRAY], list->c[TEXT_COLOR_LIGHT_GRAY]);
        DrawRibbonGender(GetBoxMonGender(mon), GetLevelFromBoxMonExp(mon), 112 + 60, y, list);
        AddHit(104, y - 1, 136, 16, HIT_ROW + i);
    }
    DrawNavLayer(sRes.searchTiles, sRes.searchCount, sRes.searchMap, 32, 0, 20, sRes.searchPal, 0);
    if (s->searchCount)
    {
        u16 rank = 1;

        for (int i = 0; i < 8; ++i)
            if (s->searchTop + i == s->searchSel)
                rank = s->searchRank[i];
        DrawStr(&sNormal, gText_NumberIndex, 12, 49, sRes.searchPal[1].c[TEXT_COLOR_DARK_GRAY], sRes.searchPal[1].c[TEXT_COLOR_LIGHT_GRAY]);
        DrawStr(&sNormal, Number(rank, 3, STR_CONV_MODE_RIGHT_ALIGN), 42, 49, sRes.searchPal[1].c[TEXT_COLOR_DARK_GRAY],
                sRes.searchPal[1].c[TEXT_COLOR_LIGHT_GRAY]);
    }
    if (sRes.mcArrows)
    {
        if (s->searchSel >= s->searchTop && s->searchSel < s->searchTop + 8)
            DrawSprite(sRes.mcArrows, 1, 2, 103, 8 + 16 * (s->searchSel - s->searchTop), sRes.mcArrowPal.c);
        if (s->searchTop + 8 < s->searchCount)
            DrawSprite(sRes.mcArrows + 2 * 32, 2, 1, 160, 132, sRes.mcArrowPal.c);
        if (s->searchTop > 0)
            DrawSprite(sRes.mcArrows + 4 * 32, 2, 1, 160, 4, sRes.mcArrowPal.c);
    }
    DrawNavLeftHeader(sRes.navLeftCondition, &sRes.navLeftPal[1], 0);
    DrawNavSubHeader(sRes.navLeftCategory[s->searchCat], &sRes.navLeftPal[sCategoryHeaderPal[s->searchCat]], 26);
    DrawNavLayer(sRes.navHeaderTiles, sRes.navHeaderCount, sRes.navHeaderMap, 32, 4, 20, sRes.searchPal, 0);
    FillRect(8, 144, 128, 16, sRes.navPal[0].c[4]);
    FillRect(8, 144, 128, 1, sRes.navPal[0].c[5]);

    DrawTallButton(2, 172, 7, 64, up, s->pressed == HIT_NAV_UP, s->searchTop > 0, HIT_NAV_UP);
    DrawTallButton(62, 172, 7, 64, down, s->pressed == HIT_NAV_DOWN, s->searchTop + 8 < s->searchCount, HIT_NAV_DOWN);
    DrawTallButton(122, 172, 14, 64, gText_Cancel, s->pressed == HIT_NAV_BACK, TRUE, HIT_NAV_BACK);
}

/* The ribbons' descriptions (src/data/text/ribbon_descriptions.h). */
extern const u8 *const gRibbonDescriptionPointers[][2];
extern const u8 *const gGiftRibbonDescriptionPointers[MAX_GIFT_RIBBON][2];

/* sRibbonGfxData: each ribbon's icon and its palette (slots 2-6). */
static const u8 sRibbonGfx[FIRST_GIFT_RIBBON + 7][2] = {
    [CHAMPION_RIBBON] = {0, 2},
    [COOL_RIBBON_NORMAL] = {1, 2}, {2, 2}, {3, 2}, {4, 2},
    [BEAUTY_RIBBON_NORMAL] = {1, 3}, {2, 3}, {3, 3}, {4, 3},
    [CUTE_RIBBON_NORMAL] = {1, 4}, {2, 4}, {3, 4}, {4, 4},
    [SMART_RIBBON_NORMAL] = {1, 5}, {2, 5}, {3, 5}, {4, 5},
    [TOUGH_RIBBON_NORMAL] = {1, 6}, {2, 6}, {3, 6}, {4, 6},
    [WINNING_RIBBON] = {5, 2}, {6, 2}, {7, 3}, {8, 4},
    [FIRST_GIFT_RIBBON] = {9, 3}, {9, 5}, {9, 6}, {10, 5}, {10, 6}, {11, 2}, {11, 3},
};

/* The gender and level of a ribbons row or name line: ♂ and ♀ in their
 * colours, then "/Lv" and the level; returns the x it ends at. */
static int DrawRibbonGender(u8 gender, u8 level, int x, int y, const Pal *pal)
{
    u8 line[12];

    if (gender == MON_MALE)
        x = DrawStr(&sNormal, gText_MaleSymbol, x, y, pal->c[TEXT_COLOR_LIGHT_RED], pal->c[TEXT_COLOR_GREEN]);
    else if (gender == MON_FEMALE)
        x = DrawStr(&sNormal, gText_FemaleSymbol, x, y, pal->c[TEXT_COLOR_LIGHT_GREEN], pal->c[TEXT_COLOR_BLUE]);
    line[0] = CHAR_SLASH;
    StringCopy(StringCopy(line + 1, gText_LevelSymbol), Number(level, 3, STR_CONV_MODE_LEFT_ALIGN));
    return DrawStr(&sNormal, line, x, y, pal->c[TEXT_COLOR_DARK_GRAY], pal->c[TEXT_COLOR_LIGHT_GRAY]);
}

/* "%3d/%3d", centred in the 56 px of its window at (x, y). */
static void DrawRibbonIndex(int x, int y, u16 index, u16 count, const Pal *pal)
{
    u8 text[12];

    StringCopy(StringCopy(StringCopy(text, Number(index, 3, STR_CONV_MODE_RIGHT_ALIGN)), gText_Slash),
               Number(count, 3, STR_CONV_MODE_RIGHT_ALIGN));
    DrawStr(&sNormal, text, x + (56 - StrWidth(&sNormal, text)) / 2, y, pal->c[TEXT_COLOR_DARK_GRAY], pal->c[TEXT_COLOR_LIGHT_GRAY]);
}

static void DrawRibbonList(const ViewState *s)
{
    static const u8 up[] = {CHAR_UP_ARROW, EOS}, down[] = {CHAR_DOWN_ARROW, EOS};
    const Pal *ui = &sRes.ribListPal[2];

    DrawNavDots();
    /* BG2, the list's window colour; its rows. */
    FillRect(104, 0, 136, 160, ui->c[1]);
    for (int i = 0; i < 8 && s->ribTop + i < s->ribCount; ++i)
    {
        u8 box = s->ribRow[i] >> 8, place = s->ribRow[i] & 0xFF;
        struct BoxPokemon *mon = box == RIB_PARTY_BOX ? &gPlayerParty[place].box : GetBoxedMonPtr(box, place);
        u8 nick[POKEMON_NAME_LENGTH + 2];
        int y = 9 + 16 * i;

        GetBoxMonData(mon, MON_DATA_NICKNAME, nick);
        StringGet_Nickname(nick);
        DrawStr(&sNormal, nick, 112, y, ui->c[TEXT_COLOR_DARK_GRAY], ui->c[TEXT_COLOR_LIGHT_GRAY]);
        DrawRibbonGender(GetBoxMonGender(mon), GetLevelFromBoxMonExp(mon), 172, y, ui);
        DrawStr(&sNormal, Number(s->ribRowCount[i], 2, STR_CONV_MODE_RIGHT_ALIGN), 226, y, ui->c[TEXT_COLOR_DARK_GRAY],
                ui->c[TEXT_COLOR_LIGHT_GRAY]);
        AddHit(104, y - 1, 136, 16, HIT_ROW + i);
    }
    /* BG1, the list's art over it; its index window (never filled). */
    DrawNavLayer(sRes.ribListTiles, sRes.ribListCount, sRes.ribListMap, 32, 0, 20, sRes.ribListPal, 0);
    if (s->ribCount)
        DrawRibbonIndex(8, 49, s->ribSel + 1, s->ribCount, &sRes.ribListPal[1]);
    /* The cursor and the arrows. */
    if (sRes.mcArrows)
    {
        if (s->ribSel >= s->ribTop && s->ribSel < s->ribTop + 8)
            DrawSprite(sRes.mcArrows, 1, 2, 103, 8 + 16 * (s->ribSel - s->ribTop), sRes.mcArrowPal.c);
        if (s->ribTop + 8 < s->ribCount)
            DrawSprite(sRes.mcArrows + 2 * 32, 2, 1, 160, 132, sRes.mcArrowPal.c);
        if (s->ribTop > 0)
            DrawSprite(sRes.mcArrows + 4 * 32, 2, 1, 160, 4, sRes.mcArrowPal.c);
    }
    DrawNavLeftHeader(sRes.navLeftRibbons, &sRes.navLeftPal[2], 0);
    DrawNavLayer(sRes.navHeaderTiles, sRes.navHeaderCount, sRes.navHeaderMap, 32, 4, 20, sRes.ribListPal, 0);
    FillRect(8, 144, 128, 16, sRes.navPal[0].c[4]);
    FillRect(8, 144, 128, 1, sRes.navPal[0].c[5]);

    DrawTallButton(2, 172, 7, 64, up, s->pressed == HIT_NAV_UP, s->ribTop > 0, HIT_NAV_UP);
    DrawTallButton(62, 172, 7, 64, down, s->pressed == HIT_NAV_DOWN, s->ribTop + 8 < s->ribCount, HIT_NAV_DOWN);
    DrawTallButton(122, 172, 14, 64, gText_Cancel, s->pressed == HIT_NAV_BACK, TRUE, HIT_NAV_BACK);
}

/* A small ribbon (BG1): its icon's 8x16 strip and the strip mirrored. */
static void DrawRibbonIcon(u8 ribbon, int x, int y)
{
    const u8 *strip = sRes.ribIcons + 2 * sRibbonGfx[ribbon][0] * 32;
    const u16 *pal = sRes.ribIconPal[sRibbonGfx[ribbon][1] - 2].c;

    DrawTile(strip, x, y, pal, FALSE, FALSE);
    DrawTile(strip, x + 8, y, pal, TRUE, FALSE);
    DrawTile(strip + 32, x, y + 8, pal, FALSE, FALSE);
    DrawTile(strip + 32, x + 8, y + 8, pal, TRUE, FALSE);
}

static void DrawRibbonSummary(const ViewState *s)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
    const Pal *box = &sRes.ribSumPal[1], *info = &sRes.ribInfoPal;
    u8 text[24];

    DrawNavDots();
    /* BG2, the summary's art, and its windows. */
    DrawNavLayer(sRes.ribSumTiles, sRes.ribSumCount, sRes.ribSumMap, 32, 0, 20, sRes.ribSumPal, 0);
    FillRect(112, 8, 104, 16, info->c[1]);
    DrawStr(&sNormal, s->ribNick, 112, 9, info->c[TEXT_COLOR_DARK_GRAY], info->c[TEXT_COLOR_LIGHT_GRAY]);
    DrawRibbonGender(s->ribGender, s->ribLevel, 112 + 60, 9, info);
    FillRect(8, 40, 56, 16, box->c[1]);
    DrawRibbonIndex(8, 41, s->ribSel + 1, s->ribCount, box);
    FillRect(96, 104, 128, 32, box->c[4]);
    if (s->ribBig == RIB_NONE)
    {
        /* gText_RibbonsF700: its {DYNAMIC 0} the count. */
        const u8 *src = gText_RibbonsF700;
        u8 *dst = text;

        for (; *src != EOS && dst < text + sizeof(text) - 4; ++src)
        {
            if (src[0] == CHAR_DYNAMIC)
            {
                dst = StringCopy(dst, Number(s->ribIdCount + s->ribGiftCount, 2, STR_CONV_MODE_LEFT_ALIGN));
                ++src;
                continue;
            }
            *dst++ = *src;
        }
        *dst = EOS;
        DrawStr(&sNormal, text, 96, 105, box->c[TEXT_COLOR_DARK_GRAY], box->c[TEXT_COLOR_LIGHT_GRAY]);
    }
    else
    {
        u8 ribbon = s->ribBig < GIFT_RIBBON_START_POS ? s->ribIds[s->ribBig] : s->ribGift[s->ribBig - GIFT_RIBBON_START_POS];
        const u8 *const *lines = NULL;

        if (ribbon < FIRST_GIFT_RIBBON)
            lines = gRibbonDescriptionPointers[ribbon];
        else
        {
            u8 v = gSaveBlock1Ptr->giftRibbons[ribbon - FIRST_GIFT_RIBBON];

            if (v != 0 && v <= MAX_GIFT_RIBBON)
                lines = gGiftRibbonDescriptionPointers[v - 1];
        }
        for (int i = 0; lines && i < 2; ++i)
            DrawStr(&sNormal, lines[i], 96, 105 + 16 * i, box->c[TEXT_COLOR_DARK_GRAY], box->c[TEXT_COLOR_LIGHT_GRAY]);
    }
    /* BG1, the small ribbons: nine to a row, the gift ones on row 3. */
    for (int i = 0; i < s->ribIdCount && sRes.ribIcons; ++i)
    {
        int x = 88 + 16 * (i % RIBBONS_PER_ROW), y = 32 + 16 * (i / RIBBONS_PER_ROW);

        DrawRibbonIcon(s->ribIds[i], x, y);
        AddHit(x, y, 16, 16, HIT_ROW + i);
    }
    for (int j = 0; j < s->ribGiftCount && sRes.ribIcons; ++j)
    {
        DrawRibbonIcon(s->ribGift[j], 88 + 16 * j, 80);
        AddHit(88 + 16 * j, 80, 16, 16, HIT_ROW + GIFT_RIBBON_START_POS + j);
    }
    /* OBJ priority 1: the ribbon shown big, 8 px past its small icon. */
    if (s->ribBig != RIB_NONE && sRes.ribBigIcons)
    {
        bool8 gift = s->ribBig >= GIFT_RIBBON_START_POS;
        u8 ribbon = gift ? s->ribGift[s->ribBig - GIFT_RIBBON_START_POS] : s->ribIds[s->ribBig];
        int pos = gift ? s->ribBig - GIFT_RIBBON_START_POS : s->ribBig % RIBBONS_PER_ROW;
        int row = gift ? GIFT_RIBBON_ROW : s->ribBig / RIBBONS_PER_ROW;

        DrawSprite(sRes.ribBigIcons + 16 * sRibbonGfx[ribbon][0] * 32, 4, 4, 80 + 16 * pos, 24 + 16 * row,
                   sRes.ribIconPal[sRibbonGfx[ribbon][1] - 2].c);
    }
    /* OBJ priority 0: the Pokémon's picture. */
    if (sSumPic.valid)
        DrawSprite(sSumPic.tiles, 8, 8, 8, 72, sSumPic.pal.c);
    DrawNavLeftHeader(sRes.navLeftRibbons, &sRes.navLeftPal[2], 0);
    DrawNavLayer(sRes.navHeaderTiles, sRes.navHeaderCount, sRes.navHeaderMap, 32, 4, 20, sRes.ribSumPal, 0);
    FillRect(8, 144, 128, 16, sRes.navPal[0].c[4]);
    FillRect(8, 144, 128, 1, sRes.navPal[0].c[5]);

    DrawTallButton(2, 172, 7, 64, left, s->pressed == HIT_NAV_UP, s->ribCount > 1, HIT_NAV_UP);
    DrawTallButton(62, 172, 7, 64, right, s->pressed == HIT_NAV_DOWN, s->ribCount > 1, HIT_NAV_DOWN);
    DrawTallButton(122, 172, 14, 64, gText_Cancel, s->pressed == HIT_NAV_BACK, TRUE, HIT_NAV_BACK);
}

static void DrawNav(const ViewState *s)
{
    int clipX1 = sClipX1;

    if (!s->navOk)
        return;
    /* The GBA's screen ends at 240: the option bars run off it there. */
    if (sClipX1 > CW)
        sClipX1 = CW;
    if (s->navView == PNAV_MATCH_CALL)
        DrawMatchCall(s);
    else if (s->navView == PNAV_CONDITION_MENU)
        DrawNavOptions(s, sNavConditionOptions, 56, 20, sRes.navLeftCondition, &sRes.navLeftPal[1], TRUE);
    else if (s->navView == PNAV_CONDITION || s->navView == PNAV_SEARCH_CONDITION)
        DrawConditionScreen(s);
    else if (s->navView == PNAV_SEARCH_MENU)
        DrawNavOptions(s, sNavSearchOptions, 40, 16, sRes.navLeftCondition, &sRes.navLeftPal[1], TRUE);
    else if (s->navView == PNAV_SEARCH_RESULTS)
        DrawSearchResults(s);
    else if (s->navView == PNAV_RIBBONS)
        DrawRibbonList(s);
    else if (s->navView == PNAV_RIBBON_SUMMARY)
        DrawRibbonSummary(s);
    else
        DrawNavMenu(s);
    sClipX1 = clipX1;
}

/*
 * A held MAIL as mail.c's ReadMail shows it, on the GBA's 240x160 at the
 * party's height: its stationery (BG1) over the bars in the player's colours
 * (BG2, its tile 1 all over - here the whole bottom screen), the words and
 * the signature in the mail's colours, the Bead and Dream mail's icon. A tap
 * goes back, as A or B does.
 */
static struct
{
    const u32 *tiles;
    u8 *tileData, *map;
    u32 tileCount;
} sMailGfx;

static void DrawPartyMail(const ViewState *s)
{
    const struct CtrMailView *m = &s->mail;
    Pal pals[16];
    u32 size;

    if (sMailGfx.tiles != m->tiles)
    {
        free(sMailGfx.tileData);
        free(sMailGfx.map);
        sMailGfx.tileData = Unlz(m->tiles, &size);
        sMailGfx.tileCount = sMailGfx.tileData ? size / 32 : 0;
        sMailGfx.map = Unlz(m->tileMap, NULL);
        sMailGfx.tiles = m->tiles;
    }
    AddHit(0, 0, CW, H, HIT_PARTY_MESSAGE);
    if (!sMailGfx.tileData || !sMailGfx.map)
        return;
    PalSymbol(m->palette, &pals[0], 1);
    pals[0].c[10] = Rgb565(m->barColors[0]);
    pals[0].c[11] = Rgb565(m->barColors[1]);
    for (int i = 1; i < 16; ++i)
        pals[i] = pals[0];
    for (int ty = 0; ty < H / 8; ++ty)
        for (int tx = 0; tx < CW / 8; ++tx)
            DrawMapEntry(sMailGfx.tileData, sMailGfx.tileCount, 1, tx * 8, ty * 8, pals);
    for (int ty = 0; ty < 20; ++ty)
        for (int tx = 0; tx < 30; ++tx)
        {
            const u8 *e = sMailGfx.map + (ty * 32 + tx) * 2;

            DrawMapEntry(sMailGfx.tileData, sMailGfx.tileCount, e[0] | (e[1] << 8), tx * 8, PARTY_Y0 + ty * 8, pals);
        }
    for (int i = 0; i < m->numLines; ++i)
        DrawStr(&sNormal, m->lines[i], m->lineX[i], PARTY_Y0 + m->lineY[i], Rgb565(m->textColor), Rgb565(m->textShadow));
    if (m->numLines)
        DrawStr(&sNormal, m->signature, m->signatureX, PARTY_Y0 + m->signatureY, Rgb565(m->textColor),
                Rgb565(m->textShadow));
    if (m->iconSpecies != SPECIES_NONE)
    {
        const u8 *tiles = MonIcon(m->iconSpecies, FALSE);
        u8 iconPal = gMonIconPaletteIndices[m->iconSpecies];

        if (tiles && iconPal < 3)
            DrawSprite(tiles, 4, 4, m->iconX - 16, PARTY_Y0 + m->iconY - 16, sRes.monIconPal[iconPal].c);
    }
}

static void DrawParty(const ViewState *s)
{
    if (s->partyView == PARTY_SUMMARY)
        DrawPartySummary(s);
    else if (s->partyView == PARTY_MAIL)
        DrawPartyMail(s);
    else
        DrawPartyList(s);
}

static void DrawSave(const ViewState *s)
{
    int lines = StrLines(s->text), ht = lines * 2 > 4 ? lines * 2 : 4;

    DrawSaveFile(s);
    if (s->saveStep == SAVE_DONE)
        DrawSavePlate(64, 112, Ascii(CTR_TEXT("OK", "VALE", "OK", "OK")), TRUE, s->pressed == HIT_OK, TRUE, HIT_OK);
    else
    {
        DrawSavePlate(10, 106, gText_Yes, TRUE, s->pressed == HIT_YES, s->canSave, HIT_YES);
        DrawSavePlate(124, 106, gText_No, FALSE, s->pressed == HIT_NO, TRUE, HIT_NO);
    }
    /* The question last: a long one (another file's warning) covers the
     * file rather than being cut. */
    DrawWindowFrame(s->frame, SAVE_MSG_X, SAVE_MSG_Y, CW / 8 - 2, ht, TXT_WHITE);
    DrawStr(&sNormal, s->text, SAVE_MSG_X + 4, SAVE_MSG_Y + 1 + (ht * 8 - 2 - lines * 16) / 2, TXT_DARK, TXT_LIGHT);
}

/*
 * One option's cell: its name on the first line and its value on the second,
 * both centred, the value between the arrows; the left half of the cell
 * steps the value back and the right half on. The frame's cell is drawn with
 * the chosen frame itself, so it is its own preview: one line inside it, the
 * name in the left half and the value between its arrows in the right.
 */
/*
 * A cell's picture, kept as a whole drawing of it left it: the OPTIONS page
 * is twelve cells of text, 4 ms in Azahar and some 10 ms on an Old 3DS each
 * time a page or a value changes. The key is all the cell is drawn from -
 * its place, its strings, its state, the font and the backdrop its cut
 * corners show - so a copy is what drawing it would give; a cell whose text
 * would run out of it is not kept. CTR_BOTTOM_CACHE_AUDIT draws it anyway and
 * logs a difference.
 */
#define CELL_PICS 24
static struct
{
    bool8 valid;
    bool8 warmed;   /* made ahead (WarmOptionCells), not by a redraw */
    u32 key, used;
    u16 px[OPT_CELL_W * OPT_CELL_H];
} sCellPics[CELL_PICS];
static u32 sCellPicClock;

static u32 Mix(u32 h, u32 v);

static u32 MixStr(u32 h, const u8 *str)
{
    for (; *str != EOS; ++str)
        h = Mix(h, *str);
    return Mix(h, EOS);
}

static bool8 CellWhole(int x, int y)
{
    int sx = x + sOX;

    return sDst == sCanvas && sx >= sClipX0 && y >= sClipY0 && sx + OPT_CELL_W <= sClipX1 && y + OPT_CELL_H <= sClipY1;
}

/* The kept picture of the cell, as much of it as the clip shows. */
static bool8 CellPicDraw(u32 key, int x, int y)
{
    int sx = x + sOX;

    if (sDst != sCanvas)
        return FALSE;
    for (int i = 0; i < CELL_PICS; ++i)
        if (sCellPics[i].valid && sCellPics[i].key == key)
        {
            int x0 = sx > sClipX0 ? sx : sClipX0, x1 = sx + OPT_CELL_W < sClipX1 ? sx + OPT_CELL_W : sClipX1;
            int y0 = y > sClipY0 ? y : sClipY0, y1 = y + OPT_CELL_H < sClipY1 ? y + OPT_CELL_H : sClipY1;

            for (int cx = x0; cx < x1; ++cx)
                memcpy(sCanvas + cx * H + (H - y1), sCellPics[i].px + (cx - sx) * OPT_CELL_H + (OPT_CELL_H - (y1 - y)),
                       (size_t)(y1 - y0) * sizeof(u16));
            sCellPics[i].used = ++sCellPicClock;
            return TRUE;
        }
    return FALSE;
}

/* The cell's picture as the canvas has it now, into slot `slot`. */
static void CellPicStore(int slot, u32 key, int x, int y, bool8 warmed)
{
    int sx = x + sOX;

    for (int cx = 0; cx < OPT_CELL_W; ++cx)
        memcpy(sCellPics[slot].px + cx * OPT_CELL_H, sCanvas + (sx + cx) * H + (H - y - OPT_CELL_H),
               OPT_CELL_H * sizeof(u16));
    sCellPics[slot].key = key;
    sCellPics[slot].used = ++sCellPicClock;
    sCellPics[slot].valid = TRUE;
    sCellPics[slot].warmed = warmed;
}

static void CellPicKeep(u32 key, int x, int y)
{
    int oldest = 0;

    /* The one least recently drawn makes room. */
    for (int i = 1; i < CELL_PICS && sCellPics[oldest].valid; ++i)
        if (!sCellPics[i].valid || sCellPics[i].used < sCellPics[oldest].used)
            oldest = i;
    CellPicStore(oldest, key, x, y, FALSE);
}

static bool8 FitsCell(const u8 *str)
{
    int ix0, ix1;

    InkColumns(&sSmall, str, &ix0, &ix1);
    return ix1 - ix0 <= OPT_CELL_W - 2;
}

/* All a cell is drawn from (see CELL_PICS): its key in the picture cache. */
static u32 CellKey(int x, int y, const u8 *name, const u8 *value, bool8 live, bool8 on, bool8 arrows)
{
    return Mix(Mix(Mix(Mix(Mix(Mix(Mix(Mix(MixStr(MixStr(0x811C9DC5u, name), value), x), y), sOX), live), on),
                       arrows), (u32)(uintptr_t)sSmall.glyphs), (u32)(uintptr_t)sCache[CACHE_MENU]);
}

/* Whether a cell's picture may be kept at all, wherever it is drawn. */
static bool8 CellKeepable(const u8 *name, const u8 *value)
{
    return sSmall.glyphs && FitsCell(name) && FitsCell(value);
}

static void DrawCellPlateBody(int x, int y, const u8 *name, const u8 *value, bool8 live, bool8 on, bool8 arrows);

/*
 * A cell's plate: the name on the first line and the value on the second,
 * both centred, the value between the arrows when it has them.
 */
static void DrawCellPlate(int x, int y, const u8 *name, const u8 *value, bool8 live, bool8 on, bool8 arrows)
{
    u32 key = CellKey(x, y, name, value, live, on, arrows);
    bool8 keep = CellWhole(x, y) && CellKeepable(name, value);

#ifdef CTR_BOTTOM_CACHE_AUDIT
    for (int i = 0; keep && i < CELL_PICS; ++i)
        if (sCellPics[i].valid && sCellPics[i].key == key)
        {
            static unsigned same;
            int sx = x + sOX;
            bool8 differs = FALSE;

            DrawCellPlateBody(x, y, name, value, live, on, arrows);
            for (int cx = 0; cx < OPT_CELL_W && !differs; ++cx)
                differs = memcmp(sCellPics[i].px + cx * OPT_CELL_H, sCanvas + (sx + cx) * H + (H - y - OPT_CELL_H),
                                 OPT_CELL_H * sizeof(u16)) != 0;
            /* A picture made ahead is checked here too, at its first use:
             * against the cell drawn on the screen itself. */
            if (differs)
                CtrLog_Write(CTR_LOG_ERROR, "CONTRACT bottom option cell picture (%s) differs from a redraw",
                             sCellPics[i].warmed ? "made ahead" : "kept");
            else if ((++same & 63) == 1 || sCellPics[i].warmed)
                CtrLog_Write(CTR_LOG_VIDEO, "bottom option cells: same as a redraw (%u%s)", same,
                             sCellPics[i].warmed ? ", made ahead" : "");
            sCellPics[i].warmed = FALSE;
            return;
        }
#else
    if (keep && CellPicDraw(key, x, y))
        return;
#endif
    DrawCellPlateBody(x, y, name, value, live, on, arrows);
    if (keep)
        CellPicKeep(key, x, y);
}

static void DrawCellPlateBody(int x, int y, const u8 *name, const u8 *value, bool8 live, bool8 on, bool8 arrows)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
    u16 nameFg = !live ? sLook.offText : on ? TXT_WHITE : TXT_DARK;
    u16 nameSh = !live ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LIGHT;
    u16 valueFg = !live ? sLook.offText : on ? TXT_WHITE : TXT_RED;
    u16 valueSh = !live ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LRED;
    int capTop, capBottom, cap, line, dy = on, aw = StrWidth(&sSmall, left);

    DrawPlate(x, y, OPT_CELL_W, OPT_CELL_H, !live ? &sLook.off : on ? &sLook.chosen : &sLook.plate, 0, on);
    /* Two lines of capitals 3px apart, centred between the outline and
     * the shade rows. */
    CapRows(&sSmall, &capTop, &capBottom);
    cap = capBottom - capTop;
    line = y + 1 + (28 - (2 * cap + 3)) / 2 + dy;
    DrawStrIn(&sSmall, name, x + 1, x + OPT_CELL_W - 1, line, line + cap, nameFg, nameSh);
    line += cap + 3;
    DrawStrIn(&sSmall, value, x + 1, x + OPT_CELL_W - 1, line, line + cap, valueFg, valueSh);
    if (!arrows)
        return;
    DrawStrIn(&sSmall, left, x + 8, x + 8 + aw, line, line + cap, nameFg, nameSh);
    DrawStrIn(&sSmall, right, x + OPT_CELL_W - 8 - aw, x + OPT_CELL_W - 8, line, line + cap, nameFg, nameSh);
}

static void DrawOptionCell(const ViewState *s, int row, const u8 *name, bool8 voxel)
{
    static const u8 left[] = {CHAR_LEFT_ARROW, EOS}, right[] = {CHAR_RIGHT_ARROW, EOS};
    const u8 *value = OptionValue(row, s->options[row]);
    bool8 live = OptionLive(row, voxel);
    bool8 on = live && (s->pressed == HIT_OPTION + row || s->pressed == HIT_OPTION + HIT_OPTION_BACK + row);
    u16 nameFg = !live ? sLook.offText : on ? TXT_WHITE : TXT_DARK;
    u16 nameSh = !live ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LIGHT;
    u16 valueFg = !live ? sLook.offText : on ? TXT_WHITE : TXT_RED;
    u16 valueSh = !live ? sLook.offShadow : on ? sLook.chosenShadow : TXT_LRED;
    int x, y, dy = on, aw = StrWidth(&sSmall, left);

    OptionCell(row, &x, &y);
    if (row == OPT_FRAME)
    {
        int top = y + 8 + dy, bottom = y + 24 + dy, vx0, vx1, group, gx;

        DrawWindowFrame(s->options[OPT_FRAME], x + 8, y + 8, 12, 2, on ? sLook.chosen.f : TXT_WHITE);
        DrawStrIn(&sSmall, name, x + 8, x + 56, top, bottom, nameFg, nameSh);
        InkColumns(&sSmall, value, &vx0, &vx1);
        group = aw + 2 + (vx1 - vx0) + 2 + aw;
        gx = x + 56 + (48 - group) / 2;
        DrawStrIn(&sSmall, left, gx, gx + aw, top, bottom, nameFg, nameSh);
        DrawStrIn(&sSmall, value, gx + aw + 2, gx + aw + 2 + (vx1 - vx0), top, bottom, valueFg, valueSh);
        DrawStrIn(&sSmall, right, gx + group - aw, gx + group, top, bottom, nameFg, nameSh);
    }
    else
        DrawCellPlate(x, y, name, value, live, on, TRUE);
    if (live)
    {
        AddHit(x, y, OPT_CELL_W / 2, OPT_CELL_H, HIT_OPTION + HIT_OPTION_BACK + row);
        AddHit(x + OPT_CELL_W / 2, y, OPT_CELL_W / 2, OPT_CELL_H, HIT_OPTION + row);
    }
    if (s->optFocus == row)
        DrawRing(x, y, OPT_CELL_W, OPT_CELL_H, s->blink);
}

/* An extra's cell: its name, and its value between the arrows, or for an
 * action its one text without them. */
static void DrawExtraCell(const ViewState *s, int row, const CtrExtra *extra, bool8 voxel)
{
    bool8 live = ExtraLive(extra, voxel);
    bool8 on = live && (s->pressed == HIT_OPTION + row || s->pressed == HIT_OPTION + HIT_OPTION_BACK + row);
    const char *value = extra->values ? extra->values[extra->count ? s->extras[row] : 0] : "";
    bool8 arrows = extra->count != 0 || extra->step != NULL;
    int x, y;

    OptionCell(row, &x, &y);
    DrawCellPlate(x, y, Ascii(extra->name), extra->text ? extra->text() : Ascii(value), live, on, arrows);
    if (arrows && live)
        AddHit(x, y, OPT_CELL_W / 2, OPT_CELL_H, HIT_OPTION + HIT_OPTION_BACK + row);
    if (live)
        AddHit(x + (arrows ? OPT_CELL_W / 2 : 0), y, arrows ? OPT_CELL_W / 2 : OPT_CELL_W, OPT_CELL_H,
               HIT_OPTION + row);
    if (s->optFocus == row)
        DrawRing(x, y, OPT_CELL_W, OPT_CELL_H, s->blink);
}

/* The tabs: SETTINGS (the options) and each page that has extras. */
static void DrawOptionTabs(const ViewState *s)
{
    static const char *const names[CTR_EXTRAS_PAGES] = {CTR_TEXT("SETTINGS", "AJUSTES", "RÉGLAGES", "OPZIONI"), CTR_TEXT("ENHANCEMENTS", "MEJORAS", "AMÉLIOR.", "MIGLIORIE"), CTR_TEXT("CHEATS", "TRUCOS", "ASTUCES", "TRUCCHI")};
    u8 pages[CTR_EXTRAS_PAGES], count = 0;
    int capTop, capBottom, cap, widths[CTR_EXTRAS_PAGES], need = 0, x = 4;
    bool8 fit = TRUE;

    for (unsigned page = 0; page < CTR_EXTRAS_PAGES; ++page)
        if (page == CTR_EXTRAS_OPTIONS || CtrExtras_PageUsed(page))
            pages[count++] = page;
    CapRows(&sSmall, &capTop, &capBottom);
    cap = capBottom - capTop;
    /*
     * Equal tabs, as long as every label fits its own; otherwise each takes
     * what its widest label needs ("ENHANCEMENTS 2/2"), and what is left is
     * shared out evenly. Measured on the longest label a tab can show, so
     * the tabs stay put as its screens turn.
     */
    for (unsigned i = 0; i < count; ++i)
    {
        char label[24];

        if (PageSubs(pages[i]) > 1)
            snprintf(label, sizeof(label), "%s %u/%u", names[pages[i]], PageSubs(pages[i]), PageSubs(pages[i]));
        else
            snprintf(label, sizeof(label), "%s", names[pages[i]]);
        widths[i] = StrWidth(&sSmall, Ascii(label)) + 6;
        need += widths[i];
        if (widths[i] > 232 / count - 4)
            fit = FALSE;
    }
    for (unsigned i = 0; !fit && i < count; ++i)
    {
        int share = 232 - 4 * (int)count - need;

        widths[i] += share / (int)count + (i + 1 == count ? share % (int)count : 0);
    }
    for (unsigned i = 0; i < count; x += widths[i] + 4, ++i)
    {
        int x0 = fit ? 4 + (int)(i * 232 / count) : x;
        int x1 = fit ? 4 + (int)((i + 1) * 232 / count) - 4 : x + widths[i];
        bool8 chosen = s->optPage == pages[i], pressed = s->pressed == HIT_PAGE + pages[i];
        int line = TAB_Y + 1 + (TAB_H - 4 - cap) / 2 + pressed;

        DrawPlate(x0, TAB_Y, x1 - x0, TAB_H, chosen ? &sLook.chosen : &sLook.plate, 0, pressed);
        char label[24];

        /* "CHEATS 1/2" on a page shown twelve at a time. */
        if (chosen && PageSubs(pages[i]) > 1)
            snprintf(label, sizeof(label), "%s %u/%u", names[pages[i]], s->optSub + 1, PageSubs(pages[i]));
        else
            snprintf(label, sizeof(label), "%s", names[pages[i]]);
        DrawStrIn(&sSmall, Ascii(label), x0 + 1, x1 - 1, line, line + cap,
                  chosen ? TXT_WHITE : TXT_DARK, chosen ? sLook.chosenShadow : TXT_LIGHT);
        AddHit(x0, TAB_Y, x1 - x0, TAB_H, HIT_PAGE + pages[i]);
    }
}

/* The options' names; six of them take Ascii's buffers until they are drawn. */
static void OptionNames(const u8 *names[OPTION_ROWS])
{
    const u8 *const table[OPTION_ROWS] = {
        gText_TextSpeed, gText_BattleScene, gText_BattleStyle, gText_Sound,
        gText_ButtonMode, gText_Frame, Ascii(CTR_TEXT("SHOW FPS", "MOSTRAR FPS", "AFFICH. FPS", "MOSTRA FPS")), Ascii("VOXEL 3D"),
        Ascii(CTR_TEXT("3D ANGLE", "ÁNGULO 3D", "ANGLE 3D", "ANGOLO 3D")), Ascii(CTR_TEXT("3D ZOOM", "ZOOM 3D", "ZOOM 3D", "ZOOM 3D")),
        Ascii(CTR_TEXT("3D BLUR", "DESENFOQUE 3D", "FLOU 3D", "SFOC. 3D")), Ascii(CTR_TEXT("3D BATTLE", "COMBATE 3D", "COMBAT 3D", "BATTAGLIA 3D"))};

    memcpy(names, table, sizeof(table));
}

static void DrawOptions(const ViewState *s)
{
    /* The tabs first: their labels take turns in Ascii's few buffers, which
     * the names below then hold until the cells are drawn. */
    if (OptionPages())
        DrawOptionTabs(s);

    const u8 *names[OPTION_ROWS];
    bool8 voxel = OPTION_SHOWN > OPT_VOXEL && s->options[OPT_VOXEL];

    OptionNames(names);

    if (s->optPage != CTR_EXTRAS_OPTIONS || s->optSub != 0)
    {
        for (unsigned row = 0; row < ARRAY_COUNT(s->extras); ++row)
        {
            const CtrExtra *extra = PageExtra(s->optPage, row);

            if (extra)
                DrawExtraCell(s, row, extra, voxel);
        }
        return;
    }
    for (int row = 0; row < OPTION_ROWS; ++row)
        if (OptionExists(row))
            DrawOptionCell(s, row, names[row], voxel);
}

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
        int tx = DrawStr(&sSmall, Ascii(CTR_TEXT("POW ", "POT. ", "PUISS. ", "POT. ")), x + 12, oy + 56, TXT_DARK, TXT_LIGHT);

        DrawStr(&sSmall, data->power > 1 ? Number(data->power, 3, STR_CONV_MODE_LEFT_ALIGN) : Ascii("---"), tx,
                oy + 56, TXT_DARK, TXT_LIGHT);
        StringCopy(text, Ascii(CTR_TEXT("ACC ", "PREC. ", "PRÉC. ", "PREC. ")));
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

/*
 * The trainer card's cache, made ahead in strips (CardCacheSteps) on frames
 * the field leaves the bottom screen idle, with the slice IdleSlice gives.
 * Its stars walk the Pokédex, so they are counted again only every
 * CARD_STARS_FRAMES; a card opened with others than these is made whole
 * then, as it always was. TRUE while the card has steps left: the frame is
 * the card's, the option cells wait (WarmOptionCells).
 */
#define CARD_STARS_FRAMES 256u

static bool8 WarmCardCache(u8 mode)
{
    static u8 stars = 0xFF;
    static u32 starsFrame;
    static unsigned starved;
    float slice;

    if (mode != MODE_FIELD || sShown.mode != MODE_FIELD || sShown.inBattle || sShown.screen == SCR_CARD
     || !sCache[CACHE_CARD] || sBuildBudget != 0xFF)
        return FALSE;
    if (stars == 0xFF || sFrames - starsFrame >= CARD_STARS_FRAMES)
    {
        stars = CardStars();
        if (stars > 4)
            stars = 4;
        starsFrame = sFrames;
    }
    if (sCardCacheKey == stars * 2 + sShown.gender)
        return FALSE;
    if (IdleSlice(&starved, &slice))
        CardCacheSteps(stars, sShown.gender, CtrPlatform_Ticks(), slice);
    return TRUE;
}

/*
 * The OPTIONS cells' pictures (CELL_PICS), made ahead on idle field frames,
 * so the first visit to a page copies them instead of drawing a page of text
 * cells (~5 ms in Azahar, two or three times that on an Old 3DS). Only the
 * pictures the settings as they are now would show: each cell's arguments
 * are worked out as DrawOptionCell and DrawExtraCell work them out (from the
 * values the snapshot reads, ReadOptionValues and ExtraShownValue; nothing
 * pressed; beside the column, sOX 0) and its key is CellKey's. It is drawn as
 * a redraw draws it, DrawCellPlateBody into the canvas over CACHE_MENU's
 * pixels (what CopyCache puts under a cell of the OPTIONS screen, the
 * backdrop the key names), clipped to the cell; the canvas' own pixels there
 * are put back after, so the screen and later partial redraws never see it.
 * The audit build checks each picture made ahead at its first use, against
 * the cell drawn on the screen (DrawCellPlate).
 *
 * Pages in order, the options' first, as many cells as CELL_PICS holds. A
 * slot holding one of those pictures is never taken for another; with the
 * screen open nothing is made (its pictures are the ones in use). Once all
 * are there the settings are looked at again only every CELL_WARM_FRAMES.
 */
#define CELL_WARM_FRAMES 64u

typedef struct
{
    int x, y;
    const u8 *name, *value;
    bool8 live, arrows;
} CellArgs;

/* The cell `row` of `page` as the screen would draw it now; FALSE for none,
 * or one whose picture is never kept (the frame's: its own preview). The
 * strings live in Ascii's and Number's rings: use them at once. */
static bool8 OptionCellArgs(unsigned page, unsigned row, const u8 options[OPTION_ROWS], CellArgs *c)
{
    if (page == CTR_EXTRAS_OPTIONS)
    {
        const u8 *names[OPTION_ROWS];
        bool8 voxel = OPTION_SHOWN > OPT_VOXEL && options[OPT_VOXEL];

        if (row >= OPTION_ROWS || !OptionExists(row) || row == OPT_FRAME)
            return FALSE;
        OptionNames(names);
        c->name = names[row];
        c->value = OptionValue(row, options[row]);
        c->live = OptionLive(row, voxel);
        c->arrows = TRUE;
    }
    else
    {
        const CtrExtra *extra = PageExtra(page, row);
        bool8 voxel = OPTION_SHOWN > OPT_VOXEL && options[OPT_VOXEL];

        if (!extra)
            return FALSE;
        c->name = Ascii(extra->name);
        if (extra->text)
            c->value = extra->text();
        else
        {
            /* The snapshot's extras[row], a u16 as there. */
            u16 shown = ExtraShownValue(extra);

            c->value = Ascii(extra->values ? extra->values[extra->count ? shown : 0] : "");
        }
        c->live = ExtraLive(extra, voxel);
        c->arrows = extra->count != 0 || extra->step != NULL;
    }
    OptionCell(row, &c->x, &c->y);
    return TRUE;
}

/* One cell's picture, made into slot `slot`. */
static void WarmOptionCell(int slot, u32 key, const CellArgs *c)
{
    static u16 under[OPT_CELL_W * OPT_CELL_H];
    u16 *dst = sDst;
    int clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1};

    for (int cx = 0; cx < OPT_CELL_W; ++cx)
    {
        size_t at = (size_t)(c->x + cx) * H + (H - c->y - OPT_CELL_H);

        memcpy(under + cx * OPT_CELL_H, sCanvas + at, OPT_CELL_H * sizeof(u16));
        memcpy(sCanvas + at, sCache[CACHE_MENU] + at, OPT_CELL_H * sizeof(u16));
    }
    sDst = sCanvas;
    sClipX0 = c->x;
    sClipY0 = c->y;
    sClipX1 = c->x + OPT_CELL_W;
    sClipY1 = c->y + OPT_CELL_H;
    DrawCellPlateBody(c->x, c->y, c->name, c->value, c->live, FALSE, c->arrows);
    CellPicStore(slot, key, c->x, c->y, TRUE);
    for (int cx = 0; cx < OPT_CELL_W; ++cx)
        memcpy(sCanvas + (size_t)(c->x + cx) * H + (H - c->y - OPT_CELL_H), under + cx * OPT_CELL_H,
               OPT_CELL_H * sizeof(u16));
    sDst = dst;
    sClipX0 = clip[0];
    sClipY0 = clip[1];
    sClipX1 = clip[2];
    sClipY1 = clip[3];
}

static void WarmOptionCells(u8 mode)
{
    static bool8 settled;
    static u32 settledFrame;
    static unsigned starved;
    u32 keys[CELL_PICS];
    u8 pages[CELL_PICS], rows[CELL_PICS], options[OPTION_ROWS];
    int n = 0, ox = sOX;
    uint64_t start;
    float slice;
    CellArgs c;

    if (mode != MODE_FIELD || sShown.mode != MODE_FIELD || sShown.inBattle || sShown.screen == SCR_OPTION
     || !sCache[CACHE_MENU] || sBuildBudget != 0xFF || !gSaveBlock2Ptr)
        return;
    if (settled && sFrames - settledFrame < CELL_WARM_FRAMES)
        return;
    if (!IdleSlice(&starved, &slice))
        return;
    start = CtrPlatform_Ticks();
    ResolveFonts();
    sOX = 0;
    ReadOptionValues(options);
    /* The pictures wanted, the options' page first. */
    for (unsigned page = 0; page < CTR_EXTRAS_PAGES && n < CELL_PICS; ++page)
    {
        if (page != CTR_EXTRAS_OPTIONS && (!OptionPages() || !CtrExtras_PageUsed(page)))
            continue;
        for (unsigned row = 0; row < CELLS_PER_PAGE && n < CELL_PICS; ++row)
        {
            u32 key;
            bool8 twice = FALSE;

            if (!OptionCellArgs(page, row, options, &c) || !CellKeepable(c.name, c.value))
                continue;
            key = CellKey(c.x, c.y, c.name, c.value, c.live, FALSE, c.arrows);
            for (int k = 0; k < n && !twice; ++k)
                twice = keys[k] == key;
            if (twice)
                continue;
            keys[n] = key;
            pages[n] = page;
            rows[n++] = row;
        }
    }
    settled = TRUE;
    for (int i = 0; i < n; ++i)
    {
        int slot = -1;

        for (int k = 0; k < CELL_PICS && slot < 0; ++k)
            if (sCellPics[k].valid && sCellPics[k].key == keys[i])
                slot = k;
        if (slot >= 0)
            continue;
        /* Settled only once every picture is made, by this frame or an
         * earlier one: a cell whose text keeps changing then costs one
         * picture every CELL_WARM_FRAMES, not one every frame. */
        if (CtrPlatform_TickMs(CtrPlatform_Ticks() - start) >= slice)
        {
            settled = FALSE;
            break;
        }
        /* An empty slot, or the least recently drawn of those holding none
         * of the pictures wanted. There is one: this picture is missing and
         * n <= CELL_PICS, so fewer than CELL_PICS slots hold wanted ones. */
        for (int k = 0; k < CELL_PICS; ++k)
        {
            bool8 wanted = FALSE;

            if (!sCellPics[k].valid)
            {
                slot = k;
                break;
            }
            for (int j = 0; j < n && !wanted; ++j)
                wanted = sCellPics[k].key == keys[j];
            if (!wanted && (slot < 0 || sCellPics[k].used < sCellPics[slot].used))
                slot = k;
        }
        /* The strings again: the keys' walk has reused the rings since. */
        if (slot < 0 || !OptionCellArgs(pages[i], rows[i], options, &c))
            continue;
        WarmOptionCell(slot, keys[i], &c);
    }
    if (settled)
        settledFrame = sFrames;
    sOX = ox;
}

/* ------------------------------------------------------------------------ */
/* Redraw and present                                                       */
/* ------------------------------------------------------------------------ */

static void BuildBackgroundCaches(void)
{
    sOX = 0;
    /* The 240px view with the column beside it: the sections' light green
     * and the rail. The whole screen (battle) keeps the party menu's. */
    sDst = sCache[CACHE_MENU];
    DrawSection(0, 0, CW, H);
    DrawColumnBackground();
    sDst = sCache[CACHE_WIDE];
    DrawPartyBackground(W / 8, H / 8);
    BuildMapCache();
    sDst = sCanvas;
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
 * The column's own screens (map, card, save, options) drawn whole, kept with
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
    return s->mode == MODE_FIELD
        && (s->screen == SCR_MAP || s->screen == SCR_CARD || s->screen == SCR_SAVE || s->screen == SCR_OPTION)
        && !s->inBattle && s->bagView != BAG_VIEW_WHOLE
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

    sCardBuildMs = 0.0f;
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
    else
    {
        /* In battle the bag and the party menu have the whole screen. */
        bool8 column = !s->inBattle && s->bagView != BAG_VIEW_WHOLE;

        if (s->screen == SCR_MAP && column)
            CopyCache(CACHE_MAP);
        else if (s->screen == SCR_CARD && column)
        {
            BuildCardCache(s->stars > 4 ? 4 : s->stars, s->gender);
            CopyCache(sCache[CACHE_CARD] ? CACHE_CARD : CACHE_MENU);
        }
        else
            CopyCache(column ? CACHE_MENU : CACHE_WIDE);
        phase[1] = CtrPlatform_Ticks();
        sOX = column ? 0 : (W - CW) / 2;
        switch (s->screen)
        {
        case SCR_MAP: DrawRegionMap(s); break;
        case SCR_POKEMON:
            /* In the field the bottom screen's own; the game's party menu
             * (opened by the game) is the compositor's, as the bag is. */
            if (s->mode == MODE_FIELD)
                DrawParty(s);
            else
                FillRect(0, 0, CW, H, 0);
            break;
        case SCR_BAG:
            if (s->mode == MODE_FIELD)
            {
                DrawBag(s);
                break;
            }
            FillRect(0, 0, CW, H, 0);
            break;
        case SCR_POKEDEX:
            /* The game's party menu, bag and Pokédex are drawn there by the compositor;
             * black until they are, as they fade in from black, and while
             * the field is on its way to opening them (OpenAsked). */
            FillRect(0, 0, CW, H, 0);
            break;
        case SCR_CARD: DrawTrainerCard(s); break;
        case SCR_POKENAV:
            if (s->mode == MODE_FIELD)
                DrawNav(s);
            break;
        case SCR_SAVE: DrawSave(s); break;
        case SCR_OPTION: DrawOptions(s); break;
        }
        sOX = 0;
        phase[2] = CtrPlatform_Ticks();
        if (column)
            DrawColumn(s);
        else if (s->bagView == BAG_VIEW_WHOLE)
            memset(sCanvas, 0, sizeof(sCanvas));
    }
    phase[3] = CtrPlatform_Ticks();
    phase[4] = CtrPlatform_Ticks();
    /* Left of the column is the PokéNav's while the compositor draws it, and
     * the whole screen the boxes'. */
    if (!ClipIsFull())
    {
        CtrBottom_BlitRect(sCanvas, sClipX0, sClipY0, sClipX1, sClipY1);
        blitted = TRUE;
    }
    else if (!CtrVideo_BottomWhole() && !sIntro.defer)
    {
        CtrBottom_Blit(sCanvas, CtrVideo_BottomInUse() ? CW : 0, W);
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
                         "backdrop=%.2f (card=%.2f) screen=%.2f column=%.2f icons=%.2f blit=%.2f "
                         "(copy=%.2f flush=%.2f) ms",
                         (unsigned long)sFrames, s->screen, s->optPage, sClipX0, sClipY0, sClipX1, sClipY1, total,
                         CtrPlatform_TickMs(phase[1] - phase[0]), sCardBuildMs,
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

/* A button's rectangle, as the last redraw laid it out, with a little margin
 * for its shadow; every hit of the id counts. FALSE when there is none. */
static bool8 RectAddHitOf(Rect *r, u8 id)
{
    bool8 found = FALSE;

    for (int i = 0; i < sHitCount; ++i)
        if (sHits[i].id == id)
        {
            RectAdd(r, sHits[i].x - 2, sHits[i].y - 2, sHits[i].x + sHits[i].w + 2, sHits[i].y + sHits[i].h + 2);
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

        /* An action cell has no back half: its one hit is enough, or the
         * whole options area is redrawn for its ring's blink. */
        bool8 forward = RectAddHitOf(r, HIT_OPTION + row);
        bool8 backward = RectAddHitOf(r, HIT_OPTION + HIT_OPTION_BACK + row);

        return forward || backward;
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

/*
 * The OPTIONS screen changed only in what the area left of the column is
 * drawn from: the page and sub-page, the values, the option focus, a press
 * that is not on the column, the ring's blink with no column item focused.
 * The column reads none of these (DrawColumn, DrawSquares) and draws nothing
 * left of CW, nor the options right of it, so a redraw of [0, CW) leaves the
 * canvas what a whole one would paint. A page switch was a whole redraw and
 * a whole-screen copy and present: ~10 ms on an Old 3DS.
 */
static bool8 OptionsAreaOnly(const ViewState *now, const ViewState *shown)
{
    static ViewState probe;
    bool8 columnPress = (now->pressed >= HIT_COLUMN && now->pressed < HIT_COLUMN + COL_ITEMS)
                     || (shown->pressed >= HIT_COLUMN && shown->pressed < HIT_COLUMN + COL_ITEMS);

    /* Beside the column, not centred on the whole screen (Render's sOX). */
    if (now->mode != MODE_FIELD || now->screen != SCR_OPTION || shown->screen != SCR_OPTION
     || now->inBattle || now->bagView == BAG_VIEW_WHOLE)
        return FALSE;
    probe = *now;
    probe.optPage = shown->optPage;
    probe.optSub = shown->optSub;
    probe.optFocus = shown->optFocus;
    memcpy(probe.options, shown->options, sizeof(probe.options));
    memcpy(probe.extras, shown->extras, sizeof(probe.extras));
    if (!columnPress)
        probe.pressed = shown->pressed;
    if (now->focus == FOCUS_NONE)
        probe.blink = shown->blink;
    return memcmp(&probe, shown, sizeof(probe)) == 0;
}

/* The clip a redraw can be limited to, when the new view differs from the
 * shown one only in things that touch one part of the screen: the button lit
 * by a press or the battle cursor, an HP bar and its status, an option's
 * value. FALSE when anything else differs, or the part cannot be told. */
static bool8 DirtyRect(const ViewState *now, const ViewState *shown, Rect *r)
{
    static ViewState probe;
    bool8 battle = now->mode >= MODE_BATTLE_INFO;

    if (now->mode == MODE_FIELD && shown->mode == MODE_FIELD
     && now->screen == SCR_MAP && shown->screen == SCR_MAP)
    {
        probe = *now;
        probe.clockShown = shown->clockShown;
        probe.clockValid = shown->clockValid;
        probe.clockRate = shown->clockRate;
        probe.clockPhase = shown->clockPhase;
        probe.clockMinute = shown->clockMinute;
        if (memcmp(&probe, shown, sizeof(probe)) != 0)
            return FALSE;
        r->x0 = CLOCK_X;
        r->y0 = CLOCK_Y;
        r->x1 = CLOCK_X + CLOCK_W;
        r->y1 = CLOCK_Y + CLOCK_H;
        return TRUE;
    }

    if (now->mode != shown->mode || (now->mode != MODE_FIELD && !battle) || shown->screen != now->screen
     || (now->mode == MODE_FIELD && now->screen != SCR_OPTION))
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
    }
    if (now->mode == MODE_FIELD)
        for (int i = 0; i < OPTION_ROWS; ++i)
            if (i != OPT_VOXEL)
                probe.options[i] = shown->options[i];
    if (memcmp(&probe, shown, sizeof(probe)) != 0)
    {
        /* Another page or sub-page of the options (or its values, focus,
         * press) with the column as it was: the area left of the column
         * alone, whole. Not the whole screen copied and sent again. */
        if (battle || !OptionsAreaOnly(now, shown))
            return FALSE;
        RectInit(r);
        RectAdd(r, 0, 0, CW, H);
        return TRUE;
    }

    RectInit(r);
    if (battle)
    {
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
    if (now->pressed != shown->pressed && !(RectAddHit(r, now->pressed) && RectAddHit(r, shown->pressed)))
        return FALSE;
    for (int i = 0; now->mode == MODE_FIELD && i < OPTION_ROWS; ++i)
        if (now->options[i] != shown->options[i] && !RectAddHit(r, HIT_OPTION + i))
            return FALSE;
    return r->x0 < r->x1 && r->y0 < r->y1;
}

static void RenderPart(const ViewState *s, int x0, int y0, int x1, int y1);

/* The focus ring moved or blinked, or a button was pressed or let go, and
 * nothing else changed: the rectangles of the buttons it touches. Beside the
 * game's own screens (the PokéNav, the bag) only the column's part is ours. */
static bool8 FocusDirtyRect(const ViewState *now, const ViewState *shown, Rect *r)
{
    static ViewState probe;
    u8 focus[2] = {now->focus, shown->focus}, opt[2] = {now->optFocus, shown->optFocus};

    probe = *now;
    probe.focus = shown->focus;
    probe.optFocus = shown->optFocus;
    probe.blink = shown->blink;
    probe.pressed = shown->pressed;
    if (now->mode == MODE_OFF || now->mode >= MODE_BATTLE_INFO || memcmp(&probe, shown, sizeof(probe)) != 0)
        return FALSE;
    RectInit(r);
    for (int k = 0; k < 2; ++k)
    {
        if (focus[k] != FOCUS_NONE && !RectAddHit(r, HIT_COLUMN + focus[k]))
            return FALSE;
        if (opt[k] != FOCUS_NONE && !RectAddHit(r, HIT_OPTION + opt[k]))
            return FALSE;
    }
    if (now->pressed != shown->pressed && !(RectAddHit(r, now->pressed) && RectAddHit(r, shown->pressed)))
        return FALSE;
    return r->x0 < r->x1 && r->y0 < r->y1;
}

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

/* One press of keys, as the player's own: the column's Y is SELECT. */
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
    /* The player's own buttons always win; not those the column holds back
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
 * The PokéNav runs as it is, drawn left of the column by the compositor; a
 * tap on one of its screens becomes the buttons that screen reads, pressed
 * only while the PokéNav waits for input (CtrPokenav_Screen): an option or a
 * list entry is reached with one press of the D-pad, the cursor put next to
 * it first (NavNext), and chosen with A; a place on the map
 * is walked to, a ribbon is picked. The POKéNAV button of the column is B,
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
 * closing, or another plan for it - drops the screen chosen from the column:
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
    /* As in RunPlan: not the keys the column holds back, such as the A that
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
static void LeaveForScreen(u8 screen)
{
    if (screen == SCR_SAVE)
        OpenSave();
    sScreen = screen;
}

static void ShowOptionSub(u8 page, u8 sub)
{
    if (page == sOptPage && sub == sOptSub)
        return;
    sOptPage = page;
    sOptSub = sub;
    sOptFocus = FixedOptions() ? OPT_TEXT_SPEED : 0;
    PlaySE(SE_SELECT);
}

void CtrExtras_ShowScreen(unsigned page, unsigned screen)
{
    if (page < CTR_EXTRAS_PAGES && screen < PageSubs(page))
        ShowOptionSub(page, screen);
}

/* A tab: its page, or on the page on show its next screen. */
static void ShowOptionPage(u8 page)
{
    if (page != CTR_EXTRAS_OPTIONS && !CtrExtras_PageUsed(page))
        return;
    if (page == sOptPage)
        ShowOptionSub(page, (sOptSub + 1) % PageSubs(page));
    else
        ShowOptionSub(page, 0);
}

static void ActivateOption(u8 id)
{
    bool8 back = id >= HIT_OPTION + HIT_OPTION_BACK;
    u8 row = (id - HIT_OPTION) % HIT_OPTION_BACK;

    if (!FixedOptions())
    {
        const CtrExtra *extra = PageExtra(sOptPage, row);

        if (extra && ExtraLive(extra, CtrSettings_Voxel()))
        {
            /* An action (no values, no stepper) has no left arrow and no
             * back half to touch: LEFT must not run it. */
            if (back && !extra->count && !extra->step)
                return;
            if (extra->step)
                extra->step(back ? -1 : 1);
            else
                CtrExtras_Step(extra, back ? -1 : 1);
            /* An action plays its own sound (done, or not possible). */
            if (extra->count || extra->step)
                PlaySE(SE_SELECT);
        }
        return;
    }
    static const u8 counts[OPTION_ROWS] = {3, 2, 2, 2, 3, WINDOW_FRAMES_COUNT, 2, 2, 0, 0, 2, 2};
    u8 value, step = back ? counts[row] - 1 : 1;

    if (!OptionLive(row, CtrSettings_Voxel()))
        return;
    if (row == OPT_FPS)
    {
        CtrSettings_SetShowFps(!CtrSettings_ShowFps());
        PlaySE(SE_SELECT);
        return;
    }
    if (row == OPT_VOXEL)
    {
        CtrSettings_SetVoxel(!CtrSettings_Voxel());
        PlaySE(SE_SELECT);
        return;
    }
    if (row == OPT_VOXEL_BATTLE)
    {
        if (!CtrSettings_Voxel())
            return;
        CtrSettings_SetVoxelBattle(!CtrSettings_VoxelBattle());
        PlaySE(SE_SELECT);
        return;
    }
    if (row == OPT_VOXEL_PITCH || row == OPT_VOXEL_ZOOM || row == OPT_VOXEL_BLUR)
    {
        if (!CtrSettings_Voxel())
            return;
        if (row == OPT_VOXEL_PITCH)
            CtrSettings_StepVoxelPitch(back ? -1 : 1);
        else if (row == OPT_VOXEL_ZOOM)
            CtrSettings_StepVoxelZoom(back ? -1 : 1);
        else
            CtrSettings_StepVoxelBlur(back ? -1 : 1);
        PlaySE(SE_SELECT);
        return;
    }
    switch (row)
    {
    case 0: value = gSaveBlock2Ptr->optionsTextSpeed; break;
    case 1: value = gSaveBlock2Ptr->optionsBattleSceneOff; break;
    case 2: value = gSaveBlock2Ptr->optionsBattleStyle; break;
    case 3: value = gSaveBlock2Ptr->optionsSound; break;
    case 4: value = gSaveBlock2Ptr->optionsButtonMode; break;
    default: value = gSaveBlock2Ptr->optionsWindowFrameType; break;
    }
    value = (value + step) % counts[row];
    switch (row)
    {
    case 0: gSaveBlock2Ptr->optionsTextSpeed = value; break;
    case 1: gSaveBlock2Ptr->optionsBattleSceneOff = value; break;
    case 2: gSaveBlock2Ptr->optionsBattleStyle = value; break;
    case 3: gSaveBlock2Ptr->optionsSound = value; SetPokemonCryStereo(value); break;
    case 4: gSaveBlock2Ptr->optionsButtonMode = value; break;
    default: gSaveBlock2Ptr->optionsWindowFrameType = value; break;
    }
    PlaySE(SE_SELECT);
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
    StartPlan(PLAN_START, screen == SCR_BAG ? START_BAG : screen == SCR_POKEMON ? START_POKEMON : START_POKEDEX);
    BeginSession(FALSE);
    return TRUE;
}

static void OpenAsked(u8 mode)
{
    static u8 lastMode = MODE_OFF;
    static u16 waited;

    /* Closed by its own button or B: back to the map, not opened again. */
    if (mode != lastMode)
    {
        if (lastMode == MODE_POKEDEX && sScreen == SCR_POKEDEX)
            sScreen = SCR_MAP;
        lastMode = mode;
        waited = 0;
    }
    if (mode != MODE_FIELD || sScreen != SCR_POKEDEX || sSession.active
     || sPlan.kind != PLAN_NONE)
        return;
    if (!(EnabledScreens() & (1 << sScreen)))
        sScreen = SCR_MAP;
    else if (OpenGameScreen(sScreen))
        waited = 0;
    /* Two seconds without the field coming back idle: given up. */
    else if (++waited > 120)
        sScreen = SCR_MAP;
}

/* The next Pokémon in the party from `from`, `step` +1 or -1, wrapping. */
static u8 NextPartySlot(u8 from, s8 step)
{
    for (int i = 1; i < PARTY_SIZE; ++i)
    {
        u8 slot = (from + PARTY_SIZE + step * i) % PARTY_SIZE;

        if (slot < gPlayerPartyCount && GetMonData(&gPlayerParty[slot], MON_DATA_SPECIES) != SPECIES_NONE)
            return slot;
    }
    return from;
}

/* pokemon_summary_screen.c's SwapMonMoves: the moves, their PP and their
 * PP Ups change places. */
static void SwapPartyMonMoves(struct Pokemon *mon, u8 a, u8 b)
{
    u16 moveA = GetMonData(mon, MON_DATA_MOVE1 + a), moveB = GetMonData(mon, MON_DATA_MOVE1 + b);
    u8 ppA = GetMonData(mon, MON_DATA_PP1 + a), ppB = GetMonData(mon, MON_DATA_PP1 + b);
    u8 bonuses = GetMonData(mon, MON_DATA_PP_BONUSES);
    u8 bonusA = (bonuses >> (a * 2)) & 3, bonusB = (bonuses >> (b * 2)) & 3;

    bonuses &= ~((3 << (a * 2)) | (3 << (b * 2)));
    bonuses |= (bonusA << (b * 2)) | (bonusB << (a * 2));
    SetMonData(mon, MON_DATA_MOVE1 + a, &moveB);
    SetMonData(mon, MON_DATA_MOVE1 + b, &moveA);
    SetMonData(mon, MON_DATA_PP1 + a, &ppB);
    SetMonData(mon, MON_DATA_PP1 + b, &ppA);
    SetMonData(mon, MON_DATA_PP_BONUSES, &bonuses);
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: moves %u <-> %u", a, b);
}

static void PartyMessage(const u8 *text, u8 back)
{
    StringCopy(sPartyMsg, text);
    sPartyAct = PACT_MESSAGE;
    sPartyActBack = back;
    sPartyMsgPage = 0;
    sPartyThen = NULL;
}

static void PartyMessageThen(const u8 *text, void (*then)(void))
{
    PartyMessage(text, PACT_NONE);
    sPartyThen = then;
}

static void PartyAsk(const u8 *text, u8 ask)
{
    StringCopy(sPartyMsg, text);
    sPartyAct = PACT_ASK;
    sPartyAsk = ask;
    sPartyMsgPage = 0;
}

static bool8 PartyMessageMore(void)
{
    u8 page[sizeof(sPartyMsg)];

    return MessagePage(sPartyMsg, sPartyMsgPage, page, sizeof(page));
}

/* A nickname and an item's name for the game's party texts. */
static void PartyItemText(u8 *dest, const u8 *template, const u8 *var1, u16 item)
{
    u8 name[ITEM_NAME_LENGTH + 4];

    CopyItemName(item, name);
    ExpandVars(dest, template, var1, name);
}

/* A MAIL given (party_menu.c's CtrParty_GiveMail): written on the game's
 * screen, the field faded out for it; PartyMailWritten has the outcome. */
static void PartyGiveMail(u8 slot, u16 mail)
{
    switch (CtrParty_GiveMail(slot, mail))
    {
    case CTR_MAIL_BAG_FULL:
        PartyMessage(gText_BagFullCouldNotRemoveItem, PACT_NONE);
        break;
    case CTR_MAIL_WRITING:
        sPartyAct = PACT_WRITING;
        CtrLog_Write(CTR_LOG_INPUT, "bottom screen: writing mail %u for %u", mail, slot);
        break;
    default:
        sPartyAct = PACT_NONE;
        break;
    }
}

/* Back from writing: Task_DisplayGaveMailFromPartyMessage's message, or
 * nothing if it was cancelled (all as it was). */
static void PartyMailWritten(void)
{
    u16 mail, old;
    u8 nick[POKEMON_NAME_LENGTH + 2], name[ITEM_NAME_LENGTH + 4], text[sizeof(sPartyMsg)];

    switch (CtrParty_MailResult(&mail, &old))
    {
    case CTR_MAIL_WRITTEN:
        if (old == ITEM_NONE)
        {
            GetMonNickname(&gPlayerParty[sPartySel], nick);
            PartyItemText(text, gText_PkmnWasGivenItem, nick, mail);
        }
        else
        {
            CopyItemName(mail, name);
            PartyItemText(text, gText_SwitchedPkmnItem, name, old);
        }
        PartyMessage(text, PACT_NONE);
        break;
    case CTR_MAIL_CANCELLED:
        sPartyAct = PACT_NONE;
        break;
    }
}

/* CB2_GiveHoldItem without the menu: given, or asked to switch with the
 * one held (Task_SwitchHoldItemsPrompt). */
static void PartyGiveItem(u8 slot, u16 item)
{
    struct Pokemon *mon = &gPlayerParty[slot];
    u16 held = GetMonData(mon, MON_DATA_HELD_ITEM);
    u8 nick[POKEMON_NAME_LENGTH + 2], text[sizeof(sPartyMsg)];

    sScreen = SCR_POKEMON;
    sPartyView = PARTY_LIST;
    sPartySel = slot;
    sPartySwap = PARTY_NO_SWAP;
    GetMonNickname(mon, nick);
    sPartyGiveItem = item;
    if (held != ITEM_NONE)
    {
        PartyItemText(text, gText_PkmnAlreadyHoldingItemSwitch, nick, held);
        PartyAsk(text, PASK_SWITCH_ITEMS);
        return;
    }
    if (ItemIsMail(item))
    {
        PartyGiveMail(slot, item);
        return;
    }
    SetMonData(mon, MON_DATA_HELD_ITEM, &item);
    RemoveBagItem(item, 1);
    PartyItemText(text, gText_PkmnWasGivenItem, nick, item);
    PartyMessage(text, PACT_NONE);
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: gave item %u to %u", item, slot);
}

/* Task_HandleSwitchItemsYesNoInput's YES: the held one back to the bag, if
 * it has room. */
static void PartySwitchItems(void)
{
    struct Pokemon *mon = &gPlayerParty[sPartySel];
    u16 held = GetMonData(mon, MON_DATA_HELD_ITEM), item = sPartyGiveItem;
    u8 name[ITEM_NAME_LENGTH + 4], text[sizeof(sPartyMsg)];

    if (CheckBagHasItem(item, 1) != TRUE)
    {
        sPartyAct = PACT_NONE;
        return;
    }
    if (ItemIsMail(item))
    {
        PartyGiveMail(sPartySel, item);
        return;
    }
    RemoveBagItem(item, 1);
    if (AddBagItem(held, 1) == FALSE)
    {
        AddBagItem(item, 1);
        PartyMessage(gText_BagFullCouldNotRemoveItem, PACT_NONE);
        return;
    }
    SetMonData(mon, MON_DATA_HELD_ITEM, &item);
    CopyItemName(item, name);
    PartyItemText(text, gText_SwitchedPkmnItem, name, held);
    PartyMessage(text, PACT_NONE);
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: switched item %u for %u on %u", held, item, sPartySel);
}

/* ---- Items from the bag on a Pokémon (party_menu.c's CtrParty_*) ---- */

/* CB2_ShowPartyMenuForItemUse and CB2_ChooseMonToGiveItem without the menu:
 * the party, to pick the Pokémon, then back to the bag. */
static void PartyStartUse(u16 item, bool8 give)
{
    sScreen = SCR_POKEMON;
    sPartyView = PARTY_LIST;
    sPartySwap = PARTY_NO_SWAP;
    sPartyUseItem = item;
    sPartyFromBag = TRUE;
    sPartyThen = NULL;
    sPartyAct = give ? PACT_GIVE : PACT_USE;
    StringCopy(sPartyMsg, give                                           ? gText_GiveToWhichPokemon
                          : CheckIfItemIsTMHMOrEvolutionStone(item) == 1 ? gText_TeachWhichPokemon
                                                                         : gText_UseOnWhichPokemon);
    sPartyMsgPage = 0;
}

static void PartyLearnDone(void);
static void PartyLevelUpMoves(bool8 first);

static void PartyNextLevelUpMove(void)
{
    PartyLevelUpMoves(FALSE);
}

static void PartyShowStats(void)
{
    sPartyAct = PACT_STATS;
    sPartyStatsPage = 0;
}

/* gText_PkmnNeedsToReplaceMove for sLearnMove, YES / NO. */
static void PartyAskReplace(void)
{
    u8 nick[POKEMON_NAME_LENGTH + 2], text[sizeof(sPartyMsg)];

    GetMonNickname(&gPlayerParty[sPartySel], nick);
    ExpandVars(text, gText_PkmnNeedsToReplaceMove, nick, gMoveNames[sLearnMove]);
    PartyAsk(text, PASK_REPLACE_MOVE);
}

static void PartyAskStop(void)
{
    u8 nick[POKEMON_NAME_LENGTH + 2], text[sizeof(sPartyMsg)];

    GetMonNickname(&gPlayerParty[sPartySel], nick);
    ExpandVars(text, gText_StopLearningMove2, nick, gMoveNames[sLearnMove]);
    PartyAsk(text, PASK_STOP_LEARNING);
}

static void PartyPickForget(void)
{
    StringCopy(sPartyMsg, gText_WhichMoveToForget);
    sPartyMsgPage = 0;
    sPartyAct = PACT_PICK_MOVE;
    sPartyPick = PICK_FORGET;
}

/* Task_PartyMenuReplaceMove: "1, 2 and… Poof!", then learned. */
static void PartyForget(u8 moveSlot)
{
    u8 nick[POKEMON_NAME_LENGTH + 2], text[sizeof(sPartyMsg)];
    u16 old = GetMonData(&gPlayerParty[sPartySel], MON_DATA_MOVE1 + moveSlot);
    u8 *end;

    if (!CtrParty_ReplaceMove(sPartySel, moveSlot, sLearnMove, sLearnTm))
    {
        PartyMessageThen(gText_HMMovesCantBeForgotten2, PartyPickForget);
        return;
    }
    GetMonNickname(&gPlayerParty[sPartySel], nick);
    ExpandVars(text, gText_12PoofForgotMove, nick, gMoveNames[old]);
    end = text + StringLength(text);
    ExpandVars(end, gText_PkmnLearnedMove3, nick, gMoveNames[sLearnMove]);
    PartyMessageThen(text, PartyLearnDone);
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: move %u forgotten for %u on %u", old, sLearnMove, sPartySel);
}

/* Task_LearnNextMoveOrClosePartyMenu: a level's next move, or done. */
static void PartyLearnDone(void)
{
    if (sLearnLevelUp)
        PartyLevelUpMoves(FALSE);
    else
        sPartyAct = PACT_NONE;
}

/* Task_TryLearnNewMoves, then PartyMenuTryEvolution. */
static void PartyLevelUpMoves(bool8 first)
{
    u8 text[sizeof(sPartyMsg)];
    u16 move = MOVE_NONE;

    switch (CtrParty_LevelUpMove(sPartySel, first, text, sizeof(text), &move))
    {
    case CTR_ITEM_LEARNED:
        PartyMessageThen(text, PartyNextLevelUpMove);
        break;
    case CTR_ITEM_REPLACE:
        sLearnMove = move;
        sLearnTm = ITEM_NONE;
        sLearnLevelUp = TRUE;
        PartyAsk(text, PASK_REPLACE_MOVE);
        break;
    default:
        /* The evolution scene, an event, takes the screens if it comes. */
        CtrParty_TryEvolve(sPartySel);
        sPartyAct = PACT_NONE;
        break;
    }
}

/* gItemUseCB on the Pokémon picked (moveSlot MAX_MON_MOVES: none yet). */
static void PartyUseItem(u8 moveSlot)
{
    u8 text[sizeof(sPartyMsg)];
    u16 move = MOVE_NONE;

    switch (CtrParty_UseItem(sPartySel, sPartyUseItem, moveSlot, text, sizeof(text), sPartyStats, &move))
    {
    case CTR_ITEM_PICK_MOVE:
        StringCopy(sPartyMsg, text);
        sPartyMsgPage = 0;
        sPartyAct = PACT_PICK_MOVE;
        sPartyPick = PICK_PP;
        break;
    case CTR_ITEM_LEVEL_UP:
        PartyMessageThen(text, PartyShowStats);
        break;
    case CTR_ITEM_EVOLVING:
        sPartyAct = PACT_NONE;
        break;
    case CTR_ITEM_LEARNED:
        sLearnLevelUp = FALSE;
        PartyMessageThen(text, PartyLearnDone);
        break;
    case CTR_ITEM_REPLACE:
        sLearnMove = move;
        sLearnTm = sPartyUseItem;
        sLearnLevelUp = FALSE;
        PartyAsk(text, PASK_REPLACE_MOVE);
        break;
    default:
        PartyMessage(text, PACT_NONE);
        break;
    }
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: item %u used on %u", sPartyUseItem, sPartySel);
}

/* Task_HandleSendMailToPCYesNoInput and ...LoseMailMessageYesNoInput's YES. */
static void PartyTakeMail(bool8 toPc)
{
    struct Pokemon *mon = &gPlayerParty[sPartySel];
    u16 item = GetMonData(mon, MON_DATA_HELD_ITEM);

    if (toPc)
        PartyMessage(TakeMailFromMonAndSave(mon) != MAIL_NONE ? gText_MailSentToPC : gText_PCMailboxFull, PACT_NONE);
    else if (AddBagItem(item, 1) == TRUE)
    {
        TakeMailFromMon(mon);
        PartyMessage(gText_MailTakenFromPkmn, PACT_NONE);
    }
    else
        PartyMessage(gText_BagFullCouldNotRemoveItem, PACT_NONE);
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: mail taken from %u (to PC: %u)", sPartySel, toPc);
}

/* CursorCb_TakeItem. */
static void PartyTakeItem(void)
{
    struct Pokemon *mon = &gPlayerParty[sPartySel];
    u16 item = GetMonData(mon, MON_DATA_HELD_ITEM), none = ITEM_NONE;
    u8 nick[POKEMON_NAME_LENGTH + 2], text[sizeof(sPartyMsg)];

    GetMonNickname(mon, nick);
    if (item == ITEM_NONE)
        ExpandVar1(text, gText_PkmnNotHolding, nick);
    else if (AddBagItem(item, 1) == FALSE)
        StringCopy(text, gText_BagFullCouldNotRemoveItem);
    else
    {
        SetMonData(mon, MON_DATA_HELD_ITEM, &none);
        PartyItemText(text, gText_ReceivedItemFromPkmn, nick, item);
        CtrLog_Write(CTR_LOG_INPUT, "bottom screen: took item %u from %u", item, sPartySel);
    }
    PartyMessage(text, PACT_NONE);
}


/* CursorCb_FieldMove: the game's checks and the move itself; a message as
 * the game's when it can't be used. On its way, the bottom screen goes back
 * to the map, as the game's menu closes. */
static void PartyFieldMove(u8 fieldMove, bool8 confirmed)
{
    u8 text[sizeof(sPartyMsg)];

    switch (CtrParty_UseFieldMove(sPartySel, fieldMove, confirmed, text, sizeof(text)))
    {
    case CTR_FIELD_MOVE_FAILED:
        PartyMessage(text, PACT_NONE);
        break;
    case CTR_FIELD_MOVE_ASK:
        PartyAsk(text, PASK_FIELD_MOVE);
        sPartyActMove = fieldMove;
        break;
    case CTR_FIELD_MOVE_CHOOSE_MON:
        StringCopy(sPartyMsg, text);
        sPartyAct = PACT_TARGET;
        sPartyMsgPage = 0;
        break;
    default:
        CtrLog_Write(CTR_LOG_INPUT, "bottom screen: field move %u by %u", fieldMove, sPartySel);
        sPartyAct = PACT_NONE;
        sScreen = SCR_MAP;
        break;
    }
}

/* fldeff_softboiled.c, without the party menu: a fifth of the user's HP to
 * one that is neither fainted, full, an egg nor the user itself. */
static void PartySoftBoiled(u8 target)
{
    struct Pokemon *user = &gPlayerParty[sPartySel], *mon = &gPlayerParty[target];
    u16 hp = GetMonData(mon, MON_DATA_HP), maxHp = GetMonData(mon, MON_DATA_MAX_HP);
    u16 cost = GetMonData(user, MON_DATA_MAX_HP) / 5, userHp = GetMonData(user, MON_DATA_HP), gain;
    u8 nick[POKEMON_NAME_LENGTH + 2], number[4];

    if (hp == 0 || target == sPartySel || hp == maxHp || GetMonData(mon, MON_DATA_IS_EGG))
    {
        PartyMessage(gText_CantBeUsedOnPkmn, PACT_TARGET);
        return;
    }
    if (userHp <= cost)
    {
        PartyMessage(gText_NotEnoughHp, PACT_NONE);
        return;
    }
    gain = maxHp - hp < cost ? maxHp - hp : cost;
    userHp -= cost;
    hp += gain;
    SetMonData(user, MON_DATA_HP, &userHp);
    SetMonData(mon, MON_DATA_HP, &hp);
    PlaySE(SE_USE_ITEM);
    GetMonNickname(mon, nick);
    StringCopy(number, Number(gain, 3, STR_CONV_MODE_LEFT_ALIGN));
    ExpandVars(sPartyMsg, gText_PkmnHPRestoredByVar2, nick, number);
    sPartyAct = PACT_MESSAGE;
    sPartyActBack = PACT_NONE;
    sPartyMsgPage = 0;
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: soft-boiled %u -> %u (%u)", sPartySel, target, gain);
}

/* The list's boxes over it: the actions, a message, YES/NO, the target. */
static bool8 PartyActTap(u8 id)
{
    switch (sPartyAct)
    {
    case PACT_MENU:
        if (id >= HIT_PARTY_ACTION && id < HIT_PARTY_ACTION + MAX_MON_MOVES + 4)
        {
            u8 actions[MAX_MON_MOVES + 4], n = PartyActions(sPartySel, actions), action;

            if (id - HIT_PARTY_ACTION >= n)
                return TRUE;
            action = actions[id - HIT_PARTY_ACTION];
            if (action >= PACTION_FIELD_MOVE)
            {
                /* As SWITCH: not in the middle of an event, nor of a step. */
                if (!FieldIdle())
                    return TRUE;
                PlaySE(SE_SELECT);
                PartyFieldMove(action - PACTION_FIELD_MOVE, FALSE);
                return TRUE;
            }
            if (action == PACTION_SWITCH && !PartyEditable())
                return TRUE;
            sPartyAct = action == PACTION_ITEM ? PACT_ITEM : action == PACTION_MAIL ? PACT_MAIL : PACT_NONE;
            if (action == PACTION_SUMMARY)
            {
                sPartyView = PARTY_SUMMARY;
                sSumPage = SUM_INFO;
                sSumMove = sSumMoveSwap = SUM_NO_MOVE;
            }
            else if (action == PACTION_SWITCH)
                sPartySwap = sPartySel;
        }
        else if (id >= HIT_SLOT && id < HIT_SLOT + PARTY_SIZE && id - HIT_SLOT < gPlayerPartyCount)
            sPartySel = id - HIT_SLOT;
        else
            return TRUE;
        break;
    case PACT_MESSAGE:
        if (id != HIT_PARTY_MESSAGE)
            return TRUE;
        if (PartyMessageMore())
        {
            ++sPartyMsgPage;
            break;
        }
        sPartyAct = sPartyActBack;
        sPartyMsgPage = 0;
        if (sPartyThen)
        {
            void (*then)(void) = sPartyThen;

            sPartyThen = NULL;
            then();
        }
        else if (sPartyAct == PACT_TARGET)
            StringCopy(sPartyMsg, gText_UseOnWhichPokemon);
        break;
    case PACT_USE:
    case PACT_GIVE:
        if (id == HIT_PARTY_BACK)
            sPartyAct = PACT_NONE;
        else if (id >= HIT_SLOT && id < HIT_SLOT + PARTY_SIZE && id - HIT_SLOT < gPlayerPartyCount)
        {
            u8 slot = id - HIT_SLOT;

            if (!PartyEditable())
                return TRUE;
            /* IsSelectedMonNotEgg. */
            if (GetMonData(&gPlayerParty[slot], MON_DATA_IS_EGG))
            {
                PlaySE(SE_FAILURE);
                return TRUE;
            }
            sPartySel = slot;
            if (sPartyAct == PACT_GIVE)
            {
                sPartyAct = PACT_NONE;
                PartyGiveItem(slot, sPartyUseItem);
                PlaySE(SE_SELECT);
            }
            else
                PartyUseItem(MAX_MON_MOVES);
            return TRUE;
        }
        else
            return TRUE;
        break;
    case PACT_PICK_MOVE:
        if (id == HIT_PARTY_BACK)
        {
            /* PP: ReturnToUseOnWhichMon; forgetting: stop learning? */
            if (sPartyPick == PICK_PP)
                PartyStartUse(sPartyUseItem, FALSE);
            else
                PartyAskStop();
        }
        else if (id >= HIT_PARTY_MOVE && id < HIT_PARTY_MOVE + MAX_MON_MOVES
                 && GetMonData(&gPlayerParty[sPartySel], MON_DATA_MOVE1 + id - HIT_PARTY_MOVE) != MOVE_NONE)
        {
            if (!PartyEditable())
                return TRUE;
            if (sPartyPick == PICK_PP)
            {
                PartyUseItem(id - HIT_PARTY_MOVE);
                return TRUE;
            }
            PartyForget(id - HIT_PARTY_MOVE);
        }
        else
            return TRUE;
        break;
    case PACT_STATS:
        if (id != HIT_PARTY_MESSAGE)
            return TRUE;
        if (sPartyStatsPage == 0)
            sPartyStatsPage = 1;
        else
            PartyLevelUpMoves(TRUE);
        break;
    case PACT_ITEM:
        if (id == HIT_PARTY_YES && PartyEditable())
        {
            /* GIVE: the bag, to pick the item (CB2_SelectBagItemToGive). */
            ResetBagScreen();
            sBagGiveTo = sPartySel;
            sPartyAct = PACT_NONE;
            sScreen = SCR_BAG;
        }
        else if (id == HIT_PARTY_NO && PartyEditable())
            PartyTakeItem();
        else if (id == HIT_PARTY_BACK)
            sPartyAct = PACT_NONE;
        else
            return TRUE;
        break;
    case PACT_MAIL:
        if (id == HIT_PARTY_YES)
        {
            sPartyAct = PACT_NONE;
            sPartyView = PARTY_MAIL;
        }
        else if (id == HIT_PARTY_NO && PartyEditable())
            PartyAsk(gText_SendMailToPC, PASK_MAIL_TO_PC);
        else if (id == HIT_PARTY_BACK)
            sPartyAct = PACT_NONE;
        else
            return TRUE;
        break;
    case PACT_WRITING:
        return TRUE;
    case PACT_ASK:
        if (id == HIT_PARTY_MESSAGE && PartyMessageMore())
            ++sPartyMsgPage;
        else if (PartyMessageMore())
            return TRUE;
        else if (id == HIT_PARTY_YES && sPartyAsk == PASK_SWITCH_ITEMS && PartyEditable())
            PartySwitchItems();
        else if (id == HIT_PARTY_YES && (sPartyAsk == PASK_MAIL_TO_PC || sPartyAsk == PASK_LOSE_MAIL)
                 && PartyEditable())
            PartyTakeMail(sPartyAsk == PASK_MAIL_TO_PC);
        else if (id == HIT_PARTY_NO && sPartyAsk == PASK_MAIL_TO_PC)
            PartyAsk(gText_MailMessageWillBeLost, PASK_LOSE_MAIL);
        else if (id == HIT_PARTY_YES && sPartyAsk == PASK_FIELD_MOVE && FieldIdle())
            PartyFieldMove(sPartyActMove, TRUE);
        else if (id == HIT_PARTY_YES && sPartyAsk == PASK_REPLACE_MOVE)
            PartyPickForget();
        else if (id == HIT_PARTY_NO && sPartyAsk == PASK_REPLACE_MOVE)
            PartyAskStop();
        else if (id == HIT_PARTY_YES && sPartyAsk == PASK_STOP_LEARNING)
        {
            u8 nick[POKEMON_NAME_LENGTH + 2], text[sizeof(sPartyMsg)];

            GetMonNickname(&gPlayerParty[sPartySel], nick);
            ExpandVars(text, gText_MoveNotLearned, nick, gMoveNames[sLearnMove]);
            PartyMessageThen(text, PartyLearnDone);
        }
        else if (id == HIT_PARTY_NO && sPartyAsk == PASK_STOP_LEARNING)
            PartyAskReplace();
        else if (id == HIT_PARTY_NO)
            sPartyAct = PACT_NONE;
        else
            return TRUE;
        break;
    case PACT_TARGET:
        if (id == HIT_PARTY_BACK)
            sPartyAct = PACT_NONE;
        else if (id >= HIT_SLOT && id < HIT_SLOT + PARTY_SIZE && id - HIT_SLOT < gPlayerPartyCount)
        {
            if (!FieldIdle())
                return TRUE;
            PartySoftBoiled(id - HIT_SLOT);
            if (sPartyAct == PACT_MESSAGE && sPartyActBack == PACT_NONE)
                return TRUE;
        }
        else
            return TRUE;
        break;
    default:
        return FALSE;
    }
    PlaySE(SE_SELECT);
    return TRUE;
}

static void PartyTap(u8 id)
{
    /* Read: back to the Pokémon's actions (CB2_ReturnToPartyMenuFromReadingMail). */
    if (sPartyView == PARTY_MAIL)
    {
        if (id == HIT_PARTY_MESSAGE)
        {
            sPartyView = PARTY_LIST;
            sPartyAct = PACT_MENU;
            PlaySE(SE_SELECT);
        }
        return;
    }
    if (sPartyView == PARTY_LIST && PartyActTap(id))
        return;
    if (sPartyView == PARTY_SUMMARY)
    {
        bool8 egg = GetMonData(&gPlayerParty[sPartySel], MON_DATA_IS_EGG);

        if (id != HIT_PARTY_SWITCH && !(id >= HIT_SUM_MOVE && id < HIT_SUM_MOVE + MAX_MON_MOVES))
            sSumMoveSwap = SUM_NO_MOVE;
        if (id == HIT_PARTY_BACK)
            sPartyView = PARTY_LIST;
        else if ((id == HIT_SUM_PAGE_PREV || id == HIT_SUM_PAGE_NEXT) && !egg)
        {
            sSumPage = (sSumPage + (id == HIT_SUM_PAGE_PREV ? SUM_PAGES - 1 : 1)) % SUM_PAGES;
            sSumMove = SUM_NO_MOVE;
        }
        else if (id == HIT_PARTY_SWITCH && sSumPage >= SUM_BATTLE && sSumMove != SUM_NO_MOVE)
        {
            if (sSumMoveSwap == SUM_NO_MOVE && !PartyEditable())
                return;
            sSumMoveSwap = sSumMoveSwap == SUM_NO_MOVE ? sSumMove : SUM_NO_MOVE;
        }
        else if (id >= HIT_SUM_MOVE && id < HIT_SUM_MOVE + MAX_MON_MOVES && sSumMoveSwap != SUM_NO_MOVE)
        {
            /* The second move: they change places (the same one: stays). */
            u8 other = id - HIT_SUM_MOVE;

            if (other != sSumMoveSwap && PartyEditable()
             && GetMonData(&gPlayerParty[sPartySel], MON_DATA_MOVE1 + other) != MOVE_NONE)
            {
                SwapPartyMonMoves(&gPlayerParty[sPartySel], sSumMoveSwap, other);
                sSumMove = other;
            }
            sSumMoveSwap = SUM_NO_MOVE;
        }
        else if (id >= HIT_SUM_MOVE && id < HIT_SUM_MOVE + MAX_MON_MOVES)
            sSumMove = sSumMove == id - HIT_SUM_MOVE ? SUM_NO_MOVE : id - HIT_SUM_MOVE;
        else if (id == HIT_PARTY_PREV || id == HIT_PARTY_NEXT)
        {
            sPartySel = NextPartySlot(sPartySel, id == HIT_PARTY_PREV ? -1 : 1);
            sSumMove = SUM_NO_MOVE;
        }
        else if (id == HIT_PARTY_SWITCH && PartyEditable())
        {
            sPartySwap = sPartySel;
            sPartyView = PARTY_LIST;
        }
        else
            return;
        PlaySE(SE_SELECT);
        return;
    }
    if (id == HIT_PARTY_BACK)
    {
        if (sPartySwap != PARTY_NO_SWAP)
            sPartySwap = PARTY_NO_SWAP;
        else
            sScreen = SCR_MAP;
        PlaySE(SE_SELECT);
        return;
    }
    if (id < HIT_SLOT || id >= HIT_SLOT + PARTY_SIZE || id - HIT_SLOT >= gPlayerPartyCount)
        return;
    if (sPartySwap == PARTY_NO_SWAP)
    {
        sPartySel = id - HIT_SLOT;
        sPartyAct = PACT_MENU;
        PlaySE(SE_SELECT);
        return;
    }
    /* Moving one: the second tap puts it there (the same one: stays). An
     * event that began in between leaves the party as it was. */
    if (id - HIT_SLOT != sPartySwap && PartyEditable())
    {
        struct Pokemon temp = gPlayerParty[sPartySwap];

        gPlayerParty[sPartySwap] = gPlayerParty[id - HIT_SLOT];
        gPlayerParty[id - HIT_SLOT] = temp;
        CtrLog_Write(CTR_LOG_INPUT, "bottom screen: party %u <-> %u", sPartySwap, id - HIT_SLOT);
    }
    sPartySwap = PARTY_NO_SWAP;
    PlaySE(SE_SELECT);
}

static void BagScroll(int rows)
{
    int total = BagStacks(sBagPocket) + 1, top = sBagTop[sBagPocket] + rows;

    if (top > total - BAG_ROWS)
        top = total - BAG_ROWS;
    if (top < 0)
        top = 0;
    if (top != sBagTop[sBagPocket])
        PlaySE(SE_SELECT);
    sBagTop[sBagPocket] = top;
}

/* {STR_VAR_1} of a bag text as an item's name. */
static void BagItemText(u8 *dest, const u8 *template, u16 item)
{
    u8 name[ITEM_NAME_LENGTH + 4];

    CopyItemName(item, name);
    ExpandVar1(dest, template, name);
}

static void BagGive(u16 item);

/* ItemMenu_UseOutOfBattle (item_use.c's CtrBag_UseItem). */
static void BagUse(u16 item)
{
    u8 text[sizeof(sBagBox)], slot = PARTY_SIZE;

    switch (CtrBag_UseItem(item, sBagPocket == BERRIES_POCKET, text, sizeof(text), &slot))
    {
    case CTR_BAG_USE_MESSAGE:
        BagBox(text, BACT_MESSAGE, FALSE);
        break;
    case CTR_BAG_USE_FIELD:
        /* On the field, as the game's bag closes for it. */
        ResetBagScreen();
        sScreen = SCR_MAP;
        break;
    case CTR_BAG_USE_PARTY:
        PartyStartUse(item, FALSE);
        break;
    case CTR_BAG_USE_SACRED_ASH:
        PartyStartUse(item, FALSE);
        CtrParty_SacredAsh(item, text, sizeof(text));
        PartyMessage(text, PACT_NONE);
        break;
    case CTR_BAG_USE_TMHM:
        BagBox(text, BACT_ASK, FALSE);
        sBagAsk = BASK_TEACH;
        break;
    case CTR_BAG_USE_FIELD_HM:
        BagBox(text, BACT_ASK, FALSE);
        sBagAsk = BASK_FIELD_HM;
        sBagHmUser = slot;
        break;
    case CTR_BAG_USE_MAIL:
        memset(&sBagMail, 0, sizeof(sBagMail));
        memset(sBagMail.words, 0xFF, sizeof(sBagMail.words));
        sBagMail.playerName[0] = EOS;
        sBagMail.itemId = item;
        sBagAct = BACT_MAIL;
        break;
    }
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: bag use %u", item);
}

/* AskTossItems. */
static void BagAskToss(void)
{
    u8 name[ITEM_NAME_LENGTH + 4], text[sizeof(sBagBox)];

    CopyItemName(sBagItem, name);
    ExpandVars(text, gText_ConfirmTossItems, name, Number(sBagTossQty, MAX_ITEM_DIGITS, STR_CONV_MODE_LEFT_ALIGN));
    BagBox(text, BACT_ASK, TRUE);
    sBagAsk = BASK_TOSS;
}

/* The picked item's action from the tray. */
static void BagAction(u8 action, u16 item)
{
    u8 name[ITEM_NAME_LENGTH + 4], text[sizeof(sBagBox)];

    sBagItem = item;
    switch (action)
    {
    case BAG_ACT_USE:
    case BAG_ACT_CHECK:
        /* Standing still, nothing under way: as SELECT. */
        if (!FieldIdle())
            return;
        PlaySE(SE_SELECT);
        BagUse(item);
        return;
    case BAG_ACT_GIVE:
        if (!PartyEditable())
            return;
        PlaySE(SE_SELECT);
        if (sBagGiveTo != PARTY_NO_SWAP)
            BagGive(item);
        else if (!IsWritingMailAllowed(item))
            BagBox(gText_CantWriteMail, BACT_MESSAGE, FALSE);
        else if (GetItemImportance(item))
        {
            BagItemText(text, gText_Var1CantBeHeld, item);
            BagBox(text, BACT_MESSAGE, FALSE);
        }
        else if (CalculatePlayerPartyCount() == 0)
            BagBox(gText_NoPokemon, BACT_MESSAGE, FALSE);
        else
            PartyStartUse(item, TRUE);
        return;
    case BAG_ACT_TOSS:
        if (!PartyEditable())
            return;
        PlaySE(SE_SELECT);
        sBagTossQty = 1;
        if (BagGetQuantityByPocketPosition(sBagPocket + 1, sBagSel[sBagPocket]) == 1)
        {
            BagAskToss();
            return;
        }
        CopyItemName(item, name);
        ExpandVar1(text, gText_TossHowManyVar1s, name);
        BagBox(text, BACT_QTY, TRUE);
        return;
    case BAG_ACT_CHECK_TAG:
        /* ItemMenu_CheckTag: the tag, drawn here (berry_tag_screen.c's). */
        PlaySE(SE_SELECT);
        sBagAct = BACT_TAG;
        return;
    case BAG_ACT_REGISTER:
        /* ItemMenu_Register: the item on SELECT, or none if it was. */
        if (!PartyEditable())
            return;
        gSaveBlock1Ptr->registeredItem = gSaveBlock1Ptr->registeredItem == item ? ITEM_NONE : item;
        PlaySE(SE_SELECT);
        return;
    }
}

/* The tray while a message, a question or how many is up. */
static void BagActTap(u8 id)
{
    u8 text[sizeof(sBagBox)];
    u8 page[sizeof(sBagBox)];
    bool8 more = MessagePage(sBagBox, sBagMsgPage, page, sizeof(page));

    switch (sBagAct)
    {
    case BACT_MAIL:
        if (id != HIT_PARTY_MESSAGE)
            return;
        sBagAct = BACT_NONE;
        break;
    case BACT_TAG:
    {
        /* TryChangeDisplayedBerry: the berry before or after, its row on show. */
        u8 *sel = &sBagSel[sBagPocket], count = BagStacks(sBagPocket);

        if (id == HIT_BAG_QTY && *sel > 0)
            --*sel;
        else if (id == HIT_BAG_QTY + 1 && *sel + 1 < count)
            ++*sel;
        else if (id == HIT_BAG_NO)
            sBagAct = BACT_NONE;
        else
            return;
        if (*sel < sBagTop[sBagPocket])
            sBagTop[sBagPocket] = *sel;
        else if (*sel >= sBagTop[sBagPocket] + BAG_ROWS)
            sBagTop[sBagPocket] = *sel - BAG_ROWS + 1;
        break;
    }
    case BACT_MESSAGE:
        if (id != HIT_BAG_OK)
            return;
        if (more)
            ++sBagMsgPage;
        else
            sBagAct = BACT_NONE;
        break;
    case BACT_QTY:
        if (id >= HIT_BAG_QTY && id < HIT_BAG_QTY + 4)
        {
            /* AdjustQuantityAccordingToDPadInput: one wraps, ten stops. */
            static const s8 steps[] = {-10, -1, 1, 10};
            s32 max = BagGetQuantityByPocketPosition(sBagPocket + 1, sBagSel[sBagPocket]);
            s32 q = sBagTossQty + steps[id - HIT_BAG_QTY];

            if (steps[id - HIT_BAG_QTY] == 1 && q > max)
                q = 1;
            else if (steps[id - HIT_BAG_QTY] == -1 && q < 1)
                q = max;
            sBagTossQty = q < 1 ? 1 : q > max ? max : q;
        }
        else if (id == HIT_BAG_OK)
            BagAskToss();
        else if (id == HIT_BAG_NO)
            sBagAct = BACT_NONE;
        else
            return;
        break;
    case BACT_ASK:
        if (more)
        {
            if (id != HIT_BAG_OK)
                return;
            ++sBagMsgPage;
            break;
        }
        if (id == HIT_BAG_NO)
        {
            /* An HM not used now is booted up instead (BootUpFieldHmInstead). */
            if (sBagAsk == BASK_FIELD_HM)
            {
                CtrBag_TMHMText(sBagItem, text, sizeof(text));
                BagBox(text, BACT_ASK, FALSE);
                sBagAsk = BASK_TEACH;
            }
            else
                sBagAct = BACT_NONE;
            break;
        }
        if (id != HIT_BAG_YES)
            return;
        if (sBagAsk == BASK_TOSS)
        {
            u8 name[ITEM_NAME_LENGTH + 4];

            /* ConfirmToss, then Task_RemoveItemFromBag. */
            if (!PartyEditable())
                return;
            CopyItemName(sBagItem, name);
            ExpandVars(text, gText_ThrewAwayVar2Var1s, name, Number(sBagTossQty, MAX_ITEM_DIGITS, STR_CONV_MODE_LEFT_ALIGN));
            RemoveBagItem(sBagItem, sBagTossQty);
            BagBox(text, BACT_MESSAGE, TRUE);
            CtrLog_Write(CTR_LOG_INPUT, "bottom screen: tossed %u x%u", sBagItem, sBagTossQty);
        }
        else if (sBagAsk == BASK_TEACH)
        {
            sBagAct = BACT_NONE;
            PartyStartUse(sBagItem, FALSE);
        }
        else
        {
            /* UseFieldHmFromBag: as the Pokémon's own move. */
            if (!FieldIdle())
                return;
            if (CtrParty_UseFieldMove(sBagHmUser, CtrParty_FieldMoveOf(ItemIdToBattleMoveId(sBagItem)), FALSE, text,
                                      sizeof(text)) == CTR_FIELD_MOVE_STARTED)
            {
                ResetBagScreen();
                sScreen = SCR_MAP;
            }
            else
                BagBox(text, BACT_MESSAGE, FALSE);
        }
        break;
    default:
        return;
    }
    PlaySE(SE_SELECT);
}

/* Task_ItemContext_GiveToParty's checks, then back to the party with it. */
static void BagGive(u16 item)
{
    if (!IsWritingMailAllowed(item))
        StringCopy(sBagMsg, gText_CantWriteMail);
    else if (!IsHoldingItemAllowed(item))
        BagItemText(sBagMsg, gText_Var1CantBeHeldHere, item);
    else if (sBagPocket == KEYITEMS_POCKET || GetItemImportance(item))
        BagItemText(sBagMsg, gText_Var1CantBeHeld, item);
    else
    {
        u8 slot = sBagGiveTo;

        ResetBagScreen();
        PartyGiveItem(slot, item);
        return;
    }
    PlaySE(SE_FAILURE);
}

static void BagTap(u8 id)
{
    u8 count = BagStacks(sBagPocket), *sel = &sBagSel[sBagPocket];

    sBagMsg[0] = EOS;
    if (sBagAct != BACT_NONE)
    {
        BagActTap(id);
        return;
    }
    if (id >= HIT_BAG_ACT && id < HIT_BAG_ACT + 5)
    {
        u8 actions[5], n;

        if (*sel >= count)
            return;
        n = BagActions(sBagPocket, BagGetItemIdByPocketPosition(sBagPocket + 1, *sel), actions);
        if (id - HIT_BAG_ACT < n)
            BagAction(actions[id - HIT_BAG_ACT], BagGetItemIdByPocketPosition(sBagPocket + 1, *sel));
        return;
    }
    /* Picking for GIVE: CANCEL (or CLOSE BAG) goes back to the party. */
    if (sBagGiveTo != PARTY_NO_SWAP)
    {
        bool8 back = id == HIT_BAG_CANCEL
                  || (id >= HIT_ROW && id < HIT_ROW + BAG_ROWS && sBagTop[sBagPocket] + (id - HIT_ROW) == count
                      && *sel == count);

        if (back)
        {
            ResetBagScreen();
            sScreen = SCR_POKEMON;
            PlaySE(SE_SELECT);
            return;
        }
        if (id == HIT_BAG_REGISTER)
        {
            if (*sel < count && PartyEditable())
            {
                PlaySE(SE_SELECT);
                BagGive(BagGetItemIdByPocketPosition(sBagPocket + 1, *sel));
            }
            return;
        }
        if (id == HIT_BAG_SWITCH)
            return;
    }

    if (id != HIT_BAG_SWITCH && !(id >= HIT_ROW && id < HIT_ROW + BAG_ROWS) && id != HIT_BAG_UP && id != HIT_BAG_DOWN
     && id != HIT_BAG_PAGE_UP && id != HIT_BAG_PAGE_DOWN)
        sBagSwap = BAG_NO_ROW;
    switch (id)
    {
    case HIT_BAG_POCKET_PREV:
    case HIT_BAG_POCKET_NEXT:
        if (id == HIT_BAG_POCKET_PREV ? sBagPocket > 0 : sBagPocket < POCKETS_COUNT - 1)
        {
            sBagPocket += id == HIT_BAG_POCKET_PREV ? -1 : 1;
            PlaySE(SE_SELECT);
        }
        return;
    case HIT_BAG_UP:
    case HIT_BAG_DOWN:
        BagScroll(id == HIT_BAG_UP ? -1 : 1);
        return;
    case HIT_BAG_PAGE_UP:
    case HIT_BAG_PAGE_DOWN:
        BagScroll(id == HIT_BAG_PAGE_UP ? -(BAG_ROWS - 1) : BAG_ROWS - 1);
        return;
    case HIT_BAG_CANCEL:
        sScreen = SCR_MAP;
        PlaySE(SE_SELECT);
        return;
    case HIT_BAG_REGISTER:
        /* ItemMenu_Register: the item on SELECT, or none if it was. */
        if (sBagPocket == KEYITEMS_POCKET && *sel < count && PartyEditable())
        {
            u16 item = BagGetItemIdByPocketPosition(sBagPocket + 1, *sel);

            gSaveBlock1Ptr->registeredItem = gSaveBlock1Ptr->registeredItem == item ? ITEM_NONE : item;
            PlaySE(SE_SELECT);
        }
        return;
    case HIT_BAG_SWITCH:
        if (sBagSwap != BAG_NO_ROW)
            sBagSwap = BAG_NO_ROW;
        else if (*sel < count && count > 1 && sBagPocket != TMHM_POCKET && sBagPocket != BERRIES_POCKET && PartyEditable())
            sBagSwap = *sel;
        else
            return;
        PlaySE(SE_SELECT);
        return;
    }
    if (id < HIT_ROW || id >= HIT_ROW + BAG_ROWS)
        return;
    {
        u8 row = sBagTop[sBagPocket] + (id - HIT_ROW);

        if (row > count)
            return;
        if (sBagSwap != BAG_NO_ROW)
        {
            /* The item moved there, the others closing up (the game's
             * SELECT): onto CANCEL, it goes last. */
            u8 to = row >= count ? count - 1 : row;

            if (to != sBagSwap && PartyEditable())
            {
                /* MoveItemSlotInList takes the gap it goes into: after the
                 * row tapped when moving down. */
                MoveItemSlotInList(gBagPockets[sBagPocket].itemSlots, sBagSwap, to > sBagSwap ? to + 1 : to);
                *sel = to;
                CtrLog_Write(CTR_LOG_INPUT, "bottom screen: bag pocket %u item %u -> %u", sBagPocket, sBagSwap, to);
            }
            sBagSwap = BAG_NO_ROW;
        }
        else if (row == count && *sel == count)
            sScreen = SCR_MAP;   /* CANCEL picked again: the bag closes */
        else
            *sel = *sel == row ? BAG_NO_ROW : row;
        PlaySE(SE_SELECT);
    }
}

/* The game's PokéNav for what is not drawn here yet (CONDITION, RIBBONS). */
static void OpenGamePokenav(void)
{
    if (FieldIdle() && CtrStartMenu_Available())
    {
        StartPlan(PLAN_START, START_POKENAV);
        BeginSession(FALSE);
    }
}

/* CALL: the message as GetMatchCallMessageText writes it, with the string
 * buffers it uses put back. */
static void McCall(u8 index)
{
    static u8 saved[3][sizeof(gStringVar1)], saved4[sizeof(gStringVar4)];
    const struct PokenavMatchCallEntry *e = &sMcEntries[index];
    const u8 *text;

    if (ScriptContext_IsEnabled() || gMain.inBattle)
        return;
    memcpy(saved[0], gStringVar1, sizeof(gStringVar1));
    memcpy(saved[1], gStringVar2, sizeof(gStringVar2));
    memcpy(saved[2], gStringVar3, sizeof(gStringVar3));
    memcpy(saved4, gStringVar4, sizeof(gStringVar4));
    if (!Overworld_MapTypeAllowsTeleportAndFly(gMapHeader.mapType))
        text = gText_CallCantBeMadeHere;
    else
    {
        if (!e->isSpecialTrainer)
            SelectMatchCallMessage(GetTrainerIdxByRematchIdx(e->headerId), gStringVar4);
        else
            MatchCall_GetMessage(e->headerId, gStringVar4);
        text = gStringVar4;
    }
    StringCopy(sMcMessage, text);
    memcpy(gStringVar1, saved[0], sizeof(gStringVar1));
    memcpy(gStringVar2, saved[1], sizeof(gStringVar2));
    memcpy(gStringVar3, saved[2], sizeof(gStringVar3));
    memcpy(gStringVar4, saved4, sizeof(gStringVar4));
    McPaginate();
    sMcPage = 0;
    sMcCall = TRUE;
    PlaySE(SE_POKENAV_CALL);
    CtrLog_Write(CTR_LOG_INPUT, "bottom screen: match call %u (%u pages)", e->headerId, sMcPages);
}

static void PokenavTap(u8 id)
{
    if (sNavView == PNAV_SEARCH_MENU)
    {
        if (id < HIT_NAV_OPTION || id > HIT_NAV_OPTION + SEARCH_OPT_CANCEL)
            return;
        PlaySE(SE_SELECT);
        if (id - HIT_NAV_OPTION != sSearchMenuCursor)
        {
            sSearchMenuCursor = id - HIT_NAV_OPTION;
            return;
        }
        if (sSearchMenuCursor == SEARCH_OPT_CANCEL)
            sNavView = PNAV_CONDITION_MENU;
        else
        {
            sSearchCat = sSearchMenuCursor;
            BuildSearch(sSearchCat);
            sSearchTop = sSearchSel = 0;
            sNavView = PNAV_SEARCH_RESULTS;
        }
        return;
    }
    if (sNavView == PNAV_SEARCH_RESULTS)
    {
        if (id == HIT_NAV_BACK)
            sNavView = PNAV_SEARCH_MENU;
        else if (id == HIT_NAV_UP || id == HIT_NAV_DOWN)
        {
            int top = sSearchTop + (id == HIT_NAV_UP ? -7 : 7);

            sSearchTop = top < 0 ? 0 : top;
        }
        else if (id >= HIT_ROW && id < HIT_ROW + 8 && sSearchTop + (id - HIT_ROW) < sSearchCount)
        {
            sSearchSel = sSearchTop + (id - HIT_ROW);
            sNavView = PNAV_SEARCH_CONDITION;
        }
        else
            return;
        PlaySE(SE_SELECT);
        return;
    }
    if (sNavView == PNAV_SEARCH_CONDITION)
    {
        if (id == HIT_NAV_BACK)
        {
            sNavView = PNAV_SEARCH_RESULTS;
            if (sSearchSel < sSearchTop)
                sSearchTop = sSearchSel;
            else if (sSearchSel >= sSearchTop + 8)
                sSearchTop = sSearchSel - 7;
        }
        else if ((id == HIT_NAV_UP || id == HIT_NAV_DOWN) && sSearchCount > 1)
            sSearchSel = (sSearchSel + (id == HIT_NAV_UP ? sSearchCount - 1 : 1)) % sSearchCount;
        else
            return;
        PlaySE(SE_SELECT);
        return;
    }
    if (sNavView == PNAV_RIBBONS)
    {
        if (id == HIT_NAV_BACK)
            sNavView = PNAV_MENU;
        else if (id == HIT_NAV_UP || id == HIT_NAV_DOWN)
        {
            int top = sRibTop + (id == HIT_NAV_UP ? -7 : 7);

            sRibTop = top < 0 ? 0 : top;
        }
        else if (id >= HIT_ROW && id < HIT_ROW + 8 && sRibTop + (id - HIT_ROW) < sRibListCount)
        {
            /* A Pokémon: its ribbons (the game's A). */
            sRibSel = sRibTop + (id - HIT_ROW);
            sRibBig = RIB_NONE;
            sNavView = PNAV_RIBBON_SUMMARY;
        }
        else
            return;
        PlaySE(SE_SELECT);
        return;
    }
    if (sNavView == PNAV_RIBBON_SUMMARY)
    {
        if (id == HIT_NAV_BACK)
        {
            sNavView = PNAV_RIBBONS;
            if (sRibSel < sRibTop)
                sRibTop = sRibSel;
            else if (sRibSel >= sRibTop + 8)
                sRibTop = sRibSel - 7;
        }
        else if ((id == HIT_NAV_UP || id == HIT_NAV_DOWN) && sRibListCount > 1)
        {
            sRibSel = (sRibSel + (id == HIT_NAV_UP ? sRibListCount - 1 : 1)) % sRibListCount;
            sRibBig = RIB_NONE;
        }
        else if (id >= HIT_ROW && id < HIT_ROW + GIFT_RIBBON_START_POS + 7)
            /* A ribbon: shown big with its description; again, the count. */
            sRibBig = sRibBig == id - HIT_ROW ? RIB_NONE : id - HIT_ROW;
        else
            return;
        PlaySE(SE_SELECT);
        return;
    }
    if (sNavView == PNAV_CONDITION_MENU)
    {
        if (id < HIT_NAV_OPTION || id >= HIT_NAV_OPTION + COND_OPT_COUNT)
            return;
        PlaySE(SE_SELECT);
        if (id - HIT_NAV_OPTION != sCondMenuCursor)
        {
            sCondMenuCursor = id - HIT_NAV_OPTION;
            return;
        }
        if (sCondMenuCursor == COND_OPT_PARTY)
        {
            sNavView = PNAV_CONDITION;
            sCondSel = 0;
        }
        else if (sCondMenuCursor == COND_OPT_SEARCH)
        {
            sNavView = PNAV_SEARCH_MENU;
            sSearchMenuCursor = CONTEST_CATEGORY_COOL;
        }
        else
            sNavView = PNAV_MENU;
        return;
    }
    if (sNavView == PNAV_CONDITION)
    {
        u8 count = 0;

        for (int i = 0; i < PARTY_SIZE && i < gPlayerPartyCount; ++i)
            if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) != SPECIES_NONE && !GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG))
                ++count;
        if (id == HIT_NAV_BACK || (id == HIT_ROW + count && sCondSel == count))
            sNavView = PNAV_CONDITION_MENU;
        else if (id == HIT_NAV_UP || id == HIT_NAV_DOWN)
            sCondSel = (sCondSel + (id == HIT_NAV_UP ? count : 1)) % (count + 1);
        else if (id >= HIT_ROW && id <= HIT_ROW + count)
            sCondSel = id - HIT_ROW;
        else
            return;
        PlaySE(SE_SELECT);
        return;
    }
    if (sNavView == PNAV_MENU)
    {
        u8 options[6], count = NavOptions(options);

        if (id < HIT_NAV_OPTION || id >= HIT_NAV_OPTION + count)
            return;
        if (id - HIT_NAV_OPTION != sNavCursor)
        {
            /* The first tap picks it, as the cursor does; the second opens it. */
            sNavCursor = id - HIT_NAV_OPTION;
            PlaySE(SE_SELECT);
            return;
        }
        PlaySE(SE_SELECT);
        switch (options[sNavCursor])
        {
        case NAV_OPT_MAP:
            sScreen = SCR_MAP;
            break;
        case NAV_OPT_MATCH_CALL:
            sNavView = PNAV_MATCH_CALL;
            sMcTop = 0;
            sMcSel = MC_NO_ENTRY;
            sMcBox = sMcCall = FALSE;
            break;
        case NAV_OPT_CONDITION:
            sNavView = PNAV_CONDITION_MENU;
            sCondMenuCursor = COND_OPT_PARTY;
            break;
        case NAV_OPT_RIBBONS:
            BuildRibbonList();
            sRibTop = sRibSel = 0;
            sRibBig = RIB_NONE;
            sNavView = PNAV_RIBBONS;
            break;
        case NAV_OPT_SWITCH_OFF:
            sScreen = SCR_MAP;
            break;
        default:
            OpenGamePokenav();
            break;
        }
        return;
    }
    if (sMcCall)
    {
        /* The call: a tap for the next page; after the last, it hangs up. */
        if (id == HIT_NAV_MESSAGE)
        {
            if (++sMcPage >= sMcPages)
            {
                sMcCall = FALSE;
                sMcBox = FALSE;
                PlaySE(SE_POKENAV_HANG_UP);
            }
            else
                PlaySE(SE_SELECT);
        }
        return;
    }
    switch (id)
    {
    case HIT_NAV_UP:
    case HIT_NAV_DOWN:
    {
        int top = sMcTop + (id == HIT_NAV_UP ? -(MC_ROWS - 1) : MC_ROWS - 1);

        if (top > (int)sMcCount - MC_ROWS)
            top = sMcCount - MC_ROWS;
        if (top < 0)
            top = 0;
        if (top != sMcTop)
            PlaySE(SE_SELECT);
        sMcTop = top;
        return;
    }
    case HIT_NAV_BACK:
        sNavView = PNAV_MENU;
        sMcBox = FALSE;
        PlaySE(SE_SELECT);
        return;
    case HIT_NAV_CALL:
        if (sMcBox && sMcSel != MC_NO_ENTRY)
        {
            sMcBoxCursor = 0;
            McCall(sMcSel);
        }
        return;
    case HIT_NAV_BOX_CANCEL:
        sMcBox = FALSE;
        PlaySE(SE_SELECT);
        return;
    }
    if (id >= HIT_ROW && id < HIT_ROW + MC_ROWS && sMcTop + (id - HIT_ROW) < sMcCount)
    {
        u8 entry = sMcTop + (id - HIT_ROW);

        /* A trainer: picked, and its options box open (the game's A). */
        sMcSel = entry;
        sMcBox = TRUE;
        sMcBoxCursor = 0;
        PlaySE(SE_SELECT);
    }
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
        /* POKéNAV is the bottom screen's own: its menu and Match Call. */
        if (screen == SCR_POKENAV)
        {
            if (sScreen != SCR_POKENAV)
                ResetNavScreen();
            sScreen = SCR_POKENAV;
            sPickMapsec = MAPSEC_NONE;
            return;
        }
        /* POKéMON is the bottom screen's own: the world goes on above it. */
        if (screen == SCR_POKEMON)
        {
            if (sScreen != SCR_POKEMON)
                ResetPartyScreen();
            sScreen = SCR_POKEMON;
            sPickMapsec = MAPSEC_NONE;
            return;
        }
        /* BAG is the bottom screen's own too. */
        if (screen == SCR_BAG)
        {
            if (sScreen != SCR_BAG)
                ResetBagScreen();
            sScreen = SCR_BAG;
            sPickMapsec = MAPSEC_NONE;
            return;
        }
        /* The game's Pokédex, as the PokéNav: it takes the area when it
         * opens. */
        if (screen == SCR_POKEDEX)
        {
            OpenGameScreen(screen);
            return;
        }
        if (screen == SCR_SAVE && sScreen != SCR_SAVE)
            OpenSave();
        sScreen = screen;
        sPickMapsec = MAPSEC_NONE;
        return;
    }

    switch (sShown.screen)
    {
    case SCR_MAP:
        if (id == HIT_MAP)
            PickMapCell(sTouch.lastX, sTouch.lastY);
        break;
    case SCR_POKEMON:
        if (mode == MODE_FIELD)
            PartyTap(id);
        break;
    case SCR_BAG:
        if (mode == MODE_FIELD)
            BagTap(id);
        break;
    case SCR_POKENAV:
        if (mode == MODE_FIELD)
            PokenavTap(id);
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
        if (id >= HIT_PAGE && id < HIT_PAGE + CTR_EXTRAS_PAGES)
            ShowOptionPage(id - HIT_PAGE);
        else if (id >= HIT_OPTION && id < HIT_OPTION + 2 * HIT_OPTION_BACK)
            ActivateOption(id);
        break;
    }
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
        int ox = (W - 240) / 2, oy = (H - 160) / 2;

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
    /* The game's bag: its picture in the middle of its area, the touch in
     * pixels of it, as it goes. */
    if ((in->touchDown && BagShown(mode) && (sShown.bagView == BAG_VIEW_WHOLE || in->touchX < CW))
     || (sTouch.bag && sTouch.active && BagShown(mode)))
    {
        int ox = sShown.bagView == BAG_VIEW_WHOLE ? (W - 240) / 2 : 0, oy = (H - 160) / 2;

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
    /* The game's Pokédex, the same way: its picture in the middle of the
     * area left of the column. */
    if ((in->touchDown && DexShown(mode) && in->touchX < CW) || (sTouch.bag && sTouch.active && DexShown(mode)))
    {
        int oy = (H - 160) / 2;

        if (in->touchDown)
        {
            sTouch.active = sTouch.bag = TRUE;
            CtrPokedex_Touch(BAG_TOUCH_DOWN, in->touchX, in->touchY - oy);
        }
        else if (in->touchActive)
            CtrPokedex_Touch(BAG_TOUCH_MOVE, in->touchX, in->touchY - oy);
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
        /* The game's party menu, its picture in the middle of its area: the
         * tap goes to whatever waits for input in it (a mon, the buttons, its
         * menu, a question, a message), in pixels of that picture. The
         * column's buttons are ours. */
        if (mode == MODE_PARTY_MENU && (sShown.bagView == BAG_VIEW_WHOLE || sTouch.startX < CW))
        {
            int ox = sShown.bagView == BAG_VIEW_WHOLE ? (W - 240) / 2 : 0, oy = (H - 160) / 2;

            if (!sTouch.dragged)
                CtrMenu_PostTap(sTouch.lastX - ox, sTouch.lastY - oy);
            return HIT_NONE;
        }
        /* The boxes have the whole screen, their picture in the middle of it,
         * and take the tap themselves, in pixels of that picture. */
        if (mode == MODE_STORAGE)
        {
            int x = sTouch.lastX - (W - 240) / 2, y = sTouch.lastY - (H - 160) / 2;

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
        if (mode == MODE_POKENAV && sTouch.startX < CW)
        {
            if (!sTouch.dragged)
                NavTap(sTouch.lastX, CtrVideo_BottomPictureY(sTouch.lastY));
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
/* The column by the buttons (X)                                            */
/* ------------------------------------------------------------------------ */

/*
 * X gives the column a focus ring: the D-pad walks it over the buttons (down
 * from OPTION to Y, left and right between Y and RUN), A does what a tap
 * would, B or X again gives the buttons back to the game. While the column
 * has them the game sees none, so the player stands still. Opened with A,
 * the options take the focus into their grid (up and down through the
 * options, left and right or A to change one, B back to the column) and the
 * save screen takes A for YES and B for NO; the game's own screens (party,
 * bag, Pokédex, PokéNav) take the buttons themselves, and X inside them
 * brings the ring back to the column to go somewhere else.
 */
uint16_t CtrBottom_FilterKeys(uint16_t held)
{
    return sFocus != FOCUS_NONE || sInside != INSIDE_NONE || sSwallow ? 0 : held;
}

/* Whether the column is on screen: not in battle, not under the boxes or
 * the bag or party menu that a battle, a shop or the PC opens. */
static bool8 ColumnShown(u8 mode)
{
    if (gMain.inBattle)
        return FALSE;
    switch (mode)
    {
    case MODE_FIELD:
    case MODE_POKENAV:
    case MODE_POKEDEX:
        return TRUE;
    case MODE_PARTY_MENU:
        return gPartyMenu.menuType == PARTY_MENU_TYPE_FIELD;
    case MODE_BAG_MENU:
        return gBagPosition.location == ITEMMENULOCATION_FIELD;
    }
    return FALSE;
}

static bool8 ColumnItemAvailable(u8 item)
{
    if (item < SCR_COUNT)
        return (EnabledScreens() >> item) & 1;
    if (item == COL_Y)
        return gSaveBlock1Ptr->registeredItem != ITEM_NONE;
    return FlagGet(FLAG_SYS_B_DASH);
}

/* The next available item up (dir < 0) or down the column, or `from`. */
static u8 ColumnStep(u8 from, int dir)
{
    if (from >= COL_Y)
    {
        if (dir > 0)
            return from;
        for (int i = SCR_COUNT - 1; i >= 0; --i)
            if (ColumnItemAvailable(i))
                return i;
        return from;
    }
    for (int i = from + dir; i >= 0 && i < SCR_COUNT; i += dir)
        if (ColumnItemAvailable(i))
            return i;
    if (dir > 0)
    {
        if (ColumnItemAvailable(COL_Y))
            return COL_Y;
        if (ColumnItemAvailable(COL_RUN))
            return COL_RUN;
    }
    return from;
}

/* The next option up or down the grid's reading order (the left column,
 * then the right), or `from`. */
static u8 OptionStep(u8 from, int dir)
{
    bool8 voxel = CtrSettings_Voxel();

    if (!FixedOptions())
    {
        const CtrExtra *extra;

        for (int i = from + dir; i >= 0 && (extra = PageExtra(sOptPage, i)) != NULL; i += dir)
            if (ExtraLive(extra, voxel))
                return i;
        return from;
    }

    for (int i = from + dir; i >= 0 && i < OPTION_ROWS; i += dir)
        if (OptionLive(i, voxel))
            return i;
    return from;
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

/* A on a column item: what a tap on it does, and where the focus goes. */
static void ChooseColumnItem(u8 mode, u8 item)
{
    bool8 gameScreen = item == SCR_POKEMON || item == SCR_BAG || item == SCR_POKEDEX || item == SCR_POKENAV;

    if (item == COL_RUN)
    {
        Activate(HIT_COLUMN + COL_RUN, mode);
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
        if (item == SCR_OPTION && FixedOptions() && !OptionLive(sOptFocus, CtrSettings_Voxel()))
            sOptFocus = OPT_TEXT_SPEED;
    }
}

static void OptionKeys(u16 down)
{
    if (down & B_BUTTON)
    {
        sInside = INSIDE_NONE;
        sFocus = SCR_OPTION;
        PlaySE(SE_SELECT);
    }
    else if (down & DPAD_UP)
        MoveFocus(&sOptFocus, OptionStep(sOptFocus, -1));
    else if (down & DPAD_DOWN)
        MoveFocus(&sOptFocus, OptionStep(sOptFocus, 1));
    else if (down & (L_BUTTON | R_BUTTON))
    {
        int dir = (down & R_BUTTON) ? 1 : -1;

        if ((dir > 0 && sOptSub + 1 < (int)PageSubs(sOptPage)) || (dir < 0 && sOptSub > 0))
            ShowOptionSub(sOptPage, sOptSub + dir);
        else
            for (int page = sOptPage + dir; page >= 0 && page < CTR_EXTRAS_PAGES; page += dir)
                if (page == CTR_EXTRAS_OPTIONS || CtrExtras_PageUsed(page))
                {
                    ShowOptionSub(page, dir > 0 ? 0 : PageSubs(page) - 1);
                    break;
                }
    }
    else if (down & DPAD_LEFT)
        ActivateOption(HIT_OPTION + HIT_OPTION_BACK + sOptFocus);
    else if (down & A_BUTTON)
        ActivateOption(HIT_OPTION + sOptFocus);
    else if (down & DPAD_RIGHT)
    {
        /* RIGHT steps a value. An action (HOENN DEX FULL, GIVE...) has no
         * value to step and cannot be undone: A alone runs it, as a tap. */
        const CtrExtra *extra = FixedOptions() ? NULL : PageExtra(sOptPage, sOptFocus);

        if (!extra || extra->count || extra->step)
            ActivateOption(HIT_OPTION + sOptFocus);
    }
}

static void SaveKeys(u8 mode, u16 down)
{
    if (down & A_BUTTON)
        Activate(sSaveStep == SAVE_DONE ? HIT_OK : HIT_YES, mode);
    else if (down & B_BUTTON)
        Activate(sSaveStep == SAVE_DONE ? HIT_OK : HIT_NO, mode);
    /* Done or declined, the screen goes back to the map: so does the focus,
     * to the column. */
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
        sFocus = SCR_MAP;
    if (x || (down & B_BUTTON))
    {
        LeaveFocus();
        PlaySE(SE_SELECT);
    }
    else if (down & DPAD_UP)
        MoveFocus(&sFocus, ColumnStep(sFocus, -1));
    else if (down & DPAD_DOWN)
        MoveFocus(&sFocus, ColumnStep(sFocus, 1));
    else if ((down & (DPAD_LEFT | DPAD_RIGHT)) && sFocus >= COL_Y)
    {
        u8 other = sFocus == COL_Y ? COL_RUN : COL_Y;

        if (ColumnItemAvailable(other))
            MoveFocus(&sFocus, other);
    }
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
 * every move used to redraw the whole screen - the column's buttons and
 * labels, the name box - 5-15 ms on an Old 3DS, in the frame the move fell
 * in. Now the mark's old square is restored from the map's cache, the mark
 * drawn on its new one, and those two squares sent to the screen.
 */
static bool8 MapCursorOnlyMoved(const ViewState *now, const ViewState *shown)
{
    ViewState moved;

    if (now->screen != SCR_MAP || now->mode == MODE_OFF || now->mode >= MODE_BATTLE_INFO
     || now->inBattle || now->bagView == BAG_VIEW_WHOLE
     || now->mapsec == MAPSEC_NONE || shown->mapsec == MAPSEC_NONE
     || (now->cursorX == shown->cursorX && now->cursorY == shown->cursorY
         && now->mapsec == shown->mapsec))
        return FALSE;
    moved = *now;
    moved.cursorX = shown->cursorX;
    moved.cursorY = shown->cursorY;
    moved.mapsec = shown->mapsec;
    return memcmp(&moved, shown, sizeof(moved)) == 0;
}

/* The mark's 2x2 tiles at a cursor cell, clipped to the map's window. */
static void MarkRect(u8 cx, u8 cy, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = MAP_ORIGIN_X + cx * 8 - 4;
    *y0 = MAP_ORIGIN_Y + cy * 8 - 4;
    *x1 = *x0 + 16;
    *y1 = *y0 + 16;
    if (*x0 < MAP_WIN_X0) *x0 = MAP_WIN_X0;
    if (*y0 < MAP_WIN_Y0) *y0 = MAP_WIN_Y0;
    if (*x1 > MAP_WIN_X1) *x1 = MAP_WIN_X1;
    if (*y1 > MAP_WIN_Y1) *y1 = MAP_WIN_Y1;
    if (*x1 < *x0) *x1 = *x0;
    if (*y1 < *y0) *y1 = *y0;
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
    /* Crossing a region changes only the label below the map. Restore and
     * redraw that plate instead of the map and the column. */
    if (to->mapsec != from->mapsec && to->pickMapsec == MAPSEC_NONE)
    {
        for (int x = NAME_X; x < NAME_X + NAME_W; ++x)
            memcpy(sCanvas + x * H + (H - (NAME_Y + NAME_H)), sCache[CACHE_MAP] + x * H + (H - (NAME_Y + NAME_H)),
                   NAME_H * sizeof(u16));
        ResolveFonts();
        DrawRegionName(to);
        CtrBottom_BlitRect(sCanvas, NAME_X, NAME_Y, NAME_X + NAME_W, NAME_Y + NAME_H);
    }
    for (int k = 0; k < 2; ++k)
        CtrBottom_BlitRect(sCanvas, r[k][0], r[k][1], r[k][2], r[k][3]);
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
    sOptPage = CTR_EXTRAS_OPTIONS;
    sOptSub = 0;
    sOptFocus = OPT_TEXT_SPEED;
    sPickMapsec = MAPSEC_NONE;
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
    if (hold && mode != MODE_POKENAV && mode != MODE_STORAGE && mode != MODE_PARTY_MENU && !BagShown(mode)
     && !DexShown(mode))   /* on show */
        FastForward();

    /* The PokéNav's last frame stays left of the column until repainted. */
    {
        static bool navDrawn;
        bool drawn = CtrVideo_BottomInUse();

        if (drawn != navDrawn)
            sForceRedraw = TRUE;
        navDrawn = drawn;
    }
    ticks[1] = CtrPlatform_Ticks();
    Snapshot(&sState, mode, pressed);

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
    /* The focus ring, or a press on the column: only those buttons. */
    if (!sForceRedraw && sShown.mode != 0xFF && !CtrVideo_BottomWhole() && memcmp(&sState, &sShown, sizeof(sState)) != 0)
    {
        Rect r;

        bool8 dirty = FocusDirtyRect(&sState, &sShown, &r);

        /* The column's hits reach 2px past its edge, the drawing does not. */
        if (dirty && CtrVideo_BottomInUse() && r.x0 < CW)
            r.x0 = CW;
        if (dirty && r.x0 < r.x1)
        {
            RenderPart(&sState, r.x0, r.y0, r.x1, r.y1);
            sShown = sState;
            BottomProfile(frames, mode, ticks, 3);
            return;
        }
    }
    /* A press, a cursor, an HP bar, an option's value: only the part that
     * changes is drawn. */
    if (!sForceRedraw && !CtrVideo_BottomInUse() && !CtrVideo_BottomWhole() && sShown.mode != 0xFF
     && memcmp(&sState, &sShown, sizeof(sState)) != 0 && PartialRedraw(&sState))
    {
        sShown = sState;
        BottomProfile(frames, mode, ticks, 3);
        return;
    }
    /* A part left with a flat label: that part again, the label further on. */
    if (sRefinePart && !sForceRedraw && !CtrVideo_BottomInUse() && !CtrVideo_BottomWhole()
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
    else if (!WarmCardCache(mode))
        WarmOptionCells(mode);
    BottomProfile(frames, mode, ticks, 0);
}

