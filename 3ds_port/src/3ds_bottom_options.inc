/*
 * OPTIONS (included by 3ds_bottom_ui.c, after 3ds_bottom_field.inc): the
 * game's options, the port's and the extras (3ds_extras.h) sorted into
 * categories (the tabs on top) and groups (the tabs under them), six rows a
 * group on a cream panel. A row is a toggle (OFF | ON, the live half an
 * emerald plate), a value between arrows, an action on a slate plate, or a
 * line of information.
 *
 * Rows name the extra they show by its settings key: one not in this build
 * (a feature compiled out) is left out, and a group or a category left with
 * nothing is too. The extras' own screens (GIVE ITEMS, the randomizer, the
 * Nuzlocke tracker's) are groups here, or the NUZLOCKE section; their
 * open/back cells are not shown.
 */

enum { ROW_GAME, ROW_EXTRA, ROW_INFO };
enum { KIND_TOGGLE, KIND_VALUE, KIND_ACTION, KIND_INFO };

typedef struct
{
    u8 type;            /* ROW_* */
    u8 game;            /* ROW_GAME: OPT_* */
    const char *key;    /* ROW_EXTRA: the extra's settings key */
    const char *label;  /* ROW_GAME, ROW_INFO: its label; an extra has its own name */
} OptRowDef;

typedef struct
{
    const char *name;
    const OptRowDef *rows;
    u8 count;
} OptGroupDef;

typedef struct
{
    const char *name;
    const OptGroupDef *groups;
    u8 count;
} OptCatDef;

#define GAME_ROW(opt, label) {ROW_GAME, opt, NULL, label}
#define EXTRA_ROW(key) {ROW_EXTRA, 0, key, NULL}
#define GROUP(name, rows) {name, rows, ARRAY_COUNT(rows)}

static const OptRowDef sRowsUi[] = {
    GAME_ROW(OPT_TEXT_SPEED, CTR_TEXT("TEXT SPEED", "VELOC. TEXTO", "VIT. TEXTE", "VEL. TESTO", "TEXTTEMPO", "VELOC. TEXTO")),
    GAME_ROW(OPT_FRAME, CTR_TEXT("FRAME", "MARCO", "CADRE", "CORNICE", "RAHMEN", "MOLDURA")),
    GAME_ROW(OPT_BUTTON_MODE, CTR_TEXT("BUTTON MODE", "MODO BOTONES", "MODE TOUCHES", "MODO TASTI", "TASTENMODUS", "MODO BOTÕES")),
    GAME_ROW(OPT_SOUND, CTR_TEXT("SOUND", "SONIDO", "SON", "AUDIO", "TON", "SOM")),
    EXTRA_ROW("classic_menus"),
};
static const OptRowDef sRowsSystem[] = {
    EXTRA_ROW("speed"),
    EXTRA_ROW("audio_clock"),
    GAME_ROW(OPT_FPS, CTR_TEXT("SHOW FPS", "MOSTRAR FPS", "AFFICH. FPS", "MOSTRA FPS", "FPS-ANZEIGE", "MOSTRAR FPS")),
};
static const OptRowDef sRows3d[] = {
    GAME_ROW(OPT_VOXEL, "VOXEL 3D"),
    EXTRA_ROW("voxel_stereo"),
    EXTRA_ROW("voxel_depth"),
    GAME_ROW(OPT_VOXEL_PITCH, CTR_TEXT("3D ANGLE", "ÁNGULO 3D", "ANGLE 3D", "ANGOLO 3D", "3D-WINKEL", "ÂNGULO 3D")),
    GAME_ROW(OPT_VOXEL_ZOOM, CTR_TEXT("3D ZOOM", "ZOOM 3D", "ZOOM 3D", "ZOOM 3D", "3D-ZOOM", "ZOOM 3D")),
    GAME_ROW(OPT_VOXEL_BLUR, CTR_TEXT("3D BLUR", "DESENFOQUE 3D", "FLOU 3D", "SFOC. 3D", "3D-UNSCHÄRFE", "DESFOQUE 3D")),
};
static const OptRowDef sRowsField[] = {
    EXTRA_ROW("visible_wild"),
    EXTRA_ROW("follower"),
    EXTRA_ROW("run_indoors"),
    EXTRA_ROW("field_hms"),
};
static const OptRowDef sRowsHelpers[] = {
    EXTRA_ROW("map_encounters"),
    EXTRA_ROW("repel_steps"),
    EXTRA_ROW("repel_prompt"),
};
static const OptRowDef sRowsClock[] = {
    EXTRA_ROW("day_night"),
    EXTRA_ROW("clock_speed"),
    EXTRA_ROW("show_clock"),
    EXTRA_ROW("timed_encounters"),
    EXTRA_ROW("clock_notice"),
    EXTRA_ROW("clock_sync"),
};
static const OptRowDef sRowsScene[] = {
    GAME_ROW(OPT_BATTLE_SCENE, CTR_TEXT("BATTLE SCENE", "ANIMACIÓN", "ANIM. COMBAT", "ANIMAZIONE", "KAMPFANIM.", "ANIMAÇÃO")),
    GAME_ROW(OPT_BATTLE_STYLE, CTR_TEXT("BATTLE STYLE", "ESTILO", "STYLE", "STILE", "KAMPFSTIL", "ESTILO")),
    GAME_ROW(OPT_VOXEL_BATTLE, CTR_TEXT("3D BATTLE", "COMBATE 3D", "COMBAT 3D", "BATTAGLIA 3D", "3D-KAMPF", "BATALHA 3D")),
};
static const OptRowDef sRowsRules[] = {
    EXTRA_ROW("move_split"),
    EXTRA_ROW("ivs_evs"),
    EXTRA_ROW("infinite_tms"),
};
static const OptRowDef sRowsExp[] = {
    EXTRA_ROW("exp_catch"),
    EXTRA_ROW("exp_share"),
    EXTRA_ROW("trade_evo"),
};
static const OptRowDef sRowsNuzlocke[] = {
    EXTRA_ROW(CTR_NUZLOCKE_KEY),
    {ROW_INFO, 0, NULL, CTR_TEXT("MENU IN THE BAR", "MENÚ EN LA BARRA", "MENU : LA BARRE", "MENU NELLA BARRA", "MENÜ IN DER LEISTE", "MENU NA BARRA")},
};
static const OptRowDef sRowsRandom[] = {
    EXTRA_ROW("rand_wild"),
    EXTRA_ROW("rand_starters"),
    EXTRA_ROW("rand_trainers"),
    EXTRA_ROW("rand_seed"),
    EXTRA_ROW("rand_new_seed"),
};
static const OptRowDef sRowsCatch[] = {
    EXTRA_ROW("encounter_rate"),
    EXTRA_ROW("shiny_odds"),
    EXTRA_ROW("always_catch"),
};
static const OptRowDef sRowsPerks[] = {
    EXTRA_ROW("instant_victory"),
    EXTRA_ROW("fast_eggs"),
    EXTRA_ROW("infinite_money"),
};
static const OptRowDef sRowsDex[] = {
    EXTRA_ROW("dex_hoenn"),
    EXTRA_ROW("dex_national_on"),
    EXTRA_ROW("dex_national"),
};
static const OptRowDef sRowsItems[] = {
    EXTRA_ROW("give_pocket"),
    EXTRA_ROW("give_item"),
    EXTRA_ROW("give_count"),
    EXTRA_ROW("give_now"),
};

static const OptGroupDef sGroupsGeneral[] = {
    GROUP(CTR_TEXT("INTERFACE", "INTERFAZ", "INTERFACE", "INTERFACCIA", "ANZEIGE", "INTERFACE"), sRowsUi),
    GROUP(CTR_TEXT("SYSTEM", "SISTEMA", "SYSTÈME", "SISTEMA", "SYSTEM", "SISTEMA"), sRowsSystem),
};
static const OptGroupDef sGroupsWorld[] = {
    GROUP("3D", sRows3d),
    GROUP(CTR_TEXT("FIELD", "CAMPO", "TERRAIN", "CAMPO", "FELD", "CAMPO"), sRowsField),
    GROUP(CTR_TEXT("HELPERS", "AYUDAS", "AIDES", "AIUTI", "HILFEN", "AJUDAS"), sRowsHelpers),
    GROUP(CTR_TEXT("CLOCK", "RELOJ", "HORLOGE", "OROLOGIO", "UHR", "RELÓGIO"), sRowsClock),
};
static const OptGroupDef sGroupsBattle[] = {
    GROUP(CTR_TEXT("SCENE", "ESCENA", "SCÈNE", "SCENA", "ANIM.", "CENA"), sRowsScene),
    GROUP(CTR_TEXT("RULES", "REGLAS", "RÈGLES", "REGOLE", "REGELN", "REGRAS"), sRowsRules),
    GROUP(CTR_TEXT("EXP", "EXP", "EXP.", "ESP.", "EP", "EXP"), sRowsExp),
};
static const OptGroupDef sGroupsModes[] = {
    GROUP("NUZLOCKE", sRowsNuzlocke),
    GROUP(CTR_TEXT("RANDOMIZER", "ALEATORIO", "ALÉATOIRE", "CASUALE", "RANDOMIZER", "ALEATÓRIO"), sRowsRandom),
};
static const OptGroupDef sGroupsCheats[] = {
    GROUP(CTR_TEXT("CATCHING", "CAPTURA", "CAPTURE", "CATTURA", "FANGEN", "CAPTURA"), sRowsCatch),
    GROUP(CTR_TEXT("PERKS", "VENTAJAS", "BONUS", "VANTAGGI", "VORTEILE", "VANTAGENS"), sRowsPerks),
    GROUP("POKéDEX", sRowsDex),
    GROUP(CTR_TEXT("ITEMS", "OBJETOS", "OBJETS", "OGGETTI", "ITEMS", "ITENS"), sRowsItems),
};

enum { CAT_GENERAL, CAT_WORLD, CAT_BATTLE, CAT_MODES, CAT_CHEATS, CAT_COUNT };
static const OptCatDef sCats[CAT_COUNT] = {
    {CTR_TEXT("GENERAL", "GENERAL", "GÉNÉRAL", "GENERALE", "BASIS", "GERAL"), sGroupsGeneral, ARRAY_COUNT(sGroupsGeneral)},
    {CTR_TEXT("WORLD", "MUNDO", "MONDE", "MONDO", "WELT", "MUNDO"), sGroupsWorld, ARRAY_COUNT(sGroupsWorld)},
    {CTR_TEXT("BATTLE", "COMBATE", "COMBAT", "LOTTA", "KAMPF", "BATALHA"), sGroupsBattle, ARRAY_COUNT(sGroupsBattle)},
    {CTR_TEXT("MODES", "PARTIDA", "PARTIE", "PARTITA", "MODI", "PARTIDA"), sGroupsModes, ARRAY_COUNT(sGroupsModes)},
    {CTR_TEXT("CHEATS", "TRUCOS", "ASTUCES", "TRUCCHI", "CHEATS", "TRAPAÇAS"), sGroupsCheats, ARRAY_COUNT(sGroupsCheats)},
};

#define OPT_GROUPS_MAX 4
#define OPT_ROWS_MAX 6

/* A row as it is in this build: its definition and, for an extra, its line. */
typedef struct
{
    const OptRowDef *def;
    const CtrExtra *extra;
} OptRow;

static const CtrExtra *ExtraByKey(const char *key)
{
    for (unsigned i = 0; i < gCtrExtraCount; ++i)
        if (gCtrExtras[i].key && strcmp(gCtrExtras[i].key, key) == 0)
            return &gCtrExtras[i];
    return NULL;
}

/* Whether a row exists in this build. */
static bool8 RowExists(const OptRowDef *def)
{
    if (def->type == ROW_GAME)
        return OptionExists(def->game);
    if (def->type == ROW_EXTRA)
        return ExtraByKey(def->key) != NULL;
    return TRUE;
}

/* A group's rows in this build, up to OPT_ROWS_MAX. */
static int GroupRows(const OptGroupDef *g, OptRow *rows)
{
    int n = 0;

    for (int i = 0; i < g->count && n < OPT_ROWS_MAX; ++i)
        if (RowExists(&g->rows[i]))
        {
            rows[n].def = &g->rows[i];
            rows[n].extra = g->rows[i].type == ROW_EXTRA ? ExtraByKey(g->rows[i].key) : NULL;
            ++n;
        }
    return n;
}

static bool8 GroupUsed(const OptGroupDef *g)
{
    OptRow rows[OPT_ROWS_MAX];

    return GroupRows(g, rows) > 0;
}

static bool8 CatUsed(int cat)
{
    for (int i = 0; i < sCats[cat].count; ++i)
        if (GroupUsed(&sCats[cat].groups[i]))
            return TRUE;
    return FALSE;
}

/* The group on show, or the first one used. */
static const OptGroupDef *ShownGroup(int cat, int group)
{
    if (group < sCats[cat].count && GroupUsed(&sCats[cat].groups[group]))
        return &sCats[cat].groups[group];
    for (int i = 0; i < sCats[cat].count; ++i)
        if (GroupUsed(&sCats[cat].groups[i]))
            return &sCats[cat].groups[i];
    return &sCats[cat].groups[0];
}

static int ShownRows(OptRow *rows)
{
    return GroupRows(ShownGroup(sOptPage, sOptSub), rows);
}

static int RowKind(const OptRow *r)
{
    if (r->def->type == ROW_INFO)
        return KIND_INFO;
    if (r->def->type == ROW_GAME)
    {
        switch (r->def->game)
        {
        case OPT_BATTLE_SCENE:
        case OPT_FPS:
        case OPT_VOXEL:
        case OPT_VOXEL_BATTLE:
            return KIND_TOGGLE;
        }
        return KIND_VALUE;
    }
    if (r->extra->count == 0 && !r->extra->step)
        return KIND_ACTION;
    if (r->extra->count == 2 && r->extra->values == gCtrExtrasOffOn)
        return KIND_TOGGLE;
    return KIND_VALUE;
}

static bool8 RowLive(const OptRow *r, bool8 voxel)
{
    if (r->def->type == ROW_GAME)
        return OptionLive(r->def->game, voxel);
    if (r->def->type == ROW_EXTRA)
        return ExtraLive(r->extra, voxel);
    return FALSE;
}

/* A toggle's state: on or off. */
static bool8 RowOn(const OptRow *r, const u8 options[OPTION_ROWS])
{
    if (r->def->type == ROW_GAME)
        return r->def->game == OPT_BATTLE_SCENE ? !options[OPT_BATTLE_SCENE] : options[r->def->game] != 0;
    return CtrExtras_Value(r->extra) != 0;
}

/* What a row shows, for the redraw to notice a change. */
static u16 RowShownValue(const OptRow *r, const u8 options[OPTION_ROWS])
{
    if (r->def->type == ROW_GAME)
        return options[r->def->game];
    if (r->def->type == ROW_EXTRA)
        return ExtraShownValue(r->extra);
    return 0;
}

/* What the rows on show show, for the redraw to notice a change. */
static void SnapshotOptionRows(ViewState *s)
{
    OptRow rows[OPT_ROWS_MAX];
    int n = ShownRows(rows);

    for (int i = 0; i < n && i < OPT_ROWS_SHOWN; ++i)
        s->rows[i] = RowShownValue(&rows[i], s->options);
}

/* The game's option values, as their own strings. */
static const u8 *OptionValue(int row, u8 value)
{
    static u8 text[16];

    switch (row)
    {
    case OPT_TEXT_SPEED: return value == 0 ? gText_TextSpeedSlow : value == 1 ? gText_TextSpeedMid : gText_TextSpeedFast;
    case OPT_BATTLE_STYLE: return value ? gText_BattleStyleSet : gText_BattleStyleShift;
    case OPT_SOUND: return value ? gText_SoundStereo : gText_SoundMono;
    case OPT_BUTTON_MODE:
        return value == 0 ? gText_ButtonTypeNormal : value == 1 ? gText_ButtonTypeLR : gText_ButtonTypeLEqualsA;
    case OPT_VOXEL_PITCH: return Number(value, 2, STR_CONV_MODE_LEFT_ALIGN);
    case OPT_VOXEL_BLUR:
    case OPT_VOXEL_ZOOM:
        /* Percentages; the blur's strength is OFF at 0 (the zoom never is). */
        if (value == 0)
            return gText_BattleSceneOff;
        StringCopy(text, Number(value, 3, STR_CONV_MODE_LEFT_ALIGN));
        text[StringLength(text) + 1] = EOS;
        text[StringLength(text)] = CHAR_PERCENT;
        return text;
    case OPT_FRAME:
        StringCopy(text, gText_FrameType);
        StringAppend(text, Number(value + 1, 2, STR_CONV_MODE_LEFT_ALIGN));
        return text;
    }
    return Ascii("");
}

/* An action's short label on its plate. */
static const u8 *ActionLabel2(const OptRow *r)
{
    static const struct { const char *key, *label; } labels[] = {
        {"clock_sync", CTR_TEXT("SYNC", "SINCR.", "SYNCHRO", "SINCR.", "ABGL.", "SINCR.")},
        {"rand_new_seed", CTR_TEXT("NEW", "NUEVA", "NOUVELLE", "NUOVO", "NEU", "NOVA")},
        {"dex_hoenn", CTR_TEXT("SET", "ACTIVAR", "ACTIVER", "ATTIVA", "SETZEN", "ATIVAR")},
        {"dex_national_on", CTR_TEXT("SET", "ACTIVAR", "ACTIVER", "ATTIVA", "SETZEN", "ATIVAR")},
        {"dex_national", CTR_TEXT("SET", "ACTIVAR", "ACTIVER", "ATTIVA", "SETZEN", "ATIVAR")},
        {"give_now", CTR_TEXT("GIVE", "DAR", "DONNER", "DAI", "GEBEN", "DAR")},
    };

    if (r->extra->text)
        return r->extra->text();
    for (unsigned i = 0; i < ARRAY_COUNT(labels); ++i)
        if (strcmp(labels[i].key, r->extra->key) == 0)
            return Ascii(labels[i].label);
    return Ascii(r->extra->values ? r->extra->values[0] : "");
}

/* A value row's text. */
static const u8 *RowValueText(const OptRow *r, const u8 options[OPTION_ROWS])
{
    if (r->def->type == ROW_GAME)
        return OptionValue(r->def->game, options[r->def->game]);
    if (r->extra->text)
        return r->extra->text();
    if (r->extra->values && r->extra->count)
        return Ascii(r->extra->values[CtrExtras_Value(r->extra) % r->extra->count]);
    return Ascii("");
}

static const u8 *RowLabel(const OptRow *r)
{
    return Ascii(r->def->type == ROW_EXTRA ? r->extra->name : r->def->label);
}

/* ------------------------------------------------------------------------ */
/* Changing a row                                                           */
/* ------------------------------------------------------------------------ */

/* The game's options and the port's, a step on (or back). */
static void StepGameOption(int row, bool8 back)
{
    static const u8 counts[OPTION_ROWS] = {3, 2, 2, 2, 3, WINDOW_FRAMES_COUNT, 2, 2, 0, 0, 2, 2};
    u8 value, step = back ? counts[row] - 1 : 1;

    if (!OptionLive(row, CtrSettings_Voxel()))
        return;
    switch (row)
    {
    case OPT_FPS: CtrSettings_SetShowFps(!CtrSettings_ShowFps()); break;
    case OPT_VOXEL: CtrSettings_SetVoxel(!CtrSettings_Voxel()); break;
    case OPT_VOXEL_BATTLE: CtrSettings_SetVoxelBattle(!CtrSettings_VoxelBattle()); break;
    case OPT_VOXEL_PITCH: CtrSettings_StepVoxelPitch(back ? -1 : 1); break;
    case OPT_VOXEL_ZOOM: CtrSettings_StepVoxelZoom(back ? -1 : 1); break;
    case OPT_VOXEL_BLUR: CtrSettings_StepVoxelBlur(back ? -1 : 1); break;
    default:
        switch (row)
        {
        case OPT_TEXT_SPEED: value = gSaveBlock2Ptr->optionsTextSpeed; break;
        case OPT_BATTLE_SCENE: value = gSaveBlock2Ptr->optionsBattleSceneOff; break;
        case OPT_BATTLE_STYLE: value = gSaveBlock2Ptr->optionsBattleStyle; break;
        case OPT_SOUND: value = gSaveBlock2Ptr->optionsSound; break;
        case OPT_BUTTON_MODE: value = gSaveBlock2Ptr->optionsButtonMode; break;
        default: value = gSaveBlock2Ptr->optionsWindowFrameType; break;
        }
        value = (value + step) % counts[row];
        switch (row)
        {
        case OPT_TEXT_SPEED: gSaveBlock2Ptr->optionsTextSpeed = value; break;
        case OPT_BATTLE_SCENE: gSaveBlock2Ptr->optionsBattleSceneOff = value; break;
        case OPT_BATTLE_STYLE: gSaveBlock2Ptr->optionsBattleStyle = value; break;
        case OPT_SOUND: gSaveBlock2Ptr->optionsSound = value; SetPokemonCryStereo(value); break;
        case OPT_BUTTON_MODE: gSaveBlock2Ptr->optionsButtonMode = value; break;
        default: gSaveBlock2Ptr->optionsWindowFrameType = value; break;
        }
        break;
    }
    PlaySE(SE_SELECT);
}

/* A row stepped on (or back); an action run (`back` never runs one). */
static void StepRow(const OptRow *r, bool8 back)
{
    if (!RowLive(r, CtrSettings_Voxel()))
        return;
    if (r->def->type == ROW_GAME)
    {
        StepGameOption(r->def->game, back);
        return;
    }
    if (RowKind(r) == KIND_ACTION)
    {
        /* It plays its own sound (done, or not possible). */
        if (!back)
            CtrExtras_Step(r->extra, 1);
        return;
    }
    if (r->extra->step)
        r->extra->step(back ? -1 : 1);
    else
        CtrExtras_Step(r->extra, back ? -1 : 1);
    PlaySE(SE_SELECT);
}

/* A toggle set to on or off; nothing if it already is. */
static void SetRow(const OptRow *r, bool8 on)
{
    u8 options[OPTION_ROWS];

    ReadOptionValues(options);
    if (RowOn(r, options) != on)
        StepRow(r, FALSE);
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                  */
/* ------------------------------------------------------------------------ */

#define OPT_TAB_H 18
#define OPT_GROUP_Y (AREA_Y + 20)
#define OPT_GROUP_H 15
#define OPT_PANEL_Y (AREA_Y + 38)
#define OPT_PANEL_H (AREA_Y + AREA_H - OPT_PANEL_Y)
#define OPT_ROW_H ((OPT_PANEL_H - 4) / OPT_ROWS_MAX)
#define OPT_ROW_Y(i) (OPT_PANEL_Y + 2 + (i) * OPT_ROW_H)
#define OPT_RIGHT (AREA_X + AREA_W - 8)

/* Hits: a category tab, a group tab, a row's back and on halves, a toggle's
 * halves (OFF on the back one). */
enum { HIT_CAT = HIT_PAGE, HIT_GROUP = HIT_PAGE + 8 };

/* The 4x7 arrow of a value, pointing left (dir < 0) or right. */
static void DrawArrow(int x, int y, int dir, u16 colour)
{
    for (int i = 0; i < 4; ++i)
        FillRect(dir < 0 ? x + i : x + 3 - i, y + 3 - i, 1, 2 * i + 1, colour);
}

/* The width of the OFF | ON halves: the wider word and some room. */
static int ToggleHalf(void)
{
    int a = StrWidth(&sSmall, Ascii(gCtrExtrasOffOn[0])), b = StrWidth(&sSmall, Ascii(gCtrExtrasOffOn[1]));

    return (a > b ? a : b) + 12;
}

/* Where a row's control starts: its label has the room left of it. */
static int RowControlLeft(const OptRow *r, const u8 options[OPTION_ROWS])
{
    switch (RowKind(r))
    {
    case KIND_TOGGLE: return OPT_RIGHT - 2 * ToggleHalf() - 2;
    case KIND_VALUE: return OPT_RIGHT - (StrWidth(&sSmall, RowValueText(r, options)) + 22);
    case KIND_ACTION: return OPT_RIGHT - (StrWidth(&sSmall, ActionLabel2(r)) + 14);
    }
    return OPT_RIGHT;
}

/* Equal tabs, as long as every label fits; else each its label's width and
 * what is left shared out (the mockups' tabs4). */
static void TabWidths(const u8 *const *labels, int n, int w, int *widths)
{
    int eq = (w - 2 * (n - 1)) / n, need = 0, most = 0;

    for (int i = 0; i < n; ++i)
    {
        widths[i] = StrWidth(&sSmall, labels[i]) + 10;
        need += widths[i];
        if (widths[i] > most)
            most = widths[i];
    }
    for (int i = 0; i < n; ++i)
        widths[i] = most <= eq ? eq : widths[i] + (w - 2 * (n - 1) - need) / n;
}

static void DrawOptionTabs(const ViewState *s)
{
    u8 labels[CAT_COUNT][24];
    const u8 *names[CAT_COUNT];
    int cats[CAT_COUNT], n = 0, widths[CAT_COUNT], x = AREA_X;

    for (int c = 0; c < CAT_COUNT; ++c)
        if (CatUsed(c))
        {
            StringCopy(labels[n], Ascii(sCats[c].name));
            names[n] = labels[n];
            cats[n++] = c;
        }
    TabWidths(names, n, AREA_W, widths);
    for (int i = 0; i < n; x += widths[i] + 2, ++i)
    {
        bool8 live = s->optPage == cats[i], pressed = s->pressed == HIT_CAT + cats[i];
        int oy = PlateEx(x, AREA_Y, widths[i], OPT_TAB_H, live || pressed ? sEmerald : sLight,
                         pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL, 0, 30, 3, 0);

        TextIn(names[i], x, oy, widths[i], OPT_TAB_H, &sSmall, 'c', live || pressed ? STYLE_WHITE : STYLE_INK, 3);
        AddHit(x, AREA_Y, widths[i], OPT_TAB_H + 1, HIT_CAT + cats[i]);
    }
}

static void DrawGroupTabs(const ViewState *s)
{
    const OptCatDef *cat = &sCats[s->optPage];
    const OptGroupDef *shown = ShownGroup(s->optPage, s->optSub);
    int groups[OPT_GROUPS_MAX], n = 0, gw;

    for (int g = 0; g < cat->count; ++g)
        if (GroupUsed(&cat->groups[g]))
            groups[n++] = g;
    if (n == 0)
        return;
    gw = (AREA_W - 2 * (n - 1)) / n;
    for (int i = 0; i < n; ++i)
    {
        int gx = AREA_X + i * (gw + 2);
        bool8 live = &cat->groups[groups[i]] == shown, pressed = s->pressed == HIT_GROUP + groups[i];
        int oy = PlateEx(gx, OPT_GROUP_Y, gw, OPT_GROUP_H, live || pressed ? sWildOff : sGroupOff,
                         pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL, 0, 30, 3, 0);

        TextIn(Ascii(cat->groups[groups[i]].name), gx, oy, gw, OPT_GROUP_H, &sSmall, 'c',
               live || pressed ? STYLE_WHITE : STYLE_INK, 3);
        AddHit(gx, OPT_GROUP_Y, gw, OPT_GROUP_H + 2, HIT_GROUP + groups[i]);
    }
}

/* The keyboard focus on a row: an orange line round it. */
static void FocusBox(int x, int y, int w, int h)
{
    u16 c = PackRgb(sPlateRing.r, sPlateRing.g, sPlateRing.b);

    FillRect(x + 1, y, w - 2, 1, c);
    FillRect(x + 1, y + h - 1, w - 2, 1, c);
    FillRect(x, y + 1, 1, h - 2, c);
    FillRect(x + w - 1, y + 1, 1, h - 2, c);
}

static void DrawOptionRow(const ViewState *s, int i, const OptRow *r, const u8 options[OPTION_ROWS], bool8 voxel)
{
    int ry = OPT_ROW_Y(i), rh = OPT_ROW_H, left = RowControlLeft(r, options), kind = RowKind(r);
    bool8 live = RowLive(r, voxel);
    bool8 pressed = live && (s->pressed == HIT_OPTION + i || s->pressed == HIT_OPTION + HIT_OPTION_BACK + i);
    u16 green = sStyle[STYLE_GREEN].fg;

    if (i)
        FillRgb(AREA_X + 8, ry, AREA_W - 16, 1, sRule);
    if (pressed)
        FillRgb(AREA_X + 4, ry + 1, AREA_W - 8, rh - 1, sGroupOff);
    switch (kind)
    {
    case KIND_TOGGLE:
    {
        int hw = ToggleHalf();
        bool8 on = RowOn(r, options);

        for (int j = 0; j < 2; ++j)
        {
            int bx = left + j * (hw + 2);
            const u8 *word = Ascii(gCtrExtrasOffOn[j]);

            if ((j == 1) == on)
            {
                int oy = PlateEx(bx, ry + 2, hw, rh - 4, live ? sEmerald : sOffGrey, PLATE_STATE_NORMAL, 0, 30, 3, 0);

                StyledCentre(&sSmall, word, bx + hw / 2, TextY(oy, rh - 4, &sSmall), STYLE_WHITE);
            }
            else
                StyledCentre(&sSmall, word, bx + hw / 2, TextY(ry + 2, rh - 4, &sSmall), STYLE_GRAY);
        }
        if (live)
        {
            AddHit(AREA_X, ry, left - AREA_X, rh, HIT_OPTION + i);
            AddHit(left, ry, hw + 1, rh, HIT_OPTION + HIT_OPTION_BACK + i);
            AddHit(left + hw + 1, ry, AREA_X + AREA_W - left - hw - 1, rh, HIT_OPTION + i);
        }
        break;
    }
    case KIND_VALUE:
    {
        int wv = OPT_RIGHT - left;

        StyledCentre(&sSmall, RowValueText(r, options), left + wv / 2, TextY(ry, rh, &sSmall),
                     live ? STYLE_GREEN : STYLE_GRAY);
        DrawArrow(left + 1, ry + rh / 2 - 3, -1, live ? green : sStyle[STYLE_GRAY].fg);
        DrawArrow(left + wv - 5, ry + rh / 2 - 3, 1, live ? green : sStyle[STYLE_GRAY].fg);
        if (live)
        {
            AddHit(AREA_X, ry, left + wv / 2 - AREA_X, rh, HIT_OPTION + HIT_OPTION_BACK + i);
            AddHit(left + wv / 2, ry, AREA_X + AREA_W - left - wv / 2, rh, HIT_OPTION + i);
        }
        break;
    }
    case KIND_ACTION:
    {
        int wv = OPT_RIGHT - left;
        int oy = PlateEx(left, ry + 2, wv, rh - 4, !live ? sOffGrey : pressed ? sEmerald : sSlate,
                         pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL, 0, 30, 3, 0);

        StyledCentre(&sSmall, ActionLabel2(r), left + wv / 2, TextY(oy, rh - 4, &sSmall), STYLE_WHITE);
        if (live)
            AddHit(AREA_X, ry, AREA_W, rh, HIT_OPTION + i);
        break;
    }
    }
    TextIn(RowLabel(r), AREA_X + 2, ry, left - AREA_X - 4, rh, &sSmall, 'l',
           kind == KIND_INFO || !live ? STYLE_GRAY : STYLE_INK, 6);
    if (s->optFocus == i)
        FocusBox(AREA_X + 4, ry + 1, AREA_W - 8, rh - 1);
}

static void DrawOptions(const ViewState *s)
{
    OptRow rows[OPT_ROWS_MAX];
    u8 options[OPTION_ROWS];
    int n = ShownRows(rows);
    bool8 voxel = OPTION_SHOWN > OPT_VOXEL && s->options[OPT_VOXEL];

    memcpy(options, s->options, sizeof(options));
    DrawOptionTabs(s);
    DrawGroupTabs(s);
    Plate(AREA_X, OPT_PANEL_Y, AREA_W, OPT_PANEL_H, sCreamPlate, PLATE_STATE_NORMAL);
    for (int i = 0; i < n; ++i)
        DrawOptionRow(s, i, &rows[i], options, voxel);
}

/* The options' panel on screen, for a redraw of it alone. */
static void OptionsPanelRect(int *x0, int *y0, int *x1, int *y1)
{
    PlateBounds(AREA_X, OPT_PANEL_Y, AREA_W, OPT_PANEL_H, x0, y0, x1, y1);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

static void ShowOptionGroup(u8 cat, u8 group)
{
    if (cat >= CAT_COUNT || !CatUsed(cat))
        return;
    if (group >= sCats[cat].count || !GroupUsed(&sCats[cat].groups[group]))
        group = (u8)(ShownGroup(cat, 0) - sCats[cat].groups);
    if (cat == sOptPage && group == sOptSub)
        return;
    sOptPage = cat;
    sOptSub = group;
    sOptFocus = 0;
    PlaySE(SE_SELECT);
}

/* The extras' own screens (an action that opens one: 3ds_extras.h), on the
 * group that holds those rows now. */
void CtrExtras_ShowScreen(unsigned page, unsigned screen)
{
    if (page == CTR_EXTRAS_CHEATS && screen == 1)
        ShowOptionGroup(CAT_CHEATS, 3);
    else if (page == CTR_EXTRAS_CHEATS && screen == 2)
        ShowOptionGroup(CAT_MODES, 1);
    else if (page == CTR_EXTRAS_ENHANCEMENTS && screen == 2 && (EnabledScreens() >> SCR_NUZLOCKE & 1))
        sScreen = SCR_NUZLOCKE;
}

static void ActivateOption(u8 id)
{
    OptRow rows[OPT_ROWS_MAX];
    int n = ShownRows(rows);

    if (id >= HIT_CAT && id < HIT_CAT + CAT_COUNT)
    {
        ShowOptionGroup(id - HIT_CAT, 0);
        return;
    }
    if (id >= HIT_GROUP && id < HIT_GROUP + OPT_GROUPS_MAX)
    {
        ShowOptionGroup(sOptPage, id - HIT_GROUP);
        return;
    }
    if (id >= HIT_OPTION && id < HIT_OPTION + 2 * HIT_OPTION_BACK)
    {
        bool8 back = id >= HIT_OPTION + HIT_OPTION_BACK;
        int row = (id - HIT_OPTION) % HIT_OPTION_BACK;

        if (row >= n)
            return;
        if (RowKind(&rows[row]) == KIND_TOGGLE && back)
            SetRow(&rows[row], FALSE);
        else if (RowKind(&rows[row]) == KIND_TOGGLE && id != HIT_OPTION + row)
            SetRow(&rows[row], TRUE);
        else
            StepRow(&rows[row], back);
    }
}

/* The next live row up or down, or `from`. */
static u8 OptionStep(u8 from, int dir)
{
    OptRow rows[OPT_ROWS_MAX];
    int n = ShownRows(rows);
    bool8 voxel = CtrSettings_Voxel();

    for (int i = from + dir; i >= 0 && i < n; i += dir)
        if (RowLive(&rows[i], voxel))
            return i;
    return from;
}

/* The next category (L, R) with groups in this build. */
static void StepCategory(int dir)
{
    for (int c = sOptPage + dir; c >= 0 && c < CAT_COUNT; c += dir)
        if (CatUsed(c))
        {
            ShowOptionGroup(c, 0);
            return;
        }
}

/* The next group, on to the next category past the last. */
static void StepGroup(int dir)
{
    const OptCatDef *cat = &sCats[sOptPage];
    int shown = (int)(ShownGroup(sOptPage, sOptSub) - cat->groups);

    for (int g = shown + dir; g >= 0 && g < cat->count; g += dir)
        if (GroupUsed(&cat->groups[g]))
        {
            ShowOptionGroup(sOptPage, g);
            return;
        }
    StepCategory(dir);
}

static void MoveFocus(u8 *focus, u8 to);

static void OptionKeys(u16 down)
{
    OptRow rows[OPT_ROWS_MAX];
    int n = ShownRows(rows);

    if (down & B_BUTTON)
    {
        sInside = INSIDE_NONE;
        sFocus = SCR_OPTION;
        PlaySE(SE_SELECT);
        return;
    }
    if (down & (L_BUTTON | R_BUTTON))
    {
        StepGroup((down & R_BUTTON) ? 1 : -1);
        return;
    }
    if (sOptFocus >= n)
        sOptFocus = OptionStep(0xFF, 1) == 0xFF ? 0 : OptionStep(0xFF, 1);
    if (down & DPAD_UP)
        MoveFocus(&sOptFocus, OptionStep(sOptFocus, -1));
    else if (down & DPAD_DOWN)
        MoveFocus(&sOptFocus, OptionStep(sOptFocus, 1));
    else if (sOptFocus < n && (down & DPAD_LEFT))
    {
        if (RowKind(&rows[sOptFocus]) != KIND_ACTION)
            StepRow(&rows[sOptFocus], TRUE);
    }
    else if (sOptFocus < n && (down & DPAD_RIGHT))
    {
        /* RIGHT steps a value. An action (HOENN DEX FULL, GIVE...) has no
         * value to step and cannot be undone: A alone runs it, as a tap. */
        if (RowKind(&rows[sOptFocus]) != KIND_ACTION)
            StepRow(&rows[sOptFocus], FALSE);
    }
    else if (sOptFocus < n && (down & A_BUTTON))
        StepRow(&rows[sOptFocus], FALSE);
}
