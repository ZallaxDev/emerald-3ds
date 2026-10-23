/*
 * The Nuzlocke tracker's lines of gCtrExtras (src/3ds_nuzlocke_game.c): its
 * opener on ENHANCEMENTS' second screen, the tracker on a third, twelve cells.
 * For src/3ds_extras.c, which includes "3ds_nuzlocke_ui.h" at its top and this
 * file inside gCtrExtras, before its terminating {0}. Off by default
 * (settings.txt nuzlocke=). Off blocks encounter/fallen edits and hides their
 * live values; choice, party selection and save-link text remain available.
 * Existing records still follow successful saves to preserve their binding.
 */
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 1), CTR_TEXT("NUZLOCKE", "NUZLOCKE", "NUZLOCKE", "NUZLOCKE", "NUZLOCKE", "ナズロック"), "nuzlocke_open", 0, 0, gCtrNuzlockeOpenText, CtrNuzlockeUi_Open, NULL, NULL},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("NUZLOCKE", "NUZLOCKE", "NUZLOCKE", "NUZLOCKE", "NUZLOCKE", "ナズロック"), "nuzlocke_back", 0, 0, gCtrNuzlockeBackText, CtrNuzlockeUi_Back, NULL, NULL},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("TRACKER", "REGISTRO", "SUIVI", "REGISTRO", "REGISTRO", "きろく"), CTR_NUZLOCKE_KEY, 2, 0, gCtrExtrasOffOn, CtrNuzlockeUi_Toggled, NULL, NULL},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("AREA", "ZONA", "ZONE", "ZONA", "ÁREA", "ばしょ"), "nuzlocke_area", 0, 0, gCtrNuzlockeInfoText, CtrNuzlockeUi_Info, NULL, CtrNuzlockeUi_AreaText},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("ENCOUNTER", "ENCUENTRO", "RENCONTRE", "INCONTRO", "ENCONTRO", "であい"), "nuzlocke_status", 0, 0, gCtrNuzlockeInfoText, CtrNuzlockeUi_Info, NULL, CtrNuzlockeUi_RecordedText},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("SET TO", "MARCAR COMO", "NOTER COMME", "SEGNA COME", "MARCAR COMO", "きろく"), "nuzlocke_choice", 0, 0, NULL, NULL, CtrNuzlockeUi_ChoiceStep, CtrNuzlockeUi_ChoiceText},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("RECORD", "ANOTAR", "NOTER", "REGISTRA", "ANOTAR", "きろく する"), "nuzlocke_record", 0, 0, gCtrNuzlockeRecordText, CtrNuzlockeUi_Record, NULL, NULL},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("PARTY POKEMON", "POKéMON EQUIPO", "POKÉMON ÉQUIPE", "POKéMON SQUADRA", "POKéMON EQUIPE", "てもちの ポケモン"), "nuzlocke_slot", 0, 0, NULL, NULL, CtrNuzlockeUi_SlotStep, CtrNuzlockeUi_SlotText},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("FALLEN MARK", "MARCA CAÍDO", "MARQUE TOMBÉ", "SEGNO CADUTO", "MARCA CAÍDO", "ひんしの しるし"), "nuzlocke_fallen", 0, 0, gCtrNuzlockeInfoText, CtrNuzlockeUi_Fallen, NULL, CtrNuzlockeUi_FallenText},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("MARK FAINTED", "MARCAR DEBILIT.", "MARQUER K.O.", "SEGNA K.O.", "MARCAR DESMAIO", "ひんしを きろく"), "nuzlocke_fainted", 0, 0, gCtrNuzlockeInfoText, CtrNuzlockeUi_MarkFainted, NULL, CtrNuzlockeUi_MarkFaintedText},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("NEXT LEVEL CAP", "NIVEL MÁXIMO", "NIVEAU MAX", "LIVELLO MAX", "NÍVEL MÁXIMO", "つぎの レベルじょうげん"), "nuzlocke_cap", 0, 0, gCtrNuzlockeInfoText, CtrNuzlockeUi_Info, NULL, CtrNuzlockeUi_CapText},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("CAUGHT/MISS/FALLEN", "CAPT./PERD./CAÍDOS", "PRIS/RATÉS/TOMBÉS", "PRESI/PERSI/CADUTI", "CAPT./PERD./CAÍDOS", "ゲット/にがす/ひんし"), "nuzlocke_totals", 0, 0, gCtrNuzlockeInfoText, CtrNuzlockeUi_Info, NULL, CtrNuzlockeUi_TotalsText},
    {CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 2), CTR_TEXT("SAVE LINK", "PARTIDA", "SAUVEGARDE", "SALVATAGGIO", "JOGO SALVO", "セーブ"), "nuzlocke_link", 0, 0, gCtrNuzlockeInfoText, CtrNuzlockeUi_Info, NULL, CtrNuzlockeUi_LinkText},
