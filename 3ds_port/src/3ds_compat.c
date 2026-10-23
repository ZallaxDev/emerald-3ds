/*
 * ARM11 implementation of the portable bridge hooks that are not resource
 * lookups: logging, debug channels, fonts, script/text pointers, save storage
 * and the audio debug channel. Every deferred behaviour is logged with the
 * plan's stub tag; none of them returns a fake success.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Birch channel out of line, for callers without its prototype. */
#define PORT_BIRCH_DEBUG_OUT_OF_LINE
#include "port_platform.h"
#include "port_log.h"
#include "3ds_data.h"
#include "3ds_assets.h"
#include "3ds_platform.h"
#include "3ds_audio.h"
#include "gba/flash_internal.h"
#include "characters.h"

/*
 * global.h redirects these four GBA registers to RAM under the bridge; this is
 * the storage the emulated registers live in, shared by every translation unit
 * that sees the bridge.
 */
volatile unsigned short gPortEmuIME;
volatile unsigned short gPortEmuIE;
volatile unsigned short gPortEmuIF;
volatile unsigned short gPortEmuDISPSTAT;

/* ── Logging bridge ─────────────────────────────────────────────────────── */

unsigned char g_PortLogActive = 1;

void Port_Log_Printf(const char *fmt, ...)
{
    char line[256];
    va_list args;
    size_t length;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    /* The game's messages end in '\n'; the 3DS logger adds its own. */
    length = strlen(line);
    while (length != 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
        line[--length] = '\0';
    CtrLog_Write(CTR_LOG_GAME, "%s", line);
}

void CtrAssets_Fatal(const char *detail)
{
    static char message[192];

    snprintf(message, sizeof(message), "[FS] %s", detail != NULL ? detail : "resource failure");
    CtrPlatform_Fatal(message);
}

/* ── Debug channels ─────────────────────────────────────────────────────── */

/* Empty, as the inline ones in port_platform.h: nothing reads them. */
void Port_SetBirchDebugEx(u16 tag, u16 a, u16 b, u16 c, u16 d)
{
    (void)tag; (void)a; (void)b; (void)c; (void)d;
}

void Port_SetBirchDebug(u16 tag, u16 a, u16 b, u16 c)
{
    (void)tag; (void)a; (void)b; (void)c;
}

/*
 * The mixer already marks its own frame through this channel: 0x0D00 is the
 * first statement of RunMixerFrame and 0x0D09 the last, after the CGB channels
 * have been generated. Timing those two is the whole mixer cost, and it costs
 * nothing to measure at a point the code was going to call anyway.
 */
void Port_SetAudioDebugEx(u16 tag, u16 a, u16 b, u16 c, u16 d)
{
    /* 0x0D02-03 the sequencer, 0x0D04-05 the PSG registers, 0x0D06-07 the
     * sampled voices, 0x0D08-09 the PSG channels' waves. */
    static uint64_t mixStart, partStart;
    static uint64_t parts[4];

    (void)a; (void)b; (void)c; (void)d;
    if ((tag & 0xFFF0) != 0x0D00)
        return;
    if (tag == 0x0D00)
    {
        mixStart = CtrPlatform_Ticks();
    }
    else if (tag == 0x0D02 || tag == 0x0D04 || tag == 0x0D06 || tag == 0x0D08)
    {
        partStart = CtrPlatform_Ticks();
    }
    else if ((tag == 0x0D03 || tag == 0x0D05 || tag == 0x0D07) && partStart != 0)
    {
        parts[(tag - 0x0D03) / 2] += CtrPlatform_Ticks() - partStart;
        partStart = 0;
    }
    else if (tag == 0x0D09 && mixStart != 0)
    {
        static float sum, peak;
        static unsigned frames;
        uint64_t now = CtrPlatform_Ticks();
        float ms = CtrPlatform_TickMs(now - mixStart);

        if (partStart != 0)
            parts[3] += now - partStart;
        partStart = 0;
        CtrAudio_Stats()->mixMs = ms;
        mixStart = 0;
        /* Wherever it runs (a worker core or the game thread), its average
         * and worst in the log, every 600 mixed frames. */
        sum += ms;
        if (ms > peak) peak = ms;
        if (++frames == 600)
        {
            CtrLog_Write(CTR_LOG_AUDIO, "mixer %.3f ms a frame, peak %.3f (sequencer %.3f, psg regs %.3f, "
                         "voices %.3f, psg waves %.3f)", sum / frames, peak,
                         CtrPlatform_TickMs(parts[0]) / frames, CtrPlatform_TickMs(parts[1]) / frames,
                         CtrPlatform_TickMs(parts[2]) / frames, CtrPlatform_TickMs(parts[3]) / frames);
            sum = peak = 0.0f;
            frames = 0;
            memset(parts, 0, sizeof(parts));
        }
    }
}

/* Safe point between main-loop callbacks: the only place payloads may retire. */
void Port_RecordMainLoopIteration(void)
{
    CtrAssets_Collect();
}

/* ── Latin fonts ────────────────────────────────────────────────────────── */

extern const u16 gFontSmallNarrowLatinGlyphs[];
extern const u16 gFontSmallLatinGlyphs[];
extern const u16 gFontNarrowLatinGlyphs[];
extern const u16 gFontShortLatinGlyphs[];
extern const u16 gFontNormalLatinGlyphs[];

/*
 * The stubs of the five latin fonts are packed almost back to back, so an
 * interior pointer derived from one of them aliases another family. Callers
 * must resolve the base first and apply glyph offsets to the loaded buffer.
 */
static struct
{
    const u16 *stubBase;
    const char *path;
    u8 *raw;
    u32 size;
} sLatinFonts[] = {
    { gFontSmallNarrowLatinGlyphs, "graphics/fonts/small_narrow.latfont", NULL, 0 },
    { gFontSmallLatinGlyphs,       "graphics/fonts/small.latfont",        NULL, 0 },
    { gFontNarrowLatinGlyphs,      "graphics/fonts/narrow.latfont",       NULL, 0 },
    { gFontShortLatinGlyphs,       "graphics/fonts/short.latfont",        NULL, 0 },
    { gFontNormalLatinGlyphs,      "graphics/fonts/normal.latfont",       NULL, 0 },
};

extern const u8 gFontSmallNarrowLatinGlyphWidths[];
extern const u8 gFontSmallLatinGlyphWidths[];
extern const u8 gFontNarrowLatinGlyphWidths[];
extern const u8 gFontShortLatinGlyphWidths[];
extern const u8 gFontNormalLatinGlyphWidths[];

static const u8 *const sLatinFontWidths[] = {
    gFontSmallNarrowLatinGlyphWidths, gFontSmallLatinGlyphWidths, gFontNarrowLatinGlyphWidths,
    gFontShortLatinGlyphWidths, gFontNormalLatinGlyphWidths,
};

/*
 * A glyph is 16x16 pixels of 2 bits in four 8x8 tiles (top left, top right,
 * bottom left, bottom right), a row of a tile in two bytes: pixels 0-3 in the
 * second, 4-7 in the first, the leftmost in the high bits (gbagfx font.c).
 * Values 1 and 2 are the ink and its shadow, 3 the background.
 */
#define GLYPH_BYTES 0x40

static u8 *GlyphByte(u8 *glyph, int x, int y)
{
    return glyph + ((y / 8) * 2 + x / 8) * 16 + (y % 8) * 2 + ((x % 8) < 4);
}

static u8 GlyphPixel(const u8 *glyph, int x, int y)
{
    return (*GlyphByte((u8 *)glyph, x, y) >> (6 - 2 * (x % 4))) & 3;
}

static void SetGlyphPixel(u8 *glyph, int x, int y, u8 value)
{
    u8 *byte = GlyphByte(glyph, x, y);
    int shift = 6 - 2 * (x % 4);

    *byte = (*byte & ~(3 << shift)) | (value << shift);
}

static bool GlyphRowInked(const u8 *glyph, int y)
{
    for (int x = 0; x < 16; x++)
    {
        u8 value = GlyphPixel(glyph, x, y);

        if (value == 1 || value == 2)
            return true;
    }
    return false;
}

/*
 * The fonts have no ã, õ, Ã or Õ (0x2F-0x32, patch 0057). Each is drawn from
 * the player's own font when it loads: the letter, with the tilde of ñ (or Ñ)
 * over it. The tilde is the rows that ñ inks and n leaves blank, centred on
 * the letter's width.
 */
static void ComposeTildeLetters(u8 *font, u32 size, const u8 *widths)
{
    static const struct { u8 slot, letter, tilde, plain; } sLetters[] = {
        { CHAR_A_TILDE, CHAR_A, CHAR_N_TILDE, CHAR_N },
        { CHAR_O_TILDE, CHAR_O, CHAR_N_TILDE, CHAR_N },
        { CHAR_a_TILDE, CHAR_a, CHAR_n_TILDE, CHAR_n },
        { CHAR_o_TILDE, CHAR_o, CHAR_n_TILDE, CHAR_n },
    };

    if (size < (u32)(CHAR_z + 1) * GLYPH_BYTES)
        return;
    for (unsigned i = 0; i < ARRAY_COUNT(sLetters); i++)
    {
        u8 *glyph = font + sLetters[i].slot * GLYPH_BYTES;
        const u8 *tilde = font + sLetters[i].tilde * GLYPH_BYTES;
        const u8 *plain = font + sLetters[i].plain * GLYPH_BYTES;
        int shift = ((int)widths[sLetters[i].letter] - (int)widths[sLetters[i].tilde]) / 2;

        memcpy(glyph, font + sLetters[i].letter * GLYPH_BYTES, GLYPH_BYTES);
        for (int y = 0; y < 16; y++)
        {
            if (GlyphRowInked(plain, y) || !GlyphRowInked(tilde, y))
                continue;
            for (int x = 0; x < 16; x++)
            {
                u8 value = GlyphPixel(tilde, x, y);

                if ((value == 1 || value == 2) && x + shift >= 0 && x + shift < 16)
                    SetGlyphPixel(glyph, x + shift, y, value);
            }
        }
    }
}

static bool LoadLatinFont(unsigned index)
{
    FILE *file;
    long size;

    if (sLatinFonts[index].raw != NULL)
        return true;
    file = CtrData_Open(sLatinFonts[index].path);
    if (file == NULL)
    {
        PORT_LOG("[ERROR] font missing: %s\n", sLatinFonts[index].path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0
     || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return false;
    }
    sLatinFonts[index].raw = malloc((size_t)size);
    if (sLatinFonts[index].raw == NULL
     || fread(sLatinFonts[index].raw, 1, (size_t)size, file) != (size_t)size)
    {
        free(sLatinFonts[index].raw);
        sLatinFonts[index].raw = NULL;
        fclose(file);
        PORT_LOG("[ERROR] font unreadable: %s\n", sLatinFonts[index].path);
        return false;
    }
    fclose(file);
    sLatinFonts[index].size = (u32)size;
    ComposeTildeLetters(sLatinFonts[index].raw, sLatinFonts[index].size, sLatinFontWidths[index]);
    return true;
}

const void *Port_ResolveFontPointer(const void *ptr)
{
    unsigned i;

    for (i = 0; i < ARRAY_COUNT(sLatinFonts); i++)
    {
        if (ptr != sLatinFonts[i].stubBase)
            continue;
        if (!LoadLatinFont(i))
            return ptr;
        return sLatinFonts[i].raw;
    }
    return ptr;
}

/*
 * A glyph of one of the latin fonts (0 small narrow, 1 small, 2 narrow,
 * 3 short, 4 normal), loaded from the pack with the tilde letters: 16x16
 * pixels of 2 bits as GlyphPixel reads them, 1 the ink and 2 its shadow.
 * For the port's own drawing of the Portuguese graphics (3ds_icons_ptbr.c).
 */
const unsigned char *Port_LatinGlyph(unsigned font, unsigned char code, int *width)
{
    if (font >= ARRAY_COUNT(sLatinFonts) || !LoadLatinFont(font)
     || (u32)(code + 1) * GLYPH_BYTES > sLatinFonts[font].size)
    {
        *width = 0;
        return NULL;
    }
    *width = sLatinFontWidths[font][code];
    return sLatinFonts[font].raw + code * GLYPH_BYTES;
}

void Port_PreloadLatinFonts(void)
{
    unsigned i;

    for (i = 0; i < ARRAY_COUNT(sLatinFonts); i++)
        LoadLatinFont(i);
}

/* ── Save storage ───────────────────────────────────────────────────────────
 *
 * The image is mirrored in FLASH_BASE so reads and verification stay cheap, and each write
 * puts *only its own range* into the file. Rewriting all 128 KiB per sector
 * would be fourteen times the traffic per save and would destroy the other
 * rotating slot on a half-finished write.
 *
 * The directory is created by CtrFs_Init, which runs before the game starts.
 */

#define CTR_SAVE_PATH CTR_DATA_DIR "emerald3ds.sav"
/* Where earlier builds kept the save; moved once on first start. */
#define CTR_OLD_SAVE_PATH "sdmc:/3ds/pokeemerald/pokeemerald.sav"
/* Bytes accepted after the 128 KiB flash: mGBA's clock footer is 16. */
#define SAVE_TRAILER_MAX 512

static bool sSaveAvailable;

static void MigrateOldSave(void)
{
    FILE *probe = fopen(CTR_SAVE_PATH, "rb");

    if (probe != NULL)
    {
        fclose(probe);
        return;
    }
    if (rename(CTR_OLD_SAVE_PATH, CTR_SAVE_PATH) == 0)
        CtrLog_Write(CTR_LOG_FS, "save moved from %s", CTR_OLD_SAVE_PATH);
}

void Port_SaveInit(void)
{
    FILE *file;
    long size, flashBytes;
    bool loaded;
    /* A measured scenario plays from the save it was recorded with. */
    const char *path = CtrScenario_SavePath() ? CtrScenario_SavePath() : CTR_SAVE_PATH;

    memset(FLASH_BASE, 0xFF, sizeof(FLASH_BASE));
    sSaveAvailable = false;

    if (!CtrScenario_SavePath())
        MigrateOldSave();
    file = fopen(path, "rb");
    if (file == NULL)
    {
        /* Created by the first real save, not while starting a new game. */
        sSaveAvailable = errno == ENOENT;
        CtrLog_Write(sSaveAvailable ? CTR_LOG_FS : CTR_LOG_ERROR,
                     "save %s: %s (errno=%d)",
                     sSaveAvailable ? "will be created" : "unavailable",
                     path, errno);
        return;
    }

    size = -1;
    if (fseek(file, 0, SEEK_END) == 0)
        size = ftell(file);
    /* Much longer than the flash plus a trailer is not an Emerald save (a
     * 256 KiB or 1 MiB image of another game): refused and never written. */
    if (size > (long)(sizeof(FLASH_BASE) + SAVE_TRAILER_MAX))
    {
        fclose(file);
        CtrLog_Write(CTR_LOG_ERROR, "save refused: %s is %ld bytes, not a 128 KiB flash image",
                     path, size);
        return;
    }
    /* A save from a cartridge dump or another emulator can carry more than
     * the flash: mGBA appends its 16 bytes of clock state. The flash is the
     * first 128 KiB, and whatever follows it is left in the file as it is. */
    flashBytes = size > (long)sizeof(FLASH_BASE) ? (long)sizeof(FLASH_BASE) : size;
    loaded = size >= 0
          && fseek(file, 0, SEEK_SET) == 0
          && fread(FLASH_BASE, 1, (size_t)flashBytes, file) == (size_t)flashBytes && !ferror(file);
    if (fclose(file) != 0)
        loaded = false;
    if (!loaded)
    {
        memset(FLASH_BASE, 0xFF, sizeof(FLASH_BASE));
        CtrLog_Write(CTR_LOG_ERROR, "save read failed: %s (size=%ld)", path, size);
        return;
    }

    /* A short file still holds whole sectors; Emerald recovers the older slot. */
    sSaveAvailable = true;
    CtrLog_Write(CTR_LOG_FS, "save loaded: %s (%ld bytes)", path, size);
    if (size > flashBytes)
        CtrLog_Write(CTR_LOG_FS, "save: %ld bytes after the flash image kept as they are",
                     size - flashBytes);
}

bool Port_SaveIsAvailable(void)
{
    return sSaveAvailable;
}

u16 Port_WriteFlash(u32 offset, const void *data, u32 size)
{
    FILE *file;
    long fileSize;
    bool ok;

    if (!sSaveAvailable || data == NULL || offset > sizeof(FLASH_BASE)
     || size > sizeof(FLASH_BASE) - offset)
        return 1;
    /* A scenario being played saves into memory only. */
    if (!CtrScenario_SaveWrites())
    {
        memmove(FLASH_BASE + offset, data, size);
        return 0;
    }

    file = fopen(CTR_SAVE_PATH, "r+b");
    if (file == NULL && errno == ENOENT)
        file = fopen(CTR_SAVE_PATH, "w+b");
    if (file == NULL)
        goto fail;

    /* Writing past the end of a short file would leave a hole, so the image is
     * grown from the mirror first and only then is the range written. */
    ok = fseek(file, 0, SEEK_END) == 0;
    fileSize = ok ? ftell(file) : -1;
    /* longer than the flash: the trailer after it (Port_SaveInit) stays */
    ok = fileSize >= 0 && (u32)fileSize <= sizeof(FLASH_BASE) + SAVE_TRAILER_MAX;
    if (ok && (u32)fileSize < sizeof(FLASH_BASE))
    {
        u32 remaining = sizeof(FLASH_BASE) - (u32)fileSize;

        ok = fwrite(FLASH_BASE + fileSize, 1, remaining, file) == remaining;
    }
    if (ok)
        ok = fseek(file, (long)offset, SEEK_SET) == 0 && fwrite(data, 1, size, file) == size;
    if (ok)
        ok = fflush(file) == 0;
    if (fclose(file) != 0)
        ok = false;
    if (!ok)
        goto fail;

    memmove(FLASH_BASE + offset, data, size);
    return 0;

fail:
    CtrLog_Write(CTR_LOG_ERROR, "save write failed: offset=%lu size=%lu errno=%d",
                 (unsigned long)offset, (unsigned long)size, errno);
    return 1;
}

/* ── Entry points the excluded GBA link/multiboot units would provide ───── */

u32 JoyBusRead(void) { return 0; }
void JoyBusWrite(void) {}
void CB2_InitBerryFixProgram(void) { CTR_STUB("BERRY_FIX", "GBA berry-fix program not applicable"); }
void CreateEReaderTask(void) { CTR_STUB("EREADER", "e-Reader link not supported"); }
void CB2_InitMysteryGift(void) { CTR_STUB("MYSTERY_GIFT", "link menu not supported"); }
void CB2_InitEReader(void) { CTR_STUB("EREADER", "e-Reader menu not supported"); }
void CB2_InitMysteryEventMenu(void) { CTR_STUB("MYSTERY_EVENT", "link menu not supported"); }

/*
 * Save-image verification. The GBA original relocated its comparison loop into
 * IWRAM because the cartridge bus needed it; here the save image is ordinary
 * RAM. VerifyFlashSector_Core is compiled out under PORTABLE (it returns 0
 * unconditionally), so this compares the bytes itself instead of reporting a
 * match that was never checked.
 */
static u32 CompareSaveBytes(const u8 *src, const u8 *target, u32 size)
{
    while (size-- != 0)
    {
        if (*target++ != *src++)
            return (u32)(target - 1);
    }
    return 0;
}

u32 VerifyFlashSectorNBytes(u16 sectorNum, u8 *src, u32 n)
{
    u32 base;

    if (gFlash == NULL || sectorNum >= gFlash->sector.count || n == 0
     || n > gFlash->sector.size)
        return 1;
    base = (u32)sectorNum << gFlash->sector.shift;
    if (base >= sizeof(FLASH_BASE) || n > sizeof(FLASH_BASE) - base)
        return 1;
    return CompareSaveBytes(src, FLASH_BASE + base, n);
}

u32 VerifyFlashSector(u16 sectorNum, u8 *src)
{
    return VerifyFlashSectorNBytes(sectorNum, src, gFlash != NULL ? gFlash->sector.size : 0);
}
