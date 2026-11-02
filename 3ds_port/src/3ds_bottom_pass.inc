/*
 * The Frontier Pass on the top screen (included by 3ds_bottom_ui.c): the
 * pass and its Frontier map drawn natively over the whole 400x240 top screen
 * in the bottom screen's style - its backdrop, plates, fonts and text styles
 * - instead of the game's 240x160 picture.
 *
 * The game still runs both screens and all their input (frontier_pass.c);
 * what is on show comes from it each frame (CtrPass_GetView). Its own art
 * the native screen keeps - the symbols, the hand cursor, the map and card
 * pictures, the Frontier map with its markers - is read from the game's
 * VRAM and palettes as it loaded them, never from the ROM image here, and
 * enlarged with a box filter (PassBlit). Texts are the game's strings; the
 * two it only has as pictures (the pass's title and its cancel button) are
 * CTR_TEXT labels.
 *
 * The game's cursor moves freely over the 240x160 picture and picks an area
 * by its tip (frontier_pass.c, sPassAreasLayout); the plates here stand
 * where those areas map to (PassX, PassY), so the tip drawn over a plate is
 * the area the game has chosen.
 *
 * The picture is composed into its own canvas, the bottom screen's
 * primitives pointed at it (PassTarget), and handed to the video side
 * (3ds_pass_top.c), which draws it and the game's fades over the top screen.
 * Everything but the cursor is composed again only when it changes; the
 * cursor alone goes over a copy of that.
 */

#include "3ds_pass_view.h"
#include "3ds_pass_top.h"
#include "3ds_pass_hand.h"
#include <stddef.h>
#include "gpu_regs.h"

#define PASS_W CTR_PASS_TOP_WIDTH
#define PASS_H CTR_PASS_TOP_HEIGHT
STATIC_ASSERT(PASS_H == H, passTopHeight);

/* The header row as the bottom screen's (3ds_bottom_shell.inc), across 400. */
#define PASS_TITLE_X 4
#define PASS_TITLE_W 244
#define PASS_CANCEL_X 254
#define PASS_CANCEL_W 142
/* The band along the bottom edge for the description, the backdrop's dark
 * strip there as the bottom screen's dock is. */
#define PASS_BAND_Y 204

/* The pass's columns: the symbols, record and points; the map and the card. */
#define PASS_LEFT_X 30
#define PASS_LEFT_W 152
#define PASS_RIGHT_X 220
#define PASS_RIGHT_W 148
#define PASS_SYMBOLS_Y 40
#define PASS_SYMBOLS_H 86
#define PASS_SYMBOLS_HEAD 22
#define PASS_RECORD_Y 130
#define PASS_RECORD_H 24
#define PASS_POINTS_Y 158
#define PASS_POINTS_H 40
#define PASS_MAP_Y 42
#define PASS_CARD_Y 122
#define PASS_THUMB_H 78
/* The map's and the card's pictures on the game's layer 1 (tiles 16,3 and
 * 16,10, 12x7 each) and their size here: 4/3. */
/* The Frontier map's picture on the map button: the map screen's art, the
 * part frontier_pass.c shows (PASS_ART_*), made smaller. */
#define PASS_MAP_PIC_W 136
#define PASS_MAP_PIC_H 70

/* The Frontier map: its picture on the game's layer 2 (in its frame), 1.5x
 * at (PASS_ART_X, PASS_ART_Y); the facilities' plates beside it. */
#define PASS_ART_GX 8
#define PASS_ART_GY 20
#define PASS_ART_GW 144
#define PASS_ART_GH 100
#define PASS_ART_X 10
#define PASS_ART_Y 44
#define PASS_NAMES_X 236
#define PASS_NAMES_W 160
#define PASS_NAMES_Y 42
#define PASS_NAME_H 20
#define PASS_NAME_STEP 22

/* A picture taken from the game: BGR555 with bit 15 set where it is drawn. */
#define PASS_OPAQUE 0x8000
typedef struct
{
    u8 w, h;
    u16 px[32 * 32];
} PassSprite;

static u16 sPassCanvas[PASS_W * PASS_H] __attribute__((aligned(32)));
/* The canvas unlit, and lit without the hand (PassComposeStatic), and the
 * backdrop it starts from. */
static u16 *sPassStatic, *sPassBase, *sPassBackdrop;
/* The map's picture decoded from the game's art, while the pass is open. */
static u16 *sPassMapPic;
/* A button's picture taken off the canvas to grow it as the game zooms. */
static u16 sPassGrab[PASS_RIGHT_W * PASS_THUMB_H];
/* The map's picture on its button, made smaller once (column by column). */
static u16 *sPassMapSmall;

/* ------------------------------------------------------------------------ */
/* Where the game's areas are here                                          */
/* ------------------------------------------------------------------------ */

/*
 * A point of the game's 240x160 pass on the 400x240 screen: across, by
 * 5/3; down, piecewise, so each of the game's areas lands on its plate.
 * The two columns have their own rows (the left one's areas are the
 * symbols, the record and the points; the right one's the map and the
 * card), and between them, where the game has no area, one gives way to
 * the other. Above them both are the cancel button's row, the header.
 * The steps meet, so the cursor never jumps.
 */
typedef struct
{
    s16 game, screen;
} PassKnot;

static const PassKnot sPassRowsLeft[] = {
    {0, 12}, {16, 36}, {24, 40}, {48, 58}, {79, 120}, {80, 130}, {102, 154}, {108, 158}, {134, 198}, {160, 220},
};
static const PassKnot sPassRowsRight[] = {
    {0, 12}, {16, 36}, {28, 42}, {76, 118}, {84, 124}, {132, 198}, {160, 220},
};
/* The game's columns end and start here. */
#define PASS_LEFT_END 108
#define PASS_RIGHT_START 132

static int PassX(int x)
{
    return x * 5 / 3;
}

static int PassRow(const PassKnot *k, unsigned n, int y)
{
    unsigned i = 1;

    while (i < n - 1 && y > k[i].game)
        ++i;
    return k[i - 1].screen + (y - k[i - 1].game) * (k[i].screen - k[i - 1].screen) / (k[i].game - k[i - 1].game);
}

static int PassY(int x, int y)
{
    int l = PassRow(sPassRowsLeft, ARRAY_COUNT(sPassRowsLeft), y);
    int r = PassRow(sPassRowsRight, ARRAY_COUNT(sPassRowsRight), y);

    if (x <= PASS_LEFT_END)
        return l;
    if (x >= PASS_RIGHT_START)
        return r;
    return l + (r - l) * (x - PASS_LEFT_END) / (PASS_RIGHT_START - PASS_LEFT_END);
}

/* The symbols' cells, as frontier_pass.c lays them out (left, top). */
static const u8 sPassSymbolCells[CTR_PASS_FACILITIES][2] = {
    {20, 50}, {32, 66}, {44, 50}, {56, 66}, {68, 50}, {80, 66}, {92, 50},
};

/* ------------------------------------------------------------------------ */
/* Drawing                                                                  */
/* ------------------------------------------------------------------------ */

static inline u16 PassColour(u16 bgr)
{
    return PackRgb((bgr & 31) * 255 / 31, ((bgr >> 5) & 31) * 255 / 31, ((bgr >> 10) & 31) * 255 / 31);
}

/*
 * A picture from the game at (x, y), enlarged to w x h with a box filter:
 * every screen pixel the average of the game pixels it covers, weighted by
 * how much of it each covers, and as opaque as they cover it. At 2x that is
 * every pixel doubled; at 1.5x and 4/3 the pixels stay sharp but for a
 * blended row or column between some of them.
 */
static void PassBlit(const u16 *src, int sw, int sh, int x, int y, int w, int h, bool8 hflip)
{
    if (w <= 0 || h <= 0)
        return;
    for (int dx = 0; dx < w; ++dx)
    {
        int sx0 = dx * sw * 256 / w, sx1 = (dx + 1) * sw * 256 / w, cx = x + dx + sOX;

        if (cx < sClipX0 || cx >= sClipX1)
            continue;
        for (int dy = 0; dy < h; ++dy)
        {
            int sy0 = dy * sh * 256 / h, sy1 = (dy + 1) * sh * 256 / h, cy = y + dy;
            int area = (sx1 - sx0) * (sy1 - sy0), cover = 0, r = 0, g = 0, b = 0;

            if (cy < sClipY0 || cy >= sClipY1)
                continue;
            for (int sy = sy0 >> 8; sy * 256 < sy1; ++sy)
            {
                int wy = (sy * 256 + 256 < sy1 ? sy * 256 + 256 : sy1) - (sy * 256 > sy0 ? sy * 256 : sy0);

                for (int sx = sx0 >> 8; sx * 256 < sx1; ++sx)
                {
                    int wx = (sx * 256 + 256 < sx1 ? sx * 256 + 256 : sx1) - (sx * 256 > sx0 ? sx * 256 : sx0);
                    u16 p = src[sy * sw + (hflip ? sw - 1 - sx : sx)];
                    int k = wx * wy;

                    if (!(p & PASS_OPAQUE))
                        continue;
                    cover += k;
                    r += (p & 31) * k;
                    g += ((p >> 5) & 31) * k;
                    b += ((p >> 10) & 31) * k;
                }
            }
            if (cover == 0)
                continue;
            {
                u16 *dst = sDst + cx * H + (H - 1 - cy);
                int a = cover * 255 / area;
                Rgb c = {r / cover * 255 / 31, g / cover * 255 / 31, b / cover * 255 / 31};

                if (a >= 255)
                    *dst = PackRgb(c.r, c.g, c.b);
                else
                {
                    Rgb u = RgbOf(*dst);

                    *dst = PackRgb(Div255(u.r * (255 - a) + c.r * a), Div255(u.g * (255 - a) + c.g * a),
                                   Div255(u.b * (255 - a) + c.b * a));
                }
            }
        }
    }
}

/* An opaque picture enlarged pixel by pixel: the zoom, every frame of it. */
static void PassBlitNearest(const u16 *src, int sw, int sh, int x, int y, int w, int h)
{
    for (int dx = 0; dx < w; ++dx)
    {
        int cx = x + dx + sOX;
        const u16 *col = src + dx * sw / w;
        u16 *dst;

        if (cx < sClipX0 || cx >= sClipX1)
            continue;
        dst = sDst + cx * H;
        for (int dy = 0; dy < h; ++dy)
        {
            int cy = y + dy;

            if (cy >= sClipY0 && cy < sClipY1)
                dst[H - 1 - cy] = PassColour(col[(dy * sh / h) * sw]);
        }
    }
}

static void PassSpriteAt(const PassSprite *s, int x, int y, int scale2, bool8 hflip)
{
    if (s->w)
        PassBlit(s->px, s->w, s->h, x, y, s->w * scale2 / 2, s->h * scale2 / 2, hflip);
}

/* Where the hand was last drawn, to put back what it covered. */
static s16 sPassHandRect[4];

/* The cursor: the port's own hand (3ds_pass_hand.h, scripts/gen_pass_hand.py)
 * at the screen's resolution, its fingertip at (tx, ty), pointing left, or
 * right on the map. */
static void PassHand(int tx, int ty, bool8 hflip)
{
    int x0 = tx - (hflip ? PASS_HAND_W - 1 - PASS_HAND_TIP_X : PASS_HAND_TIP_X), y0 = ty - PASS_HAND_TIP_Y;

    for (int y = 0; y < PASS_HAND_H; ++y)
    {
        int cy = y0 + y;

        if (cy < sClipY0 || cy >= sClipY1)
            continue;
        for (int x = 0; x < PASS_HAND_W; ++x)
        {
            int cx = x0 + (hflip ? PASS_HAND_W - 1 - x : x) + sOX, i = y * PASS_HAND_W + x;
            int a = sPassHandAlpha[i];
            u16 *dst;

            if (!a || cx < sClipX0 || cx >= sClipX1)
                continue;
            dst = sDst + cx * H + (H - 1 - cy);
            if (a >= 255)
                *dst = sPassHandColour[i];
            else
            {
                Rgb u = RgbOf(*dst), c = RgbOf(sPassHandColour[i]);

                *dst = PackRgb(Div255(u.r * (255 - a) + c.r * a), Div255(u.g * (255 - a) + c.g * a),
                               Div255(u.b * (255 - a) + c.b * a));
            }
        }
    }
    sPassHandRect[0] = x0;
    sPassHandRect[1] = y0;
    sPassHandRect[2] = x0 + PASS_HAND_W;
    sPassHandRect[3] = y0 + PASS_HAND_H;
}

/* The header: the title on the light plate, as every bottom screen's. */
static void PassHeader(const u8 *title, const u8 *cancel, bool8 cancelOn)
{
    int oy = PlateEx(PASS_TITLE_X, HEAD_Y, PASS_TITLE_W, HEAD_H, sLight, PLATE_STATE_NORMAL, PASS_TITLE_W - 10, 30,
                     PlateCut(HEAD_H), 0);

    TextIn(title, PASS_TITLE_X, oy, PASS_TITLE_W - 8, HEAD_H, &sNormal, 'l', STYLE_INK, 7);
    oy = Plate(PASS_CANCEL_X, HEAD_Y, PASS_CANCEL_W, HEAD_H, cancelOn ? sEmerald : sCancelGrey,
               cancelOn ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
    TextIn(cancel, PASS_CANCEL_X, oy, PASS_CANCEL_W, HEAD_H, &sNormal, 'c', STYLE_WHITE, 6);
}

/* The description along the bottom band: up to two lines, white. */
static void PassDescription(const u8 *text)
{
    if (text)
        TextLines(text, 10, PASS_BAND_Y + 3, PASS_W - 20, 16, 2, &sNormal, STYLE_WHITE, TRUE);
}

static const u8 *PassTitle(void)
{
    return Ascii(CTR_TEXT("FRONTIER PASS", "PASE DEL FRENTE", "PASSE ZONE COMBAT", "TESSERA LOTTA",
                          "PASSE DA BATALHA", "KAMPFZONENPASS", "フロンティアパス"));
}

static const u8 *PassCancel(void)
{
    return Ascii(CTR_TEXT("CANCEL", "SALIR", "RETOUR", "ESCI", "SAIR", "ZURÜCK", "やめる"));
}

/* The trainer card as the bottom screen's card reads it (SnapshotCard,
 * SnapshotCardBack), and what only the game's own card has. */
typedef struct
{
    u8 name[PLAYER_NAME_LENGTH + 1];
    u8 gender, hasDex, stars, badges, minutes, hofMinutes;
    u16 id, dex, hours, hofHours, linkWins, linkLosses, trades, pokeblocks, contests;
    u32 money;
    struct CtrCardTop top;
} PassCard;

typedef struct
{
    struct CtrPassView v;
    PassSprite symbols[CTR_PASS_FACILITIES], indicator, head;
    /* For a symbol not earned: its shape, from the sheet, drawn dim. */
    PassSprite empty[CTR_PASS_FACILITIES];
    s16 indicatorX, indicatorY, headX, headY;
    PassCard card;
    /* The cursor, last: everything before it is the base. */
    s16 handX, handY;
} PassScene;

/* A piece of the canvas as a picture PassBlit takes (BGR555, opaque). */
static void PassGrabRect(int x, int y, int w, int h, u16 *out)
{
    for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx)
        {
            u16 c = sPassCanvas[(x + dx) * PASS_H + (PASS_H - 1 - (y + dy))];

            out[dy * w + dx] = PASS_OPAQUE | (c >> 11) | (((c >> 6) & 31) << 5) | ((c & 31) << 10);
        }
}

/* The map's picture on its button, filling it: as wide as the button, its
 * top and bottom cut evenly to the button's height. */
static void PassMapButton(int x, int y)
{
    int h = PASS_ART_GH * PASS_MAP_PIC_W / PASS_ART_GW, saved[4];

    if (!sPassMapPic)
        return;
    /* Made smaller once a pass, then only copied: it is opaque. */
    if (sPassMapSmall)
    {
        for (int dx = 0; dx < PASS_MAP_PIC_W; ++dx)
            memcpy(sDst + (x + dx + sOX) * H + (H - y - PASS_MAP_PIC_H), sPassMapSmall + dx * PASS_MAP_PIC_H,
                   PASS_MAP_PIC_H * sizeof(u16));
        return;
    }
    ClipPush(saved, x, y, x + PASS_MAP_PIC_W, y + PASS_MAP_PIC_H);
    PassBlit(sPassMapPic, PASS_ART_GW, PASS_ART_GH, x, y + (PASS_MAP_PIC_H - h) / 2, PASS_MAP_PIC_W, h, FALSE);
    ClipPop(saved);
    sPassMapSmall = malloc(PASS_MAP_PIC_W * PASS_MAP_PIC_H * sizeof(u16));
    if (sPassMapSmall)
        for (int dx = 0; dx < PASS_MAP_PIC_W; ++dx)
            memcpy(sPassMapSmall + dx * PASS_MAP_PIC_H, sDst + (x + dx + sOX) * H + (H - y - PASS_MAP_PIC_H),
                   PASS_MAP_PIC_H * sizeof(u16));
}

/* The card, small: its green and cream, the name and the ID on the cream,
 * the trainer on the green. */
static void PassMiniCard(const PassCard *c, int x, int y, int w, int h)
{
    int oy = PlateEx(x, y, w, h, sCardGreen, PLATE_STATE_NORMAL, w - 58, 18, PlateCut(h), 0);
    int tw = w - 64;

    TextIn(c->name, x, oy + 2, tw, 20, &sNormal, 'l', STYLE_INK, 6);
    FillRgb(x + 6, oy + 24, tw - 6, 1, sRule);
    TextIn(Ascii(CTR_TEXT("ID No.", "Nº ID", "No. ID", "N. ID", "Nº ID", "ID-Nr.", "IDNo.")), x, oy + 26, tw, 18,
           &sSmall, 'l', STYLE_GRAY, 6);
    TextIn(Number(c->id, 5, STR_CONV_MODE_LEADING_ZEROS), x, oy + 42, tw, 20, &sNormal, 'r', STYLE_INK, 6);
    if (sRes.trainerPic[c->gender])
    {
        int saved[4];

        ClipPush(saved, x, y, x + w, y + h);
        DrawSprite(sRes.trainerPic[c->gender], 8, 8, x + w - 62, oy + (h - 64) / 2, sRes.trainerPicPal[c->gender].c);
        ClipPop(saved);
    }
}

/* The pass: its plates where the game's areas are, the art on them. It is
 * drawn in two goes (PassCompose): everything unlit once, then over a copy
 * of that the area the cursor is on lit, its description and the zoom. */
static void PassDrawSymbols(const PassScene *sc, bool8 on, int cell)
{
    const struct CtrPassView *v = &sc->v;
    int oy;

    /* A plate with its name in a band on top, the seven cells on its face
     * where the game has them, the ones not earned dim. */
    oy = PlateEx(PASS_LEFT_X, PASS_SYMBOLS_Y, PASS_LEFT_W, PASS_SYMBOLS_H, on ? sEmerald : sSlate,
                 on ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL, 0, 0, PlateCut(PASS_SYMBOLS_H), PASS_SYMBOLS_HEAD);
    TextIn(gText_SymbolsEarned, PASS_LEFT_X, oy, PASS_LEFT_W, PASS_SYMBOLS_HEAD, &sNormal, 'c', STYLE_WHITE, 6);
    for (int i = 0; i < CTR_PASS_FACILITIES; ++i)
    {
        int gx = sPassSymbolCells[i][0] + 8, gy = sPassSymbolCells[i][1] + 8;
        int cx = PassX(gx), cy = PassY(gx, gy) + (oy - PASS_SYMBOLS_Y);

        if (i == cell)
            Plate(cx - 15, cy - 15, 30, 30, sEmerald, PLATE_STATE_FOCUS);
        if (v->symbols[i] && sc->symbols[i].w)
            PassSpriteAt(&sc->symbols[i], cx - 12, cy - 12, 3, FALSE);
        else if (sc->empty[i].w)
            PassSpriteAt(&sc->empty[i], cx - 12, cy - 12, 3, FALSE);
    }
}

/* The record: a button, grey and never lit without one to watch. */
static void PassDrawRecord(const PassScene *sc, bool8 on)
{
    int oy;

    on = on && sc->v.hasRecord;
    oy = Plate(PASS_LEFT_X, PASS_RECORD_Y, PASS_LEFT_W, PASS_RECORD_H,
               !sc->v.hasRecord ? sOffGrey : on ? sEmerald : sSlate, on ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
    TextIn(gText_BattleRecord, PASS_LEFT_X, oy, PASS_LEFT_W, PASS_RECORD_H, &sNormal, 'c', STYLE_WHITE, 6);
}

/* The points: their name and the number, green, on cream. */
static void PassDrawPoints(const PassScene *sc, bool8 on)
{
    int oy = PlateEx(PASS_LEFT_X, PASS_POINTS_Y, PASS_LEFT_W, PASS_POINTS_H, on ? sEmerald : sLight,
                     on ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL, PASS_LEFT_W - 10, 30, PlateCut(PASS_POINTS_H), 0);

    TextIn(gText_BattlePoints, PASS_LEFT_X, oy, PASS_LEFT_W - 6, PASS_POINTS_H / 2, &sNormal, 'l', STYLE_INK, 8);
    TextIn(Number(sc->v.battlePoints, 5, STR_CONV_MODE_LEFT_ALIGN), PASS_LEFT_X, oy + PASS_POINTS_H / 2 - 2,
           PASS_LEFT_W - 6, PASS_POINTS_H / 2, &sNormal, 'r', STYLE_GREEN, 10);
}

/* The map (k 0) or the card (k 1) on its button: the map's own picture,
 * made smaller, or the card drawn small as the bottom screen draws it. */
static void PassDrawButton(const PassScene *sc, int k, bool8 on)
{
    int y = k ? PASS_CARD_Y : PASS_MAP_Y;
    int oy = Plate(PASS_RIGHT_X, y, PASS_RIGHT_W, PASS_THUMB_H, on ? sEmerald : sSlate,
                   on ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);

    if (k)
        PassMiniCard(&sc->card, PASS_RIGHT_X + 6, oy + 4, PASS_RIGHT_W - 12, PASS_THUMB_H - 10);
    else
        PassMapButton(PASS_RIGHT_X + (PASS_RIGHT_W - PASS_MAP_PIC_W) / 2, oy + (PASS_THUMB_H - 2 - PASS_MAP_PIC_H) / 2);
}

static void PassCancelPlate(bool8 on)
{
    int oy = Plate(PASS_CANCEL_X, HEAD_Y, PASS_CANCEL_W, HEAD_H, on ? sEmerald : sCancelGrey,
                   on ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);

    TextIn(PassCancel(), PASS_CANCEL_X, oy, PASS_CANCEL_W, HEAD_H, &sNormal, 'c', STYLE_WHITE, 6);
}

static void PassDrawMain(const PassScene *sc, bool8 lit)
{
    const struct CtrPassView *v = &sc->v;
    u8 area = v->cursorArea;

    if (!lit)
    {
        PassHeader(PassTitle(), PassCancel(), FALSE);
        PassDrawSymbols(sc, FALSE, -1);
        PassDrawRecord(sc, FALSE);
        PassDrawPoints(sc, FALSE);
        PassDrawButton(sc, 0, FALSE);
        PassDrawButton(sc, 1, FALSE);
        return;
    }
    if (area == CTR_PASS_AREA_CANCEL)
        PassCancelPlate(TRUE);
    else if (area == CTR_PASS_AREA_EARNED_SYMBOLS || area >= CTR_PASS_AREA_SYMBOL_TOWER)
        PassDrawSymbols(sc, TRUE, area >= CTR_PASS_AREA_SYMBOL_TOWER ? area - CTR_PASS_AREA_SYMBOL_TOWER : -1);
    else if (area == CTR_PASS_AREA_RECORD)
        PassDrawRecord(sc, TRUE);
    else if (area == CTR_PASS_AREA_POINTS)
        PassDrawPoints(sc, TRUE);
    else if (area == CTR_PASS_AREA_MAP || area == CTR_PASS_AREA_CARD)
        PassDrawButton(sc, area == CTR_PASS_AREA_CARD, TRUE);

    /* Into the map or the card: its button grows over the screen, as the
     * game's zooms, under the game's white fade. */
    if (v->zoomArea && v->zoomScale > Q_8_8(1))
    {
        int k = v->zoomArea == CTR_PASS_AREA_CARD, y = k ? PASS_CARD_Y : PASS_MAP_Y;
        int w = PASS_RIGHT_W * v->zoomScale / 256, h = PASS_THUMB_H * v->zoomScale / 256;
        int cx = PASS_RIGHT_X + PASS_RIGHT_W / 2, cy = y + PASS_THUMB_H / 2;

        PassGrabRect(PASS_RIGHT_X, y, PASS_RIGHT_W, PASS_THUMB_H, sPassGrab);
        PassBlitNearest(sPassGrab, PASS_RIGHT_W, PASS_THUMB_H, cx - w / 2, cy - h / 2, w, h);
    }
    PassDescription(v->description);
}

/* The Frontier map: its picture, the facilities as plates, the description.
 * Unlit: the header, the picture and every facility; lit, over that: the
 * chosen one, the marker and the player's head on the picture. */
static void PassDrawMap(const PassScene *sc, bool8 lit)
{
    const struct CtrPassView *v = &sc->v;
    /* The picture's top on its plate, as the unlit pass found it. */
    static int ay = PASS_ART_Y;
    int oy;

    if (!lit)
    {
        u8 name[32];

        GetMapName(name, MAPSEC_BATTLE_FRONTIER, 0);
        PassHeader(name, PassCancel(), FALSE);
        ay = Plate(PASS_ART_X - 6, PASS_ART_Y - 4, PASS_ART_GW * 3 / 2 + 12, PASS_ART_GH * 3 / 2 + 8, sSlate,
                   PLATE_STATE_NORMAL);
        if (sPassMapPic)
            PassBlit(sPassMapPic, PASS_ART_GW, PASS_ART_GH, PASS_ART_X, ay, PASS_ART_GW * 3 / 2, PASS_ART_GH * 3 / 2,
                     FALSE);
    }
    for (int i = 0; i < CTR_PASS_FACILITIES; ++i)
    {
        bool8 on = i == v->mapCursor;
        int y = PASS_NAMES_Y + i * PASS_NAME_STEP;

        if (lit != on)
            continue;
        oy = Plate(PASS_NAMES_X, y, PASS_NAMES_W, PASS_NAME_H, on ? sEmerald : sSlate,
                   on ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
        if (v->names[i])
            TextIn(v->names[i], PASS_NAMES_X, oy, PASS_NAMES_W, PASS_NAME_H, &sNormal, 'l', STYLE_WHITE, 10);
    }
    if (lit && sPassMapPic)
    {
        int saved[4];

        /* The facility's marker and the player's head on it, clipped to it. */
        ClipPush(saved, PASS_ART_X, ay, PASS_ART_X + PASS_ART_GW * 3 / 2, ay + PASS_ART_GH * 3 / 2);
        if (sc->indicator.w)
            PassSpriteAt(&sc->indicator, PASS_ART_X + (sc->indicatorX - PASS_ART_GX) * 3 / 2,
                         ay + (sc->indicatorY - PASS_ART_GY) * 3 / 2, 3, FALSE);
        if (v->mapHead && sc->head.w)
            PassSpriteAt(&sc->head, PASS_ART_X + (sc->headX - PASS_ART_GX) * 3 / 2,
                         ay + (sc->headY - PASS_ART_GY) * 3 / 2, 3, FALSE);
        ClipPop(saved);
    }
    if (lit)
        PassDescription(v->mapDescription);
}

/* The trainer card over the whole screen: the bottom screen's card made
 * wider and taller, with every row the game's own card has; its badges in
 * the band below. A flip folds it towards its middle row as the game's. */
#define PASS_CARD_X 14
#define PASS_CARD_Y0 40
#define PASS_CARD_W 372
#define PASS_CARD_H 158
#define PASS_CARD_ROW 25

static void PassCardRow(int i, int oy, const u8 *label, const u8 *value)
{
    int ry = oy + 8 + i * PASS_CARD_ROW, w = PASS_CARD_W - 132;

    if (i)
        FillRgb(PASS_CARD_X + 10, ry - 2, w - 6, 1, sRule);
    StyledStr(FitFont(&sSmall, label, w / 2), label, PASS_CARD_X + 14, TextY(ry, 22, &sSmall), STYLE_GRAY);
    StyledRight(FitFont(&sNormal, value, w / 2), value, PASS_CARD_X + w, TextY(ry, 22, &sNormal), STYLE_INK);
}

/* The card's own rows squeezed to its middle by `fold` (256: on edge). */
static void PassFold(int x, int y, int w, int h, u16 fold)
{
    int mid = y + h / 2, half = h / 2 * (256 - fold) / 256;
    u16 column[PASS_H];

    for (int cx = x; cx < x + w; ++cx)
    {
        u16 *col = sPassCanvas + cx * PASS_H;

        for (int cy = y; cy < y + h; ++cy)
            column[cy] = col[PASS_H - 1 - cy];
        for (int cy = y; cy < y + h; ++cy)
        {
            int d = cy - mid;

            if (half > 0 && d >= -half && d < half)
                col[PASS_H - 1 - cy] = column[mid + d * (h / 2) / half];
            else
                col[PASS_H - 1 - cy] = sPassBackdrop ? sPassBackdrop[cx * PASS_H + (PASS_H - 1 - cy)]
                                                     : PackRgb(146, 212, 172);
        }
    }
}

static void PassDrawCard(const PassScene *sc)
{
    const PassCard *c = &sc->card;
    u8 text[32];
    int oy, row = 0;

    PassHeader(c->top.onBack && c->top.backTitle ? c->top.backTitle : SectionName(SCR_CARD), PassCancel(), FALSE);
    oy = PlateEx(PASS_CARD_X, PASS_CARD_Y0, PASS_CARD_W, PASS_CARD_H, sCardGreen, PLATE_STATE_NORMAL,
                 PASS_CARD_W - 118, 25, PlateCut(PASS_CARD_H), 0);
    if (!c->top.onBack)
    {
        PassCardRow(row++, oy, Ascii(CTR_TEXT("NAME", "NOMBRE", "NOM", "NOME", "NOME", "NAME", "なまえ")), c->name);
        PassCardRow(row++, oy, Ascii(CTR_TEXT("ID No.", "Nº ID", "No. ID", "N. ID", "Nº ID", "ID-Nr.", "IDNo.")),
                    Number(c->id, 5, STR_CONV_MODE_LEADING_ZEROS));
        text[0] = CHAR_CURRENCY;
        StringCopy(text + 1, Number(c->money, 6, STR_CONV_MODE_LEFT_ALIGN));
        PassCardRow(row++, oy, Ascii(CTR_TEXT("MONEY", "DINERO", "ARGENT", "SOLDI", "DINHEIRO", "GELD", "おこづかい")), text);
        if (c->hasDex)
            PassCardRow(row++, oy, gText_MenuPokedex, Number(c->dex, 3, STR_CONV_MODE_LEFT_ALIGN));
        PassCardRow(row++, oy, Ascii(CTR_TEXT("TIME", "TIEMPO", "DURÉE", "TEMPO", "TEMPO", "ZEIT", "プレイじかん")),
                    HoursMinutes(c->hours, c->minutes));
    }
    else
    {
        PassCardRow(row++, oy, gText_HallOfFameDebut,
                    c->hofHours || c->hofMinutes ? HoursMinutes(c->hofHours, c->hofMinutes) : Ascii("---"));
        StringCopy(StringCopy(StringCopy(text, Number(c->linkWins, 4, STR_CONV_MODE_LEFT_ALIGN)), Ascii("-")),
                   Number(c->linkLosses, 4, STR_CONV_MODE_LEFT_ALIGN));
        PassCardRow(row++, oy, gText_LinkBattles, text);
        PassCardRow(row++, oy, gText_PokemonTrades, Number(c->trades, 5, STR_CONV_MODE_LEFT_ALIGN));
        PassCardRow(row++, oy, gText_PokeblocksWithFriends, Number(c->pokeblocks, 5, STR_CONV_MODE_LEFT_ALIGN));
        PassCardRow(row++, oy, gText_WonContestsWFriends, Number(c->contests, 3, STR_CONV_MODE_LEFT_ALIGN));
        if (c->top.frontierBP)
            PassCardRow(row++, oy, gText_BattlePtsWon, Number(c->top.frontierBP, 5, STR_CONV_MODE_LEFT_ALIGN));
    }
    /* The trainer, doubled, on the green; a star a feat above. */
    if (sRes.trainerPic[c->gender])
        DrawSpriteDouble(sRes.trainerPic[c->gender], 8, 8, PASS_CARD_X + PASS_CARD_W - 116, oy + 22,
                         sRes.trainerPicPal[c->gender].c);
    for (int k = 0; k < c->stars && k < 4; ++k)
        IconCentred(sArtStar, 2, 2 * (PASS_CARD_X + PASS_CARD_W - 100 + k * 18) + 11, 2 * (oy + 8) + 11, sArtPal2.c,
                    FALSE);
    if (c->top.fold)
        PassFold(PASS_CARD_X - 4, PASS_CARD_Y0 - 4, PASS_CARD_W + 8, PASS_CARD_H + 8, c->top.fold);

    /* The badges in the band, the ones not won as dim shapes. */
    oy = Plate(PASS_CARD_X, PASS_BAND_Y + 4, PASS_CARD_W, PASS_H - PASS_BAND_Y - 8, sLight, PLATE_STATE_NORMAL);
    if (sRes.badgeTiles)
    {
        u16 dim[16];

        for (int i = 0; i < 16; ++i)
            dim[i] = i ? PackRgb(176, 196, 184) : 0;
        for (int i = 0; i < NUM_BADGES; ++i)
        {
            const u16 *pal = (c->badges & (1 << i)) ? sRes.badgePal.c : dim;
            int bx = PASS_CARD_X + 26 + i * 44, by = oy + (PASS_H - PASS_BAND_Y - 8 - 16) / 2 - 1;

            DrawTile(sRes.badgeTiles + (2 * i) * 32, bx, by, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 1) * 32, bx + 8, by, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 16) * 32, bx, by + 8, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 17) * 32, bx + 8, by + 8, pal, FALSE, FALSE);
        }
    }
}

/* Draws into the pass's canvas instead of the bottom screen's (and back). */
static void PassTarget(bool8 on, int saved[5])
{
    if (on)
    {
        saved[0] = sClipX0;
        saved[1] = sClipY0;
        saved[2] = sClipX1;
        saved[3] = sClipY1;
        saved[4] = sOX;
        sDst = sPassCanvas;
        sOX = 0;
        sClipX0 = sClipY0 = 0;
        sClipX1 = PASS_W;
        sClipY1 = PASS_H;
    }
    else
    {
        sClipX0 = saved[0];
        sClipY0 = saved[1];
        sClipX1 = saved[2];
        sClipY1 = saved[3];
        sOX = saved[4];
        sDst = sCanvas;
    }
}

/*
 * The picture is kept in three steps, each made again only when what it
 * shows changes: sPassStatic, everything unlit (the backdrop, the header,
 * every plate with its art); sPassBase, that with the cursor's area lit, its
 * description and a zoom over it; sPassCanvas, that with the hand. When only
 * the hand moves, the piece it left is put back from sPassBase and the hand
 * drawn again, and only those two pieces go to the texture.
 */
static void PassComposeStatic(const PassScene *sc)
{
    int saved[5];

    PassTarget(TRUE, saved);
    if (sPassBackdrop)
        memcpy(sPassCanvas, sPassBackdrop, sizeof(sPassCanvas));
    else
        FillRect(0, 0, PASS_W, PASS_H, PackRgb(146, 212, 172));
    if (sc->v.view == CTR_PASS_MAP)
        PassDrawMap(sc, FALSE);
    else if (sc->v.view == CTR_PASS_CARD)
        PassDrawCard(sc);
    else
        PassDrawMain(sc, FALSE);
    PassTarget(FALSE, saved);
    memcpy(sPassStatic, sPassCanvas, sizeof(sPassCanvas));
}

static void PassComposeLit(const PassScene *sc)
{
    int saved[5];

    memcpy(sPassCanvas, sPassStatic, sizeof(sPassCanvas));
    PassTarget(TRUE, saved);
    if (sc->v.view == CTR_PASS_MAP)
        PassDrawMap(sc, TRUE);
    else if (sc->v.view == CTR_PASS_MAIN)
        PassDrawMain(sc, TRUE);
    PassTarget(FALSE, saved);
    memcpy(sPassBase, sPassCanvas, sizeof(sPassCanvas));
}

/* Where the hand goes: the game's hand's tip, on this screen. */
static void PassHandAt(const PassScene *sc, int *x, int *y, bool8 *hflip)
{
    if (sc->v.view == CTR_PASS_MAP)
    {
        /* Beside the facility's plate, following the game's hand down the
         * list (it steps 16 a facility, here PASS_NAME_STEP). */
        *x = PASS_NAMES_X - 2;
        *y = PASS_NAMES_Y + PASS_NAME_H / 2 + (sc->handY - 8) * PASS_NAME_STEP / 16;
        *hflip = TRUE;
    }
    else
    {
        /* frontier_pass.c picks the area at x - 5, y + 5 of its sprite. */
        *x = PassX(sc->handX - 5);
        *y = PassY(sc->handX - 5, sc->handY + 5);
        *hflip = FALSE;
    }
}

/* The hand over sPassBase, all of it new (`whole`) or only where the hand
 * was and is now; that much is handed to the texture. */
static void PassComposeHand(const PassScene *sc, bool8 whole)
{
    int saved[5], r[4], x, y;
    bool8 hflip;

    r[0] = sPassHandRect[0];
    r[1] = sPassHandRect[1];
    r[2] = sPassHandRect[2];
    r[3] = sPassHandRect[3];
    if (whole)
        memcpy(sPassCanvas, sPassBase, sizeof(sPassCanvas));
    else
    {
        int x0 = r[0] < 0 ? 0 : r[0], x1 = r[2] > PASS_W ? PASS_W : r[2];
        int y0 = r[1] < 0 ? 0 : r[1], y1 = r[3] > PASS_H ? PASS_H : r[3];

        for (int cx = x0; cx < x1; ++cx)
            memcpy(sPassCanvas + cx * PASS_H + (PASS_H - y1), sPassBase + cx * PASS_H + (PASS_H - y1),
                   (y1 - y0) * sizeof(u16));
    }
    sPassHandRect[0] = sPassHandRect[2] = 0;
    if (sc->v.view != CTR_PASS_CARD)
    {
        PassTarget(TRUE, saved);
        PassHandAt(sc, &x, &y, &hflip);
        PassHand(x, y, hflip);
        PassTarget(FALSE, saved);
    }
    if (whole)
        CtrPassTop_Show(sPassCanvas);
    else
    {
        if (r[2] > r[0])
            CtrPassTop_ShowRect(sPassCanvas, r[0], r[1], r[2], r[3]);
        if (sPassHandRect[2] > sPassHandRect[0])
            CtrPassTop_ShowRect(sPassCanvas, sPassHandRect[0], sPassHandRect[1], sPassHandRect[2], sPassHandRect[3]);
    }
}

/* ------------------------------------------------------------------------ */
/* From the game                                                            */
/* ------------------------------------------------------------------------ */

/* The dim of a symbol not earned yet, as the trainer card dims its badges. */
#define PASS_DIM_BGR (22 | 24 << 5 | 23 << 10)

/* Sprite tiles as the game has them in OBJ VRAM (1D), in palette `pal` of
 * its unfaded sprite palettes, or all in `dim` when it is not 0. */
static u32 PassTakeTiles(PassSprite *out, u16 tile, u8 pal, u8 w, u8 h, u16 dim)
{
    const u8 *obj = (const u8 *)VRAM_ + 0x10000;
    const u16 *plt = gPlttBufferUnfaded + 0x100 + (pal & 15) * 16;
    u32 sum = 0;

    out->w = w;
    out->h = h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            unsigned t = (tile + (y / 8) * (w / 8) + x / 8) & 1023;
            u8 b = obj[t * 32 + (y & 7) * 4 + (x & 7) / 2], c = (x & 1) ? b >> 4 : b & 15;
            u16 p = !c ? 0 : (dim ? dim : plt[c] & 0x7FFF) | PASS_OPAQUE;

            out->px[y * w + x] = p;
            sum = sum * 31 + p;
        }
    return sum;
}

/* A sprite's picture as it shows now (its animation's frame, its palette). */
static u32 PassTakeSprite(PassSprite *out, const struct Sprite *s)
{
    static const u8 sizes[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    };
    u8 w, h;

    out->w = out->h = 0;
    if (s == NULL || s->oam.shape > 2)
        return 0;
    w = sizes[s->oam.shape][s->oam.size][0];
    h = sizes[s->oam.shape][s->oam.size][1];
    if (w > 32 || h > 32)
        return 0;
    return PassTakeTiles(out, s->oam.tileNum, s->oam.paletteNum, w, h, 0);
}

/*
 * The backdrop, romfs/bottom/pass_top.bin (scripts/gen_pass_top_art.py):
 * the bottom screen's (gen_menu_art.py) made 400 wide, in the same format.
 * Without it, a plain one in its colours.
 */
static u16 *PassLoadBackdrop(void)
{
    FILE *f = fopen("romfs:/bottom/pass_top.bin", "rb");
    u16 *dst = malloc(sizeof(sPassCanvas));
    bool8 ok = FALSE;

    if (!dst)
    {
        if (f)
            fclose(f);
        return NULL;
    }
    if (f)
    {
        u8 head[20];

        if (fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "EM3DMNU1", 8) == 0
         && (head[16] | (head[17] << 8)) == PASS_W && (head[18] | (head[19] << 8)) == PASS_H)
            ok = fread(dst, 1, sizeof(sPassCanvas), f) == sizeof(sPassCanvas);
        fclose(f);
    }
    if (!ok)
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: romfs/bottom/pass_top.bin missing or bad, plain backdrop");
        for (int x = 0; x < PASS_W; ++x)
            for (int y = 0; y < PASS_H; ++y)
            {
                int t = y * 255 / (PASS_H - 1);

                dst[x * PASS_H + (PASS_H - 1 - y)] = y < 11 || y >= PASS_BAND_Y ? PackRgb(18, 52, 40)
                    : PackRgb(168 - 50 * t / 255, 226 - 34 * t / 255, 190 - 42 * t / 255);
            }
    }
    return dst;
}

/* What was last composed, and that with what only the lit step and the
 * hand show taken out. */
static PassScene sPassScene, sPassShown, sPassKey, sPassShownKey;
static bool8 sPassUp;

/* What the unlit step shows: the scene without the cursor's area, the
 * description, the zoom, the map's choice and its markers, and the hand. */
static void PassStaticKey(const PassScene *sc, PassScene *key)
{
    memcpy(key, sc, sizeof(*key));
    key->v.cursorArea = 0;
    key->v.description = NULL;
    key->v.zoomArea = 0;
    key->v.zoomScale = 0;
    key->v.mapCursor = 0;
    key->v.mapDescription = NULL;
    memset(&key->indicator, 0, sizeof(key->indicator));
    memset(&key->head, 0, sizeof(key->head));
    key->indicatorX = key->indicatorY = key->headX = key->headY = 0;
    key->handX = key->handY = 0;
}

static void PassFreeAll(void)
{
    free(sPassStatic);
    free(sPassBase);
    free(sPassBackdrop);
    sPassStatic = sPassBase = sPassBackdrop = NULL;
}

/* The Frontier map's picture from the game's art for the map screen: the
 * part frontier_pass.c shows there, colour 0 its backdrop. */
static void PassDecodeMap(const struct CtrPassView *v)
{
    const u8 *tiles;
    const u16 *map, *pal;

    if (sPassMapPic || !v->mapTiles)
        return;
    tiles = Unlz(v->mapTiles, NULL);
    map = Unlz(v->mapTilemap, NULL);
    pal = Port_ResolveAssetPointer(v->mapPalette);
    sPassMapPic = (tiles && map && pal) ? malloc(PASS_ART_GW * PASS_ART_GH * sizeof(u16)) : NULL;
    if (sPassMapPic)
        for (int y = 0; y < PASS_ART_GH; ++y)
            for (int x = 0; x < PASS_ART_GW; ++x)
            {
                int gx = PASS_ART_GX + x, gy = PASS_ART_GY + y;
                u16 e = map[(gy / 8) * 32 + gx / 8];
                int px = (e & 0x400) ? 7 - gx % 8 : gx % 8, py = (e & 0x800) ? 7 - gy % 8 : gy % 8;
                u8 b = tiles[(e & 0x3FF) * 32 + py * 4 + px / 2];
                int i = px & 1 ? b >> 4 : b & 15;
                /* The pass's palettes are eight (frontier_pass.c, top). */
                int bank = (e >> 12) < 8 ? e >> 12 : 0;

                sPassMapPic[y * PASS_ART_GW + x] = PASS_OPAQUE | (pal[(i ? bank * 16 + i : 0)] & 0x7FFF);
            }
    free((void *)tiles);
    free((void *)map);
}

/* The card as the bottom screen's reads it, and the game card's own. */
static void PassSnapCard(PassCard *c, const struct CtrCardTop *top)
{
    static ViewState snap;

    memset(&snap, 0, sizeof(snap));
    SnapshotCard(&snap);
    SnapshotCardBack(&snap);
    memcpy(c->name, snap.name, sizeof(c->name));
    c->gender = gSaveBlock2Ptr->playerGender ? FEMALE : MALE;
    c->hasDex = snap.hasDex;
    c->stars = snap.stars;
    c->badges = snap.badges;
    c->minutes = snap.minutes;
    c->hofMinutes = snap.hofMinutes;
    c->id = snap.id;
    c->dex = snap.dex;
    c->hours = snap.hours;
    c->hofHours = snap.hofHours;
    c->linkWins = snap.linkWins;
    c->linkLosses = snap.linkLosses;
    c->trades = snap.trades;
    c->pokeblocks = snap.pokeblocks;
    c->contests = snap.contests;
    c->money = snap.money;
    if (top)
        c->top = *top;
}

/* Between the zoom into the card and the pass back again. */
static bool8 sPassCardFlow;

static void PassTopFrame(void)
{
    PassScene *sc = &sPassScene;
    struct CtrPassView v;
    struct CtrCardTop card;
    bool8 changed, lit, hand;

    memset(&v, 0, sizeof(v));
    if (!CtrPass_GetView(&v) && CtrTrainerCard_TopView(&card))
        v.view = CTR_PASS_CARD;
    if (v.view == CTR_PASS_NONE)
    {
        /* Between the pass and the card or the map it opens, the game has
         * neither up for a frame or two: keep the last picture, so it does
         * not blink to the game's screen in between. */
        if (sPassUp && CtrPass_Open())
        {
            CtrPassTop_SetFade(gPaletteFade.blendColor, sPassCardFlow ? 0 : gPaletteFade.y);
            return;
        }
        sPassCardFlow = FALSE;
        if (sPassUp)
        {
            CtrPassTop_Hide();
            PassFreeAll();
            sPassUp = FALSE;
        }
        /* Kept between the pass and the map or the card it opens. */
        if (!CtrPass_Open())
        {
            free(sPassMapPic);
            free(sPassMapSmall);
            sPassMapPic = sPassMapSmall = NULL;
        }
        return;
    }
    if (!sPassUp)
    {
        sPassBackdrop = PassLoadBackdrop();
        sPassStatic = malloc(sizeof(sPassCanvas));
        sPassBase = malloc(sizeof(sPassCanvas));
        if (!sPassBackdrop || !sPassStatic || !sPassBase)
        {
            PassFreeAll();
            return;
        }
    }
    memset(sc, 0, sizeof(*sc));
    memcpy(&sc->v, &v, sizeof(v));
    /* The card does not change while the pass is up: read once a view. */
    if (sPassUp && sPassShown.v.view == v.view)
        sc->card = sPassShown.card;
    else
        PassSnapCard(&sc->card, NULL);
    PassDecodeMap(&v);
    if (v.view == CTR_PASS_MAIN)
    {
        sc->handX = v.cursor->x;
        sc->handY = v.cursor->y;
        for (int i = 0; i < CTR_PASS_FACILITIES; ++i)
        {
            if (v.symbolSprites[i])
                PassTakeSprite(&sc->symbols[i], v.symbolSprites[i]);
            else if (v.medalTiles != 0xFFFF)
                PassTakeTiles(&sc->empty[i], v.medalTiles + i * 4, 0, 16, 16, PASS_DIM_BGR);
        }
    }
    else if (v.view == CTR_PASS_CARD)
        sc->card.top = card;
    else
    {
        sc->handX = v.mapHand->x;
        sc->handY = v.mapHand->y;
        PassTakeSprite(&sc->indicator, v.mapIndicator);
        sc->indicatorX = v.mapIndicator->x + v.mapIndicator->x2 + v.mapIndicator->centerToCornerVecX;
        sc->indicatorY = v.mapIndicator->y + v.mapIndicator->y2 + v.mapIndicator->centerToCornerVecY;
        if (v.mapHead)
        {
            PassTakeSprite(&sc->head, v.mapHead);
            sc->headX = v.mapHead->x + v.mapHead->x2 + v.mapHead->centerToCornerVecX;
            sc->headY = v.mapHead->y + v.mapHead->y2 + v.mapHead->centerToCornerVecY;
        }
    }
    /* The sprites' own pointers say nothing their pictures do not. */
    sc->v.cursor = sc->v.mapHand = NULL;
    sc->v.mapIndicator = sc->v.mapHead = NULL;
    memset(sc->v.symbolSprites, 0, sizeof(sc->v.symbolSprites));

    /* The pictures taken from the game are part of the scene, so a tile or
     * a palette reaching VRAM a frame late is a change like any other. */
    PassStaticKey(sc, &sPassKey);
    changed = !sPassUp || memcmp(&sPassKey, &sPassShownKey, sizeof(sPassKey)) != 0;
    lit = changed || memcmp(sc, &sPassShown, offsetof(PassScene, handX)) != 0;
    hand = lit || sc->handX != sPassShown.handX || sc->handY != sPassShown.handY;
    if (changed)
    {
        PassComposeStatic(sc);
        memcpy(&sPassShownKey, &sPassKey, sizeof(sPassKey));
    }
    if (lit)
        PassComposeLit(sc);
    if (hand)
    {
        PassComposeHand(sc, lit);
        memcpy(&sPassShown, sc, sizeof(*sc));
    }
    /* The game fades to white into the card and back out of it, on top of
     * the zoom that already takes the button to the card: on the top screen
     * the zoom and the card meet without it. */
    if (v.view == CTR_PASS_CARD || (v.view == CTR_PASS_MAIN && v.zoomArea == CTR_PASS_AREA_CARD))
        sPassCardFlow = TRUE;
    else if (v.view != CTR_PASS_MAIN || (!v.zoomArea && !gPaletteFade.active))
        sPassCardFlow = FALSE;
    CtrPassTop_SetFade(gPaletteFade.blendColor, sPassCardFlow ? 0 : gPaletteFade.y);
    sPassUp = TRUE;
}
