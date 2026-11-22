/*
 * The bottom screen, game side: what it shows and what a touch does.
 *
 * It replaces the START menu. A header on top (Y, the section's title, the
 * Repel, the clock, RUN), a dock at the bottom with one button a section
 * (MAP, the default, POKéDEX, POKéMON, BAG, POKéNAV, the trainer card, SAVE,
 * OPTIONS, and NUZLOCKE while the tracker is on) and the content area
 * between them; everything happens here while the top screen keeps the
 * world. The game's party menu, summary, bag and Pokédex are drawn here from
 * the game's own state (3ds_bottom_game.inc, the views of 3ds_game_views.h),
 * never as their picture. With CLASSIC MENUS on, and for the PokéNav, the
 * boxes and the game's trainer card, it is the game's 240x160 picture 1:1 in
 * a window in the middle (CtrVideo_BottomInUse, CTR_BOTTOM_WIN_X/Y), drawn by
 * the compositor; a tap on it goes to the game in pixels of that picture
 * (CtrBag_Touch, CtrPokedex_Touch, CtrMenu_PostTap, CtrStorage_Tap, the
 * PokéNav's taps). Opened from a battle, a shop or the PC, the game's screen
 * has the whole screen and there is no dock.
 *
 * A screen of the game that is not the field is drawn over the menu's
 * backdrop, with no dock (MODE_PAD): the Berry Blender, the contest's move
 * choice, Easy Chat, the Rental Pokemon, and any menu or list of the game's
 * waiting for an answer. Each is up from the first frame its own callback
 * runs (PadScreenActive), so neither the field's map nor the game's picture
 * of that screen is ever on show; with nothing to answer the backdrop is
 * darkened (IdleBackdrop), as it is from the main menu to the field
 * (CtrBottom_PreField). The keyboards have their own mode (MODE_NAMING).
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
 * No GPU time, no VRAM, no linear memory - but for the Rental Pokemon's panel
 * on the top screen (3ds_bottom_factory_top.inc), which the compositor shows
 * (3ds_top_panel.h).
 *
 * One translation unit: this file keeps what every piece shares (the state a
 * redraw reads, the modes, the hit zones) and the frame, and includes the
 * rest where the pieces after it need it, 3ds_bottom_<name>.inc each:
 *   canvas, resources, battleart, text, plate
 *       what everything is drawn with: the canvas, what RomFS gives, the
 *       fonts and the smooth labels, the plates and their cache;
 *   mode, battlemenu, snapshot
 *       what the game is doing, what a battle asks, and the snapshot of it;
 *   shell, field, options, game, naming, chat, pad, fieldmenu, blender,
 *   contest, easychat, factory, factory_top
 *       the screens themselves: the header and the dock, the sections drawn
 *       here, the game's menus drawn from their state, the keyboards, and the
 *       screens over the backdrop;
 *   battleplate, battledraw, battlewarm
 *       the battle menus, their plates' pictures and their warm-up;
 *   render, rects, keys_dirty, dirty
 *       the whole redraw, and the part of it a change needs;
 *   plans, nav, taps, actions, focus
 *       input: buttons pressed for the player, the PokéNav, what a tap does,
 *       the touch itself, the dock walked with the buttons;
 *   warm
 *       the plates made ahead in idle frames.
 * tests/bottom_text.py gives the whole as one text to the tests that read it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <math.h>
#include <string.h>

#include "global.h"
#include "3ds_locale.h"
#include "3ds_daynight.h"
#include "3ds_encounters.h"
#include "3ds_locale_text.h"
#include "mail.h"
#include "main.h"
#include "money.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_controllers.h"
#include "battle_interface.h"
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
#include "landmark.h"
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
#include "field_message_box.h"
#include "sound.h"
#include "sprite.h"
#include "constants/easy_chat.h"
#include "string_util.h"
#include "strings.h"
#include "task.h"
#include "text.h"
#include "text_window.h"
#include "util.h"
#include "wild_encounter.h"
#include "constants/battle_frontier.h"
#include "constants/items.h"
#include "constants/maps.h"
#include "constants/moves.h"
#include "constants/vars.h"
#include "constants/map_types.h"
#include "constants/party_menu.h"
#include "constants/region_map_sections.h"
#include "naming_screen.h"
#include "constants/rgb.h"
#include "constants/songs.h"
#include "constants/trainers.h"
#include "port_platform.h"

#include "3ds_audio.h"
#include "3ds_data.h"
#include "3ds_assets.h"
#include "3ds_bottom.h"
#include "3ds_extras.h"
#include "3ds_bottom_art.h"
#include "3ds_clock_store.h"
#include "3ds_input.h"
#include "3ds_local.h"
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
MainCallback CtrStartMenu_DirectCallback(u8 action);
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
void CtrPokenavMenu_Select(int cursor);
void CtrPokenav_Leave(void);
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
/* frontier_pass.c: the game's pass with the classic menus; the bottom
 * screen's own pass with the new ones (3ds_bottom_field.inc). */
bool8 CtrFrontierPass_FieldOpen(void);
u8 CtrFrontierPass_Waiting(void);
void CtrFrontierPass_Tap(int x, int y);
const u8 *CtrFrontierPass_Facility(u8 id, const u8 **description, s16 *x, s16 *y, bool8 *square);
bool8 CtrFrontierPass_PlayerHead(s16 *x, s16 *y);
void CtrFrontierPass_PlayRecord(void);
bool32 CanCopyRecordedBattleSaveData(void);
bool8 CtrBottom_NativeMenus(void);

#define W CTR_BOTTOM_WIDTH
#define H CTR_BOTTOM_HEIGHT
/* The game's picture, 240 wide: centred on the whole screen in a battle. */

#include "3ds_bottom_canvas.inc"
#include "3ds_bottom_resources.inc"
#include "3ds_bottom_battleart.inc"
#include "3ds_bottom_text.inc"

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
    /* A tile's row at a time: its four bytes, pixel px the nibble at bit 4px. */
    for (int y = 0; y < ht * 8; ++y)
        for (int tx = 0; tx < wt; ++tx)
        {
            const u8 *row = tiles + ((y / 8) * wt + tx) * 32 + (y & 7) * 4;
            u32 bits = row[0] | (row[1] << 8) | (row[2] << 16) | ((u32)row[3] << 24);
            int first, last;

            if (!bits)
                continue;
            first = tx * 8 + __builtin_ctz(bits) / 4;
            last = tx * 8 + (31 - __builtin_clz(bits)) / 4;
            if (first < *x0) *x0 = first;
            if (y < *y0) *y0 = y;
            if (last + 1 > *x1) *x1 = last + 1;
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
    /* The naming screen's keyboard, drawn here over the whole screen. */
    MODE_NAMING,
    /* A screen of the game that is not the field: one of those drawn here over the backdrop (the Berry
     * Blender, the contest, Easy Chat, the Rental Pokemon, a menu or a list of the game's), or the idle
     * backdrop alone. */
    MODE_PAD,
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
    u8 held;          /* HELD_NONE, HELD_ITEM, HELD_MAIL */
    u16 hp, maxHp;
    u8 nick[POKEMON_NAME_LENGTH + 2];
} MonView;

/* A battler the target choice shows (species SPECIES_NONE: nobody there):
 * its party icon, whether the move can be aimed at it. */
typedef struct
{
    u16 species, iconSpecies, hp, maxHp;
    u8 deoxys;
    u8 level, gender, live;
    u8 nick[POKEMON_NAME_LENGTH + 1];
} TargetView;

enum { HELD_NONE, HELD_ITEM, HELD_MAIL };

/* A row of the map's wild Pokemon list (map extras): a species of one
 * method's table, its slots' levels and chances added up. */
#define ENC_ROWS 12   /* a method's rows, at most */
/* What the Pokedex knows of a row's species: its seen field. No field of its
 * own for caught: the rows are copied from the stack and compared with memcmp,
 * and one more u8 would add a padding byte that holds whatever was there. */
enum { ENC_UNSEEN, ENC_SEEN, ENC_CAUGHT };
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
    u8 dim;                           /* MODE_OFF from the main menu to the field: the idle backdrop */
    u16 enabled;                      /* bit per section */
    /* Whether the dock and the header are on screen (not under the game's
     * screen opened from a shop or the PC); in a battle's party menu or bag
     * only their own button takes a tap (dockLocked), the others grey. */
    u8 dock, dockLocked;
    /* Party: SAVE's and NUZLOCKE's icons. */
    MonView party[PARTY_SIZE];
    /* Region map, and the header's clock. Zoomed in, the view's top left in
     * map pixels; the picked section's kind (MAPSECTYPE_*), the cell's place
     * in it and the landmarks its panel lists. The Fly map: the towns
     * visited (a bit each from Littleroot), the Battle Frontier's outline,
     * whether the pick is a place to fly to. */
    u8 mapsec, cursorX, cursorY, pickMapsec, pickX, pickY;
    u8 mapZoom, pickType, pickPos, landmarks;
    s16 mapViewX, mapViewY;
    u8 mapFly, flyFrontier, flyCanFly;
    u16 flyVisited;
    /* FLY on the map itself (a town picked one can fly to): MAP_FLY_* for
     * the header's FLY, the party slots that know Fly (a bit each) and the
     * one that would fly. */
    u8 flyNow, flyers, flyPick;
    /* The Fly cinematic under way: the player's mark glides along the route
     * instead of standing on its cell, at this map pixel. */
    u8 flyGlide;
    s16 flyMx, flyMy;
    u8 clockShown, clockValid, clockRate, clockPhase;
    u16 clockMinute;
    /* Map extras (3ds_field_qol_extras.inc), all zero with them off: the
     * list toggle on, the list shown instead of the map, its page (of
     * encPages, 0 with no table here; ENC_SHOWN rows a page, each method
     * its own pages) and method, its rows; the Repel's plate and steps; the
     * clock-not-restored banner. */
    u8 encOn, encView, encPage, encPages, encMethod, encCount, encTotal, encCaught;
    u8 encUncaught, encTogglePage;
    u8 repelShown, clockNotice;
    u16 repelSteps;
    EncRow enc[ENC_SHOWN];
    /* Bag: which of the game's is on show, BAG_VIEW_*. */
    u8 bagView;
    /* Trainer card, its back with cardBack. */
    u8 name[PLAYER_NAME_LENGTH + 1];
    u8 hasDex, stars, badges, minutes, cardBack, hofMinutes;
    /* The Frontier Pass held, for PASS under FLIP. */
    u8 hasPass;
    /* The card's screen with the new menus: the card, the Frontier Pass or
     * its map (CARD_VIEW_*); what the pass describes (PASS_PICK_*), the
     * facility on its map, whether a battle is recorded, the Battle Points,
     * the symbols (two bits a facility: none, silver, gold) and the player's
     * head on the map (passHead, the GBA screen's coordinates). */
    u8 cardView, passPick, passMapPos, passRecord, passHead;
    s16 passHeadX, passHeadY;
    u16 battlePoints, symbols;
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
    TargetView targets[MAX_BATTLERS_COUNT];   /* by target plate */
    u8 text[256];
    /* The game's screen drawn here from its state (3ds_bottom_game.inc),
     * NATIVE_NONE for its picture in the window; what it reads. */
    u8 native;
    bool8 liveParty;        /* read-only field party; never forwards taps to a menu that is not running */
    bool8 liveBag;          /* read-only inventory view; no GBA bag task running */
    struct CtrPartyView pv;
    struct CtrSummaryView sv;
    struct CtrBagView bv;
    struct CtrDexView dx;
    /* MODE_NAMING: the naming screen, whole (3ds_bottom_naming.inc). */
    struct CtrNamingView nm;
    /* ... or the Union Room chat's (3ds_bottom_chat.inc). */
    struct CtrChatView ch;
    u8 chat;
    /* The game's own menu waiting for an answer in the field, as buttons (3ds_bottom_fieldmenu.inc). */
    struct CtrFieldMenuView fm;
    struct CtrFieldListView fl;
    /* The Berry Blender, over the controller (3ds_bottom_blender.inc). */
    struct CtrBlenderView bl;
    /* The contest's move choice, over the controller (3ds_bottom_contest.inc). */
    struct CtrContestView co;
    /* Easy Chat, over the controller (3ds_bottom_easychat.inc). */
    struct CtrEasyChatView ec;
    /* The Rental Pokemon screens, over the controller (3ds_bottom_factory.inc). */
    struct CtrFactoryView fac;
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

/* The bag to deposit from at the PC, to sell from at a shop or to plant a
 * berry from at a soft patch: in the window, but it goes back to the PC's or
 * the shop's menu, or to the patch, as it closes, never to another screen
 * (CtrBottom_LeaveTo), so the dock is locked to its own BAG, which closes it
 * as B does. */
static bool8 BagReturnsToMenu(u8 mode)
{
    return mode == MODE_BAG_MENU
        && (gBagPosition.location == ITEMMENULOCATION_ITEMPC || gBagPosition.location == ITEMMENULOCATION_SHOP
            || gBagPosition.location == ITEMMENULOCATION_BERRY_TREE);
}

/* A party menu a script or a facility opened to choose a Pokemon, and the summary opened from it, go
 * back to that script, never to another screen: the dock is locked to its own POKéMON, which is B. */
static bool8 ChoosesForScript(u8 mode)
{
    return (mode == MODE_PARTY_MENU || mode == MODE_SUMMARY) && !gMain.inBattle
        && gPartyMenu.menuType != PARTY_MENU_TYPE_FIELD;
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
/* #202: a passive party snapshot has no GBA menu task. ACTIONS requests a
 * real menu only once the current walking step has completed. */
static u8 sLivePartyCursor;
static u8 sLivePartyPending = PARTY_SIZE;
static u8 sLivePartyHandoff = PARTY_SIZE;
static u16 sLivePartyHandoffFrames;
/* Live Bag: independent pocket scroll/selection, touched only while browsing.
 * The original BagPosition is updated only when the player requests ACTIONS. */
static u8 sLiveBagPocket;
static u8 sLiveBagCursor[POCKETS_COUNT];
static u8 sLiveBagScroll[POCKETS_COUNT];
static bool8 sLiveBagPending;

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
    if (!voxel && CtrExtras_NeedsVoxel(extra))
        return FALSE;
#ifdef CTR_STEREO_BATTLE
    /* Spacing and placement are inactive until the feature is turned on,
     * matching the existing disabled-setting styling and touch protection. */
    if (strcmp(extra->key, "battle_plane_spread") == 0
        || strcmp(extra->key, "battle_plane_pivot") == 0)
        return CtrSettings_GetInt("battle_mon_3d", 0) == 1 && CtrExtras_Available(extra);
#endif
    return TRUE;
}

/* The OPTIONS category (CAT_*) and group on show (3ds_bottom_options.inc). */
static u8 sOptPage;
static u8 sOptSub;
/* The trainer card's back is up. */
static bool8 sCardBack;
/*
 * The card's screen with the new menus: the card, or the Frontier Pass
 * (PASS under FLIP) drawn here as the game's has it - its symbols and
 * Battle Points, its MAP of the Battle Frontier and the recorded battle -
 * or that map. What the pass describes, the map's facility, and whether a
 * battle is recorded (read from the save as the pass opens, not every frame).
 */
enum { CARD_VIEW_CARD, CARD_VIEW_PASS, CARD_VIEW_MAP };
enum
{
    PASS_PICK_SYMBOLS,
    PASS_PICK_POINTS,
    PASS_PICK_NO_RECORD,
    PASS_PICK_SYMBOL,   /* + facility */
};
static u8 sCardView, sPassPick, sPassMapPos;
static bool8 sPassRecord;

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

static u8 sPickMapsec = MAPSEC_NONE, sPickX, sPickY, sPickPos;
/* MAP zoomed in (twice the size, the view's top left in map pixels); the Fly
 * map: who flies, and the screen it came from (POKéMON or BAG), back there
 * if cancelled. While it is up the game sees no buttons. */
static bool8 sMapZoom, sMapFly;
static u8 sFlySlot, sFlyFrom;
/* Who flies from the map's own FLY: the one last chosen or flown with (its
 * personality), else the first that knows Fly. */
static u32 sFlyerPersonality;
static bool8 sFlyerKnown;
enum { MAP_FLY_NONE, MAP_FLY_GREY, MAP_FLY_READY };
static s16 sMapViewX, sMapViewY;
/* Map extras: the wild list instead of the map, its page; the load
 * (CtrClockStore_LoadSerial) whose clock banner was dismissed. */
static bool8 sEncView, sEncUncaught;
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
    HIT_PASS_MAP,          /* the Frontier Pass's MAP */
    HIT_PASS_RECORD,       /* its recorded battle */
    HIT_PASS_BACK,         /* back to the card, or from the map to the pass */
    HIT_PASS_POINTS,       /* its Battle Points: what they are */
    HIT_PASS_SYMBOLS,      /* its symbols' title: what they are */
    HIT_PASS_SYMBOL = 0x40,   /* + facility: its symbol */
    HIT_PASS_FACILITY = 0x48, /* + facility: on its map */
    HIT_ROW = 0x50,        /* + visible list row */
    HIT_ACTION = 0x60,     /* + action cursor */
    HIT_MOVE = 0x70,       /* + move slot */
    HIT_TARGET = 0x80,     /* + target plate */
    HIT_QUICK_BALL = 0x84, /* throw the selected ball */
    HIT_QUICK_BALL_PREV,
    HIT_QUICK_BALL_NEXT,
    HIT_MAP = 0x90,
    HIT_MAP_INFO,          /* the title: map <-> wild list (map extras) */
    HIT_MAP_FILTER,        /* the wild list: show only uncaught species */
    HIT_MAP_PAGE,          /* the wild list: its next page */
    HIT_CLOCK_NOTICE,      /* the clock banner: hide it */
    HIT_MAP_ZOOM,          /* zoom in or back out */
    HIT_MAP_PANEL,         /* the place's panel: nothing under it */
    HIT_MAP_FLY,           /* the Fly map's FLY */
    HIT_MAP_FLY_CANCEL,    /* the Fly map's CANCEL */
    HIT_MAP_FLY_NOW,       /* the map's own FLY, a town picked */
    HIT_MAP_FLYER,         /* + party slot: who flies */
    HIT_OPTION = 0xC0,     /* + option row; +HIT_OPTION_BACK for its back half */
    HIT_PAGE = 0xE0,       /* + OPTIONS category tab; + 8 + group tab */
};
/* More than there are option rows, so a row and a left arrow never share an id. */
#define HIT_OPTION_BACK 16

typedef struct { s16 x, y, w, h; u8 id; } Hit;
/* The widest naming keyboard can register 80 keys plus name cells, tabs and buttons. */
static Hit sHits[128];
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

#include "3ds_bottom_mode.inc"
#include "3ds_bottom_battlemenu.inc"
#include "3ds_bottom_snapshot.inc"
#include "3ds_bottom_shell.inc"
#include "3ds_bottom_field.inc"
#include "3ds_bottom_options.inc"
#include "3ds_bottom_game.inc"
#include "3ds_bottom_naming.inc"
#include "3ds_bottom_chat.inc"
#include "3ds_bottom_pad.inc"
#include "3ds_bottom_fieldmenu.inc"
#include "3ds_bottom_blender.inc"
#include "3ds_bottom_contest.inc"
#include "3ds_bottom_easychat.inc"
#include "3ds_bottom_factory.inc"
#include "3ds_bottom_factory_top.inc"
#include "3ds_bottom_title_top.inc"
#include "3ds_bottom_battleplate.inc"
#include "3ds_bottom_battledraw.inc"
#include "3ds_bottom_battlewarm.inc"
#include "3ds_bottom_render.inc"
#include "3ds_bottom_rects.inc"
#include "3ds_bottom_keys_dirty.inc"
#include "3ds_bottom_dirty.inc"
#include "3ds_bottom_plans.inc"
#include "3ds_bottom_nav.inc"
#include "3ds_bottom_taps.inc"
#include "3ds_bottom_actions.inc"
#include "3ds_bottom_focus.inc"

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
    {
        BuildBackgroundCaches();
        PlateDiskOpen();
    }
    sShown.mode = 0xFF;
    CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: resources %s in %.1f ms", sRes.ready ? "ready" : "MISSING",
                 CtrPlatform_TickMs(CtrPlatform_Ticks() - start));
}

/*
 * A naming screen is about to open (after the fade that comes first): the top
 * screen keeps what it shows - the battle, the professor's speech, a hatching -
 * while the keyboard is on the bottom one. The classic menus do not change it:
 * they are only the party, the summary and the Pokédex.
 */
void CtrBattleMenu_HoldScene(void);
void CtrBottom_KeepWorld(void);

void CtrBottom_NamingAhead(bool8 battle)
{
    if (battle)
        CtrBattleMenu_HoldScene();
    else
        CtrBottom_KeepWorld();
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
 * A battle is about to fade to black for one of its menus the bottom screen
 * runs: the party menu to send out the next Pokémon (after a faint, or the
 * switch it offers), the bag, the summary to forget a move for a new one
 * (battle_controller_player.c, battle_script_commands.c). The top screen
 * holds the battle scene from this frame on, before the fade darkens it, and
 * shows it dimmed under the menu; a session begun only once the menu is up
 * held the black the fade had left. Opened from the action menu, the session
 * is already on (CtrBattleMenu_ActionInput).
 */
void CtrBattleMenu_HoldScene(void)
{
    /* As CtrBottom_KeepWorld: without CtrBottom_Frame the hold never ends. */
    if (!sRes.ready)
        return;
    if (!sSession.active || !sSession.battle)
        BeginSession(TRUE);
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
     /* Nothing walks on the Fly map; its icons are not in the cache. */
     || now->mapFly || shown->mapFly
     || (now->cursorX == shown->cursorX && now->cursorY == shown->cursorY
         && now->mapsec == shown->mapsec && now->flyGlide == shown->flyGlide
         && now->flyMx == shown->flyMx && now->flyMy == shown->flyMy))
        return FALSE;
    moved = *now;
    moved.cursorX = shown->cursorX;
    moved.cursorY = shown->cursorY;
    moved.mapsec = shown->mapsec;
    moved.flyGlide = shown->flyGlide;
    moved.flyMx = shown->flyMx;
    moved.flyMy = shown->flyMy;
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
    MarkRectOf(from, &r[0][0], &r[0][1], &r[0][2], &r[0][3]);
    MarkRectOf(to, &r[1][0], &r[1][1], &r[1][2], &r[1][3]);

    /* The zoom button and the place panel are drawn over the map, not in its
     * cache: a mark under them is a whole redraw. */
    for (int k = 0; k < 2; ++k)
        if (MapOverlayMeets(to, r[k][0], r[k][1], r[k][2], r[k][3]))
            return FALSE;
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

/* Frames an area's new name waits for (CtrBottom_Frame). */
#define TITLE_WAIT_FRAMES 6

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
    sPreField = FALSE;
    sPadFollows = FALSE;
    memset(&sSession, 0, sizeof(sSession));
    memset(&sPlan, 0, sizeof(sPlan));
    sInjected = 0;
    memset(&sNav, 0, sizeof(sNav));
    memset(&sTouch, 0, sizeof(sTouch));
    sFocus = FOCUS_NONE;
    sInside = INSIDE_NONE;
    sSwallow = FALSE;
    sScreen = SCR_MAP;
    /* No stale selection or queued ACTIONS may survive a soft reset. */
    sLivePartyCursor = 0;
    sLivePartyPending = sLivePartyHandoff = PARTY_SIZE;
    sLivePartyHandoffFrames = 0;
    sLiveBagPocket = ITEMS_POCKET;
    memset(sLiveBagCursor, 0, sizeof(sLiveBagCursor));
    memset(sLiveBagScroll, 0, sizeof(sLiveBagScroll));
    sLiveBagPending = FALSE;
    sMapZoom = sMapFly = FALSE;
    sOptPage = CAT_GENERAL;
    sOptSub = 0;
    sOptFocus = 0;
    sCardBack = FALSE;
    sCardView = CARD_VIEW_CARD;
    sPickMapsec = MAPSEC_NONE;
    sEncView = sEncUncaught = FALSE;
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

#include "3ds_bottom_warm.inc"

/* Set by the APT hook (3ds_platform.c), taken by the next frame. */
static volatile bool sResumed;

void CtrBottom_OnResume(void)
{
    sResumed = true;
}

void CtrBottom_Frame(void)
{
    static u32 frames;
    static bool8 iconsPending, held;
    u8 mode, pressed;
    bool8 hold, look;
    uint64_t ticks[3];

    if (!sRes.ready)
        return;
    /* Back from HOME or sleep: the input was cleared with no touch let go,
     * so a touch held across it is forgotten here (the game's bag, Pokédex
     * or shop told it is cancelled), and the screen is drawn again. */
    if (sResumed)
    {
        sResumed = false;
        if (sTouch.bag)
        {
            CtrBag_Touch(BAG_TOUCH_CANCEL, 0, 0);
            CtrPokedex_Touch(BAG_TOUCH_CANCEL, 0, 0);
            CtrShop_Touch(BAG_TOUCH_CANCEL, 0, 0);
        }
        sTouch.active = sTouch.dragged = sTouch.bag = FALSE;
        sTouch.pressed = HIT_NONE;
        sForceRedraw = TRUE;
    }
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
    CompleteLivePartyHandoff(mode);
    /* Away from the action menu its labels may give way: looked at again. */
    if (mode != MODE_BATTLE_ACTION)
        sQuickWarmBall = ITEM_NONE;
    if (mode < MODE_BATTLE_INFO && sCompAny && !gMain.inBattle)
    {
        /* The plate pictures are only for a battle: kept through its own
         * party menu, summary and bag, which it comes back from. */
        FreeComps();
        sCompAny = FALSE;
        sWarmKey = sWarmIdxKey = 0xFFFFFFFF;
    }
    if (sMapFly && (gMain.inBattle || sScreen != SCR_MAP))
        sMapFly = FALSE;
    if (BattleIntro(mode))
        return;
    /* A screen to leave for, not taken by the one on show as it closed: the
     * field opens it (OpenAsked), or it is dropped after four seconds. */
    if (sLeaveTo < SCR_COUNT && (mode == MODE_FIELD || mode >= MODE_BATTLE_INFO || ++sLeaveToWait > 240))
        sLeaveTo = SCR_COUNT;
    pressed = ProcessTouch(mode);
    sPadHeld = mode == MODE_PAD && sShown.bl.state == CTR_BLENDER_PLAY && !sShown.fm.count && !sShown.fl.count
        ? PadKeysHeld(sTouch.active, sTouch.lastX, sTouch.lastY) : 0;
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
    /* The column in focus, or a section of its own up on the field (the
     * card, SAVE, OPTIONS, the Nuzlocke tracker): the top screen points
     * down here meanwhile; with the top held too, the music a little
     * quieter. */
    look = sFocus != FOCUS_NONE || sInside != INSIDE_NONE
        || (mode == MODE_FIELD && sScreen >= SCR_CARD && sScreen < SCR_COUNT);
    CtrVideo_LookDown(look);
    CtrAudio_DuckMusic(hold || look);
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
    /* Walking into another area, its name waits a few frames: the frame of
     * the crossing loads the map and moves the voxel world, and the title's
     * 2 ms on top of that were a drop on an Old 3DS. */
    {
        static u8 titleWait;

        if (!sForceRedraw && sShown.mode != 0xFF && mode == MODE_FIELD
         && sState.mapsec != sShown.mapsec && sState.mapsec != MAPSEC_NONE && sShown.mapsec != MAPSEC_NONE
         && titleWait < TITLE_WAIT_FRAMES)
        {
            ++titleWait;
            sState.mapsec = sShown.mapsec;
        }
        else
            titleWait = 0;
    }
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
        sIconsIn = TRUE;
        /* A native view's icons are all in its area (an item described, a
         * mon's): that drawn again, unless the header's Y waited too. */
        if (sState.native && sState.native == sShown.native && !sHeaderYMissing && !sForceRedraw
         && sShown.mode != 0xFF && !CtrVideo_BottomWhole())
            sAreaRedraw = TRUE;
        else
            sForceRedraw = TRUE;
    }
    else
        sIconsReady = sIconsIn = TRUE;
    /* The Rental Pokemon's panel on the top screen. */
    FactoryTopFrame(&sState);
    /* The main menu's saved game, as its trainer card on the top screen. */
    TitleTopFrame();

    ticks[2] = CtrPlatform_Ticks();
    if (sAreaRedraw)
    {
        bool8 same = memcmp(&sState, &sShown, sizeof(sState)) == 0;
        Rect r;

        sAreaRedraw = FALSE;
        if (!sForceRedraw && (same || DirtyRect(&sState, &sShown, &r)))
        {
            if (same)
                RectInit(&r);
            AreaRect(&r);
            TimedRenderPart(&sState, r);
            sShown = sState;
            BottomProfile(frames, mode, ticks, 3);
            return;
        }
        sForceRedraw = TRUE;
    }
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
        uint64_t start = CtrPlatform_Ticks(), plateTicksAt = sPlateTicks;
        u32 platesAt = sPlatesMade, evictedAt = sPlatesEvicted;
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
        /* What the game's screen showed, to make its plates ahead next time. */
        if (sShown.native > NATIVE_NONE && sShown.native < NATIVE_BLANK && !sNativeStale && sShown.dock)
        {
            if (sShown.inBattle)
            {
                sWarmSeenBattle[sShown.native] = sShown;
                sWarmSeenBattleOk[sShown.native] = TRUE;
            }
            else
            {
                sWarmSeen[sShown.native] = sShown;
                sWarmSeenOk[sShown.native] = TRUE;
            }
        }
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
            CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: redraw peak %.2f ms (mode %u screen %u) "
                         "plates made=%lu in %.2f ms evicted=%lu", ms, sShown.mode, sShown.screen,
                         (unsigned long)(sPlatesMade - platesAt), CtrPlatform_TickMs(sPlateTicks - plateTicksAt),
                         (unsigned long)(sPlatesEvicted - evictedAt));
        }
        BottomProfile(frames, mode, ticks, 2);
        return;
    }
    if (mode == MODE_BATTLE_INFO)
    {
        WarmBattleMenus();
        if (sBattleMenusReady && !sJob.t && !sRefineLabels)
            WarmScreens(TRUE, 0, WARM_COUNT - 1);
    }
    else if (mode == MODE_BATTLE_ACTION)
    {
        if (WarmQuickBalls(&sShown) && sBattleMenusReady && !sJob.t && !sRefineLabels)
            WarmScreens(TRUE, 0, WARM_COUNT - 1);
    }
    else if (mode == MODE_FIELD && FieldIdle() && !sSession.active && sPlan.kind == PLAN_NONE && !sJob.t
          && !sRefineLabels)
    {
        /* The next battle's party menu made again, from its party then. */
        memset(sWarmBattleDone, 0, sizeof(sWarmBattleDone));
        WarmScreens(FALSE, 0, WARM_COUNT - 1);
    }
    else if (mode == MODE_OFF && sShown.dim && !sShown.fm.count && !sJob.t && !sRefineLabels)
    {
        /* From the main menu to the field: the keyboard the player's name is typed on, made in the
         * frames of the professor's speech. Nothing of the save is read for it. */
        WarmScreens(FALSE, WARM_NAMING, WARM_NAMING);
    }
    PlateDiskAdopt();
    BottomProfile(frames, mode, ticks, 0);
}

