/*
 * The sections the bottom screen draws itself (included by 3ds_bottom_ui.c,
 * after 3ds_bottom_shell.inc): the map and its wild list, the trainer card,
 * SAVE and the Nuzlocke tracker. OPTIONS is 3ds_bottom_options.inc.
 */

/* ------------------------------------------------------------------------ */
/* The map                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * The region map 1:1 in its window: the GBA's 248x160 picture (31 by 20 of
 * its tiles), the player's mark on it, a tapped cell's cursor. The area's
 * name is the header's title; with AREA WILD LIST on, the title is the
 * toggle between the map and the area's wild Pokémon.
 */
#define MAP_PIC_W 248
#define MAP_PIC_H 160

static void DrawMapTile8(u8 tile, int x, int y)
{
    const u8 *src;

    if (!sRes.mapTiles || tile >= sRes.mapTileCount)
        return;
    src = sRes.mapTiles + tile * 64;
    for (int py = 0; py < 8; ++py)
        for (int px = 0; px < 8; ++px)
        {
            /* The map's colours are loaded at palette 7: indices 112 up. */
            u8 v = src[py * 8 + px];
            if (v >= 112 && v < 144)
                Put(x + px, y + py, sRes.mapPal[v - 112]);
        }
}

/* Draws only inside the map's picture, and inside the redraw's own clip. */
static void ClipToMapWindow(int saved[4])
{
    ClipPush(saved, MAP_ORIGIN_X, MAP_ORIGIN_Y, MAP_ORIGIN_X + MAP_PIC_W, MAP_ORIGIN_Y + MAP_PIC_H);
}

static void RestoreClip(const int saved[4])
{
    ClipPop(saved);
}

/* The backdrop with the map's window and the map in it, once, into its cache. */
static void BuildMapCache(void)
{
    int saved[4];

    if (!sCache[CACHE_MAP])
        return;
    memcpy(sCache[CACHE_MAP], sCache[CACHE_MENU], sizeof(sCanvas));
    sDst = sCache[CACHE_MAP];
    DrawChamferFrame(MAP_WIN_X, MAP_WIN_Y, MAP_WIN_W, MAP_WIN_H, 4, 2);
    ClipToMapWindow(saved);
    FillRect(MAP_ORIGIN_X, MAP_ORIGIN_Y, MAP_PIC_W, MAP_PIC_H, sRes.mapPal[0]);
    /* The map is a 64x64 affine map; the ocean around Hoenn is tile 0. */
    for (int ty = 0; ty < MAP_PIC_H / 8; ++ty)
        for (int tx = 0; tx < MAP_PIC_W / 8; ++tx)
            DrawMapTile8(sRes.mapMap ? sRes.mapMap[ty * 64 + tx] : 0, MAP_ORIGIN_X + tx * 8, MAP_ORIGIN_Y + ty * 8);
    RestoreClip(saved);
    sDst = sCanvas;
}

/* The player's mark and a picked cell's cursor, inside the window. */
static void DrawMapMarks(const ViewState *s)
{
    int saved[4];

    ClipToMapWindow(saved);
    if (s->mapsec != MAPSEC_NONE && sRes.playerIcon[s->gender])
        DrawSprite(sRes.playerIcon[s->gender], 2, 2, MAP_ORIGIN_X + s->cursorX * 8 - 4,
                   MAP_ORIGIN_Y + s->cursorY * 8 - 4, sRes.playerIconPal[s->gender].c);
    if (s->pickMapsec != MAPSEC_NONE && sRes.cursorTiles)
        DrawSprite(sRes.cursorTiles, 2, 2, MAP_ORIGIN_X + s->pickX * 8 - 4, MAP_ORIGIN_Y + s->pickY * 8 - 4,
                   sRes.cursorPal.c);
    RestoreClip(saved);
}

/* The mark's 2x2 tiles at a cursor cell, clipped to the map's picture. */
static void MarkRect(u8 cx, u8 cy, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = MAP_ORIGIN_X + cx * 8 - 4;
    *y0 = MAP_ORIGIN_Y + cy * 8 - 4;
    *x1 = *x0 + 16;
    *y1 = *y0 + 16;
    if (*x0 < MAP_ORIGIN_X) *x0 = MAP_ORIGIN_X;
    if (*y0 < MAP_ORIGIN_Y) *y0 = MAP_ORIGIN_Y;
    if (*x1 > MAP_ORIGIN_X + MAP_PIC_W) *x1 = MAP_ORIGIN_X + MAP_PIC_W;
    if (*y1 > MAP_ORIGIN_Y + MAP_PIC_H) *y1 = MAP_ORIGIN_Y + MAP_PIC_H;
    if (*x1 < *x0) *x1 = *x0;
    if (*y1 < *y0) *y1 = *y0;
}

/* The section under a tapped map cell, as the region map's cursor finds it. */
static void PickMapCell(int x, int y)
{
    int cx = (x - MAP_ORIGIN_X + 64) / 8 - 8, cy = (y - MAP_ORIGIN_Y + 64) / 8 - 8;

    sPickMapsec = MAPSEC_NONE;
    for (int i = 0; i < MAPSEC_NONE; ++i)
    {
        const struct RegionMapLocation *e = &gRegionMapEntries[i];
        int ex = e->x + 1, ey = e->y + 2;

        if (e->width && cx >= ex && cy >= ey && cx < ex + e->width && cy < ey + e->height)
        {
            sPickMapsec = i;
            sPickX = cx;
            sPickY = cy;
            return;
        }
    }
}

/* The wild list, where the map is: a cream plate, the method's title bar
 * with the page of how many, then a row a species - its icon, its name
 * ("???" not seen yet), its levels and its chance. */
#define ENC_X MAP_WIN_X
#define ENC_Y MAP_WIN_Y
#define ENC_W MAP_WIN_W
#define ENC_H MAP_WIN_H
#define ENC_TITLE_Y (ENC_Y + 4)
#define ENC_TITLE_H 18
#define ENC_ROW_Y (ENC_Y + 25)
#define ENC_ROW_H 22
#define ENC_COL_NAME (ENC_X + 42)
#define ENC_COL_LEVEL (ENC_X + 150)
#define ENC_COL_PERCENT (ENC_X + ENC_W - 10)

/* A species not seen is "???": the list names no more than the Pokedex. */
static const u8 *EncName(const EncRow *row)
{
    return row->seen ? gSpeciesNames[row->species] : Ascii("???");
}

static void DrawEncounters(const ViewState *s)
{
    static const char *const methods[ENC_METHODS] = {
        CTR_TEXT("GRASS", "HIERBA", "HERBES", "ERBA", "AN LAND", "GRAMA"), CTR_TEXT("SURFING", "SURF", "SURF", "SURF", "SURFER", "SURF"),
        CTR_TEXT("ROCK SMASH", "GOLPE ROCA", "ÉCLATE-ROC", "SPACCAROCCIA", "ZERTRÜMMERER", "QUEBRA-ROCHA"),
        CTR_TEXT("OLD ROD", "CAÑA VIEJA", "CANNE", "AMO VECCHIO", "ANGEL", "VARA VELHA"),
        CTR_TEXT("GOOD ROD", "CAÑA BUENA", "SUPER CANNE", "AMO BUONO", "PROFIANGEL", "VARA BOA"),
        CTR_TEXT("SUPER ROD", "SUPERCAÑA", "MÉGA CANNE", "SUPERAMO", "SUPERANGEL", "SUPER VARA"),
    };
    static const char *const none = CTR_TEXT("NO ENCOUNTER TABLE", "SIN TABLA DE ENCUENTROS", "AUCUNE TABLE ICI", "NESSUNA TABELLA", "KEINE BEGEGNUNGSTABELLE", "SEM TABELA DE ENCONTROS");
    static const char *const level = CTR_TEXT("LV", "NV", "N", "LV", "LV", "NV");
    char text[24];
    int oy, saved[4];

    oy = Plate(ENC_X, ENC_Y, ENC_W, ENC_H, sCreamPlate, PLATE_STATE_NORMAL);
    if (s->encPages == 0 || s->encMethod >= ENC_METHODS)
    {
        TextIn(Ascii(none), ENC_X, oy, ENC_W, ENC_H, &sSmall, 'c', STYLE_GRAY, 6);
        return;
    }
    oy = Plate(ENC_X + 4, ENC_TITLE_Y, ENC_W - 8, ENC_TITLE_H, sEmerald, PLATE_STATE_NORMAL);
    TextIn(Ascii(methods[s->encMethod]), ENC_X + 4, oy, ENC_W - 8, ENC_TITLE_H, &sSmall, 'l', STYLE_WHITE, 6);
    snprintf(text, sizeof(text), "%u/%u", s->encPage + 1u, (unsigned)s->encPages);
    TextIn(Ascii(text), ENC_X + 4, oy, ENC_W - 8, ENC_TITLE_H, &sSmall, 'r', STYLE_WHITE, 6);
    for (int i = 0; i < s->encCount && i < ENC_SHOWN; ++i)
    {
        const EncRow *row = &s->enc[i];
        int y = ENC_ROW_Y + i * ENC_ROW_H;

        if (i & 1)
            FillRgb(ENC_X + 4, y, ENC_W - 8, ENC_ROW_H, sOddRow);
        if (row->seen)
        {
            u16 icon = GetIconSpecies(row->species, 0);
            const u8 *tiles = MonIcon(icon, FALSE);
            u8 pal = gMonIconPaletteIndices[icon];

            /* The icon's rows 6 to 28, its body: the row holds them whole. */
            ClipPush(saved, ENC_X + 6, y, ENC_X + 6 + 32, y + ENC_ROW_H);
            if (tiles && pal < ARRAY_COUNT(sRes.monIconPal))
                DrawSprite(tiles, 4, 4, ENC_X + 6, y - 6, sRes.monIconPal[pal].c);
            ClipPop(saved);
        }
        StyledStr(FitFont(&sSmall, EncName(row), ENC_COL_LEVEL - ENC_COL_NAME - 4), EncName(row), ENC_COL_NAME,
                  TextY(y, ENC_ROW_H, &sSmall), row->seen ? STYLE_INK : STYLE_GRAY);
        if (row->minLevel == row->maxLevel)
            snprintf(text, sizeof(text), "%s%u", level, (unsigned)row->minLevel);
        else
            snprintf(text, sizeof(text), "%s%u-%u", level, (unsigned)row->minLevel, (unsigned)row->maxLevel);
        StyledStr(&sSmall, Ascii(text), ENC_COL_LEVEL, TextY(y, ENC_ROW_H, &sSmall), STYLE_INK);
        snprintf(text, sizeof(text), "%u%%", (unsigned)row->percent);
        StyledRight(&sSmall, Ascii(text), ENC_COL_PERCENT, TextY(y, ENC_ROW_H, &sSmall), STYLE_GREEN);
    }
}

/* Over the map's top, until tapped: the saved clock could not be restored
 * and the 3DS's is in use. The game goes on; nothing is said of the save. */
#define NOTICE_X (MAP_WIN_X + 6)
#define NOTICE_Y (MAP_WIN_Y + 6)
#define NOTICE_W (MAP_WIN_W - 12)
#define NOTICE_H 32

static void DrawClockNotice(const ViewState *s)
{
    static const char *const first = CTR_TEXT("SAVED CLOCK NOT RESTORED", "RELOJ GUARDADO NO RESTAURADO", "HORLOGE SAUVÉE NON RESTAURÉE", "OROLOGIO NON RIPRISTINATO", "GESPEICHERTE UHR NICHT GELADEN", "RELÓGIO SALVO NÃO RESTAURADO");
    static const char *const second = CTR_TEXT("USING 3DS CLOCK - TAP TO HIDE", "USANDO RELOJ 3DS - TOCA AQUÍ", "HORLOGE 3DS UTILISÉE - TOUCHER", "OROLOGIO 3DS IN USO - TOCCA", "3DS-UHR AKTIV - TIPPEN: AUSBLENDEN", "USANDO RELÓGIO 3DS - TOQUE AQUI");
    int oy = Plate(NOTICE_X, NOTICE_Y, NOTICE_W, NOTICE_H, sEmerald,
                   s->pressed == HIT_CLOCK_NOTICE ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);

    TextIn(Ascii(first), NOTICE_X, oy + 1, NOTICE_W, 16, &sSmall, 'c', STYLE_WHITE, 6);
    TextIn(Ascii(second), NOTICE_X, oy + 14, NOTICE_W, 16, &sSmall, 'c', STYLE_WHITE, 6);
}

static void DrawRegionMap(const ViewState *s)
{
    if (s->encView)
    {
        DrawEncounters(s);
        if (s->encPages > 1)
            AddHit(ENC_X, ENC_Y, ENC_W, ENC_H, HIT_MAP_PAGE);
    }
    else
    {
        DrawMapMarks(s);
        AddHit(MAP_ORIGIN_X, MAP_ORIGIN_Y, MAP_PIC_W, MAP_PIC_H, HIT_MAP);
    }
    if (s->clockNotice)
    {
        DrawClockNotice(s);
        AddHit(NOTICE_X, NOTICE_Y, NOTICE_W, NOTICE_H, HIT_CLOCK_NOTICE);
    }
}

/* ------------------------------------------------------------------------ */
/* The trainer card                                                         */
/* ------------------------------------------------------------------------ */

/*
 * The card as a green plate: the figures down its left on a cream end, the
 * trainer and the stars on its right; the badges on a plate below, the ones
 * not won as dim shapes; FLIP turns it over to the link records the game's
 * card has on its back.
 */
#define CARD_H 120
#define CARD_BADGES_Y (AREA_Y + 124)
#define CARD_BADGES_H 40
#define CARD_FLIP_W 66

/* A rule across a cream plate, a row of label and value under it. */
static void CardRow(int i, int oy, const u8 *label, const u8 *value)
{
    int ry = oy + 6 + i * 22;

    if (i)
        FillRgb(AREA_X + 8, ry - 1, AREA_W - 112, 1, sRule);
    StyledStr(FitFont(&sSmall, label, 120), label, AREA_X + 10, TextY(ry, 20, &sSmall), STYLE_GRAY);
    StyledRight(FitFont(&sNormal, value, 110), value, AREA_X + AREA_W - 108, TextY(ry, 20, &sNormal), STYLE_INK);
}

static const u8 *HoursMinutes(unsigned hours, unsigned minutes)
{
    static u8 text[16];
    u8 *end = StringCopy(text, Number(hours, 3, STR_CONV_MODE_LEFT_ALIGN));

    end = StringCopy(end, gText_Colon2);
    StringCopy(end, Number(minutes, 2, STR_CONV_MODE_LEADING_ZEROS));
    return text;
}

static void DrawTrainerCard(const ViewState *s)
{
    u8 text[32];
    int oy = PlateEx(AREA_X, AREA_Y, AREA_W, CARD_H, sCardGreen, PLATE_STATE_NORMAL, AREA_W - 92, 25,
                     PlateCut(CARD_H), 0);
    int row = 0;

    if (!s->cardBack)
    {
        CardRow(row++, oy, Ascii(CTR_TEXT("NAME", "NOMBRE", "NOM", "NOME", "NAME", "NOME")), s->name);
        CardRow(row++, oy, Ascii(CTR_TEXT("ID No.", "Nº ID", "No. ID", "N. ID", "ID-Nr.", "Nº ID")),
                Number(s->id, 5, STR_CONV_MODE_LEADING_ZEROS));
        text[0] = CHAR_CURRENCY;
        StringCopy(text + 1, Number(s->money, 6, STR_CONV_MODE_LEFT_ALIGN));
        CardRow(row++, oy, Ascii(CTR_TEXT("MONEY", "DINERO", "ARGENT", "SOLDI", "GELD", "DINHEIRO")), text);
        if (s->hasDex)
            CardRow(row++, oy, gText_MenuPokedex, Number(s->dex, 3, STR_CONV_MODE_LEFT_ALIGN));
        CardRow(row++, oy, Ascii(CTR_TEXT("TIME", "TIEMPO", "DURÉE", "TEMPO", "ZEIT", "TEMPO")), HoursMinutes(s->hours, s->minutes));
    }
    else
    {
        /* trainer_card.c's back: the Hall of Fame debut and the link records. */
        if (s->hofHours || s->hofMinutes)
            CardRow(row++, oy, gText_HallOfFameDebut, HoursMinutes(s->hofHours, s->hofMinutes));
        else
            CardRow(row++, oy, gText_HallOfFameDebut, Ascii("---"));
        StringCopy(StringCopy(StringCopy(text, Number(s->linkWins, 4, STR_CONV_MODE_LEFT_ALIGN)), Ascii("-")),
                   Number(s->linkLosses, 4, STR_CONV_MODE_LEFT_ALIGN));
        CardRow(row++, oy, gText_LinkBattles, text);
        CardRow(row++, oy, gText_PokemonTrades, Number(s->trades, 5, STR_CONV_MODE_LEFT_ALIGN));
        CardRow(row++, oy, gText_PokeblocksWithFriends, Number(s->pokeblocks, 5, STR_CONV_MODE_LEFT_ALIGN));
        CardRow(row++, oy, gText_WonContestsWFriends, Number(s->contests, 3, STR_CONV_MODE_LEFT_ALIGN));
    }
    /* The trainer, whole, on the green; a star a feat above. */
    if (sRes.trainerPic[s->gender])
        DrawSprite(sRes.trainerPic[s->gender], 8, 8, AREA_X + AREA_W - 80, oy + 28, sRes.trainerPicPal[s->gender].c);
    for (int k = 0; k < s->stars && k < 4; ++k)
        IconCentred(sArtStar, 2, 2 * (AREA_X + AREA_W - 74 + k * 14) + 11, 2 * (oy + 8) + 11, sArtPal2.c, FALSE);

    /* The badges: won ones as the game has them, the rest as dim shapes. */
    oy = Plate(AREA_X, CARD_BADGES_Y, AREA_W - CARD_FLIP_W - 4, CARD_BADGES_H, sLight, PLATE_STATE_NORMAL);
    if (sRes.badgeTiles)
    {
        u16 dim[16];

        for (int i = 0; i < 16; ++i)
            dim[i] = i ? PackRgb(176, 196, 184) : 0;
        for (int i = 0; i < NUM_BADGES; ++i)
        {
            const u16 *pal = (s->badges & (1 << i)) ? sRes.badgePal.c : dim;
            int x = AREA_X + 10 + i * 29, y = oy + 12;

            DrawTile(sRes.badgeTiles + (2 * i) * 32, x, y, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 1) * 32, x + 8, y, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 16) * 32, x, y + 8, pal, FALSE, FALSE);
            DrawTile(sRes.badgeTiles + (2 * i + 17) * 32, x + 8, y + 8, pal, FALSE, FALSE);
        }
    }
    oy = Plate(AREA_X + AREA_W - CARD_FLIP_W, CARD_BADGES_Y, CARD_FLIP_W, CARD_BADGES_H, s->pressed == HIT_OK ? sEmerald : sSlate,
               s->pressed == HIT_OK ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
    TextIn(Ascii(CTR_TEXT("FLIP", "GIRAR", "TOURNER", "GIRA", "WENDEN", "VIRAR")), AREA_X + AREA_W - CARD_FLIP_W, oy, CARD_FLIP_W,
           CARD_BADGES_H, &sNormal, 'c', STYLE_WHITE, 6);
    AddHit(AREA_X + AREA_W - CARD_FLIP_W, CARD_BADGES_Y, CARD_FLIP_W, CARD_BADGES_H, HIT_OK);
}

/* ------------------------------------------------------------------------ */
/* SAVE                                                                     */
/* ------------------------------------------------------------------------ */

/*
 * The player on an emerald plate, the game's save window's figures on a
 * cream one beside it, the party under them (a fainted one grey), and the
 * question with YES and NO, or what the save did with OK.
 */
#define SAVE_TOP_H 92
#define SAVE_PARTY_Y (AREA_Y + 95)
#define SAVE_PARTY_H 36
#define SAVE_ASK_Y (AREA_Y + 134)
#define SAVE_BUTTON_W 64

static int StrLines(const u8 *str)
{
    int lines = 1;

    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
        if (g == 0xFFFE)
            ++lines;
    return lines;
}

static void SaveButton(int x, int w, int y, int h, const u8 *label, Rgb colour, bool8 enabled, u8 id,
                       const ViewState *s)
{
    bool8 pressed = enabled && s->pressed == id;
    int oy = Plate(x, y, w, h, !enabled ? sOffGrey : pressed ? sEmerald : colour,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);

    TextIn(label, x, oy, w, h, &sNormal, 'c', STYLE_WHITE, 4);
    if (enabled)
        AddHit(x, y, w, h, id);
}

static void SaveRow(int i, int x, int w, int oy, const u8 *label, const u8 *value)
{
    int ry = oy + 4 + i * 17;

    StyledStr(FitFont(&sSmall, label, 100), label, x + 10, TextY(ry, 17, &sSmall), STYLE_GRAY);
    StyledRight(FitFont(&sSmall, value, w - 130), value, x + w - 20, TextY(ry, 17, &sSmall), STYLE_INK);
}

static void DrawSave(const ViewState *s)
{
    u8 text[32];
    int x = AREA_X + 84, w = AREA_W - 84, oy, lines = StrLines(s->text);
    int askY = SAVE_ASK_Y, askH = AREA_Y + AREA_H - SAVE_ASK_Y, askW = AREA_W - 2 * SAVE_BUTTON_W - 6;
    int sw = (AREA_W - 12) / PARTY_SIZE, badges = 0;

    oy = PlateEx(AREA_X, AREA_Y, 80, SAVE_TOP_H, sEmerald, PLATE_STATE_NORMAL, 68, 15, PlateCut(SAVE_TOP_H), 0);
    if (sRes.trainerPic[s->gender])
        DrawSprite(sRes.trainerPic[s->gender], 8, 8, AREA_X + 4, oy + 14, sRes.trainerPicPal[s->gender].c);

    oy = PlateEx(x, AREA_Y, w, SAVE_TOP_H, sCreamPlate, PLATE_STATE_NORMAL, w - 10, 30, PlateCut(SAVE_TOP_H), 0);
    if (s->mapsec != MAPSEC_NONE)
        GetMapName(text, s->mapsec, 0);
    else
        StringCopy(text, Ascii("--"));
    SaveRow(0, x, w, oy, Ascii(CTR_TEXT("AREA", "ZONA", "ZONE", "ZONA", "GEBIET", "ÁREA")), text);
    SaveRow(1, x, w, oy, Ascii(CTR_TEXT("PLAYER", "JUGADOR", "JOUEUR", "GIOCATORE", "SPIELER", "JOGADOR")), s->name);
    for (int i = 0; i < NUM_BADGES; ++i)
        badges += (s->badges >> i) & 1;
    SaveRow(2, x, w, oy, Ascii(CTR_TEXT("BADGES", "MEDALLAS", "BADGES", "MEDAGLIE", "ORDEN", "INSÍGNIAS")),
            Number(badges, 1, STR_CONV_MODE_LEFT_ALIGN));
    if (s->hasDex)
        SaveRow(3, x, w, oy, gText_MenuPokedex, Number(s->dex, 3, STR_CONV_MODE_LEFT_ALIGN));
    SaveRow(s->hasDex ? 4 : 3, x, w, oy, Ascii(CTR_TEXT("TIME", "TIEMPO", "DURÉE", "TEMPO", "ZEIT", "TEMPO")),
            HoursMinutes(s->hours, s->minutes));

    oy = Plate(AREA_X, SAVE_PARTY_Y, AREA_W, SAVE_PARTY_H, sLight, PLATE_STATE_NORMAL);
    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        const MonView *mon = &s->party[i];
        const u8 *tiles;
        u8 pal;

        if (!mon->species)
            continue;
        tiles = MonIcon(mon->iconSpecies, mon->deoxys);
        pal = gMonIconPaletteIndices[mon->iconSpecies];
        if (tiles && pal < ARRAY_COUNT(sRes.monIconPal))
            IconCentred(tiles, 4, 2 * (AREA_X + 6 + i * sw) + sw, 2 * (oy + 17), sRes.monIconPal[pal].c,
                        mon->fainted);
    }

    /* A long question (another file's warning) rises over the party. */
    if (lines > 2)
    {
        askH = lines * 14 + 4;
        askY = AREA_Y + AREA_H - askH;
    }
    oy = Plate(AREA_X, askY, askW, askH, sCreamPlate, PLATE_STATE_NORMAL);
    {
        const u8 *str = s->text;
        u8 line[96];

        for (int i = 0; i < lines; ++i)
        {
            int n = 0;
            u16 g;
            const u8 *start = str;

            while ((g = NextGlyph(&str)) != 0xFFFF && g != 0xFFFE)
                ;
            n = (int)(str - start) - (g == 0xFFFE ? 1 : 0);
            if (n > (int)sizeof(line) - 1)
                n = sizeof(line) - 1;
            memcpy(line, start, n);
            line[n] = EOS;
            StyledStr(FitFont(&sSmall, line, askW - 16), line, AREA_X + 8, oy + 1 + i * 14, STYLE_INK);
            if (g == 0xFFFF)
                break;
        }
    }
    if (s->saveStep == SAVE_DONE)
        SaveButton(AREA_X + askW + 3, 2 * SAVE_BUTTON_W + 3, askY, askH, Ascii("OK"), sEmerald, TRUE, HIT_OK, s);
    else
    {
        SaveButton(AREA_X + askW + 3, SAVE_BUTTON_W, askY, askH, gText_Yes, sEmerald, s->canSave, HIT_YES, s);
        SaveButton(AREA_X + askW + 6 + SAVE_BUTTON_W, SAVE_BUTTON_W, askY, askH, gText_No, sCancelGrey, TRUE, HIT_NO, s);
    }
}

/* ------------------------------------------------------------------------ */
/* NUZLOCKE                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * The tracker's own section, in the dock while it is on (3ds_nuzlocke_ui.h):
 * the area and what was recorded there (CAUGHT, MISSED, UNUSED: a tap
 * records it), the party (a tap chooses one; a fallen one grey with a mark),
 * FALLEN MARK and MARK FAINTED with what a tap would do, the next level cap
 * and the run's totals.
 */
enum { HIT_NUZ_STATUS = HIT_ROW, HIT_NUZ_SLOT = HIT_ROW + 4, HIT_NUZ_FALLEN = HIT_ROW + 12, HIT_NUZ_FAINTED };

static void DrawNuzlocke(const ViewState *s)
{
    static const char *const status[3] = {
        CTR_TEXT("CAUGHT", "CAPTURADO", "CAPTURÉ", "CATTURATO", "GEFANGEN", "CAPTURADO"),
        CTR_TEXT("MISSED", "PERDIDO", "RATÉ", "PERSO", "VERPASST", "PERDIDO"),
        CTR_TEXT("UNUSED", "LIBRE", "LIBRE", "LIBERO", "UNGENUTZT", "LIVRE"),
    };
    /* The buttons' order against CtrNuzStatus (0 unused, 1 caught, 2 missed). */
    static const u8 statusOf[3] = {1, 2, 0};
    int x = AREA_X, y = AREA_Y, w = AREA_W, oy, bw = (w - 16 - 4) / 3, sw = (w - 15) / PARTY_SIZE, half = (w - 3) / 2;

    oy = PlateEx(x, y, w, 44, sCreamPlate, PLATE_STATE_NORMAL, w - 10, 30, PlateCut(44), 0);
    TextIn(s->nuzArea, x, oy + 1, w - 90, 20, &sNormal, 'l', STYLE_INK, 8);
    TextIn(s->nuzLink, x + w - 100, oy + 1, 92, 20, &sSmall, 'r', STYLE_GRAY, 4);
    for (int i = 0; i < 3; ++i)
    {
        int bx = x + 6 + i * (bw + 2);
        bool8 live = s->nuzStatus == statusOf[i], pressed = s->pressed == HIT_NUZ_STATUS + i;
        bool8 enabled = s->nuzStatus >= 0;
        int by = Plate(bx, y + 22, bw, 17, !enabled ? sOffGrey : live || pressed ? sEmerald : sLight,
                       pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);

        TextIn(Ascii(status[i]), bx, by, bw, 17, &sSmall, 'c', live || pressed || !enabled ? STYLE_WHITE : STYLE_INK, 3);
        if (enabled)
            AddHit(bx, y + 22, bw, 17, HIT_NUZ_STATUS + i);
    }
    y += 47;
    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        const MonView *mon = &s->party[i];
        int bx = x + i * (sw + 3);
        bool8 chosen = mon->species && s->nuzSlot == i, fallen = (s->nuzFallen >> i) & 1;
        bool8 pressed = mon->species && s->pressed == HIT_NUZ_SLOT + i;

        oy = Plate(bx, y, sw, 36, !mon->species || fallen ? sOffGrey : chosen || pressed ? sEmerald : sLight,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        if (!mon->species)
            continue;
        {
            const u8 *tiles = MonIcon(mon->iconSpecies, mon->deoxys);
            u8 pal = gMonIconPaletteIndices[mon->iconSpecies];

            if (tiles && pal < ARRAY_COUNT(sRes.monIconPal))
                IconCentred(tiles, 4, 2 * bx + sw, 2 * oy + 35, sRes.monIconPal[pal].c, fallen);
        }
        if (fallen)
            DrawSprite(sArtTinyX, 1, 1, bx + sw - 11, y + 25, sArtPal.c);
        AddHit(bx, y, sw, 36, HIT_NUZ_SLOT + i);
    }
    y += 39;
    for (int col = 0; col < 2; ++col)
    {
        int bx = x + col * (half + 3);
        u8 id = col ? HIT_NUZ_FAINTED : HIT_NUZ_FALLEN;
        bool8 pressed = s->pressed == id;

        oy = Plate(bx, y, half, 30, pressed ? sEmerald : sLight, pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(Ascii(col ? CTR_TEXT("MARK FAINTED", "MARCAR DEBILIT.", "MARQUER K.O.", "SEGNA K.O.", "K.O. MARKIEREN", "MARCAR DESMAIO")
                         : CTR_TEXT("FALLEN MARK", "MARCA CAÍDO", "MARQUE TOMBÉ", "SEGNO CADUTO", "GEFALLEN-STATUS", "MARCA CAÍDO")),
               bx, oy + 1, half, 14, &sSmall, 'c', pressed ? STYLE_WHITE : STYLE_INK, 4);
        TextIn(col ? s->nuzFainted : s->nuzFallenText, bx, oy + 14, half, 14, &sSmall, 'c',
               pressed ? STYLE_WHITE : STYLE_GREEN, 4);
        AddHit(bx, y, half, 30, id);
    }
    y += 33;
    oy = Plate(x, y, w, AREA_Y + AREA_H - y, sCreamPlate, PLATE_STATE_NORMAL);
    TextIn(Ascii(CTR_TEXT("NEXT LEVEL CAP", "NIVEL MÁXIMO", "NIVEAU MAX", "LIVELLO MAX", "NÄCHSTES LV-LIMIT", "NÍVEL MÁXIMO")), x, oy + 1, w / 2, 14,
           &sSmall, 'l', STYLE_GRAY, 8);
    TextIn(s->nuzCap, x + w / 2, oy + 1, w / 2, 14, &sSmall, 'r', STYLE_INK, 8);
    TextIn(Ascii(CTR_TEXT("CAUGHT/MISS/FALLEN", "CAPT./PERD./CAÍDOS", "PRIS/RATÉS/TOMBÉS", "PRESI/PERSI/CADUTI", "GEF./VERP./GEFALL.", "CAPT./PERD./CAÍDOS")),
           x, oy + 14, w * 2 / 3, 14, &sSmall, 'l', STYLE_GRAY, 8);
    TextIn(s->nuzTotals, x + w * 2 / 3, oy + 14, w / 3, 14, &sSmall, 'r', STYLE_INK, 8);
}

static void NuzlockeTap(u8 id)
{
    static const u8 statusOf[3] = {1, 2, 0};

    if (id >= HIT_NUZ_STATUS && id < HIT_NUZ_STATUS + 3)
        CtrNuzlockeUi_RecordAs(statusOf[id - HIT_NUZ_STATUS]);
    else if (id >= HIT_NUZ_SLOT && id < HIT_NUZ_SLOT + PARTY_SIZE)
    {
        CtrNuzlockeUi_SetSlot(id - HIT_NUZ_SLOT);
        PlaySE(SE_SELECT);
    }
    else if (id == HIT_NUZ_FALLEN)
        CtrNuzlockeUi_Fallen();
    else if (id == HIT_NUZ_FAINTED)
        CtrNuzlockeUi_MarkFainted();
}
