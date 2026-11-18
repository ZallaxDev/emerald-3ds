/* Part of 3ds_bottom_ui.c: the snapshot of everything a redraw reads, and the pictures fetched for it. */

/* ------------------------------------------------------------------------ */
/* Snapshots                                                                */
/* ------------------------------------------------------------------------ */

static u8 PlayerRegionPosition(u8 *outX, u8 *outY)
{
    /* region_map.c's InitMapBasedOnPlayerLocation, without its UI state. */
    const struct MapHeader *header = &gMapHeader;
    u16 mapWidth, mapHeight, x, y, scale;
    u8 mapsec;

    switch (GetMapTypeByGroupAndId(gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum))
    {
    case MAP_TYPE_UNDERGROUND:
    case MAP_TYPE_UNKNOWN:
        if (gMapHeader.allowEscaping)
        {
            header = Overworld_GetMapHeaderByGroupAndId(gSaveBlock1Ptr->escapeWarp.mapGroup,
                                                        gSaveBlock1Ptr->escapeWarp.mapNum);
            x = gSaveBlock1Ptr->escapeWarp.x;
            y = gSaveBlock1Ptr->escapeWarp.y;
            mapsec = header->regionMapSectionId;
        }
        else
        {
            mapsec = gMapHeader.regionMapSectionId;
            header = NULL;
            x = y = 1;
        }
        break;
    case MAP_TYPE_SECRET_BASE:
        header = Overworld_GetMapHeaderByGroupAndId(gSaveBlock1Ptr->dynamicWarp.mapGroup,
                                                    gSaveBlock1Ptr->dynamicWarp.mapNum);
        x = gSaveBlock1Ptr->dynamicWarp.x;
        y = gSaveBlock1Ptr->dynamicWarp.y;
        mapsec = header->regionMapSectionId;
        break;
    case MAP_TYPE_INDOOR:
    {
        const struct WarpData *warp = gMapHeader.regionMapSectionId != MAPSEC_DYNAMIC
                                    ? &gSaveBlock1Ptr->escapeWarp : &gSaveBlock1Ptr->dynamicWarp;
        header = Overworld_GetMapHeaderByGroupAndId(warp->mapGroup, warp->mapNum);
        mapsec = gMapHeader.regionMapSectionId != MAPSEC_DYNAMIC ? gMapHeader.regionMapSectionId
                                                                 : header->regionMapSectionId;
        x = warp->x;
        y = warp->y;
        break;
    }
    default:
        mapsec = gMapHeader.regionMapSectionId;
        x = gSaveBlock1Ptr->pos.x;
        y = gSaveBlock1Ptr->pos.y;
        break;
    }
    if (mapsec >= MAPSEC_NONE)
        return MAPSEC_NONE;
    mapWidth = header && header->mapLayout ? header->mapLayout->width : 1;
    mapHeight = header && header->mapLayout ? header->mapLayout->height : 1;
    if (gRegionMapEntries[mapsec].width && gRegionMapEntries[mapsec].height)
    {
        scale = mapWidth / gRegionMapEntries[mapsec].width;
        x /= scale ? scale : 1;
        if (x >= gRegionMapEntries[mapsec].width) x = gRegionMapEntries[mapsec].width - 1;
        scale = mapHeight / gRegionMapEntries[mapsec].height;
        y /= scale ? scale : 1;
        if (y >= gRegionMapEntries[mapsec].height) y = gRegionMapEntries[mapsec].height - 1;
    }
    else
    {
        x = y = 0;
    }
    /* 1 and 2 are region_map.c's MAPCURSOR_X_MIN / MAPCURSOR_Y_MIN. */
    *outX = gRegionMapEntries[mapsec].x + x + 1;
    *outY = gRegionMapEntries[mapsec].y + y + 2;
    return mapsec;
}

/*
 * The Fly cinematic (CtrVideo_FlyProgress): the player's mark leaves its
 * cell as the flight begins and glides to the destination's, arriving as
 * the camera lands, so the map below shows the journey. The route is taken
 * once, as the flight starts (before the warp moves the player).
 */
static struct { bool8 on; int fromMx, fromMy, toMx, toMy; } sFlyRoute;

static void MapFlyGlide(ViewState *s)
{
    float share, t;

    s->flyGlide = 0;
    s->flyMx = s->flyMy = 0;
    if (!CtrVideo_FlyProgress(&share))
    {
        sFlyRoute.on = FALSE;
        return;
    }
    if (!sFlyRoute.on)
    {
        const struct MapHeader *dest = GetDestinationWarpMapHeader();
        u8 to = dest != NULL ? dest->regionMapSectionId : MAPSEC_NONE;
        const struct RegionMapLocation *e;

        if (s->mapsec == MAPSEC_NONE || to >= MAPSEC_NONE)
            return;
        e = &gRegionMapEntries[to];
        sFlyRoute.fromMx = s->cursorX * 8 + 4;
        sFlyRoute.fromMy = s->cursorY * 8 + 4;
        /* The destination's middle, in PlayerRegionPosition's cell space. */
        sFlyRoute.toMx = (e->x + 1) * 8 + e->width * 4;
        sFlyRoute.toMy = (e->y + 2) * 8 + e->height * 4;
        sFlyRoute.on = TRUE;
    }
    t = share * share * (3.0f - 2.0f * share);
    s->flyGlide = 1;
    s->flyMx = (s16)(sFlyRoute.fromMx + (sFlyRoute.toMx - sFlyRoute.fromMx) * t + 0.5f);
    s->flyMy = (s16)(sFlyRoute.fromMy + (sFlyRoute.toMy - sFlyRoute.fromMy) * t + 0.5f);
}

static void CopyText(u8 *dst, int size, const u8 *src)
{
    int n = 0;

    while (src && n < size - 1 && src[n] != EOS)
    {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = EOS;
}

/* trainer_card.c's GetRubyTrainerStars as Emerald fills it in. */
static u8 CardStars(void)
{
    return (GetGameStat(GAME_STAT_ENTERED_HOF) != 0) + (HasAllHoennMons() != 0)
         + (CountPlayerMuseumPaintings() >= CONTEST_CATEGORIES_COUNT);
}

static void SnapshotCard(ViewState *s)
{
    static u16 dex, frames;
    static u8 stars;

    StringCopy(s->name, gSaveBlock2Ptr->playerName);
    s->id = gSaveBlock2Ptr->playerTrainerId[0] | (gSaveBlock2Ptr->playerTrainerId[1] << 8);
    s->money = GetMoney(&gSaveBlock1Ptr->money);
    s->hours = gSaveBlock2Ptr->playTimeHours > 999 ? 999 : gSaveBlock2Ptr->playTimeHours;
    s->minutes = gSaveBlock2Ptr->playTimeMinutes > 59 ? 59 : gSaveBlock2Ptr->playTimeMinutes;
    s->hasDex = FlagGet(FLAG_SYS_POKEDEX_GET);
    s->hasPass = FlagGet(FLAG_SYS_FRONTIER_PASS);
    /* Counting the Pokédex walks every species; twice a second is plenty. */
    if ((frames++ % 30) == 0)
    {
        dex = !s->hasDex ? 0 : IsNationalPokedexEnabled() ? GetNationalPokedexCount(FLAG_GET_CAUGHT)
                                                         : GetHoennPokedexCount(FLAG_GET_CAUGHT);
        stars = CardStars();
    }
    s->dex = dex;
    s->stars = stars;
    for (int i = 0; i < NUM_BADGES; ++i)
        if (FlagGet(FLAG_BADGE01_GET + i))
            s->badges |= 1 << i;
}

static u16 CappedStat(u8 stat, u32 cap)
{
    u32 value = GetGameStat(stat);

    return value > cap ? cap : value;
}

/* The Frontier Pass, as frontier_pass.c's AllocateFrontierPassData fills
 * it in; on its map, where the player's head is. */
static void SnapshotPass(ViewState *s)
{
    s->hasPass = TRUE;
    s->cardView = sCardView;
    s->passPick = sPassPick;
    s->passMapPos = sPassMapPos;
    s->passRecord = sPassRecord;
    s->battlePoints = gSaveBlock2Ptr->frontier.battlePoints;
    for (int i = 0; i < NUM_FRONTIER_FACILITIES; ++i)
        s->symbols |= (FlagGet(FLAG_SYS_TOWER_SILVER + i * 2) + FlagGet(FLAG_SYS_TOWER_GOLD + i * 2)) << (2 * i);
    if (sCardView == CARD_VIEW_MAP)
        s->passHead = CtrFrontierPass_PlayerHead(&s->passHeadX, &s->passHeadY);
}

/* The card's back, as trainer_card.c's SetPlayerCardData fills it in. */
static void SnapshotCardBack(ViewState *s)
{
    u32 hof = GetGameStat(GAME_STAT_FIRST_HOF_PLAY_TIME);

    s->cardBack = TRUE;
    s->hofHours = hof >> 16;
    s->hofMinutes = (hof >> 8) & 0xFF;
    if (s->hofHours > 999)
    {
        s->hofHours = 999;
        s->hofMinutes = 59;
    }
    s->linkWins = CappedStat(GAME_STAT_LINK_BATTLE_WINS, 9999);
    s->linkLosses = CappedStat(GAME_STAT_LINK_BATTLE_LOSSES, 9999);
    s->trades = CappedStat(GAME_STAT_POKEMON_TRADES, 0xFFFF);
    s->pokeblocks = CappedStat(GAME_STAT_POKEBLOCKS_WITH_FRIENDS, 0xFFFF);
    s->contests = CappedStat(GAME_STAT_WON_LINK_CONTEST, 999);
}

/* The party's icons, a fainted one grey. */
static void SnapshotParty(ViewState *s)
{
    for (int i = 0; i < PARTY_SIZE && i < gPlayerPartyCount; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u16 species = GetMonData(mon, MON_DATA_SPECIES);
        MonView *view = &s->party[i];

        if (species == SPECIES_NONE)
            continue;
        view->species = species;
        view->isEgg = GetMonData(mon, MON_DATA_IS_EGG);
        view->iconSpecies = view->isEgg ? SPECIES_EGG : GetIconSpecies(species, GetMonData(mon, MON_DATA_PERSONALITY));
        view->deoxys = species == SPECIES_DEOXYS;
        view->fainted = !view->isEgg && GetMonData(mon, MON_DATA_HP) == 0;
    }
}

/* The party as its own menu shows it: names, levels, genders, HP. */
static void SnapshotPartyFull(ViewState *s)
{
    SnapshotParty(s);
    for (int i = 0; i < PARTY_SIZE && i < gPlayerPartyCount; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        MonView *view = &s->party[i];

        if (!view->species)
            continue;
        view->level = GetMonData(mon, MON_DATA_LEVEL);
        view->hp = GetMonData(mon, MON_DATA_HP);
        view->maxHp = GetMonData(mon, MON_DATA_MAX_HP);
        /* As the party menu reads it: fainted too, and Pokérus. */
        view->ailment = GetMonAilment(mon);
        {
            u16 item = GetMonData(mon, MON_DATA_HELD_ITEM);

            view->held = item == ITEM_NONE ? HELD_NONE : ItemIsMail(item) ? HELD_MAIL : HELD_ITEM;
        }
        GetMonData(mon, MON_DATA_NICKNAME, view->nick);
        StringGet_Nickname(view->nick);
        view->gender = view->isEgg ? MON_GENDERLESS : GetMonGender(mon);
        /* The game shows no sign by a NIDORAN still called so. */
        if ((view->species == SPECIES_NIDORAN_M || view->species == SPECIES_NIDORAN_F)
         && StringCompare(view->nick, gSpeciesNames[view->species]) == 0)
            view->gender = MON_GENDERLESS;
    }
}

/* Which native view the mode on show has, NATIVE_NONE for none. */
static u8 NativeKind(const ViewState *s)
{
    if (sClassicMenus)
        return NATIVE_NONE;
    if (s->mode == MODE_SUMMARY)
        return NATIVE_SUMMARY;
    if (s->mode == MODE_POKEDEX)
        return NATIVE_DEX;
    if (s->mode == MODE_BAG_MENU && s->bagView == BAG_VIEW_SECTION)
        return NATIVE_BAG;
    if (s->mode == MODE_PARTY_MENU && s->dock)
        return NATIVE_PARTY;
    return NATIVE_NONE;
}

/* The game's screen on show, drawn by the bottom screen itself: what it
 * reads of it. With nothing to read for a moment (the menu coming in or
 * going out, a state it has no view of) what was drawn stays; with nothing
 * drawn yet, the section's empty backdrop (NATIVE_BLANK). The game's
 * picture is never shown for these. What stays is stale: still drawn, but
 * its plates are no longer the game's, so a tap on them is not taken. */
static bool8 sNativeStale;

static void SnapshotNative(ViewState *s)
{
    u8 kind = NativeKind(s);
    bool8 got = FALSE;

    /* No native view now: one still shown is stale too, until redrawn. */
    sNativeStale = kind == NATIVE_NONE && sShown.native != NATIVE_NONE;
    if (kind == NATIVE_NONE)
        return;
    switch (kind)
    {
    case NATIVE_SUMMARY:
        got = CtrSummary_GetView(&s->sv) && s->sv.state != CTR_SUM_VIEW_NONE;
        break;
    case NATIVE_DEX:
        got = CtrPokedex_GetView(&s->dx) && s->dx.state != CTR_DEX_VIEW_NONE;
        break;
    case NATIVE_BAG:
        got = CtrBag_GetView(&s->bv) && s->bv.state != CTR_BAG_VIEW_NONE;
        break;
    case NATIVE_PARTY:
        got = CtrParty_GetView(&s->pv) && s->pv.state != CTR_PARTY_VIEW_NONE;
        break;
    }
    if (got)
        s->native = kind;
    else if (sShown.native == kind)
    {
        sNativeStale = TRUE;
        s->native = kind;
        s->pv = sShown.pv;
        s->sv = sShown.sv;
        s->bv = sShown.bv;
        s->dx = sShown.dx;
    }
    else
    {
        memset(&s->pv, 0, sizeof(s->pv));
        memset(&s->sv, 0, sizeof(s->sv));
        memset(&s->bv, 0, sizeof(s->bv));
        memset(&s->dx, 0, sizeof(s->dx));
        s->native = NATIVE_BLANK;
    }
    if (s->native == NATIVE_PARTY)
        SnapshotPartyFull(s);
}

/* The save view's file: the card's figures, where the player is, the party. */
static void SnapshotSave(ViewState *s)
{
    SnapshotCard(s);
    s->mapsec = gMapHeader.regionMapSectionId;
    SnapshotParty(s);
}

/* The NUZLOCKE section: the tracker's own figures and texts. */
static void SnapshotNuzlocke(ViewState *s)
{
    int slot = CtrNuzlockeUi_Slot();

    SnapshotParty(s);
    s->nuzStatus = CtrNuzlockeUi_AreaStatus();
    s->nuzSlot = slot < 0 ? 0xFF : slot;
    for (int i = 0; i < PARTY_SIZE; ++i)
        if (CtrNuzlockeUi_SlotFallen(i))
            s->nuzFallen |= 1 << i;
    CopyText(s->nuzArea, NUZ_TEXT, CtrNuzlockeUi_AreaText());
    CopyText(s->nuzLink, NUZ_TEXT, CtrNuzlockeUi_LinkText());
    CopyText(s->nuzFallenText, NUZ_TEXT, CtrNuzlockeUi_FallenText());
    CopyText(s->nuzFainted, NUZ_TEXT, CtrNuzlockeUi_MarkFaintedText());
    CopyText(s->nuzCap, NUZ_TEXT, CtrNuzlockeUi_CapText());
    CopyText(s->nuzTotals, NUZ_TEXT, CtrNuzlockeUi_TotalsText());
}

/* The party ball states, as battle_interface.c's party indicator has them. */
enum { PARTY_BALL_OK, PARTY_BALL_STATUS, PARTY_BALL_FAINT, PARTY_BALL_EMPTY };

/* The focus ring's blink tone, by the clock and not by game frames (16 of
 * them at 60 Hz, 267 ms): fast-forward runs several game frames in a shown
 * one, and a ring counted in them blinked as many times faster. */
static u8 BlinkPhase(void)
{
    return (CtrPlatform_Milliseconds() / 267) & 1;
}

static void SnapshotBattle(ViewState *s)
{
    s->isDouble = (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) != 0;
    s->safari = (gBattleTypeFlags & BATTLE_TYPE_SAFARI) != 0;
    if (s->mode == MODE_BATTLE_INFO)
        return;
    s->blink = BlinkPhase();
}

/* The six party balls: the mons in party order, then the empty places (an
 * egg is one, as in the game's indicator). */
static void SnapshotPartyBalls(ViewState *s)
{
    int n = 0;

    for (int i = 0; i < PARTY_SIZE; ++i)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);

        if (species == SPECIES_NONE || species == SPECIES_EGG)
            continue;
        if (GetMonData(mon, MON_DATA_HP) == 0)
            s->partyBalls[n++] = PARTY_BALL_FAINT;
        else if (GetMonData(mon, MON_DATA_STATUS) != 0)
            s->partyBalls[n++] = PARTY_BALL_STATUS;
        else
            s->partyBalls[n++] = PARTY_BALL_OK;
    }
    while (n < PARTY_SIZE)
        s->partyBalls[n++] = PARTY_BALL_EMPTY;
}

/* The battlers the target plates show. */
static void SnapshotTargets(ViewState *s, u8 user)
{
    for (int i = 0; i < MAX_BATTLERS_COUNT; ++i)
    {
        TargetView *t = &s->targets[i];
        u8 b = GetBattlerAtPosition(sTargetPlatePos[i]);
        const struct BattlePokemon *mon = &gBattleMons[b];

        if (b >= gBattlersCount || (gAbsentBattlerFlags & (1u << b)) || mon->species == SPECIES_NONE)
            continue;
        t->species = mon->species;
        t->iconSpecies = GetIconSpecies(mon->species, mon->personality);
        t->deoxys = mon->species == SPECIES_DEOXYS;
        t->hp = mon->hp;
        t->maxHp = mon->maxHP;
        t->level = mon->level;
        t->gender = GetGenderFromSpeciesAndPersonality(mon->species, mon->personality);
        t->live = TargetLive(b, user);
        {
            int k = 0;

            for (; k < POKEMON_NAME_LENGTH && mon->nickname[k] != EOS; ++k)
                t->nick[k] = mon->nickname[k];
            t->nick[k] = EOS;
        }
    }
}

/* Every option's value as the OPTIONS screen shows it. */
static void ReadOptionValues(u8 options[OPTION_ROWS])
{
    options[0] = gSaveBlock2Ptr->optionsTextSpeed;
    options[1] = gSaveBlock2Ptr->optionsBattleSceneOff;
    options[2] = gSaveBlock2Ptr->optionsBattleStyle;
    options[3] = gSaveBlock2Ptr->optionsSound;
    options[4] = gSaveBlock2Ptr->optionsButtonMode;
    options[5] = gSaveBlock2Ptr->optionsWindowFrameType;
    options[OPT_FPS] = CtrSettings_ShowFps();
    options[OPT_VOXEL] = CtrSettings_Voxel();
    options[OPT_VOXEL_PITCH] = CtrSettings_VoxelPitch();
    options[OPT_VOXEL_ZOOM] = CtrSettings_VoxelZoom();
    options[OPT_VOXEL_BLUR] = CtrSettings_VoxelBlurStrength();
    options[OPT_VOXEL_BATTLE] = CtrSettings_VoxelBattle();
}

/*
 * Map extras (3ds_field_qol_extras.inc). The wild list reads the area's
 * normal tables as the game's encounters do: the map's header in
 * gWildMonHeaders (Altering Cave's set by VAR_ALTERING_CAVE_WILD_SET), the
 * grass table through CtrEncounters_Land, so the timed tables (WILD POKEMON:
 * TIMED) show what the hour gives. Each method with a table is a page: grass,
 * surfing, Rock Smash, and the three rods (their slots of the fishing table).
 * Not shown, as they are not in those tables: Feebas's tiles, a TV outbreak's
 * species, the roamers, the Battle Pyramid's and Pike's own tables, static and
 * scripted Pokemon. A later randomizer (CtrRandomizer_Wild) is to map the
 * species here too, once its API is final.
 */
enum { ENC_LAND, ENC_WATER, ENC_ROCK, ENC_OLD_ROD, ENC_GOOD_ROD, ENC_SUPER_ROD, ENC_METHODS };

/* The game's slot chances (wild_encounter.c), in percent; the fishing table's
 * slots are the old rod's 0-1, the good rod's 2-4, the super rod's 5-9. */
static const u8 sEncLandRates[12] = {20, 20, 10, 10, 10, 10, 5, 5, 4, 4, 1, 1};
static const u8 sEncWaterRates[5] = {60, 30, 5, 4, 1};
static const u8 sEncFishRates[10] = {70, 30, 60, 20, 20, 40, 40, 15, 4, 1};
static const u8 sEncFirst[ENC_METHODS] = {0, 0, 0, 0, 2, 5};
static const u8 sEncSlots[ENC_METHODS] = {12, 5, 5, 2, 3, 5};

/* The slots [first, first + count) as rows, one a species in slot order,
 * their chances added up and their levels' range; how many rows. */
static u8 EncCollect(EncRow *rows, const struct WildPokemon *mons, u8 first, u8 count, const u8 *rates)
{
    u8 n = 0;

    for (u8 i = 0; i < count; ++i)
    {
        const struct WildPokemon *mon = &mons[first + i];
        u8 k = 0;
        u16 species;

        if (mon->species == SPECIES_NONE)
            continue;
        species = CtrEncounters_DisplaySpecies(mon->species);
        if (species == SPECIES_NONE)
            continue;
        while (k < n && rows[k].species != species)
            ++k;
        if (k == n)
        {
            if (n == ENC_ROWS)
                continue;
            rows[n].species = species;
            rows[n].minLevel = mon->minLevel;
            rows[n].maxLevel = mon->maxLevel;
            rows[n].percent = 0;
            rows[n].seen = 0;
            ++n;
        }
        if (mon->minLevel < rows[k].minLevel)
            rows[k].minLevel = mon->minLevel;
        if (mon->maxLevel > rows[k].maxLevel)
            rows[k].maxLevel = mon->maxLevel;
        rows[k].percent += rates[first + i];
    }
    return n;
}

/* The current map's wild header, as the game finds it, or NULL. */
static const struct WildPokemonHeader *EncHeader(void)
{
    u8 group = gSaveBlock1Ptr->location.mapGroup, num = gSaveBlock1Ptr->location.mapNum;

    for (u16 i = 0; gWildMonHeaders[i].mapGroup != MAP_GROUP(MAP_UNDEFINED); ++i)
    {
        if (gWildMonHeaders[i].mapGroup != group || gWildMonHeaders[i].mapNum != num)
            continue;
        /* Altering Cave's sets follow each other; an unknown set is the first, as in the game. */
        if (group == MAP_GROUP(MAP_ALTERING_CAVE) && num == MAP_NUM(MAP_ALTERING_CAVE))
        {
            u16 set = VarGet(VAR_ALTERING_CAVE_WILD_SET), run = 0;

            while (gWildMonHeaders[i + run].mapGroup == group && gWildMonHeaders[i + run].mapNum == num)
                ++run;
            if (set < run)
                i += set;
        }
        return &gWildMonHeaders[i];
    }
    return NULL;
}

/* Each method with a table, ENC_SHOWN rows a page: the page on show. */
static void SnapshotEncounters(ViewState *s)
{
    const struct WildPokemonHeader *header = EncHeader();
    const struct WildPokemonInfo *info[ENC_METHODS] = {NULL};
    static const u8 *const rates[ENC_METHODS] = {sEncLandRates, sEncWaterRates, sEncWaterRates,
                                                 sEncFishRates, sEncFishRates, sEncFishRates};
    EncRow rows[ENC_ROWS];
    u8 counts[ENC_METHODS] = {0}, pages = 0, page;

    if (header)
    {
        info[ENC_LAND] = CtrEncounters_Land(header->landMonsInfo, gSaveBlock1Ptr->location.mapGroup,
                                            gSaveBlock1Ptr->location.mapNum);
        info[ENC_WATER] = header->waterMonsInfo;
        info[ENC_ROCK] = header->rockSmashMonsInfo;
        info[ENC_OLD_ROD] = info[ENC_GOOD_ROD] = info[ENC_SUPER_ROD] = header->fishingMonsInfo;
    }
    for (u8 m = 0; m < ENC_METHODS; ++m)
        if (info[m] && info[m]->wildPokemon)
        {
            counts[m] = EncCollect(rows, info[m]->wildPokemon, sEncFirst[m], sEncSlots[m], rates[m]);
            pages += counts[m] ? (counts[m] + ENC_SHOWN - 1) / ENC_SHOWN : 1;
        }
    s->encPages = pages;
    if (pages == 0)
        return;
    s->encPage = page = sEncPage % pages;
    for (u8 m = 0; m < ENC_METHODS; ++m)
    {
        u8 n;

        if (!info[m] || !info[m]->wildPokemon)
            continue;
        n = counts[m] ? (counts[m] + ENC_SHOWN - 1) / ENC_SHOWN : 1;
        if (page >= n)
        {
            page -= n;
            continue;
        }
        EncCollect(rows, info[m]->wildPokemon, sEncFirst[m], sEncSlots[m], rates[m]);
        s->encMethod = m;
        s->encCount = counts[m] - page * ENC_SHOWN > ENC_SHOWN ? ENC_SHOWN : counts[m] - page * ENC_SHOWN;
        memcpy(s->enc, rows + page * ENC_SHOWN, s->encCount * sizeof(EncRow));
        break;
    }
    /* Only what the Pokedex has seen is named, and what it has caught marked.
     * FLAG_GET_CAUGHT clears the species' flags when they disagree: asked only
     * of a species seen, whose flags agree, it reads and changes nothing. */
    for (u8 i = 0; i < s->encCount; ++i)
    {
        u16 national = SpeciesToNationalPokedexNum(s->enc[i].species);

        if (GetSetPokedexFlag(national, FLAG_GET_SEEN) == 0)
            s->enc[i].seen = ENC_UNSEEN;
        else
            s->enc[i].seen = GetSetPokedexFlag(national, FLAG_GET_CAUGHT) != 0 ? ENC_CAUGHT : ENC_SEEN;
    }
}

static void SnapshotMapExtras(ViewState *s)
{
    s->encOn = CtrExtras_Get("map_encounters") == 1;
    s->encView = s->encOn && sEncView;
    if (s->encView)
    {
        /* The list has no marks: walking in the area redraws nothing. */
        s->cursorX = s->cursorY = 0;
        s->pickMapsec = MAPSEC_NONE;
        s->pickX = s->pickY = 0;
        SnapshotEncounters(s);
    }
    s->clockNotice = CtrExtras_Get("clock_notice") == 1 && CtrClockStore_NeedsNotice(CtrClockStore_Status())
                  && CtrClockStore_LoadSerial() != sClockNoticeSerial;
}

/* The header's clock: the hour always; the part of the day and the clock's
 * speed only with the day/night clock or SHOW CLOCK on. Off, the clock is
 * not running: the hour is the game's own. */
static void SnapshotClock(ViewState *s)
{
    unsigned seconds = 0, rate = 1;

    s->clockShown = CtrSettings_GetInt("day_night", 0) == 1 || CtrSettings_GetInt("show_clock", 0) == 1;
    s->clockValid = CtrDayNight_Time(&seconds, &rate);
    if (!s->clockValid)
        s->clockValid = CtrGame_LocalTimeSeconds(&seconds);
    if (s->clockValid)
    {
        s->clockMinute = seconds / 60;
        s->clockRate = rate;
        s->clockPhase = CtrEncounters_Phase(seconds);
    }
}

static void SnapshotOptionRows(ViewState *s);
static u8 MapLandmarks(u8 mapsec, u8 type, u8 pos);

/* The map's own FLY: with a town picked one can fly to, the Feather Badge
 * and a Pokemon that knows Fly, ready where the field lets Fly be used and
 * grey where it does not (indoors, a script); the party's icons for the
 * choice of who flies. */
static void SnapshotMapFly(ViewState *s)
{
    u8 flyers;

    if (s->mode != MODE_FIELD || s->inBattle || s->encView || s->pickMapsec == MAPSEC_NONE
     || (s->pickType != MAPSECTYPE_CITY_CANFLY && s->pickType != MAPSECTYPE_BATTLE_FRONTIER)
     || !FlagGet(FLAG_BADGE06_GET))
        return;
    flyers = MapFlyers();
    if (flyers == 0)
        return;
    s->flyers = flyers;
    s->flyPick = MapFlyerSlot(flyers);
    s->flyNow = MapFlyField() ? MAP_FLY_READY : MAP_FLY_GREY;
    SnapshotParty(s);
}

static void Snapshot(ViewState *s, u8 mode, u8 pressed)
{
    memset(s, 0, sizeof(*s));
    s->mode = mode;
    s->pressed = pressed;
    s->pickMapsec = MAPSEC_NONE;
    if (s->mode == MODE_OFF)
    {
        s->title = CtrTitleScreen_RayquazaBg() >= 0;
        s->dim = sPreField && !s->title;
        /* The title menu, the professor's questions: the game's own menus, as buttons. */
        CtrFieldMenu_GetView(&s->fm);
        CtrFieldList_GetView(&s->fl);
        return;
    }
    if (mode == MODE_PAD)
    {
        s->screen = SCR_COUNT;
        CtrBlender_GetView(&s->bl);
        CtrContest_GetView(&s->co);
        CtrEasyChat_GetView(&s->ec);
        CtrFactory_GetView(&s->fac);
        /* The Rental Pokemon's plates come up whole: once every icon on them is in. */
        s->fac.loaded = s->fac.count != 0;
        for (int i = 0; i < s->fac.count && i < CTR_FAC_MONS; ++i)
            if (!MonIconIn(GetIconSpecies(s->fac.species[i], s->fac.personality[i])))
                s->fac.loaded = FALSE;
        CtrFieldMenu_GetView(&s->fm);
        CtrFieldList_GetView(&s->fl);
        return;
    }
    if (mode == MODE_FIELD)
    {
        CtrFieldMenu_GetView(&s->fm);
        CtrFieldList_GetView(&s->fl);
    }
    if (s->mode == MODE_NAMING)
    {
        /* The whole screen, no dock or header: nothing of the save is read
         * (the player's name is being made, before there is one). */
        s->screen = SCR_COUNT;
        s->chat = CtrChat_IsOpen();
        if (s->chat)
            CtrChat_GetView(&s->ch);
        else
            CtrNaming_GetView(&s->nm);
        return;
    }
    s->gender = gSaveBlock2Ptr->playerGender ? FEMALE : MALE;
    s->inBattle = gMain.inBattle;
    s->enabled = EnabledScreens();
    s->screen = sScreen;
    /* The hidden menus take over the view they belong to. */
    if (mode == MODE_PARTY_MENU)
    {
        s->screen = SCR_POKEMON;
        /* The party menu from the field or a battle is in the window; from a
         * contest or a facility it has the whole screen (CtrCentredParty). */
        s->bagView = CtrParty_InWindow() ? BAG_VIEW_SECTION : BAG_VIEW_WHOLE;
        if (s->bagView == BAG_VIEW_WHOLE)
            s->screen = SCR_COUNT;
    }
    else if (mode == MODE_BAG_MENU)
    {
        s->screen = SCR_BAG;
        /* The bag from the field or a battle is in the window; from anything
         * else it has the whole screen. */
        s->bagView = CtrBag_InWindow() ? BAG_VIEW_SECTION : BAG_VIEW_WHOLE;
        if (s->bagView == BAG_VIEW_WHOLE)
            s->screen = SCR_COUNT;
    }
    else if (mode == MODE_POKENAV)
        s->screen = SCR_POKENAV;
    else if (mode == MODE_CARD)
        s->screen = SCR_CARD;
    else if (mode == MODE_POKEDEX)
        s->screen = SCR_POKEDEX;
    else if (mode == MODE_SUMMARY)
        s->screen = SCR_POKEMON;
    else if (mode == MODE_STORAGE)
        s->screen = SCR_COUNT;   /* the boxes have the whole screen */
    if (s->screen < SCR_COUNT && !((s->enabled >> s->screen) & 1))
        s->screen = SCR_MAP;
    /* In a battle, its party menu, summary and bag in their section, nothing else
     * of the bottom screen's to go to: the dock with only their own button. */
    s->dock = mode < MODE_BATTLE_INFO && s->screen < SCR_COUNT
           && (!s->inBattle || mode == MODE_SUMMARY
               || ((mode == MODE_PARTY_MENU || mode == MODE_BAG_MENU) && s->bagView == BAG_VIEW_SECTION));
    /* The Fly map: only MAP, which cancels it, as in a battle's menus. */
    s->mapFly = sMapFly && mode == MODE_FIELD && s->screen == SCR_MAP && s->dock;
    s->dockLocked = s->dock && (s->inBattle || s->mapFly || BagReturnsToMenu(mode) || ChoosesForScript(mode));
    StringCopy(s->name, gSaveBlock2Ptr->playerName);
    /* The header's Y and RUN, and the focus on the dock or the header; in a
     * battle neither is for now. */
    s->registered = s->dockLocked ? ITEM_NONE : gSaveBlock1Ptr->registeredItem;
    s->run = s->dockLocked ? 0 : (CtrSettings_RunAlways() ? 1 : 0) | (FlagGet(FLAG_SYS_B_DASH) ? 2 : 0);
    s->focus = sFocus;
    s->optFocus = FOCUS_NONE;

    if (s->dock)
    {
        /* The header: the area's name and the map extras' toggle with it,
         * the Repel, the clock. */
        s->mapsec = PlayerRegionPosition(&s->cursorX, &s->cursorY);
        MapFlyGlide(s);
        if (s->screen == SCR_MAP)
        {
            s->pickMapsec = sPickMapsec;
            s->pickX = sPickX;
            s->pickY = sPickY;
            s->mapZoom = sMapZoom;
            if (sMapZoom)
            {
                s->mapViewX = sMapViewX;
                s->mapViewY = sMapViewY;
            }
            if (sPickMapsec != MAPSEC_NONE)
            {
                s->pickType = CtrRegionMap_MapSecType(sPickMapsec);
                s->pickPos = sPickPos;
                if (sMapZoom)
                    s->landmarks = MapLandmarks(sPickMapsec, s->pickType, sPickPos);
            }
            if (s->mapFly)
            {
                for (int i = 0; i <= MAPSEC_EVER_GRANDE_CITY - MAPSEC_LITTLEROOT_TOWN; ++i)
                    if (FlagGet(FLAG_VISITED_LITTLEROOT_TOWN + i))
                        s->flyVisited |= 1 << i;
                s->flyFrontier = FlagGet(FLAG_LANDMARK_BATTLE_FRONTIER) != 0;
                s->flyCanFly = s->pickType == MAPSECTYPE_CITY_CANFLY || s->pickType == MAPSECTYPE_BATTLE_FRONTIER;
            }
            else
            {
                SnapshotMapExtras(s);
                SnapshotMapFly(s);
            }
        }
        else
            s->cursorX = s->cursorY = 0;
        /* Read only: the step counter is the game's (UpdateRepelCounter). */
        if (CtrExtras_Get("repel_steps") == 1 && !s->inBattle && !s->mapFly)
        {
            s->repelShown = TRUE;
            s->repelSteps = VarGet(VAR_REPEL_STEP_COUNT);
        }
        SnapshotClock(s);
    }

    SnapshotNative(s);
    switch (s->mode)
    {
    case MODE_FIELD:
        switch (s->screen)
        {
        case SCR_CARD:
            /* The pass drawn here is the new menus'; the classic ones open
             * the game's. */
            if (sCardView != CARD_VIEW_CARD && !sClassicMenus)
                SnapshotPass(s);
            else
            {
                SnapshotCard(s);
                if (sCardBack)
                    SnapshotCardBack(s);
            }
            break;
        case SCR_SAVE:
            SnapshotSave(s);
            s->saveStep = sSaveStep;
            s->canSave = FieldIdle();
            CopyText(s->text, sizeof(s->text), sSaveMessage);
            break;
        case SCR_OPTION:
            ReadOptionValues(s->options);
            s->optPage = sOptPage;
            s->optSub = sOptSub;
            SnapshotOptionRows(s);
            if (sInside == INSIDE_OPTIONS)
                s->optFocus = sOptFocus;
            break;
        case SCR_NUZLOCKE:
            SnapshotNuzlocke(s);
            break;
        }
        break;
    case MODE_BATTLE_ACTION:
    {
        u8 b = sAsked.battler;
        SnapshotBattle(s);
        SnapshotPartyBalls(s);
        s->battler = b;
        s->cursor = sQuickBallFocus ? 4 : gActionSelectionCursor[b];
        if (s->safari)
            s->safariBalls = gNumSafariBalls;
        else
            s->quickBall = CtrBattle_QuickBallItem();
        if (s->quickBall != ITEM_NONE)
            s->quickBallCount = CountTotalItemQuantityInBag(s->quickBall);
        break;
    }
    case MODE_BATTLE_MOVE:
    {
        u8 b = sAsked.battler;
        SnapshotBattle(s);
        s->battler = b;
        s->cursor = sMoveCancel ? MAX_MON_MOVES : gMoveSelectionCursor[b];
        memcpy(&s->moves4, &gBattleBufferA[b][4], sizeof(s->moves4));
        break;
    }
    case MODE_BATTLE_TARGET:
    {
        u8 b = sAsked.battler;
        SnapshotBattle(s);
        s->battler = b;
        s->cursor = TargetPlate(gMultiUsePlayerCursor);
        SnapshotTargets(s, b);
        break;
    }
    case MODE_BATTLE_INFO:
        SnapshotBattle(s);
        break;
    }
}

/* The picture the summary shows: an egg's, an UNOWN's letter. */
static u16 SummaryPicSpecies(const struct CtrSummaryView *v)
{
    if (v->isEgg)
        return SPECIES_EGG;
    if (v->species == SPECIES_UNOWN)
    {
        u16 letter = GetUnownLetterByPersonality(v->pid);

        return letter ? SPECIES_UNOWN_B + letter - 1 : SPECIES_UNOWN;
    }
    return v->species;
}

static u16 FacPicSpecies(u16 species, u32 personality);

/* Loads one icon or picture the snapshot needs; TRUE if it did. */
static bool8 Prefetch(const ViewState *s)
{
    sIconBudget = TRUE;
    for (int i = 0; i < PARTY_SIZE && sIconBudget; ++i)
        if (s->party[i].species)
            MonIcon(s->party[i].iconSpecies, s->party[i].deoxys);
    for (int i = 0; i < MAX_BATTLERS_COUNT && sIconBudget && s->mode == MODE_BATTLE_TARGET; ++i)
        if (s->targets[i].species)
            MonIcon(s->targets[i].iconSpecies, s->targets[i].deoxys);
    /* The party menu's icons move: their second frames too. */
    for (int i = 0; i < PARTY_SIZE && sIconBudget && s->native == NATIVE_PARTY; ++i)
        if (s->party[i].species)
            MonIconFrame(s->party[i].iconSpecies, s->party[i].deoxys, 1);
    for (int i = 0; i < s->encCount && sIconBudget; ++i)
        if (s->enc[i].seen)
            MonIcon(GetIconSpecies(s->enc[i].species, 0), FALSE);
    if (sIconBudget && s->registered != ITEM_NONE && s->mode < MODE_BATTLE_INFO)
        ItemIcon(s->registered);
    if (sIconBudget && s->mode == MODE_BATTLE_ACTION && s->quickBall != ITEM_NONE)
        ItemIcon(s->quickBall);
    if (sIconBudget && s->mode == MODE_BATTLE_ACTION && s->safari)
        ItemIcon(ITEM_SAFARI_BALL);
    if (s->native == NATIVE_SUMMARY && !s->sv.loading)
    {
        if (sIconBudget)
            FrontPic(SummaryPicSpecies(&s->sv), s->sv.shiny);
        if (sIconBudget && s->sv.ball)
            ItemIcon(s->sv.ball);
        if (sIconBudget && s->sv.item)
            ItemIcon(s->sv.item);
    }
    /* The Pokémon being named, whole; the player's own picture is loaded already. */
    if (s->mode == MODE_NAMING && s->nm.species
        && (s->nm.use == NAMING_SCREEN_CAUGHT_MON || s->nm.use == NAMING_SCREEN_NICKNAME) && sIconBudget)
        FrontPic(s->nm.species, FALSE);
    /* The contest's Pokemon and the Rental Pokemon, loaded one a frame. */
    if (s->mode == MODE_PAD && s->co.state && s->co.species)
        MonIcon(GetIconSpecies(s->co.species, s->co.personality), FALSE);
    if (s->mode == MODE_PAD && s->fac.screen)
        for (int i = 0; i < s->fac.count && i < CTR_FAC_MONS && sIconBudget; ++i)
            if (s->fac.species[i])
                MonIcon(GetIconSpecies(s->fac.species[i], s->fac.personality[i]), FALSE);
    if (s->mode == MODE_PAD && s->fac.screen && s->fac.count && s->fac.focus < s->fac.count && sIconBudget)
        FrontPic(FacPicSpecies(s->fac.species[s->fac.focus], s->fac.personality[s->fac.focus]), s->fac.shiny);
    if (s->native == NATIVE_DEX)
    {
        if (sIconBudget && s->dx.species)
            FrontPic(s->dx.species, FALSE);
        for (int i = 0; i < CTR_DEX_ROWS && sIconBudget; ++i)
            if (s->dx.rows[i].species)
                MonIcon(GetIconSpecies(s->dx.rows[i].species, 0), FALSE);
    }
    if (s->native == NATIVE_BAG)
    {
        if (sIconBudget)
            BagPic(s->bv.female, s->bv.bagFrame);
        if (sIconBudget)
            ItemIcon(s->bv.item != ITEM_NONE ? s->bv.item : ITEM_LIST_END);
    }
    if (sIconBudget)
    {
        sIconBudget = FALSE;
        return FALSE;
    }
    return TRUE;
}
