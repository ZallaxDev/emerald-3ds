/*
 * Game data backends: RomFS, loose files on the SD card, or one data pack.
 * See include/3ds_data.h for the contract and docs/ASSET_PIPELINE.md for the
 * pack format, which builder/emerald3ds_builder/pak.py writes.
 */

#define _GNU_SOURCE
#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "3ds_data.h"
#include "3ds_pak.h"
#include "3ds_platform.h"
#include "3ds_locale.h"
#include "../../include/constants/global.h"

/* 0 = choose at runtime; 1 = RomFS, 2 = loose, 3 = pack. */
#ifndef CTR_DATA_BACKEND
#define CTR_DATA_BACKEND 0
#endif

#define ENGINE_ABI_PATH "engine/abi.bin"

/* The ROM the data pack must come from: the one of the game's compiled
 * language, never another locale's data. */
#if GAME_LANGUAGE == LANGUAGE_SPANISH
static const uint8_t sSupportedRomSha1[20] = {
    0xfe, 0x15, 0x58, 0xa3, 0xdc, 0xb0, 0x36, 0x0a, 0xb5, 0x58,
    0x96, 0x9e, 0x09, 0xb6, 0x90, 0x88, 0x8b, 0x84, 0x6d, 0xd9,
};
#define SUPPORTED_ROM_DETAIL "Usa la ROM de Pokemon Esmeralda\n" \
                             "(Espana, BPES) con este cliente."
#elif GAME_LANGUAGE == LANGUAGE_FRENCH
static const uint8_t sSupportedRomSha1[20] = {
    0xca, 0x66, 0x66, 0x51, 0x37, 0x4d, 0x89, 0xca, 0x43, 0x90,
    0x07, 0xbe, 0xd5, 0x4d, 0x83, 0x9e, 0xb7, 0xbd, 0x14, 0xd0,
};
#define SUPPORTED_ROM_DETAIL "Utilise la ROM de Pokemon Emeraude\n" \
                             "(France, BPEF) avec ce client."
#elif GAME_LANGUAGE == LANGUAGE_ITALIAN
static const uint8_t sSupportedRomSha1[20] = {
    0x16, 0x92, 0xdb, 0x32, 0x24, 0x00, 0xc3, 0x14, 0x1c, 0x5d,
    0xe2, 0xdb, 0x38, 0x46, 0x99, 0x13, 0xce, 0xb1, 0xf4, 0xd4,
};
#define SUPPORTED_ROM_DETAIL "Usa la ROM di Pokemon Smeraldo\n" \
                             "(Italia, BPEI) con questo client."
#elif GAME_LANGUAGE == LANGUAGE_GERMAN
static const uint8_t sSupportedRomSha1[20] = {
    0x61, 0xc2, 0xeb, 0x2b, 0x38, 0x0b, 0x1a, 0x75, 0xf0, 0xc9,
    0x4b, 0x76, 0x7a, 0x2d, 0x4c, 0x26, 0xcd, 0x7c, 0xe4, 0xe3,
};
#define SUPPORTED_ROM_DETAIL "Nutze das ROM von Pokemon Smaragd\n" \
                             "(Deutschland, BPED) mit diesem Client."
#elif GAME_LANGUAGE == LANGUAGE_JAPANESE
static const uint8_t sSupportedRomSha1[20] = {
    0xd7, 0xcf, 0x8f, 0x15, 0x6b, 0xa9, 0xc4, 0x55, 0xd1, 0x64,
    0xe1, 0xea, 0x78, 0x0a, 0x6b, 0xf1, 0x94, 0x54, 0x65, 0xc2,
};
#define SUPPORTED_ROM_DETAIL "Use the Pocket Monsters Emerald\n" \
                             "(Japan, BPEJ) ROM with this client."
#else
static const uint8_t sSupportedRomSha1[20] = {
    0xf3, 0xae, 0x08, 0x81, 0x81, 0xbf, 0x58, 0x3e, 0x55, 0xda,
    0xf9, 0x62, 0xa9, 0x2b, 0xb4, 0x6f, 0x4f, 0x1d, 0x07, 0xb7,
};
#define SUPPORTED_ROM_DETAIL CTR_TEXT_PT("Only Pokemon Emerald (USA, Europe) is\n" \
                                         "supported. Run the builder with that ROM.", \
                                         "So a ROM de Pokemon Emerald (USA,\n" \
                                         "Europe) e aceita. Rode o builder com ela.")
#endif

typedef struct
{
    uint64_t base;
    uint32_t size;
    uint32_t pos;
} PakStream;

static CtrDataBackend sBackend = CTR_DATA_NONE;
static uint32_t sEngineAbi;
static FILE *sPak;
static CtrPakEntry *sEntries;
static uint32_t sEntryCount;
static LightLock sPakLock;
static char sErrorTitle[96];
static char sErrorDetail[512];

static bool FileExists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void SetError(const char *title, const char *detail)
{
    snprintf(sErrorTitle, sizeof(sErrorTitle), "%s", title);
    snprintf(sErrorDetail, sizeof(sErrorDetail), "%s", detail);
    CtrLog_Write(CTR_LOG_ERROR, "data: %s", title);
}

static void ReadEngineAbi(void)
{
    FILE *file = CtrFs_OpenAsset(ENGINE_ABI_PATH);
    uint8_t raw[4] = {0};

    sEngineAbi = 0;
    if (file != NULL)
    {
        if (fread(raw, 1, sizeof(raw), file) == sizeof(raw))
            sEngineAbi = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16)
                       | ((uint32_t)raw[3] << 24);
        fclose(file);
    }
}

/* The ABI `staging.py devdata` writes into the loose data's marker
 * ("abi %08x"); false when it has none (older tooling, or a forced loose
 * backend without a marker). */
static bool ReadLooseAbi(uint32_t *abi)
{
    FILE *file = fopen(CTR_DATA_LOOSE_MARKER, "r");
    unsigned long value;
    bool found;

    if (file == NULL)
        return false;
    found = fscanf(file, " abi %lx", &value) == 1;
    fclose(file);
    if (found)
        *abi = (uint32_t)value;
    return found;
}

static const CtrPakEntry *FindEntry(const char *path)
{
    return CtrPak_Find(sEntries, sEntryCount, path);
}

static bool OpenPak(void)
{
    uint8_t header[CTR_PAK_HEADER_BYTES];
    uint8_t *index = NULL;
    CtrPakHeader info;
    CtrPakStatus status;
    char detail[512];

    sPak = fopen(CTR_DATA_PAK_PATH, "rb");
    if (sPak == NULL)
    {
        SetError(CTR_TEXT("The game data pack was not found.", "The game data pack was not found.", "Le pack de donnees est introuvable.", "Pacchetto dati di gioco non trovato.", "O pacote de dados nao foi encontrado.", "Das Datenpaket wurde nicht gefunden.", "The game data pack was not found."),
                 CTR_TEXT_FR_IT_PT_DE("Run the builder that came with this\n"
                             "release with your own Pokemon Emerald\n"
                             "ROM and install the data pack to:\n\n"
                             "  /3ds/emerald3ds/emerald3ds.pak",
                             "Lance le builder fourni avec cette\n"
                             "version et ta ROM de Pokemon Emeraude,\n"
                             "puis installe le pack de donnees ici :\n\n"
                             "  /3ds/emerald3ds/emerald3ds.pak",
                             "Avvia il builder fornito con questa\n"
                             "versione usando la tua ROM di Pokemon\n"
                             "Smeraldo e installa il pacchetto in:\n\n"
                             "  /3ds/emerald3ds/emerald3ds.pak",
                             "Rode o builder que veio com esta\n"
                             "versao com a sua ROM de Pokemon\n"
                             "Emerald e instale o pacote em:\n\n"
                             "  /3ds/emerald3ds/emerald3ds.pak",
                             "Starte den Builder dieser Version\n"
                             "mit deinem eigenen Smaragd-ROM und\n"
                             "installiere das Datenpaket nach:\n\n"
                             "  /3ds/emerald3ds/emerald3ds.pak"));
        return false;
    }
    setvbuf(sPak, NULL, _IOFBF, 64 * 1024);
    status = fread(header, 1, sizeof(header), sPak) == sizeof(header)
           ? CtrPak_ParseHeader(header, sEngineAbi, sSupportedRomSha1, &info) : CTR_PAK_BAD_MAGIC;
    switch (status)
    {
    case CTR_PAK_OK:
        break;
    case CTR_PAK_BAD_MAGIC:
        SetError(CTR_TEXT("This is not a game data pack.", "This is not a game data pack.", "Ce fichier n'est pas un pack de donnees.", "Questo non e' un pacchetto di dati.", "Isto nao e um pacote de dados.", "Das ist kein Datenpaket des Spiels.", "This is not a game data pack."),
                 CTR_TEXT_FR_IT_PT_DE("/3ds/emerald3ds/emerald3ds.pak is not a\n"
                             "data pack. Run the builder again.",
                             "/3ds/emerald3ds/emerald3ds.pak n'est pas\n"
                             "un pack de donnees. Relance le builder.",
                             "/3ds/emerald3ds/emerald3ds.pak non e' un\n"
                             "pacchetto di dati. Riavvia il builder.",
                             "/3ds/emerald3ds/emerald3ds.pak nao e um\n"
                             "pacote de dados. Rode o builder de novo.",
                             "/3ds/emerald3ds/emerald3ds.pak ist kein\n"
                             "Datenpaket. Starte den Builder erneut."));
        goto fail;
    case CTR_PAK_BAD_SCHEMA:
    case CTR_PAK_BAD_ABI:
        snprintf(detail, sizeof(detail),
                 CTR_TEXT_FR_IT_PT_DE("It was generated for a different\n"
                             "release. Run the builder that came\n"
                             "with this release again.\n\n"
                             "Engine ABI: %08lX (schema %u)\n"
                             "Pack ABI:   %08lX (schema %lu)",
                             "Ce pack a ete genere pour une autre\n"
                             "version. Relance le builder fourni\n"
                             "avec cette version.\n\n"
                             "ABI du moteur : %08lX (schema %u)\n"
                             "ABI du pack :   %08lX (schema %lu)",
                             "Il pacchetto proviene da un'altra\n"
                             "versione. Riavvia il builder fornito\n"
                             "con questa versione.\n\n"
                             "ABI motore:    %08lX (schema %u)\n"
                             "ABI pacchetto: %08lX (schema %lu)",
                             "Ele foi gerado para outra versao.\n"
                             "Rode de novo o builder que veio\n"
                             "com esta versao.\n\n"
                             "ABI do motor:  %08lX (schema %u)\n"
                             "ABI do pacote: %08lX (schema %lu)",
                             "Es wurde fuer eine andere Version\n"
                             "erstellt. Starte den Builder dieser\n"
                             "Version erneut.\n\n"
                             "Engine-ABI: %08lX (Schema %u)\n"
                             "Paket-ABI:  %08lX (Schema %lu)"),
                 (unsigned long)sEngineAbi, CTR_PAK_SCHEMA_VERSION,
                 (unsigned long)info.engineAbi, (unsigned long)info.schema);
        SetError(CTR_TEXT("The data pack does not match this release.", "The data pack does not match this release.", "Le pack ne correspond pas a cette version.", "Il pacchetto non corrisponde alla versione.", "O pacote de dados nao e desta versao.", "Datenpaket passt nicht zu dieser Version.", "The data pack does not match this release."), detail);
        goto fail;
    case CTR_PAK_BAD_ROM:
        SetError(CTR_TEXT("The data pack comes from another ROM.", "The data pack comes from another ROM.", "Le pack provient d'une autre ROM.", "Il pacchetto proviene da un'altra ROM.", "O pacote de dados veio de outra ROM.", "Datenpaket stammt von einem anderen ROM.", "The data pack comes from another ROM."), SUPPORTED_ROM_DETAIL);
        goto fail;
    default:
        SetError(CTR_TEXT("The data pack is damaged.", "The data pack is damaged.", "Le pack de donnees est endommage.", "Il pacchetto di dati e' danneggiato.", "O pacote de dados esta corrompido.", "Das Datenpaket ist beschaedigt.", "The data pack is damaged."),
                 CTR_TEXT_FR_IT_PT_DE("It does not pass the integrity check.\n"
                             "Run the builder again.",
                             "Le controle d'integrite a echoue.\n"
                             "Relance le builder.",
                             "Errore nel controllo d'integrita'.\n"
                             "Riavvia il builder.",
                             "Ele nao passa na verificacao de\n"
                             "integridade. Rode o builder de novo.",
                             "Die Integritaetspruefung schlug fehl.\n"
                             "Starte den Builder erneut."));
        goto fail;
    }
    index = malloc((size_t)info.entryCount * CTR_PAK_ENTRY_BYTES);
    sEntries = malloc((size_t)info.entryCount * sizeof(*sEntries));
    if (index == NULL || sEntries == NULL
     || fseek(sPak, (long)info.indexOffset, SEEK_SET) != 0
     || fread(index, CTR_PAK_ENTRY_BYTES, info.entryCount, sPak) != info.entryCount)
    {
        SetError(CTR_TEXT("The data pack could not be read.", "The data pack could not be read.", "Lecture du pack de donnees impossible.", "Impossibile leggere il pacchetto di dati.", "Nao foi possivel ler o pacote de dados.", "Das Datenpaket konnte nicht gelesen werden.", "The data pack could not be read."),
                 CTR_TEXT("The SD card could not deliver its index.", "The SD card could not deliver its index.", "La carte SD n'a pas pu fournir l'index.", "Impossibile leggere l'indice\ndalla scheda SD.", "O cartao SD nao entregou o seu indice.", "Die SD-Karte lieferte kein Verzeichnis.", "The SD card could not deliver its index."));
        goto fail;
    }
    if (CtrPak_ParseIndex(index, &info, sEntries) != CTR_PAK_OK)
    {
        SetError(CTR_TEXT("The data pack is damaged.", "The data pack is damaged.", "Le pack de donnees est endommage.", "Il pacchetto di dati e' danneggiato.", "O pacote de dados esta corrompido.", "Das Datenpaket ist beschaedigt.", "The data pack is damaged."),
                 CTR_TEXT_FR_IT_PT_DE("Its index does not pass the integrity\n"
                             "check. Run the builder again.",
                             "L'index a echoue au controle\n"
                             "d'integrite. Relance le builder.",
                             "Errore nel controllo d'integrita'\n"
                             "dell'indice. Riavvia il builder.",
                             "O indice nao passa na verificacao de\n"
                             "integridade. Rode o builder de novo.",
                             "Die Integritaetspruefung des Index\n"
                             "schlug fehl. Starte den Builder erneut."));
        goto fail;
    }
    free(index);
    sEntryCount = info.entryCount;
    LightLock_Init(&sPakLock);
    CtrLog_Write(CTR_LOG_FS, "data: pack %lu entries, ABI %08lx",
                 (unsigned long)sEntryCount, (unsigned long)info.engineAbi);
    return true;

fail:
    free(index);
    free(sEntries);
    sEntries = NULL;
    fclose(sPak);
    sPak = NULL;
    return false;
}

static LightLock sAheadLock;
static bool sAheadReady;

bool CtrData_Init(void)
{
    int choice = CTR_DATA_BACKEND;

    /* Here, before any worker can ask for a file. */
    LightLock_Init(&sAheadLock);
    sAheadReady = true;

    ReadEngineAbi();
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/emerald3ds", 0777);
    if (choice == 0)
    {
        if (FileExists(CTR_DATA_LOOSE_MARKER))
            choice = 2;
        else if (FileExists(CTR_DATA_EMBEDDED_MARKER))
            choice = 1;
        else
            choice = 3;
    }
    switch (choice)
    {
    case 1:
        if (!FileExists(CTR_DATA_EMBEDDED_MARKER))
        {
            SetError(CTR_TEXT("This build has no embedded game data.", "This build has no embedded game data.", "Cette version n'integre pas de donnees.", "Questa versione non include dati di gioco.", "Esta versao nao traz dados embutidos.", "Diese Version enthaelt keine Spieldaten.", "This build has no embedded game data."),
                     CTR_TEXT("Install a data pack or loose data files.", "Install a data pack or loose data files.", "Installe un pack ou des fichiers separes.", "Installa un pacchetto o i file separati.", "Instale um pacote ou os dados soltos.", "Datenpaket oder lose Dateien installieren.", "Install a data pack or loose data files."));
            return false;
        }
        sBackend = CTR_DATA_ROMFS;
        break;
    case 2:
    {
        uint32_t looseAbi;

        /* Only reported: the ABI covers every data file, so it also changes
         * when one generator's output is copied over during development,
         * which is what the loose backend is for. After an engine change a
         * mismatch here explains the crash that follows. */
        if (!ReadLooseAbi(&looseAbi) || sEngineAbi == 0)
            CtrLog_Write(CTR_LOG_FS, "data: loose data ABI not checked (unknown)");
        else if (looseAbi != sEngineAbi)
            CtrLog_Write(CTR_LOG_ERROR, "data: loose data staged for ABI %08lX, engine is %08lX "
                         "(make devdata again if the engine changed)",
                         (unsigned long)looseAbi, (unsigned long)sEngineAbi);
        sBackend = CTR_DATA_LOOSE;
        break;
    }
    default:
        if (!OpenPak())
            return false;
        sBackend = CTR_DATA_PAK;
        break;
    }
    CtrLog_Write(CTR_LOG_FS, "data: %s backend, engine ABI %08lx",
                 CtrData_BackendName(), (unsigned long)sEngineAbi);
    return true;
}

const char *CtrData_ErrorTitle(void) { return sErrorTitle; }
const char *CtrData_ErrorDetail(void) { return sErrorDetail; }

const char *CtrData_BackendName(void)
{
    switch (sBackend)
    {
    case CTR_DATA_ROMFS: return "romfs";
    case CTR_DATA_LOOSE: return "loose";
    case CTR_DATA_PAK: return "pak";
    default: return "none";
    }
}

static ssize_t PakRead(void *cookie, char *buffer, size_t size)
{
    PakStream *stream = cookie;
    size_t left = stream->size - stream->pos;
    size_t got;

    if (size > left)
        size = left;
    if (size == 0)
        return 0;
    LightLock_Lock(&sPakLock);
    got = fseek(sPak, (long)(stream->base + stream->pos), SEEK_SET) == 0
        ? fread(buffer, 1, size, sPak) : 0;
    LightLock_Unlock(&sPakLock);
    stream->pos += (uint32_t)got;
    return got == 0 && size != 0 ? -1 : (ssize_t)got;
}

static int PakSeek(void *cookie, off_t *offset, int whence)
{
    PakStream *stream = cookie;
    int64_t target;

    switch (whence)
    {
    case SEEK_SET: target = *offset; break;
    case SEEK_CUR: target = (int64_t)stream->pos + *offset; break;
    case SEEK_END: target = (int64_t)stream->size + *offset; break;
    default: errno = EINVAL; return -1;
    }
    if (target < 0 || target > stream->size)
    {
        errno = EINVAL;
        return -1;
    }
    stream->pos = (uint32_t)target;
    *offset = target;
    return 0;
}

static int PakClose(void *cookie)
{
    free(cookie);
    return 0;
}

static bool PathValid(const char *path)
{
    return path != NULL && *path && *path != '/' && strstr(path, "..") == NULL && strchr(path, ':') == NULL;
}

FILE *CtrData_Open(const char *path)
{
    char full[512];

    if (!PathValid(path))
        return NULL;
    switch (sBackend)
    {
    case CTR_DATA_ROMFS:
        snprintf(full, sizeof(full), "romfs:/%s", path);
        return fopen(full, "rb");
    case CTR_DATA_LOOSE:
        snprintf(full, sizeof(full), CTR_DATA_LOOSE_DIR "%s", path);
        return fopen(full, "rb");
    case CTR_DATA_PAK:
    {
        const CtrPakEntry *entry = FindEntry(path);
        PakStream *stream;
        cookie_io_functions_t io = { PakRead, NULL, PakSeek, PakClose };
        FILE *file;

        if (entry == NULL || (stream = malloc(sizeof(*stream))) == NULL)
            return NULL;
        stream->base = entry->offset;
        stream->size = entry->rawSize;
        stream->pos = 0;
        file = fopencookie(stream, "rb", io);
        if (file == NULL)
            free(stream);
        return file;
    }
    default:
        return NULL;
    }
}

bool CtrData_Size(const char *path, uint32_t *outSize)
{
    if (sBackend == CTR_DATA_PAK)
    {
        const CtrPakEntry *entry = PathValid(path) ? FindEntry(path) : NULL;
        if (entry != NULL && outSize != NULL)
            *outSize = entry->rawSize;
        return entry != NULL;
    }
    FILE *file = CtrData_Open(path);
    long size;

    if (file == NULL)
        return false;
    size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    fclose(file);
    if (size < 0)
        return false;
    if (outSize != NULL)
        *outSize = (uint32_t)size;
    return true;
}

bool CtrData_Locate(const char *path, uint64_t *outOffset, uint32_t *outSize)
{
    const CtrPakEntry *entry;

    if (sBackend != CTR_DATA_PAK || !PathValid(path) || (entry = FindEntry(path)) == NULL)
        return false;
    *outOffset = entry->offset;
    *outSize = entry->rawSize;
    return true;
}

bool CtrData_ReadRange(uint64_t offset, void *dest, uint32_t size)
{
    bool ok;

    if (sBackend != CTR_DATA_PAK || sPak == NULL)
        return false;
    LightLock_Lock(&sPakLock);
    ok = fseek(sPak, (long)offset, SEEK_SET) == 0 && fread(dest, 1, size, sPak) == size;
    LightLock_Unlock(&sPakLock);
    return ok;
}

bool CtrData_Exists(const char *path)
{
    return CtrData_Size(path, NULL);
}

static void *LoadNow(const char *path, uint32_t *outSize)
{
    uint32_t size;
    uint8_t *buffer;
    FILE *file;

    if (!CtrData_Size(path, &size) || (file = CtrData_Open(path)) == NULL)
        return NULL;
    buffer = malloc((size_t)size + 1);
    if (buffer == NULL || fread(buffer, 1, size, file) != size)
    {
        fclose(file);
        free(buffer);
        CtrLog_Write(CTR_LOG_ERROR, "data: short read %s", path);
        return NULL;
    }
    fclose(file);
    buffer[size] = 0;
    if (sBackend == CTR_DATA_PAK)
    {
        const CtrPakEntry *entry = FindEntry(path);
        if (entry != NULL && CtrPak_Crc32(0, buffer, size) != entry->crc32)
        {
            CtrLog_Write(CTR_LOG_ERROR, "data: %s fails its CRC; the pack is damaged", path);
            free(buffer);
            return NULL;
        }
    }
    if (outSize != NULL)
        *outSize = size;
    return buffer;
}

/*
 * Files read ahead (CtrData_Prefetch) on a worker of their own: in a frame,
 * one read of the SD card is 10-30 ms on an Old 3DS, and a visible wild
 * Pokemon appearing every two seconds on its HGSS sprite (follower_mon.c)
 * dropped a frame each time. CtrData_Load hands back a copy of one read
 * ahead, waits for the one the worker is reading, or reads it on the spot as
 * before: the same bytes either way, only the moment of the read changes.
 * Small: a few map tables' worth of sprites, the least recently asked for
 * going first. sAheadLock guards the slots; no file is read under it.
 */
#define AHEAD_SLOTS 16
#define AHEAD_BYTES (128u * 1024u)

enum { AHEAD_FREE, AHEAD_QUEUED, AHEAD_READING, AHEAD_READY, AHEAD_MISSING };

typedef struct
{
    char path[48];
    uint8_t state;
    uint8_t *data;
    uint32_t size, used;
} AheadSlot;

static AheadSlot sAhead[AHEAD_SLOTS];
static bool sAheadWorker;
static uint32_t sAheadClock, sAheadBytes;

static AheadSlot *AheadFind(const char *path)
{
    for (unsigned i = 0; i < AHEAD_SLOTS; ++i)
        if (sAhead[i].state != AHEAD_FREE && strcmp(sAhead[i].path, path) == 0)
            return &sAhead[i];
    return NULL;
}

static void AheadDrop(AheadSlot *slot)
{
    sAheadBytes -= slot->size;
    free(slot->data);
    slot->data = NULL;
    slot->size = 0;
    slot->state = AHEAD_FREE;
}

/* A free slot, else the least recently asked for of those not being read. */
static AheadSlot *AheadVictim(const AheadSlot *keep)
{
    AheadSlot *victim = NULL;

    for (unsigned i = 0; i < AHEAD_SLOTS; ++i)
    {
        AheadSlot *slot = &sAhead[i];

        if (slot == keep || slot->state == AHEAD_QUEUED || slot->state == AHEAD_READING)
            continue;
        if (slot->state == AHEAD_FREE)
            return slot;
        if (victim == NULL || (int32_t)(slot->used - victim->used) < 0)
            victim = slot;
    }
    return victim;
}

/* What was read into its slot, the oldest others let go past the budget. */
static void AheadPublish(AheadSlot *slot, uint8_t *data, uint32_t size)
{
    slot->data = data;
    slot->size = data != NULL ? size : 0;
    slot->state = data != NULL ? AHEAD_READY : AHEAD_MISSING;
    sAheadBytes += slot->size;
    while (sAheadBytes > AHEAD_BYTES)
    {
        AheadSlot *victim = AheadVictim(slot);

        if (victim == NULL || victim->state == AHEAD_FREE)
            break;
        AheadDrop(victim);
    }
}

static void AheadWorker(void *arg)
{
    (void)arg;
    for (;;)
    {
        AheadSlot *slot = NULL;
        char path[sizeof(sAhead[0].path)];
        uint8_t *data;
        uint32_t size = 0;

        LightLock_Lock(&sAheadLock);
        for (unsigned i = 0; i < AHEAD_SLOTS && slot == NULL; ++i)
            if (sAhead[i].state == AHEAD_QUEUED)
                slot = &sAhead[i];
        if (slot == NULL)
        {
            sAheadWorker = false;
            LightLock_Unlock(&sAheadLock);
            return;
        }
        slot->state = AHEAD_READING;
        memcpy(path, slot->path, sizeof(path));
        LightLock_Unlock(&sAheadLock);

        data = LoadNow(path, &size);

        LightLock_Lock(&sAheadLock);
        AheadPublish(slot, data, size);
        LightLock_Unlock(&sAheadLock);
    }
}

void CtrData_Prefetch(const char *path)
{
    AheadSlot *slot;
    bool start = false;

    if (!sAheadReady || sBackend == CTR_DATA_NONE || !PathValid(path) || strlen(path) >= sizeof(sAhead[0].path))
        return;
    LightLock_Lock(&sAheadLock);
    slot = AheadFind(path);
    if (slot == NULL && (slot = AheadVictim(NULL)) != NULL)
    {
        if (slot->state != AHEAD_FREE)
            AheadDrop(slot);
        strcpy(slot->path, path);
        slot->state = AHEAD_QUEUED;
        start = !sAheadWorker;
        sAheadWorker = true;
    }
    if (slot != NULL)
        slot->used = ++sAheadClock;
    LightLock_Unlock(&sAheadLock);
    /* One step below the game, on its core: the reads are the FS service's
     * waits, and come while the game waits for the screen. */
    if (start && !CtrPlatform_StartThread(AheadWorker, NULL, 16 * 1024, -2))
    {
        LightLock_Lock(&sAheadLock);
        for (unsigned i = 0; i < AHEAD_SLOTS; ++i)
            if (sAhead[i].state == AHEAD_QUEUED)
                sAhead[i].state = AHEAD_FREE;
        sAheadWorker = false;
        LightLock_Unlock(&sAheadLock);
    }
}

void *CtrData_Load(const char *path, uint32_t *outSize)
{
    AheadSlot *slot;
    uint8_t *copy = NULL;
    uint32_t size = 0;

    if (!sAheadReady || path == NULL)
        return LoadNow(path, outSize);
    LightLock_Lock(&sAheadLock);
    slot = AheadFind(path);
    if (slot != NULL && slot->state == AHEAD_QUEUED)
    {
        /* Not begun: read here, and kept for the next one. */
        uint8_t *data;

        slot->state = AHEAD_READING;
        LightLock_Unlock(&sAheadLock);
        data = LoadNow(path, &size);
        LightLock_Lock(&sAheadLock);
        AheadPublish(slot, data, size);
    }
    while (slot != NULL && slot->state == AHEAD_READING)
    {
        /* The worker's: no sooner read again. */
        LightLock_Unlock(&sAheadLock);
        svcSleepThread(200 * 1000);
        LightLock_Lock(&sAheadLock);
    }
    if (slot != NULL && slot->state == AHEAD_READY && (copy = malloc((size_t)slot->size + 1)) != NULL)
    {
        size = slot->size;
        memcpy(copy, slot->data, size);
        copy[size] = 0;
        slot->used = ++sAheadClock;
    }
    LightLock_Unlock(&sAheadLock);
    if (copy == NULL)
        return LoadNow(path, outSize);
    if (outSize != NULL)
        *outSize = size;
    return copy;
}
