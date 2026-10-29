#ifndef GUARD_3DS_GAME_VIEWS_H
#define GUARD_3DS_GAME_VIEWS_H

/*
 * The bottom screen's own views of the game's screens (3ds_bottom_game.inc):
 * the game's menu code runs as always and these say where it is, so the
 * bottom screen can draw it natively instead of showing its picture, and
 * take a tap as the game's own tap code takes one (CtrMenu_PostTap). A view
 * the bottom screen cannot draw says so (state NONE) and the game's picture
 * is shown in its window as before.
 */

#include "global.h"

#define CTR_VIEW_ENTRIES 8
#define CTR_VIEW_TEXT 160

/* ------------------------------------------------------------ party menu */

enum
{
    CTR_PARTY_VIEW_NONE,    /* not one the bottom screen draws: the picture */
    CTR_PARTY_VIEW_LIST,    /* choosing a mon (to act on, switch, give, teach) */
    CTR_PARTY_VIEW_MENU,    /* a mon's actions, its moves, an item's GIVE/TAKE */
    CTR_PARTY_VIEW_STATS,   /* a level up's stats: what each rose by (page 0), what it is (1) */
};

#define CTR_PARTY_DESC_NONE 0xFF

struct CtrPartyView
{
    u8 state;
    s8 cursor;              /* the slot the game's cursor is on; PARTY_SIZE + 1 CANCEL */
    s8 moving;              /* the slot being switched, -1 */
    s8 chosen;              /* the slot the menu is about, -1 */
    u8 desc[PARTY_SIZE];    /* the box's description (ABLE, NOT ABLE...), CTR_PARTY_DESC_NONE */
    u8 numEntries;
    s8 entryCursor;
    u8 fieldMove;           /* bit per entry: a field move, coloured */
    s8 cancelEntry;         /* the entry that is CANCEL, -1 */
    bool8 message;          /* a message is up (msg) */
    bool8 yesNo;            /* a yes/no box is open */
    const u8 *entries[CTR_VIEW_ENTRIES];
    u8 prompt[CTR_VIEW_TEXT]; /* the prompt shown ("Choose a POKéMON."), expanded */
    u8 msg[CTR_VIEW_TEXT];  /* the message printed, expanded */
    u8 statsPage;           /* STATS: 0 the rise, 1 the new figures */
    s16 statsBefore[6], statsAfter[6]; /* NUM_STATS: HP ATK DEF SPEED SPATK SPDEF */
    /* The game's icons as they move: the frame each shows, and how far the
     * chosen one bounces (up is negative). Last: CtrParty_IconsOnly. */
    u8 iconFrame[PARTY_SIZE];
    s8 iconLift[PARTY_SIZE];
};

bool8 CtrParty_GetView(struct CtrPartyView *view);
/* A box tapped, as the game's own tap on it; PARTY_SIZE + 1 is CANCEL. */
void CtrParty_TapSlot(s8 slot);
const u8 *CtrParty_DescText(u8 desc);
bool8 CtrParty_Building(void);
/* The party menu is in the bottom screen's window: the field's, or a battle's own. */
bool8 CtrParty_InWindow(void);

/* ------------------------------------------------------------ summary */

enum
{
    CTR_SUM_VIEW_NONE,
    CTR_SUM_VIEW_PAGE,      /* a page (INFO, SKILLS, BATTLE MOVES, CONTEST MOVES) */
};

enum
{
    CTR_SUM_MOVES_VIEW,     /* the moves as they are */
    CTR_SUM_MOVES_CHOOSE,   /* one chosen (its details shown) */
    CTR_SUM_MOVES_SWITCH,   /* one being moved to another place */
    CTR_SUM_MOVES_REPLACE,  /* the one to forget for the new one (a fifth) */
    CTR_SUM_MOVES_HM,       /* that one is an HM: the game says so */
};

/* The summary's taps, as its own code takes them (CtrSummary_NativeTap). */
enum
{
    CTR_SUM_TAP_PAGE,       /* a page's tab: index */
    CTR_SUM_TAP_MON,        /* the previous (0) or next (1) mon */
    CTR_SUM_TAP_MOVE,       /* a move's row: index, 4 the new move */
    CTR_SUM_TAP_BACK,       /* B */
    CTR_SUM_TAP_STATS,      /* SKILLS' stats, IVs, EVs */
    CTR_SUM_TAP_LEAVE,      /* B, for another screen (CtrBottom_LeaveTo) */
};

struct CtrSummaryView
{
    u8 state;
    bool8 loading;          /* still being built: its frame, nothing in it */
    u8 page, minPage, maxPage;
    u8 movesMode;           /* CTR_SUM_MOVES_* */
    u8 firstMove, secondMove;
    bool8 isEgg, shiny, canChangeMon;
    u8 level, gender, ailment, statsMode, ppBonuses;
    u16 species, picSpecies, ball, item, dexNum, otId;
    u32 pid, exp, toNextLevel;
    u16 hp, maxHp, stats[5];   /* ATK DEF SPATK SPDEF SPEED */
    u8 ivs[6], evs[6];
    u16 moves[5];           /* the fifth: the move to learn */
    u8 pp[5];
    u8 types[2];
    const u8 *abilityName, *abilityDesc;
    u8 nick[12], ot[12];
    u8 memo[CTR_VIEW_TEXT];
    /* Last: the mon's sprite as it is now, moving with its animation (the
     * bottom screen redraws only the picture when nothing before changed).
     * picTiles (64x64, the GBA's 1D layout) and picPal are copies, kept
     * after the summary closes; NULL with no sprite to read. picMatrix is
     * the sprite's 8.8 matrix, screen to picture, with picAffine set. */
    const u8 *picTiles;
    const u16 *picPal;
    u32 picSum;
    s16 picX, picY;
    s16 picMatrix[4];
    u8 picAffine;           /* ST_OAM_AFFINE_OFF, _NORMAL or _DOUBLE */
    bool8 picHFlip;
};

bool8 CtrSummary_GetView(struct CtrSummaryView *view);
/* The summary from the field's or a battle's party menu is up (drawn here). */
bool8 CtrSummary_InWindow(void);
void CtrSummary_NativeTap(u8 kind, u8 index);

/* ------------------------------------------------------------ bag */

enum
{
    CTR_BAG_VIEW_NONE,
    CTR_BAG_VIEW_LIST,      /* choosing an item */
    CTR_BAG_VIEW_SWAP,      /* one being moved (swapRow) */
    CTR_BAG_VIEW_ACTIONS,   /* its menu: USE, GIVE, TOSS... */
    CTR_BAG_VIEW_QUANTITY,  /* how many to toss */
    CTR_BAG_VIEW_YESNO,     /* a question (text) and YES/NO */
    CTR_BAG_VIEW_MESSAGE,   /* a message (msg) waiting to go on */
};

#define CTR_BAG_ROWS 8
#define CTR_BAG_NAME 24
#define CTR_BAG_ENTRIES 6

/* A row's extras. */
#define CTR_BAG_ROW_COUNT      (1 << 0) /* its quantity is shown */
#define CTR_BAG_ROW_REGISTERED (1 << 1) /* the item SELECT uses */

struct CtrBagView
{
    u8 state;
    u8 pocket, bagFrame;    /* the bag's picture for the pocket (its frame) */
    bool8 female, pocketSwitch;
    u8 numRows, cursor;     /* the rows shown and the cursor's among them */
    s8 swapRow;             /* the row of the item being moved, -1 */
    u16 scroll, total;      /* the first row shown, the rows there are */
    u16 items[CTR_BAG_ROWS];   /* ITEM_NONE: CLOSE BAG */
    u16 counts[CTR_BAG_ROWS];
    u8 flags[CTR_BAG_ROWS];
    u8 names[CTR_BAG_ROWS][CTR_BAG_NAME];
    u16 item;               /* the item described or acted on */
    u8 numEntries;
    s8 entryCursor;         /* among the entries shown */
    u8 entryPos[CTR_BAG_ENTRIES];  /* each one's place in the game's menu */
    const u8 *entries[CTR_BAG_ENTRIES];
    s16 count, max;
    u8 text[CTR_VIEW_TEXT]; /* what the description window says */
    u8 msg[CTR_VIEW_TEXT];  /* the message printed */
};

bool8 CtrBag_GetView(struct CtrBagView *view);
/* The bag in the bottom screen's window (from the field or the field's party
 * menu), not over the whole screen. */
bool8 CtrBag_InWindow(void);
/* A point of the item list, its rows counted in 1/256s from its top, in
 * pixels of the bag's picture (for CtrBag_Touch). */
void CtrBag_ListPoint(s32 row256, s16 *x, s16 *y);
/* The pocket name's arrows: the previous (-1) or next (1) pocket. */
void CtrBag_TapPocket(s8 dir);
/* How many: delta more (fewer), as the d-pad would count. */
void CtrBag_AddCount(s16 delta);
/* How many: the number taken (as A), or not (as B). */
void CtrBag_TapCount(bool8 ok);
/* Moving an item: left where it was. */
void CtrBag_CancelSwap(void);
u8 CtrBag_SpriteFrame(u8 pocket);

/* ------------------------------------------------------------ Pokédex */

enum
{
    CTR_DEX_VIEW_NONE,
    CTR_DEX_VIEW_LIST,      /* the list, all or a search's results */
    CTR_DEX_VIEW_INFO,      /* a mon's entry */
    CTR_DEX_VIEW_MENU,      /* the list's START menu, over the list */
    CTR_DEX_VIEW_SEARCH,    /* the search */
    CTR_DEX_VIEW_AREA,      /* the entry's area page */
    CTR_DEX_VIEW_CRY,       /* its cry page */
    CTR_DEX_VIEW_SIZE,      /* its size page */
};

/* The search's parameters, in the game's order (pokedex.c's SEARCH_*). */
enum
{
    CTR_DEX_PARAM_NAME,
    CTR_DEX_PARAM_COLOR,
    CTR_DEX_PARAM_TYPE1,
    CTR_DEX_PARAM_TYPE2,
    CTR_DEX_PARAM_ORDER,
    CTR_DEX_PARAM_MODE,
    CTR_DEX_PARAMS
};

/* The search's state: choosing, searching, its result waiting for a tap. */
enum
{
    CTR_DEX_SEARCH_CHOOSE,
    CTR_DEX_SEARCH_BUSY,
    CTR_DEX_SEARCH_FOUND,
    CTR_DEX_SEARCH_NONE_FOUND,
};

/* The area page's map: the region map's cells (32 by 20 of its 8x8 tiles,
 * as region_map.c's GetRegionMapSecIdAt counts them) where the mon lives,
 * and the places marked (caves and the Safari Zone) by their cell. */
#define CTR_DEX_AREA_ROWS 20
#define CTR_DEX_AREA_MARKS 16
#define CTR_DEX_WAVE 64

/* The rows shown round the chosen one: it is in the middle, but for the
 * list's ends, where the rows start at the top (or end at the bottom). */
#define CTR_DEX_ROWS 5

struct CtrDexRow
{
    u16 dexNum;             /* national; 0 no row */
    u16 species;            /* SPECIES_NONE while not seen */
    bool8 owned;
    u8 number[8];           /* as the game numbers it in this dex ("No010") */
};

struct CtrDexView
{
    u8 state;
    bool8 national, searchResults, owned;
    u8 order;
    u16 seen, own, selected, count;
    struct CtrDexRow rows[CTR_DEX_ROWS];
    u8 chosenRow;           /* the row of the chosen one */
    /* The mon chosen (in the list) or the entry's. */
    u16 dexNum, species;
    u8 number[8];
    u8 category[32], height[16], weight[16];
    const u8 *description;
    u8 footprint[32];       /* 16x16, 1bpp, as the game's */
    /* INFO: the tab under the game's cursor (1 AREA, 2 CRY, 3 SIZE, 4 CANCEL). */
    u8 tabCursor;
    /* MENU: its cursor and how many entries (4, or 5 over search results). */
    u8 menuCursor, menuCount;
    /* SEARCH: each parameter's value (index of its options) and how many
     * options it has (0 when it is not offered), the top bar's item and the
     * menu's (the game's cursor), its state. */
    u8 param[CTR_DEX_PARAMS], paramCount[CTR_DEX_PARAMS];
    const u8 *paramText[CTR_DEX_PARAMS];
    u8 topBarItem, menuItem, searchState;
    bool8 searchMenu;       /* the cursor is on the parameters, not the top bar */
    /* AREA: a bit a cell, the marked places, whether none is known. */
    u32 areaRows[CTR_DEX_AREA_ROWS];
    u8 marks, markX[CTR_DEX_AREA_MARKS], markY[CTR_DEX_AREA_MARKS];
    bool8 areaUnknown;
    /* CRY: the waveform drawn so far, oldest first (-127..127), the meter. */
    s8 wave[CTR_DEX_WAVE];
    u8 volume;              /* 0..10 */
    /* SIZE: the game's scales (256 is 1:1) and offsets of the trainer and
     * the mon, the player's gender. */
    u16 trainerScale, monScale;
    s16 trainerOffset, monOffset;
    u8 gender;
};

enum
{
    CTR_DEX_TAP_CHOOSE,     /* A: the chosen mon's entry */
    CTR_DEX_TAP_SEARCH,     /* SELECT: the search */
    CTR_DEX_TAP_TAB,        /* the entry's tab: index */
    CTR_DEX_TAP_BACK,       /* B */
    CTR_DEX_TAP_MENU,       /* the START menu's entry: index (0xFF: close it) */
    CTR_DEX_TAP_PARAM,      /* a search parameter one back (index * 2) or on (+ 1) */
    CTR_DEX_TAP_SEARCH_GO,  /* the search's SEARCH (0) or SHIFT (1) */
    CTR_DEX_TAP_ANY,        /* a message waiting for A goes on */
    CTR_DEX_TAP_PLAY,       /* the cry page's play */
};

bool8 CtrPokedex_GetView(struct CtrDexView *view);
void CtrPokedex_NativeTap(u8 kind, u8 index);
/* A touch on the list, rows256 rows (1/256s) from the chosen one's middle,
 * as the list's own touch at that row (a tap, a drag scrolling it). */
void CtrPokedex_ListTouch(u8 phase, s32 rows256);
/* The scroll bar tapped at at256 of its length (0..256). */
void CtrPokedex_TapBar(s32 at256);
/* A touch on the entry, dy pixels down from where it began: a swipe up or
 * down is the next or previous mon, as in the game's own touch. */
void CtrPokedex_EntryTouch(u8 phase, s16 dy);
/* The game's caught mark: 8x16, 4bpp. */
const u8 *CtrPokedex_CaughtBallGfx(void);

/* ------------------------------------------------------------ menu.c */

/* A tap on the game's screen, in pixels of its picture. */
void CtrMenu_PostTap(s16 x, s16 y);
/* The open menu's entry tapped (or -1: away from it, as B), as a tap on the
 * picture there would be. */
void CtrMenu_PostEntry(s8 entry);
/* A message waiting for A goes on. */
void CtrMenu_PostAck(void);
bool8 CtrMenu_YesNoOpen(void);

#endif
