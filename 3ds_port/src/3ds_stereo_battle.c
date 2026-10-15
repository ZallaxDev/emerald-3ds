/*
 * The stereoscopic pictures in battle (3ds_stereo_mon.h): the Pokémon's
 * fronts and backs, the trainers' fronts and the player's back drawn as
 * their stacks of depth planes, where the sprite is and with its own
 * palette, so that the compositor's 3D battle stands them in the scene with
 * a depth of their own rather than as flat cards.
 *
 * The game side names each battler position's sprite (CtrStereoBattle_
 * SetSlot); here each frame finds it in OAM by the tile its picture is at,
 * reads which picture and frame OBJ VRAM holds - a frame of the position's
 * buffer (the Pokémon, or the trainer who stood there), or of the player's
 * back sheet - and composes the matching planes into one texture, once per
 * change of picture, frame, palette or plane count. The buffer's planes are
 * the Pokémon's or the trainer's: whichever's own picture (the game's,
 * handed over with the ids) has the buffer's silhouette; the planes files
 * carry no outline of their own. The compositor leaves those OAM entries out and calls
 * CtrStereoBattle_Draw where it would have drawn them, per eye: each plane
 * a card shifted by its depth, as 3ds_stereo_mon.c draws them: no turn and
 * no sway, each card at its own whole-pixel scale, so the texels stay crisp.
 */
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <string.h>
#include "3ds_platform.h"
#include "3ds_video.h"
#include "3ds_log.h"
#include "3ds_stereo_mon.h"

#define DIM CTR_STEREO_MON_DIM
#define FRAME_BYTES CTR_STEREO_BATTLE_FRAME_BYTES
#define BUFFER_FRAMES CTR_STEREO_BUFFER_FRAMES
#define BACK_FRAMES CTR_STEREO_BACK_FRAMES
#define SLOTS CTR_STEREO_SLOTS
/* One 128x512 RGBA5551 texture of 16 tiles (128 KiB of linear memory, made
 * once and kept: the emulator's texture cache keys surfaces by address,
 * and a fresh texture over memory a freed one used was read with the old
 * one's layout), each position with tiles of its own: eight planes each
 * for the two of a single battle, four each for the four of a double, so
 * that no position ever writes over another's. Each tile is sampled half a texel inside its edges: filtered,
 * an edge texel would take in the neighbouring tile's. */
#define TEX_TILES 16
#define GROUPS_MAX 8
#define GROUPS_MIN 2
#define TEX_W (2 * DIM)
#define TEX_H 512
/* Total stack depth in sprite pixels, and the plane that
 * lies at the sprite's own depth (3ds_stereo_mon.c). */
#define DEPTH_SPAN 14.4f /* Original ten-plane span, independent of plane count. */
#define PIVOT_FRACTION 0.62f
/* The stack's depth on screen is OPTIONS' (3ds_extras.c): "MON PLANES",
 * the screen pixels per eye at the slider's top between the farthest and
 * the nearest plane (SPREAD_KEY), and "PLANES SIT", where the stack stands
 * against the sprite's own depth (BATTLE_SPRITE_DEPTH in 3ds_video.c):
 * behind it (the nearest plane at the sprite's depth), around it, or in
 * front of it (the farthest plane there). The world's 3D DEPTH no longer
 * touches it, so the two can be set against each other. Much past three
 * pixels the planes tear apart at the scene's magnification. */
#define SPREAD_KEY "battle_plane_spread"
#define PIVOT_KEY "battle_plane_pivot"
static const float sSpreads[] = {1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
static const float sPivots[] = {1.0f, PIVOT_FRACTION, 0.0f};
/* Pixels of the planes behind carried on each card (CtrStereoMon_ComposeTile):
 * the gap a card's shift opens shows those, not the scene. */
#define SKIRT 2
/* A back's stack (a Pokémon's or the player's) at this much of a front's
 * depth: seen from behind and large at the bottom of the scene, it tore
 * apart at a front's (BACK_DEPTH in 3ds_stereo_mon.c). */
#define BACK_DEPTH 0.6f
#define PLANES_KEY "stereo_planes"
#define ENABLED_KEY "battle_mon_3d"

/* One picture's planes from a file, with the frames it has. The reads are
 * a thread's (PlanesFor): `pending` while it has the entry, `ok` once the
 * planes are in. */
typedef struct
{
    uint16_t id;
    unsigned frames;
    bool ok, failed;        /* failed: read, nothing there; not asked again */
    volatile bool pending;
    uint8_t *planes;        /* frames * FRAME_BYTES */
} Planes;

/* What OBJ VRAM was found to hold. */
enum { SHOWS_NOTHING, SHOWS_BUFFER, SHOWS_BACK };

typedef struct
{
    bool set, player;
    unsigned tileNum;
    /* The position's picture buffer and, on the player's side, the player's
     * back sheet, copied here. */
    unsigned bufferFrames, backFrames;
    uint8_t buffer[BUFFER_FRAMES][FRAME_BYTES];
    uint8_t *back;          /* BACK_FRAMES * FRAME_BYTES, player slots only */
    /* The planes the buffer could be showing, and the back's. */
    Planes mon, trainer, playerBack;
    /* The first frame of the Pokémon's and the trainer's own pictures
     * (SetSlot), whether each was given; which of mon and trainer the
     * buffer holds (by silhouette), or NULL, and the candidates' state it
     * was decided on. */
    uint8_t monPicture[FRAME_BYTES], trainerPicture[FRAME_BYTES];
    bool hasMonPicture, hasTrainerPicture;
    const Planes *bufferPlanes;
    uint32_t silhouetteKey;
    /* This frame: the OAM entry (-1 none), what it shows and which frame,
     * the sprite's place and priority. */
    int oam, frame;
    unsigned shows;
    /* Bytes of the frame's tail OBJ VRAM holds as zero: a fainting Pokémon
     * is cut off a tile row (256 bytes) at a time from the bottom as it
     * slides down (SpriteCB_AnimFaintOpponent), the buffer itself zeroed. */
    unsigned cut;
    int x, y;
    unsigned priority;
    /* The sprite's own affine matrix (a squish, a bounce, a lean, a turn)
     * as the GBA holds it, target to texture; the identity when plain. */
    float ma, mb, mc, md;
    /* What the texture holds for it: the planes, frame, groups and tile base. */
    bool composed;
    const Planes *composedPlanes;
    int composedFrame;
    unsigned composedGroups, composedBase, composedShows, composedCut;
    uint16_t composedPal[16];
} Slot;

static Slot sSlots[SLOTS];
static uint8_t sMonPlanes[SLOTS][BUFFER_FRAMES * FRAME_BYTES];
static uint8_t sTrainerPlanes[SLOTS][FRAME_BYTES];
static uint8_t sBackSheets[2][BACK_FRAMES * FRAME_BYTES];
static uint8_t sBackPlanes[2][BACK_FRAMES * FRAME_BYTES];
static C3D_Tex sTex;
static bool sTexAllocated, sTexFailed;
static Tex3DS_SubTexture sTiles[TEX_TILES];
static unsigned sLogged;

/* The texture made at boot, while the linear heap still has a run of
 * 128 KiB: made at a battle's start it failed on hardware once the voxel
 * world had taken the heap (the battle then had
 * no planes at all). */
static bool TextureReady(void);
void CtrStereoBattle_Reserve(void)
{
    /* The two files' indexes read now, not on a battle's first frame; a
     * build with no planes in either keeps its 128 KiB. */
    bool any = CtrStereoMon_Load();

    if (CtrStereoMon_BacksHeader() != NULL) any = true;
    if (any) TextureReady();
}

bool CtrStereoBattle_Enabled(void)
{
    return CtrSettings_GetInt(ENABLED_KEY, 1) != 0;
}

static void SlotsInit(void)
{
    static bool done;

    if (done) return;
    done = true;
    for (unsigned i = 0; i < SLOTS; ++i)
    {
        sSlots[i].mon.planes = sMonPlanes[i];
        sSlots[i].trainer.planes = sTrainerPlanes[i];
        sSlots[i].player = (i & 1) == 0;
        if (sSlots[i].player)
        {
            sSlots[i].back = sBackSheets[i / 2];
            sSlots[i].playerBack.planes = sBackPlanes[i / 2];
        }
        sSlots[i].oam = -1;
    }
}

/*
 * The planes files are read on a thread of their own: a battle's first
 * frame asks for every position's planes at once, and the romfs reads took
 * the game thread well over a second on hardware (the stall at every
 * battle's start). The game thread queues
 * (Planes, id) and goes on; the sprite is drawn flat until the planes are
 * in. Without the thread (no memory for its stack) the read is done here.
 */
typedef struct { Planes *p; uint16_t id; unsigned capacity; uint64_t at; } Request;
#define REQUESTS 16
static Request sRequests[REQUESTS];
static unsigned sRequestHead, sRequestTail;    /* the game thread adds at the tail */
static CtrLock sRequestLock;
static LightEvent sRequestEvent;
static int sReaderState;                        /* 0 not tried, 1 running, -1 none */

/* The entry read into `p` (the backs file, the pack). */
static void PlanesRead(Planes *p, uint16_t id, unsigned capacityFrames)
{
    bool ok;
    unsigned frames = 0;

    if (id & 0xC000)
        ok = CtrStereoMon_BackPlanesFor(id, p->planes, capacityFrames * FRAME_BYTES, &frames);
    else
    {
        const CtrStereoMonHeader *h = CtrStereoMon_Header();

        ok = h != NULL && capacityFrames >= h->frames && CtrStereoMon_PlanesFor(id, p->planes, capacityFrames * FRAME_BYTES);
        if (ok) frames = h->frames;
    }
    if (!ok) CtrLog_Write(CTR_LOG_VIDEO, "stereo battle: no planes for id %04x", id);
    /* The planes before the flags: the unlock orders the writes. */
    CtrLock_Lock(&sRequestLock);
    p->frames = ok ? frames : 0;
    p->ok = ok;
    p->failed = !ok;
    p->pending = false;
    CtrLock_Unlock(&sRequestLock);
}

static void Reader(void *arg)
{
    (void)arg;
    for (;;)
    {
        Request r;
        bool any;

        LightEvent_Wait(&sRequestEvent);
        for (;;)
        {
            CtrLock_Lock(&sRequestLock);
            any = sRequestHead != sRequestTail;
            if (any) r = sRequests[sRequestHead++ % REQUESTS];
            CtrLock_Unlock(&sRequestLock);
            if (!any) break;
            /* NULL: the game thread took it back (PlanesFor). */
            if (r.p) PlanesRead(r.p, r.id, r.capacity);
        }
    }
}

static bool ReaderReady(void)
{
    if (sReaderState == 0)
    {
        CtrLock_Init(&sRequestLock);
        LightEvent_Init(&sRequestEvent, RESET_ONESHOT);
        sReaderState = CtrPlatform_StartThread(Reader, NULL, 16 * 1024, 1) ? 1 : -1;
        CtrLog_Write(CTR_LOG_VIDEO, "stereo battle: planes reader thread %s", sReaderState > 0 ? "on" : "NOT started");
    }
    return sReaderState > 0;
}

/* The planes of `id` into `p` (a front's from the fronts file, anything
 * else from the backs file), once per id; a read already on its way is
 * left to finish before another id is asked for. */
/* A request the thread has not taken within a second (it never got the
 * core, or is starved): taken back and read here, and the thread given up
 * on. One the thread holds is left to finish. */
#define READ_WAIT_TICKS (SYSCLOCK_ARM11 / 1)
static bool TakeBack(Planes *p)
{
    bool taken = false;

    CtrLock_Lock(&sRequestLock);
    for (unsigned i = sRequestHead; i != sRequestTail; ++i)
    {
        Request *r = &sRequests[i % REQUESTS];

        if (r->p == p)
        {
            if (svcGetSystemTick() - r->at > READ_WAIT_TICKS)
            {
                r->p = NULL;
                p->pending = false;
                taken = true;
            }
            break;
        }
    }
    CtrLock_Unlock(&sRequestLock);
    if (taken)
    {
        if (sReaderState > 0)
            CtrLog_Write(CTR_LOG_ERROR, "stereo battle: the planes reader thread never took a request; reading inline from now on");
        sReaderState = -1;
    }
    return taken;
}

static void PlanesFor(Planes *p, uint16_t id, unsigned capacityFrames)
{
    if (p->pending)
    {
        if (!TakeBack(p)) return;
        /* Taken back: the read it was waiting for, here. */
        PlanesRead(p, p->id, capacityFrames);
        return;
    }
    if (p->id == id && (p->ok || p->failed || id == 0)) return;
    p->id = id;
    p->ok = false;
    p->failed = false;
    p->frames = 0;
    if (id == 0 || p->planes == NULL) return;
    if (ReaderReady())
    {
        bool queued;

        CtrLock_Lock(&sRequestLock);
        queued = sRequestTail - sRequestHead < REQUESTS;
        if (queued)
        {
            sRequests[sRequestTail++ % REQUESTS] = (Request){p, id, capacityFrames, svcGetSystemTick()};
            p->pending = true;
        }
        CtrLock_Unlock(&sRequestLock);
        if (queued)
        {
            LightEvent_Signal(&sRequestEvent);
            return;
        }
    }
    PlanesRead(p, id, capacityFrames);
}

/* How many bytes of a frame's tail `vram` holds as zero where `tiles` is
 * not, in whole tile rows; the rest equal. 0 when the frame is not shown
 * so (a whole picture is an exact match first). */
static unsigned CutOf(const uint8_t *vram, const uint8_t *tiles)
{
    unsigned same = 0;

    while (same < FRAME_BYTES && vram[same] == tiles[same]) ++same;
    same &= ~255u;
    if (same == 0 || same == FRAME_BYTES) return 0;
    for (unsigned i = same; i < FRAME_BYTES; ++i)
        if (vram[i] != 0) return 0;
    if (memcmp(vram, tiles, same) != 0) return 0;
    return FRAME_BYTES - same;
}

/* Whether a 4bpp frame has the silhouette of a picture's (opaque where its
 * pixel is not 0, both in GBA tile order), whose planes are in. The game's
 * own picture, not the planes: they carry no outline. Pixel values may
 * differ (Spinda's spots), the silhouette may not; a Substitute's doll, a
 * Transform's other species or a trainer's picture do not match. */
static bool SilhouetteMatches(const uint8_t *tiles, const uint8_t *picture, bool given, const Planes *p)
{
    if (!p->ok || !given) return false;
    for (unsigned i = 0; i < FRAME_BYTES; ++i)
    {
        uint8_t a = tiles[i], b = picture[i];

        if (((a & 15) != 0) != ((b & 15) != 0) || ((a >> 4) != 0) != ((b >> 4) != 0)) return false;
    }
    return true;
}

static const CtrStereoMonHeader *HeaderOf(const Planes *p)
{
    return (p->id & 0xC000) ? CtrStereoMon_BacksHeader() : CtrStereoMon_Header();
}

void CtrStereoBattle_SetSlot(unsigned position, unsigned tileNum, const uint8_t *buffer, unsigned bufferFrames,
                             uint16_t monId, uint16_t trainerId, const uint8_t *monPicture,
                             const uint8_t *trainerPicture, uint16_t playerId, const uint8_t *backSheet,
                             unsigned backFrames)
{
    Slot *s;
    bool bufferChanged, picturesChanged = false;

    SlotsInit();
    if (position >= SLOTS) return;
    s = &sSlots[position];
    if (buffer == NULL || bufferFrames == 0)
    {
        s->set = false;
        return;
    }
    if (bufferFrames > BUFFER_FRAMES) bufferFrames = BUFFER_FRAMES;
    bufferChanged = !s->set || s->bufferFrames != bufferFrames
                    || memcmp(s->buffer, buffer, bufferFrames * FRAME_BYTES) != 0;
    /* The buffer itself cut from the bottom (a faint): the whole picture is
     * kept, and OBJ VRAM is matched against it with the cut (Shown). */
    if (bufferChanged && s->set && s->bufferFrames == bufferFrames)
    {
        bool cutOnly = true;

        for (unsigned f = 0; f < bufferFrames && cutOnly; ++f)
            if (memcmp(s->buffer[f], buffer + f * FRAME_BYTES, FRAME_BYTES) != 0
                && CutOf(buffer + f * FRAME_BYTES, s->buffer[f]) == 0)
                cutOnly = false;
        if (cutOnly) bufferChanged = false;
    }
    if (bufferChanged)
    {
        memcpy(s->buffer, buffer, bufferFrames * FRAME_BYTES);
        s->bufferFrames = bufferFrames;
    }
    PlanesFor(&s->mon, monId, BUFFER_FRAMES);
    PlanesFor(&s->trainer, trainerId, 1);
    /* The pictures change with the ids, which the key below holds; copied
     * only when they do. */
    if ((monPicture != NULL) != s->hasMonPicture
        || (monPicture != NULL && memcmp(s->monPicture, monPicture, FRAME_BYTES) != 0))
    {
        s->hasMonPicture = monPicture != NULL;
        if (monPicture != NULL) memcpy(s->monPicture, monPicture, FRAME_BYTES);
        picturesChanged = true;
    }
    if ((trainerPicture != NULL) != s->hasTrainerPicture
        || (trainerPicture != NULL && memcmp(s->trainerPicture, trainerPicture, FRAME_BYTES) != 0))
    {
        s->hasTrainerPicture = trainerPicture != NULL;
        if (trainerPicture != NULL) memcpy(s->trainerPicture, trainerPicture, FRAME_BYTES);
        picturesChanged = true;
    }
    if (s->player && backSheet != NULL && backFrames != 0)
    {
        if (backFrames > BACK_FRAMES) backFrames = BACK_FRAMES;
        if (s->backFrames != backFrames || memcmp(s->back, backSheet, backFrames * FRAME_BYTES) != 0)
        {
            memcpy(s->back, backSheet, backFrames * FRAME_BYTES);
            s->backFrames = backFrames;
        }
        PlanesFor(&s->playerBack, playerId, BACK_FRAMES);
    }
    else
        s->backFrames = 0;
    /* Whose silhouette the buffer has: looked at again when it changes or
     * the candidates do (a read coming in, another id). */
    {
        uint32_t key = ((uint32_t)s->mon.id << 16) | s->trainer.id | (s->mon.ok ? 1u << 15 : 0) | (s->trainer.ok ? 1u << 14 : 0);

        if (bufferChanged || picturesChanged || !s->set || key != s->silhouetteKey)
        {
            s->silhouetteKey = key;
            s->bufferPlanes = NULL;
            if (SilhouetteMatches(s->buffer[0], s->monPicture, s->hasMonPicture, &s->mon)) s->bufferPlanes = &s->mon;
            else if (SilhouetteMatches(s->buffer[0], s->trainerPicture, s->hasTrainerPicture, &s->trainer))
                s->bufferPlanes = &s->trainer;
        }
    }
    s->set = true;
    s->tileNum = tileNum;
}

void CtrStereoBattle_ClearBattlers(void)
{
    SlotsInit();
    sLogged = 0;    /* a few lines per battle, not per session */
    for (unsigned i = 0; i < SLOTS; ++i)
    {
        sSlots[i].set = false;
        sSlots[i].oam = -1;
    }
}

/* The texture, made once; false without the linear memory for it. */
static bool TextureReady(void)
{
    if (sTexAllocated) return true;
    if (sTexFailed) return false;
    if (!C3D_TexInit(&sTex, TEX_W, TEX_H, GPU_RGBA5551))
    {
        sTexFailed = true;
        CtrLog_Write(CTR_LOG_ERROR, "stereo battle: no linear memory for a %ux%u texture (free=%lu)",
                     TEX_W, TEX_H, (unsigned long)linearSpaceFree());
        return false;
    }
    C3D_TexSetFilter(&sTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(sTex.data, 0, TEX_W * TEX_H * 2);
    C3D_TexFlush(&sTex);
    for (unsigned tile = 0; tile < TEX_TILES; ++tile)
    {
        unsigned x = (tile % 2) * DIM, y = (tile / 2) * DIM;

        sTiles[tile] = (Tex3DS_SubTexture){DIM, DIM, (x + 0.5f) / TEX_W, 1.0f - (y + 0.5f) / TEX_H,
                                           (x + DIM - 0.5f) / TEX_W, 1.0f - (y + DIM - 0.5f) / TEX_H};
    }
    sTexAllocated = true;
    CtrLog_Write(CTR_LOG_VIDEO, "stereo battle: a %ux%u texture (linear free=%lu)", TEX_W, TEX_H,
                 (unsigned long)linearSpaceFree());
    return true;
}

/* A 4bpp frame as 64x64 RGBA5551 pixels with the palette it is drawn with
 * (rows top to bottom, 0 transparent). */
static void Picture(const uint8_t *tiles, const uint16_t *pal, uint16_t *out)
{
    uint16_t colours[16];

    colours[0] = 0;
    for (unsigned i = 1; i < 16; ++i) colours[i] = CtrVideo_RGBA5551(pal[i]);
    for (unsigned y = 0; y < DIM; ++y)
        for (unsigned x = 0; x < DIM; ++x)
        {
            unsigned tile = (y / 8) * 8 + x / 8;
            uint8_t v = (tiles[tile * 32 + (y & 7) * 4 + (x & 7) / 2] >> ((x & 1) * 4)) & 15;

            out[y * DIM + x] = colours[v];
        }
}

/* The slot's planes into its tiles of the texture, with this palette. */
static void Compose(Slot *s, const Planes *p, const uint16_t *pal, unsigned groups, unsigned base)
{
    static uint16_t picture[DIM * DIM];
    static uint8_t cutTiles[FRAME_BYTES];
    const uint8_t *tiles = s->shows == SHOWS_BACK ? s->back + s->frame * FRAME_BYTES : s->buffer[s->frame];
    CtrStereoMonHeader hdr = *HeaderOf(p);

    hdr.frames = p->frames;
    if (s->cut)
    {
        memcpy(cutTiles, tiles, FRAME_BYTES - s->cut);
        memset(cutTiles + FRAME_BYTES - s->cut, 0, s->cut);
        tiles = cutTiles;
    }
    Picture(tiles, pal, picture);
    for (unsigned g = 0; g < groups; ++g)
    {
        unsigned tile = base + g;

        CtrStereoMon_ComposeTile(picture, p->planes, &hdr, (unsigned)s->frame, groups, g, sTex.data, TEX_W,
                                 (tile % 2) * DIM, (tile / 2) * DIM, SKIRT);
    }
    C3D_TexFlush(&sTex);
    s->composed = true;
    s->composedPlanes = p;
    s->composedFrame = s->frame;
    s->composedGroups = groups;
    s->composedBase = base;
    s->composedShows = s->shows;
    s->composedCut = s->cut;
    memcpy(s->composedPal, pal, sizeof(s->composedPal));
}

/* Which picture and frame OBJ VRAM holds at the slot's tile, with the
 * planes for it; SHOWS_NOTHING when none. */
static unsigned Shown(Slot *s, const uint8_t *vram, int *frame, const Planes **planes)
{
    s->cut = 0;
    for (unsigned f = 0; f < s->bufferFrames; ++f)
        if (s->bufferPlanes != NULL && f < s->bufferPlanes->frames && memcmp(vram, s->buffer[f], FRAME_BYTES) == 0)
        {
            *frame = (int)f;
            *planes = s->bufferPlanes;
            return SHOWS_BUFFER;
        }
    for (unsigned f = 0; f < s->backFrames && s->playerBack.ok && f < s->playerBack.frames; ++f)
        if (memcmp(vram, s->back + f * FRAME_BYTES, FRAME_BYTES) == 0)
        {
            *frame = (int)f;
            *planes = &s->playerBack;
            return SHOWS_BACK;
        }
    /* Fainting: the frame with its bottom cut off; the planes stay. */
    for (unsigned f = 0; f < s->bufferFrames; ++f)
    {
        unsigned cut;

        if (s->bufferPlanes == NULL || f >= s->bufferPlanes->frames) continue;
        cut = CutOf(vram, s->buffer[f]);
        if (cut == 0) continue;
        s->cut = cut;
        *frame = (int)f;
        *planes = s->bufferPlanes;
        return SHOWS_BUFFER;
    }
    return SHOWS_NOTHING;
}

unsigned CtrStereoBattle_Frame(const void *memory, bool battle, uint32_t skip[4])
{
    const CtrVideoMemory *mem = memory;
    unsigned found = 0, active = 0, groups, base = 0;
    int setting;
    const Planes *shownPlanes[SLOTS] = {NULL, NULL, NULL, NULL};

    SlotsInit();
    memset(skip, 0, 4 * sizeof(uint32_t));
    for (unsigned i = 0; i < SLOTS; ++i) sSlots[i].oam = -1;
    if (!battle || !CtrStereoBattle_Enabled() || !(mem->regs[0] & 0x1000) || !(mem->regs[0] & 0x40)) return 0;
    /* The sprites first: how many slots show, then the planes they share. */
    for (unsigned i = 0; i < SLOTS; ++i)
    {
        Slot *s = &sSlots[i];

        if (!s->set) continue;
        for (unsigned o = 0; o < 128 && s->oam < 0; ++o)
        {
            unsigned attr0 = mem->oam[o * 4], attr1 = mem->oam[o * 4 + 1], attr2 = mem->oam[o * 4 + 2];
            bool affine = (attr0 & 0x100) != 0, twice = affine && (attr0 & 0x200);
            int x, y, box = twice ? 2 * DIM : DIM, frame;
            const Planes *p;
            unsigned shows;

            float ma = 1.0f, mb = 0.0f, mc = 0.0f, md = 1.0f;

            /* A shown, plain 64x64 16-colour sprite at the picture's tile:
             * not hidden, blending, in mosaic, flipped or a window's. The
             * battlers' sprites are affine for their animations: the matrix
             * is the identity between them (the double-size box then holds
             * the picture in its middle); a squish, a bounce, a lean or a
             * turn the planes follow, each card through the same matrix. */
            if ((!affine && (attr0 & 0x200)) || (attr0 & 0x3c00) != 0 || (attr0 >> 14) != 0) continue;
            if ((attr1 >> 14) != 3 || (!affine && (attr1 & 0x3000)) || (attr2 & 1023) != s->tileNum) continue;
            if (affine)
            {
                unsigned m = ((attr1 >> 9) & 31) * 16;

                ma = (int16_t)mem->oam[m + 3] / 256.0f;
                mb = (int16_t)mem->oam[m + 7] / 256.0f;
                mc = (int16_t)mem->oam[m + 11] / 256.0f;
                md = (int16_t)mem->oam[m + 15] / 256.0f;
                if (fabsf(ma * md - mb * mc) < 0.00001f) continue;
            }
            shows = Shown(s, mem->vram + 0x10000 + s->tileNum * 32, &frame, &p);
            if (shows == SHOWS_NOTHING) continue;
            x = (int)(attr1 & 511);
            y = (int)(attr0 & 255);
            if (x + box > 512) x -= 512;
            if (y + box > 256) y -= 256;
            if (twice) { x += DIM / 2; y += DIM / 2; }
            s->oam = (int)o;
            s->shows = shows;
            s->frame = frame;
            s->x = x;
            s->y = y;
            s->ma = ma;
            s->mb = mb;
            s->mc = mc;
            s->md = md;
            s->priority = (attr2 >> 10) & 3;
            shownPlanes[i] = p;
            ++active;
        }
    }
    if (active == 0 || !TextureReady()) return 0;
    /* A double battle (a right-hand position in use) halves the planes. */
    setting = CtrSettings_GetInt(PLANES_KEY, GROUPS_MAX);
    groups = setting < GROUPS_MIN ? GROUPS_MIN : (unsigned)setting;
    if (groups > GROUPS_MAX) groups = GROUPS_MAX;
    if ((sSlots[2].set || sSlots[3].set) && groups > TEX_TILES / SLOTS) groups = TEX_TILES / SLOTS;
    for (unsigned i = 0; i < SLOTS; ++i)
    {
        Slot *s = &sSlots[i];
        const uint16_t *pal;
        unsigned attr2, spriteGroups;

        base = (sSlots[2].set || sSlots[3].set) ? i * (TEX_TILES / SLOTS) : (i & 1) * (TEX_TILES / 2);
        if (s->oam < 0) continue;
        attr2 = mem->oam[s->oam * 4 + 2];
        pal = mem->palette + (16 + (attr2 >> 12)) * 16;
        spriteGroups = HeaderOf(shownPlanes[i])->planes;
        if (spriteGroups > groups) spriteGroups = groups;
        if (!s->composed || s->composedPlanes != shownPlanes[i] || s->composedFrame != s->frame
            || s->composedGroups != spriteGroups || s->composedBase != base || s->composedShows != s->shows
            || s->composedCut != s->cut || memcmp(s->composedPal, pal, sizeof(s->composedPal)) != 0)
            Compose(s, shownPlanes[i], pal, spriteGroups, base);
        skip[s->oam >> 5] |= 1u << (s->oam & 31);
        ++found;
        if (sLogged < 8)
        {
            ++sLogged;
            CtrLog_Write(CTR_LOG_VIDEO, "stereo battle: slot %u id %04x frame %d as %u planes at OAM %d (%d,%d)", i,
                         shownPlanes[i]->id, s->frame, spriteGroups, s->oam, s->x, s->y);
        }
    }
    return found;
}

static Slot *ByOam(unsigned oam)
{
    for (unsigned i = 0; i < SLOTS; ++i)
        if (sSlots[i].oam == (int)oam && sSlots[i].composed) return &sSlots[i];
    return NULL;
}

bool CtrStereoBattle_Oam(unsigned oam, int *x, int *y, unsigned *priority)
{
    const Slot *s = ByOam(oam);

    if (!s) return false;
    if (x) *x = s->x;
    if (y) *y = s->y;
    if (priority) *priority = s->priority;
    return true;
}

void CtrStereoBattle_Veto(unsigned oam, uint32_t skip[4])
{
    Slot *s = ByOam(oam);

    if (s) s->oam = -1;
    skip[oam >> 5] &= ~(1u << (oam & 31));
}

void CtrStereoBattle_Draw(unsigned oam, float x, float y, float scale, float eyeParallax, float shiftScale,
                          bool smooth)
{
    static bool sSmooth;
    const Slot *s = ByOam(oam);
    const CtrStereoMonHeader *header;
    unsigned groups;
    float pivot, spacing, depth, spread;
    int setting;

    if (!s || !sTexAllocated || s->composedPlanes == NULL) return;
    header = HeaderOf(s->composedPlanes);
    if (header == NULL) return;
    if (smooth != sSmooth)
    {
        C2D_Flush();
        C3D_TexSetFilter(&sTex, smooth ? GPU_LINEAR : GPU_NEAREST, smooth ? GPU_LINEAR : GPU_NEAREST);
        sSmooth = smooth;
    }
    groups = s->composedGroups;
    setting = CtrSettings_GetInt(PIVOT_KEY, 1);
    pivot = sPivots[setting >= 0 && setting < (int)(sizeof(sPivots) / sizeof(sPivots[0])) ? setting : 1] * (groups - 1);
    spacing = DEPTH_SPAN / (float)(groups - 1);
    setting = CtrSettings_GetInt(SPREAD_KEY, 2);
    spread = sSpreads[setting >= 0 && setting < (int)(sizeof(sSpreads) / sizeof(sSpreads[0])) ? setting : 2];
    /* Screen pixels per eye per sprite pixel of the stack's depth. */
    depth = eyeParallax * (spread / DEPTH_SPAN) * shiftScale;
    if ((s->composedPlanes->id & 0xC000) == CTR_STEREO_ID_BACK || (s->composedPlanes->id & 0xC000) == CTR_STEREO_ID_PLAYER)
        depth *= BACK_DEPTH;
    {
        /* The sprite's matrix inverted (texture to target) about the box's
         * middle, with the target's scale: every card goes through it, the
         * eye's shift of each added on the target's axis after. Plain
         * sprites take the cheap path. */
        const bool plain = s->ma == 1.0f && s->mb == 0.0f && s->mc == 0.0f && s->md == 1.0f;
        const float det = s->ma * s->md - s->mb * s->mc;
        const float ia = s->md / det * scale, ib = -s->mb / det * scale, ic = -s->mc / det * scale, id = s->ma / det * scale;
        const float cx = x + DIM / 2.0f * scale, cy = y + DIM / 2.0f * scale;
        C3D_Mtx saved;

        /* On top of whatever view the caller draws in (the compositor's
         * base view carries the zoom and the offset in DrawObjects). */
        if (!plain) C2D_ViewSave(&saved);
        for (unsigned plane = 0; plane < groups; ++plane)
        {
            /* The plane's depth towards the viewer in sprite pixels. The left
             * eye sees what is nearer than the sprite shifted right. */
            float z = (plane - pivot) * spacing;
            C2D_Image image = {&sTex, &sTiles[s->composedBase + plane]};

            if (plain)
            {
                /* On whole pixels of the target: a card between two pixels
                 * is sampled unevenly, and composed at 2x (3ds_video.c
                 * MonCells) that is half a GBA pixel, the finest the eye
                 * shift can be. */
                C2D_DrawImageAt(image, roundf(x + depth * z), roundf(y), 0, NULL, scale, scale);
            }
            else
            {
                C3D_Mtx m, view;

                Mtx_Identity(&m);
                m.r[0] = FVec4_New(ia, ib, 0, roundf(cx - (ia + ib) * DIM / 2.0f + depth * z));
                m.r[1] = FVec4_New(ic, id, 0, roundf(cy - (ic + id) * DIM / 2.0f));
                Mtx_Multiply(&view, &saved, &m);
                C2D_ViewRestore(&view);
                C2D_DrawImageAt(image, 0, 0, 0, NULL, 1.0f, 1.0f);
            }
        }
        if (!plain) { C2D_Flush(); C2D_ViewRestore(&saved); }
    }
}
