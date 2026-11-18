/* Part of 3ds_bottom_ui.c: the Rental Pokemon's panel on the top screen (included after 3ds_bottom_factory.inc). */

/*
 * While the Rental Pokemon screens are up the top screen shows the Pokemon
 * the cursor is on, its front picture at exactly twice its size, with what
 * the bottom screen's plates do not say: its level, ability, held item,
 * nature and moves; and the three of the team. The game's own picture up
 * there said nothing the bottom screen does not (its menu, its message, the
 * balls), so the panel covers it whole (3ds_top_panel.h), from the screen's
 * first frame to its last: the backdrop alone until the card can be drawn
 * whole, then the card, drawn again only when what it shows changes.
 *
 * It is drawn with the bottom screen's own writers, straight into the panel's
 * 400x240 canvas of the same layout (column by column, each from the bottom
 * row up), on the menu's backdrop made 400 wide (gen_menu_art.py --top).
 */
#include "3ds_top_panel.h"

#define FTOP_W CTR_TOP_PANEL_W
#define FTOP_H CTR_TOP_PANEL_H
#define FTOP_LEFT_X 6
#define FTOP_LEFT_W 152
#define FTOP_RIGHT_X 166
#define FTOP_RIGHT_W 228
#define FTOP_BAR_Y 5
#define FTOP_BAR_H 24
#define FTOP_PILL_W 40
#define FTOP_PILL_H 16
#define FTOP_TEAM_Y 190
#define FTOP_TEAM_H 44
#define FTOP_SLOT 36
/* Frames a front picture may take to come in before the card is drawn without it. */
#define FTOP_WAIT_FRAMES 90

/* The backdrop, held while the Rental screens are up. */
static u16 *sFacTopBack;
/* What the panel was handed: nothing, the backdrop alone, a card. */
enum { FTOP_NONE, FTOP_BACKDROP, FTOP_CARD };
static u8 sFacTopKind;
/* Frames the focus' picture has been waited for, and which one it is. */
static u16 sFacTopWait;
static u32 sFacTopWaitKey;
/* Everything the card draws, to draw it again only on a change. */
static struct FacTopSig
{
    u8 screen, focus, enemy, chosen, shiny, sprite;
    u8 level, ability, nature;
    u16 item, moves[MAX_MON_MOVES];
    u16 species[CTR_FAC_MONS];
    u32 personality[CTR_FAC_MONS];
    u8 order[CTR_FAC_MONS];
} sFacTopShown;

/* The plain backdrop: the menu's green, light at the top. */
static void FacTopPlain(u16 *dst)
{
    for (int y = 0; y < FTOP_H; ++y)
    {
        int t = y * 255 / (FTOP_H - 1);
        u16 c = PackRgb(168 - 50 * t / 255, 226 - 34 * t / 255, 190 - 42 * t / 255);

        for (int x = 0; x < FTOP_W; ++x)
            dst[x * FTOP_H + (FTOP_H - 1 - y)] = c;
    }
}

/*
 * The backdrop read from RomFS into its own buffer: once, when the screen's
 * first frame needs it or ahead of it (the idle warm-up). Never inside a
 * redraw that has it already. If the file is missing the buffer is the plain
 * one; if there is no memory for the buffer nothing is kept and the panel
 * paints the plain one straight, trying again at the next call.
 */
static void FactoryTopWarm(void)
{
    FILE *f;
    u8 head[20];
    bool8 ok = FALSE;

    if (sFacTopBack)
        return;
    sFacTopBack = malloc(sizeof(u16) * FTOP_W * FTOP_H);
    if (!sFacTopBack)
        return;
    f = fopen("romfs:/bottom/menu_top.bin", "rb");
    if (f)
    {
        ok = fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "EM3DMNU1", 8) == 0
          && (head[16] | (head[17] << 8)) == FTOP_W && (head[18] | (head[19] << 8)) == FTOP_H
          && fread(sFacTopBack, 1, sizeof(u16) * FTOP_W * FTOP_H, f) == sizeof(u16) * FTOP_W * FTOP_H;
        fclose(f);
    }
    if (!ok)
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: romfs/bottom/menu_top.bin missing or bad, plain backdrop");
        FacTopPlain(sFacTopBack);
    }
}

/* The backdrop under everything, into the canvas. */
static void FacTopPaint(u16 *dst)
{
    if (sFacTopBack)
        memcpy(dst, sFacTopBack, sizeof(u16) * FTOP_W * FTOP_H);
    else
        FacTopPlain(dst);
}

/* The panel is no longer asked for: the backdrop goes with it. */
static void FactoryTopLeave(void)
{
    free(sFacTopBack);
    sFacTopBack = NULL;
    sFacTopKind = FTOP_NONE;
    sFacTopWait = 0;
}

/* A 4bpp sprite with every pixel an exact 2x2 block. */
static void DrawSprite2x(const u8 *tiles, int wt, int ht, int x, int y, const u16 *pal)
{
    for (int ty = 0; ty < ht; ++ty)
        for (int tx = 0; tx < wt; ++tx)
        {
            const u8 *tile = tiles + (ty * wt + tx) * 32;

            for (int py = 0; py < 8; ++py)
                for (int px = 0; px < 8; ++px)
                {
                    u8 v = (tile[py * 4 + px / 2] >> ((px & 1) * 4)) & 15;

                    if (v)
                        FillRect(x + (tx * 8 + px) * 2, y + (ty * 8 + py) * 2, 2, 2, pal[v]);
                }
        }
}

/* A box with its corners rounded by r pixels. */
static void FacTopRound(int x, int y, int w, int h, int r, u16 c)
{
    for (int row = 0; row < h; ++row)
    {
        int d = row < r ? r - row : row >= h - r ? row - (h - r) + 1 : 0;
        int cut = d ? d - (d > 1) : 0;

        FillRect(x + cut, y + row, w - 2 * cut, 1, c);
    }
}

/* A ring (the summary's faint ball) and an ellipse (the ground), row by row. */
static void FacTopRing(int cx, int cy, int r, int thick, u16 c)
{
    for (int dy = -r; dy <= r; ++dy)
    {
        int outer = 0, inner = -1, ri = r - thick;

        while ((outer + 1) * (outer + 1) + dy * dy <= r * r)
            ++outer;
        if (dy * dy <= ri * ri)
        {
            inner = 0;
            while ((inner + 1) * (inner + 1) + dy * dy <= ri * ri)
                ++inner;
        }
        if (inner < 0)
            FillRect(cx - outer, cy + dy, 2 * outer + 1, 1, c);
        else
        {
            FillRect(cx - outer, cy + dy, outer - inner, 1, c);
            FillRect(cx + inner + 1, cy + dy, outer - inner, 1, c);
        }
    }
}

static void FacTopEllipse(int cx, int cy, int rx, int ry, u16 c)
{
    /* Each row as wide as the ellipse reaches across it, its edge at the
     * pixels' middle: no single-pixel tips at the top and bottom. */
    for (int dy = -ry; dy <= ry; ++dy)
    {
        float t = dy / (ry + 0.5f);
        int half = (int)((rx + 0.5f) * sqrtf(1.0f - t * t));

        if (half >= 0)
            FillRect(cx - half, cy + dy, 2 * half + 1, 1, c);
    }
}

static void FacTopRow(const u8 *label, const u8 *value, int y)
{
    TextIn(label, FTOP_RIGHT_X + 10, y, 70, 14, &sSmall, 'l', STYLE_GRAY, 0);
    TextIn(value, FTOP_RIGHT_X + 84, y, FTOP_RIGHT_W - 94, 14, &sSmall, 'l', STYLE_INK, 0);
}

/* A front picture is in the cache (FrontPic loads them, one a frame, in Prefetch); never loads one itself. */
enum { FRONT_PENDING, FRONT_READY, FRONT_FAILED };

static int FacFrontState(u16 species, bool8 shiny, int *slot)
{
    for (int i = 0; i < FRONT_SLOTS; ++i)
        if (sFronts[i].loaded && sFronts[i].species == species && sFronts[i].shiny == shiny)
        {
            *slot = i;
            return sFronts[i].valid ? FRONT_READY : FRONT_FAILED;
        }
    return FRONT_PENDING;
}

/* The panel's picture, into the canvas now in sDst; slot is the front picture's, -1 for none. */
static void DrawFactoryTop(const ViewState *s, int slot)
{
    const struct CtrFactoryView *v = &s->fac;
    u16 species = v->species[v->focus];
    int oy;
    u8 label[16];

    FacTopPaint(sDst);
    /* The title bar, and how many are chosen: in a dark pill in the middle of its height. */
    oy = Plate(FTOP_LEFT_X, FTOP_BAR_Y, FTOP_W - 2 * FTOP_LEFT_X, FTOP_BAR_H, sEmerald, PLATE_STATE_NORMAL);
    TextIn(v->screen == CTR_FAC_SELECT ? gText_RentalPkmn2 : gText_PkmnSwap, FTOP_LEFT_X + 8, oy, 200, 20, &sNormal, 'l',
           STYLE_WHITE, 0);
    if (v->screen == CTR_FAC_SELECT)
    {
        int px = FTOP_W - FTOP_LEFT_X - 8 - FTOP_PILL_W, py = FTOP_BAR_Y + (FTOP_BAR_H - FTOP_PILL_H) / 2;

        ConvertIntToDecimalStringN(label, v->chosen, STR_CONV_MODE_LEFT_ALIGN, 1);
        StringAppend(label, Ascii("/3"));
        FacTopRound(px, py, FTOP_PILL_W, FTOP_PILL_H, 3, PackRgb(40, 94, 70));
        TextIn(label, px, py, FTOP_PILL_W, FTOP_PILL_H, &sSmall, 'c', STYLE_WHITE, 0);
    }
    /* The picture, twice its size, on the ball and its ground. */
    oy = Plate(FTOP_LEFT_X, 36, FTOP_LEFT_W, 148, sCreamPlate, PLATE_STATE_NORMAL);
    FacTopRing(FTOP_LEFT_X + FTOP_LEFT_W / 2, oy + 72, 64, 10, PackRgb(234, 222, 206));
    FacTopEllipse(FTOP_LEFT_X + FTOP_LEFT_W / 2, oy + 128, 52, 8, PackRgb(214, 200, 184));
    if (slot >= 0)
        DrawSprite2x(sFronts[slot].tiles, 8, 8, FTOP_LEFT_X + FTOP_LEFT_W / 2 - 64, oy + 6, sFronts[slot].pal.c);
    /* The three of the team: their icons in the order chosen, in three boxes
     * spaced evenly along the plate and in the middle of its height. */
    Plate(FTOP_LEFT_X, FTOP_TEAM_Y, FTOP_LEFT_W, FTOP_TEAM_H, sCreamPlate, PLATE_STATE_NORMAL);
    for (int k = 0; k < 3; ++k)
    {
        const int gap = (FTOP_LEFT_W - 3 * FTOP_SLOT) / 4;
        int x = FTOP_LEFT_X + gap + k * (FTOP_SLOT + gap), y = FTOP_TEAM_Y + (FTOP_TEAM_H - FTOP_SLOT) / 2 - 1;
        int mon = -1;

        if (v->screen == CTR_FAC_SELECT)
        {
            for (int i = 0; i < v->count; ++i)
                if (v->order[i] == k + 1)
                    mon = i;
        }
        else if (k < v->count)
            mon = k;
        if (mon < 0)
        {
            FacTopRound(x, y, FTOP_SLOT, FTOP_SLOT, 3, PackRgb(176, 164, 148));
            FacTopRound(x + 1, y + 1, FTOP_SLOT - 2, FTOP_SLOT - 2, 2, PackRgb(240, 232, 220));
            continue;
        }
        FacTopRound(x, y, FTOP_SLOT, FTOP_SLOT, 3, PackRgb(226, 214, 198));
        {
            u16 iconSpecies = GetIconSpecies(v->species[mon], v->personality[mon]);
            const u8 *tiles = MonIcon(iconSpecies, FALSE);
            u8 pal = gMonIconPaletteIndices[iconSpecies];

            if (tiles && pal < ARRAY_COUNT(sRes.monIconPal))
                DrawSprite(tiles, 4, 4, x + (FTOP_SLOT - 32) / 2, y + (FTOP_SLOT - 32) / 2, sRes.monIconPal[pal].c);
        }
        if (v->screen == CTR_FAC_SELECT)
        {
            /* The order, on a disc inside the box's top-right corner. */
            FacTopEllipse(x + FTOP_SLOT - 9, y + 9, 7, 7, PackRgb(70, 166, 106));
            TextIn(Number(k + 1, 1, STR_CONV_MODE_LEFT_ALIGN), x + FTOP_SLOT - 16, y + 2, 15, 15, &sSmall, 'c', STYLE_WHITE, 0);
        }
    }
    /* Its name, level and types. */
    oy = Plate(FTOP_RIGHT_X, 36, FTOP_RIGHT_W, 44, sCreamPlate, PLATE_STATE_NORMAL);
    TextIn(gSpeciesNames[species], FTOP_RIGHT_X + 10, oy + 1, 150, 16, &sNormal, 'l', STYLE_INK, 0);
    TextIn(LevelText(v->level), FTOP_RIGHT_X + FTOP_RIGHT_W - 70, oy + 2, 60, 14, &sSmall, 'r', STYLE_GRAY, 0);
    DrawTypeIcon(gSpeciesInfo[species].types[0], FTOP_RIGHT_X + 10, oy + 20);
    if (gSpeciesInfo[species].types[1] != gSpeciesInfo[species].types[0])
        DrawTypeIcon(gSpeciesInfo[species].types[1], FTOP_RIGHT_X + 48, oy + 20);
    /* Ability, held item, nature. */
    oy = Plate(FTOP_RIGHT_X, 86, FTOP_RIGHT_W, 52, sCreamPlate, PLATE_STATE_NORMAL);
    FacTopRow(Ascii(CTR_TEXT("ABILITY", "HABILIDAD", "TALENT", "ABILITÀ", "HABILIDADE", "FÄHIGKEIT", "とくせい")),
              gAbilityNames[v->ability], oy + 3);
    FacTopRow(Ascii(CTR_TEXT("ITEM", "OBJETO", "OBJET", "STRUMENTO", "ITEM", "ITEM", "もちもの")),
              v->item ? GetItemName(v->item)
                      : Ascii(CTR_TEXT("NONE", "NINGUNO", "AUCUN", "NESSUNO", "NENHUM", "KEINE", "なし")),
              oy + 17);
    FacTopRow(Ascii(CTR_TEXT("NATURE", "NATURALEZA", "NATURE", "NATURA", "NATUREZA", "WESEN", "せいかく")),
              gNatureNamePointers[v->nature], oy + 31);
    /* Its moves, each with its type. */
    oy = Plate(FTOP_RIGHT_X, 144, FTOP_RIGHT_W, 90, sCreamPlate, PLATE_STATE_NORMAL);
    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        u16 move = v->moves[i];

        if (move == MOVE_NONE)
            continue;
        DrawTypeIcon(gBattleMoves[move].type, FTOP_RIGHT_X + 8, oy + 3 + i * 21);
        TextIn(gMoveNames[move], FTOP_RIGHT_X + 46, oy + 2 + i * 21, FTOP_RIGHT_W - 54, 18, &sSmall, 'l', STYLE_INK, 0);
    }
}

static void FacTopSigOf(struct FacTopSig *sig, const struct CtrFactoryView *v, bool8 sprite)
{
    memset(sig, 0, sizeof(*sig));
    sig->screen = v->screen;
    sig->focus = v->focus;
    sig->enemy = v->enemy;
    sig->chosen = v->chosen;
    sig->shiny = v->shiny;
    sig->sprite = sprite;
    sig->level = v->level;
    sig->ability = v->ability;
    sig->nature = v->nature;
    sig->item = v->item;
    memcpy(sig->moves, v->moves, sizeof(sig->moves));
    memcpy(sig->species, v->species, sizeof(sig->species));
    memcpy(sig->personality, v->personality, sizeof(sig->personality));
    memcpy(sig->order, v->order, sizeof(sig->order));
}

/*
 * Every frame: the panel up from the screen's first frame, the backdrop alone
 * until there is a card (the view's Pokemon, every icon in and the focus' front
 * picture in, or given up on after FTOP_WAIT_FRAMES), then the card, handed
 * over only when what it shows changes. While a new picture is waited for the
 * card shown stays. Drawn into the panel's own canvas: nothing is shown until
 * it is whole. It follows the game's fade to black as the screen leaves.
 */
static void FactoryTopFrame(const ViewState *s)
{
    const struct CtrFactoryView *v = &s->fac;
    u16 *dst = sDst;
    int ox = sOX, cx0 = sClipX0, cy0 = sClipY0, cx1 = sClipX1, cy1 = sClipY1;
    int slot = -1, front = FRONT_FAILED;
    bool8 card;
    struct FacTopSig sig;

    if (s->mode != MODE_PAD || !v->screen)
    {
        FactoryTopLeave();
        return;
    }
    /* The panel went (this is not asked every frame): it is handed over again. */
    if (sFacTopKind != FTOP_NONE && !CtrTopPanel_Active())
        sFacTopKind = FTOP_NONE;
    card = v->count && v->loaded && v->focus < v->count && v->species[v->focus];
    if (card)
    {
        u16 pic = FacPicSpecies(v->species[v->focus], v->personality[v->focus]);
        u32 key = pic << 1 | v->shiny;

        front = FacFrontState(pic, v->shiny, &slot);
        if (key != sFacTopWaitKey)
        {
            sFacTopWaitKey = key;
            sFacTopWait = 0;
        }
        if (front == FRONT_PENDING && sFacTopWait < FTOP_WAIT_FRAMES)
        {
            ++sFacTopWait;
            card = FALSE;
        }
        if (front != FRONT_READY)
            slot = -1;
    }
    if (!card)
    {
        /* The card shown stays while the next one is waited for. */
        if (sFacTopKind == FTOP_NONE)
        {
            FactoryTopWarm();
            FacTopPaint(CtrTopPanel_Canvas());
            CtrTopPanel_Show();
            sFacTopKind = FTOP_BACKDROP;
        }
    }
    else
    {
        FacTopSigOf(&sig, v, slot >= 0);
        if (sFacTopKind != FTOP_CARD || memcmp(&sig, &sFacTopShown, sizeof(sig)) != 0)
        {
            FactoryTopWarm();
            sDst = CtrTopPanel_Canvas();
            sOX = 0;
            sClipX0 = sClipY0 = 0;
            sClipX1 = FTOP_W;
            sClipY1 = FTOP_H;
            DrawFactoryTop(s, slot);
            sDst = dst;
            sOX = ox;
            sClipX0 = cx0;
            sClipY0 = cy0;
            sClipX1 = cx1;
            sClipY1 = cy1;
            CtrTopPanel_Show();
            memcpy(&sFacTopShown, &sig, sizeof(sig));
            sFacTopKind = FTOP_CARD;
            CtrLog_Write(CTR_LOG_VIDEO, "factory top: card drawn (screen %u, focus %u, picture %d)", v->screen, v->focus, slot);
        }
    }
    CtrTopPanel_Keep(CtrFactory_Dark());
}
