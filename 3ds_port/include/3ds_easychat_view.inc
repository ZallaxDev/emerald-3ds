/*
 * Easy Chat's view for the 3DS bottom screen (3ds_game_views.h).
 * Included at the end of src/easy_chat.c (under PLATFORM_3DS): it reads that
 * file's own state. The screen stays on the top one; the bottom one offers
 * the phrase, the groups or alphabet and the words as buttons.
 *
 * A tap is never written into the game's state. It is stored (CtrEasyChat_Act)
 * and, each frame the game takes input, turned into the one key that brings
 * the cursor a step nearer - the D-pad's own movement, with its scrolling,
 * wrapping and animations - and into A when it is there. So the top screen
 * does exactly what it would for the keys. A tap is taken only in the state
 * its button was drawn for, dropped when the game leaves that state, when a
 * button of the console is pressed, when a session ends, or when it has had
 * its time. The drawing is 3ds_bottom_easychat.inc.
 */

#include "3ds_game_views.h"

#define CTR_EC_TTL 240       /* frames a tap may take to be carried out */
#define CTR_EC_TTL_PAGE 20   /* frames more for each page of four rows a far word is away */
#define CTR_EC_PAD_KEYS (A_BUTTON | B_BUTTON | START_BUTTON | SELECT_BUTTON | DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT)
/* The longest word in the data (all languages) is 13 characters and the end; this has room to spare. */
#define CTR_EC_SCRATCH 32

static u8 sCtrEcAct;
static u16 sCtrEcA, sCtrEcTtl, sCtrEcFirst;
static u32 sCtrEcSince;

static u8 CtrEc_State(void)
{
    if (sEasyChatScreen == NULL || gMain.callback2 != CB2_EasyChatScreen)
        return CTR_EC_NONE;
    switch (sEasyChatScreen->inputState)
    {
    case INPUTSTATE_PHRASE:
    case INPUTSTATE_MAIN_SCREEN_BUTTONS:
        return CTR_EC_MAIN;
    case INPUTSTATE_KEYBOARD:
        return CTR_EC_KEYBOARD;
    case INPUTSTATE_WORD_SELECT:
        return CTR_EC_WORDS_PAGE;
    case INPUTSTATE_WAIT_FOR_MSG:
        return CTR_EC_MESSAGE;
    case INPUTSTATE_QUIZ_QUESTION:
        return CTR_EC_QUIZ;
    }
    /*
     * The prompts (leave, delete all, confirm): their YES / NO are the game's
     * own menu, which the bottom screen shows in place of this view.
     */
    return CTR_EC_OTHER;
}

/* The screen is running: its callback, or one of the steps between the Quiz Lady's screens. */
bool8 CtrEasyChat_Active(void)
{
    MainCallback cb = gMain.callback2;
    u32 i;

    if (cb == CB2_EasyChatScreen || cb == CB2_QuizLadyQuestion)
        return TRUE;
    for (i = 0; i < ARRAY_COUNT(sQuizLadyEasyChatScreens); i++)
    {
        if (cb == sQuizLadyEasyChatScreens[i].callback)
            return TRUE;
    }
    return FALSE;
}

/* A session starts or ends: nothing asked for the last one is carried out in the next. */
static void CtrEc_Reset(void)
{
    sCtrEcAct = CTR_EC_ACT_NONE;
    sCtrEcFirst = 0;
}

/* A group's words are opened: its list starts at the first page. */
static void CtrEc_OpenWords(void)
{
    sCtrEcFirst = 0;
}

/* A word of the game's data, copied whole to a scratch and cut to what the view holds. */
static void CtrEc_CopyWord(u8 *dst, u16 word)
{
    u8 scratch[CTR_EC_SCRATCH];
    u8 i;

    scratch[0] = EOS;
    CopyEasyChatWord(scratch, word);
    scratch[CTR_EC_SCRATCH - 1] = EOS;
    for (i = 0; i < CTR_EC_TEXT - 1 && scratch[i] != EOS; i++)
        dst[i] = scratch[i];
    dst[i] = EOS;
}

bool8 CtrEasyChat_GetView(struct CtrEasyChatView *view)
{
    u8 i, j;
    int footer;

    memset(view, 0, sizeof(*view));
    view->state = CtrEc_State();
    if (view->state == CTR_EC_NONE)
        return FALSE;
    view->columns = sEasyChatScreen->numColumns;
    view->rows = sEasyChatScreen->numRows;
    view->maxWords = sEasyChatScreen->maxWords > CTR_EC_WORDS ? CTR_EC_WORDS : sEasyChatScreen->maxWords;
    view->footer = FooterHasFourOptions() ? 4 : 3;
    footer = GetFooterIndex();
    if (footer < NUM_FOOTER_TYPES)
    {
        for (j = 0; j < 4; j++)
            view->footerText[j] = sFooterTextOptions[footer][j];
    }
    view->mainColumn = sEasyChatScreen->mainCursorColumn;
    view->mainRow = sEasyChatScreen->mainCursorRow;
    view->alphabet = sEasyChatScreen->inAlphabetMode;
    view->keyColumn = sEasyChatScreen->keyboardColumn;
    view->keyRow = sEasyChatScreen->keyboardRow;
    view->keyScroll = sEasyChatScreen->keyboardScrollOffset;
    view->numGroups = GetNumUnlockedEasyChatGroups();
    if (sWordData != NULL)
        view->numWords = GetNumWordsInSelectedGroup();
    view->wordColumn = sEasyChatScreen->wordSelectColumn;
    view->wordRow = sEasyChatScreen->wordSelectRow + sEasyChatScreen->wordSelectScrollOffset;
    view->first = view->state == CTR_EC_WORDS_PAGE ? sCtrEcFirst : 0;
    view->title = sEasyChatScreen->titleText;
    for (i = 0; i < view->maxWords; i++)
    {
        view->phrase[i] = sEasyChatScreen->currentPhrase[i];
        if (view->phrase[i] != EC_EMPTY_WORD)
            CtrEc_CopyWord(view->phraseText[i], view->phrase[i]);
        else
            view->phraseText[i][0] = EOS;
    }
    for (i = 0; i < view->numGroups && i < CTR_EC_GROUPS; i++)
        view->group[i] = GetEasyChatWordGroupName(GetUnlockedEasyChatGroupId(i));
    if (view->state == CTR_EC_WORDS_PAGE && !view->alphabet)
        view->groupOpen = GetEasyChatWordGroupName(GetUnlockedEasyChatGroupId(GetSelectedGroupIndex()));
    return TRUE;
}

void CtrEasyChat_WordText(u16 index, u8 *dst)
{
    u16 word = sEasyChatScreen != NULL && sWordData != NULL ? GetWordFromSelectedGroup(index) : EC_EMPTY_WORD;

    dst[0] = EOS;
    if (word != EC_EMPTY_WORD)
        CtrEc_CopyWord(dst, word);
}

/* The page of words shown, CTR_EC_PAGE at a time; the game's own cursor does not move. */
void CtrEasyChat_Page(s8 direction)
{
    if (direction < 0)
        sCtrEcFirst = sCtrEcFirst >= CTR_EC_PAGE ? sCtrEcFirst - CTR_EC_PAGE : 0;
    else if (CtrEc_State() == CTR_EC_WORDS_PAGE && sWordData != NULL
          && sCtrEcFirst + CTR_EC_PAGE < GetNumWordsInSelectedGroup())
        sCtrEcFirst += CTR_EC_PAGE;
}

/*
 * Whether the game is in the state a tap was drawn for, and what it asks for
 * is there. A button drawn a frame ago can be tapped as the game leaves its
 * screen; it is the state that tells.
 */
static bool8 CtrEc_Takes(u8 kind, u16 a)
{
    const struct EasyChatScreen *ec = sEasyChatScreen;
    u8 state = CtrEc_State();

    if (state == CTR_EC_NONE)
        return FALSE;
    switch (kind)
    {
    case CTR_EC_ACT_SLOT:
        return state == CTR_EC_MAIN && a < ec->maxWords && a < CTR_EC_WORDS;
    case CTR_EC_ACT_FOOTER:
        return state == CTR_EC_MAIN && a < (FooterHasFourOptions() ? 4 : 3);
    case CTR_EC_ACT_GROUP:
        return state == CTR_EC_KEYBOARD && !ec->inAlphabetMode && a < GetNumUnlockedEasyChatGroups();
    case CTR_EC_ACT_LETTER:
        return state == CTR_EC_KEYBOARD && ec->inAlphabetMode && a / 8 < NUM_ALPHABET_ROWS
            && a % 8 <= GetLastAlphabetColumn(a / 8);
    case CTR_EC_ACT_WORD:
        return state == CTR_EC_WORDS_PAGE && sWordData != NULL && a < GetNumWordsInSelectedGroup();
    case CTR_EC_ACT_KB_MODE:
    case CTR_EC_ACT_KB_DELETE:
        return state == CTR_EC_KEYBOARD;
    case CTR_EC_ACT_BACK:
        /* `a` is the state the button was drawn in. */
        return a == state && (state == CTR_EC_KEYBOARD || state == CTR_EC_WORDS_PAGE || state == CTR_EC_QUIZ);
    case CTR_EC_ACT_MESSAGE:
        return a == state && (state == CTR_EC_MESSAGE || state == CTR_EC_QUIZ);
    }
    return FALSE;
}

void CtrEasyChat_Act(u8 kind, u16 a)
{
    if (!CtrEc_Takes(kind, a))
        return;
    sCtrEcAct = kind;
    sCtrEcA = a;
    sCtrEcSince = gMain.vblankCounter1;
    sCtrEcTtl = CTR_EC_TTL;
    if (kind == CTR_EC_ACT_WORD)
    {
        int now = sEasyChatScreen->wordSelectRow + sEasyChatScreen->wordSelectScrollOffset;
        int away = a / 2 > now ? a / 2 - now : now - a / 2;

        sCtrEcTtl += (away / NUM_WORD_SELECT_ROWS + NUM_WORD_SELECT_ROWS) * CTR_EC_TTL_PAGE;
    }
}

/* The key that takes `now` a step nearer `target` (no wrapping), 0 if there. */
static u16 CtrEc_Toward(int now, int target, u16 less, u16 more)
{
    return now == target ? 0 : now < target ? more : less;
}

/*
 * Called when the game is about to read its input: the key (or none) the tap
 * pending asks for this frame. Dropped when the game has left the state it
 * was for, or when its time is up.
 */
static u16 CtrEc_NextKey(void)
{
    const struct EasyChatScreen *ec = sEasyChatScreen;
    int target = sCtrEcA;
    u16 key;

    if (sCtrEcAct == CTR_EC_ACT_NONE)
        return 0;
    if ((u32)(gMain.vblankCounter1 - sCtrEcSince) > sCtrEcTtl || !CtrEc_Takes(sCtrEcAct, sCtrEcA))
    {
        sCtrEcAct = CTR_EC_ACT_NONE;
        return 0;
    }
    switch (sCtrEcAct)
    {
    case CTR_EC_ACT_BACK:
    case CTR_EC_ACT_MESSAGE:
    case CTR_EC_ACT_KB_MODE:
        key = sCtrEcAct == CTR_EC_ACT_BACK ? B_BUTTON : sCtrEcAct == CTR_EC_ACT_MESSAGE ? A_BUTTON : SELECT_BUTTON;
        sCtrEcAct = CTR_EC_ACT_NONE;
        return key;
    case CTR_EC_ACT_SLOT:
    {
        int row = target / ec->numColumns, column = target % ec->numColumns;

        if (ec->inputState == INPUTSTATE_MAIN_SCREEN_BUTTONS)
            return DPAD_UP;
        key = CtrEc_Toward(ec->mainCursorRow, row, DPAD_UP, DPAD_DOWN);
        if (!key)
            key = CtrEc_Toward(ec->mainCursorColumn, column, DPAD_LEFT, DPAD_RIGHT);
        if (!key)
        {
            sCtrEcAct = CTR_EC_ACT_NONE;
            return A_BUTTON;
        }
        return key;
    }
    case CTR_EC_ACT_FOOTER:
        if (ec->inputState != INPUTSTATE_MAIN_SCREEN_BUTTONS)
            return DPAD_DOWN;
        key = CtrEc_Toward(ec->mainCursorColumn, target, DPAD_LEFT, DPAD_RIGHT);
        if (!key)
        {
            sCtrEcAct = CTR_EC_ACT_NONE;
            return A_BUTTON;
        }
        return key;
    case CTR_EC_ACT_GROUP:
    case CTR_EC_ACT_LETTER:
    {
        bool8 group = sCtrEcAct == CTR_EC_ACT_GROUP;
        int row = group ? target / 2 : target / 8, column = group ? target % 2 : target % 8;
        int rowNow = group ? ec->keyboardRow + ec->keyboardScrollOffset : ec->keyboardRow;

        if (ec->keyboardColumn == -1)
            return DPAD_RIGHT;
        key = CtrEc_Toward(rowNow, row, DPAD_UP, DPAD_DOWN);
        if (!key)
            key = CtrEc_Toward(ec->keyboardColumn, column, DPAD_LEFT, DPAD_RIGHT);
        if (!key)
        {
            sCtrEcAct = CTR_EC_ACT_NONE;
            return A_BUTTON;
        }
        return key;
    }
    case CTR_EC_ACT_KB_DELETE:
        /* To the side buttons (left), to the second, then A. */
        if (ec->keyboardColumn != -1)
            return DPAD_LEFT;
        key = CtrEc_Toward(ec->keyboardRow, 1, DPAD_UP, DPAD_DOWN);
        if (!key)
        {
            sCtrEcAct = CTR_EC_ACT_NONE;
            return A_BUTTON;
        }
        return key;
    case CTR_EC_ACT_WORD:
    {
        /*
         * The rows go four at a time with START / SELECT while that does not
         * pass the word's row, then one at a time. Both pages are the game's
         * (MoveWordSelectCursor): START takes the first visible row up by
         * four or to the top, SELECT down by four or to where the last row
         * is the lowest shown; the cursor keeps its place in the window.
         * Each step moves it nearer, so it always gets there: a page is used
         * only when it can move (there is room), a single row always can.
         */
        int row = target / 2, column = target % 2;
        int first = ec->wordSelectScrollOffset, last = ec->wordSelectLastRow;
        int now = ec->wordSelectRow + first, page;

        if (now < row)
        {
            page = first + NUM_WORD_SELECT_ROWS > last - NUM_WORD_SELECT_ROWS + 1 ? last - NUM_WORD_SELECT_ROWS + 1 - first
                                                                                : NUM_WORD_SELECT_ROWS;
            return first <= last - NUM_WORD_SELECT_ROWS && page <= row - now ? SELECT_BUTTON : DPAD_DOWN;
        }
        if (now > row)
        {
            page = first < NUM_WORD_SELECT_ROWS ? first : NUM_WORD_SELECT_ROWS;
            return first > 0 && page <= now - row ? START_BUTTON : DPAD_UP;
        }
        key = CtrEc_Toward(ec->wordSelectColumn, column, DPAD_LEFT, DPAD_RIGHT);
        if (!key)
        {
            sCtrEcAct = CTR_EC_ACT_NONE;
            return A_BUTTON;
        }
        return key;
    }
    }
    sCtrEcAct = CTR_EC_ACT_NONE;
    return 0;
}

/*
 * The top of HandleEasyChatInput: adds the key of a tap to what the pads
 * gave. A press on the console's own buttons ends the tap: the two would
 * fight for the cursor.
 */
static void CtrEc_Inject(void)
{
    u16 key;

    if ((gMain.newKeys | gMain.newAndRepeatedKeys) & CTR_EC_PAD_KEYS)
    {
        sCtrEcAct = CTR_EC_ACT_NONE;
        return;
    }
    key = CtrEc_NextKey();
    gMain.newKeys |= key;
    gMain.newAndRepeatedKeys |= key;
}
