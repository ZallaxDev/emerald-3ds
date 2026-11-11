/*
 * Part of 3ds_video.c. What goes over the finished picture: the FPS, the HOME
 * notice, the poison veil, the speed badge, the swoop's light; and the held
 * top screen.
 */

#ifndef CTR_SHOW_FPS
#define CTR_SHOW_FPS 1
#endif
#if CTR_SHOW_FPS
/*
 * The FPS counter (SHOW_FPS=1), shown when the options turn it on
 * (CtrSettings_ShowFps): a 3x5 pixel font, each pixel a 1x1 solid
 * rectangle, over a translucent box in the top-left corner. Drawn last, over
 * whatever the frame composed, and at zero parallax in both eyes.
 */
#define FPS_PIXEL 1.0f
#define FPS_ADVANCE (4 * FPS_PIXEL)

static const char *const sFpsGlyphs[] =
{
    "111101101101111", "010110010010111", "111001111100111", "111001111001111",
    "101101111001001", "111100111001111", "111100111101111", "111001001001001",
    "111101111101111", "111101111001111",
    "111100111100100", /* F */
    "111101111100100", /* P */
    "001001010100100", /* / */
};
#endif

/* Game frames a second, hidden ones included (CtrPlatform_FrameCount): the
 * game's speed, beside the pictures a second the counter always showed.
 * With real-time pacing (3ds_platform.c) the two differ on a slow frame. */
static float sGameFps;
static uint64_t sGameFpsMark;

/*
 * The field's poison step (fldeff_misc.c, Task_FieldPoisonEffect) is a mosaic
 * that swells to 10 pixels and back, and the mosaic is not composed here: the
 * step had a sound and nothing to see. It is a violet veil over the view
 * instead, as deep as the mosaic is coarse - on whatever drew the field, the
 * 2D layers or the voxel world, which reads no register at all.
 */
static unsigned PoisonVeil(void)
{
    unsigned size = Reg(0x4c) & 15;

    return size > 10 ? 150 : size * 15;
}

/*
 * The HOME button refused in a local wireless session (3ds_platform.c): the
 * system draws nothing, so the game shows it was pressed - a house crossed
 * out, in the top screen's top right corner, for a second and a half.
 */
static uint32_t sNoHomeUntil;

void CtrVideo_ShowNoHome(void)
{
    sNoHomeUntil = sStats.frames + 90;
}

static void DrawNoHome(C3D_RenderTarget *target)
{
    const float x = CTR_GAME_WIDTH - 34, y = 6, w = 28;
    const u32 white = C2D_Color32(255, 255, 255, 255), red = C2D_Color32(230, 40, 40, 255);

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawCircleSolid(x + w / 2, y + w / 2, 0, w / 2, C2D_Color32(0, 0, 0, 170));
    /* The house: its roof and its walls, a door cut out of them. */
    C2D_DrawTriangle(x + 6, y + 14, white, x + 14, y + 6, white, x + 22, y + 14, white, 0);
    C2D_DrawRectSolid(x + 8, y + 14, 0, 12, 8, white);
    C2D_DrawRectSolid(x + 12, y + 17, 0, 4, 5, C2D_Color32(0, 0, 0, 255));
    /* Crossed out. */
    C2D_DrawLine(x + 6, y + 6, red, x + 22, y + 22, red, 3, 0);
    C2D_Flush();
}

/*
 * The swoop's light over a finished picture of the top screen, the same in
 * both eyes (on the glass): the flat flash and a radial bloom from the middle
 * as it dies away. Four triangles and a rectangle at most, nothing allocated.
 */
static void DrawSwoopLight(C3D_RenderTarget *target, const VoxelSwoopFrame *fx)
{
    const float w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT, cx = w * 0.5f, cy = h * 0.5f;
    uint32_t rgb = fx->colour & 0x00FFFFFFu;

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    if (fx->bloom > 0.005f)
    {
        /* A diamond, bright in the middle and gone at the screen's edges. */
        uint32_t core = rgb | ((uint32_t)(fx->bloom * 0.9f * 255.0f) << 24), edge = rgb;
        float rx = w * 0.75f, ry = h * 0.9f;

        C2D_DrawTriangle(cx, cy, core, cx - rx, cy, edge, cx, cy - ry, edge, 0);
        C2D_DrawTriangle(cx, cy, core, cx, cy - ry, edge, cx + rx, cy, edge, 0);
        C2D_DrawTriangle(cx, cy, core, cx + rx, cy, edge, cx, cy + ry, edge, 0);
        C2D_DrawTriangle(cx, cy, core, cx, cy + ry, edge, cx - rx, cy, edge, 0);
    }
    if (fx->flash > 0.005f)
        C2D_DrawRectSolid(0, 0, 0, w, h, rgb | ((uint32_t)(fminf(fx->flash, 1.0f) * 255.0f) << 24));
    C2D_Flush();
}

/*
 * The Fly cinematic's cloud over a finished picture of the top screen
 * (voxel_fly.c): a wash of white, the whole cover once closed (whatever is
 * under it - the warp's black - must not show through), and two sheets of
 * the world's cloud coverage (CtrVoxel_CloudTexture, tileable A8) at
 * different scales drifting across each other, so the cloud churns rather
 * than fades. The texture repeats under the picture through its own wrap
 * mode: the image's texture rectangle just runs past 1. In 3D the sheets
 * stand in front of the screen: the fine one nearest the eye, the broad one
 * behind it, shifted apart by the slider (to the right in the left eye, as
 * StereoParallax has what is nearer than the screen).
 */
/*
 * â”€â”€ The Fly sky scene â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * Inside the cloud, with the SKY FLIGHT option on (3ds_fly_sky.h), the
 * flight is seen instead of hidden: the player on their Pokemon bursts out
 * over the cloud deck into the open sky, soars across it with the wind
 * streaking past and the region's wild flyers passing by, slower than us -
 * a flock or two, now and then a legendary tearing past - and dives back
 * into the deck as the picture goes white for the descent. The game's warp
 * and the new map's build happen under it (voxel_fly.c, VOXEL_FLY_SKY).
 *
 * All of it is 2D over the closed cloud cover: a sky gradient, three sheets
 * of the world's cloud texture as the deck in parallax, the sprites from
 * one atlas texture packed at the flight's start (the Pokemon's and the
 * natives' overworld frames from the follower data, the player's riding
 * frame), the wind as streaks. The way of travel sets the side: east and
 * we cross left to right; south and the whole path slopes down. In 3D the
 * layers stand at their depths about the screen.
 */
#define SKY_W ((float)CTR_GAME_WIDTH)
#define SKY_H ((float)CTR_GAME_HEIGHT)
/* The fewest scene frames between two lightning flashes (DrawSkyScene). */
#define SKY_FLASH_GAP 120u
#define SKY_ATLAS 256u
#define SKY_NATIVES 4u
#define SKY_FLOCKS 8u
#define SKY_FLOCK_MAX 6u
#define SKY_LEGEND_ODDS 30u /* one flight in this many */
#define SKY_RAIN_DROPS 90u
#define SKY_PUFFS 9u      /* the last SKY_BIG_PUFFS of them the big thick ones */
#define SKY_BIG_PUFFS 3u
#define SKY_PUFF_DENSE 3u /* the puff texture's thick slot */
#define SKY_ASH_FLAKES 48u
#define SKY_MARKS 26u
#define SKY_FLYER_SCALE 2.0f
#define SKY_FADE_IN 14u
#define SKY_FADE_OUT 16u
/* Emerald's own species numbers of the legendaries that may pass. */
#define SKY_SPECIES_RAYQUAZA 406
#define SKY_SPECIES_LATIAS 407
#define SKY_SPECIES_LATIOS 408

typedef struct
{
    Tex3DS_SubTexture sub[2]; /* two wingbeat frames */
    uint16_t width;
    bool ok;
} SkyActor;

/* The sky's weather as drawn: colours and strengths, blended between the
 * map flown from and the map flown to halfway through the soar. */
typedef struct
{
    float zenith[3], mid[3], horizon[3]; /* the sky gradient, 0..1 */
    float deckTint[3];                   /* the cloud sheets' colour */
    float deck;        /* the deck's thickness: the sheets' alpha, 0..1 */
    float haze[3];     /* the air's colour over the far planes ... */
    float hazeAmount;  /* ... and how much of it */
    float rain, storm, ash, sand;
} SkyWeather;

typedef struct
{
    uint8_t actor;     /* index into native[], or SKY_NATIVES for the legend */
    uint8_t count;
    bool crossing;     /* flying the other way (drawn flipped) */
    uint16_t start;    /* scene frame it sets out */
    float y, speed, depth, phase;
} SkyFlock;

static struct
{
    bool loaded;
    C3D_Tex atlas;
    unsigned packX, packY, packRow;
    SkyActor flyer, rider, native[SKY_NATIVES], legend;
    unsigned natives;
    int8_t seatX, seatY;
    int sx;            /* +1: left to right (east), -1: right to left */
    float slope;       /* -1..1, the path's fall down the picture (south) */
    SkyFlock flock[SKY_FLOCKS];
    unsigned flocks;
    struct { float y, len, speed, phase; uint8_t alpha, thick; } mark[SKY_MARKS];
    /* Dollops of cloud whizzing past in front of everything, each a window
     * of the cloud texture faded to its edges (its holes show us through). */
    struct { float y, w, h, speed, phase; uint8_t alpha, slot; bool mirrored; } puff[SKY_PUFFS];
    uint32_t seed;
    SkyWeather from, to;
    float thickness;   /* the deck this flight, 0.35 thin .. 1 thick (clear skies vary) */
    unsigned flashFrame; /* the last lightning's scene frame, or ~0u */
    unsigned flashAsked; /* the scene frame the lightning was last decided on, or ~0u */
    /* The flyers' cries: the species behind the actors, which flocks have
     * called, the scene frame of the last call + 1, the frame last heard. */
    uint16_t nativeSpecies[SKY_NATIVES], legendSpecies;
    bool cried[SKY_FLOCKS];
    unsigned lastCry, soundFrame;
} sSky;

static void SkySetColour(float *c, unsigned r, unsigned g, unsigned b)
{
    c[0] = r / 255.0f;
    c[1] = g / 255.0f;
    c[2] = b / 255.0f;
}

static void SkyWeatherPreset(SkyWeather *w, unsigned class, float thickness)
{
    memset(w, 0, sizeof(*w));
    SkySetColour(w->deckTint, 240, 246, 254);
    SkySetColour(w->haze, 220, 230, 240);
    w->deck = thickness;
    switch (class)
    {
    default:
    case CTR_SKY_CLEAR:
        SkySetColour(w->zenith, 58, 118, 212);
        SkySetColour(w->mid, 132, 182, 236);
        SkySetColour(w->horizon, 196, 222, 246);
        break;
    case CTR_SKY_CLOUDY:
        SkySetColour(w->zenith, 96, 132, 186);
        SkySetColour(w->mid, 160, 184, 214);
        SkySetColour(w->horizon, 206, 214, 226);
        SkySetColour(w->deckTint, 228, 232, 240);
        w->deck = fmaxf(thickness, 0.85f);
        w->hazeAmount = 0.3f;
        break;
    case CTR_SKY_RAIN:
        SkySetColour(w->zenith, 66, 80, 108);
        SkySetColour(w->mid, 108, 122, 148);
        SkySetColour(w->horizon, 148, 158, 174);
        SkySetColour(w->deckTint, 186, 192, 204);
        SkySetColour(w->haze, 150, 158, 172);
        w->deck = 0.95f;
        w->hazeAmount = 0.45f;
        w->rain = 0.7f;
        break;
    case CTR_SKY_STORM:
        SkySetColour(w->zenith, 24, 26, 42);
        SkySetColour(w->mid, 44, 48, 68);
        SkySetColour(w->horizon, 70, 74, 92);
        SkySetColour(w->deckTint, 76, 80, 96);
        SkySetColour(w->haze, 64, 68, 84);
        w->deck = 1.0f;
        w->hazeAmount = 0.6f;
        w->rain = 1.0f;
        w->storm = 1.0f;
        break;
    case CTR_SKY_SAND:
        SkySetColour(w->zenith, 188, 150, 92);
        SkySetColour(w->mid, 218, 186, 126);
        SkySetColour(w->horizon, 236, 214, 168);
        SkySetColour(w->deckTint, 226, 196, 136);
        SkySetColour(w->haze, 218, 184, 120);
        w->deck = 0.95f;
        w->hazeAmount = 0.85f;
        w->sand = 1.0f;
        break;
    case CTR_SKY_ASH:
        SkySetColour(w->zenith, 104, 104, 114);
        SkySetColour(w->mid, 146, 146, 154);
        SkySetColour(w->horizon, 178, 178, 184);
        SkySetColour(w->deckTint, 196, 196, 200);
        SkySetColour(w->haze, 170, 170, 176);
        w->deck = 0.7f;
        w->hazeAmount = 0.5f;
        w->ash = 1.0f;
        break;
    case CTR_SKY_DROUGHT:
        SkySetColour(w->zenith, 236, 166, 104);
        SkySetColour(w->mid, 248, 206, 146);
        SkySetColour(w->horizon, 252, 232, 198);
        SkySetColour(w->deckTint, 252, 236, 210);
        SkySetColour(w->haze, 250, 220, 170);
        w->deck = 0.4f;
        w->hazeAmount = 0.25f;
        break;
    }
}

static float SkyLerp(float a, float b, float t) { return a + (b - a) * t; }

static void SkyWeatherAt(SkyWeather *w, float t)
{
    const float *a = (const float *)&sSky.from, *b = (const float *)&sSky.to;
    float *o = (float *)w;

    for (unsigned i = 0; i < sizeof(SkyWeather) / sizeof(float); ++i)
        o[i] = SkyLerp(a[i], b[i], t);
}

static uint32_t SkyColour(const float *c, unsigned alpha)
{
    return C2D_Color32((uint8_t)(c[0] * 255.0f + 0.5f), (uint8_t)(c[1] * 255.0f + 0.5f),
                       (uint8_t)(c[2] * 255.0f + 0.5f), (uint8_t)alpha);
}

static uint32_t SkyRandom(void)
{
    sSky.seed = sSky.seed * 1664525u + 1013904223u;
    return sSky.seed >> 8;
}

static float SkyUnit(void)
{
    return (float)(SkyRandom() & 0xFFFF) / 65535.0f;
}

/* One frame of a 4bpp sprite into the atlas; false when it has no room. */
static bool SkyPackFrame(const CtrSkySprite *sprite, unsigned frame, Tex3DS_SubTexture *sub)
{
    unsigned w = sprite->width;
    uint16_t *data = sSky.atlas.data;
    const uint8_t *tiles = sprite->tiles + (size_t)frame * sprite->frameBytes;

    if (frame >= sprite->frames || (w != 32 && w != 64))
        return false;
    if (sSky.packX + w > SKY_ATLAS)
    {
        sSky.packX = 0;
        sSky.packY += sSky.packRow;
        sSky.packRow = 0;
    }
    if (sSky.packY + w > SKY_ATLAS)
        return false;
    for (unsigned y = 0; y < w; ++y)
        for (unsigned x = 0; x < w; ++x)
        {
            const uint8_t *tile = tiles + ((y / 8) * (w / 8) + x / 8) * 32;
            unsigned index = (tile[(y & 7) * 4 + (x & 7) / 2] >> ((x & 1) * 4)) & 15;

            data[CtrVideo_Texel(sSky.packX + x, sSky.packY + y, SKY_ATLAS)] =
                index ? CtrVideo_RGBA5551(sprite->palette[index]) : 0;
        }
    sub->width = (uint16_t)w;
    sub->height = (uint16_t)w;
    sub->left = (float)sSky.packX / SKY_ATLAS;
    sub->right = (float)(sSky.packX + w) / SKY_ATLAS;
    sub->top = 1.0f - (float)sSky.packY / SKY_ATLAS;
    sub->bottom = 1.0f - (float)(sSky.packY + w) / SKY_ATLAS;
    sSky.packX += w;
    if (w > sSky.packRow) sSky.packRow = w;
    return true;
}

/* An actor's two frames facing the way of travel. */
static void SkyPackActor(SkyActor *actor, const CtrSkySprite *sprite, unsigned firstFrame)
{
    actor->width = sprite->width;
    actor->ok = SkyPackFrame(sprite, firstFrame, &actor->sub[0])
             && SkyPackFrame(sprite, firstFrame + 1, &actor->sub[1]);
}

/* The dollops' texture, below. */
#define SKY_PUFF_TEX_W 128u
#define SKY_PUFF_TEX_H 64u
#define SKY_PUFF_SLOTS 4u
static bool SkyPuffTexture(void);

/* The scene's atlas and state; the sprite files read for it are kept. */
static void SkySceneDrop(void)
{
    if (!sSky.loaded) return;

    C3D_TexDelete(&sSky.atlas);
    memset(&sSky, 0, sizeof(sSky));
}

static void SkySceneFree(void)
{
    /* The files read for the scene go whether or not it loaded. */
    CtrFlySky_Release();
    SkySceneDrop();
}

/* Readies the scene for a flight that way: false and the flight stays
 * inside the cloud (no sprite for the Pokemon, no memory). */
static bool SkySceneLoad(float dirX, float dirZ)
{
    CtrSkySprite sprite;
    uint16_t natives[SKY_NATIVES];
    unsigned facing;

    SkySceneDrop(); /* the files read as the Fly began are what it loads from */
    if (!CtrFlySky_FlyerSprite(&sprite, &sSky.seatX, &sSky.seatY))
        return false;
    if (!C3D_TexInit(&sSky.atlas, SKY_ATLAS, SKY_ATLAS, GPU_RGBA5551))
    {
        CtrLog_Write(CTR_LOG_ERROR, "FLY sky: no linear memory for the sprite atlas (free=%lu)",
                     (unsigned long)linearSpaceFree());
        memset(&sSky, 0, sizeof(sSky));
        return false;
    }
    sSky.loaded = true;
    C3D_TexSetFilter(&sSky.atlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sSky.atlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(sSky.atlas.data, 0, SKY_ATLAS * SKY_ATLAS * 2);
    sSky.seed = (uint32_t)sStats.frames * 2654435761u + 12345u;
    sSky.sx = dirX >= 0.0f ? 1 : -1;
    sSky.flashFrame = ~0u;
    sSky.flashAsked = ~0u;
    if (!SkyPuffTexture())
        CtrLog_Write(CTR_LOG_ERROR, "FLY sky: no linear memory for the puffs; none this flight");
    /* The weather at both ends, the deck thin or thick by chance. */
    {
        uint8_t from, to;

        CtrFlySky_Weather(&from, &to);
        sSky.thickness = 0.35f + 0.65f * SkyUnit();
        SkyWeatherPreset(&sSky.from, from, sSky.thickness);
        SkyWeatherPreset(&sSky.to, to, sSky.thickness);
    }
    sSky.slope = fmaxf(-1.0f, fminf(1.0f, dirZ));
    facing = sSky.sx > 0 ? CTR_SKY_FRAME_RIGHT : CTR_SKY_FRAME_LEFT;
    SkyPackActor(&sSky.flyer, &sprite, facing);
    if (!sSky.flyer.ok)
    {
        SkySceneFree();
        return false;
    }
    if (CtrFlySky_RiderSprite(&sprite))
    {
        sSky.rider.width = sprite.width;
        /* Frame 0 is the seated pose for a ride west, frame 1 its mirror
         * for a ride east: the flight to the right takes the east one (seen
         * facing left on a flight right, the other way round). */
        sSky.rider.ok = SkyPackFrame(&sprite, sSky.sx > 0 ? 1 : 0, &sSky.rider.sub[0]);
        sSky.rider.sub[1] = sSky.rider.sub[0];
    }
    sSky.natives = CtrFlySky_Natives(natives, SKY_NATIVES);
    for (unsigned i = 0; i < sSky.natives; ++i)
    {
        sSky.nativeSpecies[i] = natives[i];
        if (CtrFlySky_MonSprite(natives[i], 0, false, &sprite))
            SkyPackActor(&sSky.native[i], &sprite, facing);
    }
    memset(sSky.cried, 0, sizeof(sSky.cried));
    sSky.lastCry = 0;
    sSky.soundFrame = ~0u;
    /* Rarely a legendary tears past. */
    if (SkyRandom() % SKY_LEGEND_ODDS == 0)
    {
        static const uint16_t legends[3] = {SKY_SPECIES_LATIOS, SKY_SPECIES_LATIAS, SKY_SPECIES_RAYQUAZA};

        sSky.legendSpecies = legends[SkyRandom() % 3];
        if (CtrFlySky_MonSprite(sSky.legendSpecies, 0, false, &sprite))
            SkyPackActor(&sSky.legend, &sprite, facing);
    }
    C3D_TexFlush(&sSky.atlas);
    /* The flocks: set out over the soar, each from the far edge, drifting
     * back past us (we are the faster), some crossing the other way. Some
     * flights are quiet - a lone bird - and a storm keeps most grounded. */
    {
        unsigned count = (SkyRandom() & 3) == 0 ? 1 + SkyRandom() % 2 : 2 + SkyRandom() % 4, usable = 0;
        bool stormy = sSky.from.storm > 0.5f || sSky.to.storm > 0.5f;

        if (stormy && count > 2) count = 1 + SkyRandom() % 2;

        for (unsigned i = 0; i < sSky.natives; ++i) usable += sSky.native[i].ok;
        sSky.flocks = 0;
        if (usable != 0)
            for (unsigned i = 0; i < count && sSky.flocks < SKY_FLOCKS - 1; ++i)
            {
                SkyFlock *f = &sSky.flock[sSky.flocks];
                unsigned pick = SkyRandom() % usable, actor = 0;

                for (unsigned j = 0; j < sSky.natives; ++j)
                    if (sSky.native[j].ok && pick-- == 0) { actor = j; break; }
                f->actor = (uint8_t)actor;
                {
                    unsigned roll = SkyRandom() % 10;

                    f->count = (uint8_t)(roll < 4 ? 1 : roll < 7 ? 2 + SkyRandom() % 2 : 3 + SkyRandom() % (SKY_FLOCK_MAX - 2));
                    /* The big ones (64-pixel sprites) fly alone, or in a pair. */
                    if (sSky.native[actor].width == 64)
                        f->count = (uint8_t)(1 + (SkyRandom() % 3 == 0));
                }
                f->crossing = (SkyRandom() & 3) == 0;
                f->depth = SkyUnit();
                f->start = (uint16_t)(VOXEL_FLY_SKY_ENTER / 2
                                      + (unsigned)((float)i / (float)count * (VOXEL_FLY_SKY_SOAR - 30u))
                                      + SkyRandom() % 20);
                f->y = SKY_H * (0.12f + 0.55f * SkyUnit());
                f->speed = -(1.4f + 2.5f * f->depth + 1.1f * SkyUnit()) * (f->crossing ? 2.2f : 1.0f);
                f->phase = SkyUnit() * 100.0f;
                ++sSky.flocks;
            }
        if (sSky.legend.ok)
        {
            SkyFlock *f = &sSky.flock[sSky.flocks++];

            f->actor = SKY_NATIVES;
            f->count = 1;
            f->crossing = false;
            f->depth = 0.85f;
            f->start = (uint16_t)(VOXEL_FLY_SKY_ENTER + VOXEL_FLY_SKY_SOAR / 3);
            f->y = SKY_H * 0.26f;
            f->speed = 8.5f; /* overtaking us from behind */
            f->phase = 0.0f;
        }
    }
    for (unsigned i = 0; i < SKY_PUFFS; ++i)
    {
        bool big = i >= SKY_PUFFS - SKY_BIG_PUFFS;

        if (big)
        {
            /* The big thick ones: a moment of near-white with the two of
             * us only shapes in it. Spread along the scene so they pass
             * one at a time. */
            sSky.puff[i].w = 360.0f + SkyUnit() * 280.0f;
            sSky.puff[i].h = sSky.puff[i].w * (0.40f + SkyUnit() * 0.12f);
            sSky.puff[i].y = SKY_H * (0.25f + 0.5f * SkyUnit());
            sSky.puff[i].speed = 11.0f + SkyUnit() * 6.0f;
            sSky.puff[i].phase = (float)(i - (SKY_PUFFS - SKY_BIG_PUFFS)) * 1400.0f / SKY_BIG_PUFFS + SkyUnit() * 300.0f;
            sSky.puff[i].slot = SKY_PUFF_DENSE;
            sSky.puff[i].alpha = (uint8_t)(232 + SkyRandom() % 20);
        }
        else
        {
            sSky.puff[i].w = 110.0f + SkyUnit() * 120.0f;
            sSky.puff[i].h = sSky.puff[i].w * (0.42f + SkyUnit() * 0.2f);
            sSky.puff[i].y = SKY_H * (0.08f + 0.78f * SkyUnit());
            sSky.puff[i].speed = 7.0f + SkyUnit() * 8.0f;
            sSky.puff[i].phase = SkyUnit() * (SKY_W + 600.0f);
            sSky.puff[i].slot = (uint8_t)(SkyRandom() % SKY_PUFF_DENSE);
            sSky.puff[i].alpha = (uint8_t)(190 + SkyRandom() % 66);
        }
        sSky.puff[i].mirrored = (SkyRandom() & 1) != 0;
    }
    for (unsigned i = 0; i < SKY_MARKS; ++i)
    {
        sSky.mark[i].y = SkyUnit() * SKY_H;
        sSky.mark[i].len = 30.0f + SkyUnit() * 90.0f;
        sSky.mark[i].speed = 12.0f + SkyUnit() * 12.0f;

        sSky.mark[i].phase = SkyUnit() * (SKY_W + 120.0f);
        sSky.mark[i].alpha = (uint8_t)(70 + SkyRandom() % 110);
        sSky.mark[i].thick = (SkyRandom() & 3) == 0 ? 2 : 1;
    }
    CtrLog_Write(CTR_LOG_VIDEO, "FLY sky: scene ready, %s, %u natives, %u flocks%s, rider %s, deck %.2f",
                 sSky.sx > 0 ? "left to right" : "right to left", sSky.natives, sSky.flocks,
                 sSky.legend.ok ? " and a legendary" : "", sSky.rider.ok ? "on" : "missing",
                 (double)sSky.thickness);

    return true;
}

/* A travel-space x (forward positive, from the left edge) to the screen. */
static float SkyScreenX(float x)
{
    return sSky.sx > 0 ? x : SKY_W - x;
}

static float SkySmooth(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Our Pokemon's place along the scene: in fast, easing to a soar past the
 * middle, then away fast; `speed` 0..1 how fast it is going. */
static void SkyFlyerPath(unsigned n, float *x, float *y, float *tilt, float *speed)
{
    const float E = VOXEL_FLY_SKY_ENTER, S = VOXEL_FLY_SKY_SOAR, X = VOXEL_FLY_SKY_EXIT;
    float p = (float)n / (float)VOXEL_FLY_SKY_FRAMES;
    float bob = sinf((float)n * (6.2831853f / 70.0f));
    float tx, ty = SKY_H * 0.46f + sSky.slope * SKY_H * 0.14f * (2.0f * p - 1.0f), dive = 0.0f, lift = 0.0f;

    if ((float)n < E)
    {
        float u = (float)n / E, ease = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);

        tx = -0.3f * SKY_W + (0.44f * SKY_W + 0.3f * SKY_W) * ease;
        ty -= (1.0f - u) * (1.0f - u) * 24.0f;
        lift = 1.0f - u;
        *speed = 1.0f - 0.65f * ease;
    }
    else if ((float)n < E + S)
    {
        float t = ((float)n - E) / S;

        tx = 0.44f * SKY_W + 0.10f * SKY_W * t;
        *speed = 0.35f;
    }
    else
    {
        float u = ((float)n - E - S) / X, ease = u * u * u;

        tx = 0.54f * SKY_W + (1.45f * SKY_W - 0.54f * SKY_W) * ease;
        ty += u * u * 30.0f;
        dive = u;
        *speed = 0.35f + 0.65f * SkySmooth(u * 1.5f);
    }
    *x = SkyScreenX(tx);
    *y = ty + 8.0f * bob;
    *tilt = (float)sSky.sx * (0.09f * cosf((float)n * (6.2831853f / 70.0f)) + 0.28f * dive - 0.12f * lift);
}

static void SkyDrawActor(const SkyActor *actor, unsigned frame, float cx, float cy, float angle,
                         float scale, bool flipped, uint8_t alpha)
{
    C2D_Image image = {&sSky.atlas, &actor->sub[frame & 1]};
    C2D_ImageTint tint;

    C2D_PlainImageTint(&tint, C2D_Color32(255, 255, 255, alpha), 0.0f);
    C2D_DrawImageAtRotated(image, cx, cy, 0.0f, angle, &tint, flipped ? -scale : scale, scale);
}

/* A sheet of the cloud texture as a band of the deck, scrolled back along
 * the way at its own pace (the nearer the faster), shifted for the eye,
 * its edges faded (`fadeTop`, `fadeBottom`: the colour's alpha there, as a
 * share of it) so no band shows a line. */
static void SkyDrawDeck(C3D_Tex *tex, unsigned n, float top, float bottom, float repeats, float speed,
                        uint32_t colour, float shift, float fadeTop, float fadeBottom)
{
    float h = bottom - top;
    float ru = repeats, rv = repeats * h / SKY_W;
    float u0 = (float)fmod((speed * (float)n * (float)sSky.sx - shift) * repeats / SKY_W, 1.0);
    Tex3DS_SubTexture sub = {(uint16_t)SKY_W, (uint16_t)h, u0, 0.73f, u0 + ru, 0.73f - rv};
    C2D_Image image = {tex, &sub};
    C2D_ImageTint tint;
    unsigned alpha = colour >> 24;
    uint32_t rgb = colour & 0x00FFFFFFu;
    uint32_t topColour = rgb | ((uint32_t)(alpha * fadeTop + 0.5f) << 24);
    uint32_t bottomColour = rgb | ((uint32_t)(alpha * fadeBottom + 0.5f) << 24);

    C2D_SetImageTint(&tint, C2D_TopLeft, topColour, 1.0f);
    C2D_SetImageTint(&tint, C2D_TopRight, topColour, 1.0f);
    C2D_SetImageTint(&tint, C2D_BotLeft, bottomColour, 1.0f);
    C2D_SetImageTint(&tint, C2D_BotRight, bottomColour, 1.0f);
    C2D_DrawImageAt(image, 0.0f, top, 0.0f, &tint, 1.0f, 1.0f);
}

/*
 * The dollops' texture: four puffs of cloud, each a value noise (three
 * octaves on an 8-pixel lattice, wrapping) under an elliptical fall-off,
 * thin where the noise is low so the puff has holes, nothing at its edge
 * so it has no edge. Made once, A8, 128x64 a puff; a dollop is one image
 * of it, scaled, perhaps mirrored, moved along x.
 */
#define SKY_PUFF_TEX_W 128u
#define SKY_PUFF_TEX_H 64u
#define SKY_PUFF_SLOTS 4u
static C3D_Tex sSkyPuffTex;

static uint32_t SkyHash(uint32_t a)
{
    a ^= a >> 16; a *= 0x7feb352du; a ^= a >> 15; a *= 0x846ca68bu; a ^= a >> 16;
    return a;
}

/* Value noise: a wrapping lattice of `cells` across and `cells`/2 down,
 * its values made once into `table`, then read bilinearly. */
#define SKY_LATTICE_MAX (32u * 16u)
static void SkyLattice(uint32_t seed, unsigned cells, float *table)
{
    unsigned rows = cells / 2 ? cells / 2 : 1;

    for (unsigned cy = 0; cy < rows; ++cy)
        for (unsigned cx = 0; cx < cells; ++cx)
            table[cy * cells + cx] = (float)(SkyHash(seed ^ (cx * 73856093u) ^ (cy * 19349663u)) & 0xFFFF) / 65535.0f;
}

static float SkyLatticeNoise(const float *table, unsigned cells, float x, float y)
{
    unsigned rows = cells / 2 ? cells / 2 : 1;
    float fx = x * (float)cells, fy = y * (float)rows;
    unsigned ix = (unsigned)fx, iy = (unsigned)fy;
    float tx = fx - (float)ix, ty = fy - (float)iy;
    float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
    unsigned ix1 = (ix + 1) % cells, iy1 = (iy + 1) % rows;
    float top = table[iy * cells + ix] + (table[iy * cells + ix1] - table[iy * cells + ix]) * sx;
    float bottom = table[iy1 * cells + ix] + (table[iy1 * cells + ix1] - table[iy1 * cells + ix]) * sx;

    return top + (bottom - top) * sy;
}

/* Made at start-up (CtrVideo_Init), a few milliseconds; the first flight
 * made it on the spot once, and dropped a frame for it. */
static bool SkyPuffTexture(void)
{
    static float coarse[SKY_LATTICE_MAX], medium[SKY_LATTICE_MAX], fine[SKY_LATTICE_MAX];
    uint8_t *data;

    if (sSkyPuffTex.data != NULL) return true;
    if (!C3D_TexInit(&sSkyPuffTex, SKY_PUFF_TEX_W, SKY_PUFF_TEX_H * SKY_PUFF_SLOTS, GPU_A8))
    {
        memset(&sSkyPuffTex, 0, sizeof(sSkyPuffTex));
        return false;
    }
    C3D_TexSetFilter(&sSkyPuffTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sSkyPuffTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    data = sSkyPuffTex.data;
    for (unsigned slot = 0; slot < SKY_PUFF_SLOTS; ++slot)
    {
        uint32_t seed = 0x51ED270Bu + slot * 0x9E3779B9u;
        bool dense = slot == SKY_PUFF_DENSE;

        SkyLattice(seed, 8, coarse);
        SkyLattice(seed + 1, 16, medium);
        SkyLattice(seed + 2, 32, fine);
        for (unsigned y = 0; y < SKY_PUFF_TEX_H; ++y)
        {
            float v = (y + 0.5f) / SKY_PUFF_TEX_H, ey = (v - 0.5f) * 2.0f;

            for (unsigned x = 0; x < SKY_PUFF_TEX_W; ++x)
            {
                float u = (x + 0.5f) / SKY_PUFF_TEX_W, ex = (u - 0.5f) * 2.0f;
                float n = 0.5f * SkyLatticeNoise(coarse, 8, u, v) + 0.3f * SkyLatticeNoise(medium, 16, u, v)
                        + 0.2f * SkyLatticeNoise(fine, 32, u, v);
                /* The rim is the noise's: the puff ends where the noise
                 * runs out at a radius that wanders with it, so no ellipse
                 * shows. The last slot is the thick one: a lower floor, few
                 * holes, a wider body. */
                float d = ex * ex + ey * ey;
                float rim = 1.0f - d / (dense ? 0.55f + 0.55f * n : 0.40f + 0.65f * n);
                float body = (n - (dense ? 0.12f : 0.38f)) / 0.32f, alpha;

                rim = rim < 0.0f ? 0.0f : rim > 1.0f ? 1.0f : rim * rim * (3.0f - 2.0f * rim);
                body = body < 0.0f ? 0.0f : body > 1.0f ? 1.0f : body * body * (3.0f - 2.0f * body);
                alpha = body * rim;

                data[CtrVideo_Texel(x, slot * SKY_PUFF_TEX_H + y, SKY_PUFF_TEX_W)] = (uint8_t)(alpha * 255.0f + 0.5f);
            }
        }
    }
    C3D_TexFlush(&sSkyPuffTex);
    return true;
}

/* A dollop: one puff of the texture, `w` by `h` about (cx, cy). */
static void SkyDrawPuff(float cx, float cy, float w, float h, unsigned slot, bool mirrored, uint32_t colour)
{
    float v0 = 1.0f - (float)slot / SKY_PUFF_SLOTS, v1 = 1.0f - (float)(slot + 1) / SKY_PUFF_SLOTS;
    Tex3DS_SubTexture sub = {SKY_PUFF_TEX_W, SKY_PUFF_TEX_H, mirrored ? 1.0f : 0.0f, v0, mirrored ? 0.0f : 1.0f, v1};
    C2D_Image image = {&sSkyPuffTex, &sub};
    C2D_ImageTint tint;

    C2D_PlainImageTint(&tint, colour, 1.0f);
    C2D_DrawImageAt(image, cx - w * 0.5f, cy - h * 0.5f, 0.0f, &tint, w / SKY_PUFF_TEX_W, h / SKY_PUFF_TEX_H);
}

/* The flocks of one tier of depth: 0 far behind the middle deck, 1 before
 * us, 2 nearer than us (they cross in front). */
static unsigned SkyFlockTier(const SkyFlock *f)
{
    return f->depth < 0.5f ? 0u : f->depth < 0.8f ? 1u : 2u;
}

static void SkyDrawFlocks(unsigned n, float par, unsigned tier)
{
    for (unsigned i = 0; i < sSky.flocks; ++i)
    {
        const SkyFlock *f = &sSky.flock[i];
        const SkyActor *actor = f->actor < SKY_NATIVES ? &sSky.native[f->actor] : &sSky.legend;
        float scale = 1.0f + 0.9f * f->depth + (tier == 2 ? 0.5f : 0.0f), dir = f->speed < 0.0f ? -1.0f : 1.0f;
        float shift = (f->depth - 0.55f) * 8.0f * par;
        float x0 = f->speed < 0.0f ? 1.15f * SKY_W : -0.3f * SKY_W;
        float headX;

        if (SkyFlockTier(f) != tier || !actor->ok || n < f->start)
            continue;
        headX = x0 + f->speed * (float)(n - f->start);
        for (unsigned m = 0; m < f->count; ++m)
        {
            float mx = headX - dir * (float)m * (14.0f + 10.0f * f->depth) * (actor->width / 32.0f);
            float my = f->y + (float)((m + 1) / 2) * 11.0f * ((m & 1) ? -1.0f : 1.0f) * scale
                     + 5.0f * sinf(((float)n + f->phase) * 0.09f + (float)m);
            float sxp = SkyScreenX(mx) + shift;
            unsigned wing;

            if (sxp < -80.0f || sxp > SKY_W + 80.0f)
                continue;
            wing = ((n + m * 3) / (f->actor < SKY_NATIVES ? 7u : 4u)) & 1;
            SkyDrawActor(actor, wing, sxp, my, 0.0f, scale, f->crossing, 255);
        }
        /* The legendary's wake. */
        if (f->actor == SKY_NATIVES)
            for (unsigned k = 0; k < 5; ++k)
            {
                float lx = SkyScreenX(headX - dir * (30.0f + 22.0f * (float)k)) + shift;
                uint32_t head = C2D_Color32(255, 255, 255, (uint8_t)(150 - 25 * k)), tail = C2D_Color32(255, 255, 255, 0);

                C2D_DrawRectangle(sSky.sx > 0 ? lx - 40.0f : lx, f->y - 6.0f + 3.0f * (float)k, 0, 40.0f, 1.0f,
                                  sSky.sx > 0 ? tail : head, sSky.sx > 0 ? head : tail,
                                  sSky.sx > 0 ? tail : head, sSky.sx > 0 ? head : tail);
            }
    }
}

/* The flyers' cries: each flock calls once, softly, as its head comes into
 * the picture - fainter the farther back, panned to its side, not two
 * within half a second (the cry has one channel); the legendary louder,
 * at once. Once a scene frame (the frame is held while the new world is
 * built). */
static void SkySounds(unsigned n)
{
    if (n == sSky.soundFrame)
        return;
    sSky.soundFrame = n;
    for (unsigned i = 0; i < sSky.flocks; ++i)
    {
        const SkyFlock *f = &sSky.flock[i];
        const SkyActor *actor = f->actor < SKY_NATIVES ? &sSky.native[f->actor] : &sSky.legend;
        bool legend = f->actor >= SKY_NATIVES;
        uint16_t species = legend ? sSky.legendSpecies : sSky.nativeSpecies[f->actor];
        unsigned tier = SkyFlockTier(f);
        float x0 = f->speed < 0.0f ? 1.15f * SKY_W : -0.3f * SKY_W, sxp, pan;

        if (sSky.cried[i] || !actor->ok || species == 0 || n < f->start)
            continue;
        sxp = SkyScreenX(x0 + f->speed * (float)(n - f->start));
        if (sxp < -8.0f || sxp > SKY_W + 8.0f)
            continue;
        if (!legend && sSky.lastCry != 0 && n + 1 - sSky.lastCry < 30u)
            continue;
        sSky.cried[i] = true;
        sSky.lastCry = n + 1;
        pan = (sxp / SKY_W - 0.5f) * 112.0f;
        CtrFlySky_Cry(species, (int8_t)fmaxf(-64.0f, fminf(64.0f, pan)),
                      legend ? 84u : tier == 0 ? 30u : tier == 1 ? 42u : 56u);
    }
}

static void DrawSkyScene(C3D_RenderTarget *target, int eye, unsigned n)
{
    C3D_Tex *cloud = CtrVoxel_CloudTexture();
    float slider = eye < 0 ? 0.0f : CtrScenario_Slider(osGet3DSliderState());
    float eyeSign = eye == 0 ? 1.0f : -1.0f; /* in front of the screen: right in the left eye */
    float par = eyeSign * slider;
    float fx, fy, tilt, speed;
    unsigned wing;
    SkyWeather w;
    /* The crossing: out of the one map's weather into the other's over the
     * middle of the soar. */
    float crossing = SkySmooth(((float)n / (float)VOXEL_FLY_SKY_FRAMES - 0.2f) / 0.3f);
    float deckA;

    SkyWeatherAt(&w, crossing);
    deckA = 0.55f + 0.45f * w.deck; /* a thin deck is a fainter one too */

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    /* The sky: deep above, pale at the deck. */
    {
        uint32_t zenith = SkyColour(w.zenith, 255), mid = SkyColour(w.mid, 255), horizon = SkyColour(w.horizon, 255);

        C2D_DrawRectangle(0, 0, 0, SKY_W, SKY_H * 0.36f, zenith, zenith, mid, mid);
        C2D_DrawRectangle(0, SKY_H * 0.36f, 0, SKY_W, SKY_H * 0.64f, mid, mid, horizon, horizon);
    }
    SkyFlyerPath(n, &fx, &fy, &tilt, &speed);
    /* The planes, far to near, with the flyers between them: high thin
     * cloud far behind, the far deck, the far flocks, the air's haze over
     * those, the middle deck, the near flocks and us, and a thin veil of
     * the nearest cloud over all, seen through. Every sheet fades in from
     * its top edge; a thin deck is a sparser, lower one. */
    if (cloud != NULL)
    {
        SkyDrawDeck(cloud, n, 0.0f, SKY_H * 0.34f, 2.6f, 1.3f, SkyColour(w.deckTint, (unsigned)(110.0f * deckA)), -4.0f * par, 0.7f, 0.0f);
        SkyDrawDeck(cloud, n, SKY_H * (0.48f + 0.12f * (1.0f - w.deck)), SKY_H * 1.0f, 2.2f, 1.0f,
                    SkyColour(w.deckTint, (unsigned)(150.0f * deckA)), -3.0f * par, 0.0f, 1.0f);
    }
    SkyDrawFlocks(n, par, 0);
    if (w.hazeAmount > 0.005f)
        C2D_DrawRectSolid(0, 0, 0, SKY_W, SKY_H, SkyColour(w.haze, (unsigned)(w.hazeAmount * 150.0f)));
    if (cloud != NULL)
        SkyDrawDeck(cloud, n, SKY_H * (0.60f + 0.10f * (1.0f - w.deck)), SKY_H * 1.0f, 1.6f, 2.4f,
                    SkyColour(w.deckTint, (unsigned)(190.0f * deckA)), 0.0f, 0.0f, 1.0f);
    SkyDrawFlocks(n, par, 1);
    /* Us: the Pokemon, the player on its back. */
    {
        float cx = fx + 3.0f * par, cy = fy;
        float seatX = (float)(sSky.sx > 0 ? -sSky.seatX : sSky.seatX) * SKY_FLYER_SCALE;
        float seatY = (float)sSky.seatY * SKY_FLYER_SCALE;
        float c = cosf(tilt), s = sinf(tilt);

        wing = (n / (speed > 0.6f ? 4u : 7u)) & 1;
        SkyDrawActor(&sSky.flyer, wing, cx, cy, tilt, SKY_FLYER_SCALE, false, 255);
        if (sSky.rider.ok)
            SkyDrawActor(&sSky.rider, 0, cx + seatX * c - seatY * s, cy + seatX * s + seatY * c, tilt,
                         SKY_FLYER_SCALE, false, 255);
    }
    /* The nearest flyers cross in front of us; then the veil and the
     * dollops of cloud whizzing past over everything, now and then right
     * over us. */
    SkyDrawFlocks(n, par, 2);
    if (cloud != NULL)
    {
        SkyDrawDeck(cloud, n, SKY_H * 0.70f, SKY_H * 1.0f, 1.1f, 4.6f, SkyColour(w.deckTint, (unsigned)(120.0f * deckA)),
                    6.0f * par, 0.0f, 0.85f);
        for (unsigned i = 0; sSkyPuffTex.data != NULL && i < SKY_PUFFS; ++i)
        {
            /* The big ones run a long loop, so each passes about once; they
             * go with the weather - under a thin clear deck fewer and
             * lighter, in rain and storm all of them, thicker and larger,
             * so for a moment the two of us are shapes in the murk. */
            bool big = i >= SKY_PUFFS - SKY_BIG_PUFFS;
            float heavy = w.deck > w.rain ? w.deck : w.rain;
            float grow = big ? 1.0f + 0.35f * w.rain : 1.0f;
            float span = big ? 1400.0f + 2.0f * sSky.puff[i].w : SKY_W + 2.0f * sSky.puff[i].w;
            float x = span - (float)fmod(sSky.puff[i].phase + sSky.puff[i].speed * (float)n, span) - sSky.puff[i].w;
            float alpha = sSky.puff[i].alpha * (big ? 0.45f + 0.55f * heavy : 0.6f + 0.4f * w.deck);

            if (big && i == SKY_PUFFS - 1 && heavy < 0.5f)
                continue; /* fine weather: one big one fewer */
            SkyDrawPuff(SkyScreenX(x) + 9.0f * par, sSky.puff[i].y, sSky.puff[i].w * grow, sSky.puff[i].h * grow,
                        sSky.puff[i].slot, sSky.puff[i].mirrored,
                        SkyColour(w.deckTint, (unsigned)(alpha > 255.0f ? 255.0f : alpha)));
        }
    }
    /* The rain: drops driven down and back past us; the ash: flakes
     * drifting down; the sand: the wind's streaks thick and tan. */
    if (w.rain > 0.02f)
    {
        unsigned count = (unsigned)(SKY_RAIN_DROPS * w.rain);
        uint32_t drop = C2D_Color32(206, 222, 250, (uint8_t)(150.0f * w.rain)), clear = C2D_Color32(206, 222, 250, 0);
        float fall = 13.0f + 5.0f * w.storm, drift = (3.5f + 3.0f * w.storm) * (float)sSky.sx;

        for (unsigned i = 0; i < count; ++i)
        {
            uint32_t h = SkyHash(i * 2654435761u + sSky.seed);
            /* Each drop its own pace and length: in step they read as a grid. */
            float pace = 0.7f + (float)(SkyHash(h) & 255) / 255.0f * 0.6f;
            float x0 = (float)(h & 0xFFFF) / 65535.0f * (SKY_W + 60.0f), y0 = (float)((h >> 16) & 0xFFFF) / 65535.0f * (SKY_H + 40.0f);
            float y = (float)fmod(y0 + fall * pace * (float)n, SKY_H + 40.0f) - 20.0f;
            float x = (float)fmod(x0 - drift * pace * (float)n + 4000.0f * (SKY_W + 60.0f), SKY_W + 60.0f) - 30.0f + 5.0f * par;

            C2D_DrawLine(x, y, drop, x + drift * 0.9f * pace, y - fall * 0.9f * pace, clear, 1.0f, 0.0f);
        }
        /* The spray: fine flecks torn off the rain, nearer, lighter and
         * quicker, driven almost level back past us. */
        count = (unsigned)(SKY_RAIN_DROPS * (0.7f + 0.5f * w.storm) * w.rain);
        {
            uint32_t fleck = C2D_Color32(224, 236, 255, (uint8_t)(110.0f * w.rain)), gone = C2D_Color32(224, 236, 255, 0);
            float rush = (14.0f + 6.0f * w.storm) * (float)sSky.sx;

            for (unsigned i = 0; i < count; ++i)
            {
                uint32_t h = SkyHash(i * 2246822519u + sSky.seed * 3u);
                float speed = rush * (0.8f + (float)(h & 15) / 40.0f);
                float x0 = (float)(h & 0xFFFF) / 65535.0f * (SKY_W + 80.0f), y0 = (float)((h >> 16) & 0xFFFF) / 65535.0f * (SKY_H + 20.0f);
                float y = (float)fmod(y0 + (2.5f + (float)(h & 7) * 0.4f) * (float)n, SKY_H + 20.0f) - 10.0f;
                float x = (float)fmod(x0 - speed * (float)n + 4000.0f * (SKY_W + 80.0f), SKY_W + 80.0f) - 40.0f + 10.0f * par;

                C2D_DrawLine(x, y, fleck, x + speed * 0.3f, y - 1.2f, gone, 1.0f, 0.0f);
            }
        }
    }

    if (w.sand > 0.02f)
    {
        /* Blown sand: grains tearing past level with us, thick and quick. */
        unsigned count = (unsigned)(SKY_RAIN_DROPS * 1.4f * w.sand);
        uint32_t grain = SkyColour(w.haze, (unsigned)(190.0f * w.sand)), clear = SkyColour(w.haze, 0);
        float rush = 18.0f * (float)sSky.sx;

        for (unsigned i = 0; i < count; ++i)
        {
            uint32_t h = SkyHash(i * 3266489917u + sSky.seed);
            float speed = rush * (0.8f + (float)(h & 15) / 32.0f);
            float x0 = (float)(h & 0xFFFF) / 65535.0f * (SKY_W + 80.0f), y0 = (float)((h >> 16) & 0xFFFF) / 65535.0f * SKY_H;
            float y = y0 + 3.0f * sinf((float)n * 0.2f + (float)i);
            float x = (float)fmod(x0 - speed * (float)n + 4000.0f * (SKY_W + 80.0f), SKY_W + 80.0f) - 40.0f + 8.0f * par;

            C2D_DrawLine(x, y, grain, x + speed * 0.5f, y + 0.6f, clear, (h & 8) ? 1.5f : 1.0f, 0.0f);
        }
    }
    if (w.ash > 0.02f)
    {
        unsigned count = (unsigned)(SKY_ASH_FLAKES * w.ash);

        uint32_t flake = C2D_Color32(226, 226, 230, (uint8_t)(200.0f * w.ash));

        for (unsigned i = 0; i < count; ++i)
        {
            uint32_t h = SkyHash(i * 2246822519u + sSky.seed);
            float x0 = (float)(h & 0xFFFF) / 65535.0f * (SKY_W + 40.0f), y0 = (float)((h >> 16) & 0xFFFF) / 65535.0f * SKY_H;
            float y = (float)fmod(y0 + (1.2f + (float)(h & 7) * 0.2f) * (float)n, SKY_H + 10.0f) - 5.0f;
            float x = (float)fmod(x0 - 2.0f * (float)sSky.sx * (float)n + 4000.0f * (SKY_W + 40.0f), SKY_W + 40.0f) - 20.0f
                    + 6.0f * sinf((float)n * 0.05f + (float)i) + 4.0f * par;

            C2D_DrawRectSolid(x, y, 0, (h & 8) ? 2.0f : 1.0f, (h & 8) ? 2.0f : 1.0f, flake);
        }
    }
    /* The wind: streaks running back past us, thick and fast as we go. */
    for (unsigned i = 0; i < SKY_MARKS; ++i)
    {
        float span = SKY_W + 140.0f;
        float x = (float)fmod(sSky.mark[i].phase - sSky.mark[i].speed * (0.4f + speed) * (float)n, span);
        float len = sSky.mark[i].len * (0.5f + speed);
        uint8_t a = (uint8_t)((float)sSky.mark[i].alpha * (0.25f + 0.75f * speed) * (1.0f + 0.5f * w.sand));
        uint32_t bright = w.sand > 0.02f ? SkyColour(w.haze, a) : C2D_Color32(255, 255, 255, a);
        uint32_t clear = bright & 0x00FFFFFFu;
        float sx0;

        if (x < 0.0f) x += span;
        x -= 120.0f;
        sx0 = sSky.sx > 0 ? x : SKY_W - x - len;
        sx0 += 7.0f * par;
        /* The head (forward) end bright, the tail clear. */
        if (sSky.sx > 0)
            C2D_DrawRectangle(sx0, sSky.mark[i].y, 0, len, sSky.mark[i].thick, clear, bright, clear, bright);
        else
            C2D_DrawRectangle(sx0, sSky.mark[i].y, 0, len, sSky.mark[i].thick, bright, clear, bright, clear);
    }
    /*
     * The storm's lightning: now and then a glow in the deck, decided once a
     * scene frame (the first eye; a frame held while the new world builds is
     * not asked again). Kept gentle for photosensitive players: one soft
     * stroke dying away, never two within SKY_FLASH_GAP frames (two seconds,
     * well under three flashes a second), at most about a third of white.
     */
    if (w.storm > 0.3f)
    {
        /* Not in the fade back to white: its last frame is held while the
         * new world builds, and a stroke there would never die away. */
        if (eye <= 0 && n != sSky.flashAsked && n + SKY_FADE_OUT < VOXEL_FLY_SKY_FRAMES)
        {
            sSky.flashAsked = n;
            if ((sSky.flashFrame == ~0u || n >= sSky.flashFrame + SKY_FLASH_GAP)
             && ((n * 2654435761u + sSky.seed) >> 8) % 1000u < (unsigned)(w.storm * 8.0f))
                sSky.flashFrame = n;
        }
        if (sSky.flashFrame != ~0u && n >= sSky.flashFrame && n - sSky.flashFrame < 8u)
        {
            /* The deck lit from inside, brightest low down where the bolt is. */
            static const float stroke[8] = {1.0f, 0.75f, 0.52f, 0.34f, 0.2f, 0.11f, 0.05f, 0.02f};
            float flash = stroke[n - sSky.flashFrame] * w.storm;
            uint32_t lit = C2D_Color32(255, 250, 236, (uint8_t)(flash * 90.0f)), none = C2D_Color32(255, 250, 236, 0);

            C2D_DrawRectSolid(0, 0, 0, SKY_W, SKY_H, C2D_Color32(230, 234, 255, (uint8_t)(flash * 36.0f)));
            C2D_DrawRectangle(0, SKY_H * 0.42f, 0, SKY_W, SKY_H * 0.58f, none, none, lit, lit);
        }
    }
    /* Out of the white, and back into it. */
    {
        float white = 0.0f;


        if (n < SKY_FADE_IN)
            white = 1.0f - (float)n / (float)SKY_FADE_IN;
        else if (n + SKY_FADE_OUT >= VOXEL_FLY_SKY_FRAMES)
            white = (float)(n + SKY_FADE_OUT - VOXEL_FLY_SKY_FRAMES + 1) / (float)SKY_FADE_OUT;
        if (white > 0.0f)
            C2D_DrawRectSolid(0, 0, 0, SKY_W, SKY_H, C2D_Color32(238, 242, 248, (uint8_t)(fminf(white, 1.0f) * 255.0f + 0.5f)));
    }
    C2D_Flush();
}

/*
 * The flight's wind: wisps of cloud streaming out from the picture's centre
 * to its edges, over the world while the camera is up, as the air rushing
 * past. Each is a long thin strip of the world's cloud texture (tileable A8
 * noise, CtrVoxel_CloudTexture) stretched along its way and scrolled
 * outwards, clear at its inner end and whitest at the edge; under them the
 * edges are hazed white. Together they hide what the edges of the high view
 * show before it is built (the black of unbuilt chunks) and sell the rush.
 * Drawn under the cloud cover, a little in front of the screen in 3D.
 */
#define WIND_WISPS 30u
#define WIND_PARALLAX 3.0f
#define WIND_HAZE 0.6f     /* the edges' white haze, at full strength */
#define WIND_SPEED 0.9f    /* texture repeats a second, outwards */
static uint32_t WindRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static float WindUnit(uint32_t *state)
{
    return (float)(WindRandom(state) & 0xFFFF) / 65535.0f;
}

static void DrawWind(C3D_RenderTarget *target, float strength, int eye)
{
    const float w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT, cx = w * 0.5f, cy = h * 0.5f;
    C3D_Tex *tex = CtrVoxel_CloudTexture();
    double seconds = (double)svcGetSystemTick() / SYSCLOCK_ARM11;
    float k = fminf(strength, 1.0f);
    float parallax = eye < 0 ? 0.0f : (eye == 0 ? 1.0f : -1.0f) * WIND_PARALLAX * CtrScenario_Slider(osGet3DSliderState());
    uint32_t state = 0x9E3779B9u; /* one set: the motion is the texture's */
    unsigned i;

    if (k <= 0.005f) return;
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    /* The haze: the picture's edges whitened, clear towards the middle. */
    {
        uint32_t edge = C2D_Color32(240, 244, 250, (uint8_t)(k * WIND_HAZE * 255.0f + 0.5f));
        uint32_t none = C2D_Color32(240, 244, 250, 0);
        float band = h * 0.45f;

        C2D_DrawRectangle(0, 0, 0, w, band, edge, edge, none, none);
        C2D_DrawRectangle(0, h - band, 0, w, band, none, none, edge, edge);
        C2D_DrawRectangle(0, 0, 0, band, h, edge, none, edge, none);
        C2D_DrawRectangle(w - band, 0, 0, band, h, none, edge, none, edge);
    }
    if (tex == NULL) { C2D_Flush(); return; }
    for (i = 0; i < WIND_WISPS; ++i)
    {
        float angle = ((float)i + WindUnit(&state) * 0.8f) * (6.2831853f / (float)WIND_WISPS);
        float dx = cosf(angle), dy = sinf(angle);
        float inner = 50.0f + WindUnit(&state) * 80.0f;  /* clear of the middle */
        float outer = 280.0f;                             /* past the corners */
        float length = outer - inner;
        float thick = 8.0f + WindUnit(&state) * 16.0f;
        float alpha = k * (0.3f + WindUnit(&state) * 0.45f);
        float speed = WIND_SPEED * (0.7f + WindUnit(&state) * 0.6f);
        float v0 = WindUnit(&state);
        /* The noise drawn out along the wisp (few repeats over its length,
         * a sliver of it across), scrolled outwards. */
        float ru = 0.45f + WindUnit(&state) * 0.35f, rv = thick / 128.0f * 0.5f;
        float u0 = (float)fmod(-seconds * speed, 1.0);
        Tex3DS_SubTexture sub = {(uint16_t)length, (uint16_t)thick, u0, v0, u0 + ru, v0 + rv};
        C2D_Image image = {tex, &sub};
        C2D_ImageTint tint;
        uint32_t base = C2D_Color32(255, 255, 255, (uint8_t)(alpha * 255.0f + 0.5f));
        uint32_t tip = C2D_Color32(255, 255, 255, 0);
        float mx = cx + parallax + dx * (inner + length * 0.5f), my = cy + dy * (inner + length * 0.5f);

        C2D_SetImageTint(&tint, C2D_TopLeft, tip, 1.0f);
        C2D_SetImageTint(&tint, C2D_BotLeft, tip, 1.0f);
        C2D_SetImageTint(&tint, C2D_TopRight, base, 1.0f);
        C2D_SetImageTint(&tint, C2D_BotRight, base, 1.0f);
        C2D_DrawImageAtRotated(image, mx, my, 0.0f, angle, &tint, 1.0f, 1.0f);
    }
    C2D_Flush();
}

#define CLOUD_COVER_PARALLAX 7.0f /* the fine sheet's shift at the slider's top, pixels */

static void DrawCloudCover(C3D_RenderTarget *target, float amount, int eye)
{
    const float w = CTR_GAME_WIDTH, h = CTR_GAME_HEIGHT;
    C3D_Tex *tex = CtrVoxel_CloudTexture();
    double seconds = (double)svcGetSystemTick() / SYSCLOCK_ARM11;
    float k = fminf(amount, 1.0f);
    float parallax = eye < 0 ? 0.0f : (eye == 0 ? 1.0f : -1.0f) * CLOUD_COVER_PARALLAX * CtrScenario_Slider(osGet3DSliderState());
    C2D_ImageTint tint;

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    /* The wash: a cool white. */
    C2D_DrawRectSolid(0, 0, 0, w, h, C2D_Color32(238, 242, 248, (uint8_t)(k * k * 255.0f + 0.5f)));
    if (tex != NULL)
    {
        /* Two sheets: a broad slow one and a finer quicker one over it. */
        static const struct { float repeats, speedU, speedV, depth; uint8_t alpha; } sheets[2] = {
            {1.6f, 0.030f, 0.012f, 0.45f, 225},
            {3.1f, -0.055f, 0.028f, 1.0f, 150},
        };

        for (unsigned s = 0; s < 2; ++s)
        {
            /* The eye's shift, in the sheet's own texture units (it is drawn
             * over the whole picture, so it is scrolled rather than moved). */
            float shift = parallax * sheets[s].depth * sheets[s].repeats / w;
            float u = (float)fmod(seconds * sheets[s].speedU - shift, 1.0), v = (float)fmod(seconds * sheets[s].speedV, 1.0);
            float ru = sheets[s].repeats, rv = sheets[s].repeats * h / w;
            Tex3DS_SubTexture sub = {(uint16_t)w, (uint16_t)h, u, v, u + ru, v + rv};
            C2D_Image image = {tex, &sub};

            C2D_PlainImageTint(&tint, C2D_Color32(255, 255, 255, (uint8_t)(k * (float)sheets[s].alpha + 0.5f)), 1.0f);
            C2D_DrawImageAt(image, 0.0f, 0.0f, 0.0f, &tint, 1.0f, 1.0f);
        }
    }
    C2D_Flush();
}

static void DrawPoisonVeil(C3D_RenderTarget *target, unsigned alpha)
{
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(0, 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT, C2D_Color32(150, 60, 200, alpha));
}

#if CTR_SHOW_FPS
static void DrawFps(C3D_RenderTarget *target)
{
    unsigned fps = (unsigned)(sStats.fps + 0.5f), game = (unsigned)(sGameFps + 0.5f);
    unsigned glyphs[12], count = 0;
    char digits[8];
    int length = snprintf(digits, sizeof(digits), "%u/%u", fps > 999 ? 999 : fps, game > 999 ? 999 : game);

    glyphs[count++] = 10;   /* F */
    glyphs[count++] = 11;   /* P */
    glyphs[count++] = 5;    /* S, which is a 5 */
    glyphs[count++] = ~0u;  /* space */
    for (int i = 0; i < length; ++i)
        glyphs[count++] = digits[i] == '/' ? 12u : (unsigned)(digits[i] - '0');

    /* Whatever path composed the frame, the counter draws with the 2D
     * program and no depth test, as RenderVoxel's UI pass does. */
    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(2, 2, 0, count * FPS_ADVANCE + FPS_PIXEL * 3, 5 * FPS_PIXEL + FPS_PIXEL * 4,
                      C2D_Color32(0, 0, 0, 160));
    for (unsigned i = 0; i < count; ++i)
    {
        if (glyphs[i] == ~0u) continue;
        const char *bits = sFpsGlyphs[glyphs[i]];
        float x = 2 + FPS_PIXEL * 2 + i * FPS_ADVANCE, y = 2 + FPS_PIXEL * 2;
        for (unsigned p = 0; p < 15; ++p)
            if (bits[p] == '1')
                C2D_DrawRectSolid(x + (p % 3) * FPS_PIXEL, y + (p / 3) * FPS_PIXEL, 0,
                                  FPS_PIXEL, FPS_PIXEL, C2D_Color32(255, 255, 255, 255));
    }
    C2D_Flush();
}
#endif

/*
 * The battle speed badge (CtrPlatform_BattleSpeedBadge): ">> X2", ">> X1.5"
 * in the top right corner, in the FPS counter's 3x5 pixel font at 3x, over a
 * translucent box. Drawn last, like the counter, at zero parallax.
 */
#define BADGE_PIXEL 3.0f
static const char *const sBadgeGlyphs[] =
{
    "111101101101111", "010110010010111", "111001111100111", "111001111001111",
    "101101111001001", "111100111001111", "111100111101111", "111001001001001",
    "111101111101111", "111101111001111",
    "101101010101101", /* X */
    "000000000000010", /* . */
};

static void DrawSpeedBadge(C3D_RenderTarget *target, int halves)
{
    char text[16];
    unsigned glyphs[6], count = 0;
    float width = 0.0f, x, y;
    const float arrows = 2 * (3 * BADGE_PIXEL + BADGE_PIXEL), pad = 2 * BADGE_PIXEL;
    const u32 white = C2D_Color32(255, 255, 255, 255);

    halves = halves < 2 ? 2 : halves > 8 ? 8 : halves;
    snprintf(text, sizeof(text), "X%d%s", halves / 2, halves & 1 ? ".5" : "");
    for (const char *c = text; *c && count < 6; ++c)
        glyphs[count++] = *c == 'X' ? 10u : *c == '.' ? 11u : (unsigned)(*c - '0');
    for (unsigned i = 0; i < count; ++i)
        width += (glyphs[i] == 11 ? 2 : 4) * BADGE_PIXEL;
    x = CTR_GAME_WIDTH - 4 - (pad + arrows + width + pad);
    y = 4;

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectSolid(x, y, 0, pad + arrows + width + pad, 5 * BADGE_PIXEL + 2 * pad, C2D_Color32(0, 0, 0, 160));
    x += pad;
    y += pad;
    /* Two arrowheads: fast forward. */
    for (int i = 0; i < 2; ++i)
    {
        float ax = x + i * 4 * BADGE_PIXEL;

        C2D_DrawTriangle(ax, y, white, ax, y + 5 * BADGE_PIXEL, white, ax + 3 * BADGE_PIXEL, y + 2.5f * BADGE_PIXEL, white, 0);
    }
    x += arrows;
    for (unsigned i = 0; i < count; ++i)
    {
        const char *bits = sBadgeGlyphs[glyphs[i]];

        for (unsigned p = 0; p < 15; ++p)
            if (bits[p] == '1')
                C2D_DrawRectSolid(x + (p % 3) * BADGE_PIXEL, y + (p / 3) * BADGE_PIXEL, 0, BADGE_PIXEL, BADGE_PIXEL, white);
        x += (glyphs[i] == 11 ? 2 : 4) * BADGE_PIXEL;
    }
    C2D_Flush();
}

/*
 * Look below: while the bottom screen has the player's attention, the top
 * screen darkens towards its foot under an arrow pointing down at the touch
 * screen, as on the DS games. Over a held top (below) it is drawn once, a
 * few frames into the hold so that a hold between two screens does not
 * flash; with the bottom screen's column in focus (CtrVideo_LookDown) the
 * game still draws the top, and it goes over every frame. sTopDark: the
 * top's buffers have it already (left there by a hold, or kept with the
 * held frame), never to be darkened twice over.
 */
#define LOOK_DOWN_AFTER 6   /* frames held before the top darkens */
static bool sLookDown, sTopDark;
static unsigned sHeldFrames;

void CtrVideo_LookDown(bool on) { sLookDown = on; }

static void DrawLookDown(C3D_RenderTarget *target)
{
    const float cx = CTR_GAME_WIDTH / 2.0f, tip = CTR_GAME_HEIGHT - 10.0f;
    u32 top = C2D_Color32(0, 0, 0, 90), foot = C2D_Color32(0, 0, 0, 200);
    u32 white = C2D_Color32(255, 255, 255, 235), edge = C2D_Color32(0, 0, 0, 255);

    C2D_Prepare();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    BlendForget();
    SceneOn(target);
    C2D_ViewReset();
    Blend(5, false, false);
    C2D_DrawRectangle(0, 0, 0, CTR_GAME_WIDTH, CTR_GAME_HEIGHT, top, top, foot, foot);
    /* The arrow: a shaft and a head, each over its outline a pixel wider. */
    C2D_DrawRectSolid(cx - 7, tip - 33, 0, 14, 17, edge);
    C2D_DrawTriangle(cx - 17, tip - 17, edge, cx + 17, tip - 17, edge, cx, tip + 1, edge, 0);
    C2D_DrawRectSolid(cx - 6, tip - 32, 0, 12, 16, white);
    C2D_DrawTriangle(cx - 15, tip - 16, white, cx + 15, tip - 16, white, cx, tip - 1, white, 0);
    C2D_Flush();
}

/*
 * The bottom screen runs some of the game's menus without showing them (the
 * party menu, the bag): while it does, the top screen keeps the last frame it
 * presented, the world as it was when the menu opened. Nothing is rendered;
 * the frame is only paced to the display.
 */
static bool sHoldTop;

/*
 * The Pokédex draws its device over the held frame, and a screen chosen from
 * it opens with the top still held (CtrBottom_LeaveTo): the world's frame is
 * kept aside as the device comes up and put back as it goes, the top still
 * held. Each eye's colour buffer, copied whole, in linear memory meanwhile.
 */
static struct
{
    void *eye[2];
    bool kept, dexShown, failed, dark;
} sHeldTop;

static void HeldTopForget(void)
{
    for (int i = 0; i < 2; ++i)
    {
        if (sHeldTop.eye[i]) linearFree(sHeldTop.eye[i]);
        sHeldTop.eye[i] = NULL;
    }
    sHeldTop.kept = sHeldTop.dexShown = false;
}

static u32 TargetBytes(const C3D_RenderTarget *target)
{
    const C3D_FrameBuf *fb = &target->frameBuf;

    return (u32)fb->width * fb->height * (fb->colorFmt == GPU_RB_RGBA8 ? 4 : fb->colorFmt == GPU_RB_RGB8 ? 3 : 2);
}

/* Outside a frame, the last one having left the top screen alone (held). */
static void HeldTopKeep(void)
{
    C3D_RenderTarget *eyes[2] = { sTop, sStereo ? sTopRight : NULL };

    if (!sTop) return;
    for (int i = 0; i < 2; ++i)
    {
        u32 size;

        if (!eyes[i]) continue;
        size = TargetBytes(eyes[i]);
        sHeldTop.eye[i] = linearAlloc(size);
        if (!sHeldTop.eye[i])
        {
            if (!sHeldTop.failed)
                CtrLog_Write(CTR_LOG_ERROR, "video: no linear memory to keep the held top screen (free=%lu); "
                             "the Pokédex's device stays on it", (unsigned long)linearSpaceFree());
            sHeldTop.failed = true;
            HeldTopForget();
            return;
        }
        C3D_SyncTextureCopy((u32 *)eyes[i]->frameBuf.colorBuf, 0, (u32 *)sHeldTop.eye[i], 0, size,
                            GX_TRANSFER_RAW_COPY(1));
    }
    sHeldTop.kept = true;
    sHeldTop.dark = sTopDark;
}

/* In a frame: the kept frame back in the top's buffers, which go out at its end. */
static void HeldTopPutBack(void)
{
    C3D_RenderTarget *eyes[2] = { sTop, sTopRight };

    for (int i = 0; i < 2; ++i)
        if (sHeldTop.eye[i] && eyes[i])
        {
            C3D_SyncTextureCopy((u32 *)sHeldTop.eye[i], 0, (u32 *)eyes[i]->frameBuf.colorBuf, 0,
                                TargetBytes(eyes[i]), GX_TRANSFER_RAW_COPY(1));
            C3D_FrameDrawOn(eyes[i]);
        }
    sTopDark = sHeldTop.dark;
    bool stereo = sHeldTop.eye[1] != NULL && sTopRight;
    if (stereo != sStereo) { gfxSet3D(stereo); sStereo = stereo; }
    CtrLog_Write(CTR_LOG_VIDEO, "top screen: the held frame back from under the Pokédex");
    HeldTopForget();
}

void CtrVideo_HoldTop(bool hold)
{
    if (hold != sHoldTop)
        CtrLog_Write(CTR_LOG_VIDEO, "top screen %s at frame %lu", hold ? "held" : "released",
                     (unsigned long)sStats.frames);
    sHoldTop = hold;
    if (!hold) HeldTopForget();
}

bool CtrVideo_TopHeld(void) { return sHoldTop; }
