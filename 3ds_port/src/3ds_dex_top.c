/*
 * The Pokédex on the top screen, native side (3ds_dex_top.h): the picture
 * and backdrop textures and the device drawn around them.
 *
 * What the game side hands over is kept in plain buffers here and written
 * into the textures only inside the frame (CtrDexTop_Upload), once
 * C3D_FrameBegin has waited for the GPU to finish the last one: the GPU never
 * reads a texture half written. The textures are made when the Pokédex is
 * first drawn and given back as soon as it is not (CtrDexTop_Idle).
 */
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "3ds_video.h"
#include "3ds_log.h"
#include "3ds_dex_top.h"

#define DIM CTR_DEX_TOP_DIM
#define FRAMES CTR_DEX_TOP_FRAMES
#define BACKDROP_W CTR_DEX_TOP_BACKDROP_W
#define BACKDROP_H CTR_DEX_TOP_BACKDROP_H
/* The picture texture: the frames one under the other. */
#define PICTURE_TEX_H (FRAMES * DIM)

/* The screen window on the top screen, and the picture in it at 2x (every
 * GBA pixel an exact 2x2), its middle at (WIN_CX, PICTURE_CY). */
#define WIN_X 70
#define WIN_Y 28
#define WIN_W 260
#define WIN_H 180
#define WIN_CX (WIN_X + WIN_W / 2)
#define PICTURE_SCALE 2.0f
#define PICTURE_CY 130.0f
/* With the 3D slider up, the picture stands a little in front of the
 * device's screen and the terrain a little behind it, in screen pixels per
 * eye at the slider's top. */
#define PICTURE_DEPTH 1.5f
#define BACKDROP_DEPTH 2.0f
/* The backdrop's magnification in the window, and where the opponent's
 * mound is in it (the battle stands the opponent there): under the
 * Pokémon's feet. */
#define BACKDROP_SCALE 1.6f
#define BACKDROP_MOUND_X 176
#define BACKDROP_MOUND_Y 76
/* The animation: the second frame for a moment, every so often. */
#define ANIM_PERIOD 150
#define ANIM_HOLD 14

/* What the game side handed over. */
static bool sActive;
static uint16_t sNational;
static unsigned sFrames;
static uint16_t sPicture[FRAMES][DIM * DIM];
static char sName[CTR_DEX_TOP_NAME];
static uint16_t sBackdrop[BACKDROP_H][BACKDROP_W];
static unsigned sBackdropRows;
/* The sky: the backdrop's top-left pixel, for the window above it. */
static uint32_t sSky;

/* The textures, and whether they hold what was handed over. */
static C3D_Tex sPictureTex, sBackdropTex;
static bool sTexAllocated, sTexFailLogged;
/* Frames to wait before trying to make them again after linear memory ran
 * short: a moment's shortage does not take the device away for the session. */
#define TEX_RETRY_FRAMES 120
static unsigned sTexRetry;
static bool sPictureDirty, sBackdropDirty, sBackdropShown;
static uint32_t sTick;

void CtrDexTop_Show(uint16_t national, const uint16_t *picture, unsigned frames, const char *name)
{
    if (!sActive) sTick = 0;
    sActive = true;
    sNational = picture != NULL && frames > 0 ? national : 0;
    sFrames = sNational ? (frames > FRAMES ? FRAMES : frames) : 0;
    if (sFrames) memcpy(sPicture, picture, sFrames * sizeof(sPicture[0]));
    snprintf(sName, sizeof(sName), "%s", sNational && name ? name : "");
    sPictureDirty = true;
    /* The backdrop is kept: the game side sends a new one (BackdropBegin)
     * when this Pokémon's terrain is not the last one's. */
}

bool CtrDexTop_BackdropComplete(void)
{
    return sBackdropRows == BACKDROP_H;
}

void CtrDexTop_BackdropBegin(void)
{
    sBackdropRows = 0;
    sBackdropDirty = sBackdropShown = false;
}

void CtrDexTop_BackdropRow(unsigned y, const uint16_t *row)
{
    if (y >= BACKDROP_H || row == NULL) return;
    if (y == 0)
        sSky = C2D_Color32(((row[0] >> 11) & 31) * 255 / 31, ((row[0] >> 6) & 31) * 255 / 31,
                           ((row[0] >> 1) & 31) * 255 / 31, 255);
    memcpy(sBackdrop[y], row, sizeof(sBackdrop[y]));
    if (++sBackdropRows == BACKDROP_H) sBackdropDirty = true;
}

void CtrDexTop_Hide(void) { sActive = false; }
bool CtrDexTop_Active(void) { return sActive; }

static bool TexturesReady(void)
{
    if (sTexAllocated) return true;
    if (sTexRetry != 0)
    {
        --sTexRetry;
        return false;
    }
    if (!C3D_TexInit(&sPictureTex, DIM, PICTURE_TEX_H, GPU_RGBA5551))
    {
        sTexRetry = TEX_RETRY_FRAMES;
        if (!sTexFailLogged)
            CtrLog_Write(CTR_LOG_ERROR, "Pokedex top: no linear memory for its picture (free=%lu)",
                         (unsigned long)linearSpaceFree());
        sTexFailLogged = true;
        return false;
    }
    if (!C3D_TexInit(&sBackdropTex, BACKDROP_W, BACKDROP_H, GPU_RGBA5551))
    {
        C3D_TexDelete(&sPictureTex);
        sTexRetry = TEX_RETRY_FRAMES;
        if (!sTexFailLogged)
            CtrLog_Write(CTR_LOG_ERROR, "Pokedex top: no linear memory for its backdrop (free=%lu)",
                         (unsigned long)linearSpaceFree());
        sTexFailLogged = true;
        return false;
    }
    sTexFailLogged = false;
    C3D_TexSetFilter(&sPictureTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sPictureTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&sBackdropTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sBackdropTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sTexAllocated = true;
    /* Fresh memory: whatever was handed over goes in again. */
    sPictureDirty = true;
    sBackdropDirty = sBackdropRows == BACKDROP_H;
    sBackdropShown = false;
    return true;
}

void CtrDexTop_Upload(void)
{
    if (!sActive || !TexturesReady()) return;
    ++sTick;
    if (sPictureDirty)
    {
        uint16_t *texels = sPictureTex.data;

        /* Every texel written: a missing frame is transparent. */
        memset(texels, 0, DIM * PICTURE_TEX_H * sizeof(uint16_t));
        for (unsigned f = 0; f < sFrames; ++f)
            for (unsigned y = 0; y < DIM; ++y)
                for (unsigned x = 0; x < DIM; ++x)
                    texels[CtrVideo_Texel(x, f * DIM + y, DIM)] = sPicture[f][y * DIM + x];
        C3D_TexFlush(&sPictureTex);
        sPictureDirty = false;
    }
    if (sBackdropDirty)
    {
        uint16_t *texels = sBackdropTex.data;

        for (unsigned y = 0; y < BACKDROP_H; ++y)
            for (unsigned x = 0; x < BACKDROP_W; ++x)
                texels[CtrVideo_Texel(x, y, BACKDROP_W)] = sBackdrop[y][x];
        C3D_TexFlush(&sBackdropTex);
        sBackdropDirty = false;
        sBackdropShown = true;
    }
}

void CtrDexTop_Idle(void)
{
    /* Not shown: the next time it is, the textures are tried at once. */
    sTexRetry = 0;
    if (!sTexAllocated) return;
    C3D_TexDelete(&sPictureTex);
    C3D_TexDelete(&sBackdropTex);
    sTexAllocated = false;
}

/* ── Text: a 3x5 font, drawn as rectangles ──────────────────────────── */

#define GLYPH_PIXEL 2

static const char *const sGlyphs[] = {
    "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
    "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111",
    "010101111101101", "110101110101110", "111100100100111", "110101101101110", "111100111100111",
    "111100111100100", "111100101101111", "101101111101101", "111010010010111", "001001001101111",
    "101101110101101", "100100100100111", "101111111101101", "110101101101101", "111101101101111",
    "111101111100100", "111101101111001", "110101110101101", "111100111001111", "111010010010010",
    "101101101101111", "101101101101010", "101101111111101", "101101010101101", "101101010010010",
    "111001010100111",
    /* . - */
    "000000000000010", "000000111000000",
};

static int GlyphOf(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + c - 'A';
    if (c == '.') return 36;
    if (c == '-') return 37;
    return -1;
}

static void DrawText(float x, float y, const char *text, uint32_t colour)
{
    for (; *text; ++text, x += 4 * GLYPH_PIXEL)
    {
        int g = GlyphOf(*text);

        if (g < 0) continue;
        for (unsigned p = 0; p < 15; ++p)
            if (sGlyphs[g][p] == '1')
                C2D_DrawRectSolid(x + (p % 3) * GLYPH_PIXEL, y + (p / 3) * GLYPH_PIXEL, 0, GLYPH_PIXEL, GLYPH_PIXEL,
                                  colour);
    }
}

/* ── The device ─────────────────────────────────────────────────────── */

/*
 * The Pokédex as the device itself: its red shell across the top screen,
 * the hinge along the top, the blue lens and the lights at the left, the
 * buttons and the pad below and beside its screen window; in the window the
 * species' terrain a little behind the screen and its front picture a little
 * in front, its name on a strip along the window's bottom.
 */
void CtrDexTop_DrawEye(float sign, float slider)
{
    const uint32_t red = C2D_Color32(222, 48, 36, 255), redDark = C2D_Color32(150, 26, 20, 255);
    const uint32_t redLight = C2D_Color32(240, 92, 70, 255);
    const uint32_t grey = C2D_Color32(52, 52, 58, 255), greyLight = C2D_Color32(190, 190, 196, 255);
    const int x0 = WIN_X, y0 = WIN_Y, w = WIN_W, h = WIN_H;

    if (!sActive) return;
    if (slider < 0.0f) slider = 0.0f;
    if (slider > 1.0f) slider = 1.0f;
    /* The shell, lit from the top left; the hinge; the lens and the lights. */
    C2D_DrawRectangle(0, 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT, redLight, red, red, redDark);
    C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, 12, redDark);
    C2D_DrawRectSolid(0, 12, 0, CTR_GAME_WIDTH, 2, redLight);
    /* The lens: a disc of stacked bars (the ellipse primitive draws a box
     * in the emulator), lit at its top left. */
    for (int r = -17; r <= 17; ++r)
    {
        float half = sqrtf(17.0f * 17.0f - (float)(r * r));

        C2D_DrawRectSolid(28 - half, 58 + r, 0, 2 * half, 1, C2D_Color32(20, 40, 90, 255));
        if (r > -15 && r < 15)
        {
            float inner = sqrtf(14.0f * 14.0f - (float)(r * r));

            C2D_DrawRectSolid(28 - inner, 58 + r, 0, 2 * inner, 1,
                              r < -4 ? C2D_Color32(90, 160, 240, 255) : C2D_Color32(40, 110, 220, 255));
        }
    }
    C2D_DrawRectSolid(20, 48, 0, 8, 4, C2D_Color32(200, 225, 255, 255));
    C2D_DrawRectSolid(54, 44, 0, 6, 6, C2D_Color32(60, 220, 80, 255));
    C2D_DrawRectSolid(54, 54, 0, 6, 6, C2D_Color32(240, 200, 40, 255));
    C2D_DrawRectSolid(54, 64, 0, 6, 6, C2D_Color32(240, 60, 60, 255));
    /* The buttons below the window and the pad beside it. */
    C2D_DrawRectSolid(x0 + 40, y0 + h + 10, 0, 44, 10, grey);
    C2D_DrawRectSolid(x0 + 96, y0 + h + 10, 0, 44, 10, grey);
    C2D_DrawRectSolid(x0 + 42, y0 + h + 12, 0, 40, 3, greyLight);
    C2D_DrawRectSolid(x0 + 98, y0 + h + 12, 0, 40, 3, greyLight);
    C2D_DrawRectSolid(356, 186, 0, 14, 42, grey);
    C2D_DrawRectSolid(342, 200, 0, 42, 14, grey);
    C2D_DrawRectSolid(361, 191, 0, 4, 32, greyLight);
    C2D_DrawRectSolid(347, 205, 0, 32, 4, greyLight);
    /* The window: a dark bevel, a light rim, the screen. */
    C2D_DrawRectSolid(x0 - 6, y0 - 6, 0, w + 12, h + 12, grey);
    C2D_DrawRectSolid(x0 - 2, y0 - 2, 0, w + 4, h + 4, greyLight);
    C2D_DrawRectSolid(x0, y0, 0, w, h, C2D_Color32(150, 190, 120, 255));
    if (!sTexAllocated || sNational == 0) return;
    C2D_Flush();
    /* The screen's picture, clipped to the window (the top screen's
     * framebuffer is turned: its x runs down the screen). */
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (unsigned)(CTR_GAME_HEIGHT - y0 - h), (unsigned)x0,
                   (unsigned)(CTR_GAME_HEIGHT - y0), (unsigned)(x0 + w));
    if (sBackdropShown)
    {
        const Tex3DS_SubTexture whole = {BACKDROP_W, BACKDROP_H, 0.0f, 1.0f, 1.0f, 0.0f};
        /* The mound under the Pokémon's feet, a little above the window's
         * name strip; the sky fills what is left above the picture. The
         * left eye sees what is behind the screen shifted left. */
        const float bx = WIN_CX - BACKDROP_MOUND_X * BACKDROP_SCALE - sign * slider * BACKDROP_DEPTH;
        const float by = (PICTURE_CY + DIM / 2 * PICTURE_SCALE - 6) - BACKDROP_MOUND_Y * BACKDROP_SCALE;

        C2D_DrawRectSolid(x0, y0, 0, w, h, sSky);
        C2D_DrawImageAt((C2D_Image){&sBackdropTex, &whole}, bx, by, 0, NULL, BACKDROP_SCALE, BACKDROP_SCALE);
    }
    {
        const unsigned frame = sFrames > 1 && sTick % ANIM_PERIOD < ANIM_HOLD ? 1 : 0;
        const Tex3DS_SubTexture tile = {DIM, DIM, 0.0f, 1.0f - frame * DIM / (float)PICTURE_TEX_H,
                                        1.0f, 1.0f - (frame + 1) * DIM / (float)PICTURE_TEX_H};
        /* Whole pixels, so every texel stays an exact 2x2 in each eye. */
        const float x = WIN_CX - DIM / 2 * PICTURE_SCALE + roundf(sign * slider * PICTURE_DEPTH);
        const float y = PICTURE_CY - DIM / 2 * PICTURE_SCALE;

        C2D_DrawImageAt((C2D_Image){&sPictureTex, &tile}, x, y, 0, NULL, PICTURE_SCALE, PICTURE_SCALE);
    }
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* The name strip (the bottom screen has the number, Hoenn's or the
     * national one as the list is ordered). */
    C2D_DrawRectSolid(x0, y0 + h - 16, 0, w, 16, C2D_Color32(20, 30, 20, 170));
    DrawText(x0 + 6, y0 + h - 12, sName, C2D_Color32(230, 245, 220, 255));
}
