/* Part of 3ds_bottom_ui.c: the saved game's trainer card on the top screen while the main menu is up (included after 3ds_bottom_factory_top.inc). */

/*
 * With a saved game to continue, the main menu's top screen showed the game's
 * windows: CONTINUE with the player, the time, the Pokedex and the badges,
 * then NEW GAME and OPTION, the very entries the bottom screen has as its
 * buttons. The top screen shows that game's trainer card instead, in the
 * bottom screen's style, on the menu's backdrop made 400 wide: where the game
 * goes on and CONTINUE, the card's rows, the trainer at twice his size, the
 * badges (3ds_top_panel.h). The entries stay the bottom screen's.
 *
 * main_menu.c says when (CtrMainMenu_CardUp, 0095-main-menu-top-card.patch):
 * from the menu's first highlight until its fade out; the card fades with the
 * game's picture, to black or to white (CtrTopPanel_Keep, CtrTopPanel_KeepWhite).
 */
#define TTOP_X 14
#define TTOP_W (FTOP_W - 2 * TTOP_X)
#define TTOP_BAR_Y 8
#define TTOP_BAR_H 26
#define TTOP_GO_W 132
#define TTOP_CARD_Y 40
#define TTOP_CARD_H 158
#define TTOP_ROW 25
#define TTOP_BAND_Y 204

/* The card is in the panel's canvas (drawn once as the menu comes up). */
static bool8 sTitleTopShown;

/* The panel follows the game's fade: to black as A or the options leave the
 * menu, to white as B goes back to the title screen. */
static void TitleTopKeep(void)
{
    unsigned y = gPaletteFade.y > 16 ? 16 : gPaletteFade.y;

    if (gPaletteFade.mode == 0 && gPaletteFade.blendColor == (RGB_WHITE & 0x7FFF))
        CtrTopPanel_KeepWhite(y);
    else if (gPaletteFade.mode == 0 && gPaletteFade.blendColor == RGB_BLACK)
        CtrTopPanel_Keep(y);
    else
        CtrTopPanel_Keep(0);
}

static void TitleTopRow(int i, int oy, const u8 *label, const u8 *value)
{
    int ry = oy + 8 + i * TTOP_ROW, w = TTOP_W - 132;

    if (i)
        FillRgb(TTOP_X + 10, ry - 2, w - 6, 1, sRule);
    StyledStr(FitFont(&sSmall, label, w / 2), label, TTOP_X + 14, TextY(ry, 22, &sSmall), STYLE_GRAY);
    StyledRight(FitFont(&sNormal, value, w / 2), value, TTOP_X + w, TextY(ry, 22, &sNormal), STYLE_INK);
}

static void DrawTitleTop(const ViewState *c)
{
    const struct MapHeader *map = Overworld_GetMapHeaderByGroupAndId(gSaveBlock1Ptr->location.mapGroup,
                                                                     gSaveBlock1Ptr->location.mapNum);
    u8 text[32];
    int oy, row = 0;

    FacTopPaint(sDst);
    /* Where the game goes on, and CONTINUE lit beside it. */
    oy = PlateEx(TTOP_X, TTOP_BAR_Y, TTOP_W - TTOP_GO_W - 6, TTOP_BAR_H, sLight, PLATE_STATE_NORMAL,
                 TTOP_W - TTOP_GO_W - 16, 30, PlateCut(TTOP_BAR_H), 0);
    GetMapName(text, map->regionMapSectionId, 0);
    TextIn(text, TTOP_X, oy, TTOP_W - TTOP_GO_W - 14, TTOP_BAR_H, &sNormal, 'l', STYLE_INK, 7);
    oy = Plate(TTOP_X + TTOP_W - TTOP_GO_W, TTOP_BAR_Y, TTOP_GO_W, TTOP_BAR_H, sEmerald, PLATE_STATE_FOCUS);
    TextIn(gText_MainMenuContinue, TTOP_X + TTOP_W - TTOP_GO_W, oy, TTOP_GO_W, TTOP_BAR_H, &sNormal, 'c', STYLE_WHITE, 6);

    /* The card's front, as the bottom screen's card has it. */
    oy = PlateEx(TTOP_X, TTOP_CARD_Y, TTOP_W, TTOP_CARD_H, sCardGreen, PLATE_STATE_NORMAL, TTOP_W - 118, 25,
                 PlateCut(TTOP_CARD_H), 0);
    TitleTopRow(row++, oy, Ascii(CTR_TEXT("NAME", "NOMBRE", "NOM", "NOME", "NOME", "NAME", "なまえ")), c->name);
    TitleTopRow(row++, oy, Ascii(CTR_TEXT("ID No.", "Nº ID", "No. ID", "N. ID", "Nº ID", "ID-Nr.", "IDNo.")),
                Number(c->id, 5, STR_CONV_MODE_LEADING_ZEROS));
    text[0] = CHAR_CURRENCY;
    StringCopy(text + 1, Number(c->money, 6, STR_CONV_MODE_LEFT_ALIGN));
    TitleTopRow(row++, oy, Ascii(CTR_TEXT("MONEY", "DINERO", "ARGENT", "SOLDI", "DINHEIRO", "GELD", "おこづかい")), text);
    if (c->hasDex)
        TitleTopRow(row++, oy, gText_MenuPokedex, Number(c->dex, 3, STR_CONV_MODE_LEFT_ALIGN));
    TitleTopRow(row++, oy, Ascii(CTR_TEXT("TIME", "TIEMPO", "DURÉE", "TEMPO", "TEMPO", "ZEIT", "プレイじかん")),
                HoursMinutes(c->hours, c->minutes));
    /* The trainer, doubled, on the green; a star a feat above. */
    if (sRes.trainerPic[c->gender])
        DrawSpriteDouble(sRes.trainerPic[c->gender], 8, 8, TTOP_X + TTOP_W - 116, oy + 22,
                         sRes.trainerPicPal[c->gender].c);
    for (int k = 0; k < c->stars && k < 4; ++k)
        IconCentred(sArtStar, 2, 2 * (TTOP_X + TTOP_W - 100 + k * 18) + 11, 2 * (oy + 8) + 11, sArtPal2.c, FALSE);

    /* The badges in the band, the ones not won as dim shapes. */
    oy = Plate(TTOP_X, TTOP_BAND_Y + 4, TTOP_W, FTOP_H - TTOP_BAND_Y - 8, sLight, PLATE_STATE_NORMAL);
    if (sRes.badgeTiles)
    {
        u16 dim[16];

        for (int i = 0; i < 16; ++i)
            dim[i] = i ? PackRgb(176, 196, 184) : 0;
        for (int i = 0; i < NUM_BADGES; ++i)
        {
            const u16 *pal = (c->badges & (1 << i)) ? sRes.badgePal.c : dim;
            int bx = TTOP_X + 26 + i * 44, by = oy + (FTOP_H - TTOP_BAND_Y - 8 - 16) / 2 - 1;

            DrawTile(sRes.badgeTiles + (2 * i) * 32, bx, by, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 1) * 32, bx + 8, by, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 16) * 32, bx, by + 8, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 17) * 32, bx + 8, by + 8, pal, FALSE, FALSE);
        }
    }
}

/* Every frame: the card drawn once when the menu comes up, and kept up. */
static void TitleTopFrame(void)
{
    static ViewState card;
    u16 *dst = sDst;
    int ox = sOX, cx0 = sClipX0, cy0 = sClipY0, cx1 = sClipX1, cy1 = sClipY1;

    if (!CtrMainMenu_CardUp())
    {
        sTitleTopShown = FALSE;
        return;
    }
    /* The panel went (or another took it): the card is handed over again. */
    if (sTitleTopShown && !CtrTopPanel_Active())
        sTitleTopShown = FALSE;
    if (!sTitleTopShown)
    {
        /* The saved game does not change while the menu is up; its Pokédex
         * count and stars are counted afresh each time the card is drawn,
         * not taken from the field card's twice-a-second cache. */
        memset(&card, 0, sizeof(card));
        SnapshotCard(&card);
        card.dex = CardDexCount(card.hasDex);
        card.stars = CardStars();
        card.gender = gSaveBlock2Ptr->playerGender ? FEMALE : MALE;
        FactoryTopWarm();
        sDst = CtrTopPanel_Canvas();
        sOX = 0;
        sClipX0 = sClipY0 = 0;
        sClipX1 = FTOP_W;
        sClipY1 = FTOP_H;
        DrawTitleTop(&card);
        sDst = dst;
        sOX = ox;
        sClipX0 = cx0;
        sClipY0 = cy0;
        sClipX1 = cx1;
        sClipY1 = cy1;
        CtrTopPanel_Show();
        sTitleTopShown = TRUE;
        CtrLog_Write(CTR_LOG_VIDEO, "title top: card drawn");
    }
    TitleTopKeep();
}
