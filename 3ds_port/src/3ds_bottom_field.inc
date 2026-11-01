/*
 * The sections the bottom screen draws itself (included by 3ds_bottom_ui.c,
 * after 3ds_bottom_shell.inc): the map and its wild list, the trainer card,
 * SAVE and the Nuzlocke tracker. OPTIONS is 3ds_bottom_options.inc.
 */

/* ------------------------------------------------------------------------ */
/* The map                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * The region map in its window: the GBA's 248x160 picture (31 by 20 of its
 * tiles), 1:1 or zoomed in twice the size as the PokéNav's A shows it, the
 * player's mark on it, a tapped cell's cursor. The area's name is the
 * header's title; with AREA WILD LIST on, the title is the toggle between the
 * map and the area's wild Pokémon. The zoom, the place panel and the Fly map
 * come from public PR #108 (luksgrin), drawn with this screen's plates.
 */
#define MAP_PIC_W 248
#define MAP_PIC_H 160
/* The zoom button, inside the picture's bottom right corner (the sea). */
#define ZOOM_W 24
#define ZOOM_H 24
#define ZOOM_X (MAP_ORIGIN_X + MAP_PIC_W - 4 - ZOOM_W)
#define ZOOM_Y (MAP_ORIGIN_Y + MAP_PIC_H - 4 - ZOOM_H)
/* Zoomed in, the picked place's panel at the picture's top, on the side
 * away from the place: a town's layout, or a route's landmarks. */
#define PANEL_Y (MAP_ORIGIN_Y + 4)
#define PANEL_CITY_W 104
#define PANEL_MARKS_W 132
#define PANEL_TITLE_H 18
#define PANEL_ROW_H 16
#define PANEL_MARKS_MAX 4

/* A map pixel's colour, from the picture's top left. */
static u16 MapPixel(int mx, int my)
{
    u8 tile = 0, v;

    if (!sRes.mapTiles)
        return sRes.mapPal[0];
    /* The map is a 64x64 affine map; the ocean around Hoenn is tile 0. */
    if (sRes.mapMap && mx >= 0 && my >= 0 && mx < 64 * 8 && my < 64 * 8)
        tile = sRes.mapMap[(my >> 3) * 64 + (mx >> 3)];
    if (tile >= sRes.mapTileCount)
        return sRes.mapPal[0];
    /* The map's colours are loaded at palette 7: indices 112 up. */
    v = sRes.mapTiles[tile * 64 + (my & 7) * 8 + (mx & 7)];
    return v >= 112 && v < 144 ? sRes.mapPal[v - 112] : sRes.mapPal[0];
}

/* One 8x8 tile of the map 1:1, for the Pokédex's area page. */
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

/* Map pixels to the screen, as the view has them. */
static void MapToScreen(const ViewState *s, int mx, int my, int *sx, int *sy)
{
    if (s->mapZoom)
    {
        *sx = MAP_ORIGIN_X + (mx - s->mapViewX) * 2;
        *sy = MAP_ORIGIN_Y + (my - s->mapViewY) * 2;
    }
    else
    {
        *sx = MAP_ORIGIN_X + mx;
        *sy = MAP_ORIGIN_Y + my;
    }
}

/* A cursor cell's centre on the screen (cells as the region map's cursor has
 * them: the picture's 8x8 tiles). */
static void CellCentre(const ViewState *s, int cx, int cy, int *sx, int *sy)
{
    MapToScreen(s, cx * 8 + 4, cy * 8 + 4, sx, sy);
}

/* The view the map's cache holds: zoomed or not, and its top left. */
static s16 sMapCacheView[3] = {-1, 0, 0};

/* The backdrop with the map's window and the map in it, into its cache: 1:1,
 * or zoomed in from (vx, vy). Drawn whole whatever redraw asks for it. */
static void BuildMapCache(bool8 zoom, int vx, int vy)
{
    int clip[4] = {sClipX0, sClipY0, sClipX1, sClipY1}, ox = sOX, saved[4];
    u16 *dst = sDst;

    if (!sCache[CACHE_MAP])
        return;
    sClipX0 = sClipY0 = 0;
    sClipX1 = W;
    sClipY1 = H;
    sOX = 0;
    memcpy(sCache[CACHE_MAP], sCache[CACHE_MENU], sizeof(sCanvas));
    sDst = sCache[CACHE_MAP];
    DrawChamferFrame(MAP_WIN_X, MAP_WIN_Y, MAP_WIN_W, MAP_WIN_H, 4, 2);
    ClipToMapWindow(saved);
    if (zoom)
    {
        for (int y = 0; y < MAP_PIC_H; y += 2)
            for (int x = 0; x < MAP_PIC_W; x += 2)
                FillRect(MAP_ORIGIN_X + x, MAP_ORIGIN_Y + y, 2, 2, MapPixel(vx + x / 2, vy + y / 2));
    }
    else
    {
        for (int y = 0; y < MAP_PIC_H; ++y)
            for (int x = 0; x < MAP_PIC_W; ++x)
                Put(MAP_ORIGIN_X + x, MAP_ORIGIN_Y + y, MapPixel(x, y));
    }
    RestoreClip(saved);
    sDst = dst;
    sOX = ox;
    sClipX0 = clip[0];
    sClipY0 = clip[1];
    sClipX1 = clip[2];
    sClipY1 = clip[3];
    sMapCacheView[0] = zoom;
    sMapCacheView[1] = vx;
    sMapCacheView[2] = vy;
}

/* The cache with the view this state shows, made again only when it moves. */
static void EnsureMapCache(const ViewState *s)
{
    int zoom = s->mapZoom != 0, vx = zoom ? s->mapViewX : 0, vy = zoom ? s->mapViewY : 0;

    if (sMapCacheView[0] != zoom || sMapCacheView[1] != vx || sMapCacheView[2] != vy)
        BuildMapCache(zoom, vx, vy);
}

/* The PokéNav's large cursor, for the map zoomed in; loaded the first time. */
static u8 *sCursorLarge;
static bool8 sCursorLargeTried;

static const u8 *LargeCursor(void)
{
    if (!sCursorLargeTried)
    {
        sCursorLarge = UnlzFile("pokenav/region_map/cursor_large.4bpp.lz", NULL);
        sCursorLargeTried = TRUE;
    }
    return sCursorLarge;
}

/* The player's mark and a picked cell's cursor, inside the window. */
static void DrawMapMarks(const ViewState *s)
{
    int saved[4], x, y;

    ClipToMapWindow(saved);
    if (s->mapsec != MAPSEC_NONE && sRes.playerIcon[s->gender])
    {
        CellCentre(s, s->cursorX, s->cursorY, &x, &y);
        DrawSprite(sRes.playerIcon[s->gender], 2, 2, x - 8, y - 8, sRes.playerIconPal[s->gender].c);
    }
    if (s->pickMapsec != MAPSEC_NONE)
    {
        CellCentre(s, s->pickX, s->pickY, &x, &y);
        if (s->mapZoom && LargeCursor())
            DrawSprite(sCursorLarge, 4, 4, x - 16, y - 16, sRes.cursorPal.c);
        else if (sRes.cursorTiles)
            DrawSprite(sRes.cursorTiles, 2, 2, x - 8, y - 8, sRes.cursorPal.c);
    }
    RestoreClip(saved);
}

/* The mark's 2x2 tiles at a cursor cell, clipped to the map's picture. */
static void MarkRect(const ViewState *s, u8 cx, u8 cy, int *x0, int *y0, int *x1, int *y1)
{
    int x, y;

    CellCentre(s, cx, cy, &x, &y);
    *x0 = x - 8;
    *y0 = y - 8;
    *x1 = *x0 + 16;
    *y1 = *y0 + 16;
    if (*x0 < MAP_ORIGIN_X) *x0 = MAP_ORIGIN_X;
    if (*y0 < MAP_ORIGIN_Y) *y0 = MAP_ORIGIN_Y;
    if (*x1 > MAP_ORIGIN_X + MAP_PIC_W) *x1 = MAP_ORIGIN_X + MAP_PIC_W;
    if (*y1 > MAP_ORIGIN_Y + MAP_PIC_H) *y1 = MAP_ORIGIN_Y + MAP_PIC_H;
    if (*x1 < *x0) *x1 = *x0;
    if (*y1 < *y0) *y1 = *y0;
}

/*
 * The Fly map's icons (CreateFlyDestIcons, TryCreateRedOutlineFlyDestIcons):
 * each town and city, lit if visited; the Battle Frontier's red outline.
 * Their sprites kept an 8x8 sprite's centre, so their top left is the
 * section's cell's (the outline's 4 pixels up and left). Zoomed in, twice
 * the size, as the map under them.
 */
static u8 *sFlyIcons;
static Pal sFlyIconPal;
static bool8 sFlyIconsTried;

static void DrawSpriteDouble(const u8 *tiles, int wt, int ht, int x, int y, const u16 *pal)
{
    for (int ty = 0; ty < ht; ++ty)
        for (int tx = 0; tx < wt; ++tx)
        {
            const u8 *tile = tiles + (ty * wt + tx) * 32;

            for (int py = 0; py < 8; ++py)
                for (int px = 0; px < 8; ++px)
                {
                    u8 v = (tile[py * 4 + (px >> 1)] >> ((px & 1) * 4)) & 15;

                    if (v)
                        FillRect(x + (tx * 8 + px) * 2, y + (ty * 8 + py) * 2, 2, 2, pal[v]);
                }
        }
}

static void DrawFlyIcon(const ViewState *s, int mx, int my, int wt, int ht, int frame)
{
    int x, y;

    MapToScreen(s, mx, my, &x, &y);
    if (s->mapZoom)
        DrawSpriteDouble(sFlyIcons + frame * 32, wt, ht, x, y, sFlyIconPal.c);
    else
        DrawSprite(sFlyIcons + frame * 32, wt, ht, x, y, sFlyIconPal.c);
}

static void DrawFlyIcons(const ViewState *s)
{
    int saved[4];

    if (!sFlyIconsTried)
    {
        sFlyIcons = UnlzFile("pokenav/region_map/fly_target_icons.4bpp.lz", NULL);
        PalFile("pokenav/region_map/fly_target_icons.gbapal", &sFlyIconPal, 1, FALSE);
        sFlyIconsTried = TRUE;
    }
    if (!sFlyIcons)
        return;
    ClipToMapWindow(saved);
    for (int mapsec = MAPSEC_LITTLEROOT_TOWN; mapsec <= MAPSEC_EVER_GRANDE_CITY; ++mapsec)
    {
        const struct RegionMapLocation *e = &gRegionMapEntries[mapsec];
        int wt = e->width == 2 ? 2 : 1, ht = wt == 1 && e->height == 2 ? 2 : 1;
        int frame = wt == 2 ? 1 : ht == 2 ? 3 : 0;

        if (!((s->flyVisited >> (mapsec - MAPSEC_LITTLEROOT_TOWN)) & 1))
            frame += 5;
        DrawFlyIcon(s, (e->x + 1) * 8, (e->y + 2) * 8, wt, ht, frame);
    }
    if (s->flyFrontier)
    {
        const struct RegionMapLocation *e = &gRegionMapEntries[MAPSEC_BATTLE_FRONTIER];

        DrawFlyIcon(s, (e->x + 1) * 8 - 4, (e->y + 2) * 8 - 4, 2, 2, 10);
    }
    RestoreClip(saved);
}

/*
 * The picked place's panel, zoomed in, as the PokéNav map's window has it
 * (UpdateMapSecInfoWindow): a town or city one can fly to, its layout
 * (DrawCityMap); a route, the landmarks found so far (PrintLandmarkNames).
 */
static u8 *sCityTiles;
static u32 sCityTileCount;
static Pal sCityPal;
static bool8 sCityTilesTried;
static const u32 *sCityMapKey;
static u16 *sCityMap;

static const u32 *MapCityPicture(u8 mapsec, u8 type, u8 pos)
{
    return mapsec != MAPSEC_NONE && type == MAPSECTYPE_CITY_CANFLY ? CtrPokenav_CityMap(mapsec, pos) : NULL;
}

/* The panel's width, 0 without one. */
static int MapPanelWidth(const ViewState *s)
{
    if (!s->mapZoom || s->pickMapsec == MAPSEC_NONE)
        return 0;
    if (MapCityPicture(s->pickMapsec, s->pickType, s->pickPos))
        return PANEL_CITY_W;
    return s->landmarks ? PANEL_MARKS_W : 0;
}

/* The panel on the right if the place is on the picture's left half. */
static bool8 MapPanelRight(int cx)
{
    return cx * 8 + 4 < MAP_PIC_W / 2;
}

static void MapPanelRect(const ViewState *s, int *x, int *w, int *h)
{
    *w = MapPanelWidth(s);
    *x = MapPanelRight(s->pickX) ? MAP_ORIGIN_X + MAP_PIC_W - 4 - *w : MAP_ORIGIN_X + 4;
    *h = MapCityPicture(s->pickMapsec, s->pickType, s->pickPos) ? PANEL_TITLE_H + 8 + 80 + 6
                                                                 : PANEL_TITLE_H + 8 + s->landmarks * PANEL_ROW_H + 4;
}

static void DrawMapPanel(const ViewState *s)
{
    const u32 *city;
    u8 name[32];
    int x, w, h, oy, ty;

    if (!MapPanelWidth(s))
        return;
    MapPanelRect(s, &x, &w, &h);
    city = MapCityPicture(s->pickMapsec, s->pickType, s->pickPos);
    oy = Plate(x, PANEL_Y, w, h, sCreamPlate, PLATE_STATE_NORMAL);
    GetMapName(name, s->pickMapsec, 0);
    ty = Plate(x + 4, oy + 4, w - 8, PANEL_TITLE_H, sEmerald, PLATE_STATE_NORMAL);
    TextIn(name, x + 4, ty, w - 8, PANEL_TITLE_H, FitFont(&sSmall, name, w - 20), 'c', STYLE_WHITE, 6);
    /* A tap on the panel picks nothing under it. */
    AddHit(x, PANEL_Y, w, h, HIT_MAP_PANEL);
    if (city)
    {
        if (!sCityTilesTried)
        {
            u32 size = 0;

            sCityTiles = Unlz(CtrPokenav_CityZoomTiles(), &size);
            sCityTileCount = size / 32;
            PalFile("pokenav/region_map/zoom_tiles.gbapal", &sCityPal, 1, FALSE);
            sCityTilesTried = TRUE;
        }
        if (sCityMapKey != city)
        {
            free(sCityMap);
            sCityMap = Unlz(city, NULL);
            sCityMapKey = city;
        }
        if (sCityTiles && sCityMap)
        {
            Pal pals[16];

            /* The pictures name their tiles' palette bank as the PokéNav
             * loads it; here the one palette is every bank. */
            for (int i = 0; i < 16; ++i)
                pals[i] = sCityPal;
            for (int cy = 0; cy < 10; ++cy)
                for (int cx = 0; cx < 10; ++cx)
                    DrawMapEntry(sCityTiles, sCityTileCount, sCityMap[cy * 10 + cx], x + (w - 80) / 2 + cx * 8,
                                 oy + PANEL_TITLE_H + 8 + cy * 8, pals);
        }
        return;
    }
    for (int i = 0; i < s->landmarks; ++i)
    {
        const u8 *mark = GetLandmarkName(s->pickMapsec, s->pickPos, i);

        if (mark)
            StyledStr(FitFont(&sSmall, mark, w - 16), mark, x + 8,
                      TextY(oy + PANEL_TITLE_H + 8 + i * PANEL_ROW_H, PANEL_ROW_H, &sSmall), STYLE_INK);
    }
}

/* The zoom button, emerald while zoomed in; its glass says which way. */
static void DrawZoomButton(const ViewState *s)
{
    bool8 pressed = s->pressed == HIT_MAP_ZOOM;
    int oy = Plate(ZOOM_X, ZOOM_Y, ZOOM_W, ZOOM_H, s->mapZoom || pressed ? sEmerald : sLight,
                   pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);

    IconCentred(s->mapZoom ? sArtZoomOut : sArtZoomIn, 2, 2 * ZOOM_X + ZOOM_W, 2 * oy + ZOOM_H - 1, sArtPal.c, FALSE);
    AddHit(ZOOM_X - 2, ZOOM_Y - 2, ZOOM_W + 4, ZOOM_H + 4, HIT_MAP_ZOOM);
}

/* Whether a rectangle of the screen meets what is drawn over the map and is
 * not in its cache: the zoom button, the place panel. */
static bool8 MapOverlayMeets(const ViewState *s, int x0, int y0, int x1, int y1)
{
    int px, pw, ph, b[4];

    PlateBounds(ZOOM_X, ZOOM_Y, ZOOM_W, ZOOM_H, &b[0], &b[1], &b[2], &b[3]);
    if (RectsMeet(x0, y0, x1, y1, b[0], b[1], b[2], b[3]))
        return TRUE;
    if (!MapPanelWidth(s))
        return FALSE;
    MapPanelRect(s, &px, &pw, &ph);
    PlateBounds(px, PANEL_Y, pw, ph, &b[0], &b[1], &b[2], &b[3]);
    return RectsMeet(x0, y0, x1, y1, b[0], b[1], b[2], b[3]);
}

/* The view zoomed in round a cell: in the middle of what the panel leaves. */
static void MapZoomTo(int cx, int cy, int panelW)
{
    int free = MAP_PIC_W - (panelW ? panelW + 8 : 0);
    int left = panelW && !MapPanelRight(cx) ? panelW + 8 : 0;
    int vx = cx * 8 + 4 - (left + free / 2) / 2, vy = cy * 8 + 4 - MAP_PIC_H / 4;

    sMapViewX = vx < 0 ? 0 : vx > MAP_PIC_W / 2 ? MAP_PIC_W / 2 : vx;
    sMapViewY = vy < 0 ? 0 : vy > MAP_PIC_H / 2 ? MAP_PIC_H / 2 : vy;
}

/* How many landmarks a route's panel lists (PrintLandmarkNames). */
static u8 MapLandmarks(u8 mapsec, u8 type, u8 pos)
{
    u8 n = 0;

    if (type == MAPSECTYPE_ROUTE || type == MAPSECTYPE_BATTLE_FRONTIER)
        while (n < PANEL_MARKS_MAX && GetLandmarkName(mapsec, pos, n))
            ++n;
    return n;
}

/* A place's panel width once picked, as MapPanelWidth will have it. */
static int PickPanelWidth(u8 mapsec, u8 pos)
{
    u8 type = CtrRegionMap_MapSecType(mapsec);

    if (MapCityPicture(mapsec, type, pos))
        return PANEL_CITY_W;
    return MapLandmarks(mapsec, type, pos) ? PANEL_MARKS_W : 0;
}

/* The section under a tapped cell, as the region map's cursor finds it
 * (GetRegionMapSecIdAt), and the cell's place in it; zoomed in, the view
 * comes round it. */
static void PickMapCell(int x, int y)
{
    int mx = sMapZoom ? sMapViewX + (x - MAP_ORIGIN_X) / 2 : x - MAP_ORIGIN_X;
    int my = sMapZoom ? sMapViewY + (y - MAP_ORIGIN_Y) / 2 : y - MAP_ORIGIN_Y;
    int cx = mx >> 3, cy = my >> 3;
    u16 mapsec = mx >= 0 && my >= 0 ? GetRegionMapSecIdAt(cx, cy) : MAPSEC_NONE;

    sPickMapsec = MAPSEC_NONE;
    if (mapsec == MAPSEC_NONE)
        return;
    sPickMapsec = mapsec;
    sPickX = cx;
    sPickY = cy;
    sPickPos = CtrRegionMap_PosWithinMapSec(cx, cy, mapsec);
    if (sMapZoom)
        MapZoomTo(cx, cy, PickPanelWidth(mapsec, sPickPos));
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
        CTR_TEXT("GRASS", "HIERBA", "HERBES", "ERBA", "GRAMA", "GRAS", "くさむら"), CTR_TEXT("SURFING", "SURF", "SURF", "SURF", "SURF", "SURFER", "なみのり"),
        CTR_TEXT("ROCK SMASH", "GOLPE ROCA", "ÉCLATE-ROC", "SPACCAROCCIA", "QUEBRA-ROCHA", "ZERTRÜMMERER", "いわくだき"),
        CTR_TEXT("OLD ROD", "CAÑA VIEJA", "CANNE", "AMO VECCHIO", "VARA VELHA", "ANGEL", "ボロのつりざお"),
        CTR_TEXT("GOOD ROD", "CAÑA BUENA", "SUPER CANNE", "AMO BUONO", "VARA BOA", "PROFIANGEL", "いいつりざお"),
        CTR_TEXT("SUPER ROD", "SUPERCAÑA", "MÉGA CANNE", "SUPERAMO", "SUPER VARA", "SUPERANGEL", "すごいつりざお"),
    };
    static const char *const none = CTR_TEXT("NO ENCOUNTER TABLE", "SIN TABLA DE ENCUENTROS", "AUCUNE TABLE ICI",
                                             "NESSUNA TABELLA", "SEM TABELA DE ENCONTROS", "KEINE BEGEGNUNGEN", "でてくる ポケモンは いない");
    static const char *const level = CTR_TEXT("LV", "NV", "N", "LV", "NV", "LV", "Lv");
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
    static const char *const first = CTR_TEXT("SAVED CLOCK NOT RESTORED", "RELOJ GUARDADO NO RESTAURADO",
                                              "HORLOGE SAUVÉE NON RESTAURÉE", "OROLOGIO NON RIPRISTINATO", "RELÓGIO SALVO NÃO RESTAURADO", "GESPEICH. UHR NICHT GELADEN", "セーブの とけいは もどして いない");
    static const char *const second = CTR_TEXT("USING 3DS CLOCK - TAP TO HIDE", "USANDO RELOJ 3DS - TOCA AQUÍ",
                                               "HORLOGE 3DS UTILISÉE - TOUCHER", "OROLOGIO 3DS IN USO - TOCCA", "USANDO RELÓGIO 3DS - TOQUE AQUI", "3DS-UHR AKTIV - TIPPEN: AUS", "3DSの とけい - タッチで けす");
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
        if (s->mapFly)
            DrawFlyIcons(s);
        DrawMapMarks(s);
        AddHit(MAP_ORIGIN_X, MAP_ORIGIN_Y, MAP_PIC_W, MAP_PIC_H, HIT_MAP);
        DrawMapPanel(s);
        DrawZoomButton(s);
    }
    if (s->clockNotice)
    {
        DrawClockNotice(s);
        AddHit(NOTICE_X, NOTICE_Y, NOTICE_W, NOTICE_H, HIT_CLOCK_NOTICE);
    }
}

/*
 * The header while the Fly map is up (FLY from the party or the bag's HM):
 * Y grey, the destination as the title - emerald when one can fly there -
 * then FLY where the Repel and the clock were and CANCEL where the clock and
 * RUN were. The dock is locked meanwhile; MAP, the screen's own, cancels too.
 */
#define FLY_X REPEL_X
#define FLY_W 80
#define FLY_CANCEL_X (FLY_X + FLY_W + 2)
#define FLY_CANCEL_W (RUN_X + RUN_W - FLY_CANCEL_X)

static void DrawFlyHeader(const ViewState *s)
{
    bool8 flyPressed = s->flyCanFly && s->pressed == HIT_MAP_FLY, cancelPressed = s->pressed == HIT_MAP_FLY_CANCEL;
    const u8 *title = gText_FlyToWhere;
    u8 name[32];
    int oy;

    if (s->pickMapsec != MAPSEC_NONE)
    {
        GetMapName(name, s->pickMapsec, 0);
        title = name;
    }
    if (s->flyCanFly)
    {
        oy = Plate(TITLE_X, HEAD_Y, TITLE_W, HEAD_H, sEmerald, PLATE_STATE_NORMAL);
        TextIn(title, TITLE_X, oy, TITLE_W, HEAD_H, FitFont(&sNormal, title, TITLE_W - 12), 'c', STYLE_WHITE, 6);
    }
    else
    {
        oy = PlateEx(TITLE_X, HEAD_Y, TITLE_W, HEAD_H, sLight, PLATE_STATE_NORMAL, TITLE_W - 10, 30, PlateCut(HEAD_H), 0);
        TextIn(title, TITLE_X, oy, TITLE_W - 8, HEAD_H, FitFont(&sNormal, title, TITLE_W - 16), 'l', STYLE_INK, 7);
    }
    oy = Plate(FLY_X, HEAD_Y, FLY_W, HEAD_H, !s->flyCanFly ? sOffGrey : flyPressed ? sEmerald : sFieldBlue,
               flyPressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
    TextIn(gMoveNames[MOVE_FLY], FLY_X, oy, FLY_W, HEAD_H, &sNormal, 'c', s->flyCanFly ? STYLE_WHITE : STYLE_GRAY, 4);
    if (s->flyCanFly)
        AddHit(FLY_X, HEAD_Y - 2, FLY_W, HEAD_H + 4, HIT_MAP_FLY);
    oy = Plate(FLY_CANCEL_X, HEAD_Y, FLY_CANCEL_W, HEAD_H, cancelPressed ? sEmerald : sCancelGrey,
               cancelPressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
    TextIn(gText_Cancel2, FLY_CANCEL_X, oy, FLY_CANCEL_W, HEAD_H, FitFont(&sNormal, gText_Cancel2, FLY_CANCEL_W - 8), 'c',
           STYLE_WHITE, 4);
    AddHit(FLY_CANCEL_X, HEAD_Y - 2, W - FLY_CANCEL_X, HEAD_H + 4, HIT_MAP_FLY_CANCEL);
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
        CardRow(row++, oy, Ascii(CTR_TEXT("NAME", "NOMBRE", "NOM", "NOME", "NOME", "NAME", "なまえ")), s->name);
        CardRow(row++, oy, Ascii(CTR_TEXT("ID No.", "Nº ID", "No. ID", "N. ID", "Nº ID", "ID-Nr.", "IDNo.")),
                Number(s->id, 5, STR_CONV_MODE_LEADING_ZEROS));
        text[0] = CHAR_CURRENCY;
        StringCopy(text + 1, Number(s->money, 6, STR_CONV_MODE_LEFT_ALIGN));
        CardRow(row++, oy, Ascii(CTR_TEXT("MONEY", "DINERO", "ARGENT", "SOLDI", "DINHEIRO", "GELD", "おこづかい")), text);
        if (s->hasDex)
            CardRow(row++, oy, gText_MenuPokedex, Number(s->dex, 3, STR_CONV_MODE_LEFT_ALIGN));
        CardRow(row++, oy, Ascii(CTR_TEXT("TIME", "TIEMPO", "DURÉE", "TEMPO", "TEMPO", "ZEIT", "プレイじかん")), HoursMinutes(s->hours, s->minutes));
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
    {
        /* With the Frontier Pass, FLIP over PASS, each half the height: the
         * pass is the game's, which PLAYER opened in the original. */
        int fh = s->hasPass ? CARD_BADGES_H / 2 - 1 : CARD_BADGES_H;

        oy = Plate(AREA_X + AREA_W - CARD_FLIP_W, CARD_BADGES_Y, CARD_FLIP_W, fh, s->pressed == HIT_OK ? sEmerald : sSlate,
                   s->pressed == HIT_OK ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(Ascii(CTR_TEXT("FLIP", "GIRAR", "TOURNER", "GIRA", "VIRAR", "DREHEN", "うらがえす")), AREA_X + AREA_W - CARD_FLIP_W, oy, CARD_FLIP_W,
               fh, s->hasPass ? &sSmall : &sNormal, 'c', STYLE_WHITE, s->hasPass ? 4 : 6);
        AddHit(AREA_X + AREA_W - CARD_FLIP_W, CARD_BADGES_Y, CARD_FLIP_W, fh, HIT_OK);
        if (s->hasPass)
        {
            int py = CARD_BADGES_Y + CARD_BADGES_H - fh;

            oy = Plate(AREA_X + AREA_W - CARD_FLIP_W, py, CARD_FLIP_W, fh, s->pressed == HIT_YES ? sEmerald : sSlate,
                       s->pressed == HIT_YES ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
            TextIn(Ascii(CTR_TEXT("PASS", "PASE", "PASSE", "TESSERA", "PASSE", "KAMPFPASS", "パス")), AREA_X + AREA_W - CARD_FLIP_W, oy,
                   CARD_FLIP_W, fh, &sSmall, 'c', STYLE_WHITE, 4);
            AddHit(AREA_X + AREA_W - CARD_FLIP_W, py, CARD_FLIP_W, fh, HIT_YES);
        }
    }
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
    SaveRow(0, x, w, oy, Ascii(CTR_TEXT("AREA", "ZONA", "ZONE", "ZONA", "ÁREA", "GEBIET", "ばしょ")), text);
    SaveRow(1, x, w, oy, Ascii(CTR_TEXT("PLAYER", "JUGADOR", "JOUEUR", "GIOCATORE", "JOGADOR", "SPIELER", "プレイヤー")), s->name);
    for (int i = 0; i < NUM_BADGES; ++i)
        badges += (s->badges >> i) & 1;
    SaveRow(2, x, w, oy, Ascii(CTR_TEXT("BADGES", "MEDALLAS", "BADGES", "MEDAGLIE", "INSÍGNIAS", "ORDEN", "バッジ")),
            Number(badges, 1, STR_CONV_MODE_LEFT_ALIGN));
    if (s->hasDex)
        SaveRow(3, x, w, oy, gText_MenuPokedex, Number(s->dex, 3, STR_CONV_MODE_LEFT_ALIGN));
    SaveRow(s->hasDex ? 4 : 3, x, w, oy, Ascii(CTR_TEXT("TIME", "TIEMPO", "DURÉE", "TEMPO", "TEMPO", "ZEIT", "プレイじかん")),
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
        CTR_TEXT("CAUGHT", "CAPTURADO", "CAPTURÉ", "CATTURATO", "CAPTURADO", "GEFANGEN", "つかまえた"),
        CTR_TEXT("MISSED", "PERDIDO", "RATÉ", "PERSO", "PERDIDO", "VERPASST", "にがした"),
        CTR_TEXT("UNUSED", "LIBRE", "LIBRE", "LIBERO", "LIVRE", "FREI", "まだ"),
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
        TextIn(Ascii(col ? CTR_TEXT("MARK FAINTED", "MARCAR DEBILIT.", "MARQUER K.O.", "SEGNA K.O.", "MARCAR DESMAIO", "BESIEGTE MARK.", "ひんしを きろく")
                         : CTR_TEXT("FALLEN MARK", "MARCA CAÍDO", "MARQUE TOMBÉ", "SEGNO CADUTO", "MARCA CAÍDO", "MARKE GEFALLEN", "ひんしの しるし")),
               bx, oy + 1, half, 14, &sSmall, 'c', pressed ? STYLE_WHITE : STYLE_INK, 4);
        TextIn(col ? s->nuzFainted : s->nuzFallenText, bx, oy + 14, half, 14, &sSmall, 'c',
               pressed ? STYLE_WHITE : STYLE_GREEN, 4);
        AddHit(bx, y, half, 30, id);
    }
    y += 33;
    oy = Plate(x, y, w, AREA_Y + AREA_H - y, sCreamPlate, PLATE_STATE_NORMAL);
    TextIn(Ascii(CTR_TEXT("NEXT LEVEL CAP", "NIVEL MÁXIMO", "NIVEAU MAX", "LIVELLO MAX", "NÍVEL MÁXIMO", "LEVEL-GRENZE", "つぎの レベルじょうげん")), x, oy + 1, w / 2, 14,
           &sSmall, 'l', STYLE_GRAY, 8);
    TextIn(s->nuzCap, x + w / 2, oy + 1, w / 2, 14, &sSmall, 'r', STYLE_INK, 8);
    TextIn(Ascii(CTR_TEXT("CAUGHT/MISS/FALLEN", "CAPT./PERD./CAÍDOS", "PRIS/RATÉS/TOMBÉS", "PRESI/PERSI/CADUTI", "CAPT./PERD./CAÍDOS", "GEF./VERP./GEFAL.", "ゲット/にがす/ひんし")),
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
