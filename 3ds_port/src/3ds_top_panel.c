/*
 * The bottom screen's panel on the top screen (3ds_top_panel.h): what it
 * hands over is kept in a plain buffer and written into the texture only
 * inside the frame, once C3D_FrameBegin has waited for the GPU to finish the
 * last one (as the Pokedex's device does, 3ds_dex_top.c).
 */
#include <3ds.h>
#include <citro2d.h>
#include <string.h>
#include "3ds_video.h"
#include "3ds_log.h"
#include "3ds_top_panel.h"

#define PW CTR_TOP_PANEL_W
#define PH CTR_TOP_PANEL_H
#define TEX_W 512
#define TEX_H 256
/* Frames the panel stays without a Keep: the bottom screen asks every frame. */
#define KEEP_FRAMES 3
#define TEX_RETRY_FRAMES 120

static uint16_t sPicture[PW * PH];
static bool sHas, sDirty, sTexAllocated, sTexFailLogged;
static unsigned sKeep, sTexRetry;
static C3D_Tex sTex;

void CtrTopPanel_Show(const uint16_t *canvas)
{
    memcpy(sPicture, canvas, sizeof(sPicture));
    sHas = sDirty = true;
    sKeep = KEEP_FRAMES;
}

void CtrTopPanel_Keep(void)
{
    if (sHas) sKeep = KEEP_FRAMES;
}

bool CtrTopPanel_Active(void) { return sHas && sKeep != 0; }

/* Once a frame, by the video side: the panel goes when no longer asked for. */
void CtrTopPanel_Tick(void)
{
    if (sKeep != 0 && --sKeep == 0) sHas = false;
}

static bool TextureReady(void)
{
    if (sTexAllocated) return true;
    if (sTexRetry != 0)
    {
        --sTexRetry;
        return false;
    }
    if (!C3D_TexInit(&sTex, TEX_W, TEX_H, GPU_RGB565))
    {
        sTexRetry = TEX_RETRY_FRAMES;
        if (!sTexFailLogged)
            CtrLog_Write(CTR_LOG_ERROR, "top panel: no linear memory for its texture (free=%lu)",
                         (unsigned long)linearSpaceFree());
        sTexFailLogged = true;
        return false;
    }
    sTexFailLogged = false;
    C3D_TexSetFilter(&sTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sTexAllocated = true;
    sDirty = true;
    return true;
}

void CtrTopPanel_Upload(void)
{
    if (!CtrTopPanel_Active() || !TextureReady() || !sDirty) return;
    uint16_t *texels = sTex.data;

    for (unsigned x = 0; x < PW; ++x)
    {
        const uint16_t *column = sPicture + x * PH;

        for (unsigned y = 0; y < PH; ++y)
            texels[CtrVideo_Texel(x, y, TEX_W)] = column[PH - 1 - y];
    }
    C3D_TexFlush(&sTex);
    sDirty = false;
}

/* Over the whole top screen, on the target the caller set up for 2D. */
void CtrTopPanel_Draw(void)
{
    if (!CtrTopPanel_Active() || !sTexAllocated || sDirty) return;
    const Tex3DS_SubTexture part = {PW, PH, 0.0f, 1.0f, (float)PW / TEX_W, 1.0f - (float)PH / TEX_H};

    C2D_DrawImageAt((C2D_Image){&sTex, &part}, 0.0f, 0.0f, 0.0f, NULL, 1.0f, 1.0f);
}

void CtrTopPanel_Idle(void)
{
    sTexRetry = 0;
    if (!sTexAllocated) return;
    C3D_TexDelete(&sTex);
    sTexAllocated = false;
}
