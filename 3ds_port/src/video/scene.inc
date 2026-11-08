/*
 * Part of 3ds_video.c. The scene surface and its cache: SceneMake,
 * ScenePrepare, the sprites' depths, SceneComposeInto, and the keys that say
 * the scenery holds (SceneCacheKey, SceneHoldsSince).
 */

/*
 * Composes the logical frame with this eye's parallax and blits it to one
 * screen buffer. Render-to-texture followed by sampling that texture needs a
 * GPU command split, and so does composing the surface again for the other
 * eye, so each pass ends with one.
 */
/*
 * Outside the frame: the battle scene's surface while the battle is up, given
 * back a few seconds after. Without VRAM for it the scene is composed at 1.5
 * directly, and the allocation is tried again later.
 */
static bool sSceneCached;
/*
 * The scene surface cleared whole this frame as a memory fill (SceneComposeInto
 * from column 0, the mono keeper's copy): nothing of a frame before is left in
 * its free columns. Reset as the stereo battle begins its frame, which puts
 * slots there only then (RenderBattleStereo).
 */
static bool sSceneFilled;
/* A battle without its scene surface has no 3D: tried again soon. */
#define SCENE_RETRY_FRAMES 60u

static void SceneRelease(void)
{
    sSceneCached = false;
    if (sScene) { ClearWhenMadeForget(sScene); C3D_RenderTargetDelete(sScene); }
    if (sSceneTex.data) C3D_TexDelete(&sSceneTex);
    sScene = NULL;
    memset(&sSceneTex, 0, sizeof(sSceneTex));
}

static bool SceneMake(void)
{
    if (C3D_TexInitVRAM(&sSceneTex, SCENE_W, SCENE_H, GPU_RGBA5551)
        && (sScene = C3D_RenderTargetCreateFromTex(&sSceneTex, GPU_TEXFACE_2D, 0, -1)))
    {
        ClearWhenMade(sScene);
        return true;
    }
    SceneRelease();
    return false;
}

static void SceneReady(void)
{
    CtrLog_Write(CTR_LOG_VIDEO, "battle scene surface ready: filtered zoom (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
    sSceneFailed = false;
    C3D_TexSetFilter(&sSceneTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sSceneTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

/*
 * With the battle over the world, the scene surface is kept from the field
 * on, made while the VRAM is still in one piece. Made at a battle's start, it
 * needed 512 KiB in one block when the field's atlases had cut the VRAM into
 * smaller ones: atlases on screen went for it under the transition, blocks
 * missing there, and the battle's own layer at its start found no block
 * either - the atlases went again, one no longer fitted back, and the battle
 * stood on its classic background for over a second (port.log 2026-10-22:
 * 460 KiB free, the largest block 205 KiB, 76 frames drawn flat).
 */
static bool SceneKept(void)
{
    return CtrSettings_Voxel() && CtrSettings_VoxelBattle() && CtrVoxel_HoldsVram();
}

/*
 * Outside the frame (LayersPrepare): a stage's layer textures that found no
 * room beside its depth planes. No battle comes from a stage, so the scene
 * surface kept for one goes before the planes do: kept from start-up, it held
 * the 512 KiB that the intro's four layers needed beside two planes, and the
 * intro and the title gave all their planes up and were composed per eye -
 * 40 to 50 fps on an Old 3DS with the slider up (port.log 2026-11-01; with
 * the battle over the world off they kept two planes). Made again once no
 * stage is up (ScenePrepare). Whether it went.
 */
static bool SceneYield(void)
{
    if (!sScene || sBattle || sTransition || sBattleRequested || sTransitionRequested) return false;
    SceneRelease();
    sSceneFailFrame = sStats.frames;
    CtrLog_Write(CTR_LOG_VIDEO, "battle scene surface given back for a stage's layers (VRAM free=%lu)",
                 (unsigned long)vramSpaceFree());
    return true;
}

static void ScenePrepare(void)
{
    if (!sBattle && !sTransition)
    {
        if (SceneKept())
        {
            /* A map on screen that found no VRAM for its atlas comes first:
             * the layers and surfaces no screen draws go, and if there were
             * none, the scene surface, made again once the room is free. */
            if (sScene && sVramAsked)
            {
                bool freed = ReleaseIdleLayers();

                if (!(ReleaseIdleSurfaces() || freed))
                {
                    SceneRelease();
                    sSceneFailFrame = sStats.frames;
                }
            }
            /* From free VRAM and the atlases out of view only, nothing on
             * screen giving way for it - the option switched on in a game,
             * the field's atlases already had the VRAM (port.log
             * 2026-10-22: settings battle=0 at start-up, the surface made at
             * the first battle instead); the largest block looked for twice a
             * second, not every frame. Not while a stage is up: its layers
             * had it (SceneYield). */
            else if (!sScene && !sStage && sStats.frames - sSceneFailFrame >= 30u)
            {
                /* At most every ten seconds: freed for nothing - the VRAM
                 * in pieces still - the ring's atlases were made again. */
                static uint32_t sSceneFreedFrame;

                if (CtrGpuMem_LargestVram() < SCENE_W * SCENE_H * 2u
                    && (sSceneFreedFrame == 0 || sStats.frames - sSceneFreedFrame >= 600u))
                {
                    sSceneFreedFrame = sStats.frames | 1u;
                    CtrVoxel_ReleaseIdleVramFor(SCENE_W * SCENE_H * 2u, false);
                }
                if (CtrGpuMem_LargestVram() >= SCENE_W * SCENE_H * 2u && SceneMake())
                    SceneReady();
                else
                    sSceneFailFrame = sStats.frames;
            }
            return;
        }
        /* Kept for a battle that follows another, but not over the voxel
         * field: its atlases want the room back, and an atlas that found
         * none drew the field in 2D for a frame and asked for it then. */
        if (sScene && (sVramAsked || sFieldVoxel || sStats.frames - sSceneUsedFrame > LAYER_IDLE_FRAMES))
            SceneRelease();
        return;
    }
    sSceneUsedFrame = sStats.frames;
    if (sScene || (sSceneFailed && sStats.frames - sSceneFailFrame < SCENE_RETRY_FRAMES)) return;
    for (unsigned attempt = 0; attempt < 3 && !sScene; ++attempt)
    {
        bool freed = false;

        if (SceneMake())
            break;
        /*
         * Back from the bag or the party (a centred screen), what it held is
         * not idle yet: its layer textures, its depth planes, the bottom
         * surface. Kept, they left no 512 KiB gap, the battle had no scene
         * surface and so no 3D until the retry - the turn was over by then
         * (port.log, 2026-10-15: 187 frames with the slider up, none in 3D).
         * Then the overworld's atlases of other maps, as for the depth planes.
         */
        if (attempt == 0)
        {
            freed = ReleaseIdleLayers();
            freed = ReleaseIdleSurfaces() || freed;
        }
        if (!freed && attempt < 2)
            freed = CtrVoxel_ReleaseIdleVramFor(SCENE_W * SCENE_H * 2u, true) > 0;
        if (!freed)
            break;
    }
    if (!sScene)
    {
        if (!sSceneFailed)
            CtrLog_Write(CTR_LOG_ERROR, "VIDEO: no VRAM for the battle scene surface (free=%lu); "
                         "unfiltered zoom", (unsigned long)vramSpaceFree());
        sSceneFailed = true;
        sSceneFailFrame = sStats.frames;
        return;
    }
    SceneReady();
}

static void GpuSplit(void);

/*
 * The battle with the 3D slider up (RenderBattleStereo). The scenery - the
 * terrain, the sky, a move's background - is one picture behind the screen,
 * BATTLE_BACK_DEPTH in each eye, the same all over. The sprites stand in
 * front of it at a depth from where they are: the opponent and its info box
 * at the top of the scene BATTLE_SPRITE_DEPTH behind the screen, the player's
 * Pokemon and its info box at the bottom on the screen itself, and a move's
 * sprites in between as they cross (BattleSpriteShift). The text box, and any
 * window over the scene, is the screen too, never displaced: its caps and
 * middle (DrawBattleText) stay joined.
 *
 * The scenery is composed once into the scene surface without the sprites,
 * and the sprites per eye into the columns of that same surface the scenery
 * leaves unused (it is 1024 texels wide, the scenery takes about 590): no
 * VRAM of their own, which beside the voxel battle's world there is not; the
 * text box once into the logical surface. Composing the whole
 * scene per eye was 40 fps on an Old 3DS. What that split cannot keep - a
 * sprite blending with what is under it, a background in front of a sprite,
 * sprites spread wider than their surface - is composed whole, for as long
 * as it lasts: once, at the scenery's depth, in the 2D battle; per eye, with
 * the same depths, over the voxel world. The scene is composed
 * BATTLE_STEREO_MARGIN GBA pixels wider on each side so that its displaced
 * edge still has picture under it. Depths in screen pixels per eye at the
 * widest slider setting.
 */
#ifdef CTR_STEREO_ZERO
#define BATTLE_BACK_DEPTH 0.0f
#define BATTLE_SPRITE_DEPTH 0.0f
#else
#define BATTLE_BACK_DEPTH 4.0f
#define BATTLE_SPRITE_DEPTH 2.0f
#endif
/* GBA lines: a sprite centred above the first is at BATTLE_SPRITE_DEPTH, one
 * centred below the second at the screen. */
#define BATTLE_SPRITE_FAR 44
#define BATTLE_SPRITE_NEAR 80
#define BATTLE_STEREO_MARGIN 3
/* Where the scene surface goes on the logical one (BattleScenePlace). */
static struct { float ox, oy, scale; int top, width, height; } sScenePut;
/* The eye being composed: +slider for the left, -slider for the right. */
static float sEyeParallax;


/* How near a sprite centred on GBA line `centre` stands: 0 at
 * BATTLE_SPRITE_DEPTH, 1 on the screen. */
static float BattleSpriteNear(int centre)
{
    float near = (centre - BATTLE_SPRITE_FAR) / (float)(BATTLE_SPRITE_NEAR - BATTLE_SPRITE_FAR);

    return near < 0 ? 0 : near > 1 ? 1 : near;
}

/* A sprite centred on GBA line `centre`, in screen pixels for this eye. */
/* This eye's displacement of the scenery, put behind the screen. */
static float BattleSceneryShift(void)
{
    return -sEyeParallax * BATTLE_BACK_DEPTH;
}

static float BattleSpriteShift(int centre)
{
    return -sEyeParallax * BATTLE_SPRITE_DEPTH * (1 - BattleSpriteNear(centre));
}

/* The view of a SCENE_ZOOM surface from `column` on, whose texel there is
 * GBA (left, top), width x height texels of it used. */
static void SceneViewEnter(int column, int left, int top, int width, int height)
{
    sStillAsked = 0;
    sZoom = SCENE_ZOOM;
    sOffX = column - left * SCENE_ZOOM;
    sOffY = -top * SCENE_ZOOM;
    sTargetW = column + width;
    sTargetH = height;
    sSurfaceH = SCENE_H;
    ClipToView();
    BlendForget();
}

/* Back to the logical surface's view, as it was. */
static void SceneViewLeave(float zoom, float offX, float offY)
{
    sZoom = zoom;
    sOffX = offX;
    sOffY = offY;
    sTargetW = CTR_GAME_WIDTH;
    sTargetH = CTR_GAME_HEIGHT;
    sSurfaceH = 256;
    ClipToView();
    BlendForget();
}

/*
 * Composes the layers not in `exclude` (Layers mask bits) into `target`, a
 * SCENE_ZOOM surface whose top-left texel is GBA (left, top), width x height
 * texels of it used.
 */
static void SceneComposeInto(C3D_RenderTarget *target, int column, int left, int top, int width,
                             int height, unsigned exclude)
{
    float zoom = sZoom, offX = sOffX, offY = sOffY;

    SceneViewEnter(column, left, top, width, height);
    /* Cleared transparent: the backdrop is the logical surface's own clear,
     * which is RGBA8. A colour cleared into this 5551 surface comes out
     * wrong - black was a bright blue. From a column on, only that part is
     * cleared, by drawing it transparent: the rest is the scenery. */
    if (column == 0)
    {
        C2D_TargetClear(target, 0);
        if (target == sScene) sSceneFilled = true;
        SceneOn(target);
    }
    else
    {
        SceneOn(target);
        C2D_ViewReset();
        C2D_Flush();
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C2D_DrawRectSolid(column, 0, 0, width, height, 0);
        C2D_Flush();
        BlendForget();
    }
    Blend(5, false, false);
    sLayerExclude = exclude;
    memset(sBgDrawn, 0, sizeof(sBgDrawn));
    if (!(Reg(0) & 128)) Compose();
    sLayerExclude = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* Seal the world list; FrameEnd owns queue submission and completion. */
    GpuSplit();

    SceneViewLeave(zoom, offX, offY);
}

/*
 * The scenery of the 3D battle, kept in the scene surface from one frame to
 * the next while nothing it is made of changes: most of a battle the terrain
 * stands still, and composing it was 1-3 ms of CPU a frame on an Old 3DS. It
 * is made of the registers (and the line-by-line scroll), the BG palettes
 * and the VRAM of the layers it draws, and of what those layers decide from
 * more than these (SceneCacheKey); anything else that draws into the
 * scene surface's scenery columns drops it (sSceneCached). Only the stereo
 * battle's scenery pass asks for it (sSceneCacheAllowed); a scene with the
 * OBJ window in it is never kept, as the sprites shape it. One that leaves the
 * window to the eyes (sObjWinSlotted) has nothing of it and is kept as any
 * other, under a key of its own (SceneWindowHidden).
 */
static bool sSceneCacheAllowed;
static uint32_t sSceneCacheKey, sSceneCacheToken;
static unsigned sSceneCacheDrawn[4];
static StillRecord sSceneCacheStill;
/*
 * A scene that came out empty: no background layer drew anything in it (the
 * battle over the voxel world, whose world stands in for the scenery). Its
 * surface is then lent to the sprite slots and the cache above is dropped,
 * so every frame walked those empty layers at 2x again: 1.1 ms in Azahar of
 * the 3D voxel battle's frame. Made of the same, unchanged VRAM, it is empty
 * again and is not composed.
 */
static bool sSceneEmpty;
static uint32_t sSceneEmptyKey, sSceneEmptyToken;
static StillRecord sSceneEmptyStill;
static unsigned sSceneEmptyReused;
static unsigned sSceneReused;
/* The scene kept this frame (BattleSceneCompose): its edges may be too. */
static bool sSceneKeptNow;
/*
 * The battle over the voxel world with the 3D slider up shares the scene
 * surface with the world's picture, which is drawn into its first columns
 * every frame after a memory fill of the whole surface (RenderBattleWorld),
 * and that fill dropped the scenery beside it: every frame of a move's
 * background (Sandstorm's sand, BG1 at 2x) composed it again, 2.8 ms in
 * Azahar, while 2D battles kept the same frames. With the scenery of the
 * frame before still holding the fill is left out (sSceneFillOwed) and paid
 * as soon as anything needs the surface filled after all (SceneFillPay).
 * sSceneWorldLent: the last such frame's slots went into the surface's
 * columns, as the next one's likely will. Counted per 600 frames.
 */
static bool sSceneFillOwed, sSceneWorldLent;
static struct { unsigned kept, key, vram, still, slots, whole, late, other; } sWorldScene;
static void SceneFillPay(void);
/*
 * A background blending over the rest of the 2D battle's scenery (Sandstorm's
 * sand, BG1 at EVA 1-7 over all): the game steps BLDALPHA every four frames
 * while it fades in and out, and each step composed the whole scenery again -
 * 6.4 ms of an Old 3DS's frame, a dropped frame every four (port.log
 * 2026-10-28). Composed apart instead, that layer alone into a surface of its
 * own (sBlendLayerTarget, raw, its effects off) and the rest into the scene
 * surface, both kept while only the blend's weights change; the layer is then
 * blended over the scenery as the scenery is put on the screen
 * (BattleScenePlace), with Blend's own state for it.
 */
static C3D_Tex sBlendLayerTex;
static C3D_RenderTarget *sBlendLayerTarget;
static bool sBlendLayerWanted, sBlendLayerCached;
static uint32_t sBlendLayerKey, sBlendLayerToken, sBlendLayerFailFrame, sBlendLayerUsedFrame;
static StillRecord sBlendLayerStill;
static unsigned sBlendLayerAlpha;
/* The layer blended at BattleScenePlace this frame, or -1. */
static int sSceneBlendLayer = -1;
/*
 * With BLDALPHA line by line (Surf's and Muddy Water's wave): the layer is
 * blended in bands of the lines whose weights are the same, each with its
 * own (BattleScenePlace), as ComposeAlphaBands composes them - the scenery
 * and the layer then kept while only the weights change, as at one weight.
 * sBlendLayerLines: the version of the table the edges were composed with
 * (sLineAlphaVersion), beside sBlendLayerAlpha. Counted per 600 frames: the
 * frames composed apart so, those with the weights by line composed whole,
 * and the asks for the layer's surface that found none.
 */
static uint32_t sBlendLayerLines;
static unsigned sLineAlphaApart, sLineAlphaWhole, sBlendLayerNoSurface;
/* SceneCacheKey without BLDALPHA, for the two parts above; without BLDY as
 * well for the mono battle's keeper (BattleSceneComposeMono). */
static bool sSceneKeyNoAlpha, sSceneKeyNoBright;
static uint32_t SceneCacheMix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 0x01000193u;
    return h ^ (h >> 15);
}

/*
 * Why a kept scenery's key came out different: SceneCacheKey hashes its
 * terms into one key, and each kind of term into a part of its own beside
 * it - the registers and the view, the layers' scroll, the line-by-line
 * tables (scroll and BLDALPHA), what the layers decide from more than the
 * registers (LayerDrawable, a move's background on BG3), and the scenery's
 * version as battle_bg.c publishes it (a terrain or a move's background
 * loaded). The parts only explain; the key is the whole hash. Counted per
 * 600 frames where a scenery kept by key was composed again for it.
 */
enum { SCENE_PART_REG, SCENE_PART_SCROLL, SCENE_PART_LINES, SCENE_PART_DRAWABLE, SCENE_PART_VERSION,
       SCENE_PARTS };
typedef struct { uint32_t part[SCENE_PARTS]; } SceneKeyParts;
/* The parts of the last key SceneCacheKey made. */
static SceneKeyParts sSceneKeyLast;
static unsigned sSceneAgain[SCENE_PARTS];
/* The parts of the kept scenery's key (sSceneCacheKey), and of the blending
 * layer's (sBlendLayerKey). */
static SceneKeyParts sSceneCacheParts, sBlendLayerParts;

/*
 * Why this frame's scenery was composed or kept (CTR_SCENE_WHY_* bits, the
 * frame's record), and what the last check of a kept scenery found against it
 * with its key the same (SceneKeptSince, SceneHoldsSince). Only written by
 * those checks and read for the record: nothing is decided by them.
 */
_Static_assert((int)SCENE_PARTS == (int)CTR_SCENE_WHY_KEY, "the key's parts first, in their order");
#define SCENE_WHY(bit) (1u << CTR_SCENE_WHY_##bit)
#define SCENE_WHY_REASONS (SCENE_WHY(KEPT) - 1u)
static uint16_t sSceneWhy, sHoldWhy;

/* The parts of the key that differ, into the frame's bits; a key that differs
 * in no part differs by a term outside them. */
static unsigned SceneKeyParted(const SceneKeyParts *was, const SceneKeyParts *now)
{
    unsigned bits = 0;

    for (unsigned i = 0; i < SCENE_PARTS; ++i)
        if (was->part[i] != now->part[i]) bits |= 1u << i;
    sSceneWhy |= bits ? bits : SCENE_WHY(KEY);
    return bits;
}

/* A kept scenery made under the key parts `was`, composed again for a key
 * of parts `now`: counted by the parts that differ. */
static void SceneKeyWhy(const SceneKeyParts *was, const SceneKeyParts *now)
{
    unsigned bits = SceneKeyParted(was, now);

    for (unsigned i = 0; i < SCENE_PARTS; ++i)
        if (bits & (1u << i)) ++sSceneAgain[i];
}

/* Composed with its key the same, or with nothing kept (`kept` false): what
 * the check found (`held`, sHoldWhy as it left it), else its measures. Over
 * the world the fill drops the kept scenery after saying why: no NEW then. */
static void SceneHeldWhy(bool kept, unsigned held)
{
    if (kept) sSceneWhy |= held ? held : SCENE_WHY(STILL);
    else if (!(sSceneWhy & SCENE_WHY_REASONS)) sSceneWhy |= SCENE_WHY(NEW);
}

static void SceneLayerVram(unsigned bg, unsigned *map, unsigned *mapBytes, unsigned *chars, unsigned *charBytes);

/*
 * A layer whose whole map names only clear tiles: whatever its scroll, it
 * draws nothing. Looked at again only when its map, or the characters the
 * map names, change. The battle's BG1 is such a layer between the moves, and
 * its scroll changed on every frame.
 */
static struct { uint32_t token; unsigned control, mode, start, bytes; bool blank; } sLayerBlank[4];

static bool SceneLayerBlank(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), mode = Reg(0) & 7, map, mapBytes, chars, charBytes;
    bool affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
    unsigned tileBytes = affine || (control & 0x80) ? 64 : 32, lo = ~0u, hi = 0;
    static uint32_t seen[1024 / 32];
    bool blank = true;

    if (mode > 2) return false;
    SceneLayerVram(bg, &map, &mapBytes, &chars, &charBytes);
    if (map + mapBytes > 0x10000) mapBytes = 0x10000 - map;
    if (sLayerBlank[bg].token && sLayerBlank[bg].control == control && sLayerBlank[bg].mode == mode
        && !VramChangedAfter(map, mapBytes, sLayerBlank[bg].token)
        && !VramChangedAfter(sLayerBlank[bg].start, sLayerBlank[bg].bytes, sLayerBlank[bg].token))
        return sLayerBlank[bg].blank;
    memset(seen, 0, sizeof(seen));
    for (unsigned i = 0; i < mapBytes && blank; i += affine ? 1 : 2)
    {
        unsigned tile = affine ? sMemory.vram[map + i] : (sMemory.vram[map + i] | sMemory.vram[map + i + 1] << 8) & 0x3ff;
        unsigned at = chars + tile * tileBytes;

        if (seen[tile >> 5] & (1u << (tile & 31))) continue;
        seen[tile >> 5] |= 1u << (tile & 31);
        if (tile < lo) lo = tile;
        if (tile > hi) hi = tile;
        if (at + tileBytes > 0x10000) { blank = false; break; }
        for (unsigned k = 0; k < tileBytes; k += 4)
            if (sMemory.vram[at + k] | sMemory.vram[at + k + 1] | sMemory.vram[at + k + 2] | sMemory.vram[at + k + 3])
            {
                blank = false;
                break;
            }
    }
    VramSyncRange(map, mapBytes);
    sLayerBlank[bg].token = sStats.frames + 1;
    sLayerBlank[bg].control = control;
    sLayerBlank[bg].mode = mode;
    sLayerBlank[bg].blank = blank;
    sLayerBlank[bg].start = lo > hi ? 0 : chars + lo * tileBytes;
    sLayerBlank[bg].bytes = lo > hi ? 0 : (hi - lo + 1) * tileBytes;
    if (sLayerBlank[bg].start + sLayerBlank[bg].bytes > 0x10000)
        sLayerBlank[bg].bytes = 0x10000 - sLayerBlank[bg].start;
    VramSyncRange(sLayerBlank[bg].start, sLayerBlank[bg].bytes);
    return blank;
}

/*
 * Whether the frame's scenery has nothing of the OBJ window: drawn over the
 * eyes instead (sObjWinSlotted, ObjWindowEye), ObjWindowDraw leaves it out of
 * the composes of the frame. SceneWindowKey keeps such a scenery apart from
 * one with no window on. nocache.txt bit 16777216: never kept with the window
 * on, as before (A/B frames).
 */
static bool SceneWindowApart(void)
{
#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 16777216u) return false;
#endif
    return sObjWin.ready && sObjWinSlotted;
}

static uint32_t SceneWindowKey(void)
{
    return sObjWinSlotted ? 0x5bd1e995u : 0u;
}

/*
 * The layers such a scenery cannot show at all: those only the OBJ window
 * adds (sObjWin.layers, none of them WINOUT's) that neither window 0 nor 1
 * shows - off, empty, or with a mask without them. A stat change's pattern on
 * BG1, with WIN0 on and empty: its scroll moved the key every frame. They leave
 * the key and what a kept scenery is checked by, as a layer the scene does not
 * draw; the compose itself goes on as ever, the masks leaving them out
 * rectangle by rectangle (WindowPartition). Only registers decide, all of them
 * in the key; windows or registers line by line are not looked into.
 */
static unsigned SceneWindowHidden(void)
{
    unsigned display = Reg(0), hidden = sObjWin.layers;

    if (!SceneWindowApart() || sRegLine || LineWindows()) return 0;
    for (unsigned w = 0; w < 2; ++w)
        if (display & (0x2000u << w))
        {
            int x0, x1, y0, y1;

            WindowRect(w, &x0, &x1, &y0, &y1);
            if (x0 < x1 && y0 < y1) hidden &= ~(Reg(0x48) >> (8 * w));
        }
    return hidden;
}

static uint32_t SceneCacheKey(int left, int top, int width, int height, unsigned exclude)
{
    uint32_t h = 0x811c9dc5u;
    uint32_t *part = sSceneKeyLast.part;
    union { float f; uint32_t u; } zoom = {sZoom}, offX = {sOffX}, offY = {sOffY};
    union { float f; uint32_t u; } fade = {sPaletteFade}, origin = {sLayerOrigin};
/* A term into the key and into its kind's part (SceneKeyWhy). */
#define SCENE_TERM(kind, v) (h = SceneCacheMix(h, (v)), part[kind] = SceneCacheMix(part[kind], (v)))

    for (unsigned i = 0; i < SCENE_PARTS; ++i)
        part[i] = 0x811c9dc5u;
    SCENE_TERM(SCENE_PART_REG, (uint32_t)left);
    SCENE_TERM(SCENE_PART_REG, (uint32_t)sSceneColumn);
    SCENE_TERM(SCENE_PART_REG, (uint32_t)top);
    SCENE_TERM(SCENE_PART_REG, (uint32_t)width);
    SCENE_TERM(SCENE_PART_REG, (uint32_t)height);
    SCENE_TERM(SCENE_PART_REG, exclude);
    SCENE_TERM(SCENE_PART_REG, zoom.u);
    SCENE_TERM(SCENE_PART_REG, offX.u);
    SCENE_TERM(SCENE_PART_REG, offY.u);
    SCENE_TERM(SCENE_PART_REG, (uint32_t)sViewX);
    SCENE_TERM(SCENE_PART_REG, (uint32_t)sViewY);
    SCENE_TERM(SCENE_PART_REG, origin.u);
    /* A palette fade is drawn as a tint (Blend), not in the palette. */
    SCENE_TERM(SCENE_PART_REG, fade.u);
    SCENE_TERM(SCENE_PART_REG, sPaletteFadeColor);
    /* DISPCNT, the BG registers, windows, mosaic and blending: 0x00-0x55;
     * the wide windows' edges past 255 beside them, 0x56-0x5D. Not the
     * scroll of a layer the scene does not draw: the text box's BG1 scrolls
     * on through the battle, and the scene was composed again every frame
     * for it - 4-5 ms of an Old 3DS's 2D battle frame in 3D. */
    unsigned drawn = (Reg(0) >> 8) & 15 & ~exclude;

    SCENE_TERM(SCENE_PART_REG, Reg(0));
    for (unsigned offset = 0x08; offset < 0x5e; offset += 2)
    {
        /* BGnHOFS/VOFS at 0x10 + 4n, BG2's and BG3's affine sets at 0x20 and 0x30. */
        unsigned bg = offset >= 0x10 && offset < 0x20 ? (offset - 0x10) / 4
                    : offset >= 0x20 && offset < 0x40 ? 2 + (offset - 0x20) / 16 : 4;

        if (bg < 4 && !(drawn & (1u << bg))) continue;
        if (bg < 4 && SceneLayerBlank(bg)) continue;
        if (offset == 0x52 && sSceneKeyNoAlpha) continue;
        if (offset == 0x54 && sSceneKeyNoBright) continue;
        SCENE_TERM(bg < 4 ? SCENE_PART_SCROLL : SCENE_PART_REG, Reg(offset));
    }
    SCENE_TERM(SCENE_PART_LINES, sLineCount);
    for (unsigned r = 0; r < 8; ++r)
        if ((sLineMask & (1u << r)) && (drawn & (1u << (r / 2))) && !SceneLayerBlank(r / 2))
        {
            SCENE_TERM(SCENE_PART_LINES, r);
            for (unsigned y = 0; y < sLineCount && y < LINE_MAX; y += 2)
                SCENE_TERM(SCENE_PART_LINES,
                           sLineScroll[r][y] | (y + 1 < LINE_MAX ? (uint32_t)sLineScroll[r][y + 1] << 16 : 0));
        }
    /*
     * What the battle's layers decide from more than the registers above and
     * the VRAM and palettes a kept scene is checked by (SceneKeptSince). Kept
     * across a change of one, the scene came out as the compose before it,
     * while what is composed again beside it every frame - the mono battle's
     * columns past its kept scenery (BattleSceneComposeMono), the edges - came
     * out as the compose after it:
     * - whether BG3 holds a move's background (BattleTerrainSplit,
     *   BattlePicture);
     * - whether a line layer is drawn from its texture or walked flat
     *   (DrawLineBg, LayerDrawable): its texture made late, short of VRAM,
     *   or not whole for a frame;
     * - whether a large layer is drawn from its texture or walked
     *   (DrawBattleBgTex): the same at the scene's scale, but a scene made one
     *   way is not kept as made the other. Only that: a layer asked for and not
     *   ready is walked as one not asked for, so a stationary layer short of
     *   its surface is not composed again each time the asking comes and goes.
     *   What is left of its choice, a picture, is the registers' and the VRAM's.
     * Not here: whether a 32-column layer wraps into the sides (sPanorama).
     * It is measured every few frames (MeasureStill), by the compose, on the
     * frames it walks the layer; a kept scene is checked by the layers its
     * compose asked about, measured where that compose would measure them
     * (StillHolds). Measured here, a layer the windows hid from the compose
     * moved the 8-frame turn.
     */
    if (sBattle && !(Reg(0) & 128))
    {
        unsigned mode = Reg(0) & 7, lines = LineBackgrounds() & drawn, large = sBattleLayerWanted & drawn;

        SCENE_TERM(SCENE_PART_DRAWABLE, (drawn & 8) && CtrBattleBg_MoveBgShown() ? 0x3du : 0x3cu);
        for (unsigned bg = 1; bg < 4; ++bg)
        {
            /* The layers Layers walks as text; the boxes are DrawBattleTextLayer's. */
            if (!(drawn & (1u << bg)) || (mode != 0 && !(mode == 1 && bg < 2)) || (BattleBoxLayers() & (1u << bg)))
                continue;
            if (lines & (1u << bg))
                SCENE_TERM(SCENE_PART_DRAWABLE, 0x40u | (bg << 1) | (LayerDrawable(bg) ? 1u : 0u));
            if ((large & (1u << bg)) && BattleLayerReady(bg))
                SCENE_TERM(SCENE_PART_DRAWABLE, 0x48u | (bg << 1));
        }
        /*
         * The scenery's own version (battle_bg.c): a terrain, its entry
         * picture or a move's background loaded. Added to the terms above,
         * never instead of them - those still see every write that comes
         * without a load (an animation's own) - so a load can only compose
         * the scenery again, never keep it.
         */
        SCENE_TERM(SCENE_PART_VERSION, CtrBattleBg_SceneryVersion());
    }
    /* BLDALPHA line by line (ComposeAlphaBands), as the register above. */
    SCENE_TERM(SCENE_PART_LINES, sLineAlphaOn);
    if (sLineAlphaOn && !sSceneKeyNoAlpha)
        for (unsigned y = 0; y < CTR_GBA_LINES; y += 2)
            SCENE_TERM(SCENE_PART_LINES, sLineAlpha[y] | (uint32_t)sLineAlpha[y + 1] << 16);
#undef SCENE_TERM
    return h;
}

/*
 * The key of the 3D battle's scenery, by what its compose reads. BLDALPHA and
 * BLDY reach a background only as a first target (Blend), as in the mono
 * battle's keeper (BattleSceneComposeMono). So with the sprites out of the
 * scenery and every first target out of it too - left to another pass, or a
 * layer only the OBJ window shows (SceneWindowHidden) - a blend stepping is no
 * change of it: a stat change's pattern fades in and out every four frames,
 * and each step composed the whole scenery again. BLDCNT stays in the key. The
 * bands of BLDALPHA by line look at first targets not drawn too, so a hidden
 * one keeps the weights in the key there. nocache.txt bit 67108864: the whole
 * key, as before.
 */
static uint32_t SceneViewKey(int left, int top, int width, int height, unsigned seen)
{
    bool weightless = (seen & 16u) && !(Reg(0x50) & 63u & ~seen)
                   && !(sLineAlphaOn && (Reg(0x50) & SceneWindowHidden()));
    uint32_t key;

#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 67108864u) weightless = false;
#endif
    sSceneKeyNoAlpha = sSceneKeyNoBright = weightless;
    key = SceneCacheKey(left, top, width, height, seen) ^ SceneWindowKey();
    sSceneKeyNoAlpha = sSceneKeyNoBright = false;
    return key;
}

/* The VRAM a layer's picture is made of: its map and its characters. */
static void SceneLayerVram(unsigned bg, unsigned *map, unsigned *mapBytes, unsigned *chars, unsigned *charBytes)
{
    unsigned control = Reg(8 + bg * 2), size = control >> 14, mode = Reg(0) & 7;
    bool affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);

    *map = ((control >> 8) & 31) * 0x800;
    *chars = ((control >> 2) & 3) * 0x4000;
    *mapBytes = affine ? (16u << size) * (16u << size) : 0x800u * (size == 0 ? 1 : size == 3 ? 4 : 2);
    *charBytes = 0x10000 - *chars;
}

/*
 * The characters a scene's layers can show, kept or empty: from the lowest to
 * the highest tile their whole maps name (SceneEmptyChars), not all of VRAM
 * from their base. The voxel battle's empty layers share their character block
 * with the text box, whose printing changed it nearly every frame: the empty
 * scene was composed again in two frames of three.
 */
static struct { unsigned start, bytes, control, mode; uint32_t token; } sSceneEmptyChars[4];

static void SceneEmptyChars(unsigned exclude)
{
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        unsigned map, mapBytes, chars, charBytes, control = Reg(8 + bg * 2), mode = Reg(0) & 7;
        bool affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
        unsigned tileBytes = affine || (control & 0x80) ? 64 : 32, lo = ~0u, hi = 0;

        if (!(Reg(0) & (0x100u << bg)) || (exclude & (1u << bg)))
        {
            sSceneEmptyChars[bg].token = 0;
            continue;
        }
        SceneLayerVram(bg, &map, &mapBytes, &chars, &charBytes);
        if (map + mapBytes > 0x10000)
            mapBytes = 0x10000 - map;
        /* The same map as at the last look: the same tiles named. */
        if (sSceneEmptyChars[bg].token && sSceneEmptyChars[bg].control == control
            && sSceneEmptyChars[bg].mode == mode && !VramChangedAfter(map, mapBytes, sSceneEmptyChars[bg].token))
            continue;
        sSceneEmptyChars[bg].start = sSceneEmptyChars[bg].bytes = 0;
        sSceneEmptyChars[bg].control = control;
        sSceneEmptyChars[bg].mode = mode;
        sSceneEmptyChars[bg].token = sStats.frames + 1;
        if (affine)
            for (unsigned i = 0; i < mapBytes; ++i)
            {
                unsigned tile = sMemory.vram[map + i];

                if (tile < lo) lo = tile;
                if (tile > hi) hi = tile;
            }
        else
            for (unsigned i = 0; i + 1 < mapBytes; i += 2)
            {
                unsigned tile = (sMemory.vram[map + i] | sMemory.vram[map + i + 1] << 8) & 0x3ff;

                if (tile < lo) lo = tile;
                if (tile > hi) hi = tile;
            }
        if (lo > hi || chars + lo * tileBytes >= 0x10000)
            continue;
        sSceneEmptyChars[bg].start = chars + lo * tileBytes;
        sSceneEmptyChars[bg].bytes = (hi - lo + 1) * tileBytes;
        if (sSceneEmptyChars[bg].start + sSceneEmptyChars[bg].bytes > 0x10000)
            sSceneEmptyChars[bg].bytes = 0x10000 - sSceneEmptyChars[bg].start;
    }
}

/* The empty scene's layers unchanged since `token`: their maps, and the
 * characters those name. Whether a layer draws anything does not depend on
 * its palette, which is not looked at. */
static bool SceneHoldsSince(unsigned exclude, uint32_t token)
{
    /* A bitmap mode's picture is not reached through map entries; and with
     * sprites in the OBJ window, those shape it, unless the window is left out
     * of the scenery (SceneWindowApart). Not DISPCNT's OBJ window bit alone:
     * the battle keeps it on throughout (0xbf40) with no such sprite, and no
     * battle scene was ever kept. */
    if ((Reg(0) & 7) > 2 || (sObjWin.ready && !SceneWindowApart()))
    {
        sHoldWhy = SCENE_WHY(PATH);
        return false;
    }
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        unsigned map, mapBytes, chars, charBytes;

        if (!(Reg(0) & (0x100u << bg)) || (exclude & (1u << bg)))
            continue;
        SceneLayerVram(bg, &map, &mapBytes, &chars, &charBytes);
        if (VramChangedAfter(map, mapBytes, token)
            || (sSceneEmptyChars[bg].bytes
                && VramChangedAfter(sSceneEmptyChars[bg].start, sSceneEmptyChars[bg].bytes, token)))
        {
            sHoldWhy = SCENE_WHY(VRAM);
            return false;
        }
    }
    return true;
}

/*
 * The kept scene still holds: the palettes, and the layers as SceneHoldsSince
 * looks at them. A layer's picture is its map and the characters that map
 * names, so with the map unchanged only those characters can change it; all
 * of VRAM from the character base was 48 blocks compared every battle frame.
 */
/*
 * The BG palette banks a layer's picture can take its colours from: all
 * sixteen for a 256-colour or affine layer, else those its whole map names
 * (bits 12-15 of each entry), looked at again only when that map changes.
 * One bank changing - Surf turns its wave's colours every four frames, a
 * monbg copy gets the faded colours of its Pokemon each frame - recomposed
 * the whole kept scenery, terrain and all, for the one layer that uses it.
 */
static struct { uint32_t token; unsigned control, mode, banks; bool tiled; } sLayerBanks[4];

static unsigned LayerBanks(unsigned bg)
{
    unsigned control = Reg(8 + bg * 2), mode = Reg(0) & 7, map, mapBytes, chars, charBytes, banks = 0;
    bool affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);

    if (affine || mode > 2 || (control & 0x80))
    {
        sLayerBanks[bg].tiled = false;
        return 0xffffu;
    }
    SceneLayerVram(bg, &map, &mapBytes, &chars, &charBytes);
    if (map + mapBytes > 0x10000) mapBytes = 0x10000 - map;
    if (sLayerBanks[bg].token && sLayerBanks[bg].control == control && sLayerBanks[bg].mode == mode
        && sTilemapBankStamp <= sLayerBanks[bg].token && sBgOtherStamp <= sLayerBanks[bg].token
        && !VramChangedAfter(map, mapBytes, sLayerBanks[bg].token))
        return sLayerBanks[bg].banks;
    sLayerBanks[bg].tiled = false;
    for (unsigned i = 0; i + 1 < mapBytes; i += 2)
    {
        banks |= 1u << (sMemory.vram[map + i + 1] >> 4);
        if (TilemapBankAt(map + i, sMemory.vram[map + i] | (unsigned)sMemory.vram[map + i + 1] << 8))
            sLayerBanks[bg].tiled = true;
    }
    VramSyncRange(map, mapBytes);
    sLayerBanks[bg].token = sStats.frames + 1;
    sLayerBanks[bg].control = control;
    sLayerBanks[bg].mode = mode;
    sLayerBanks[bg].banks = banks;
    return banks;
}

/*
 * The tile banks' colours as they were at `token`, as far as `layers` show them.
 * Their colours follow the map's (UpdateTileBankPalettes), so any palette the
 * game turns moves them - Surf's wave, every four frames - though a battle's
 * layer reads a bank only for an entry a field map left where its own names the
 * same (TilemapBankAt): the text and the scenery were composed again for the
 * colours of a map nothing showed. A layer with such an entry goes by them, but
 * for a terrain of 64 columns, which reads none in its walk or its texture
 * (sCellBanked); a line layer's texture does. Counted once a frame of a change,
 * at its first look. nocache.txt bit 268435456: any change, as before (A/B
 * frames).
 */
static struct { uint32_t frame; unsigned held, reached; } sBankColours;

static bool BankColoursHold(unsigned layers, uint32_t token)
{
    bool hold = true;

    if (sBankColourStamp <= token) return true;
#ifdef CTR_AUDIT_BITS
    if (CtrDumpOff() & 268435456u) hold = false;
#endif
    for (unsigned bg = 0; bg < 4 && hold; ++bg)
        if (layers & (1u << bg))
        {
            LayerBanks(bg);
            hold = !sLayerBanks[bg].tiled || (BattleTerrainSplit(bg) && (Reg(8 + bg * 2) & 0x4000)
                                              && !(LineBackgrounds() & (1u << bg)));
        }
    if (sBankColourStamp == sStats.frames + 1 && sBankColours.frame != sStats.frames + 1)
    {
        sBankColours.frame = sStats.frames + 1;
        ++*(hold ? &sBankColours.held : &sBankColours.reached);
    }
    return hold;
}

/* The palettes of the layers not in `exclude` unchanged since `token`. A
 * blank layer shows no colour (its map and characters are SceneHoldsSince's
 * to look after); the tile banks' palettes are any layer's. */
static bool ScenePalettesHold(unsigned exclude, uint32_t token)
{
    unsigned display = Reg(0), banks = 0, shown = 0;

    if (sBgOtherStamp > token)
    {
        sHoldWhy = SCENE_WHY(STAMP);
        return false;
    }
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((display & (0x100u << bg)) && !(exclude & (1u << bg)) && !SceneLayerBlank(bg))
        {
            shown |= 1u << bg;
            banks |= LayerBanks(bg);
        }
    if (!BankColoursHold(shown, token))
    {
        sHoldWhy = SCENE_WHY(STAMP);
        return false;
    }
    for (unsigned b = 0; b < 16; ++b)
        if ((banks & (1u << b)) && sBgBankStamp[b] > token)
        {
            sHoldWhy = SCENE_WHY(PALETTE);
            return false;
        }
    return true;
}

static bool SceneKeptSince(unsigned exclude, uint32_t token)
{
    sHoldWhy = 0;
    if (!ScenePalettesHold(exclude, token))
        return false;
    if (sTilemapBankStamp > token)
    {
        sHoldWhy = SCENE_WHY(STAMP);
        return false;
    }
    return SceneHoldsSince(exclude, token);
}

static bool SceneCacheHolds(unsigned exclude)
{
    return SceneKeptSince(exclude, sSceneCacheToken);
}

/* What the scenery was made of, compared now: so a change and a change back
 * before the next look is still a change (see SyncBlock). */
static void SceneCacheSync(unsigned exclude)
{
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        unsigned map, mapBytes, chars, charBytes;

        if (!(Reg(0) & (0x100u << bg)) || (exclude & (1u << bg)))
            continue;
        SceneLayerVram(bg, &map, &mapBytes, &chars, &charBytes);
        VramSyncRange(map, mapBytes);
        VramSyncRange(chars, charBytes);
    }
}
