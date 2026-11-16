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
#include "3ds_naming_layout.h"

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
    CTR_BAG_VIEW_QUANTITY,  /* how many to toss, deposit or sell */
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
/* The bag in the bottom screen's window (from the field, the field's party
 * menu, a battle, the PC's deposit or a shop's SELL), not over the whole
 * screen. */
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

/* ------------------------------------------------------------ naming screen */

enum
{
    CTR_NAMING_VIEW_NONE,   /* not up, or fading in or out: nothing to draw */
    CTR_NAMING_VIEW_KEYS,   /* the keyboard, taking keys */
    CTR_NAMING_VIEW_BUSY,   /* a page turning or the cursor travelling: it draws, it takes no tap */
    CTR_NAMING_VIEW_MESSAGE,/* "sent to the PC": msg, a tap goes on */
};

#define CTR_NAMING_ROWS 4
#define CTR_NAMING_COLS 16      /* the widest row: the Japanese kana rows */
#define CTR_NAMING_TEXT 16      /* the game's text buffer */

/* The game's tap kinds (CtrNaming_NativeTap). */
enum
{
    CTR_NAMING_TAP_KEY,     /* a = column, b = row of the keyboard page shown */
    CTR_NAMING_TAP_PAGE,    /* a = the page to turn to (CtrNamingView.page's values) */
    CTR_NAMING_TAP_BACK,    /* delete, as B */
    CTR_NAMING_TAP_OK,      /* accept, as A on OK */
    CTR_NAMING_TAP_CELL,    /* a = a character cell of the text: the caret goes there */
    CTR_NAMING_TAP_MESSAGE, /* a tap on the message: on with it */
};

struct CtrNamingView
{
    u8 state;
    u8 use;                 /* NAMING_SCREEN_PLAYER, _BOX, _CAUGHT_MON, _NICKNAME, _WALDA */
    u8 maxChars;            /* the template's: 7, 8, 10, 15 in most games, 5 for a Japanese name */
    u8 caret;               /* the cell a key would write in */
    u8 text[CTR_NAMING_TEXT]; /* as typed: EOS where there is nothing yet */
    u8 page;                /* the keyboard page shown, the game's own number (0 symbols, 1 upper, 2 lower) */
    u8 cols;                /* the keys in a row on this page */
    u16 gapAfter;           /* bit n: a gap after column n (the keys come in groups) */
    u16 spacer;             /* bit n: column n holds no key (a Japanese row's space between groups) */
    u8 keys[CTR_NAMING_ROWS][CTR_NAMING_COLS]; /* the character each key writes */
    s8 cursorX, cursorY;    /* the game's cursor, for the focus ring; x == cols is the buttons' column */
    u8 gender;              /* a Pokémon's: MON_MALE, MON_FEMALE or MON_GENDERLESS */
    u16 species;            /* the Pokémon named; for the player's name, the player's gender */
    u32 personality;        /* its personality, for its colours */
    u8 title[CTR_VIEW_TEXT]; /* the question as the game prints it, expanded */
    u8 msg[CTR_VIEW_TEXT];  /* MESSAGE: what the game says */
};

bool8 CtrNaming_GetView(struct CtrNamingView *view);
/* True from when the naming screen is created until it is closed. */
bool8 CtrNaming_IsOpen(void);
/* A tap, as the game's own input takes it (CtrNaming_TakeTap in naming_screen.c). */
void CtrNaming_NativeTap(u8 kind, u8 a, u8 b);

/* ------------------------------------------------------------ union room chat */

enum
{
    CTR_CHAT_VIEW_NONE,     /* not up */
    CTR_CHAT_VIEW_KEYS,     /* the keyboard, taking keys */
    CTR_CHAT_VIEW_BUSY,     /* sending, a page turning, a message on its own: drawn, no tap */
    CTR_CHAT_VIEW_MENU,     /* the page menu (SELECT): items, itemCursor */
    CTR_CHAT_VIEW_YESNO,    /* a question: prompt, yes or no */
    CTR_CHAT_VIEW_MESSAGE,  /* a message with no choice; a tap goes on */
    CTR_CHAT_VIEW_PICK,     /* "register where": a row of the registered texts, or cancel */
};

#define CTR_CHAT_COLS 5
#define CTR_CHAT_ROWS 10
#define CTR_CHAT_LINES 4
#define CTR_CHAT_LINE_LEN 64
#define CTR_CHAT_MAX 15         /* MAX_MESSAGE_LENGTH */
#define CTR_CHAT_ITEMS 5
#define CTR_CHAT_PAGE_REGISTER 3   /* UNION_ROOM_KB_PAGE_REGISTER (checked in 3ds_chat_view.inc) */

/* The chat's taps (CtrChat_NativeTap). */
enum
{
    CTR_CHAT_TAP_KEY,       /* a = column, b = row; on the registered page its row is appended */
    CTR_CHAT_TAP_PAGE,      /* a = the page (UNION_ROOM_KB_PAGE_*) */
    CTR_CHAT_TAP_BACK,      /* delete, or quit from an empty message; the cancel of a pick */
    CTR_CHAT_TAP_SEND,      /* START */
    CTR_CHAT_TAP_CASE,      /* R: the last letter's case, or register on the registered page */
    CTR_CHAT_TAP_ITEM,      /* a = the page menu's item, or 0 yes, 1 no */
    CTR_CHAT_TAP_MESSAGE,   /* on with a message */
};

struct CtrChatView
{
    u8 state;
    u8 page;                /* UNION_ROOM_KB_PAGE_* */
    s8 col, row;            /* the game's cursor */
    u8 length;              /* the characters in the message */
    u8 text[2 * CTR_CHAT_MAX + 1];
    /* The page's keys, each a character (two bytes for an extra symbol), EOS where there is none. */
    u8 keys[CTR_CHAT_ROWS][CTR_CHAT_COLS][3];
    u8 registered[CTR_CHAT_ROWS][21];
    /* The last messages received, oldest first, and who from (a colour index). */
    u8 numLines;
    u8 lines[CTR_CHAT_LINES][CTR_CHAT_LINE_LEN];
    u8 lineColor[CTR_CHAT_LINES];
    const u8 *tabs[4];      /* the page menu's titles of the four pages */
    const u8 *items[CTR_CHAT_ITEMS];
    u8 numItems, itemCursor;
    u8 prompt[CTR_VIEW_TEXT];
};

bool8 CtrChat_GetView(struct CtrChatView *view);
bool8 CtrChat_IsOpen(void);
void CtrChat_NativeTap(u8 kind, u8 a, u8 b);

/* ------------------------------------------------------------ a menu in the field */

#define CTR_FM_MAX 12
#define CTR_FM_TEXT 24

/*
 * The game's own menu (menu.c: a script's multichoice, a yes/no box, the PC's
 * and the others that use its cursor) being asked for an answer, for the
 * bottom screen to offer its entries as buttons. What an entry says is what
 * the game printed in the menu's window; a tap is the entry chosen as A
 * would (CtrFieldMenu_Pick).
 */
struct CtrFieldMenuView
{
    u8 count;               /* 0: no menu is waiting */
    u8 columns;             /* 1 for a list */
    u8 cursor;
    u8 yesNo;
    u8 text[CTR_FM_MAX][CTR_FM_TEXT];
};

bool8 CtrFieldMenu_GetView(struct CtrFieldMenuView *view);
/* An entry tapped; -1 is B. */
void CtrFieldMenu_Pick(s8 entry);

/* ------------------------------------------------------------ Berry Blender */

#define CTR_BLENDER_PLAYERS 4

struct CtrBlenderView
{
    u8 state;               /* 0: not blending; 1: the bowl turning, taking A */
    u8 numPlayers, own;     /* who is playing, and which of them is this console's */
    u16 rpm, maxRpm;        /* whole revolutions per minute, now and the best so far */
    u16 progress, progressMax;
    u16 scores[CTR_BLENDER_PLAYERS][3]; /* best, good, miss */
    u16 items[CTR_BLENDER_PLAYERS];     /* the berry each put in */
};

bool8 CtrBlender_GetView(struct CtrBlenderView *view);

/* ------------------------------------------------------------ Contest: the move to appeal with */

#define CTR_CONTEST_MOVES 4
#define CTR_CONTEST_NICK 12
#define CTR_CONTEST_HEARTS 4 /* the hearts a move can have (MAX_CONTEST_MOVE_HEARTS) */

enum { CTR_CONTEST_COMBO = 1, CTR_CONTEST_REPEAT = 2 };
enum { CTR_CONTEST_SELECT, CTR_CONTEST_CONFIRM, CTR_CONTEST_CANCEL };

/*
 * The contest's move choice (Task_HandleMoveSelectInput) while it waits: the
 * four moves with what the game's own page shows of each - its category, the
 * appeal hearts, the jam hearts and the COMBO / repeated marks - and the text
 * of the effect of the one the cursor is on. A tap is handed back as the
 * D-pad, A or B would have been (CtrContest_Act).
 */
struct CtrContestView
{
    u8 state;               /* 0: no choice is waiting */
    u8 count, selected;     /* moves it has, and the cursor's */
    u8 round, rounds;       /* the appeal now, of how many */
    u16 species, personality;
    u8 nick[CTR_CONTEST_NICK];
    u16 moves[CTR_CONTEST_MOVES];
    u8 category[CTR_CONTEST_MOVES];
    u8 hearts[CTR_CONTEST_MOVES], jam[CTR_CONTEST_MOVES];
    u8 flags[CTR_CONTEST_MOVES];
    const u8 *effect;       /* the selected move's description */
};

bool8 CtrContest_GetView(struct CtrContestView *view);
/* kind CTR_CONTEST_SELECT: move `value` of those shown; the others take no value. */
void CtrContest_Act(u8 kind, u8 value);

/* ------------------------------------------------------------ Easy Chat */

#define CTR_EC_WORDS 9
#define CTR_EC_TEXT 20
#define CTR_EC_GROUPS 22

enum { CTR_EC_NONE, CTR_EC_MAIN, CTR_EC_KEYBOARD, CTR_EC_WORDS_PAGE, CTR_EC_MESSAGE, CTR_EC_OTHER };

/* What a tap asks for (CtrEasyChat_Act); each is carried out as the keys the game reads would. */
enum
{
    CTR_EC_ACT_NONE,
    CTR_EC_ACT_SLOT,        /* a: the phrase's word to change */
    CTR_EC_ACT_FOOTER,      /* a: DEL. ALL / CANCEL / OK / (the quiz's) */
    CTR_EC_ACT_GROUP,       /* a: the group, in the order the game lists them */
    CTR_EC_ACT_LETTER,      /* a: row * 8 + column of the alphabet */
    CTR_EC_ACT_WORD,        /* a: the word of the group */
    CTR_EC_ACT_KB_MODE,     /* groups <-> alphabet */
    CTR_EC_ACT_KB_DELETE,   /* clear the word being changed */
    CTR_EC_ACT_BACK,        /* B */
    CTR_EC_ACT_START,       /* START: confirm (main), page up (words) */
    CTR_EC_ACT_SELECT,      /* SELECT: page down (words) */
    CTR_EC_ACT_UP,          /* one row up in the words */
    CTR_EC_ACT_DOWN,        /* one row down in the words */
    CTR_EC_ACT_MESSAGE      /* A: a message read */
};

/*
 * The Easy Chat screen (easy_chat.c): the phrase, and what the game is asking
 * of it - the slot or button the cursor is on, the page of groups or of the
 * alphabet, or the words of a group. Its words are not copied here: the
 * bottom screen asks for them as it draws (CtrEasyChat_WordText).
 */
struct CtrEasyChatView
{
    u8 state;                       /* CTR_EC_NONE ... */
    u8 ready;                       /* the game is taking input, not animating */
    u8 columns, rows, maxWords;     /* the phrase's grid */
    u8 footer;                      /* 3 or 4 buttons under it */
    s8 mainColumn, mainRow;         /* the cursor; row == rows: on the footer */
    u8 alphabet;                    /* the keyboard shows the alphabet, not the groups */
    s8 keyColumn, keyRow;           /* its cursor; column -1: on the side buttons */
    u8 keyScroll;
    u8 numGroups;
    u16 numWords;                   /* of the group opened */
    s8 wordColumn;
    u16 wordRow;                    /* the cursor in the words, the row from the top */
    u16 phrase[CTR_EC_WORDS];       /* EC_EMPTY_WORD (0xFFFF) for none */
    u8 phraseText[CTR_EC_WORDS][CTR_EC_TEXT];
    const u8 *title;
    const u8 *group[CTR_EC_GROUPS];
    const u8 *groupOpen;            /* the name of the group opened, NULL in the alphabet */
};

bool8 CtrEasyChat_GetView(struct CtrEasyChatView *view);
void CtrEasyChat_Act(u8 kind, u8 a);
/* The text of a word of the group opened, "" past the last. */
void CtrEasyChat_WordText(u16 index, u8 *dst);

/* ------------------------------------------------------------ Battle Factory (Rental Pokémon) */

#define CTR_FAC_MONS 6

enum { CTR_FAC_NONE, CTR_FAC_SELECT, CTR_FAC_SWAP };
/* BUSY: the game is animating or between screens; CHOOSE: its cursor on a Pokémon; MENU: the popup; YESNO: the question. */
enum { CTR_FAC_BUSY, CTR_FAC_CHOOSE, CTR_FAC_MENU, CTR_FAC_YESNO };
enum
{
    CTR_FAC_ACT_NONE,
    CTR_FAC_ACT_CURSOR,     /* a: the Pokémon, or (swap) the button 3 + n */
    CTR_FAC_ACT_SLOT,       /* a: the Pokémon, and A on it */
    CTR_FAC_ACT_OPTION,     /* a: the popup's option (opens it first) */
    CTR_FAC_ACT_YES,
    CTR_FAC_ACT_NO,
    CTR_FAC_ACT_BACK
};

/*
 * The Rental Pokémon screens (battle_factory_screen.c): choosing three of
 * six, and swapping one after a win. What the bottom screen needs of either:
 * the Pokémon with their order of choice, where the game's cursor is, the
 * popup's and the question's cursors, and which question it is. A tap is
 * carried out as the keys the game reads (CtrFactory_Act), like Easy Chat's.
 */
struct CtrFactoryView
{
    u8 screen, phase;
    u8 count;                       /* the Pokémon shown: 6, or 3 when swapping */
    u8 cursor;                      /* the Pokémon; swapping, 3 + n is a button */
    u8 buttons;                     /* swapping: the buttons after them (CANCEL, PKMN FOR SWAP) */
    u8 enemy;                       /* swapping: choosing the new one */
    u8 menuCursor, yesNoCursor;
    u8 chosen;                      /* the Pokémon chosen so far (choosing) */
    u8 cursorChosen;                /* the one the cursor is on is among them */
    u16 species[CTR_FAC_MONS], personality[CTR_FAC_MONS];
    u8 order[CTR_FAC_MONS];         /* 0, or 1..3: the order chosen */
    const u8 *question;
    /* The Pokemon the cursor is on (or was last on, while it is on a
     * button), for the top screen's panel: focus is its index. */
    u8 focus, level, ability, nature;
    u16 item;
    u16 moves[MAX_MON_MOVES];
};

bool8 CtrFactory_GetView(struct CtrFactoryView *view);
void CtrFactory_Act(u8 kind, u8 a);

/* ------------------------------------------------------------ a list in the field */

#define CTR_FL_ROWS 8

/*
 * The game's own scrolling list (list_menu.c: a script's long choice, the
 * move relearner's moves, the Pokéblock case...) being asked for an answer,
 * for the bottom screen to offer its rows as buttons. What a row says is the
 * name of its item; a tap chooses it as A would, an arrow scrolls the list as
 * the D-pad does (CtrFieldList_Pick).
 */
struct CtrFieldListView
{
    u8 count;               /* the rows shown, 0: no list is waiting */
    u8 selected;            /* the row the game's cursor is on */
    u16 total, scroll;      /* every item the list has, and the first shown */
    u8 header;              /* bit per row: a heading, not to be chosen */
    u8 text[CTR_FL_ROWS][CTR_FM_TEXT];
};

enum { CTR_FL_ROW, CTR_FL_SCROLL, CTR_FL_CANCEL };

bool8 CtrFieldList_GetView(struct CtrFieldListView *view);
/* kind CTR_FL_ROW: the row (of those shown) chosen; CTR_FL_SCROLL: value rows down (up if negative). */
void CtrFieldList_Pick(u8 kind, s16 value);

#endif
