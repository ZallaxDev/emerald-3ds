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
 * does exactly what it would for the keys. The drawing is
 * 3ds_bottom_easychat.inc.
 */

#include "3ds_game_views.h"

#define CTR_EC_TTL 150

static u8 sCtrEcAct, sCtrEcA, sCtrEcState, sCtrEcTtl, sCtrEcReady;

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
    }
    return CTR_EC_OTHER;
}

bool8 CtrEasyChat_GetView(struct CtrEasyChatView *view)
{
    u8 i;

    memset(view, 0, sizeof(*view));
    view->state = CtrEc_State();
    if (view->state == CTR_EC_NONE)
        return FALSE;
    view->ready = sCtrEcReady > 0;
    if (sCtrEcReady)
        sCtrEcReady--;
    view->columns = sEasyChatScreen->numColumns;
    view->rows = sEasyChatScreen->numRows;
    view->maxWords = sEasyChatScreen->maxWords > CTR_EC_WORDS ? CTR_EC_WORDS : sEasyChatScreen->maxWords;
    view->footer = FooterHasFourOptions() ? 4 : 3;
    view->mainColumn = sEasyChatScreen->mainCursorColumn;
    view->mainRow = sEasyChatScreen->mainCursorRow;
    view->alphabet = sEasyChatScreen->inAlphabetMode;
    view->keyColumn = sEasyChatScreen->keyboardColumn;
    view->keyRow = sEasyChatScreen->keyboardRow;
    view->keyScroll = sEasyChatScreen->keyboardScrollOffset;
    view->numGroups = GetNumUnlockedEasyChatGroups();
    view->numWords = GetNumWordsInSelectedGroup();
    view->wordColumn = sEasyChatScreen->wordSelectColumn;
    view->wordRow = sEasyChatScreen->wordSelectRow + sEasyChatScreen->wordSelectScrollOffset;
    view->title = sEasyChatScreen->titleText;
    for (i = 0; i < view->maxWords; i++)
    {
        view->phrase[i] = sEasyChatScreen->currentPhrase[i];
        if (view->phrase[i] != EC_EMPTY_WORD)
        {
            u8 *end = CopyEasyChatWord(view->phraseText[i], view->phrase[i]);

            if (end - view->phraseText[i] >= CTR_EC_TEXT)
                view->phraseText[i][CTR_EC_TEXT - 1] = EOS;
        }
        else
        {
            view->phraseText[i][0] = EOS;
        }
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
    {
        u8 *end = CopyEasyChatWord(dst, word);

        if (end - dst >= CTR_EC_TEXT)
            dst[CTR_EC_TEXT - 1] = EOS;
    }
}

void CtrEasyChat_Act(u8 kind, u8 a)
{
    sCtrEcAct = kind;
    sCtrEcA = a;
    sCtrEcState = CtrEc_State();
    sCtrEcTtl = CTR_EC_TTL;
}

/* The key that takes `now` a step nearer `target` (no wrapping), 0 if there. */
static u16 CtrEc_Toward(int now, int target, u16 less, u16 more)
{
    return now == target ? 0 : now < target ? more : less;
}

/*
 * Called when the game is about to read its input: the key (or none) the tap
 * pending asks for this frame. Dropped when the screen it was for has moved
 * on, or after CTR_EC_TTL frames.
 */
static u16 CtrEc_NextKey(void)
{
    const struct EasyChatScreen *ec = sEasyChatScreen;
    int target = sCtrEcA;
    u16 key;

    if (sCtrEcAct == CTR_EC_ACT_NONE)
        return 0;
    if (sCtrEcTtl == 0 || --sCtrEcTtl == 0 || CtrEc_State() != sCtrEcState)
    {
        sCtrEcAct = CTR_EC_ACT_NONE;
        return 0;
    }
    switch (sCtrEcAct)
    {
    case CTR_EC_ACT_BACK:
    case CTR_EC_ACT_MESSAGE:
    case CTR_EC_ACT_KB_MODE:
    case CTR_EC_ACT_START:
    case CTR_EC_ACT_SELECT:
    case CTR_EC_ACT_UP:
    case CTR_EC_ACT_DOWN:
        key = sCtrEcAct == CTR_EC_ACT_BACK ? B_BUTTON
            : sCtrEcAct == CTR_EC_ACT_MESSAGE ? A_BUTTON
            : sCtrEcAct == CTR_EC_ACT_KB_MODE || sCtrEcAct == CTR_EC_ACT_SELECT ? SELECT_BUTTON
            : sCtrEcAct == CTR_EC_ACT_START ? START_BUTTON
            : sCtrEcAct == CTR_EC_ACT_UP ? DPAD_UP : DPAD_DOWN;
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

        if (ec->inAlphabetMode == group)
        {
            sCtrEcAct = CTR_EC_ACT_NONE;
            return 0;
        }
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
        int row = target / 2, column = target % 2;

        key = CtrEc_Toward(ec->wordSelectRow + ec->wordSelectScrollOffset, row, DPAD_UP, DPAD_DOWN);
        if (!key)
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

/* The top of HandleEasyChatInput: adds the key of a tap to what the pads gave. */
static void CtrEc_Inject(void)
{
    u16 key;

    sCtrEcReady = 3;
    key = CtrEc_NextKey();
    gMain.newKeys |= key;
    gMain.newAndRepeatedKeys |= key;
}
