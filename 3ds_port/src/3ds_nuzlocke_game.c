/*
 * The Nuzlocke tracker in the game (3ds_nuzlocke.h, 3ds_nuzlocke_ui.h): the
 * session of the loaded save, the save hooks, and the OPTIONS cells' callbacks
 * over the current area, the party and the badges. It only records what the
 * player confirms: no catch, battle or release is changed or prevented.
 */
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "3ds_locale.h"
#include "3ds_extras.h"
#include "3ds_platform.h"
#include "3ds_nuzlocke.h"
#include "3ds_nuzlocke_core.h"
#include "3ds_nuzlocke_store.h"
#include "3ds_nuzlocke_ui.h"
#include "event_data.h"
#include "fieldmap.h"
#include "region_map.h"
#include "pokemon.h"
#include "sound.h"
#include "characters.h"
#include "constants/flags.h"
#include "constants/region_map_sections.h"
#include "constants/songs.h"
#include "constants/species.h"

static CtrNuzSession sSession;
static CtrNuzStatus sChoice = CTR_NUZ_CAUGHT;
static u8 sSlot;
/* A first tap arms a mark, the second (on the same Pokemon) makes it. */
enum { ARM_NONE, ARM_FALLEN, ARM_FAINTED };
static u8 sArm;
static u32 sArmPersonality, sArmOtId;
static bool sArmShedinja;

const char *const gCtrNuzlockeOpenText[1] = {CTR_TEXT("TAP TO OPEN", "TOCA PARA ABRIR", "OUVRIR", "APRI", "TOQUE P/ ABRIR", "ÖFFNEN", "タッチで ひらく")};
const char *const gCtrNuzlockeBackText[1] = {CTR_TEXT("BACK", "VOLVER", "RETOUR", "INDIETRO", "VOLTAR", "ZURÜCK", "もどる")};
const char *const gCtrNuzlockeRecordText[1] = {CTR_TEXT("TAP TO RECORD", "TOCA PARA ANOTAR", "NOTER", "REGISTRA", "TOQUE P/ ANOTAR", "ZUM EINTRAGEN", "タッチで きろく")};
const char *const gCtrNuzlockeInfoText[1] = {"--"};

static const char *const sStatusText[CTR_NUZ_STATUS_COUNT] = {
    CTR_TEXT("UNUSED", "LIBRE", "LIBRE", "LIBERO", "LIVRE", "FREI", "まだ"),
    CTR_TEXT("CAUGHT", "CAPTURADO", "CAPTURÉ", "CATTURATO", "CAPTURADO", "GEFANGEN", "つかまえた"),
    CTR_TEXT("MISSED", "PERDIDO", "RATÉ", "PERSO", "PERDIDO", "VERPASST", "にがした"),
};
static const char *const sAliveText[] = {CTR_TEXT("ALIVE", "VIVO", "VIVANT", "VIVO", "VIVO", "LEBT", "げんき")};
static const char *const sFallenText[] = {CTR_TEXT("FALLEN", "CAÍDO", "TOMBÉ", "CADUTO", "CAÍDO", "GEFALLEN", "ひんし")};
static const char *const sEggText[] = {CTR_TEXT("EGG", "HUEVO", "OEUF", "UOVO", "OVO", "EI", "タマゴ")};
static const char *const sConfirmText[] = {CTR_TEXT("TAP TO CONFIRM", "TOCA OTRA VEZ", "CONFIRMER", "CONFERMA", "TOQUE DE NOVO", "BESTÄTIGEN", "もういちど タッチ")};
static const char *const sNoneText[] = {CTR_TEXT("NONE", "NINGUNO", "AUCUN", "NESSUNO", "NENHUM", "KEINE", "なし")};
static const char *const sToMarkFormat[] = {CTR_TEXT("%u TO MARK", "%u POR MARCAR", "%u À MARQUER", "%u DA SEGNARE", "%u A MARCAR", "%u ZU MARKIEREN", "のこり %u")};
static const char *const sGymFormat[] = {CTR_TEXT("GYM %u: LV %u", "GIM. %u: NV %u", "ARÈNE %u: N %u", "PAL. %u: LV %u", "GIN. %u: NV %u", "ARENA %u: LV %u", "ジム %u: Lv %u")};
static const char *const sChampionFormat[] = {CTR_TEXT("CHAMPION: LV %u", "CAMPEÓN: NV %u", "MAÎTRE: N %u", "CAMPIONE: LV %u", "CAMPEÃO: NV %u", "CHAMP: LV %u", "チャンピオン: Lv %u")};
static const char *const sLabelText[CTR_NUZ_LABEL_COUNT] = {
    CTR_TEXT("THIS SAVE", "ESTA PARTIDA", "CETTE PARTIE", "QUESTA PARTITA", "ESTE JOGO", "DIESER STAND", "この セーブ"),
    CTR_TEXT("NEW", "NUEVO", "NOUVEAU", "NUOVO", "NOVO", "NEU", "あたらしく"),
    CTR_TEXT("OTHER SAVE", "OTRA PARTIDA", "AUTRE PARTIE", "ALTRA PARTITA", "OUTRO JOGO", "ANDERER STAND", "べつの セーブ"),
    CTR_TEXT("DAMAGED", "DAÑADO", "ENDOMMAGÉ", "DANNEGGIATO", "DANIFICADO", "BESCHÄDIGT", "こわれている"),
    CTR_TEXT("SAVE TO KEEP", "GUARDA PARA FIJAR", "SAUVER POUR GARDER", "SALVA PER TENERE", "SALVE P/ MANTER", "SPEICHERN NÖTIG", "セーブで ほぞん"),
    CTR_TEXT("WRITE FAILED", "ERROR AL ESCRIBIR", "ÉCHEC ÉCRITURE", "ERRORE SCRITTURA", "ERRO AO GRAVAR", "SCHREIBFEHLER", "かきこみ しっぱい"),
};

#define TEXT_SIZE 40

/*
 * ASCII and UTF-8 text in the game's characters: letters, digits and a few
 * marks; an accented Latin letter as its plain one (the cells' own texts keep
 * the accents, these dynamic ones are drawn with the game's font).
 */
/* U+00C0..U+00FF; a space: no plain letter, dropped. */
static const char sLatin1Plain[] = "AAAAAAACEEEEIIIIDNOOOOO OUUUUY s" "aaaaaaaceeeeiiiidnooooo ouuuuy y";

static u8 GameChar(unsigned c)
{
    if (c >= 'A' && c <= 'Z')
        return CHAR_A + (c - 'A');
    if (c >= 'a' && c <= 'z')
        return CHAR_a + (c - 'a');
    if (c >= '0' && c <= '9')
        return CHAR_0 + (c - '0');
    switch (c)
    {
    case '.': return CHAR_PERIOD;
    case '/': return CHAR_SLASH;
    case ':': return CHAR_COLON;
    case '-': return CHAR_HYPHEN;
    case ',': return CHAR_COMMA;
    case '!': return CHAR_EXCL_MARK;
    case '?': return CHAR_QUESTION_MARK;
    default: return CHAR_SPACE;
    }
}

/* Appends `text` at `out` (`used` characters already), EOS-terminated. */
static unsigned AppendText(u8 *out, unsigned used, const char *text)
{
    const unsigned char *s = (const unsigned char *)text;

    while (*s != '\0' && used + 1 < TEXT_SIZE)
    {
        unsigned c = *s++;

        if (c >= 0x80)
        {
            /* Two-byte Latin-1 letters (U+00C0..U+00FF) by their plain form;
             * anything else dropped with its continuation bytes. */
            if (c == 0xC3 && *s >= 0x80 && *s <= 0xBF)
            {
                c = (unsigned char)sLatin1Plain[*s++ - 0x80];
                if (c == ' ')
                    continue;
            }
            else
            {
                while (*s >= 0x80 && *s <= 0xBF)
                    s++;
                continue;
            }
        }
        out[used++] = GameChar(c);
    }
    out[used] = EOS;
    return used;
}

static const u8 *SetText(u8 *out, const char *text)
{
    AppendText(out, 0, text);
    return out;
}

static bool Enabled(void)
{
    return CtrSettings_GetInt(CTR_NUZLOCKE_KEY, 0) == 1;
}

/* The current area: the map's region map section, in the field only. */
static bool CurrentArea(unsigned *area)
{
    unsigned section;

    if (!CtrGame_IsOverworld())
        return false;
    section = gMapHeader.regionMapSectionId;
    if (section >= MAPSEC_NONE || section >= CTR_NUZ_AREA_COUNT)
        return false;
    *area = section;
    return true;
}

static unsigned BadgeCount(void)
{
    static const u16 badges[] = {FLAG_BADGE01_GET, FLAG_BADGE02_GET, FLAG_BADGE03_GET, FLAG_BADGE04_GET,
                                 FLAG_BADGE05_GET, FLAG_BADGE06_GET, FLAG_BADGE07_GET, FLAG_BADGE08_GET};
    unsigned count = 0;

    for (unsigned i = 0; i < ARRAY_COUNT(badges); i++)
        count += FlagGet(badges[i]) ? 1 : 0;
    return count;
}

static unsigned PartyCount(void)
{
    if (!CtrGame_IsOverworld())
        return 0;
    return gPlayerPartyCount <= PARTY_SIZE ? gPlayerPartyCount : PARTY_SIZE;
}

/* The chosen party Pokemon, or NULL (no party, an empty slot). */
static struct Pokemon *SlotMon(void)
{
    unsigned count = PartyCount();

    if (count == 0)
        return NULL;
    if (sSlot >= count)
        sSlot = 0;
    if (GetMonData(&gPlayerParty[sSlot], MON_DATA_SPECIES) == 0)
        return NULL;
    return &gPlayerParty[sSlot];
}

static bool IsEgg(struct Pokemon *mon)
{
    return GetMonData(mon, MON_DATA_IS_EGG) != 0;
}

/* Fainted party Pokemon not yet marked; marks them too when `mark`. */
static unsigned FaintedUnmarked(bool mark)
{
    unsigned count = PartyCount(), found = 0;

    for (unsigned i = 0; i < count; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u32 personality, otId;
        bool shedinja;

        if (GetMonData(mon, MON_DATA_SPECIES) == 0 || IsEgg(mon) || GetMonData(mon, MON_DATA_HP) != 0)
            continue;
        personality = GetMonData(mon, MON_DATA_PERSONALITY);
        otId = GetMonData(mon, MON_DATA_OT_ID);
        shedinja = GetMonData(mon, MON_DATA_SPECIES) == SPECIES_SHEDINJA;
        if (CtrNuz_IsFallen(&sSession.state, personality, otId, shedinja))
            continue;
        if (mark && !CtrNuz_SetFallen(&sSession.state, personality, otId, shedinja, true))
            break;
        found++;
    }
    return found;
}

/* Save hooks (3ds_nuzlocke.h). */

void CtrNuzlocke_Loaded(const void *save, size_t size)
{
    CtrNuzLink link = CtrNuzStore_Load(save, size, &sSession.state);

    CtrNuzSession_Loaded(&sSession, link);
    sArm = ARM_NONE;
    sSlot = 0;
}

void CtrNuzlocke_Saved(const void *save, size_t size)
{
    if (CtrNuzSession_ShouldWrite(&sSession, Enabled()))
        CtrNuzSession_Written(&sSession, CtrNuzStore_Save(&sSession.state, save, size));
}

void CtrNuzlocke_NewGame(void)
{
    CtrNuzSession_NewGame(&sSession);
    sArm = ARM_NONE;
    sSlot = 0;
}

/* OPTIONS cells (3ds_nuzlocke_ui.h). */

void CtrNuzlockeUi_Open(void)
{
    sArm = ARM_NONE;
    CtrExtras_ShowScreen(CTR_EXTRAS_ENHANCEMENTS, 2);
}

void CtrNuzlockeUi_Back(void)
{
    sArm = ARM_NONE;
    CtrExtras_ShowScreen(CTR_EXTRAS_ENHANCEMENTS, 1);
}

void CtrNuzlockeUi_Toggled(void)
{
    sArm = ARM_NONE;
}

void CtrNuzlockeUi_Info(void)
{
    PlaySE(SE_SELECT);
}

const uint8_t *CtrNuzlockeUi_AreaText(void)
{
    static u8 text[TEXT_SIZE];
    unsigned area;

    if (!Enabled() || !CurrentArea(&area))
        return SetText(text, "--");
    /* Map names are short (at most 16 characters, as the region map shows them). */
    GetMapName(text, area, 0);
    text[TEXT_SIZE - 1] = EOS;
    return text;
}

const uint8_t *CtrNuzlockeUi_RecordedText(void)
{
    static u8 text[TEXT_SIZE];
    unsigned area;

    if (!Enabled() || !CurrentArea(&area))
        return SetText(text, "--");
    return SetText(text, sStatusText[CtrNuz_Area(&sSession.state, area)]);
}

void CtrNuzlockeUi_ChoiceStep(int direction)
{
    sChoice = (CtrNuzStatus)((sChoice + (direction < 0 ? CTR_NUZ_STATUS_COUNT - 1 : 1)) % CTR_NUZ_STATUS_COUNT);
}

const uint8_t *CtrNuzlockeUi_ChoiceText(void)
{
    static u8 text[TEXT_SIZE];

    return SetText(text, sStatusText[sChoice < CTR_NUZ_STATUS_COUNT ? sChoice : CTR_NUZ_UNUSED]);
}

void CtrNuzlockeUi_Record(void)
{
    unsigned area;

    sArm = ARM_NONE;
    if (!Enabled() || !CurrentArea(&area) || !CtrNuz_SetArea(&sSession.state, area, sChoice))
    {
        PlaySE(SE_FAILURE);
        return;
    }
    CtrNuzSession_Edited(&sSession);
    PlaySE(SE_SUCCESS);
}

void CtrNuzlockeUi_SlotStep(int direction)
{
    unsigned count = PartyCount();

    sArm = ARM_NONE;
    if (count == 0)
    {
        sSlot = 0;
        return;
    }
    sSlot = (sSlot % count + (direction < 0 ? count - 1 : 1)) % count;
}

const uint8_t *CtrNuzlockeUi_SlotText(void)
{
    static u8 text[TEXT_SIZE];
    u8 name[POKEMON_NAME_LENGTH + 2];
    struct Pokemon *mon = SlotMon();
    unsigned used, i;

    if (mon == NULL)
        return SetText(text, "--");
    text[0] = CHAR_0 + sSlot + 1;
    text[1] = CHAR_SPACE;
    used = 2;
    GetMonData(mon, MON_DATA_NICKNAME, name);
    name[POKEMON_NAME_LENGTH + 1] = EOS;
    for (i = 0; name[i] != EOS && used + 1 < TEXT_SIZE; i++)
        text[used++] = name[i];
    text[used] = EOS;
    return text;
}

void CtrNuzlockeUi_Fallen(void)
{
    struct Pokemon *mon = Enabled() ? SlotMon() : NULL;
    u32 personality, otId;
    bool shedinja;
    bool fallen;

    if (mon == NULL || IsEgg(mon))
    {
        sArm = ARM_NONE;
        PlaySE(SE_FAILURE);
        return;
    }
    personality = GetMonData(mon, MON_DATA_PERSONALITY);
    otId = GetMonData(mon, MON_DATA_OT_ID);
    shedinja = GetMonData(mon, MON_DATA_SPECIES) == SPECIES_SHEDINJA;
    if (sArm != ARM_FALLEN || sArmPersonality != personality || sArmOtId != otId || sArmShedinja != shedinja)
    {
        sArm = ARM_FALLEN;
        sArmPersonality = personality;
        sArmOtId = otId;
        sArmShedinja = shedinja;
        PlaySE(SE_SELECT);
        return;
    }
    sArm = ARM_NONE;
    fallen = CtrNuz_IsFallen(&sSession.state, personality, otId, shedinja);
    if (!CtrNuz_SetFallen(&sSession.state, personality, otId, shedinja, !fallen))
    {
        PlaySE(SE_FAILURE);
        return;
    }
    CtrNuzSession_Edited(&sSession);
    PlaySE(SE_SUCCESS);
}

const uint8_t *CtrNuzlockeUi_FallenText(void)
{
    static u8 text[TEXT_SIZE];
    struct Pokemon *mon = Enabled() ? SlotMon() : NULL;
    u32 personality, otId;
    bool shedinja;

    if (mon == NULL)
        return SetText(text, "--");
    if (IsEgg(mon))
        return SetText(text, sEggText[0]);
    personality = GetMonData(mon, MON_DATA_PERSONALITY);
    otId = GetMonData(mon, MON_DATA_OT_ID);
    shedinja = GetMonData(mon, MON_DATA_SPECIES) == SPECIES_SHEDINJA;
    if (sArm == ARM_FALLEN && sArmPersonality == personality && sArmOtId == otId && sArmShedinja == shedinja)
        return SetText(text, sConfirmText[0]);
    return SetText(text, CtrNuz_IsFallen(&sSession.state, personality, otId, shedinja) ? sFallenText[0] : sAliveText[0]);
}

void CtrNuzlockeUi_MarkFainted(void)
{
    if (!Enabled() || FaintedUnmarked(false) == 0)
    {
        sArm = ARM_NONE;
        PlaySE(SE_FAILURE);
        return;
    }
    if (sArm != ARM_FAINTED)
    {
        sArm = ARM_FAINTED;
        PlaySE(SE_SELECT);
        return;
    }
    sArm = ARM_NONE;
    if (FaintedUnmarked(true) == 0)
    {
        PlaySE(SE_FAILURE);
        return;
    }
    CtrNuzSession_Edited(&sSession);
    PlaySE(SE_SUCCESS);
}

const uint8_t *CtrNuzlockeUi_MarkFaintedText(void)
{
    static u8 text[TEXT_SIZE];
    char ascii[TEXT_SIZE];
    unsigned count;

    if (!Enabled() || !CtrGame_IsOverworld())
        return SetText(text, "--");
    count = FaintedUnmarked(false);
    if (count == 0)
        return SetText(text, sNoneText[0]);
    if (sArm == ARM_FAINTED)
        return SetText(text, sConfirmText[0]);
    snprintf(ascii, sizeof(ascii), sToMarkFormat[0], count);
    return SetText(text, ascii);
}

const uint8_t *CtrNuzlockeUi_CapText(void)
{
    static u8 text[TEXT_SIZE];
    char ascii[TEXT_SIZE];
    unsigned leader, cap;

    if (!Enabled() || !CtrGame_IsOverworld())
        return SetText(text, "--");
    cap = CtrNuz_NextCap(BadgeCount(), &leader);
    if (leader > 8)
        snprintf(ascii, sizeof(ascii), sChampionFormat[0], cap);
    else
        snprintf(ascii, sizeof(ascii), sGymFormat[0], leader, cap);
    return SetText(text, ascii);
}

const uint8_t *CtrNuzlockeUi_TotalsText(void)
{
    static u8 text[TEXT_SIZE];
    char ascii[TEXT_SIZE];
    unsigned caught, missed, fallen;

    if (!Enabled())
        return SetText(text, "--");
    CtrNuz_Counts(&sSession.state, &caught, &missed, &fallen);
    snprintf(ascii, sizeof(ascii), "%u/%u/%u", caught, missed, fallen);
    return SetText(text, ascii);
}

const uint8_t *CtrNuzlockeUi_LinkText(void)
{
    static u8 text[TEXT_SIZE];
    CtrNuzLabel label = CtrNuzSession_Label(&sSession);

    return SetText(text, sLabelText[label < CTR_NUZ_LABEL_COUNT ? label : CTR_NUZ_LABEL_NEW]);
}

int CtrNuzlockeUi_Enabled(void)
{
    return Enabled();
}

int CtrNuzlockeUi_AreaStatus(void)
{
    unsigned area;

    if (!Enabled() || !CurrentArea(&area))
        return -1;
    return CtrNuz_Area(&sSession.state, area);
}

void CtrNuzlockeUi_RecordAs(int status)
{
    if (status < 0 || status >= CTR_NUZ_STATUS_COUNT)
        return;
    sChoice = (CtrNuzStatus)status;
    CtrNuzlockeUi_Record();
}

int CtrNuzlockeUi_Slot(void)
{
    unsigned count = PartyCount();

    return count == 0 ? -1 : (int)(sSlot % count);
}

void CtrNuzlockeUi_SetSlot(int slot)
{
    if (slot < 0 || (unsigned)slot >= PartyCount() || GetMonData(&gPlayerParty[slot], MON_DATA_SPECIES) == 0)
        return;
    if (sSlot != slot)
        sArm = ARM_NONE;
    sSlot = slot;
}

int CtrNuzlockeUi_SlotFallen(int slot)
{
    struct Pokemon *mon;

    if (!Enabled() || slot < 0 || (unsigned)slot >= PartyCount())
        return 0;
    mon = &gPlayerParty[slot];
    if (GetMonData(mon, MON_DATA_SPECIES) == 0 || IsEgg(mon))
        return 0;
    return CtrNuz_IsFallen(&sSession.state, GetMonData(mon, MON_DATA_PERSONALITY), GetMonData(mon, MON_DATA_OT_ID),
                           GetMonData(mon, MON_DATA_SPECIES) == SPECIES_SHEDINJA);
}
