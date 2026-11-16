/* Part of 3ds_bottom_ui.c: the Rental Pokemon screens (included after 3ds_bottom_easychat.inc). */

/*
 * Choosing three of six (the Pokemon as plates with their icon, name, types
 * and the order they were chosen in; SUMMARY and RENT / DESELECT) and, after a
 * win, swapping one (the three of the team, or of the beaten team, and the
 * game's own buttons). The question the game asks is a small dialog. Every
 * label is the game's own string; a tap is carried out as the keys the game
 * reads (CtrFactory_Act).
 */
enum
{
    FAC_HIT_MON = HIT_ROW,      /* + Pokemon */
    FAC_HIT_OPTION = HIT_ACTION, /* + the popup's option */
    FAC_HIT_BUTTON = HIT_ACTION + 8, /* + swapping: the button after the Pokemon */
};

#define FAC_HEAD_Y 14
#define FAC_HEAD_H 26
#define FAC_MON_Y 44
#define FAC_MON_W 98
#define FAC_MON_H 72
#define FAC_MON_STEP_Y 76
#define FAC_BUTTON_Y 200
#define FAC_BUTTON_H 22

static void FacMonIcon(u16 species, u16 personality, int cx, int y)
{
    u16 iconSpecies = GetIconSpecies(species, personality);
    const u8 *tiles = MonIcon(iconSpecies, FALSE);
    u8 pal = gMonIconPaletteIndices[iconSpecies];

    if (tiles && pal < ARRAY_COUNT(sRes.monIconPal))
        DrawSprite(tiles, 4, 4, cx - 16, y, sRes.monIconPal[pal].c);
}

static void DrawFacMon(const ViewState *s, int i, int x, int y)
{
    const struct CtrFactoryView *v = &s->fac;
    u16 species = v->species[i];
    bool8 focus = v->phase != CTR_FAC_BUSY && v->cursor == i;
    int oy = Plate(x, y, FAC_MON_W, FAC_MON_H, sCreamPlate,
                   s->pressed == FAC_HIT_MON + i ? PLATE_STATE_PRESSED : focus ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
    u8 first = gSpeciesInfo[species].types[0], second = gSpeciesInfo[species].types[1];

    FacMonIcon(species, v->personality[i], x + FAC_MON_W / 2, oy + 6);
    TextIn(gSpeciesNames[species], x, oy + 38, FAC_MON_W, 12, &sSmall, 'c', STYLE_INK, 3);
    if (first == second)
        DrawTypeIcon(first, x + FAC_MON_W / 2 - 16, oy + 51);
    else
    {
        DrawTypeIcon(first, x + FAC_MON_W / 2 - 34, oy + 51);
        DrawTypeIcon(second, x + FAC_MON_W / 2 + 2, oy + 51);
    }
    if (v->order[i])
    {
        static u8 number[4];

        FillRect(x + FAC_MON_W - 22, oy + 5, 14, 14, PackRgb(28, 28, 24));
        FillRect(x + FAC_MON_W - 21, oy + 6, 12, 12, PackRgb(47, 143, 91));
        ConvertIntToDecimalStringN(number, v->order[i], STR_CONV_MODE_LEFT_ALIGN, 1);
        TextIn(number, x + FAC_MON_W - 22, oy + 5, 14, 14, &sSmall, 'c', STYLE_WHITE, 0);
    }
    AddHit(x, y, FAC_MON_W, FAC_MON_H, FAC_HIT_MON + i);
}

static void FacButton(const ViewState *s, int x, int w, u8 hit, Rgb colour, const u8 *text, bool8 focus)
{
    int oy = Plate(x, FAC_BUTTON_Y, w, FAC_BUTTON_H, colour,
                   s->pressed == hit ? PLATE_STATE_PRESSED : focus ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);

    TextIn(text, x, oy, w, FAC_BUTTON_H, &sSmall, 'c', colour.r == sCreamPlate.r ? STYLE_INK : STYLE_WHITE, 3);
    AddHit(x, FAC_BUTTON_Y, w, FAC_BUTTON_H, hit);
}

static void DrawFacQuestion(const ViewState *s)
{
    const struct CtrFactoryView *v = &s->fac;
    int oy = Plate(40, 70, 240, 104, sCreamPlate, PLATE_STATE_NORMAL);
    Rgb green = {88, 192, 128};

    TextLines(v->question, 52, oy + 12, 216, 14, 3, &sNormal, STYLE_INK, TRUE);
    {
        int y = oy + 56;
        int by = Plate(56, y, 100, 24, green, s->pressed == HIT_YES ? PLATE_STATE_PRESSED
                                                 : v->yesNoCursor == 0 ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);

        TextIn(gText_Yes2, 56, by, 100, 24, &sSmall, 'c', STYLE_WHITE, 3);
        AddHit(56, y, 100, 24, HIT_YES);
        by = Plate(164, y, 100, 24, sCreamPlate, s->pressed == HIT_NO ? PLATE_STATE_PRESSED
                                                  : v->yesNoCursor == 1 ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL);
        TextIn(gText_No2, 164, by, 100, 24, &sSmall, 'c', STYLE_INK, 3);
        AddHit(164, y, 100, 24, HIT_NO);
    }
}

static void DrawFactory(const ViewState *s)
{
    const struct CtrFactoryView *v = &s->fac;
    Rgb green = {88, 192, 128}, cream = {252, 245, 234};
    int oy = Plate(8, FAC_HEAD_Y, 304, FAC_HEAD_H, sCreamPlate, PLATE_STATE_NORMAL);
    bool8 menu = v->phase == CTR_FAC_MENU;

    NameBackdrop();
    TextIn(v->screen == CTR_FAC_SELECT ? gText_RentalPkmn2 : gText_PkmnSwap, 8, oy, 304, FAC_HEAD_H, &sSmall, 'l', STYLE_INK, 10);
    if (v->screen == CTR_FAC_SELECT)
    {
        static u8 count[8];

        ConvertIntToDecimalStringN(count, v->chosen, STR_CONV_MODE_LEFT_ALIGN, 1);
        StringAppend(count, Ascii(" / 3"));
        TextIn(count, 8, oy, 304, FAC_HEAD_H, &sSmall, 'r', STYLE_GREEN, 10);
    }
    for (int i = 0; i < v->count; ++i)
    {
        int x = 8 + (i % 3) * 102;

        DrawFacMon(s, i, x, v->screen == CTR_FAC_SELECT ? FAC_MON_Y + (i / 3) * FAC_MON_STEP_Y : FAC_MON_Y + 32);
    }
    if (v->phase == CTR_FAC_YESNO)
    {
        DrawFacQuestion(s);
        return;
    }
    if (v->screen == CTR_FAC_SELECT)
    {
        FacButton(s, 8, 98, FAC_HIT_OPTION + 0, cream, gText_Summary, menu && v->menuCursor == 0);
        FacButton(s, 110, 202, FAC_HIT_OPTION + 1, green, v->cursorChosen ? gText_Deselect : gText_Rent,
                  menu && v->menuCursor == 1);
    }
    else if (menu)
    {
        FacButton(s, 8, 98, FAC_HIT_OPTION + 0, cream, gText_Summary2, v->menuCursor == 0);
        FacButton(s, 110, 98, FAC_HIT_OPTION + 1, green, gText_Swap, v->menuCursor == 1);
        FacButton(s, 212, 100, FAC_HIT_OPTION + 2, cream, gText_Rechoose, v->menuCursor == 2);
    }
    else
    {
        /* The game's buttons: CANCEL, and while choosing the new one PKMN FOR SWAP before it. */
        for (int n = 0; n < v->buttons; ++n)
        {
            bool8 cancel = n == v->buttons - 1;
            int w = v->buttons == 1 ? 304 : 150;

            FacButton(s, 8 + n * 154, w, FAC_HIT_BUTTON + n, cancel ? cream : green, cancel ? gText_Cancel3 : gText_PkmnForSwap,
                      v->cursor == FRONTIER_PARTY_SIZE + n);
        }
    }
}

static void FactoryTap(u8 id)
{
    const struct CtrFactoryView *v = &sShown.fac;

    if (id >= FAC_HIT_MON && id < FAC_HIT_MON + CTR_FAC_MONS)
        CtrFactory_Act(v->screen == CTR_FAC_SELECT ? CTR_FAC_ACT_CURSOR : CTR_FAC_ACT_SLOT, id - FAC_HIT_MON);
    else if (id >= FAC_HIT_OPTION && id < FAC_HIT_OPTION + 3)
        CtrFactory_Act(CTR_FAC_ACT_OPTION, id - FAC_HIT_OPTION);
    else if (id >= FAC_HIT_BUTTON && id < FAC_HIT_BUTTON + 2)
        CtrFactory_Act(CTR_FAC_ACT_SLOT, FRONTIER_PARTY_SIZE + (id - FAC_HIT_BUTTON));
    else if (id == HIT_YES)
        CtrFactory_Act(CTR_FAC_ACT_YES, 0);
    else if (id == HIT_NO)
        CtrFactory_Act(CTR_FAC_ACT_NO, 0);
}
