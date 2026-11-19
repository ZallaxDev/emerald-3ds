/* Part of 3ds_bottom_ui.c: what is loaded from RomFS and decoded once, the sections and options, the icons' caches. */

/* ------------------------------------------------------------------------ */
/* Resources                                                                */
/* ------------------------------------------------------------------------ */

static void *ReadRomfs(const char *path, u32 *outSize)
{
    char full[128];
    void *data;

    snprintf(full, sizeof(full), "graphics/%s", path);
    data = CtrData_Load(full, outSize);
    if (!data)
        CtrLog_Write(CTR_LOG_ERROR, "bottom: %s missing", full);
    return data;
}

/*
 * LZ77 data, whether a game symbol (an asset stub, resolved from RomFS) or a
 * buffer read here. The size comes from the stream's own header.
 */
static void *Unlz(const void *src, u32 *outSize)
{
    const u8 *res = src ? Port_ResolveAssetPointer(src) : NULL;
    u32 header, size;
    void *dst;

    if (!res)
        return NULL;
    header = res[0] | (res[1] << 8) | (res[2] << 16) | ((u32)res[3] << 24);
    size = header >> 8;
    if ((header & 0xFF) != 0x10 || size == 0 || size > 0x10000)
        return NULL;
    dst = malloc(size);
    if (!dst)
        return NULL;
    LZ77UnCompWram((const u32 *)res, dst);
    if (outSize)
        *outSize = size;
    return dst;
}

static void *UnlzFile(const char *path, u32 *outSize)
{
    void *packed = ReadRomfs(path, NULL);
    u32 size = 0;
    void *data = Unlz(packed, &size);
    char plain[96];
    size_t n = strlen(path);

    /* Graphics with English words, redrawn for a translated build: the
     * path without its ".lz", as the game's own sheets are named. */
    if (data != NULL && n > 3 && n - 3 < sizeof(plain))
    {
        memcpy(plain, path, n - 3);
        plain[n - 3] = '\0';
        CtrAssets_TransformAsset(plain, data, size);
    }
    if (outSize != NULL)
        *outSize = size;

    free(packed);
    return data;
}

static bool8 PalFile(const char *path, Pal *dst, int count, bool8 compressed)
{
    u32 size = 0;
    u16 *raw = compressed ? UnlzFile(path, &size) : ReadRomfs(path, &size);

    if (!raw)
        return FALSE;
    if ((int)(size / 32) < count)
        count = size / 32;
    ToPals(dst, raw, count);
    free(raw);
    return TRUE;
}

/* An uncompressed game palette; the pointer may be an asset stub. */
static void PalSymbol(const void *src, Pal *dst, int count)
{
    const u16 *res = src ? Port_ResolveAssetPointer(src) : NULL;

    if (res)
        ToPals(dst, res, count);
}

/* The sections, the dock's buttons left to right; NUZLOCKE only while the
 * tracker is on. */
enum { SCR_MAP, SCR_POKEDEX, SCR_POKEMON, SCR_BAG, SCR_POKENAV, SCR_CARD, SCR_SAVE, SCR_OPTION, SCR_NUZLOCKE,
       SCR_COUNT };
/* The header's buttons, after the dock's for the focus: the registered item
 * (Y), the title (the wild list's toggle) and RUN. */
enum { COL_Y = SCR_COUNT, COL_TITLE, COL_RUN, COL_ITEMS };

/*
 * The game's six options, then the port's own, off by default and kept in
 * settings.txt rather than in the save (3ds_settings.c): the FPS counter and
 * the voxel overworld. Only a build with the counter has its row, and only
 * one with the voxel renderer has the voxel rows.
 */
#ifndef CTR_SHOW_FPS
#define CTR_SHOW_FPS 1
#endif
enum { OPT_TEXT_SPEED, OPT_BATTLE_SCENE, OPT_BATTLE_STYLE, OPT_SOUND, OPT_BUTTON_MODE, OPT_FRAME,
       OPT_FPS, OPT_VOXEL, OPT_VOXEL_PITCH, OPT_VOXEL_ZOOM, OPT_VOXEL_BLUR, OPT_VOXEL_BATTLE,
       OPTION_ROWS };
#define OPTION_SHOWN OPTION_ROWS

typedef struct
{
    u8 *tiles;
    u8 size;        /* 3 for a 24x24 item icon, 4 for 32x32 */
    Pal pal;
} Icon;

static struct
{
    bool8 ready;
    Pal text;                        /* the message box text palette */
    /* Party menu. */
    u8 *partyTiles;
    u32 partyTileCount;
    u16 partyRaw[16 * 11];
    Pal partyPal[11];
    /* Icons: the dock's, and the Repel's in the header. */
    Icon column[SCR_COUNT];
    Icon repel;
    u8 *typeTiles;
    Pal typePal[3];
    const u8 *categoryTiles;
    Pal categoryPal;
    /* The mon icons' palettes (the game has three): the party's, the wild list's. */
    Pal monIconPal[3];
    /* Region map. */
    u8 *mapTiles;
    u32 mapTileCount;
    u8 *mapMap;
    u16 mapPal[32];
    u8 *playerIcon[2];
    Pal playerIconPal[2];
    u8 *cursorTiles;
    Pal cursorPal;
    /* The party menu's held item and mail signs, a tile each. */
    u8 *holdTiles;
    Pal holdPal;
    u8 *statusTiles;
    Pal statusPal;
    /* Trainer card. */
    Pal badgePal;
    u8 *badgeTiles;
    u8 *trainerPic[2];
    Pal trainerPicPal[2];
    /* The Frontier Pass (the new menus'): its map's picture, the mark on a
     * facility, the player's head, the symbols. */
    u8 *passMapTiles;
    u32 passMapTileCount;
    u16 *passMapMap;
    Pal passPal[8];
    u8 *passMarkTiles;
    Pal passMarkPal;
    u8 *passHeadTiles;
    Pal passHeadPal[2];
    u8 *medalTiles;
    Pal medalPal[2];
} sRes;

static void LoadItemIcon(Icon *icon, const void *tiles, const void *pal)
{
    u16 *raw = Unlz(pal, NULL);

    icon->tiles = Unlz(tiles, NULL);
    icon->size = 3;
    if (raw)
    {
        ToPals(&icon->pal, raw, 1);
        free(raw);
    }
}

/* Both players' trainer picture and bag, so nothing is decoded in a frame. */
static void LoadGenderResources(void)
{
    for (u8 gender = MALE; gender <= FEMALE; ++gender)
    {
        u16 pic = gFacilityClassToPicIndex[gender == FEMALE ? FACILITY_CLASS_MAY : FACILITY_CLASS_BRENDAN];
        u16 *pal = Unlz(gTrainerFrontPicPaletteTable[pic].data, NULL);

        sRes.trainerPic[gender] = Unlz(gTrainerFrontPicTable[pic].data, NULL);
        if (pal)
        {
            ToPals(&sRes.trainerPicPal[gender], pal, 1);
            free(pal);
        }
    }
}

static void LoadBattleArt(void);
static void BuildRayquaza(void);
static void BuildTypeColours(void);
static void InitLook(void);

static void LoadResources(void)
{
    u32 size = 0;   /* UnlzFile leaves it alone when it fails */

    PalSymbol(GetOverworldTextboxPalettePtr(), &sRes.text, 1);
    InitLook();

    sRes.partyTiles = UnlzFile("party_menu/bg.4bpp.lz", &size);
    sRes.partyTileCount = size / 32;
    {
        u16 *raw = UnlzFile("party_menu/bg.gbapal.lz", &size);
        if (raw)
        {
            memcpy(sRes.partyRaw, raw, size < sizeof(sRes.partyRaw) ? size : sizeof(sRes.partyRaw));
            free(raw);
        }
        ToPals(sRes.partyPal, sRes.partyRaw, 11);
    }

    /* The dock's icons: the game's own item icons, and the PokéNav's. */
    LoadItemIcon(&sRes.column[SCR_MAP], gItemIcon_TownMap, gItemIconPalette_TownMap);
    LoadItemIcon(&sRes.column[SCR_POKEMON], gItemIcon_PokeBall, gItemIconPalette_PokeBall);
    LoadItemIcon(&sRes.column[SCR_BAG], gItemIcon_BerryPouch, gItemIconPalette_BerryPouch);
    LoadItemIcon(&sRes.column[SCR_CARD], gItemIcon_ContestPass, gItemIconPalette_ContestPass);
    /* Emerald has no Pokédex item icon; port art (Platinum menu Pokédex). */
    sRes.column[SCR_POKEDEX].tiles = (u8 *)sArtPokedex;
    sRes.column[SCR_POKEDEX].size = 3;
    ToPals(&sRes.column[SCR_POKEDEX].pal, sArtPalette, 1);
    LoadItemIcon(&sRes.column[SCR_SAVE], GetItemIconPicOrPalette(ITEM_LETTER, 0), GetItemIconPicOrPalette(ITEM_LETTER, 1));
    LoadItemIcon(&sRes.column[SCR_OPTION], gItemIcon_TeachyTV, gItemIconPalette_TeachyTV);
    sRes.column[SCR_POKENAV].tiles = UnlzFile("pokenav/nav_icon.4bpp.lz", NULL);
    sRes.column[SCR_POKENAV].size = 4;
    PalFile("pokenav/nav_icon.gbapal", &sRes.column[SCR_POKENAV].pal, 1, FALSE);
    /* Port art: a Poké Ball with a cross on it. */
    sRes.column[SCR_NUZLOCKE].tiles = (u8 *)sArtNuzlocke;
    sRes.column[SCR_NUZLOCKE].size = 3;
    ToPals(&sRes.column[SCR_NUZLOCKE].pal, sArtPalette2, 1);
    LoadItemIcon(&sRes.repel, GetItemIconPicOrPalette(ITEM_REPEL, 0), GetItemIconPicOrPalette(ITEM_REPEL, 1));

    sRes.typeTiles = UnlzFile("types/move_types.4bpp.lz", NULL);
    PalFile("types/move_types.gbapal.lz", sRes.typePal, 3, TRUE);
    {
        const u16 *pal;

        sRes.categoryTiles = GetMoveCategoryIconGfx(&pal);
        ToPals(&sRes.categoryPal, pal, 1);
    }
    for (int i = 0; i < (int)ARRAY_COUNT(sRes.monIconPal); ++i)
        PalSymbol(gMonIconPaletteTable[i].data, &sRes.monIconPal[i], 1);

    sRes.mapTiles = UnlzFile("pokenav/region_map/map.8bpp.lz", &size);
    sRes.mapTileCount = size / 64;
    sRes.mapMap = UnlzFile("pokenav/region_map/map.bin.lz", NULL);
    {
        u16 *raw = ReadRomfs("pokenav/region_map/map.gbapal", &size);
        if (raw)
        {
            for (u32 i = 0; i < 32 && i < size / 2; ++i)
                sRes.mapPal[i] = Rgb565(raw[i]);
            free(raw);
        }
    }
    sRes.playerIcon[MALE] = ReadRomfs("pokenav/region_map/brendan_icon.4bpp", NULL);
    sRes.playerIcon[FEMALE] = ReadRomfs("pokenav/region_map/may_icon.4bpp", NULL);
    PalFile("pokenav/region_map/brendan_icon.gbapal", &sRes.playerIconPal[MALE], 1, FALSE);
    PalFile("pokenav/region_map/may_icon.gbapal", &sRes.playerIconPal[FEMALE], 1, FALSE);
    sRes.cursorTiles = UnlzFile("pokenav/region_map/cursor_small.4bpp.lz", NULL);
    PalFile("pokenav/region_map/cursor.gbapal", &sRes.cursorPal, 1, FALSE);

    sRes.holdTiles = ReadRomfs("party_menu/hold_icons.4bpp", NULL);
    PalFile("party_menu/hold_icons.gbapal", &sRes.holdPal, 1, FALSE);
    /* The status icons the party menu and the summary show (PSN to FNT). */
    sRes.statusTiles = Unlz(gStatusGfx_Icons, NULL);
    {
        u16 *raw = Unlz(gStatusPal_Icons, NULL);

        if (raw)
        {
            ToPals(&sRes.statusPal, raw, 1);
            free(raw);
        }
    }

    PalFile("trainer_card/badges.gbapal", &sRes.badgePal, 1, FALSE);
    sRes.badgeTiles = UnlzFile("trainer_card/badges.4bpp.lz", NULL);

    sRes.passMapTiles = UnlzFile("frontier_pass/map_screen.4bpp.lz", &size);
    sRes.passMapTileCount = size / 32;
    sRes.passMapMap = UnlzFile("frontier_pass/map_screen.bin.lz", NULL);
    PalFile("frontier_pass/bg.gbapal", sRes.passPal, 8, FALSE);
    sRes.passMarkTiles = UnlzFile("frontier_pass/map_cursor.4bpp.lz", NULL);
    PalFile("frontier_pass/map_cursor.gbapal", &sRes.passMarkPal, 1, FALSE);
    sRes.passHeadTiles = UnlzFile("frontier_pass/map_heads.4bpp.lz", NULL);
    PalFile("frontier_pass/map_heads.gbapal", &sRes.passHeadPal[MALE], 1, FALSE);
    PalFile("frontier_pass/map_heads_female.gbapal", &sRes.passHeadPal[FEMALE], 1, FALSE);
    sRes.medalTiles = UnlzFile("frontier_pass/medals.4bpp.lz", NULL);
    PalFile("frontier_pass/silver.gbapal", &sRes.medalPal[0], 1, FALSE);
    PalFile("frontier_pass/gold.gbapal", &sRes.medalPal[1], 1, FALSE);

    LoadGenderResources();
    LoadBattleArt();
    BuildTypeColours();
    BuildRayquaza();

    for (int i = 0; i < CACHE_BATTLE; ++i)
        sCache[i] = malloc(sizeof(sCanvas));
    sRes.ready = sRes.partyTiles && sCache[CACHE_MENU] && sCache[CACHE_WIDE];
    if (!sRes.ready)
        CtrLog_Write(CTR_LOG_ERROR, "bottom: party menu graphics or canvas caches missing, screen stays off");
}

/*
 * Icons are read from RomFS, which on hardware is the slowest thing a frame
 * can do. Drawing only ever uses what is already cached; Prefetch loads at
 * most one missing icon per frame, and the screen is redrawn once they are in.
 */
static bool8 sIconBudget;
static u32 sIconClock;

/* Mon icons, one of their two 32x32 frames each: the party (SAVE,
 * NUZLOCKE, the party menu's, which move) and the wild list's page. */
#define MON_ICON_SLOTS 20
static struct
{
    u16 key;
    u32 age;
    u8 tiles[512];
} sMonIcons[MON_ICON_SLOTS];

static const u8 *MonIconFrame(u16 iconSpecies, bool8 deoxysForm, u8 frame)
{
    u16 key = iconSpecies | (deoxysForm ? 0x8000 : 0) | (frame ? 0x4000 : 0);
    int victim = 0;
    const u8 *src;

    for (int i = 0; i < MON_ICON_SLOTS; ++i)
    {
        if (sMonIcons[i].key == key && key != 0)
        {
            sMonIcons[i].age = ++sIconClock;
            return sMonIcons[i].tiles;
        }
        if (sMonIcons[i].age < sMonIcons[victim].age)
            victim = i;
    }
    if (iconSpecies >= SPECIES_EGG + 28 || iconSpecies == SPECIES_NONE || !sIconBudget)
        return NULL;
    sIconBudget = FALSE;
    src = Port_ResolveAssetPointer(gMonIconTable[iconSpecies]);
    if (!src)
        return NULL;
    /* Deoxys keeps its alternate form in the same file, 0x400 in. */
    memcpy(sMonIcons[victim].tiles, src + (deoxysForm ? 0x400 : 0) + (frame ? 0x200 : 0), 512);
    sMonIcons[victim].key = key;
    sMonIcons[victim].age = ++sIconClock;
    return sMonIcons[victim].tiles;
}

static const u8 *MonIcon(u16 iconSpecies, bool8 deoxysForm)
{
    return MonIconFrame(iconSpecies, deoxysForm, 0);
}

/* Whether a mon's icon is in already (or never will be: no such icon), without loading it. */
static bool8 MonIconIn(u16 iconSpecies)
{
    if (iconSpecies >= SPECIES_EGG + 28 || iconSpecies == SPECIES_NONE)
        return TRUE;
    for (int i = 0; i < MON_ICON_SLOTS; ++i)
        if (sMonIcons[i].key == iconSpecies)
            return TRUE;
    return FALSE;
}

/* Item icons: 24x24 with their own palette. */
#define ITEM_ICON_SLOTS 48
static struct
{
    u16 item;
    u32 age;
    bool8 valid, loaded;
    u8 tiles[9 * 32];
    Pal pal;
} sItemIcons[ITEM_ICON_SLOTS];

static int ItemIcon(u16 item)
{
    int victim = 0;
    u32 size = 0;
    u8 *tiles;
    u16 *pal;

    for (int i = 0; i < ITEM_ICON_SLOTS; ++i)
    {
        if (sItemIcons[i].loaded && sItemIcons[i].item == item)
        {
            sItemIcons[i].age = ++sIconClock;
            return sItemIcons[i].valid ? i : -1;
        }
        if (sItemIcons[i].age < sItemIcons[victim].age)
            victim = i;
    }
    if (!sIconBudget)
        return -1;
    sIconBudget = FALSE;
    tiles = Unlz(GetItemIconPicOrPalette(item, 0), &size);
    pal = Unlz(GetItemIconPicOrPalette(item, 1), NULL);
    sItemIcons[victim].valid = tiles && pal && size >= sizeof(sItemIcons[victim].tiles);
    if (sItemIcons[victim].valid)
    {
        memcpy(sItemIcons[victim].tiles, tiles, sizeof(sItemIcons[victim].tiles));
        ToPals(&sItemIcons[victim].pal, pal, 1);
    }
    free(tiles);
    free(pal);
    sItemIcons[victim].item = item;
    sItemIcons[victim].loaded = TRUE;
    sItemIcons[victim].age = ++sIconClock;
    return sItemIcons[victim].valid ? victim : -1;
}

/* Front pictures (the summary's mon): the first of their frames, 64x64,
 * and the palette, normal or shiny. */
#define FRONT_SLOTS 2
static struct
{
    u16 species;
    bool8 shiny, valid, loaded;
    u32 age;
    u8 tiles[64 * 64 / 2];
    Pal pal;
} sFronts[FRONT_SLOTS];

static int FrontPic(u16 species, bool8 shiny)
{
    int victim = 0;
    u32 size = 0;
    u8 *tiles;
    u16 *pal;

    for (int i = 0; i < FRONT_SLOTS; ++i)
    {
        if (sFronts[i].loaded && sFronts[i].species == species && sFronts[i].shiny == shiny)
        {
            sFronts[i].age = ++sIconClock;
            return sFronts[i].valid ? i : -1;
        }
        if (sFronts[i].age < sFronts[victim].age)
            victim = i;
    }
    if (!sIconBudget || species == SPECIES_NONE || species >= NUM_SPECIES + 28)
        return -1;
    sIconBudget = FALSE;
    tiles = Unlz(gMonFrontPicTable[species].data, &size);
    pal = Unlz((shiny ? gMonShinyPaletteTable : gMonPaletteTable)[species].data, NULL);
    sFronts[victim].valid = tiles && pal && size >= sizeof(sFronts[victim].tiles);
    if (sFronts[victim].valid)
    {
        memcpy(sFronts[victim].tiles, tiles, sizeof(sFronts[victim].tiles));
        ToPals(&sFronts[victim].pal, pal, 1);
    }
    free(tiles);
    free(pal);
    sFronts[victim].species = species;
    sFronts[victim].shiny = shiny;
    sFronts[victim].loaded = TRUE;
    sFronts[victim].age = ++sIconClock;
    return sFronts[victim].valid ? victim : -1;
}

/* The bag's own picture, the frame of a pocket, male or female. */
static struct
{
    bool8 female, valid, loaded;
    u8 frame;
    u8 tiles[64 * 64 / 2];
    Pal pal;
} sBagPic;

static int BagPic(bool8 female, u8 frame)
{
    u32 size = 0;
    u8 *tiles;
    u16 *pal;

    if (sBagPic.loaded && sBagPic.female == female && sBagPic.frame == frame)
        return sBagPic.valid ? 0 : -1;
    if (!sIconBudget)
        return -1;
    sIconBudget = FALSE;
    tiles = Unlz(female ? gBagFemaleTiles : gBagMaleTiles, &size);
    pal = Unlz(gBagPalette, NULL);
    sBagPic.valid = tiles && pal && size >= (frame + 1) * sizeof(sBagPic.tiles);
    if (sBagPic.valid)
    {
        memcpy(sBagPic.tiles, tiles + frame * sizeof(sBagPic.tiles), sizeof(sBagPic.tiles));
        ToPals(&sBagPic.pal, pal, 1);
    }
    free(tiles);
    free(pal);
    sBagPic.female = female;
    sBagPic.frame = frame;
    sBagPic.loaded = TRUE;
    return sBagPic.valid ? 0 : -1;
}

static void DrawItemIcon(u16 item, int x, int y)
{
    int slot = ItemIcon(item);

    if (slot >= 0)
        DrawSprite(sItemIcons[slot].tiles, 3, 3, x, y, sItemIcons[slot].pal.c);
}
