/* Part of 3ds_bottom_ui.c: the contest's move choice (included after 3ds_bottom_blender.inc). */

/*
 * While the contest waits for the move to appeal with (CtrContestView): the
 * four moves as plates - the game's icon of their category, the appeal
 * hearts and the jam hearts, the COMBO or repeated mark - the effect of the
 * one chosen, and CANCEL / USE. The stage stays on the top screen. A tap on
 * a move chooses it, USE confirms (A), CANCEL goes back (B).
 */
#define CONTEST_HEAD_Y 16
#define CONTEST_HEAD_H 34
#define CONTEST_MOVE_Y 56
#define CONTEST_MOVE_W 150
#define CONTEST_MOVE_H 44
#define CONTEST_MOVE_STEP_Y 50
#define CONTEST_DESC_Y 158
#define CONTEST_DESC_H 34
#define CONTEST_BTN_Y 198
#define CONTEST_BTN_H 24
/* On a plate: the name starts 12 px in, a chip ends 8 px from its right side,
 * and the hearts - 8 slots of 9 px, so 70 px - sit right of the 32 px icon
 * (x 14 to 46), the appeal row under the chip and the jam row below it. */
#define CONTEST_HEARTS_X 52
#define CONTEST_APPEAL_Y 20
#define CONTEST_JAM_Y 29

/* A string without the plates' light shadow, on a chip. */
static void FlatStr(const u8 *str, int x, int y, const Font *font, u16 colour)
{
    if (font->glyphs && str)
        StyledPass(font, str, x, y, colour, 0, 1);
}

/* A small chip: its fill, a dark edge and the text over it. Returns its width. */
static int ContestChip(const u8 *str, int x, int y, u16 fill, u16 ink)
{
    int w = StrWidth(&sSmall, str) + 8, h = sSmall.height + 3;
    u16 edge = PackRgb(28, 28, 24);

    FillRect(x, y, w, h, edge);
    FillRect(x + 1, y + 1, w - 2, h - 2, fill);
    FlatStr(str, x + 4, y + 2, &sSmall, ink);
    return w;
}

/* A heart, 7 x 6. */
static void ContestHeart(int x, int y, u16 c)
{
    FillRect(x + 1, y, 2, 1, c);
    FillRect(x + 4, y, 2, 1, c);
    FillRect(x, y + 1, 7, 2, c);
    FillRect(x + 1, y + 3, 5, 1, c);
    FillRect(x + 2, y + 4, 3, 1, c);
    FillRect(x + 3, y + 5, 1, 1, c);
}

static void ContestHearts(int x, int y, int full, int slots, u16 on, u16 off)
{
    if (slots > CTR_CONTEST_HEARTS)
        slots = CTR_CONTEST_HEARTS;
    for (int i = 0; i < slots; ++i)
        ContestHeart(x + i * 9, y, i < full ? on : off);
}

static void DrawContestMove(const ViewState *s, int i)
{
    const struct CtrContestView *v = &s->co;
    int x = 8 + (i & 1) * 154, y = CONTEST_MOVE_Y + (i >> 1) * CONTEST_MOVE_STEP_Y, oy, tx, nameW;
    Rgb type = sContestColour[v->category[i] % CONTEST_CATEGORIES_COUNT];
    u8 state = s->pressed == HIT_MOVE + i ? PLATE_STATE_PRESSED : v->selected == i ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL;
    u16 ink = PackRgb(28, 28, 24);

    oy = Plate(x, y, CONTEST_MOVE_W, CONTEST_MOVE_H, sCreamPlate, state);
    FillRect(x + 3, oy + 3, 7, CONTEST_MOVE_H - 10, PackRgb(type.r, type.g, type.b));
    tx = x + 14;
    nameW = CONTEST_MOVE_W - 18;
    if (v->flags[i])
    {
        bool8 combo = (v->flags[i] & CTR_CONTEST_COMBO) != 0;
        const u8 *word = combo ? Ascii("COMBO")
                               : Ascii(CTR_TEXT("REPEAT", "REPET.", "RÉPÉTÉ", "RIPETUTA", "REPET.", "WIEDERH.", "くりかえし"));
        int chipX = x + CONTEST_MOVE_W - 8 - (StrWidth(&sSmall, word) + 8);

        /* The chip is as wide as its word; the name has what is left of the plate. */
        ContestChip(word, chipX, oy + 3, PackRgb(255, 255, 255), combo ? ink : PackRgb(90, 90, 100));
        nameW = chipX - tx;
    }
    TextIn(gMoveNames[v->moves[i]], tx - 2, oy + 1, nameW, 14, &sNormal, 'l', STYLE_INK, 2);
    /* The game's own contest category icon (the one the summary's CONTEST page draws). */
    if (sRes.typeTiles)
    {
        u8 cat = v->category[i] % CONTEST_CATEGORIES_COUNT;

        DrawSprite(sRes.typeTiles + (NUMBER_OF_MON_TYPES + cat) * 8 * 32, 4, 2, tx, oy + 15,
                   sRes.typePal[sContestIconPal[cat]].c);
    }
    ContestHearts(x + CONTEST_HEARTS_X, oy + CONTEST_APPEAL_Y, v->hearts[i], CTR_CONTEST_HEARTS, PackRgb(232, 72, 72),
                  PackRgb(207, 196, 180));
    if (v->jam[i])
        ContestHearts(x + CONTEST_HEARTS_X, oy + CONTEST_JAM_Y, v->jam[i], v->jam[i], PackRgb(58, 58, 138),
                      PackRgb(207, 196, 180));
    AddHit(x, y, CONTEST_MOVE_W, CONTEST_MOVE_H, HIT_MOVE + i);
}

static void DrawContest(const ViewState *s)
{
    const struct CtrContestView *v = &s->co;
    int oy;
    u16 iconSpecies = GetIconSpecies(v->species, v->personality);
    static u8 label[64];

    NameBackdrop();
    /* The mon, and which appeal it is. */
    oy = Plate(8, CONTEST_HEAD_Y, 304, CONTEST_HEAD_H, sEmerald, PLATE_STATE_NORMAL);
    {
        const u8 *tiles = MonIcon(iconSpecies, FALSE);
        u8 pal = gMonIconPaletteIndices[iconSpecies];

        if (tiles && pal < ARRAY_COUNT(sRes.monIconPal))
            DrawSprite(tiles, 4, 4, 14, oy + 1, sRes.monIconPal[pal].c);
    }
    TextIn(v->nick, 48, oy + 2, 180, 14, &sNormal, 'l', STYLE_WHITE, 0);
    TextIn(Ascii(CTR_TEXT("Which move?", "¿Qué movimiento?", "Quelle capacité ?", "Quale mossa?", "Qual golpe?",
                          "Welche Attacke?", "どのわざにする?")),
           48, oy + 16, 190, 12, &sSmall, 'l', STYLE_SOFT, 0);
    ConvertIntToDecimalStringN(label, v->round, STR_CONV_MODE_LEFT_ALIGN, 1);
    StringAppend(label, Ascii("/"));
    ConvertIntToDecimalStringN(label + StringLength(label), v->rounds, STR_CONV_MODE_LEFT_ALIGN, 1);
    TextIn(label, 240, oy + 2, 64, 28, &sNormal, 'r', STYLE_WHITE, 0);

    for (int i = 0; i < v->count; ++i)
        DrawContestMove(s, i);

    /* What the chosen one does. */
    oy = Plate(8, CONTEST_DESC_Y, 304, CONTEST_DESC_H, sCreamPlate, PLATE_STATE_NORMAL);
    TextLines(v->effect, 18, oy + 4, 284, 12, 2, &sSmall, STYLE_INK, TRUE);

    oy = Plate(8, CONTEST_BTN_Y, 90, CONTEST_BTN_H, sCreamPlate,
               s->pressed == HIT_CANCEL ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
    TextIn(gText_Cancel2, 8, oy, 90, CONTEST_BTN_H, &sNormal, 'c', STYLE_INK, 4);
    AddHit(8, CONTEST_BTN_Y, 90, CONTEST_BTN_H, HIT_CANCEL);
    oy = Plate(104, CONTEST_BTN_Y, 208, CONTEST_BTN_H, sEmerald,
               s->pressed == HIT_OK ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
    StringCopy(label, Ascii(CTR_TEXT("USE ", "USAR ", "UTILISER ", "USA ", "USAR ", "EINSETZEN: ", "つかう: ")));
    StringAppend(label, gMoveNames[v->moves[v->selected]]);
    TextIn(label, 104, oy, 208, CONTEST_BTN_H, &sNormal, 'c', STYLE_WHITE, 4);
    AddHit(104, CONTEST_BTN_Y, 208, CONTEST_BTN_H, HIT_OK);
}

static void ContestTap(u8 id)
{
    if (id >= HIT_MOVE && id < HIT_MOVE + CTR_CONTEST_MOVES)
        CtrContest_Act(CTR_CONTEST_SELECT, id - HIT_MOVE);
    else if (id == HIT_OK)
        CtrContest_Act(CTR_CONTEST_CONFIRM, 0);
    else if (id == HIT_CANCEL)
        CtrContest_Act(CTR_CONTEST_CANCEL, 0);
}
