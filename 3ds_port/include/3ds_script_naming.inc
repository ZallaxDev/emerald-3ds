/*
 * Included at the end of src/scrcmd.c (under PLATFORM_3DS). A script that
 * names a Pokémon - the Name Rater's, and one received into a box - fades the
 * screen to black and only then calls the special that opens the naming
 * screen: by then the top screen has nothing left to keep. So the fade itself
 * asks the bottom screen to hold the world when what follows it is that
 * special (the bottom screen then draws the keyboard). The same for a special
 * that opens the party menu to choose a Pokémon, which the bottom screen
 * draws too.
 */

void ChangePokemonNickname(void);
void ChangeBoxPokemonNickname(void);
void ChooseSendDaycareMon(void);
void ChooseHalfPartyForBattle(void);
void ChoosePartyForBattleFrontier(void);
void ChooseMonForMoveTutor(void);
void ChooseMonForWirelessMinigame(void);
void CtrBottom_NamingAhead(bool8 battle);
void CtrBottom_KeepWorld(void);

#define CTR_SCR_OP_SPECIAL 0x25 // asm/macros/event.inc: special

static void CtrScript_FadeAhead(struct ScriptContext *ctx)
{
    const u8 *next = ctx->scriptPtr;
    u16 index;

    if (next[0] != CTR_SCR_OP_SPECIAL)
        return;
    index = next[1] | (next[2] << 8);
    if (gSpecials[index] == ChangePokemonNickname || gSpecials[index] == ChangeBoxPokemonNickname)
        CtrBottom_NamingAhead(FALSE);
    else if (gSpecials[index] == ChooseSendDaycareMon || gSpecials[index] == ChooseHalfPartyForBattle
          || gSpecials[index] == ChoosePartyForBattleFrontier || gSpecials[index] == ChooseMonForMoveTutor
          || gSpecials[index] == ChooseMonForWirelessMinigame)
        CtrBottom_KeepWorld();
}
