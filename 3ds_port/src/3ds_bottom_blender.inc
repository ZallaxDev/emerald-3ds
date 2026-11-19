/* Part of 3ds_bottom_ui.c: the Berry Blender's panel (included after 3ds_bottom_pad.inc). */

/*
 * While the bowl turns on the top screen: who is blending and how each is
 * doing (the berry they put in; how many times they hit it best, good and
 * missed), the revolutions now and the best so far, how far the Pokéblock has
 * got, and a button as large as the screen leaves, which the game reads as A.
 * The figures are the game's own (CtrBlenderView); the button is held as long
 * as it is touched, like the controller's (HIT_PAD_A). It is there only while
 * the game takes A (CTR_BLENDER_PLAY); with the berries going in and the
 * results (CTR_BLENDER_WATCH) the panel keeps the figures and nothing to tap.
 */
#define BLEND_PLAYER_Y 16
#define BLEND_PLAYER_H 46
#define BLEND_GAUGE_Y 70
#define BLEND_BUTTON_Y 118

static const u8 *BlendNumber(u16 value)
{
    static u8 text[8];

    ConvertIntToDecimalStringN(text, value, STR_CONV_MODE_LEFT_ALIGN, 4);
    return text;
}

/* A coloured pip and its count, at (x, y). */
static void BlendCount(int x, int y, u16 pip, u16 value)
{
    FillRect(x, y + 3, 6, 6, pip);
    StyledStr(&sSmall, BlendNumber(value), x + 9, y, STYLE_INK);
}

static void DrawBlender(const ViewState *s)
{
    const struct CtrBlenderView *v = &s->bl;
    int n = v->numPlayers, gap = 4, w = (304 - (n - 1) * gap) / n, oy, scale;
    u16 best = PackRgb(64, 176, 96), good = PackRgb(64, 136, 216), miss = PackRgb(208, 64, 56);

    NameBackdrop();
    for (int i = 0; i < n; ++i)
    {
        int x = 8 + i * (w + gap);

        oy = Plate(x, BLEND_PLAYER_Y, w, BLEND_PLAYER_H, i == v->own ? sEmerald : sCreamPlate,
                   PLATE_STATE_NORMAL);
        TextIn(GetItemName(v->items[i]), x, oy + 2, w, 12, &sSmall, 'c', i == v->own ? STYLE_WHITE : STYLE_INK, 2);
        if (w >= 60)
        {
            BlendCount(x + 4, oy + 20, best, v->scores[i][0]);
            BlendCount(x + 4 + (w - 8) / 3, oy + 20, good, v->scores[i][1]);
            BlendCount(x + 4 + 2 * (w - 8) / 3, oy + 20, miss, v->scores[i][2]);
        }
    }
    /* The turning: now, and the best so far. */
    oy = Plate(8, BLEND_GAUGE_Y, 304, 30, sCreamPlate, PLATE_STATE_NORMAL);
    if (v->state == CTR_BLENDER_PLAY)
    {
        scale = v->maxRpm > 100 ? v->maxRpm : 100;
        FillRect(14, oy + 18, 292, 6, PackRgb(208, 200, 188));
        FillRect(14, oy + 18, 292 * (v->rpm > scale ? scale : v->rpm) / scale, 6, PackRgb(88, 192, 128));
        FillRect(14 + 292 * (v->maxRpm > scale ? scale : v->maxRpm) / scale, oy + 16, 2, 10, PackRgb(216, 88, 80));
        StyledStr(&sNormal, BlendNumber(v->rpm), 14, oy + 2, STYLE_INK);
        StyledRight(&sSmall, BlendNumber(v->maxRpm), 304, oy + 4, STYLE_GRAY);
    }
    else
    {
        StyledStr(&sNormal, BlendNumber(v->maxRpm), 14, oy + 6, STYLE_INK);
    }
    /* How much of the Pokéblock there is. */
    FillRect(8, BLEND_GAUGE_Y + 36, 304, 6, PackRgb(208, 200, 188));
    /* In steps of about six pixels (CtrBlenderView). */
    if (v->progressMax)
        FillRect(8, BLEND_GAUGE_Y + 36, 304 * (v->progress > v->progressMax ? v->progressMax : v->progress) / v->progressMax, 6,
                 PackRgb(255, 184, 72));
    /* The button. */
    if (v->state == CTR_BLENDER_PLAY)
    {
        bool8 pressed = s->pressed == HIT_PAD_A;

        oy = Plate(8, BLEND_BUTTON_Y, 304, 92, pressed ? sBlue : sEmerald, pressed ? PLATE_STATE_PRESSED : PLATE_STATE_NORMAL);
        TextIn(Ascii(CTR_TEXT("BLEND", "MEZCLAR", "MIXER", "MESCOLA", "MISTURAR", "MIXEN", "まぜる")), 8, oy, 304, 92, &sNormal,
               'c', STYLE_WHITE, 4);
        AddHit(8, BLEND_BUTTON_Y, 304, 92, HIT_PAD_A);
    }
}
