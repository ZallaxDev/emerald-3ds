/*
 * The Frontier Pass as the top screen draws it natively
 * (3ds_bottom_pass.inc), handed over by frontier_pass.c
 * (0086-frontier-pass-top.patch), and the trainer card it opens, handed over
 * by trainer_card.c (0087-trainer-card-top.patch). The game still runs the
 * pass, its map, the card and their input; this is only what is on show.
 */
#ifndef CTR_PASS_VIEW_H
#define CTR_PASS_VIEW_H

#include "global.h"

struct Sprite;

enum
{
    CTR_PASS_NONE,
    /* The pass: symbols, record, points, the map and the card. */
    CTR_PASS_MAIN,
    /* The Frontier map with its facilities. */
    CTR_PASS_MAP,
    /* The player's own trainer card (trainer_card.c), from the pass or the
     * classic menus' PLAYER. */
    CTR_PASS_CARD,
};

/* frontier_pass.c's cursor areas, the same values. */
enum
{
    CTR_PASS_AREA_NOTHING,
    CTR_PASS_AREA_MAP,
    CTR_PASS_AREA_CARD,
    CTR_PASS_AREA_RECORD,
    CTR_PASS_AREA_CANCEL,
    CTR_PASS_AREA_POINTS,
    CTR_PASS_AREA_EARNED_SYMBOLS,
    CTR_PASS_AREA_SYMBOL_TOWER,
    CTR_PASS_AREA_COUNT = CTR_PASS_AREA_SYMBOL_TOWER + 7,
};

#define CTR_PASS_FACILITIES 7

struct CtrPassView
{
    u8 view;                /* CTR_PASS_* */
    /* The pass. */
    u8 cursorArea;          /* CTR_PASS_AREA_* under the cursor */
    u8 stars;
    bool8 hasRecord;
    u16 battlePoints;
    u8 symbols[CTR_PASS_FACILITIES];        /* 0 none, 1 silver, 2 gold */
    /* Zooming into the map or the card (or out of it): which, and how far
     * as Q8.8 (256 = not at all). */
    u8 zoomArea;
    u16 zoomScale;
    const struct Sprite *cursor;
    const struct Sprite *symbolSprites[CTR_PASS_FACILITIES];
    /* The symbols' sheet in OBJ VRAM, every facility's (four tiles each,
     * TOWER first), for the ones not earned yet; 0xFFFF when not in. */
    u16 medalTiles;
    const u8 *description;  /* the cursor area's text, NULL for none */
    /* The Frontier map's picture, as the game loads it for its map screen
     * (LZ77 4bpp tiles and tilemap, its 16-colour palettes): drawn small
     * on the pass's map button. Asset pointers, resolved by the reader. */
    const void *mapTiles, *mapTilemap, *mapPalette;
    /* The map. */
    u8 mapCursor;           /* the facility chosen */
    const struct Sprite *mapHand, *mapIndicator, *mapHead;
    const u8 *names[CTR_PASS_FACILITIES];
    const u8 *mapDescription;
};

/* What is on show; FALSE (view CTR_PASS_NONE) while neither is up. */
bool8 CtrPass_GetView(struct CtrPassView *view);
/* Whether the pass is open, also while the map, the card or a fade is up. */
bool8 CtrPass_Open(void);

/* The trainer card: its side, how far a flip has folded it, and what its
 * back has that the bottom screen's card leaves out. */
struct CtrCardTop
{
    bool8 onBack;
    /* The fold as the card turns over: 0 open, 256 on its edge. */
    u16 fold;
    u16 frontierBP;
    const u8 *backTitle;    /* "<name>'s TRAINER CARD" as the game words it */
};

/* The player's own card, once it is drawn; FALSE for none or a link one. */
bool8 CtrTrainerCard_TopView(struct CtrCardTop *view);

#endif
