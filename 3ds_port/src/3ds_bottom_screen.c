/*
 * Native half of the bottom screen: the only part that touches libctru.
 *
 * CtrLog_Init leaves the bottom screen as a single RGB565 buffer that nothing
 * else draws to (the diagnostic console only takes it over on a fatal error),
 * and Citro3D never renders to it. The game-side canvas already has the
 * framebuffer's layout, so presenting is a copy of whole columns and a data
 * cache flush over the same range: the LCD reads the buffer from memory.
 */
#include <3ds.h>
#include <string.h>
#include "3ds_bottom.h"

static u16 *Framebuffer(void)
{
    if (gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES)
        return NULL;
    return (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
}

/* The last Blit's or BlitRect's copy and flush, in system ticks: on an Old
 * 3DS the copy is most of what presenting a redraw costs, and the slow-redraw
 * log splits the two (CtrBottom_LastBlitTicks). */
static u64 sCopyTicks, sFlushTicks;

void CtrBottom_LastBlitTicks(uint64_t *copy, uint64_t *flush)
{
    *copy = sCopyTicks;
    *flush = sFlushTicks;
}

void CtrBottom_Blit(const uint16_t *canvas, int x0, int x1)
{
    u16 *fb = Framebuffer();
    u64 t0, t1;

    sCopyTicks = sFlushTicks = 0;
    if (!fb || x0 >= x1) return;
    t0 = svcGetSystemTick();
    memcpy(fb + x0 * CTR_BOTTOM_HEIGHT, canvas + x0 * CTR_BOTTOM_HEIGHT,
           (size_t)(x1 - x0) * CTR_BOTTOM_HEIGHT * sizeof(u16));
    t1 = svcGetSystemTick();
    GSPGPU_FlushDataCache(fb + x0 * CTR_BOTTOM_HEIGHT, (u32)(x1 - x0) * CTR_BOTTOM_HEIGHT * sizeof(u16));
    sCopyTicks = t1 - t0;
    sFlushTicks = svcGetSystemTick() - t1;
}

void CtrBottom_BlitRect(const uint16_t *canvas, int x0, int y0, int x1, int y1)
{
    u16 *fb = Framebuffer();
    /* A column runs bottom-to-top, so rows [y0, y1) are one contiguous run. */
    int first = CTR_BOTTOM_HEIGHT - y1, count = y1 - y0;
    u64 t0, t1;

    sCopyTicks = sFlushTicks = 0;
    if (!fb || x0 >= x1 || count <= 0) return;
    t0 = svcGetSystemTick();
    /* Whole columns side by side are one run as well: a single copy. */
    if (count == CTR_BOTTOM_HEIGHT)
        memcpy(fb + x0 * CTR_BOTTOM_HEIGHT, canvas + x0 * CTR_BOTTOM_HEIGHT,
               (size_t)(x1 - x0) * CTR_BOTTOM_HEIGHT * sizeof(u16));
    else
        for (int x = x0; x < x1; ++x)
            memcpy(fb + x * CTR_BOTTOM_HEIGHT + first, canvas + x * CTR_BOTTOM_HEIGHT + first,
                   (size_t)count * sizeof(u16));
    t1 = svcGetSystemTick();
    /* Only the columns written: [x0, x1) is one range of the framebuffer. */
    GSPGPU_FlushDataCache(fb + x0 * CTR_BOTTOM_HEIGHT, (u32)(x1 - x0) * CTR_BOTTOM_HEIGHT * sizeof(u16));
    sCopyTicks = t1 - t0;
    sFlushTicks = svcGetSystemTick() - t1;
}

void CtrBottom_BlitBand(const uint16_t *band, int y0, int y1)
{
    u16 *fb = Framebuffer();
    int first = CTR_BOTTOM_HEIGHT - y1, count = y1 - y0;

    if (!fb || count <= 0) return;
    for (int x = 0; x < CTR_BOTTOM_WIDTH; ++x)
        memcpy(fb + x * CTR_BOTTOM_HEIGHT + first, band + x * count, (size_t)count * sizeof(u16));
    GSPGPU_FlushDataCache(fb, (u32)CTR_BOTTOM_WIDTH * CTR_BOTTOM_HEIGHT * sizeof(u16));
}

/*
 * The whole canvas at level/4 of its brightness (0 black, 4 as it is), for
 * the fades. RGB565 halves and quarters with a shift and a mask, two pixels
 * to a word; nothing is allocated and nothing is drawn.
 */
void CtrBottom_BlitDim(const uint16_t *canvas, int level)
{
    u16 *fb = Framebuffer();
    size_t pixels = (size_t)CTR_BOTTOM_WIDTH * CTR_BOTTOM_HEIGHT;

    if (!fb) return;
    if (level >= 4)
    {
        memcpy(fb, canvas, pixels * sizeof(u16));
    }
    else if (level <= 0)
    {
        memset(fb, 0, pixels * sizeof(u16));
    }
    else if ((((uintptr_t)canvas | (uintptr_t)fb) & 3) == 0)
    {
        const u32 *src = (const u32 *)canvas;
        u32 *dst = (u32 *)fb;

        for (size_t i = 0; i < pixels / 2; ++i)
        {
            u32 v = src[i], half = (v >> 1) & 0x7BEF7BEFu, quarter = (v >> 2) & 0x39E739E7u;

            dst[i] = level == 3 ? half + quarter : level == 2 ? half : quarter;
        }
    }
    else
    {
        for (size_t i = 0; i < pixels; ++i)
        {
            u16 v = canvas[i], half = (v >> 1) & 0x7BEF, quarter = (v >> 2) & 0x39E7;

            fb[i] = level == 3 ? half + quarter : level == 2 ? half : quarter;
        }
    }
    GSPGPU_FlushDataCache(fb, (u32)(pixels * sizeof(u16)));
}
