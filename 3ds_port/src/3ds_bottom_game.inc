/*
 * The game's own screens drawn natively (included by 3ds_bottom_ui.c): the
 * party menu, the summary, the bag and the Pokédex as the approved v4.2
 * mockups lay them out, read from where the game's menu code is
 * (3ds_game_views.h). The game's code runs as always and its picture is not
 * composed meanwhile (CtrVideo_SetBottomNative); a tap here is the tap the
 * game's own takes on that thing (CtrMenu_PostTap, CtrMenu_PostEntry), so
 * what it does, its sounds included, is still the game's. A state with no
 * view here (state NONE) shows the game's picture in its window.
 */

static const Rgb sFieldMove = {64, 120, 200};

/* Ids of the native views' things: a box, an entry of the open menu, the
 * message going on. CANCEL, YES and NO are the shared ones. */
enum { HIT_NAT_SLOT = HIT_ROW, HIT_NAT_ENTRY = HIT_ROW + 8, HIT_NAT_MSG = HIT_ROW + 16 };

/* The game's HP bar: green over half, yellow over a fifth, red under. */
static void HpBar(int x, int y, int w, u16 hp, u16 maxHp)
{
    int fill = maxHp ? ((w - 2) * hp + maxHp / 2) / maxHp : 0;
    u16 colour, dark;

    if (hp * 2 > maxHp)
        colour = PackRgb(112, 248, 168), dark = PackRgb(88, 208, 128);
    else if (hp * 5 > maxHp)
        colour = PackRgb(248, 224, 0), dark = PackRgb(200, 168, 0);
    else
        colour = PackRgb(248, 88, 56), dark = PackRgb(200, 56, 40);
    FillRect(x, y, w, 5, PackRgb(40, 48, 56));
    FillRect(x + 1, y + 1, w - 2, 3, PackRgb(88, 96, 104));
    if (hp && fill < 1)
        fill = 1;
    if (fill)
    {
        FillRect(x + 1, y + 1, fill, 2, colour);
        FillRect(x + 1, y + 3, fill, 1, dark);
    }
}

static void GenderSign(int x, int y, u8 gender)
{
    static const u8 male[] = {CHAR_MALE, EOS}, female[] = {CHAR_FEMALE, EOS};

    if (gender == MON_MALE)
        StyledStr(&sSmall, male, x, y, STYLE_MALE);
    else if (gender == MON_FEMALE)
        StyledStr(&sSmall, female, x, y, STYLE_FEMALE);
}

static const u8 *LevelText(u8 level)
{
    static u8 text[16];
    u8 *end = StringCopy(text, Ascii(CTR_TEXT("LV", "NV", "N", "LV", "NV")));

    ConvertIntToDecimalStringN(end, level, STR_CONV_MODE_LEFT_ALIGN, 3);
    return text;
}

static const u8 *HpText(u16 hp, u16 maxHp)
{
    static u8 text[16];
    u8 *end = ConvertIntToDecimalStringN(text, hp, STR_CONV_MODE_LEFT_ALIGN, 3);

    *end++ = CHAR_SLASH;
    ConvertIntToDecimalStringN(end, maxHp, STR_CONV_MODE_LEFT_ALIGN, 3);
    return text;
}

/* ------------------------------------------------------------------------ */
/* POKéMON: the party menu                                                  */
/* ------------------------------------------------------------------------ */

/*
 * The lead (or, with a mon's menu open, that mon) on a big plate on the
 * left, the prompt under it and CANCEL; the other five on the right, or
 * the open menu's entries in their place. A message rises over the right
 * column's foot, with YES and NO over it when the game asks. The box the
 * game's cursor is on is emerald (chosen); while switching, the one being
 * moved is, and the cursor's glows.
 */
#define PARTY_LEFT_W 104
#define PARTY_RIGHT_X (AREA_X + PARTY_LEFT_W + 4)
#define PARTY_RIGHT_W (AREA_W - PARTY_LEFT_W - 4)
#define PARTY_ROW 33
#define PARTY_ROW_H 30
#define PARTY_LINE 15

/* A party menu's icon as the game's moves: its frame and bounce. Its first
 * frame while the second is on its way, and its place kept from frame to
 * frame (centred on the first's pixels). */
#define PARTY_ICON_R 20
static void PartyIconAt(const ViewState *s, int slot, int cx, int cy)
{
    const MonView *mon = &s->party[slot];
    const u8 *tiles = MonIconFrame(mon->iconSpecies, mon->deoxys, s->pv.iconFrame[slot]);
    const u8 *first = MonIcon(mon->iconSpecies, mon->deoxys);
    u8 pal = gMonIconPaletteIndices[mon->iconSpecies];
    int x0, y0, x1, y1;

    if (!tiles)
        tiles = first;
    if (!tiles || !first || pal >= ARRAY_COUNT(sRes.monIconPal))
        return;
    SpriteBox(first, 4, 4, &x0, &y0, &x1, &y1);
    if (x1 <= x0)
        return;
    {
        const u16 *colours = sRes.monIconPal[pal].c;
        u16 g[16];

        if (mon->fainted)
        {
            GreyPal(colours, g);
            colours = g;
        }
        DrawSprite(tiles, 4, 4, Half(2 * cx - x0 - x1), Half(2 * (cy + s->pv.iconLift[slot]) - y0 - y1), colours);
    }
}

/* Where the icon of a party menu's slot can draw, for a redraw of it alone. */
static bool8 PartyIconRect(const ViewState *s, int slot, int *x0, int *y0, int *x1, int *y1)
{
    const struct CtrPartyView *v = &s->pv;
    int lead = (v->state == CTR_PARTY_VIEW_MENU || v->state == CTR_PARTY_VIEW_STATS) && v->chosen >= 0
            && v->chosen < PARTY_SIZE ? v->chosen : 0;
    int cx, cy;

    if (slot == lead)
    {
        cx = AREA_X + 19;
        cy = AREA_Y + 20;
    }
    else if (v->state == CTR_PARTY_VIEW_LIST && slot > 0)
    {
        cx = PARTY_RIGHT_X + 18;
        cy = AREA_Y + (slot - 1) * PARTY_ROW + 15;
    }
    else
        return FALSE;
    *x0 = cx - PARTY_ICON_R;
    *x1 = cx + PARTY_ICON_R;
    *y0 = cy - PARTY_ICON_R;
    *y1 = cy + PARTY_ICON_R;
    return TRUE;
}

static Rgb PartyBoxColour(const ViewState *s, int slot)
{
    const struct CtrPartyView *v = &s->pv;

    if (v->state == CTR_PARTY_VIEW_MENU ? slot == v->chosen
                                        : v->moving >= 0 ? slot == v->moving : slot == v->cursor)
        return sEmerald;
    return s->party[slot].fainted ? sRedPlate : sBlue;
}

static u8 PartyBoxState(const ViewState *s, int slot)
{
    const struct CtrPartyView *v = &s->pv;

    if (s->pressed == HIT_NAT_SLOT + slot)
        return PLATE_STATE_PRESSED;
    if (v->state == CTR_PARTY_VIEW_LIST && v->moving >= 0 && slot == v->cursor && slot != v->moving)
        return PLATE_STATE_FOCUS;
    return PLATE_STATE_NORMAL;
}

/* Whether a box's description reads as a yes (ABLE, LEARNED) or a no. */
static bool8 DescIsYes(u8 desc)
{
    return desc != PARTYBOX_DESC_NO_USE && desc != PARTYBOX_DESC_NOT_ABLE && desc != PARTYBOX_DESC_NOT_ABLE_2
        && desc != PARTYBOX_DESC_DONT_HAVE;
}

/* What the mon holds, as the party menu shows it: the item or mail sign at
 * the foot of its icon, right of its centre, still while the icon hops. */
static void PartyHeldSign(const ViewState *s, int slot, int cx, int cy)
{
    u8 held = s->party[slot].held;

    if (held != HELD_NONE && sRes.holdTiles)
        DrawSprite(sRes.holdTiles + (held == HELD_MAIL ? 32 : 0), 1, 1, cx, cy + 6, sRes.holdPal.c);
}

static void DrawPartyLead(const ViewState *s, int slot)
{
    const MonView *mon = &s->party[slot];
    const struct CtrPartyView *v = &s->pv;
    int x = AREA_X, oy;
    bool8 tappable = v->state == CTR_PARTY_VIEW_LIST;

    oy = PlateEx(x, AREA_Y, PARTY_LEFT_W, 84, PartyBoxColour(s, slot), tappable ? PartyBoxState(s, slot)
                 : PLATE_STATE_NORMAL, 40, 25, PlateCut(84), 0);
    if (!mon->species)
        return;
    PartyIconAt(s, slot, x + 19, oy + 20);
    PartyHeldSign(s, slot, x + 19, oy + 20);
    if (!mon->isEgg)
    {
        StyledRight(&sSmall, LevelText(mon->level), x + 98, oy + 6, STYLE_WHITE);
        GenderSign(x + 88, oy + 20, mon->gender);
    }
    StyledStr(FitFont(&sNormal, mon->nick, 90), mon->nick, x + 8, oy + 40, STYLE_WHITE);
    if (v->desc[slot] != CTR_PARTY_DESC_NONE && v->state == CTR_PARTY_VIEW_LIST)
    {
        const u8 *desc = CtrParty_DescText(v->desc[slot]);

        TextIn(desc, x, oy + 56, PARTY_LEFT_W, 22, &sNormal, 'r', DescIsYes(v->desc[slot]) ? STYLE_WHITE : STYLE_SOFT, 6);
    }
    else if (!mon->isEgg)
    {
        HpBar(x + 8, oy + 60, 88, mon->hp, mon->maxHp);
        StyledRight(&sSmall, HpText(mon->hp, mon->maxHp), x + 98, oy + 67, STYLE_WHITE);
    }
    if (tappable)
        AddHit(x, AREA_Y, PARTY_LEFT_W, 84, HIT_NAT_SLOT + slot);
}

static void DrawPartyBox(const ViewState *s, int slot, int by)
{
    const MonView *mon = &s->party[slot];
    const struct CtrPartyView *v = &s->pv;
    int x = PARTY_RIGHT_X, w = PARTY_RIGHT_W, oy;

    if (!mon->species)
    {
        Plate(x, by, w, PARTY_ROW_H, sGroupOff, PLATE_STATE_NORMAL);
        return;
    }
    oy = PlateEx(x, by, w, PARTY_ROW_H, PartyBoxColour(s, slot), PartyBoxState(s, slot), 36, 30, PlateCut(PARTY_ROW_H), 0);
    PartyIconAt(s, slot, x + 18, oy + 15);
    PartyHeldSign(s, slot, x + 18, oy + 15);
    StyledStr(FitFont(&sNormal, mon->nick, 80), mon->nick, x + 44, TextY(oy, 16, &sNormal) + 1, STYLE_WHITE);
    if (!mon->isEgg)
    {
        StyledStr(&sSmall, LevelText(mon->level), x + 44, oy + 15, STYLE_WHITE);
        GenderSign(x + 74, oy + 15, mon->gender);
    }
    if (v->desc[slot] != CTR_PARTY_DESC_NONE)
        TextIn(CtrParty_DescText(v->desc[slot]), x + 120, oy, w - 120, PARTY_ROW_H, &sNormal, 'r',
               DescIsYes(v->desc[slot]) ? STYLE_WHITE : STYLE_SOFT, 10);
    else if (!mon->isEgg)
    {
        HpBar(x + 130, oy + 8, 68, mon->hp, mon->maxHp);
        StyledRight(&sSmall, HpText(mon->hp, mon->maxHp), x + w - 8, oy + 14, STYLE_WHITE);
    }
    AddHit(x, by, w, PARTY_ROW_H, HIT_NAT_SLOT + slot);
}

/* A dot in the cream end of an entry: the game's cursor emerald. */
static void EntryDot(int cx, int cy, Rgb colour)
{
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx)
        {
            int d = dx * dx + dy * dy;

            if (d <= 10)
                Put(cx + dx, cy + dy, d > 5 ? PackRgb(24, 88, 56) : PackRgb(colour.r, colour.g, colour.b));
        }
}

static void DrawPartyEntries(const ViewState *s, int bottom)
{
    const struct CtrPartyView *v = &s->pv;
    int n = v->numEntries, pitch = n <= 5 ? PARTY_ROW : (AREA_H + 3) / (n ? n : 1), h = pitch - 3;

    for (int i = 0; i < n; ++i)
    {
        int by = AREA_Y + i * pitch, oy;
        bool8 pressed = s->pressed == HIT_NAT_ENTRY + i;
        Rgb colour = i == v->cancelEntry ? sCancelGrey : (v->fieldMove >> i) & 1 ? sFieldMove : sSlate;

        if (by + h > bottom)
            break;
        oy = PlateEx(PARTY_RIGHT_X, by, PARTY_RIGHT_W, h, pressed ? sEmerald : colour,
                     pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL, 36, 30, PlateCut(h), 0);
        EntryDot(PARTY_RIGHT_X + 16, oy + h / 2, i == v->entryCursor ? sEmerald : colour);
        TextIn(v->entries[i], PARTY_RIGHT_X + 40, oy, PARTY_RIGHT_W - 40, h, &sNormal, 'l', STYLE_WHITE, 6);
        AddHit(PARTY_RIGHT_X, by, PARTY_RIGHT_W, h, HIT_NAT_ENTRY + i);
    }
}

/* The message over the right column's foot (and YES/NO over it); returns
 * where it starts, the column's foot without one. */
static int DrawPartyMessage(const ViewState *s)
{
    const struct CtrPartyView *v = &s->pv;
    int lines, h, y, oy;

    if (!v->message && !v->yesNo)
        return AREA_Y + AREA_H;
    lines = v->message ? TextLines(v->msg, 0, 0, PARTY_RIGHT_W - 16, PARTY_LINE, 4, &sNormal, STYLE_INK, FALSE) : 0;
    if (lines < 1)
        lines = 1;
    h = 12 + lines * PARTY_LINE;
    y = AREA_Y + AREA_H - h;
    if (v->message)
    {
        oy = Plate(PARTY_RIGHT_X, y, PARTY_RIGHT_W, h, sCreamPlate, PLATE_STATE_NORMAL);
        TextLines(v->msg, PARTY_RIGHT_X + 8, oy + 5, PARTY_RIGHT_W - 16, PARTY_LINE, 4, &sNormal, STYLE_INK, TRUE);
        if (!v->yesNo)
            AddHit(PARTY_RIGHT_X, y, PARTY_RIGHT_W, h, HIT_NAT_MSG);
    }
    else
        h = 0, y = AREA_Y + AREA_H + 3;
    if (v->yesNo)
    {
        int bw = (PARTY_RIGHT_W - 4) / 2, by = y - PARTY_ROW;

        for (int i = 0; i < 2; ++i)
        {
            u8 id = i ? HIT_NO : HIT_YES;
            bool8 pressed = s->pressed == id;
            int bx = PARTY_RIGHT_X + i * (bw + 4);

            oy = Plate(bx, by, bw, PARTY_ROW_H, pressed ? sEmerald : i ? sCancelGrey : sSlate,
                       pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
            TextIn(i ? gText_No : gText_Yes, bx, oy, bw, PARTY_ROW_H, &sNormal, 'c', STYLE_WHITE, 4);
            AddHit(bx, by, bw, PARTY_ROW_H, id);
        }
        y = by;
    }
    return y - 3;
}

static const u8 *SummaryStatName(int i);

/* A level up's stats (a Rare Candy) over the right column, as the game's
 * window: what each rose by, then what it is; a tap goes on, as A. */
/* A cream plate whose face is one flat colour: a list's ground, where the
 * plate's darker lower band would cross its rows. */
static void FlatPlate(int x, int y, int w, int h)
{
    int c = PlateCut(h);
    u16 face = PackRgb(sCreamPlate.r, sCreamPlate.g, sCreamPlate.b);

    Plate(x, y, w, h, sCreamPlate, PLATE_STATE_NORMAL);
    FillRect(x + 2, y + c, w - 4, h - 2 * c, face);
    FillRect(x + c, y + 2, w - 2 * c, h - 4, face);
}

static void DrawPartyStats(const ViewState *s)
{
    /* The task's order (HP ATK DEF SPEED SPATK SPDEF) as the window lists it. */
    static const u8 order[6] = {0, 1, 2, 4, 5, 3};
    const struct CtrPartyView *v = &s->pv;
    int x = PARTY_RIGHT_X, w = PARTY_RIGHT_W, rowH = (AREA_H - 12) / 6, oy;

    FlatPlate(x, AREA_Y, w, AREA_H);
    oy = AREA_Y;
    for (int i = 0; i < 6; ++i)
    {
        int k = order[i], ry = oy + 6 + i * rowH, ty = TextY(ry, rowH, &sNormal);
        u8 text[12];

        if (i)
            FillRgb(x + 8, ry - 1, w - 16, 1, sRule);
        StyledStr(&sNormal, SummaryStatName(i), x + 12, ty, STYLE_GRAY);
        if (v->statsPage == 0)
        {
            text[0] = CHAR_PLUS;
            StringCopy(text + 1, Number(v->statsAfter[k] - v->statsBefore[k], 3, STR_CONV_MODE_LEFT_ALIGN));
            StyledRight(&sNormal, text, x + w - 12, ty, STYLE_GREEN);
        }
        else
            StyledRight(&sNormal, Number(v->statsAfter[k], 3, STR_CONV_MODE_LEFT_ALIGN), x + w - 12, ty, STYLE_INK);
    }
    AddHit(x, AREA_Y, w, AREA_H, HIT_NAT_MSG);
}

static void DrawGameParty(const ViewState *s)
{
    const struct CtrPartyView *v = &s->pv;
    bool8 menu = v->state == CTR_PARTY_VIEW_MENU, stats = v->state == CTR_PARTY_VIEW_STATS;
    int lead = (menu || stats) && v->chosen >= 0 && v->chosen < PARTY_SIZE ? v->chosen : 0, oy, bottom;

    DrawPartyLead(s, lead);
    oy = Plate(AREA_X, AREA_Y + 87, PARTY_LEFT_W, 44, sCreamPlate, PLATE_STATE_NORMAL);
    TextLines(v->prompt, AREA_X + 8, oy + 5, PARTY_LEFT_W - 14, PARTY_LINE, 2, &sNormal, STYLE_INK, TRUE);
    {
        bool8 pressed = s->pressed == HIT_CANCEL;

        oy = Plate(AREA_X, AREA_Y + 134, PARTY_LEFT_W, 30, pressed ? sEmerald : sSlate,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(gText_Cancel2, AREA_X, oy, PARTY_LEFT_W, 30, &sNormal, 'c', STYLE_WHITE, 4);
        AddHit(AREA_X, AREA_Y + 134, PARTY_LEFT_W, 30, HIT_CANCEL);
    }
    if (stats)
    {
        DrawPartyStats(s);
        return;
    }
    bottom = DrawPartyMessage(s);
    if (menu)
    {
        DrawPartyEntries(s, bottom);
        return;
    }
    for (int k = 1; k < PARTY_SIZE; ++k)
    {
        int by = AREA_Y + (k - 1) * PARTY_ROW;

        if (by + PARTY_ROW_H > bottom)
            break;
        DrawPartyBox(s, k, by);
    }
}

static void PartyTap(const ViewState *s, u8 id)
{
    const struct CtrPartyView *v = &s->pv;

    if (id >= HIT_NAT_SLOT && id < HIT_NAT_SLOT + PARTY_SIZE)
        CtrParty_TapSlot(id - HIT_NAT_SLOT);
    else if (id >= HIT_NAT_ENTRY && id < HIT_NAT_ENTRY + CTR_VIEW_ENTRIES)
        CtrMenu_PostEntry(id - HIT_NAT_ENTRY);
    else if (id == HIT_YES || id == HIT_NO)
        CtrMenu_PostEntry(id == HIT_YES ? 0 : 1);
    else if (id == HIT_NAT_MSG)
        CtrMenu_PostAck();
    else if (id == HIT_CANCEL && v->state == CTR_PARTY_VIEW_STATS)
        CtrMenu_PostAck();
    else if (id == HIT_CANCEL)
    {
        if (v->state == CTR_PARTY_VIEW_MENU || v->yesNo)
            CtrMenu_PostEntry(-1);
        else
            CtrParty_TapSlot(PARTY_SIZE + 1);
    }
}

/* ------------------------------------------------------------------------ */
/* POKéMON: the summary                                                     */
/* ------------------------------------------------------------------------ */

/*
 * Four tabs (INFO, SKILLS, MOVES, CONTEST) over the mon on the left - its
 * front picture whole, its name, level, the ball it was caught in (its own
 * item icon) and its number - and the page on the right. A move tapped opens
 * its selection on it: the moves then as rows with the chosen one's details
 * under them; tapped again it is picked to be moved, and a move tapped then
 * takes its place, as A does in the game. Learning a move, the fifth row is
 * the new one.
 */
enum
{
    HIT_SUM_TAB = HIT_ROW,          /* + page */
    HIT_SUM_PREV = HIT_ROW + 4,
    HIT_SUM_NEXT,
    HIT_SUM_STATS,
    HIT_SUM_MOVE = HIT_ROW + 8,     /* + move row, 4 the new move */
};

#define SUM_TAB_H 18
#define SUM_Y (AREA_Y + SUM_TAB_H + 3)
#define SUM_H (AREA_H - SUM_TAB_H - 3)
#define SUM_LEFT_W 100
#define SUM_RIGHT_X (AREA_X + SUM_LEFT_W + 4)
#define SUM_RIGHT_W (AREA_W - SUM_LEFT_W - 4)

static const Rgb sContestColour[CONTEST_CATEGORIES_COUNT] = {
    {224, 88, 64}, {72, 128, 232}, {240, 120, 176}, {88, 184, 96}, {224, 184, 48},
};
static const u8 sContestIconPal[CONTEST_CATEGORIES_COUNT] = {0, 1, 1, 2, 0};

static const u8 *SummaryTabName(int page)
{
    switch (page)
    {
    case 0: return Ascii(CTR_TEXT("INFO", "INFO", "INFOS", "INFO", "INFO"));
    case 1: return Ascii(CTR_TEXT("SKILLS", "HABILID.", "TALENTS", "ABILITÀ", "ATRIBUTOS"));
    case 2: return Ascii(CTR_TEXT("MOVES", "MOVIM.", "CAPACITÉS", "MOSSE", "GOLPES"));
    }
    return Ascii(CTR_TEXT("CONTEST", "CONCURSO", "CONCOURS", "GARE", "CONCURSO"));
}

static bool8 SummaryMovesPage(const struct CtrSummaryView *v)
{
    return v->page == 2 || v->page == 3;
}

/* The tabs; choosing or moving a move, only its page's and CANCEL. With
 * CANCEL (choosing, moving or replacing a move) the tabs have three places
 * and CANCEL the fourth: the last three tabs when the last one is to show
 * (CONTEST its page, or open while replacing), else the first three. */
static void DrawSummaryTabs(const ViewState *s)
{
    const struct CtrSummaryView *v = &s->sv;
    bool8 picking = v->movesMode == CTR_SUM_MOVES_CHOOSE || v->movesMode == CTR_SUM_MOVES_SWITCH;
    bool8 cancel = v->movesMode != CTR_SUM_MOVES_VIEW;
    int n = 4, x = AREA_X, gap = 2, tw = (AREA_W - gap * (n - 1)) / n;
    int first = 0, count = n;

    if (cancel)
    {
        bool8 lastShown = v->page == n - 1 || (!picking && n - 1 >= v->minPage && n - 1 <= v->maxPage);

        count = n - 1;
        first = lastShown ? 1 : 0;
    }
    for (int i = first; i < first + count; ++i)
    {
        bool8 live = i == v->page, open = i >= v->minPage && i <= v->maxPage && !(picking && !live);
        bool8 pressed = s->pressed == HIT_SUM_TAB + i;
        int oy;

        oy = Plate(x, AREA_Y, tw, SUM_TAB_H, live || pressed ? sEmerald : open ? sLight : sGroupOff,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(SummaryTabName(i), x, oy, tw, SUM_TAB_H, &sSmall, 'c', live || pressed ? STYLE_WHITE : open ? STYLE_INK
               : STYLE_GRAY, 3);
        if (open && !live)
            AddHit(x, AREA_Y, tw, SUM_TAB_H, HIT_SUM_TAB + i);
        x += tw + gap;
    }
    if (cancel)
    {
        int w = AREA_X + AREA_W - x, oy;
        bool8 pressed = s->pressed == HIT_CANCEL;

        if (w < 40)
            return;
        oy = Plate(x, AREA_Y, w, SUM_TAB_H, pressed ? sEmerald : sCancelGrey,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(gText_Cancel2, x, oy, w, SUM_TAB_H, &sSmall, 'c', STYLE_WHITE, 3);
        AddHit(x, AREA_Y, w, SUM_TAB_H, HIT_CANCEL);
    }
}

/* The picture's part of the summary's left plate, above the name: where the
 * mon moves, the part redrawn while only it does (DirtyRect). */
#define SUM_PIC_BOTTOM (SUM_Y + 74)

/*
 * The mon as its sprite shows it now (CtrSummaryView's pic): the frame and
 * colours of its OBJ tiles, centred on (cx, cy), moved, turned and scaled by
 * its animation as the GBA draws an affine sprite - each pixel of its box
 * mapped through the matrix back into the 64x64 picture - and kept to the
 * box (x0, y0)-(x1, y1).
 */
static void DrawSummaryPicLive(const struct CtrSummaryView *v, int cx, int cy, int x0, int y0, int x1, int y1)
{
    bool8 affine = v->picAffine & ST_OAM_AFFINE_ON_MASK;
    int half = v->picAffine == ST_OAM_AFFINE_DOUBLE ? 64 : 32;
    u16 pal[16];

    for (int i = 0; i < 16; ++i)
        pal[i] = Rgb565(v->picPal[i]);
    cx += v->picX;
    cy += v->picY;
    for (int py = -half; py < half; ++py)
    {
        int y = cy + py;

        if (y < y0 || y >= y1)
            continue;
        for (int px = -half; px < half; ++px)
        {
            int x = cx + px, tx, ty;
            u8 byte;

            if (x < x0 || x >= x1)
                continue;
            if (affine)
            {
                tx = ((v->picMatrix[0] * px + v->picMatrix[1] * py) >> 8) + 32;
                ty = ((v->picMatrix[2] * px + v->picMatrix[3] * py) >> 8) + 32;
                if (tx < 0 || tx >= 64 || ty < 0 || ty >= 64)
                    continue;
            }
            else
            {
                tx = v->picHFlip ? 31 - px : px + 32;
                ty = py + 32;
            }
            byte = v->picTiles[((ty >> 3) * 8 + (tx >> 3)) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)];
            byte = tx & 1 ? byte >> 4 : byte & 15;
            if (byte)
                Put(x, y, pal[byte]);
        }
    }
}

static void DrawSummaryMon(const ViewState *s)
{
    const struct CtrSummaryView *v = &s->sv;
    int x = AREA_X, oy, slot;
    u8 text[16];

    oy = PlateEx(x, SUM_Y, SUM_LEFT_W, SUM_H, sEmerald, PLATE_STATE_NORMAL, SUM_LEFT_W - 14, 15, PlateCut(SUM_H), 0);
    if (v->loading)
        return;
    if (v->picTiles)
        DrawSummaryPicLive(v, x + 46, oy + 36, x + 2, SUM_Y + 2, x + SUM_LEFT_W - 2, min(oy + 70, SUM_PIC_BOTTOM));
    else if ((slot = FrontPic(SummaryPicSpecies(v), v->shiny)) >= 0)
        DrawSprite(sFronts[slot].tiles, 8, 8, x + 14, oy + 4, sFronts[slot].pal.c);
    StyledStr(FitFont(&sNormal, v->nick, 78), v->nick, x + 8, oy + 72, STYLE_INK);
    if (!v->isEgg)
    {
        StyledStr(&sSmall, LevelText(v->level), x + 8, oy + 88, STYLE_INK);
        GenderSign(x + 70, oy + 88, v->gender);
        if (v->ball)
            DrawItemIcon(v->ball, x + 6, oy + 104);
        text[0] = CHAR_N;
        text[1] = CHAR_o;
        ConvertIntToDecimalStringN(text + 2, v->dexNum, STR_CONV_MODE_LEADING_ZEROS, 3);
        StyledStr(&sSmall, text, x + 32, oy + 110, STYLE_GRAY);
    }
    if (v->canChangeMon)
    {
        AddHit(x, SUM_Y, SUM_LEFT_W, SUM_H / 2, HIT_SUM_PREV);
        AddHit(x, SUM_Y + SUM_H / 2, SUM_LEFT_W, SUM_H - SUM_H / 2, HIT_SUM_NEXT);
    }
}

static void SummaryRow(int x, int w, int ry, int h, const u8 *label, const u8 *value)
{
    StyledStr(FitFont(&sSmall, label, 78), label, x + 8, TextY(ry, h, &sSmall), STYLE_GRAY);
    if (value)
        StyledStr(FitFont(&sSmall, value, w - 98), value, x + 90, TextY(ry, h, &sSmall), STYLE_INK);
}

static void DrawSummaryInfo(const struct CtrSummaryView *v, int x, int y, int w, int h)
{
    int oy, memoY = y + 88;

    if (!v->isEgg)
    {
        const u8 *item = v->item ? GetItemName(v->item) : Ascii(CTR_TEXT("NONE", "NINGUNO", "AUCUN", "NESSUNO", "NENHUM"));

        for (int i = 0; i < 4; ++i)
        {
            int ry = y + 4 + i * 20;

            if (i)
                FillRect(x + 8, ry, w - 16, 1, PackRgb(228, 218, 202));
        }
        SummaryRow(x, w, y + 4, 20, Ascii(CTR_TEXT("TYPE", "TIPO", "TYPE", "TIPO", "TIPO")), NULL);
        DrawTypeIcon(v->types[0], x + 90, y + 6);
        if (v->types[1] != v->types[0])
            DrawTypeIcon(v->types[1], x + 124, y + 6);
        SummaryRow(x, w, y + 24, 20, Ascii(CTR_TEXT("OT", "EO", "DO", "AO", "TO")), v->ot);
        SummaryRow(x, w, y + 44, 20, Ascii(CTR_TEXT("ID No.", "Nº ID", "No. ID", "N. ID", "Nº ID")),
                   Number(v->otId, 5, STR_CONV_MODE_LEADING_ZEROS));
        SummaryRow(x, w, y + 64, 20, Ascii(CTR_TEXT("ITEM", "OBJETO", "OBJET", "STRUMENTO", "ITEM")), item);
    }
    else
    {
        memoY = y + 4;
        if (v->abilityDesc)
        {
            oy = Plate(x + 4, memoY, w - 8, 44, sLight, PLATE_STATE_NORMAL);
            TextLines(v->abilityDesc, x + 10, oy + 6, w - 20, 16, 2, &sNormal, STYLE_INK, TRUE);
            memoY += 48;
        }
    }
    oy = Plate(x + 4, memoY, w - 8, y + h - 4 - memoY, sLight, PLATE_STATE_NORMAL);
    TextLines(v->memo, x + 10, oy + 8, w - 20, 16, (y + h - 12 - memoY) / 16, &sNormal, STYLE_INK, TRUE);
}

static const u8 *SummaryStatName(int i)
{
    switch (i)
    {
    case 0: return Ascii(CTR_TEXT("HP", "PS", "PV", "PS", "PS"));
    case 1: return Ascii(CTR_TEXT("ATTACK", "ATAQUE", "ATTAQUE", "ATTACCO", "ATAQUE"));
    case 2: return Ascii(CTR_TEXT("DEFENSE", "DEFENSA", "DÉFENSE", "DIFESA", "DEFESA"));
    case 3: return Ascii(CTR_TEXT("SP. ATK", "AT. ESP.", "ATQ. SPÉ.", "ATT. SP.", "AT. ESP."));
    case 4: return Ascii(CTR_TEXT("SP. DEF", "DEF. ESP.", "DÉF. SPÉ.", "DIF. SP.", "DEF. ESP."));
    }
    return Ascii(CTR_TEXT("SPEED", "VELOC.", "VITESSE", "VELOCITÀ", "VELOC."));
}

static void DrawSummarySkills(const ViewState *s, int x, int y, int w, int h)
{
    const struct CtrSummaryView *v = &s->sv;
    bool8 extra = v->statsMode != 0;
    /* The game's stats are in its order: HP, ATK, DEF, SP. ATK, SP. DEF,
     * SPEED; its IVs and EVs in the stat order (SPEED fourth). */
    static const u8 order[6] = {STAT_HP, STAT_ATK, STAT_DEF, STAT_SPATK, STAT_SPDEF, STAT_SPEED};
    int oy, ry;

    if (extra)
        StyledRight(&sSmall, Ascii(v->statsMode == 1 ? "IV" : "EV"), x + w - 10, y + 2, STYLE_GRAY);
    for (int i = 0; i < 6; ++i)
    {
        const u8 *value = i == 0 ? HpText(v->hp, v->maxHp) : Number(v->stats[i - 1], 3, STR_CONV_MODE_LEFT_ALIGN);

        ry = y + 4 + i * 16 + (extra ? 6 : 0);
        StyledStr(FitFont(&sSmall, SummaryStatName(i), 80), SummaryStatName(i), x + 8, TextY(ry, 16, &sSmall), STYLE_GRAY);
        StyledRight(&sSmall, value, x + 120, TextY(ry, 16, &sSmall), STYLE_INK);
        if (extra)
        {
            u8 n = v->statsMode == 1 ? v->ivs[order[i]] : v->evs[order[i]];

            StyledRight(&sSmall, Number(n, 3, STR_CONV_MODE_LEFT_ALIGN), x + w - 10, TextY(ry, 16, &sSmall),
                        v->statsMode == 1 && n >= 30 ? STYLE_GREEN : STYLE_INK);
        }
    }
    if (!extra)
        HpBar(x + 128, y + 9, 70, v->hp, v->maxHp);
    if (CtrExtras_Get("ivs_evs"))
        AddHit(x, y, w, 6 * 16 + 8, HIT_SUM_STATS);
    ry = y + 102 + (extra ? 4 : 0);
    StyledStr(&sSmall, Ascii(CTR_TEXT("EXP. POINTS", "PUNTOS EXP.", "POINTS EXP.", "PUNTI ESP.", "PONTOS EXP.")), x + 8,
              TextY(ry, 16, &sSmall), STYLE_GRAY);
    StyledRight(&sSmall, Number(v->exp, 7, STR_CONV_MODE_LEFT_ALIGN), x + w - 8, TextY(ry, 16, &sSmall), STYLE_INK);
    if (!extra)
    {
        ry += 15;
        StyledStr(&sSmall, Ascii(CTR_TEXT("NEXT LV.", "SIG. NV.", "N. SUIV.", "LV SUCC.", "PRÓX. NV.")), x + 8,
                  TextY(ry, 16, &sSmall), STYLE_GRAY);
        StyledRight(&sSmall, Number(v->toNextLevel, 7, STR_CONV_MODE_LEFT_ALIGN), x + w - 8, TextY(ry, 16, &sSmall),
                    STYLE_INK);
    }
    oy = Plate(x + 4, y + h - 24, w - 8, 20, sLight, PLATE_STATE_NORMAL);
    if (v->abilityName)
    {
        TextIn(v->abilityName, x + 4, oy, 80, 20, &sSmall, 'l', STYLE_GREEN, 6);
        TextIn(v->abilityDesc, x + 84, oy, w - 92, 20, &sSmall, 'l', STYLE_INK, 2);
    }
}

static u8 MoveMaxPp(const struct CtrSummaryView *v, int i)
{
    return i < MAX_MON_MOVES ? CalculatePPWithBonus(v->moves[i], v->ppBonuses, i) : gBattleMoves[v->moves[i]].pp;
}

static const u8 *PpText(const struct CtrSummaryView *v, int i)
{
    static u8 text[16];
    u8 *end = StringCopy(text, Ascii("PP "));

    if (i < MAX_MON_MOVES)
    {
        end = ConvertIntToDecimalStringN(end, v->pp[i], STR_CONV_MODE_LEFT_ALIGN, 2);
        *end++ = CHAR_SLASH;
    }
    ConvertIntToDecimalStringN(end, MoveMaxPp(v, i), STR_CONV_MODE_LEFT_ALIGN, 2);
    return text;
}

static Rgb MoveColour(const struct CtrSummaryView *v, u16 move)
{
    if (move == MOVE_NONE)
        return sGroupOff;
    if (v->page == 3)
        return sContestColour[gContestMoves[move].contestCategory % CONTEST_CATEGORIES_COUNT];
    return gBattleMoves[move].type < NUMBER_OF_MON_TYPES ? sTypeColour[gBattleMoves[move].type] : sOffGrey;
}

/* The move's label: its type (and category with the split on), or its
 * contest category. */
static void DrawMoveLabel(const struct CtrSummaryView *v, u16 move, int cx, int y)
{
    if (v->page == 3)
    {
        u8 cat = gContestMoves[move].contestCategory % CONTEST_CATEGORIES_COUNT;

        if (sRes.typeTiles)
            DrawSprite(sRes.typeTiles + (NUMBER_OF_MON_TYPES + cat) * 8 * 32, 4, 2, cx - 16, y,
                       sRes.typePal[sContestIconPal[cat]].c);
    }
    else if (MoveSplitEnabled())
    {
        DrawTypeIcon(gBattleMoves[move].type, cx - 31, y);
        DrawCategoryIcon(move, cx + 3, y + 1);
    }
    else
        DrawTypeIcon(gBattleMoves[move].type, cx - 16, y);
}

/* The four moves as the page shows them: plates two by two. */
static void DrawSummaryMoveGrid(const ViewState *s, int x, int y, int w, int h)
{
    const struct CtrSummaryView *v = &s->sv;
    int cw = (w - 11) / 2, ch = (h - 11) / 2;

    for (int i = 0; i < MAX_MON_MOVES; ++i)
    {
        int bx = x + 4 + (i % 2) * (cw + 3), by = y + 4 + (i / 2) * (ch + 3), oy;
        u16 move = v->moves[i];
        bool8 pressed = move && s->pressed == HIT_SUM_MOVE + i;

        oy = PlateEx(bx, by, cw, ch, pressed ? sEmerald : MoveColour(v, move),
                     pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL, 0, 30, 5, 22);
        if (!move)
        {
            TextIn(gText_OneDash, bx, oy, cw, 22, &sNormal, 'c', STYLE_WHITE, 4);
            continue;
        }
        TextIn(gMoveNames[move], bx, oy, cw, 22, &sNormal, 'c', STYLE_WHITE, 4);
        DrawMoveLabel(v, move, bx + cw / 2, oy + 27);
        TextIn(PpText(v, i), bx, oy + 44, cw, ch - 46, &sSmall, 'c', v->pp[i] ? STYLE_INK : STYLE_RED, 4);
        AddHit(bx, by, cw, ch, HIT_SUM_MOVE + i);
    }
}

/* Choosing, moving or replacing a move: the moves as rows, the one the
 * game's cursor is on glowing (the one being moved emerald), and the
 * cursor's details under them. */
static void DrawSummaryMoveRows(const ViewState *s, int x, int y, int w, int h)
{
    const struct CtrSummaryView *v = &s->sv;
    int rows = v->movesMode == CTR_SUM_MOVES_REPLACE || v->movesMode == CTR_SUM_MOVES_HM ? 5 : 4;
    /* five rows (the new move too) are closer, so the details still fit under them */
    int pitch = rows == 5 ? 18 : 20, rh = pitch - 2, dy, oy, ph, descLines;
    u16 cursorMove = v->firstMove < 5 ? v->moves[v->firstMove] : MOVE_NONE;
    u8 cursor = v->movesMode == CTR_SUM_MOVES_SWITCH ? v->secondMove : v->firstMove;

    if (v->movesMode == CTR_SUM_MOVES_SWITCH)
        cursorMove = v->secondMove < 5 ? v->moves[v->secondMove] : MOVE_NONE;
    for (int i = 0; i < rows; ++i)
    {
        int by = y + 4 + i * pitch;
        u16 move = v->moves[i];
        bool8 moving = v->movesMode == CTR_SUM_MOVES_SWITCH && i == v->firstMove;
        bool8 pressed = s->pressed == HIT_SUM_MOVE + i;
        u8 state = pressed ? PLATE_STATE_PRESSED : i == cursor ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL;

        oy = PlateEx(x + 4, by, w - 8, rh, moving || pressed ? sEmerald : MoveColour(v, move), state, 0, 30, 3, 0);
        if (move)
        {
            TextIn(gMoveNames[move], x + 4, oy, w - 70, rh, &sSmall, 'l', STYLE_WHITE, 8);
            TextIn(PpText(v, i), x + w - 70, oy, 62, rh, &sSmall, 'r', STYLE_WHITE, 6);
        }
        else
            TextIn(gText_OneDash, x + 4, oy, w - 8, rh, &sSmall, 'l', STYLE_WHITE, 8);
        if (move || i == MAX_MON_MOVES)
            AddHit(x + 4, by, w - 8, rh, HIT_SUM_MOVE + i);
    }
    dy = y + 4 + rows * pitch + 1;
    ph = y + h - 4 - dy;
    oy = Plate(x + 4, dy, w - 8, ph, sLight, PLATE_STATE_NORMAL);
    descLines = (dy + ph - 3 - (oy + 16)) / 12;
    if (descLines < 1)
        descLines = 1;
    if (v->movesMode == CTR_SUM_MOVES_HM)
    {
        TextLines(gText_HMMovesCantBeForgotten2, x + 10, oy + 4, w - 20, 14, (dy + ph - 3 - (oy + 4)) / 14, &sSmall, STYLE_INK, TRUE);
        return;
    }
    if (cursorMove == MOVE_NONE)
        return;
    if (v->page == 3)
    {
        const struct ContestEffect *effect = &gContestEffects[gContestMoves[cursorMove].effect];

        StyledStr(&sSmall, Ascii(CTR_TEXT("APPEAL", "EXHIB.", "SÉDUC.", "ESIB.", "APELO")), x + 10, oy + 3, STYLE_GRAY);
        StyledStr(&sSmall, Number(effect->appeal / 10, 1, STR_CONV_MODE_LEFT_ALIGN), x + 62, oy + 3, STYLE_INK);
        StyledStr(&sSmall, Ascii(CTR_TEXT("JAM", "INTERF.", "PERTURB.", "INTRALC.", "INTERF.")), x + 90, oy + 3, STYLE_GRAY);
        StyledStr(&sSmall, Number(effect->jam / 10, 1, STR_CONV_MODE_LEFT_ALIGN), x + 150, oy + 3, STYLE_INK);
        TextLines(gContestEffectDescriptionPointers[gContestMoves[cursorMove].effect], x + 10, oy + 16, w - 20, 12, descLines,
                  &sSmall, STYLE_INK, TRUE);
    }
    else
    {
        u8 power = gBattleMoves[cursorMove].power, acc = gBattleMoves[cursorMove].accuracy;

        StyledStr(&sSmall, Ascii(CTR_TEXT("POWER", "POTENCIA", "PUISS.", "POTENZA", "PODER")), x + 10, oy + 3, STYLE_GRAY);
        StyledStr(&sSmall, power > 1 ? Number(power, 3, STR_CONV_MODE_LEFT_ALIGN) : gText_ThreeDashes, x + 62, oy + 3,
                  STYLE_INK);
        StyledStr(&sSmall, Ascii(CTR_TEXT("ACCURACY", "PRECISIÓN", "PRÉCISION", "PRECISIONE", "PRECISÃO")), x + 90, oy + 3,
                  STYLE_GRAY);
        StyledStr(&sSmall, acc ? Number(acc, 3, STR_CONV_MODE_LEFT_ALIGN) : gText_ThreeDashes, x + 150, oy + 3, STYLE_INK);
        TextLines(gMoveDescriptionPointers[cursorMove - 1], x + 10, oy + 16, w - 20, 12, descLines, &sSmall, STYLE_INK, TRUE);
    }
}

static void DrawGameSummary(const ViewState *s)
{
    const struct CtrSummaryView *v = &s->sv;
    int x = SUM_RIGHT_X, w = SUM_RIGHT_W;

    DrawSummaryTabs(s);
    DrawSummaryMon(s);
    Plate(x, SUM_Y, w, SUM_H, sCreamPlate, PLATE_STATE_NORMAL);
    if (v->loading)
        return;
    switch (v->page)
    {
    case 0: DrawSummaryInfo(v, x, SUM_Y, w, SUM_H); break;
    case 1: DrawSummarySkills(s, x, SUM_Y, w, SUM_H); break;
    default:
        if (v->movesMode == CTR_SUM_MOVES_VIEW)
            DrawSummaryMoveGrid(s, x, SUM_Y, w, SUM_H);
        else
            DrawSummaryMoveRows(s, x, SUM_Y, w, SUM_H);
        break;
    }
}

static void SummaryTap(const ViewState *s, u8 id)
{
    const struct CtrSummaryView *v = &s->sv;

    if (id >= HIT_SUM_TAB && id < HIT_SUM_TAB + 4)
        CtrSummary_NativeTap(CTR_SUM_TAP_PAGE, id - HIT_SUM_TAB);
    else if (id == HIT_SUM_PREV || id == HIT_SUM_NEXT)
        CtrSummary_NativeTap(CTR_SUM_TAP_MON, id == HIT_SUM_NEXT);
    else if (id == HIT_SUM_STATS && v->page == 1)
        CtrSummary_NativeTap(CTR_SUM_TAP_STATS, 0);
    else if (id >= HIT_SUM_MOVE && id <= HIT_SUM_MOVE + MAX_MON_MOVES && SummaryMovesPage(v))
        CtrSummary_NativeTap(CTR_SUM_TAP_MOVE, id - HIT_SUM_MOVE);
    else if (id == HIT_CANCEL)
        CtrSummary_NativeTap(CTR_SUM_TAP_BACK, 0);
}

/* ------------------------------------------------------------------------ */
/* BAG                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * The pocket's name over the bag's own picture on the left, its arrows the
 * previous and next pocket, a dot a pocket under it; the rows the game's
 * list shows on the right, the cursor's emerald; under them the item's
 * description with its icon. The item's menu, how many and the questions
 * take the description's place, a message too. The list's rows are the
 * game's: a touch there goes to the bag's own list touch at the same row
 * (BagListTouch), so a tap picks, a drag scrolls and a hold moves the item.
 */
enum
{
    HIT_BAG_PREV = HIT_ROW,
    HIT_BAG_NEXT,
    HIT_BAG_DESC,           /* moving an item: left where it was */
    HIT_BAG_ENTRY = HIT_ROW + 4,    /* + entry shown */
    HIT_BAG_COUNT = HIT_ROW + 12,   /* -10, -1, +1, +10 */
};
#define BAG_LEFT_W 104
#define BAG_LIST_X (AREA_X + BAG_LEFT_W + 4)
/* Right of the list, its scroll bar, as the Pokédex's. */
#define BAG_LIST_W (AREA_W - BAG_LEFT_W - 4 - 12)
#define BAG_BAR_X (BAG_LIST_X + BAG_LIST_W + 2)
#define BAG_ROW_H 14
#define BAG_LIST_TOP (AREA_Y + 3)
#define BAG_LIST_H (CTR_BAG_ROWS * BAG_ROW_H + 6)
#define BAG_FOOT_Y (AREA_Y + BAG_LIST_H + 3)
#define BAG_FOOT_H (AREA_Y + AREA_H - BAG_FOOT_Y)

static const s8 sBagSteps[4] = {-10, -1, 1, 10};

static bool8 BagListShown(const struct CtrBagView *v)
{
    return v->state == CTR_BAG_VIEW_LIST || v->state == CTR_BAG_VIEW_SWAP;
}

static void DrawBagPocket(const ViewState *s)
{
    const struct CtrBagView *v = &s->bv;
    int x = AREA_X, oy, slot = BagPic(v->female, v->bagFrame);

    oy = Plate(x, AREA_Y, BAG_LEFT_W, 20, sEmerald, PLATE_STATE_NORMAL);
    TextIn(gPocketNamesStringsTable[v->pocket], x + 10, oy, BAG_LEFT_W - 20, 20, &sSmall, 'c', STYLE_WHITE, 2);
    if (v->pocketSwitch && BagListShown(v))
    {
        DrawArrow(x + 5, oy + 6, -1, PackRgb(255, 255, 255));
        DrawArrow(x + BAG_LEFT_W - 9, oy + 6, 1, PackRgb(255, 255, 255));
        AddHit(x, AREA_Y, BAG_LEFT_W / 2, 24, HIT_BAG_PREV);
        AddHit(x + BAG_LEFT_W / 2, AREA_Y, BAG_LEFT_W - BAG_LEFT_W / 2, 24, HIT_BAG_NEXT);
    }
    if (slot >= 0)
        DrawSprite(sBagPic.tiles, 8, 8, x + (BAG_LEFT_W - 64) / 2, AREA_Y + 26, sBagPic.pal.c);
    for (int k = 0; k < POCKETS_COUNT; ++k)
        EntryDot(x + BAG_LEFT_W / 2 + (2 * k - (POCKETS_COUNT - 1)) * 5, AREA_Y + 100, k == v->pocket ? sEmerald : sLight);
}

static void DrawBagList(const ViewState *s)
{
    const struct CtrBagView *v = &s->bv;
    int x = BAG_LIST_X, w = BAG_LIST_W;

    FlatPlate(x, AREA_Y, w, BAG_LIST_H);
    for (int i = 0; i < v->numRows; ++i)
    {
        int ry = BAG_LIST_TOP + i * BAG_ROW_H, oy = ry;
        bool8 moving = v->state == CTR_BAG_VIEW_SWAP && i == v->swapRow;
        bool8 chosen = v->state == CTR_BAG_VIEW_SWAP ? moving : i == v->cursor;
        u8 style = chosen ? STYLE_WHITE : STYLE_INK;

        if (chosen)
            oy = PlateEx(x + 4, ry, w - 8, BAG_ROW_H, sEmerald, PLATE_STATE_NORMAL, 0, 30, 3, 0);
        else if (i & 1)
            FillRect(x + 4, ry, w - 8, BAG_ROW_H, PackRgb(sOddRow.r, sOddRow.g, sOddRow.b));
        /* TextY keeps a small font a row into its room: a row this low
         * centres its capitals 2 higher. */
        oy -= 2;
        TextIn(v->names[i], x + 3, oy, w - 50, BAG_ROW_H, &sSmall, 'l', style, 6);
        if (v->flags[i] & CTR_BAG_ROW_COUNT)
        {
            u8 count[8] = {CHAR_MULT_SIGN};

            ConvertIntToDecimalStringN(count + 1, v->counts[i], STR_CONV_MODE_LEFT_ALIGN, 3);
            TextIn(count, x + w - 50, oy, 44, BAG_ROW_H, &sSmall, 'r', style, 4);
        }
        else if (v->flags[i] & CTR_BAG_ROW_REGISTERED)
            TextIn(Ascii("SELECT"), x + w - 60, oy, 54, BAG_ROW_H, &sSmall, 'r', chosen ? STYLE_WHITE : STYLE_GRAY, 4);
        /* Where the item being moved would go: a line over that row. */
        if (v->state == CTR_BAG_VIEW_SWAP && i == v->cursor && !moving)
            FillRect(x + 4, ry - 1, w - 8, 2, PackRgb(sEmerald.r, sEmerald.g, sEmerald.b));
    }
    /* The scroll bar: the rows shown out of those the pocket has, the thumb
     * as long as the page is of the whole. */
    Plate(BAG_BAR_X, AREA_Y, 10, BAG_LIST_H, sGroupOff, PLATE_STATE_NORMAL);
    if (v->total > v->numRows && v->numRows > 0)
    {
        int th = BAG_LIST_H * v->numRows / v->total, room;

        if (th < 18)
            th = 18;
        room = BAG_LIST_H - th;
        Plate(BAG_BAR_X, AREA_Y + room * min(v->scroll, v->total - v->numRows) / (v->total - v->numRows), 10, th,
              sEmerald, PLATE_STATE_NORMAL);
    }
}

/* The description: the item's icon and what the game's window says. */
static void DrawBagDesc(const ViewState *s)
{
    const struct CtrBagView *v = &s->bv;
    int oy = PlateEx(AREA_X, BAG_FOOT_Y, AREA_W, BAG_FOOT_H, sCreamPlate, PLATE_STATE_NORMAL, 0, 25, 4, 0);

    DrawItemIcon(v->item != ITEM_NONE ? v->item : ITEM_LIST_END, AREA_X + 8, oy + (BAG_FOOT_H - 24) / 2);
    TextLines(v->text, AREA_X + 40, oy + 2, AREA_W - 48, 13, 3, &sNormal, STYLE_INK, TRUE);
    if (v->state == CTR_BAG_VIEW_SWAP)
        AddHit(AREA_X, BAG_FOOT_Y, AREA_W, BAG_FOOT_H, HIT_BAG_DESC);
}

static void DrawBagActions(const ViewState *s)
{
    const struct CtrBagView *v = &s->bv;
    int n = v->numEntries ? v->numEntries : 1, aw = (AREA_W - (n - 1) * 2) / n;

    for (int i = 0; i < v->numEntries; ++i)
    {
        int bx = AREA_X + i * (aw + 2), oy;
        bool8 pressed = s->pressed == HIT_BAG_ENTRY + i, cancel = i == v->numEntries - 1;
        u8 state = pressed ? PLATE_STATE_PRESSED : i == v->entryCursor ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL;

        oy = Plate(bx, BAG_FOOT_Y, aw, BAG_FOOT_H, pressed ? sEmerald : cancel ? sCancelGrey : sSlate, state);
        TextIn(v->entries[i], bx, oy, aw, BAG_FOOT_H, &sNormal, 'c', STYLE_WHITE, 3);
        AddHit(bx, BAG_FOOT_Y, aw, BAG_FOOT_H, HIT_BAG_ENTRY + i);
    }
}

/* A plate of the foot's row: its text centred, its hit. */
static void BagFootButton(const ViewState *s, int x, int w, Rgb colour, const u8 *text, const Font *font, u8 id)
{
    bool8 pressed = s->pressed == id;
    int oy = Plate(x, BAG_FOOT_Y, w, BAG_FOOT_H, pressed ? sEmerald : colour,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);

    TextIn(text, x, oy, w, BAG_FOOT_H, font, 'c', STYLE_WHITE, 2);
    AddHit(x, BAG_FOOT_Y, w, BAG_FOOT_H, id);
}

/* The foot's prompt on cream; w wide. */
static void BagPrompt(const u8 *text, int w)
{
    int oy = Plate(AREA_X, BAG_FOOT_Y, w, BAG_FOOT_H, sCreamPlate, PLATE_STATE_NORMAL);
    int lines = TextLines(text, 0, 0, w - 12, 12, 3, &sSmall, STYLE_INK, FALSE);

    TextLines(text, AREA_X + 6, oy + (BAG_FOOT_H - 4 - lines * 12) / 2, w - 12, 12, 3, &sSmall, STYLE_INK, TRUE);
}

static void DrawBagQuantity(const ViewState *s)
{
    const struct CtrBagView *v = &s->bv;
    int bx = AREA_X + 100, oy;
    u8 count[8] = {CHAR_MULT_SIGN};

    BagPrompt(v->text, 96);
    AddHit(AREA_X, BAG_FOOT_Y, 96, BAG_FOOT_H, HIT_CANCEL);
    for (int k = 0; k < 4; ++k)
    {
        u8 label[6], *end = label;

        if (k == 2)
        {
            ConvertIntToDecimalStringN(count + 1, v->count, STR_CONV_MODE_LEADING_ZEROS, v->max >= 100 ? 3 : 2);
            oy = Plate(bx, BAG_FOOT_Y, 40, BAG_FOOT_H, sLight, PLATE_STATE_NORMAL);
            TextIn(count, bx, oy, 40, BAG_FOOT_H, &sNormal, 'c', STYLE_INK, 2);
            AddHit(bx, BAG_FOOT_Y, 40, BAG_FOOT_H, HIT_OK);
            bx += 42;
        }
        *end++ = sBagSteps[k] < 0 ? CHAR_HYPHEN : CHAR_PLUS;
        ConvertIntToDecimalStringN(end, sBagSteps[k] < 0 ? -sBagSteps[k] : sBagSteps[k], STR_CONV_MODE_LEFT_ALIGN, 2);
        BagFootButton(s, bx, 28, sSlate, label, &sSmall, HIT_BAG_COUNT + k);
        bx += 30;
    }
    BagFootButton(s, bx, AREA_X + AREA_W - bx, sEmerald, Ascii("OK"), &sNormal, HIT_OK);
}

static void DrawBagYesNo(const ViewState *s)
{
    int bw = 72, bx = AREA_X + AREA_W - 2 * bw - 2;

    BagPrompt(s->bv.text, bx - AREA_X - 2);
    BagFootButton(s, bx, bw, sSlate, gText_Yes, &sNormal, HIT_YES);
    BagFootButton(s, bx + bw + 2, bw, sCancelGrey, gText_No, &sNormal, HIT_NO);
}

static void DrawBagMessage(const ViewState *s)
{
    int oy = Plate(AREA_X, BAG_FOOT_Y, AREA_W, BAG_FOOT_H, sCreamPlate, PLATE_STATE_NORMAL);

    TextLines(s->bv.msg, AREA_X + 10, oy + 2, AREA_W - 20, 13, 3, &sNormal, STYLE_INK, TRUE);
    AddHit(AREA_X, BAG_FOOT_Y, AREA_W, BAG_FOOT_H, HIT_NAT_MSG);
}

static void DrawGameBag(const ViewState *s)
{
    DrawBagPocket(s);
    DrawBagList(s);
    switch (s->bv.state)
    {
    case CTR_BAG_VIEW_ACTIONS: DrawBagActions(s); break;
    case CTR_BAG_VIEW_QUANTITY: DrawBagQuantity(s); break;
    case CTR_BAG_VIEW_YESNO: DrawBagYesNo(s); break;
    case CTR_BAG_VIEW_MESSAGE: DrawBagMessage(s); break;
    default: DrawBagDesc(s); break;
    }
}

static void BagTap(const ViewState *s, u8 id)
{
    const struct CtrBagView *v = &s->bv;

    if (id == HIT_BAG_PREV || id == HIT_BAG_NEXT)
        CtrBag_TapPocket(id == HIT_BAG_PREV ? -1 : 1);
    else if (id == HIT_BAG_DESC)
        CtrBag_CancelSwap();
    else if (id >= HIT_BAG_ENTRY && id < HIT_BAG_ENTRY + CTR_BAG_ENTRIES && id - HIT_BAG_ENTRY < v->numEntries)
        CtrMenu_PostEntry(v->entryPos[id - HIT_BAG_ENTRY]);
    else if (id >= HIT_BAG_COUNT && id < HIT_BAG_COUNT + 4)
        CtrBag_AddCount(sBagSteps[id - HIT_BAG_COUNT]);
    else if (id == HIT_OK || id == HIT_CANCEL)
        CtrBag_TapCount(id == HIT_OK);
    else if (id == HIT_YES || id == HIT_NO)
        CtrMenu_PostEntry(id == HIT_YES ? 0 : 1);
    else if (id == HIT_NAT_MSG)
        CtrMenu_PostAck();
}

/* A touch on the list's rows: the bag's own list touch at the same place of
 * its list (CtrBag_Touch), in 1/256s of a row. TRUE when it is the list's. */
static bool8 BagListTouch(const ViewState *s, u8 phase, int x, int y, bool8 start)
{
    s32 row256 = (y - BAG_LIST_TOP) * 256 / BAG_ROW_H;
    s16 gx, gy;

    if (start && (!BagListShown(&s->bv) || x < BAG_LIST_X || x >= BAG_LIST_X + BAG_LIST_W || y < AREA_Y
                  || y >= AREA_Y + BAG_LIST_H || row256 < 0 || row256 >= s->bv.numRows * 256))
        return FALSE;
    CtrBag_ListPoint(row256, &gx, &gy);
    CtrBag_Touch(phase, gx, gy);
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* POKéDEX                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * The list: the chosen mon whole in a lens on the left, the counts and
 * SEARCH under it; the rows round the chosen one on the right, in the
 * middle, each with its number, the game's caught mark, its icon whole and
 * its name. The entry: its tabs (the area, cry and size pages are the
 * game's own picture), the mon in the lens, its data and its description.
 * The list's rows and its bar, and a swipe over the entry, are the
 * Pokédex's own touch (DexTouch).
 */
enum
{
    HIT_DEX_LENS = HIT_ROW,
    HIT_DEX_SEARCH,
    HIT_DEX_TAB = HIT_ROW + 4,      /* + tab */
    HIT_DEX_MENU = HIT_ROW + 8,     /* + START menu entry */
    HIT_DEX_MENU_CLOSE = HIT_ROW + 13,
    HIT_DEX_PLAY,
    HIT_DEX_ANY,
    HIT_DEX_PARAM = HIT_ACTION,     /* + parameter * 2, + 1 on */
    HIT_DEX_GO = HIT_ACTION + 12,   /* + 0 SEARCH, 1 SHIFT */
};
#define DEX_LEFT_W 104
#define DEX_LIST_X (AREA_X + DEX_LEFT_W + 4)
#define DEX_LIST_W (AREA_W - DEX_LEFT_W - 4 - 12)
#define DEX_LIST_Y (AREA_Y + 23)
#define DEX_LIST_H (AREA_H - 23)
#define DEX_BAR_X (DEX_LIST_X + DEX_LIST_W + 2)
#define DEX_ROW_H 27
#define DEX_ROWS_Y (DEX_LIST_Y + 3)
#define DEX_TAB_H 18

/* The caught mark's own shape (caught_ball.4bpp) in a Poké Ball's colours:
 * the Pokédex's palette, where the game blits it, has magenta at its red. */
static const u16 *CaughtBallPal(void)
{
    static const u16 gba[16] = {[1] = RGB(31, 31, 31), [11] = RGB(28, 6, 4), [15] = RGB(5, 7, 8)};
    static Pal pal;
    static bool8 made;

    if (!made)
    {
        ToPals(&pal, gba, 1);
        made = TRUE;
    }
    return pal.c;
}

/* The lens: a dark ring with the Pokédex's green in it. */
static void DrawLens(int cx, int cy, int r)
{
    int r2 = r * r, in = (r - 5) * (r - 5), ring0 = (r - 13) * (r - 13), ring1 = (r - 11) * (r - 11);
    u16 dark = PackRgb(24, 32, 40);

    for (int dy = -r; dy <= r; ++dy)
    {
        int t = (dy + r) * 256 / (2 * r);
        u16 green = PackRgb(168 - (96 * t >> 8), 232 - (64 * t >> 8), 176 - (72 * t >> 8));

        for (int dx = -r; dx <= r; ++dx)
        {
            int d = dx * dx + dy * dy;

            if (d > r2)
                continue;
            Put(cx + dx, cy + dy, d > in ? dark : d >= ring0 && d < ring1 ? PackRgb(196, 240, 204) : green);
        }
    }
}

static void DrawDexPic(u16 species, int cx, int cy)
{
    int slot = species ? FrontPic(species, FALSE) : -1;

    if (slot >= 0)
        DrawSprite(sFronts[slot].tiles, 8, 8, cx - 32, cy - 34, sFronts[slot].pal.c);
}

static void DrawDexList(const ViewState *s)
{
    const struct CtrDexView *v = &s->dx;
    int x = DEX_LIST_X, w = DEX_LIST_W, oy;
    const u8 *ball = CtrPokedex_CaughtBallGfx();

    DrawLens(AREA_X + DEX_LEFT_W / 2, AREA_Y + 52, 50);
    DrawDexPic(v->species, AREA_X + DEX_LEFT_W / 2, AREA_Y + 52);
    AddHit(AREA_X, AREA_Y, DEX_LEFT_W, 104, HIT_DEX_LENS);
    oy = Plate(AREA_X, AREA_Y + 108, DEX_LEFT_W, 26, sLight, PLATE_STATE_NORMAL);
    StyledStr(&sSmall, Ascii(CTR_TEXT("SEEN", "VISTOS", "VUS", "VISTI", "VISTOS")), AREA_X + 6, oy + 1, STYLE_GRAY);
    StyledRight(&sSmall, Number(v->seen, 3, STR_CONV_MODE_LEFT_ALIGN), AREA_X + DEX_LEFT_W - 6, oy + 1, STYLE_INK);
    StyledStr(&sSmall, Ascii(CTR_TEXT("OWN", "CAPTUR.", "PRIS", "PRESI", "CAPTUR.")), AREA_X + 6, oy + 12, STYLE_GRAY);
    StyledRight(&sSmall, Number(v->own, 3, STR_CONV_MODE_LEFT_ALIGN), AREA_X + DEX_LEFT_W - 6, oy + 12, STYLE_INK);
    {
        bool8 pressed = s->pressed == HIT_DEX_SEARCH;

        oy = Plate(AREA_X, AREA_Y + 137, DEX_LEFT_W, 27, pressed ? sEmerald : sSlate,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(Ascii(CTR_TEXT("SEARCH", "BUSCAR", "CHERCHER", "CERCA", "BUSCAR")), AREA_X, oy, DEX_LEFT_W, 27, &sNormal, 'c',
               STYLE_WHITE, 4);
        AddHit(AREA_X, AREA_Y + 137, DEX_LEFT_W, 27, HIT_DEX_SEARCH);
    }
    /* The dex and its order. */
    oy = Plate(x, AREA_Y, AREA_W - DEX_LEFT_W - 4, 20, sDexRed, PLATE_STATE_NORMAL);
    TextIn(v->national ? gText_DexNatTitle : gText_DexHoennTitle, x, oy, 120, 20, &sSmall, 'l', STYLE_WHITE, 8);
    {
        static const u8 *const order[] = {gText_DexSortNumericalTitle, gText_DexSortAtoZTitle,
                                          gText_DexSortHeaviestTitle, gText_DexSortLightestTitle,
                                          gText_DexSortTallestTitle, gText_DexSortSmallestTitle};

        if (v->order < ARRAY_COUNT(order))
            TextIn(order[v->order], x + 100, oy, AREA_W - DEX_LEFT_W - 104, 20, &sSmall, 'r', STYLE_WHITE, 8);
    }
    Plate(x, DEX_LIST_Y, w, DEX_LIST_H, sCreamPlate, PLATE_STATE_NORMAL);
    /* The rows, then their icons over them: whole, as the rows are lower. */
    for (int i = 0; i < CTR_DEX_ROWS; ++i)
    {
        const struct CtrDexRow *row = &v->rows[i];
        int ry = DEX_ROWS_Y + i * DEX_ROW_H, ty;
        bool8 chosen = i == v->chosenRow;
        u8 style = chosen ? STYLE_WHITE : STYLE_INK;

        if (!row->dexNum)
            continue;
        oy = chosen ? PlateEx(x + 3, ry, w - 6, DEX_ROW_H - 1, sEmerald, PLATE_STATE_NORMAL, 0, 30, 3, 0) : ry;
        ty = TextY(oy, DEX_ROW_H - 1, &sSmall);
        StyledStr(&sSmall, row->number, x + 8, ty, style);
        if (row->owned && ball)
            DrawSprite(ball, 1, 2, x + 42, oy + (DEX_ROW_H - 1 - 16) / 2, CaughtBallPal());
        if (row->species)
            StyledStr(FitFont(&sSmall, gSpeciesNames[row->species], w - 94), gSpeciesNames[row->species], x + 88, ty, style);
        else
            StyledStr(&sSmall, Ascii("----------"), x + 88, ty, chosen ? STYLE_WHITE : STYLE_GRAY);
    }
    for (int i = 0; i < CTR_DEX_ROWS; ++i)
    {
        const struct CtrDexRow *row = &v->rows[i];
        u16 icon = row->species ? GetIconSpecies(row->species, 0) : SPECIES_NONE;
        const u8 *tiles = icon ? MonIcon(icon, FALSE) : NULL;
        u8 pal = icon ? gMonIconPaletteIndices[icon] : 0;

        /* Its drawn pixels on the row's middle, as the text is: the icons
         * stand low in their 32x32. */
        if (tiles && pal < ARRAY_COUNT(sRes.monIconPal))
            IconCentred(tiles, 4, 2 * (x + 68), 2 * (DEX_ROWS_Y + i * DEX_ROW_H) + DEX_ROW_H - 1,
                        sRes.monIconPal[pal].c, FALSE);
    }
    /* The scroll bar: where the chosen one is in the list. */
    Plate(DEX_BAR_X, DEX_LIST_Y, 10, DEX_LIST_H, sGroupOff, PLATE_STATE_NORMAL);
    if (v->count > 1)
        Plate(DEX_BAR_X, DEX_LIST_Y + (DEX_LIST_H - 30) * v->selected / (v->count - 1), 10, 30, sEmerald,
              PLATE_STATE_NORMAL);
}

/* The game's cursor on a tab: a bar under its name. */
static void DexTabCursor(int x, int oy, int w)
{
    FillRgb(x + 6, oy + DEX_TAB_H - 6, w - 12, 2, sEmerald);
}

static void DrawDexTabs(const ViewState *s)
{
    static const char *const names[][5] = {
        {"INFO", "INFO", "INFO", "INFO", "INFO"}, {"AREA", "ÁREA", "ZONE", "AREA", "ÁREA"},
        {"CRY", "GRITO", "CRI", "VERSO", "GRITO"}, {"SIZE", "TAMAÑO", "TAILLE", "ALTEZZA", "TAMANHO"}};
    int n = 4, gap = 2, tw = (AREA_W - 64 - gap * (n - 1)) / n, x = AREA_X, oy;
    u8 state = s->dx.state;
    int liveTab = state == CTR_DEX_VIEW_AREA ? 1 : state == CTR_DEX_VIEW_CRY ? 2 : state == CTR_DEX_VIEW_SIZE ? 3 : 0;
    /* On the entry the game's cursor (the D-pad's) is marked; SIZE is only
     * for a mon caught. */
    int cursor = state == CTR_DEX_VIEW_INFO ? s->dx.tabCursor : -1;
    bool8 sizeOff = !s->dx.owned;

    for (int i = 0; i < n; ++i)
    {
        bool8 live = i == liveTab, pressed = s->pressed == HIT_DEX_TAB + i, off = i == 3 && sizeOff;

        oy = Plate(x, AREA_Y, tw, DEX_TAB_H, live || pressed ? sEmerald : off ? sGroupOff : sLight,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(Ascii(CTR_TEXT(names[i][0], names[i][1], names[i][2], names[i][3], names[i][4])), x, oy, tw, DEX_TAB_H, &sSmall, 'c',
               live || pressed ? STYLE_WHITE : off ? STYLE_GRAY : STYLE_INK, 3);
        if (i == cursor && !live)
            DexTabCursor(x, oy, tw);
        if (!live && !off)
            AddHit(x, AREA_Y, tw, DEX_TAB_H, HIT_DEX_TAB + i);
        x += tw + gap;
    }
    {
        bool8 pressed = s->pressed == HIT_CANCEL;
        int w = AREA_X + AREA_W - x;

        oy = Plate(x, AREA_Y, w, DEX_TAB_H, pressed ? sEmerald : sCancelGrey,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(gText_Cancel2, x, oy, w, DEX_TAB_H, &sSmall, 'c', STYLE_WHITE, 3);
        if (cursor == 4)
            DexTabCursor(x, oy, w);
        AddHit(x, AREA_Y, w, DEX_TAB_H, HIT_CANCEL);
    }
}

/* The footprint, 16x16 of the game's 1bpp tiles (top left, top right,
 * bottom left, bottom right; a byte a row, its low bit leftmost). */
static void DrawFootprint(const u8 *bits, int x, int y)
{
    u16 ink = PackRgb(64, 56, 48);

    for (int t = 0; t < 4; ++t)
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col)
                if (bits[t * 8 + row] & (1 << col))
                    Put(x + (t & 1) * 8 + col, y + (t >> 1) * 8 + row, ink);
}

static void DrawDexInfo(const ViewState *s)
{
    const struct CtrDexView *v = &s->dx;
    int y = AREA_Y + DEX_TAB_H + 3, x = AREA_X + 104, w = AREA_W - 104, oy;

    DrawDexTabs(s);
    DrawLens(AREA_X + 50, y + 48, 46);
    DrawDexPic(v->species, AREA_X + 50, y + 48);
    oy = Plate(x, y, w, 96, sCreamPlate, PLATE_STATE_NORMAL);
    StyledStr(&sSmall, v->number, x + 8, oy + 5, STYLE_GRAY);
    if (v->species)
        StyledStr(FitFont(&sNormal, gSpeciesNames[v->species], w - 54), gSpeciesNames[v->species], x + 44, oy + 2, STYLE_INK);
    StyledStr(FitFont(&sSmall, v->category, w - 16), v->category, x + 8, oy + 22, STYLE_GREEN);
    StyledStr(&sSmall, gText_HTHeight, x + 8, oy + 42, STYLE_GRAY);
    StyledStr(&sNormal, v->height, x + 60, oy + 39, STYLE_INK);
    StyledStr(&sSmall, gText_WTWeight, x + 8, oy + 60, STYLE_GRAY);
    StyledStr(&sNormal, v->weight, x + 60, oy + 57, STYLE_INK);
    if (v->owned)
    {
        DrawFootprint(v->footprint, x + w - 34, oy + 46);
        DrawTypeIcon(gSpeciesInfo[v->species].types[0], x + 8, oy + 76);
        if (gSpeciesInfo[v->species].types[1] != gSpeciesInfo[v->species].types[0])
            DrawTypeIcon(gSpeciesInfo[v->species].types[1], x + 44, oy + 76);
    }
    oy = Plate(AREA_X, y + 100, AREA_W, AREA_Y + AREA_H - y - 100, sLight, PLATE_STATE_NORMAL);
    if (v->description)
        TextLines(v->description, AREA_X + 10, oy + 2, AREA_W - 20, 13, 3, &sNormal, STYLE_INK, TRUE);
}

/* The list's START menu over the list, as the game's slides up over it: its
 * entries as plates, the cursor's dotted; a tap beside it closes it. */
static void DrawDexMenu(const ViewState *s)
{
    static const char *const names[5][5] = {
        {"BACK TO LIST", "VOLVER A LISTA", "RETOUR LISTE", "TORNA ALLA LISTA", "VOLTAR À LISTA"},
        {"LIST TOP", "PRINCIPIO", "HAUT DE LISTE", "INIZIO LISTA", "INÍCIO DA LISTA"},
        {"LIST BOTTOM", "FINAL", "BAS DE LISTE", "FINE LISTA", "FIM DA LISTA"},
        {"BACK TO POKéDEX", "VOLVER A POKéDEX", "RETOUR POKéDEX", "TORNA AL POKéDEX", "VOLTAR À POKéDEX"},
        {"CLOSE POKéDEX", "CERRAR POKéDEX", "FERMER POKéDEX", "CHIUDI POKéDEX", "FECHAR POKéDEX"},
    };
    const struct CtrDexView *v = &s->dx;
    int n = v->menuCount <= 5 ? v->menuCount : 5, pitch = 26, h = 23;
    int x = DEX_LIST_X + 6, w = AREA_X + AREA_W - x - 6, y0 = DEX_LIST_Y + (DEX_LIST_H - n * pitch) / 2, oy;

    DrawDexList(s);
    /* Beside the menu: it closes. The entries over it. */
    AddHit(AREA_X, AREA_Y, AREA_W, AREA_H, HIT_DEX_MENU_CLOSE);
    Plate(x - 4, y0 - 5, w + 8, n * pitch + 7, sDexRed, PLATE_STATE_NORMAL);
    for (int i = 0; i < n; ++i)
    {
        /* The search results' menu has BACK TO POKéDEX before CLOSE. */
        int name = n == 4 && i == 3 ? 4 : i;
        bool8 pressed = s->pressed == HIT_DEX_MENU + i;
        int by = y0 + i * pitch;
        Rgb colour = i == n - 1 ? sCancelGrey : sSlate;

        oy = PlateEx(x, by, w, h, pressed ? sEmerald : colour, pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL,
                     30, 30, PlateCut(h), 0);
        EntryDot(x + 14, oy + h / 2, i == v->menuCursor ? sEmerald : colour);
        TextIn(Ascii(CTR_TEXT(names[name][0], names[name][1], names[name][2], names[name][3], names[name][4])), x + 30, oy, w - 32, h,
               FitFont(&sNormal, Ascii(CTR_TEXT(names[name][0], names[name][1], names[name][2], names[name][3], names[name][4])), w - 40),
               'l', STYLE_WHITE, 4);
        AddHit(x, by, w, h, HIT_DEX_MENU + i);
    }
}

/* The search: a row a parameter with its value between arrows (the left
 * half of the row one back, the right one on), SEARCH, SHIFT and CANCEL
 * under them; searching, and its result, over them. */
static void DrawDexSearch(const ViewState *s)
{
    static const char *const labels[CTR_DEX_PARAMS][5] = {
        {"NAME", "NOMBRE", "NOM", "NOME", "NOME"}, {"COLOR", "COLOR", "COULEUR", "COLORE", "COR"},
        {"TYPE 1", "TIPO 1", "TYPE 1", "TIPO 1", "TIPO 1"}, {"TYPE 2", "TIPO 2", "TYPE 2", "TIPO 2", "TIPO 2"},
        {"ORDER", "ORDEN", "ORDRE", "ORDINE", "ORDEM"}, {"MODE", "MODO", "MODE", "MODO", "MODO"},
    };
    const struct CtrDexView *v = &s->dx;
    int n = 0, rowH, panelH = AREA_H - 34, oy, i, row = 0;
    u16 green = sStyle[STYLE_GREEN].fg;

    for (i = 0; i < CTR_DEX_PARAMS; ++i)
        n += v->paramCount[i] != 0;
    if (n == 0)
        n = 1;
    rowH = (panelH - 6) / n;
    oy = Plate(AREA_X, AREA_Y, AREA_W, panelH, sCreamPlate, PLATE_STATE_NORMAL);
    for (i = 0; i < CTR_DEX_PARAMS; ++i)
    {
        int ry = oy + 3 + row * rowH, left = AREA_X + 130, wv = AREA_X + AREA_W - 12 - left;
        bool8 focus = v->searchMenu && v->menuItem == i;
        bool8 pressed = s->pressed == HIT_DEX_PARAM + i * 2 || s->pressed == HIT_DEX_PARAM + i * 2 + 1;

        if (!v->paramCount[i])
            continue;
        if (row)
            FillRgb(AREA_X + 8, ry, AREA_W - 16, 1, sRule);
        if (pressed || focus)
            FillRgb(AREA_X + 4, ry + 1, AREA_W - 8, rowH - 1, sGroupOff);
        TextIn(Ascii(CTR_TEXT(labels[i][0], labels[i][1], labels[i][2], labels[i][3], labels[i][4])), AREA_X + 6, ry, 120, rowH,
               &sSmall, 'l', focus ? STYLE_GREEN : STYLE_INK, 6);
        if (v->paramText[i])
            StyledCentre(FitFont(&sSmall, v->paramText[i], wv - 24), v->paramText[i], left + wv / 2,
                         TextY(ry, rowH, &sSmall), STYLE_GREEN);
        DrawArrow(left + 1, ry + rowH / 2 - 3, -1, green);
        DrawArrow(left + wv - 5, ry + rowH / 2 - 3, 1, green);
        if (v->searchState == CTR_DEX_SEARCH_CHOOSE)
        {
            AddHit(AREA_X, ry, left + wv / 2 - AREA_X, rowH, HIT_DEX_PARAM + i * 2);
            AddHit(left + wv / 2, ry, AREA_X + AREA_W - left - wv / 2, rowH, HIT_DEX_PARAM + i * 2 + 1);
        }
        row++;
    }
    /* SEARCH, SHIFT, CANCEL. */
    {
        static const char *const names[3][5] = {
            {"SEARCH", "BUSCAR", "CHERCHER", "CERCA", "BUSCAR"}, {"SHIFT", "ORDENAR", "TRIER", "ORDINA", "ORDENAR"},
            {"CANCEL", "SALIR", "RETOUR", "ESCI", "SAIR"},
        };
        int by = AREA_Y + AREA_H - 30, bw = (AREA_W - 8) / 3;

        for (i = 0; i < 3; ++i)
        {
            u8 id = i == 2 ? HIT_CANCEL : HIT_DEX_GO + i;
            bool8 pressed = s->pressed == id;
            int bx = AREA_X + i * (bw + 4);
            Rgb colour = i == 0 ? sRedPlate : i == 1 ? sSlate : sCancelGrey;

            oy = Plate(bx, by, bw, 30, pressed ? sEmerald : colour, pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
            TextIn(Ascii(CTR_TEXT(names[i][0], names[i][1], names[i][2], names[i][3], names[i][4])), bx, oy, bw, 30, &sNormal, 'c',
                   STYLE_WHITE, 4);
            if (v->searchState == CTR_DEX_SEARCH_CHOOSE)
                AddHit(bx, by, bw, 30, id);
        }
    }
    /* Searching, then what it found: a tap goes on. */
    if (v->searchState != CTR_DEX_SEARCH_CHOOSE)
    {
        const u8 *text = v->searchState == CTR_DEX_SEARCH_BUSY ? gText_SearchingPleaseWait
                       : v->searchState == CTR_DEX_SEARCH_FOUND ? gText_SearchCompleted : gText_NoMatchingPkmnWereFound;
        int w = AREA_W - 60, h = 44, x = AREA_X + 30, y = AREA_Y + (AREA_H - h) / 2;

        oy = Plate(x, y, w, h, sLight, PLATE_STATE_NORMAL);
        TextLines(text, x + 10, oy + 6, w - 20, 15, 2, &sNormal, STYLE_INK, TRUE);
        if (v->searchState != CTR_DEX_SEARCH_BUSY)
            AddHit(AREA_X, AREA_Y, AREA_W, AREA_H, HIT_DEX_ANY);
    }
}

/* A pixel of the canvas mixed with a colour, alpha of 256. */
static void MixPut(int x, int y, Rgb c, int alpha)
{
    u16 v;
    int r, g, b;

    x += sOX;
    if (x < sClipX0 || x >= sClipX1 || y < sClipY0 || y >= sClipY1)
        return;
    v = sDst[x * H + (H - 1 - y)];
    r = (v >> 11) << 3;
    g = ((v >> 5) & 63) << 2;
    b = (v & 31) << 3;
    r += (c.r - r) * alpha >> 8;
    g += (c.g - g) * alpha >> 8;
    b += (c.b - b) * alpha >> 8;
    sDst[x * H + (H - 1 - y)] = PackRgb(r, g, b);
}

#define DEX_AREA_CROP 12

static bool8 DexAreaCell(const struct CtrDexView *v, int cx, int cy)
{
    return cx >= 0 && cx < 32 && cy >= 0 && cy < CTR_DEX_AREA_ROWS && (v->areaRows[cy] >> cx) & 1;
}

/* The area page: the region map in its window under the tabs, the cells
 * where the mon lives lit (the game's glow), each place marked with a dot. */
static void DrawDexArea(const ViewState *s)
{
    const struct CtrDexView *v = &s->dx;
    int wy = AREA_Y + DEX_TAB_H + 3, wh = AREA_Y + AREA_H - wy, ox = MAP_ORIGIN_X, oy = wy + 2 - DEX_AREA_CROP;
    int saved[4];
    Rgb glow = {255, 208, 48}, edge = {232, 96, 32};

    DrawDexTabs(s);
    DrawChamferFrame(MAP_WIN_X, wy, MAP_WIN_W, wh, 4, 2);
    ClipPush(saved, MAP_ORIGIN_X, wy + 2, MAP_ORIGIN_X + MAP_PIC_W, wy + wh - 2);
    FillRect(MAP_ORIGIN_X, wy + 2, MAP_PIC_W, wh - 4, sRes.mapPal[0]);
    for (int ty = DEX_AREA_CROP / 8; ty < MAP_PIC_H / 8; ++ty)
        for (int tx = 0; tx < MAP_PIC_W / 8; ++tx)
            DrawMapTile8(sRes.mapMap ? sRes.mapMap[ty * 64 + tx] : 0, ox + tx * 8, oy + ty * 8);
    for (int cy = 0; cy < CTR_DEX_AREA_ROWS; ++cy)
        for (int cx = 0; cx < 32; ++cx)
        {
            int px = ox + cx * 8, py = oy + cy * 8;

            if (!DexAreaCell(v, cx, cy))
                continue;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    MixPut(px + x, py + y, glow, 128);
            if (!DexAreaCell(v, cx - 1, cy)) FillRect(px, py, 1, 8, PackRgb(edge.r, edge.g, edge.b));
            if (!DexAreaCell(v, cx + 1, cy)) FillRect(px + 7, py, 1, 8, PackRgb(edge.r, edge.g, edge.b));
            if (!DexAreaCell(v, cx, cy - 1)) FillRect(px, py, 8, 1, PackRgb(edge.r, edge.g, edge.b));
            if (!DexAreaCell(v, cx, cy + 1)) FillRect(px, py + 7, 8, 1, PackRgb(edge.r, edge.g, edge.b));
        }
    for (int i = 0; i < v->marks && i < CTR_DEX_AREA_MARKS; ++i)
    {
        int cx = ox + v->markX[i] * 8 + 4, cy = oy + v->markY[i] * 8 + 4;

        for (int dy = -5; dy <= 5; ++dy)
            for (int dx = -5; dx <= 5; ++dx)
            {
                int d = dx * dx + dy * dy;

                if (d <= 25)
                    Put(cx + dx, cy + dy, d > 12 ? PackRgb(255, 255, 255) : PackRgb(edge.r, edge.g, edge.b));
            }
    }
    RestoreClip(saved);
    /* Its name, and that its area is not known. */
    {
        int pw = v->areaUnknown ? 230 : 140, ph = 18, px = MAP_ORIGIN_X + 4, py = wy + wh - ph - 6, o;
        u8 text[40];

        o = Plate(px, py, pw, ph, sLight, PLATE_STATE_NORMAL);
        StringCopy(text, v->species ? gSpeciesNames[v->species] : gText_DexEmptyString);
        if (v->areaUnknown)
        {
            StringAppend(text, Ascii(": "));
            StringAppend(text, Ascii(CTR_TEXT("AREA UNKNOWN", "ZONA DESCONOCIDA", "ZONE INCONNUE", "ZONA SCONOSC.", "ÁREA DESCONHECIDA")));
        }
        TextIn(text, px, o, pw, ph, FitFont(&sSmall, text, pw - 12), 'l', STYLE_INK, 6);
    }
}

/* The cry page: the mon in the lens, PLAY under it; the waveform as it is
 * drawn on a dark panel, the meter's level under it. */
static void DrawDexCry(const ViewState *s)
{
    const struct CtrDexView *v = &s->dx;
    int y = AREA_Y + DEX_TAB_H + 3, x = AREA_X + 104, w = AREA_W - 104, h = 96, oy, mid;

    DrawDexTabs(s);
    DrawLens(AREA_X + 50, y + 48, 46);
    DrawDexPic(v->species, AREA_X + 50, y + 48);
    {
        bool8 pressed = s->pressed == HIT_DEX_PLAY;
        int by = AREA_Y + AREA_H - 40;

        oy = Plate(AREA_X, by, 100, 40, pressed ? sEmerald : sSlate, pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(Ascii(CTR_TEXT("PLAY CRY", "OÍR GRITO", "ÉCOUTER", "ASCOLTA", "OUVIR GRITO")), AREA_X, oy, 100, 40, &sNormal, 'c',
               STYLE_WHITE, 4);
        AddHit(AREA_X, by, 100, 40, HIT_DEX_PLAY);
        AddHit(AREA_X, y, 100, 96, HIT_DEX_PLAY);
    }
    oy = Plate(x, y, w, h, (Rgb){32, 44, 52}, PLATE_STATE_NORMAL);
    mid = oy + h / 2 - 2;
    FillRect(x + 8, mid, w - 16, 1, PackRgb(48, 96, 88));
    /* A cry seldom reaches a quarter of full scale: drawn at twice the
     * game's height, mirrored about the line. */
    for (int i = 0; i < CTR_DEX_WAVE; ++i)
    {
        int bx = x + 8 + i * (w - 16) / CTR_DEX_WAVE, a = v->wave[i] * (h / 2 - 10) / 64;

        if (a < 0)
            a = -a;
        if (a > h / 2 - 10)
            a = h / 2 - 10;
        FillRect(bx, mid - a, 2, 2 * a + 1, PackRgb(96, 224, 176));
    }
    oy = Plate(x, y + h + 4, w, AREA_Y + AREA_H - y - h - 4, sLight, PLATE_STATE_NORMAL);
    {
        int ph = AREA_Y + AREA_H - y - h - 4, sw = 12, sx = x + w - 10 * (sw + 2) - 8;

        TextIn(Ascii(CTR_TEXT("VOLUME", "VOLUMEN", "VOLUME", "VOLUME", "VOLUME")), x + 6, oy, sx - x - 10, ph, &sSmall, 'l',
               STYLE_GRAY, 4);
        for (int i = 0; i < 10; ++i)
            FillRect(sx + i * (sw + 2), oy + 8, sw, ph - 18,
                     i < v->volume ? PackRgb(88, 192, 128) : PackRgb(208, 228, 214));
    }
}

/* A 64x64 front picture (8x8 tiles, 4bpp) as a silhouette at the game's
 * affine scale (256 is 1:1, more is smaller) about (cx, cy). */
static void Silhouette(const u8 *tiles, int cx, int cy, int scale, u16 ink)
{
    if (!tiles || scale <= 0)
        return;
    for (int dy = -32; dy < 32; ++dy)
        for (int dx = -32; dx < 32; ++dx)
        {
            int sx = 32 + dx * scale / 256, sy = 32 + dy * scale / 256;
            u8 byte;

            if (sx < 0 || sx >= 64 || sy < 0 || sy >= 64)
                continue;
            byte = tiles[((sy / 8) * 8 + sx / 8) * 32 + (sy % 8) * 4 + (sx % 8) / 2];
            if ((sx & 1 ? byte >> 4 : byte & 15) != 0)
                Put(cx + dx, cy + dy, ink);
        }
}

/* The size page: the trainer and the mon side by side as the game's
 * silhouettes on a squared cream plate, their names under them. */
static void DrawDexSize(const ViewState *s)
{
    const struct CtrDexView *v = &s->dx;
    int y = AREA_Y + DEX_TAB_H + 3, h = AREA_Y + AREA_H - y, oy, cy, tx = AREA_X + AREA_W / 2 - 60,
        mx = AREA_X + AREA_W / 2 + 60;
    int slot = v->species ? FrontPic(v->species, FALSE) : -1;
    u16 ink = PackRgb(56, 64, 64);

    DrawDexTabs(s);
    oy = Plate(AREA_X, y, AREA_W, h, sCreamPlate, PLATE_STATE_NORMAL);
    for (int gx = AREA_X + 12; gx < AREA_X + AREA_W - 8; gx += 12)
        FillRect(gx, oy + 4, 1, h - 34, PackRgb(232, 222, 206));
    for (int gy = oy + 4; gy < oy + h - 30; gy += 12)
        FillRect(AREA_X + 6, gy, AREA_W - 12, 1, PackRgb(232, 222, 206));
    cy = oy + 4 + 48;
    FillRect(AREA_X + 6, cy + 32, AREA_W - 12, 2, PackRgb(176, 160, 136));
    if (sRes.trainerPic[v->gender ? 1 : 0])
        Silhouette(sRes.trainerPic[v->gender ? 1 : 0], tx, cy + v->trainerOffset, v->trainerScale, ink);
    if (slot >= 0)
        Silhouette(sFronts[slot].tiles, mx, cy + v->monOffset, v->monScale, ink);
    StyledCentre(&sSmall, s->name, tx, oy + h - 22, STYLE_INK);
    if (v->species)
    {
        u8 text[POKEMON_NAME_LENGTH + 2 + sizeof(v->height)];
        int n;

        /* The height as the entry has it, bounded to its field. */
        StringCopy(text, gSpeciesNames[v->species]);
        n = StringLength(text);
        text[n++] = CHAR_SPACE;
        for (int i = 0; i < (int)sizeof(v->height) && v->height[i] != EOS; ++i)
            text[n++] = v->height[i];
        text[n] = EOS;
        StyledCentre(FitFont(&sSmall, text, 150), text, mx, oy + h - 22, STYLE_INK);
    }
}

static void DrawGameDex(const ViewState *s)
{
    switch (s->dx.state)
    {
    case CTR_DEX_VIEW_INFO: DrawDexInfo(s); break;
    case CTR_DEX_VIEW_MENU: DrawDexMenu(s); break;
    case CTR_DEX_VIEW_SEARCH: DrawDexSearch(s); break;
    case CTR_DEX_VIEW_AREA: DrawDexArea(s); break;
    case CTR_DEX_VIEW_CRY: DrawDexCry(s); break;
    case CTR_DEX_VIEW_SIZE: DrawDexSize(s); break;
    default: DrawDexList(s); break;
    }
}

static void DexTap(const ViewState *s, u8 id)
{
    (void)s;
    if (id >= HIT_DEX_MENU && id < HIT_DEX_MENU + 5)
        CtrPokedex_NativeTap(CTR_DEX_TAP_MENU, id - HIT_DEX_MENU);
    else if (id == HIT_DEX_MENU_CLOSE)
        CtrPokedex_NativeTap(CTR_DEX_TAP_MENU, 0xFF);
    else if (id >= HIT_DEX_PARAM && id < HIT_DEX_PARAM + CTR_DEX_PARAMS * 2)
        CtrPokedex_NativeTap(CTR_DEX_TAP_PARAM, id - HIT_DEX_PARAM);
    else if (id == HIT_DEX_GO || id == HIT_DEX_GO + 1)
        CtrPokedex_NativeTap(CTR_DEX_TAP_SEARCH_GO, id - HIT_DEX_GO);
    else if (id == HIT_DEX_ANY)
        CtrPokedex_NativeTap(CTR_DEX_TAP_ANY, 0);
    else if (id == HIT_DEX_PLAY)
        CtrPokedex_NativeTap(CTR_DEX_TAP_PLAY, 0);
    else if (id == HIT_DEX_LENS)
        CtrPokedex_NativeTap(CTR_DEX_TAP_CHOOSE, 0);
    else if (id == HIT_DEX_SEARCH)
        CtrPokedex_NativeTap(CTR_DEX_TAP_SEARCH, 0);
    else if (id >= HIT_DEX_TAB && id < HIT_DEX_TAB + 4)
        CtrPokedex_NativeTap(CTR_DEX_TAP_TAB, id - HIT_DEX_TAB);
    else if (id == HIT_CANCEL)
        CtrPokedex_NativeTap(CTR_DEX_TAP_BACK, 0);
}

/* A touch the Pokédex takes as it goes: on the list's rows (a tap on a row,
 * a drag scrolling), on its bar (a tap there), or over the entry (a swipe).
 * start: the touch beginning, TRUE when it is one of those. */
static bool8 DexTouch(const ViewState *s, u8 phase, int x, int y, bool8 start)
{
    enum { DEX_TOUCH_ROWS = 1, DEX_TOUCH_BAR, DEX_TOUCH_ENTRY };
    static u8 sKind;
    static int sY0, sY, sMiddle;
    const struct CtrDexView *v = &s->dx;

    if (start)
    {
        sKind = 0;
        if (v->state == CTR_DEX_VIEW_LIST && y >= DEX_LIST_Y && y < DEX_LIST_Y + DEX_LIST_H)
        {
            if (x >= DEX_LIST_X && x < DEX_BAR_X)
                sKind = DEX_TOUCH_ROWS;
            else if (x >= DEX_BAR_X && x < AREA_X + AREA_W)
                sKind = DEX_TOUCH_BAR;
        }
        else if (v->state == CTR_DEX_VIEW_INFO && y >= AREA_Y + DEX_TAB_H + 3 && y < AREA_Y + AREA_H
                 && x >= AREA_X && x < AREA_X + AREA_W)
            sKind = DEX_TOUCH_ENTRY;
        if (!sKind)
            return FALSE;
        sY0 = y;
        /* The chosen row's middle as the touch began: the rows are counted
         * from it until the touch ends, the chosen row moving or not. */
        sMiddle = DEX_ROWS_Y + v->chosenRow * DEX_ROW_H + DEX_ROW_H / 2;
    }
    if (phase != BAG_TOUCH_UP)
        sY = y;
    switch (sKind)
    {
    case DEX_TOUCH_ROWS:
        CtrPokedex_ListTouch(phase, (s32)(sY - sMiddle) * 256 / DEX_ROW_H);
        break;
    case DEX_TOUCH_ENTRY:
        CtrPokedex_EntryTouch(phase, sY - sY0);
        break;
    case DEX_TOUCH_BAR:
        if (phase == BAG_TOUCH_UP)
            CtrPokedex_TapBar((s32)(sY - DEX_LIST_Y) * 256 / DEX_LIST_H);
        break;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Dispatch                                                                 */
/* ------------------------------------------------------------------------ */

/* A section on its way (its menu opening, or another one closing first):
 * its dock icon in the middle of the plain backdrop, drawn once. */
static void DrawSectionWait(const ViewState *s)
{
    const Icon *icon;

    if (s->screen >= SCR_COUNT)
        return;
    icon = &sRes.column[s->screen];
    IconCentred(icon->tiles, icon->size, 2 * (AREA_X + AREA_W / 2), 2 * (AREA_Y + AREA_H / 2), icon->pal.c, TRUE);
}

static void DrawGameView(const ViewState *s)
{
    switch (s->native)
    {
    case NATIVE_PARTY: DrawGameParty(s); break;
    case NATIVE_SUMMARY: DrawGameSummary(s); break;
    case NATIVE_BAG: DrawGameBag(s); break;
    case NATIVE_DEX: DrawGameDex(s); break;
    case NATIVE_BLANK: DrawSectionWait(s); break;   /* not there yet */
    }
}

static void GameViewTap(const ViewState *s, u8 id)
{
    switch (s->native)
    {
    case NATIVE_PARTY: PartyTap(s, id); break;
    case NATIVE_SUMMARY: SummaryTap(s, id); break;
    case NATIVE_BAG: BagTap(s, id); break;
    case NATIVE_DEX: DexTap(s, id); break;
    }
}
