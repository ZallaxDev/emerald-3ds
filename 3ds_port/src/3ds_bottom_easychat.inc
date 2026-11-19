/* Part of 3ds_bottom_ui.c: Easy Chat (included after 3ds_bottom_contest.inc). */

/*
 * The phrase on top as a grid of its words; under it, by what the game is
 * asking: its title and a hint; the groups (all of them at once) or the
 * alphabet; or the words of the group opened, twelve at a time (the page is
 * the view's own, CtrEasyChat_Page); or, for the Quiz Lady's question, its
 * title and the two buttons the game reads, A and B. Every button is a tap the
 * game takes as the keys it reads (CtrEasyChat_Act): the cursor on the top
 * screen moves as it would for the D-pad. A prompt's YES / NO are the game's
 * own menu, which the bottom screen draws in place of this.
 *
 * The hit ids stay apart from each other: slots 0-8, groups 0x10-0x25, the
 * side buttons 0x38-0x3D, words 0x50-0x5B, the footer 0x60-0x63, letters
 * 0x70-0x8F (row * 8 + column). At most 31 are in the table at once.
 */
enum
{
    EC_HIT_SLOT = 0x00,      /* + word of the phrase */
    EC_HIT_GROUP = 0x10,     /* + group, in the game's order */
    EC_HIT_MODE = 0x38,
    EC_HIT_DELETE,
    EC_HIT_PAGE_UP,
    EC_HIT_PAGE_DOWN,
    EC_HIT_BACK,
    EC_HIT_MESSAGE,
    EC_HIT_WORD = HIT_ROW,   /* + the word shown */
    EC_HIT_FOOTER = HIT_ACTION, /* + footer button */
    EC_HIT_LETTER = HIT_MOVE,   /* + row * 8 + column */
};

#define EC_PHRASE_Y 16
#define EC_FOOT_Y 202
#define EC_FOOT_H 20
#define EC_SHOWN CTR_EC_PAGE
#define EC_WORD_COLS 3

static int EcPhraseHeight(const struct CtrEasyChatView *v)
{
    int rows = v->rows ? v->rows : 1;

    return 10 + rows * 14 < 34 ? 34 : 10 + rows * 14;
}

/* The tokens of one row of the alphabet (A B C... others), as the game prints them. */
static int EcLetterRow(int row, u8 tokens[8][12])
{
    static const u8 *const rows[4] = {gText_EasyChatKeyboard_ABCDEFothers, gText_EasyChatKeyboard_GHIJKL,
                                      gText_EasyChatKeyboard_MNOPQRS, gText_EasyChatKeyboard_TUVWXYZ};
    const u8 *p = rows[row & 3];
    int count = 0, len = 0;

    while (*p != EOS)
    {
        if (*p == EXT_CTRL_CODE_BEGIN)
        {
            if (len && count < 8)
            {
                tokens[count][len] = EOS;
                ++count;
            }
            len = 0;
            if (p[1] == EOS)
                break;
            p += 1 + GetExtCtrlCodeLength(p[1]);
            continue;
        }
        if (*p == CHAR_SPACE)
        {
            ++p;
            continue;
        }
        if (len < 11 && count < 8)
            tokens[count][len++] = *p;
        ++p;
    }
    if (len && count < 8)
    {
        tokens[count][len] = EOS;
        ++count;
    }
    return count;
}

static u8 EcState(const ViewState *s, u8 hit, bool8 focused)
{
    return s->pressed == hit ? PLATE_STATE_PRESSED : focused ? PLATE_STATE_FOCUS : PLATE_STATE_NORMAL;
}

/* A button of the plates' kind with a centred text. */
static void EcButton(const ViewState *s, int x, int y, int w, int h, u8 hit, Rgb colour, const u8 *text, bool8 focused)
{
    int oy = Plate(x, y, w, h, colour, EcState(s, hit, focused));

    TextIn(text, x, oy, w, h, &sSmall, 'c', colour.r == sCreamPlate.r && colour.g == sCreamPlate.g ? STYLE_INK : STYLE_WHITE, 2);
    AddHit(x, y, w, h, hit);
}

static void DrawEcPhrase(const ViewState *s, bool8 tappable, int ph)
{
    const struct CtrEasyChatView *v = &s->ec;
    int columns = v->columns ? v->columns : 1, rows = v->rows ? v->rows : 1;
    int cw = 288 / columns, ch = 14, oy = Plate(8, EC_PHRASE_Y, 304, ph, sCreamPlate, PLATE_STATE_NORMAL);

    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
        {
            int i = r * columns + c, x = 16 + c * cw, y = oy + 5 + r * ch;
            bool8 here = v->mainRow == r && v->mainColumn == c;

            if (i >= v->maxWords)
                continue;
            if (here)
            {
                FillRect(x, y, cw - 2, ch - 1, PackRgb(255, 120, 40));
                FillRect(x + 2, y + 2, cw - 6, ch - 5, PackRgb(255, 233, 200));
            }
            if (v->phrase[i] == EC_EMPTY_WORD)
                TextIn(Ascii("--"), x, y, cw - 2, ch - 1, &sSmall, 'c', STYLE_GRAY, 1);
            else
                TextIn(v->phraseText[i], x, y, cw - 2, ch - 1, &sSmall, 'c', STYLE_INK, 1);
            if (tappable)
                AddHit(x, y, cw - 2, ch - 1, EC_HIT_SLOT + i);
        }
}

static void DrawEcFooter(const ViewState *s)
{
    const struct CtrEasyChatView *v = &s->ec;
    int n = v->footer, gap = 4, w = (304 - (n - 1) * gap) / n;
    Rgb colour[4] = {{232, 96, 72}, {252, 245, 234}, {88, 192, 128}, {88, 192, 128}};

    /* The labels are the game's own, whatever the language. */
    for (int i = 0; i < n; ++i)
    {
        if (v->footerText[i])
            EcButton(s, 8 + i * (w + gap), EC_FOOT_Y, w, EC_FOOT_H, EC_HIT_FOOTER + i, colour[i], v->footerText[i],
                     v->mainRow == v->rows && v->mainColumn == i);
    }
}

static void DrawEcGroups(const ViewState *s, int y0)
{
    const struct CtrEasyChatView *v = &s->ec;
    int avail = EC_FOOT_Y - 4 - y0, step = avail / 6 > 21 ? 21 : avail / 6, h = step - 3;

    for (int i = 0; i < v->numGroups && i < CTR_EC_GROUPS; ++i)
    {
        int x = 8 + (i & 3) * 77, y = y0 + (i >> 2) * step;
        bool8 here = v->keyColumn >= 0 && (v->keyRow + v->keyScroll) * 2 + v->keyColumn == i;

        EcButton(s, x, y, 73, h, EC_HIT_GROUP + i, sCreamPlate, v->group[i], here);
    }
}

static void DrawEcAlphabet(const ViewState *s, int y0)
{
    const struct CtrEasyChatView *v = &s->ec;
    int avail = EC_FOOT_Y - 4 - y0, step = avail / 4 > 30 ? 30 : avail / 4, h = step - 3;

    for (int r = 0; r < 4; ++r)
    {
        u8 tokens[8][12];
        int n = EcLetterRow(r, tokens);

        for (int c = 0; c < n && c < 7; ++c)
        {
            /* Seven columns of 40 pixels; the first row's last is "others", out to the plate's edge (312). */
            bool8 others = c == 6 && r == 0;
            int x = 8 + c * 43, w = others ? 312 - x : 40;

            EcButton(s, x, y0 + r * step, w, h, EC_HIT_LETTER + r * 8 + c, sCreamPlate, tokens[c],
                     v->keyColumn == c && v->keyRow == r);
        }
    }
}

static void DrawEcWords(const ViewState *s, int y0)
{
    const struct CtrEasyChatView *v = &s->ec;
    int cursor = v->wordRow * 2 + v->wordColumn, oy;
    u8 text[CTR_EC_TEXT];
    int avail = EC_FOOT_Y - 4 - (y0 + 20), step = avail / 4 > 24 ? 24 : avail / 4;

    oy = Plate(8, y0, 172, 16, sEmerald, PLATE_STATE_NORMAL);
    if (v->groupOpen)
        TextIn(v->groupOpen, 8, oy, 172, 16, &sSmall, 'c', STYLE_WHITE, 3);
    EcButton(s, 184, y0, 60, 16, EC_HIT_PAGE_UP, sCreamPlate, Ascii("^"), FALSE);
    EcButton(s, 248, y0, 64, 16, EC_HIT_PAGE_DOWN, sCreamPlate, Ascii("v"), FALSE);
    for (int i = 0; i < EC_SHOWN; ++i)
    {
        int index = v->first + i, x = 8 + (i % EC_WORD_COLS) * 102, y = y0 + 20 + (i / EC_WORD_COLS) * step;

        if (index >= v->numWords)
            break;
        CtrEasyChat_WordText(index, text);
        EcButton(s, x, y, 98, step - 3, EC_HIT_WORD + i, sCreamPlate, text, index == cursor);
    }
}

static void DrawEasyChat(const ViewState *s)
{
    const struct CtrEasyChatView *v = &s->ec;
    int ph = EcPhraseHeight(v), y0 = EC_PHRASE_Y + ph + 6;
    Rgb red = {232, 96, 72}, green = {88, 192, 128}, cream = {252, 245, 234};

    NameBackdrop();
    DrawEcPhrase(s, v->state == CTR_EC_MAIN, ph);
    if (v->state == CTR_EC_QUIZ)
    {
        int oy = Plate(8, y0, 304, 26, sEmerald, PLATE_STATE_NORMAL);

        if (v->title)
            TextIn(v->title, 8, oy, 304, 18, &sNormal, 'c', STYLE_WHITE, 6);
        EcButton(s, 8, EC_FOOT_Y, 146, EC_FOOT_H, EC_HIT_BACK, cream, gText_Cancel5, FALSE);
        EcButton(s, 158, EC_FOOT_Y, 154, EC_FOOT_H, EC_HIT_MESSAGE, green, gText_Answer, FALSE);
    }
    else if (v->state == CTR_EC_MAIN)
    {
        int oy = Plate(8, y0, 304, 40, sEmerald, PLATE_STATE_NORMAL);

        if (v->title)
            TextIn(v->title, 8, oy + 2, 304, 18, &sNormal, 'c', STYLE_WHITE, 6);
        TextIn(Ascii(CTR_TEXT("Touch a word to change it", "Toca una palabra para cambiarla", "Touchez un mot pour le changer",
                              "Tocca una parola per cambiarla", "Toque numa palavra para mudá-la",
                              "Wort antippen zum Ändern", "ことばをタッチ")),
               8, oy + 20, 304, 14, &sSmall, 'c', STYLE_SOFT, 6);
        DrawEcFooter(s);
    }
    else if (v->state == CTR_EC_KEYBOARD)
    {
        if (v->alphabet)
            DrawEcAlphabet(s, y0);
        else
            DrawEcGroups(s, y0);
        EcButton(s, 8, EC_FOOT_Y, 86, EC_FOOT_H, EC_HIT_DELETE, red,
                 Ascii(CTR_TEXT("DELETE", "BORRAR", "EFFACER", "CANCELLA", "APAGAR", "LÖSCHEN", "けす")), v->keyColumn == -1 && v->keyRow == 1);
        EcButton(s, 98, EC_FOOT_Y, 86, EC_FOOT_H, EC_HIT_MODE, cream,
                 v->alphabet ? Ascii(CTR_TEXT("GROUPS", "GRUPOS", "GROUPES", "GRUPPI", "GRUPOS", "GRUPPEN", "グループ"))
                             : Ascii("A-Z"), v->keyColumn == -1 && v->keyRow == 0);
        EcButton(s, 188, EC_FOOT_Y, 124, EC_FOOT_H, EC_HIT_BACK, green, gText_Cancel2, v->keyColumn == -1 && v->keyRow == 2);
    }
    else if (v->state == CTR_EC_WORDS_PAGE)
    {
        DrawEcWords(s, y0);
        EcButton(s, 8, EC_FOOT_Y, 304, EC_FOOT_H, EC_HIT_BACK, cream,
                 Ascii(CTR_TEXT("BACK", "ATRÁS", "RETOUR", "INDIETRO", "VOLTAR", "ZURÜCK", "もどる")), FALSE);
    }
    else if (v->state == CTR_EC_MESSAGE)
    {
        EcButton(s, 8, EC_FOOT_Y, 304, EC_FOOT_H, EC_HIT_MESSAGE, green,
                 Ascii(CTR_TEXT("OK", "ACEPTAR", "OK", "OK", "OK", "OK", "OK")), FALSE);
    }
}

static void EasyChatTap(u8 id)
{
    const struct CtrEasyChatView *v = &sShown.ec;

    if (id < EC_HIT_SLOT + CTR_EC_WORDS)
        CtrEasyChat_Act(CTR_EC_ACT_SLOT, id - EC_HIT_SLOT);
    else if (id >= EC_HIT_GROUP && id < EC_HIT_GROUP + CTR_EC_GROUPS)
        CtrEasyChat_Act(CTR_EC_ACT_GROUP, id - EC_HIT_GROUP);
    else if (id >= EC_HIT_LETTER && id < EC_HIT_LETTER + 32)
        CtrEasyChat_Act(CTR_EC_ACT_LETTER, id - EC_HIT_LETTER);
    else if (id >= EC_HIT_WORD && id < EC_HIT_WORD + EC_SHOWN)
        CtrEasyChat_Act(CTR_EC_ACT_WORD, v->first + (id - EC_HIT_WORD));
    else if (id >= EC_HIT_FOOTER && id < EC_HIT_FOOTER + 4)
        CtrEasyChat_Act(CTR_EC_ACT_FOOTER, id - EC_HIT_FOOTER);
    else if (id == EC_HIT_MODE)
        CtrEasyChat_Act(CTR_EC_ACT_KB_MODE, 0);
    else if (id == EC_HIT_DELETE)
        CtrEasyChat_Act(CTR_EC_ACT_KB_DELETE, 0);
    else if (id == EC_HIT_BACK)
        CtrEasyChat_Act(CTR_EC_ACT_BACK, v->state);
    else if (id == EC_HIT_MESSAGE)
        CtrEasyChat_Act(CTR_EC_ACT_MESSAGE, v->state);
    else if (id == EC_HIT_PAGE_UP)
        CtrEasyChat_Page(-1);
    else if (id == EC_HIT_PAGE_DOWN)
        CtrEasyChat_Page(1);
}
