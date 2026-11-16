/* Part of 3ds_bottom_ui.c: the Rental Pokemon's panel on the top screen (included after 3ds_bottom_factory.inc). */

/*
 * While the Rental Pokemon screens are up the top screen shows the Pokemon
 * the cursor is on, its front picture at exactly twice its size, with what
 * the bottom screen's plates do not say: its level, ability, held item,
 * nature and moves; and the three of the team. The game's own picture up
 * there said nothing the bottom screen does not (its menu, its message, the
 * balls), so the panel covers it whole (3ds_top_panel.h).
 *
 * It is drawn with the bottom screen's own writers, into a 400x240 canvas of
 * the same layout (column by column, each from the bottom row up), on the
 * menu's backdrop made 400 wide (gen_menu_art.py --top), and handed over only
 * when what it shows changes.
 */
#include "3ds_top_panel.h"

static bool8 sIconsReady;

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

static u16 *sFacTop, *sFacTopBack;
static bool8 sFacTopBackTried;
/* What the panel handed over shows, to hand it over again only on a change. */
static struct
{
    u8 screen, focus, enemy, chosen, ready;
    u8 order[CTR_FAC_MONS];
    u16 species;
    u32 personality;
} sFacTopShown;

static void FacTopBackdrop(void)
{
    FILE *f;
    long size = 0;
    u8 *data = NULL;

    if (sFacTopBackTried)
        return;
    sFacTopBackTried = TRUE;
    sFacTopBack = malloc(sizeof(u16) * FTOP_W * FTOP_H);
    if (!sFacTopBack)
        return;
    f = fopen("romfs:/bottom/menu_top.bin", "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = size > 0 ? malloc(size) : NULL;
        if (data && fread(data, 1, size, f) != (size_t)size)
            size = 0;
        fclose(f);
    }
    if (data && size >= (long)(10 + 10 + sizeof(u16) * FTOP_W * FTOP_H) && memcmp(data, "EM3DMNU1", 8) == 0
     && (data[16] | (data[17] << 8)) == FTOP_W && (data[18] | (data[19] << 8)) == FTOP_H)
        memcpy(sFacTopBack, data + 20, sizeof(u16) * FTOP_W * FTOP_H);
    else
    {
        CtrLog_Write(CTR_LOG_ERROR, "bottom: romfs/bottom/menu_top.bin missing or bad, plain backdrop");
        for (int x = 0; x < FTOP_W; ++x)
            for (int y = 0; y < FTOP_H; ++y)
            {
                int t = y * 255 / (FTOP_H - 1);

                sFacTopBack[x * FTOP_H + (FTOP_H - 1 - y)] = PackRgb(168 - 50 * t / 255, 226 - 34 * t / 255, 190 - 42 * t / 255);
            }
    }
    free(data);
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

/* The panel's picture, into sFacTop. FALSE while the front picture is not in yet. */
static bool8 DrawFactoryTop(const ViewState *s)
{
    const struct CtrFactoryView *v = &s->fac;
    u16 species = v->species[v->focus];
    int oy, slot = FrontPic(species, FALSE);
    u8 label[16];

    memcpy(sDst, sFacTopBack, sizeof(u16) * FTOP_W * FTOP_H);
    /* The title bar, and how many are chosen: in a dark pill in the middle of its height. */
    oy = Plate(FTOP_LEFT_X, FTOP_BAR_Y, FTOP_W - 2 * FTOP_LEFT_X, FTOP_BAR_H, sEmerald, PLATE_STATE_NORMAL);
    TextIn(gText_RentalPkmn2, FTOP_LEFT_X + 8, oy, 200, 20, &sNormal, 'l', STYLE_WHITE, 0);
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
    return slot >= 0;
}

/* Every frame: the panel drawn again when what it shows changed, and kept up. */
static void FactoryTopFrame(const ViewState *s)
{
    const struct CtrFactoryView *v = &s->fac;
    u16 *dst = sDst;
    int ox = sOX, cx0 = sClipX0, cy0 = sClipY0, cx1 = sClipX1, cy1 = sClipY1;
    bool8 changed;

    if (s->mode != MODE_PAD || !v->screen || v->focus >= v->count || !v->species[v->focus])
    {
        if (sFacTop)
        {
            free(sFacTop);
            sFacTop = NULL;
            sFacTopShown.screen = 0;
        }
        return;
    }
    FacTopBackdrop();
    if (!sFacTopBack)
        return;
    if (!sFacTop && !(sFacTop = malloc(sizeof(u16) * FTOP_W * FTOP_H)))
        return;
    changed = !sFacTopShown.ready || sFacTopShown.screen != v->screen || sFacTopShown.focus != v->focus
           || sFacTopShown.enemy != v->enemy || sFacTopShown.chosen != v->chosen
           || sFacTopShown.species != v->species[v->focus] || sFacTopShown.personality != v->personality[v->focus]
           || memcmp(sFacTopShown.order, v->order, sizeof(v->order)) != 0;
    if (changed)
    {
        sDst = sFacTop;
        sOX = 0;
        sClipX0 = sClipY0 = 0;
        sClipX1 = FTOP_W;
        sClipY1 = FTOP_H;
        sFacTopShown.ready = DrawFactoryTop(s) && sIconsReady;
        sDst = dst;
        sOX = ox;
        sClipX0 = cx0;
        sClipY0 = cy0;
        sClipX1 = cx1;
        sClipY1 = cy1;
        sFacTopShown.screen = v->screen;
        sFacTopShown.focus = v->focus;
        sFacTopShown.enemy = v->enemy;
        sFacTopShown.chosen = v->chosen;
        sFacTopShown.species = v->species[v->focus];
        sFacTopShown.personality = v->personality[v->focus];
        memcpy(sFacTopShown.order, v->order, sizeof(v->order));
        CtrTopPanel_Show(sFacTop);
        CtrLog_Write(CTR_LOG_VIDEO, "factory top: panel drawn (screen %u, focus %u, ready %u)", v->screen, v->focus,
                     sFacTopShown.ready);
    }
    CtrTopPanel_Keep();
}
