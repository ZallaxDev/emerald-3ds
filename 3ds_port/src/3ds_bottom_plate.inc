/*
 * The menu's plates and its text styles (included by 3ds_bottom_ui.c).
 *
 * A plate is the battle menus' plate (scripts/gen_battle_art.py, plate()) at
 * any size and in any colour: a soft shadow under it, an ink outline with its
 * corners cut, a face shaded top to bottom with a darker band across its
 * lower part, a gloss over its top half, a thin light rim inside the edge and
 * the screen's faint scanlines. It may have a cream end on the left (slanted,
 * with three light stripes past it and a faint Poké Ball in it) and a light
 * head under a coloured top (the move plates' look). Focused, it glows
 * orange round its edge; pressed, it sinks SINK pixels and goes a step
 * darker.
 *
 * The script draws at four times the size and shrinks the result; here each
 * shape's edge coverage is worked out from its distance to the pixel instead
 * (a chamfered rectangle is the intersection of a few half-planes, so its
 * distance is the largest of theirs), and a Gaussian blur of an edge from
 * the normal distribution's integral: the same layers, in the same order,
 * at one sample a pixel.
 *
 * A plate is made once and kept (sPlates): each pixel as what is left of
 * whatever lies under it (t, 0..255) and what the plate adds over that (c,
 * RGB565), so drawing it again is a multiply and an add a pixel. Kept
 * packed (PlatePack): a column the same as the one before it is kept once,
 * and most of a plate's columns are its face's long middle, the same one
 * after another - a plate takes a third of its bytes or less.
 */

#define PLATE_SINK 2
/* Room round a plate for its ring and shadow: left, top, right, bottom. */
#define PLATE_ML 6
#define PLATE_MT 6
#define PLATE_MR 6
#define PLATE_MB 8

enum { PLATE_STATE_NORMAL, PLATE_STATE_FOCUS, PLATE_STATE_PRESSED };

typedef struct
{
    s16 w, h;
    s16 cream;          /* width of the cream end on the left, 0 for none */
    u8 cut;             /* the corners' cut */
    u8 slope;           /* the cream end's slant, in hundredths */
    u8 head;            /* rows of coloured top over a light face, 0 for none */
    u8 state;           /* PLATE_STATE_* */
    Rgb base;
} PlateSpec;

static const Rgb sPlateInk = {22, 28, 38};
static const Rgb sPlateRing = {248, 104, 48};

/* The normal distribution's integral, Phi(z), at z = i / 16 - 4. */
#define PHI_STEPS 129
static float sPhi[PHI_STEPS];

static void BuildPhi(void)
{
    for (int i = 0; i < PHI_STEPS; ++i)
        sPhi[i] = 0.5f * erfcf(-((float)i / 16.0f - 4.0f) / 1.41421356f);
}

/* A blurred edge's coverage at signed distance d (negative inside), blur sigma. */
static inline float Blurred(float d, float sigma)
{
    float z = -d / sigma, f;
    int i;

    if (z <= -4.0f)
        return 0.0f;
    if (z >= 4.0f)
        return 1.0f;
    f = (z + 4.0f) * 16.0f;
    i = (int)f;
    return sPhi[i] + (sPhi[i + 1] - sPhi[i]) * (f - i);
}

static inline float Clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* A pixel's coverage by a shape whose edge is at signed distance d. */
static inline float Cover(float d)
{
    return Clamp01(0.5f - d);
}

/* Distance from (u, v) to the chamfered rectangle (x, y, w, h, cut). */
static inline float ChamferDist(float u, float v, float x, float y, float w, float h, float c)
{
    float hw = w * 0.5f, hh = h * 0.5f;
    float du = fabsf(u - (x + hw)), dv = fabsf(v - (y + hh));
    float a = du - hw, b = dv - hh, k = (du + dv - (hw + hh - c)) * 0.70710678f;

    if (b > a) a = b;
    return k > a ? k : a;
}

typedef struct { float r, g, b; } Rgbf;

static inline Rgbf Mixf(Rgbf a, Rgbf b, float t)
{
    return (Rgbf){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

static inline Rgbf Fl(Rgb c)
{
    return (Rgbf){c.r, c.g, c.b};
}

/* Through colour stops (t ascending) at t. */
static Rgbf Ramp(const float *ts, const Rgbf *cs, int n, float t)
{
    if (t <= ts[0])
        return cs[0];
    for (int k = 0; k + 1 < n; ++k)
        if (t <= ts[k + 1])
            return Mixf(cs[k], cs[k + 1], (t - ts[k]) / (ts[k + 1] - ts[k] > 1e-6f ? ts[k + 1] - ts[k] : 1e-6f));
    return cs[n - 1];
}

/* One layer over the pixel so far: t what is left of the backdrop, c what is added. */
typedef struct { float t, r, g, b; } Px;

static inline void Paint(Px *p, Rgbf c, float a)
{
    if (a <= 0.0f)
        return;
    if (a > 1.0f)
        a = 1.0f;
    p->t *= 1.0f - a;
    p->r += (c.r - p->r) * a;
    p->g += (c.g - p->g) * a;
    p->b += (c.b - p->b) * a;
}

/* The faint Poké Ball's coverage at distance (dx, dy) from its centre. */
static float FaintBall(float dx, float dy, float r)
{
    float d = sqrtf(dx * dx + dy * dy);
    float ring = Cover(d - r) - Cover(d - (r - 2.5f));
    float band = Clamp01(0.5f - (fabsf(dy) - r * 0.11f)) * Cover(fabsf(dx) - r);
    float disc = Cover(d - r * 0.42f);
    float hole = Cover(d - r * 0.24f);
    float m = ring > band ? ring : band;

    if (disc > m)
        m = disc;
    return m * (1.0f - hole);
}

/*
 * The plate's picture, (w + PLATE_ML + PLATE_MR) x (h + PLATE_MT + PLATE_MB),
 * column by column, its top-left PLATE_ML, PLATE_MT out from the plate's. `oddRow`
 * says whether its first row is an odd one on screen (the scanlines). Its
 * rows [j0, j1) only: a plate made ahead is made a few rows a frame.
 */
static void RenderPlateRows(const PlateSpec *s, u8 *outT, u16 *outC, bool8 oddRow, int j0, int j1)
{
    static const Rgbf white = {255, 255, 255}, black = {0, 0, 0};
    const Rgbf base = Fl(s->base), ink = Fl(sPlateInk), ring = Fl(sPlateRing);
    const Rgbf creamTop = {252, 245, 234}, creamLow = {236, 224, 208}, creamBall = {222, 204, 188};
    const Rgbf shadowColour = {10, 50, 44};
    bool8 pressed = s->state == PLATE_STATE_PRESSED, focus = s->state == PLATE_STATE_FOCUS;
    int pw = s->w + PLATE_ML + PLATE_MR, ph = s->h + PLATE_MT + PLATE_MB;
    float w = s->w, h = s->h, c = s->cut, slope = s->slope / 100.0f;
    float oy = pressed ? PLATE_SINK : 0, hh = h - (pressed ? PLATE_SINK : 0);
    float shadowY = pressed ? PLATE_SINK - 0.5f : 3.5f;
    Rgbf top = Mixf(base, white, 0.22f), mid = base, low = Mixf(base, black, 0.16f), deep = Mixf(base, black, 0.30f);
    Rgbf faceStops[3] = {Mixf(base, white, 0.88f), Mixf(base, white, 0.84f), Mixf(base, white, 0.72f)};
    static const float faceTs[3] = {0, 0.6f, 1};
    static const float fadeTs[4] = {0, 0.55f, 0.8f, 1};
    static const Rgbf fadeCs[4] = {{255, 255, 255}, {90, 90, 90}, {40, 40, 40}, {170, 170, 170}};

    if (focus)
    {
        top = Mixf(top, white, 0.15f);
        mid = Mixf(base, white, 0.12f);
    }
    if (pressed)
    {
        top = mid;
        mid = low;
        low = deep;
    }
    if (j1 > ph)
        j1 = ph;
    for (int j = j0; j < j1; ++j)
    {
        float v = j - PLATE_MT + 0.5f, tRow = (v - oy) / hh;
        float creamEdge = s->cream + (oy + hh * 0.5f - v) * slope;
        float bandEdge0 = oy + hh * 0.64f, bandSlope = -hh * 0.06f / w;
        float gloss = 0.16f * (1.0f - Clamp01((v - oy) / (hh * 0.5f)));
        float fade = Ramp(fadeTs, fadeCs, 4, tRow).r / 255.0f;
        bool8 scan = ((j & 1) != 0) == (oddRow != 0);
        static const float gradTs[3] = {0, 0.45f, 1};
        Rgbf grad[3] = {top, mid, low};
        Rgbf face = Ramp(gradTs, grad, 3, tRow);
        Rgbf cream = tRow <= 0.6f ? creamTop : Mixf(creamTop, creamLow, Clamp01((tRow - 0.6f) / 0.4f));
        Rgbf headFace = s->head ? Ramp(faceTs, faceStops, 3, (v - (oy + s->head)) / (hh - s->head)) : black;
        float headCover = s->head ? Clamp01(v + 0.5f - (oy + s->head)) : 0.0f;
        float sep = s->head ? Clamp01((v + 0.5f < oy + s->head + 0.6f ? v + 0.5f : oy + s->head + 0.6f)
                                      - (v - 0.5f > oy + s->head - 0.6f ? v - 0.5f : oy + s->head - 0.6f)) : 0.0f;

        for (int i = 0; i < pw; ++i)
        {
            float u = i - PLATE_ML + 0.5f;
            float d = ChamferDist(u, v, 0, oy, w, hh, c);
            float covI = Cover(d + 1.2f);
            Px p = {1.0f, 0, 0, 0};

            Paint(&p, shadowColour, 0.55f * Blurred(ChamferDist(u, v, 0, shadowY, w, h - (pressed ? PLATE_SINK : 0), c), 1.23f));
            if (focus)
            {
                float dr = ChamferDist(u, v, -2.5f, oy - 2.5f, w + 5, hh + 5, c + 1);

                Paint(&p, ring, 0.9f * Blurred(dr, 1.62f));
                Paint(&p, ring, Cover(dr));
            }
            if (d < 1.0f)
            {
                Paint(&p, ink, Cover(d));
                if (covI > 0.0f)
                {
                    float creamCover = s->cream ? Clamp01(creamEdge - u + 0.5f) * covI : 0.0f;

                    Paint(&p, face, covI);
                    Paint(&p, deep, 0.45f * covI * Clamp01(v + 0.5f - (bandEdge0 + bandSlope * u)));
                    if (s->cream)
                    {
                        static const float stripes[3][3] = {{1.0f, 2.0f, 0.85f}, {5, 9, 0.22f}, {17, 4, 0.14f}};

                        Paint(&p, cream, creamCover);
                        for (int k = 0; k < 3; ++k)
                        {
                            float a = creamEdge + stripes[k][0], b = a + stripes[k][1];
                            float cov = Clamp01(u + 0.5f - a) - Clamp01(u + 0.5f - b);

                            Paint(&p, white, stripes[k][2] * cov * covI);
                        }
                        Paint(&p, creamBall, 0.85f * creamCover * FaintBall(u - 13.0f, v - (oy + 16.0f), 12.0f));
                    }
                    if (s->head)
                    {
                        Paint(&p, headFace, headCover * covI);
                        Paint(&p, ink, 0.9f * sep * covI);
                        if (v < oy + s->head + 0.5f)
                        {
                            static const float stripes[2][3] = {{0, 10, 0.20f}, {14, 3, 0.16f}};
                            float rows = Clamp01(oy + s->head - (v - 0.5f)) * Clamp01(v + 0.5f - oy);

                            for (int k = 0; k < 2; ++k)
                            {
                                float a = w - 46 + stripes[k][0] - (v - oy) * 0.5f, b = a + stripes[k][1];
                                float cov = Clamp01(u + 0.5f - a) - Clamp01(u + 0.5f - b);

                                Paint(&p, white, stripes[k][2] * cov * rows * covI);
                            }
                        }
                    }
                    Paint(&p, white, gloss * covI);
                    {
                        float rim = 0.875f - fabsf(d + 1.9f);

                        if (rim > 0.75f) rim = 0.75f;
                        if (rim > 0.0f)
                            Paint(&p, white, 0.75f * rim * fade);
                    }
                    if (scan)
                        Paint(&p, black, 0.045f * covI);
                }
            }
            {
                /* Each layer lerps towards its colour: what it adds is premultiplied already. */
                int t = (int)(p.t * 255.0f + 0.5f), r = (int)(p.r + 0.5f), g = (int)(p.g + 0.5f), b = (int)(p.b + 0.5f);

                outT[i * ph + j] = (u8)(t > 255 ? 255 : t);
                outC[i * ph + j] = (r > 255 ? 31 : (r * 31 + 127) / 255) << 11 | (g > 255 ? 63 : (g * 63 + 127) / 255) << 5
                                 | (b > 255 ? 31 : (b * 31 + 127) / 255);
            }
        }
    }
}

/*
 * A plate's picture packed, one block: for each of its pw columns the one
 * kept for it (col), then the kept columns' c and their t, ph each. A
 * column is kept when it differs from the one before it.
 */
#define PLATE_MAX_W 400
#define PLATE_MAX_H 300

static size_t PlateBlockBytes(int pw, int ph, int cols)
{
    return (size_t)pw * sizeof(u16) + (size_t)cols * ph * 3;
}

static void PlateSplit(u8 *block, int pw, int ph, int cols, u16 **col, u16 **c, u8 **t)
{
    *col = (u16 *)block;
    *c = *col + pw;
    *t = (u8 *)(*c + (size_t)cols * ph);
}

/* A picture made (t and c, column by column) packed into a block of its own;
 * NULL without memory. */
static u8 *PlatePack(const u8 *t, const u16 *c, int pw, int ph, u16 *colsOut)
{
    u16 index[PLATE_MAX_W], *col, *pc;
    u8 *block, *pt;
    int cols = 0;

    if (pw < 1 || pw > PLATE_MAX_W || ph < 1)
        return NULL;
    for (int i = 0; i < pw; ++i)
    {
        if (i == 0 || memcmp(t + i * ph, t + (i - 1) * ph, ph) != 0
         || memcmp(c + i * ph, c + (i - 1) * ph, ph * sizeof(u16)) != 0)
            ++cols;
        index[i] = cols - 1;
    }
    block = malloc(PlateBlockBytes(pw, ph, cols));
    if (!block)
        return NULL;
    PlateSplit(block, pw, ph, cols, &col, &pc, &pt);
    memcpy(col, index, pw * sizeof(u16));
    for (int i = 0; i < pw; ++i)
        if (i == 0 || index[i] != index[i - 1])
        {
            memcpy(pc + index[i] * ph, c + i * ph, ph * sizeof(u16));
            memcpy(pt + index[i] * ph, t + i * ph, ph);
        }
    *colsOut = cols;
    return block;
}

/*
 * The plates made so far, the least recently drawn given up first once they
 * take more than PLATE_BUDGET bytes. A screen's few dozen plates are a few
 * hundred KiB whole, a third of that packed; the big ones (a list's panel)
 * are drawn into the screen's own backdrop cache and seldom again.
 */
#define PLATE_PICS 160
#define PLATE_BUDGET (1536u * 1024u)
typedef struct
{
    PlateSpec spec;
    bool8 odd, valid;
    u16 pw, ph, cols;
    u32 used;
    u8 *block;
    u16 *col, *c;
    u8 *t;
} PlatePic;
static PlatePic sPlates[PLATE_PICS];
static u32 sPlateClock;
static size_t sPlateBytes;
/* Plates made, the ticks spent making them and the ones given up for room,
 * for the slow-redraw and redraw-peak logs. */
static u32 sPlatesMade, sPlatesEvicted;
static uint64_t sPlateTicks;
/* The ticks spent drawing plates made already and text, for the same logs. */
static uint64_t sPlateDrawTicks, sTextTicks;

static void FreePlate(PlatePic *p)
{
    if (!p->valid)
        return;
    sPlateBytes -= PlateBlockBytes(p->pw, p->ph, p->cols);
    free(p->block);
    p->block = NULL;
    p->valid = FALSE;
}

/*
 * Plates made ahead (PlateWarm, 3ds_bottom_ui.c): a screen's plates drawn with
 * nothing to draw into make their pictures, under a time budget and never in
 * the room of one made already. The cost of a plate, learnt from those made,
 * in microseconds a pixel (its picture with its margins).
 */
static bool8 sPlateWarm;
static uint64_t sPlateWarmStart;
static float sPlateWarmMs, sPlateUsPerPixel = 0.5f;
/* What the walk left undone: a plate with no time for it, or no room. */
static bool8 sPlateWarmShort, sPlateWarmFull;
static u8 sPlateWarmBuilt;
/*
 * The plate being made ahead, a few rows a frame: one costs more than a
 * frame has to spare (some 25 ms on the console), so it is made over several,
 * its rows into buffers of its own, and goes into the cache whole. A plate
 * drawn for real meanwhile takes the rows made already.
 */
static struct
{
    PlateSpec spec;
    bool8 odd, active;
    u16 pw, ph, row;
    u8 *t;
    u16 *c;
} sPlateJob;

static void PlateJobDrop(void)
{
    free(sPlateJob.t);
    free(sPlateJob.c);
    sPlateJob.t = NULL;
    sPlateJob.c = NULL;
    sPlateJob.active = FALSE;
}

/* The walk came to the job's plate (WarmScreens drops a job it did not). */
static bool8 sPlateJobSeen;

static bool8 PlateJobIs(const PlateSpec *s, bool8 odd)
{
    return sPlateJob.active && sPlateJob.odd == odd && memcmp(&sPlateJob.spec, s, sizeof(*s)) == 0;
}

/*
 * The plates kept on the SD card (sdmc:/3ds/emerald3ds/plates.bin): a plate
 * is worked out once on this console and kept there for the next sessions.
 * The card is slow on the console - 30-50 ms to read a plate, more than
 * making it, and 100 ms or more to write one, a second to look through the
 * file seeking - so the game never touches it: a worker of its own reads
 * the file through once, start to end, as the game boots, and hands the
 * plates over to the cache (PlateDiskAdopt) until it holds as many as it
 * may; writes the plates made since; and reads one the walk ahead asks for.
 * A plate the cache does not have yet is made as before, never waited for.
 *
 * The file starts with a probe, two small plates' pictures hashed: a build
 * whose plates come out differently (RenderPlateRows changed) finds another
 * hash and starts the file again. Then each plate as made: its spec, its
 * size, a sum of its pixels, the pixels - packed (PLATE_DISK_PACKED: its
 * kept columns' count, then its block) or, as the first builds wrote them,
 * whole (t, then c), packed as they are read. A plate whose sum is wrong is
 * made again and written after it, the newest copy winning; a file cut short
 * is kept up to its last whole plate.
 */
#define PLATE_DISK_FILE "plates.bin"
#define PLATE_DISK_MAGIC 0x43503345u   /* "E3PC" */
#define PLATE_DISK_ENTRY 0x54414C50u   /* "PLAT" */
#define PLATE_DISK_VERSION 1u
#define PLATE_DISK_MAX 768
#define PLATE_DISK_BYTES (12u * 1024u * 1024u)
/* Plates read and not taken yet, plates made and not written yet. */
#define PLATE_DISK_READY 8
#define PLATE_DISK_WRITES 6
#define PLATE_DISK_ASKS 8
#define PLATE_DISK_IDLE_US 20000

typedef struct
{
    u32 magic, version, probe, reserved;
} PlateDiskHeader;

typedef struct
{
    u32 magic;
    PlateSpec spec;
    u8 odd, format;
    u16 pw, ph;
    u32 sum;
} PlateDiskEntry;

/* PlateDiskEntry.format: 0 whole, as the first builds wrote; packed, the
 * entry followed by a PlateDiskPacked and the block. */
#define PLATE_DISK_PACKED 1
typedef struct
{
    u16 cols, zero;
} PlateDiskPacked;

/* A plate's packed pixels between the worker and the game: read, or to write. */
typedef struct
{
    PlateSpec spec;
    u8 odd;
    u16 pw, ph, cols;
    u8 *block;
} PlateDiskPic;

/* Everything under `lock` but the file, which only the worker uses. */
static struct
{
    CtrLock lock;
    bool8 started, scanned;
    u32 probe;
    u16 count;
    struct { PlateSpec spec; u8 odd, bad, asked; u32 at; } index[PLATE_DISK_MAX];
    PlateDiskPic ready[PLATE_DISK_READY];
    u8 readyCount;
    PlateDiskPic writes[PLATE_DISK_WRITES];
    u8 writeCount;
    s16 asks[PLATE_DISK_ASKS];
    u8 askCount;
    u32 reads, written, adopted;
} sDisk;

static FILE *sDiskFile;
static u32 sDiskEnd;

static u32 PlateSum(const u8 *t, const u16 *c, u32 n)
{
    u32 h = 2166136261u;

    for (u32 i = 0; i < n; ++i)
        h = (h ^ t[i]) * 16777619u;
    for (u32 i = 0; i < n; ++i)
        h = (h ^ c[i]) * 16777619u;
    return h;
}

/* A packed plate's sum: its kept columns' count, then its block's bytes. */
static u32 PlateBlockSum(const u8 *block, size_t bytes, u16 cols)
{
    u32 h = (2166136261u ^ cols) * 16777619u;

    for (size_t i = 0; i < bytes; ++i)
        h = (h ^ block[i]) * 16777619u;
    return h;
}

/* Two plates small enough to make at boot, between them every layer a
 * plate can have (the shadow, the ring, the cream end and its stripes, the
 * head, the gloss and rim, both scanline phases). */
static u32 PlateDiskProbe(void)
{
    static const PlateSpec probes[2] = {
        {.w = 40, .h = 24, .cream = 12, .cut = 5, .slope = 20, .head = 0, .state = PLATE_STATE_FOCUS, .base = {48, 160, 120}},
        {.w = 32, .h = 22, .cream = 0, .cut = 4, .slope = 0, .head = 8, .state = PLATE_STATE_PRESSED, .base = {200, 72, 56}},
    };
    static u8 t[(40 + PLATE_ML + PLATE_MR) * (24 + PLATE_MT + PLATE_MB)];
    static u16 c[sizeof(t)];
    u32 h = PLATE_DISK_VERSION;

    for (int k = 0; k < 2; ++k)
    {
        int pw = probes[k].w + PLATE_ML + PLATE_MR, ph = probes[k].h + PLATE_MT + PLATE_MB;

        RenderPlateRows(&probes[k], t, c, k, 0, ph);
        h = h * 31 + PlateSum(t, c, (u32)(pw * ph));
    }
    return h;
}

static void PlateDiskPicFree(PlateDiskPic *p)
{
    free(p->block);
    p->block = NULL;
}

/* The newest copy of a plate on the card, or -1; under the lock. */
static int PlateDiskFindLocked(const PlateSpec *s, bool8 odd)
{
    for (int i = sDisk.count - 1; i >= 0; --i)
        if (!sDisk.index[i].bad && sDisk.index[i].odd == odd && memcmp(&sDisk.index[i].spec, s, sizeof(*s)) == 0)
            return i;
    return -1;
}

static int PlateDiskFind(const PlateSpec *s, bool8 odd)
{
    int i;

    if (!sDisk.started)
        return -1;
    CtrLock_Lock(&sDisk.lock);
    i = PlateDiskFindLocked(s, odd);
    CtrLock_Unlock(&sDisk.lock);
    return i;
}

/* The worker: what follows a plate's entry in `file` - a packed plate's
 * count of kept columns, read here - and the bytes of its pixels; 0 for
 * none that makes sense. */
static u32 PlateDiskStored(FILE *file, const PlateDiskEntry *e, u16 *cols)
{
    PlateDiskPacked packed;

    *cols = 0;
    if (e->format != PLATE_DISK_PACKED)
        return (u32)e->pw * e->ph * 3;
    if (fread(&packed, sizeof(packed), 1, file) != 1 || packed.cols < 1 || packed.cols > e->pw)
        return 0;
    *cols = packed.cols;
    return (u32)PlateBlockBytes(e->pw, e->ph, packed.cols);
}

/* The worker: a plate's pixels from where the file stands (past
 * PlateDiskStored), checked, and packed if they were kept whole. */
static bool8 PlateDiskReadPixels(const PlateDiskEntry *e, u16 cols, PlateDiskPic *out)
{
    u32 n = (u32)e->pw * e->ph;

    out->spec = e->spec;
    out->odd = e->odd;
    out->pw = e->pw;
    out->ph = e->ph;
    out->cols = cols;
    out->block = NULL;
    if (e->format == PLATE_DISK_PACKED)
    {
        size_t bytes = PlateBlockBytes(e->pw, e->ph, cols);
        const u16 *col;

        out->block = malloc(bytes);
        if (!out->block || fread(out->block, 1, bytes, sDiskFile) != bytes
         || PlateBlockSum(out->block, bytes, cols) != e->sum)
        {
            PlateDiskPicFree(out);
            return FALSE;
        }
        /* Each column's kept one among those kept. */
        col = (const u16 *)out->block;
        for (int i = 0; i < e->pw; ++i)
            if (col[i] >= cols)
            {
                PlateDiskPicFree(out);
                return FALSE;
            }
        return TRUE;
    }
    {
        u8 *t = malloc(n);
        u16 *c = malloc(n * sizeof(u16));

        if (t && c && fread(t, 1, n, sDiskFile) == n && fread(c, sizeof(u16), n, sDiskFile) == n
         && PlateSum(t, c, n) == e->sum)
            out->block = PlatePack(t, c, e->pw, e->ph, &out->cols);
        free(t);
        free(c);
        return out->block != NULL;
    }
}

/* The worker: a plate read handed over, once the game has room to take it. */
static void PlateDiskHand(PlateDiskPic *pic)
{
    for (;;)
    {
        CtrLock_Lock(&sDisk.lock);
        if (sDisk.readyCount < PLATE_DISK_READY)
        {
            sDisk.ready[sDisk.readyCount++] = *pic;
            ++sDisk.reads;
            CtrLock_Unlock(&sDisk.lock);
            return;
        }
        CtrLock_Unlock(&sDisk.lock);
        CtrPlatform_SleepUs(PLATE_DISK_IDLE_US);
    }
}

static bool8 PlateDiskFresh(u32 probe)
{
    PlateDiskHeader head = {PLATE_DISK_MAGIC, PLATE_DISK_VERSION, probe, 0};

    if (sDiskFile)
        fclose(sDiskFile);
    sDiskFile = CtrFs_OpenData(PLATE_DISK_FILE, "w+b");
    if (!sDiskFile || fwrite(&head, sizeof(head), 1, sDiskFile) != 1 || fflush(sDiskFile) != 0)
    {
        if (sDiskFile)
            fclose(sDiskFile);
        sDiskFile = NULL;
        return FALSE;
    }
    sDiskEnd = sizeof(head);
    return TRUE;
}

/*
 * The worker's start: the file read through once, its index made and, up to
 * the cache's budget, its plates handed over as they come - the first made
 * first, the dock's and the header's among them.
 */
static void PlateDiskScan(void)
{
    uint64_t start = CtrPlatform_Ticks();
    PlateDiskHeader head;
    struct stat st;
    u32 handed = 0, handedBytes = 0, size = 0;
    FILE *file = NULL;

    /* None yet: not an error, the first session's. */
    if (stat("sdmc:/3ds/emerald3ds/" PLATE_DISK_FILE, &st) == 0)
    {
        file = CtrFs_OpenData(PLATE_DISK_FILE, "r+b");
        size = (u32)st.st_size;
    }
    if (file)
        setvbuf(file, NULL, _IOFBF, 32 * 1024);
    if (file && fread(&head, sizeof(head), 1, file) == 1 && head.magic == PLATE_DISK_MAGIC
     && head.version == PLATE_DISK_VERSION && head.probe == sDisk.probe)
    {
        PlateDiskEntry e;
        u32 at = sizeof(head);

        sDiskFile = file;
        while (sDisk.count < PLATE_DISK_MAX && fread(&e, sizeof(e), 1, file) == 1)
        {
            u32 bytes = 0, head = sizeof(e) + (e.format == PLATE_DISK_PACKED ? sizeof(PlateDiskPacked) : 0);
            u16 cols = 0;
            PlateDiskPic pic;

            if (e.magic != PLATE_DISK_ENTRY || e.pw != e.spec.w + PLATE_ML + PLATE_MR
             || e.ph != e.spec.h + PLATE_MT + PLATE_MB || e.pw > PLATE_MAX_W || e.ph > PLATE_MAX_H
             || (e.format != 0 && e.format != PLATE_DISK_PACKED)
             || (bytes = PlateDiskStored(file, &e, &cols)) == 0 || at + head + bytes > size)
                break;
            CtrLock_Lock(&sDisk.lock);
            sDisk.index[sDisk.count].spec = e.spec;
            sDisk.index[sDisk.count].odd = e.odd;
            sDisk.index[sDisk.count].bad = FALSE;
            sDisk.index[sDisk.count].asked = FALSE;
            sDisk.index[sDisk.count].at = at;
            ++sDisk.count;
            CtrLock_Unlock(&sDisk.lock);
            /* One kept whole is read to learn what it takes packed. */
            if (handedBytes + (e.format == PLATE_DISK_PACKED ? bytes : 0) <= PLATE_BUDGET)
            {
                if (!PlateDiskReadPixels(&e, cols, &pic))
                {
                    CtrLock_Lock(&sDisk.lock);
                    sDisk.index[sDisk.count - 1].bad = TRUE;
                    CtrLock_Unlock(&sDisk.lock);
                    if (fseek(file, at + head + bytes, SEEK_SET) != 0)
                        break;
                }
                else if (handedBytes + PlateBlockBytes(pic.pw, pic.ph, pic.cols) > PLATE_BUDGET)
                    PlateDiskPicFree(&pic);
                else
                {
                    handedBytes += PlateBlockBytes(pic.pw, pic.ph, pic.cols);
                    PlateDiskHand(&pic);
                    ++handed;
                }
            }
            else if (fseek(file, bytes, SEEK_CUR) != 0)
                break;
            at += head + bytes;
        }
        sDiskEnd = at;
        CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: %u plates on the SD card (%lu KiB), %lu read ahead (%lu KiB) "
                     "in %.1f ms on the worker", sDisk.count, (unsigned long)(at / 1024), (unsigned long)handed,
                     (unsigned long)(handedBytes / 1024), CtrPlatform_TickMs(CtrPlatform_Ticks() - start));
        return;
    }
    if (file)
        fclose(file);
    if (PlateDiskFresh(sDisk.probe))
        CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: a new plate file on the SD card (%s)",
                     file ? "the plates are drawn differently now, or it was damaged" : "none yet");
    else
        CtrLog_Write(CTR_LOG_ERROR, "bottom screen: no plate file on the SD card; plates are made each session");
}

/* The worker: one plate made since, onto the end of the file. */
static void PlateDiskWrite(PlateDiskPic *p)
{
    uint64_t start = CtrPlatform_Ticks();
    size_t n = PlateBlockBytes(p->pw, p->ph, p->cols);
    u32 bytes = sizeof(PlateDiskEntry) + sizeof(PlateDiskPacked) + n;
    PlateDiskPacked packed = {p->cols, 0};
    PlateDiskEntry e;

    if (sDiskFile && sDiskEnd + bytes <= PLATE_DISK_BYTES)
    {
        memset(&e, 0, sizeof(e));
        e.magic = PLATE_DISK_ENTRY;
        e.spec = p->spec;
        e.odd = p->odd;
        e.format = PLATE_DISK_PACKED;
        e.pw = p->pw;
        e.ph = p->ph;
        e.sum = PlateBlockSum(p->block, n, p->cols);
        if (fseek(sDiskFile, sDiskEnd, SEEK_SET) != 0 || fwrite(&e, sizeof(e), 1, sDiskFile) != 1
         || fwrite(&packed, sizeof(packed), 1, sDiskFile) != 1 || fwrite(p->block, 1, n, sDiskFile) != n
         || fflush(sDiskFile) != 0)
        {
            CtrLog_Write(CTR_LOG_ERROR, "bottom screen: the SD card took no more plates");
            fclose(sDiskFile);
            sDiskFile = NULL;
        }
        else
        {
            CtrLock_Lock(&sDisk.lock);
            if (sDisk.count < PLATE_DISK_MAX)
            {
                sDisk.index[sDisk.count].spec = p->spec;
                sDisk.index[sDisk.count].odd = p->odd;
                sDisk.index[sDisk.count].bad = FALSE;
                sDisk.index[sDisk.count].asked = FALSE;
                sDisk.index[sDisk.count].at = sDiskEnd;
                ++sDisk.count;
            }
            ++sDisk.written;
            CtrLock_Unlock(&sDisk.lock);
            sDiskEnd += bytes;
            if ((sDisk.written & 15) == 1)
                CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: plates written to the SD card %lu, the last in %.1f ms "
                             "on the worker", (unsigned long)sDisk.written,
                             CtrPlatform_TickMs(CtrPlatform_Ticks() - start));
        }
    }
    PlateDiskPicFree(p);
}

/* The worker: a plate the walk asked for, read and handed over. */
static void PlateDiskReadAsked(int i)
{
    uint64_t start = CtrPlatform_Ticks();
    PlateDiskEntry e;
    PlateDiskPic pic;
    u32 at;
    u16 cols;

    CtrLock_Lock(&sDisk.lock);
    at = sDisk.index[i].at;
    CtrLock_Unlock(&sDisk.lock);
    if (!sDiskFile || fseek(sDiskFile, at, SEEK_SET) != 0 || fread(&e, sizeof(e), 1, sDiskFile) != 1
     || PlateDiskStored(sDiskFile, &e, &cols) == 0 || !PlateDiskReadPixels(&e, cols, &pic))
    {
        CtrLock_Lock(&sDisk.lock);
        sDisk.index[i].bad = TRUE;
        CtrLock_Unlock(&sDisk.lock);
        CtrLog_Write(CTR_LOG_ERROR, "bottom screen: a plate on the SD card did not read back; made again");
        return;
    }
    PlateDiskHand(&pic);
    if ((sDisk.reads & 31) == 1)
        CtrLog_Write(CTR_LOG_VIDEO, "bottom screen: plates read from the SD card %lu, the last in %.1f ms on the worker",
                     (unsigned long)sDisk.reads, CtrPlatform_TickMs(CtrPlatform_Ticks() - start));
}

static void PlateDiskWorker(void *arg)
{
    (void)arg;
    PlateDiskScan();
    CtrLock_Lock(&sDisk.lock);
    sDisk.scanned = TRUE;
    CtrLock_Unlock(&sDisk.lock);
    for (;;)
    {
        PlateDiskPic write;
        int ask = -1;
        bool8 haveWrite = FALSE;

        CtrLock_Lock(&sDisk.lock);
        if (sDisk.askCount)
            ask = sDisk.asks[--sDisk.askCount];
        else if (sDisk.writeCount)
        {
            write = sDisk.writes[0];
            memmove(&sDisk.writes[0], &sDisk.writes[1], --sDisk.writeCount * sizeof(sDisk.writes[0]));
            haveWrite = TRUE;
        }
        CtrLock_Unlock(&sDisk.lock);
        if (ask >= 0)
            PlateDiskReadAsked(ask);
        else if (haveWrite)
            PlateDiskWrite(&write);
        else
            CtrPlatform_SleepUs(PLATE_DISK_IDLE_US);
    }
}

/* At boot (CtrBottom_Init): the probe made here, the rest on the worker. */
static void PlateDiskOpen(void)
{
    if (sDisk.started)
        return;
    CtrLock_Init(&sDisk.lock);
    sDisk.probe = PlateDiskProbe();
    sDisk.started = CtrPlatform_StartThread(PlateDiskWorker, NULL, 16 * 1024, -2);
    if (!sDisk.started)
        CtrLog_Write(CTR_LOG_ERROR, "bottom screen: no worker for the plate file; plates are made each session");
}

/* A plate just made, to be written: its block copied for the worker. */
static void PlateDiskQueue(const PlateSpec *s, bool8 odd, const u8 *block, int pw, int ph, int cols)
{
    size_t n = PlateBlockBytes(pw, ph, cols);
    PlateDiskPic p;
    bool8 wanted;

    if (!sDisk.started)
        return;
    CtrLock_Lock(&sDisk.lock);
    wanted = sDisk.scanned && sDisk.writeCount < PLATE_DISK_WRITES && sDisk.count < PLATE_DISK_MAX
          && PlateDiskFindLocked(s, odd) < 0;
    for (int i = 0; i < sDisk.writeCount && wanted; ++i)
        wanted = !(sDisk.writes[i].odd == odd && memcmp(&sDisk.writes[i].spec, s, sizeof(*s)) == 0);
    CtrLock_Unlock(&sDisk.lock);
    if (!wanted)
        return;
    p.spec = *s;
    p.odd = odd;
    p.pw = pw;
    p.ph = ph;
    p.cols = cols;
    p.block = malloc(n);
    if (!p.block)
        return;
    memcpy(p.block, block, n);
    CtrLock_Lock(&sDisk.lock);
    if (sDisk.writeCount < PLATE_DISK_WRITES)
    {
        sDisk.writes[sDisk.writeCount++] = p;
        p.block = NULL;
    }
    CtrLock_Unlock(&sDisk.lock);
    PlateDiskPicFree(&p);
}

/* The walk wants plate i of the card: asked for once, read on the worker. */
static void PlateDiskAsk(int i)
{
    CtrLock_Lock(&sDisk.lock);
    if (!sDisk.index[i].asked && sDisk.askCount < PLATE_DISK_ASKS)
    {
        sDisk.index[i].asked = TRUE;
        sDisk.asks[sDisk.askCount++] = (s16)i;
    }
    CtrLock_Unlock(&sDisk.lock);
}

static PlatePic *FindPlate(const PlateSpec *s, bool8 odd)
{
    for (int i = 0; i < PLATE_PICS; ++i)
        if (sPlates[i].valid && sPlates[i].spec.w == s->w && sPlates[i].odd == odd
         && memcmp(&sPlates[i].spec, s, sizeof(*s)) == 0)
            return &sPlates[i];
    return NULL;
}

/* A packed picture into a free slot of the cache. */
static void PlateTake(PlatePic *slot, const PlateSpec *s, bool8 odd, int pw, int ph, int cols, u8 *block)
{
    slot->block = block;
    PlateSplit(block, pw, ph, cols, &slot->col, &slot->c, &slot->t);
    slot->spec = *s;
    slot->odd = odd;
    slot->pw = pw;
    slot->ph = ph;
    slot->cols = cols;
    slot->used = ++sPlateClock;
    slot->valid = TRUE;
    sPlateBytes += PlateBlockBytes(pw, ph, cols);
}

/* Plates the worker has read, into the cache's free room (a plate made
 * there is never given up for one); those with no room are let go, and may
 * be asked for again. Between redraws, on the game's thread. */
static void PlateDiskAdopt(void)
{
    PlateDiskPic got[PLATE_DISK_READY];
    u8 n;

    if (!sDisk.started)
        return;
    CtrLock_Lock(&sDisk.lock);
    n = sDisk.readyCount;
    memcpy(got, sDisk.ready, n * sizeof(got[0]));
    sDisk.readyCount = 0;
    for (u8 k = 0; k < n; ++k)
    {
        int i = PlateDiskFindLocked(&got[k].spec, got[k].odd);

        if (i >= 0)
            sDisk.index[i].asked = FALSE;
    }
    CtrLock_Unlock(&sDisk.lock);
    for (u8 k = 0; k < n; ++k)
    {
        PlateDiskPic *p = &got[k];
        size_t bytes = PlateBlockBytes(p->pw, p->ph, p->cols);
        PlatePic *slot = NULL;

        if (!FindPlate(&p->spec, p->odd) && sPlateBytes + bytes <= PLATE_BUDGET)
            for (int i = 0; i < PLATE_PICS && !slot; ++i)
                if (!sPlates[i].valid)
                    slot = &sPlates[i];
        if (!slot)
        {
            PlateDiskPicFree(p);
            continue;
        }
        PlateTake(slot, &p->spec, p->odd, p->pw, p->ph, p->cols, p->block);
        ++sDisk.adopted;
    }
}

static const PlatePic *PlatePictureEx(const PlateSpec *s, bool8 odd, bool8 evict)
{
    PlatePic *slot = NULL;
    int pw = s->w + PLATE_ML + PLATE_MR, ph = s->h + PLATE_MT + PLATE_MB;
    bool8 job = PlateJobIs(s, odd);
    uint64_t start;
    size_t bytes;
    u8 *block, *t;
    u16 *c, cols;

    if ((slot = FindPlate(s, odd)) != NULL)
    {
        slot->used = ++sPlateClock;
        return slot;
    }
    /* Made - begun ahead, the rows left into its own buffers - and packed. */
    if (job)
    {
        t = sPlateJob.t;
        c = sPlateJob.c;
    }
    else
    {
        t = malloc((size_t)pw * ph);
        c = malloc((size_t)pw * ph * sizeof(u16));
        if (!t || !c)
        {
            free(t);
            free(c);
            return NULL;
        }
    }
    start = CtrPlatform_Ticks();
    RenderPlateRows(s, t, c, odd, job ? sPlateJob.row : 0, ph);
    start = CtrPlatform_Ticks() - start;
    sPlateTicks += start;
    if (!job)
        sPlateUsPerPixel = sPlateUsPerPixel * 0.75f + CtrPlatform_TickMs(start) * 1000.0f / (pw * ph) * 0.25f;
    else
        sPlateJob.row = ph;
    block = PlatePack(t, c, pw, ph, &cols);
    if (!job)
    {
        free(t);
        free(c);
    }
    if (!block)
        return NULL;
    /* Room: a free slot, then the oldest ones while over budget. */
    bytes = PlateBlockBytes(pw, ph, cols);
    for (;;)
    {
        PlatePic *oldest = NULL;

        slot = NULL;
        for (int i = 0; i < PLATE_PICS; ++i)
        {
            if (!sPlates[i].valid && !slot)
                slot = &sPlates[i];
            if (sPlates[i].valid && (!oldest || sPlates[i].used < oldest->used))
                oldest = &sPlates[i];
        }
        if (slot && sPlateBytes + bytes <= PLATE_BUDGET)
            break;
        /* Made ahead: only into room free now. */
        if (!evict || !oldest)
        {
            slot = NULL;
            break;
        }
        FreePlate(oldest);
        ++sPlatesEvicted;
    }
    if (!slot)
    {
        free(block);
        return NULL;
    }
    if (job)
        PlateJobDrop();
    PlateDiskQueue(s, odd, block, pw, ph, cols);
    PlateTake(slot, s, odd, pw, ph, cols, block);
    ++sPlatesMade;
    return slot;
}

static const PlatePic *PlatePicture(const PlateSpec *s, bool8 odd)
{
    return PlatePictureEx(s, odd, TRUE);
}

/* A plate of the walk (sPlateWarm): its picture made, a few rows a frame
 * (sPlateJob), while what is left of the walk's time lasts and only into the
 * room free. One plate at a time: the others wait for a later walk. */
static void WarmPlate(const PlateSpec *s, bool8 odd)
{
    int pw = s->w + PLATE_ML + PLATE_MR, ph = s->h + PLATE_MT + PLATE_MB;
    float rowMs = sPlateUsPerPixel * pw / 1000.0f;
    bool8 slotFree = FALSE;

    if (FindPlate(s, odd))
        return;
    for (int i = 0; i < PLATE_PICS && !slotFree; ++i)
        slotFree = !sPlates[i].valid;
    /* Its room packed, about: some third of its columns kept. */
    if (!slotFree || sPlateBytes + PlateBlockBytes(pw, ph, pw / 3 + 1) > PLATE_BUDGET)
    {
        sPlateWarmFull = TRUE;
        return;
    }
    /* On the card: asked of the worker, taken when it comes (PlateDiskAdopt). */
    {
        int i = PlateDiskFind(s, odd);

        if (i >= 0)
        {
            if (PlateJobIs(s, odd))
                PlateJobDrop();
            PlateDiskAsk(i);
            sPlateWarmShort = TRUE;
            return;
        }
    }
    if (!PlateJobIs(s, odd))
    {
        if (sPlateJob.active)
        {
            sPlateWarmShort = TRUE;
            return;
        }
        sPlateJob.t = malloc((size_t)pw * ph);
        sPlateJob.c = malloc((size_t)pw * ph * sizeof(u16));
        if (!sPlateJob.t || !sPlateJob.c)
        {
            PlateJobDrop();
            sPlateWarmFull = TRUE;
            return;
        }
        sPlateJob.spec = *s;
        sPlateJob.odd = odd;
        sPlateJob.pw = pw;
        sPlateJob.ph = ph;
        sPlateJob.row = 0;
        sPlateJob.active = TRUE;
    }
    sPlateJobSeen = TRUE;
    /* A row at a time, while the next one fits what the walk has left. */
    while (sPlateJob.row < ph && CtrPlatform_TickMs(CtrPlatform_Ticks() - sPlateWarmStart) + rowMs <= sPlateWarmMs)
    {
        uint64_t start = CtrPlatform_Ticks();

        RenderPlateRows(s, sPlateJob.t, sPlateJob.c, odd, sPlateJob.row, sPlateJob.row + 1);
        ++sPlateJob.row;
        start = CtrPlatform_Ticks() - start;
        sPlateTicks += start;
        rowMs = CtrPlatform_TickMs(start);
    }
    if (sPlateJob.row < ph)
    {
        sPlateWarmShort = TRUE;
        return;
    }
    /* Whole: packed into the cache, its buffers let go (PlatePictureEx). */
    if (PlatePictureEx(s, odd, FALSE))
        ++sPlateWarmBuilt;
    else
    {
        PlateJobDrop();
        sPlateWarmFull = TRUE;
    }
}

/* Two RGB565 pixels added, each channel saturating. */
static inline u16 Add565(u16 a, u16 b)
{
    int r = (a >> 11) + (b >> 11), g = ((a >> 5) & 63) + ((b >> 5) & 63), bl = (a & 31) + (b & 31);

    return (u16)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (bl > 31 ? 31 : bl));
}

/*
 * Add565 of c and p's channels each times t / 255 (Div255's rounding), t
 * 0..255. Red and blue go through as two 16-bit lanes of one word, red at
 * bit 16: one multiply and one Div255 for both, no lane carrying into the
 * other (31 * 255 + 128 + 255 < 65536).
 */
static inline u16 Blend565(u16 p, int t, u16 c)
{
    u32 rb = ((u32)(p & 0xF800) << 5 | (p & 0x1Fu)) * (u32)t + 0x00800080u;
    u32 g = ((p >> 5) & 63u) * (u32)t + 128u, r, b;

    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    g = ((g + (g >> 8)) >> 8) + ((c >> 5) & 63u);
    rb += (u32)(c >> 11) << 16 | (c & 31u);
    r = rb >> 16;
    b = rb & 0xFFFFu;
    return (u16)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
}

/* A plate with its top-left at (x, y); returns the top of its face (lower when pressed). */
static int DrawPlateSpec(int x, int y, const PlateSpec *s)
{
    int x0 = x - PLATE_ML + sOX, y0 = y - PLATE_MT;
    const PlatePic *p;
    int i0, i1, j0, j1;

    if (x0 >= sClipX1 || y0 >= sClipY1 || x0 + s->w + PLATE_ML + PLATE_MR <= sClipX0
     || y0 + s->h + PLATE_MT + PLATE_MB <= sClipY0)
    {
        if (sPlateWarm)
            WarmPlate(s, (y0 & 1) != 0);
        return y + (s->state == PLATE_STATE_PRESSED ? PLATE_SINK : 0);
    }
    p = PlatePicture(s, (y0 & 1) != 0);
    if (p)
    {
        uint64_t start = CtrPlatform_Ticks();

        i0 = sClipX0 - x0 > 0 ? sClipX0 - x0 : 0;
        j0 = sClipY0 - y0 > 0 ? sClipY0 - y0 : 0;
        i1 = sClipX1 - x0 < p->pw ? sClipX1 - x0 : p->pw;
        j1 = sClipY1 - y0 < p->ph ? sClipY1 - y0 : p->ph;
        for (int i = i0; i < i1; ++i)
        {
            u16 *dst = sDst + (x0 + i) * H + (H - 1 - (y0 + j0));
            const u8 *t = p->t + p->col[i] * p->ph + j0;
            const u16 *c = p->c + p->col[i] * p->ph + j0;

            for (int j = j0; j < j1; ++j, --dst, ++t, ++c)
            {
                if (*t == 255)
                {
                    if (*c)
                        *dst = Add565(*dst, *c);
                }
                else if (*t == 0)
                    *dst = *c;
                else
                    *dst = Blend565(*dst, *t, *c);
            }
        }
        sPlateDrawTicks += CtrPlatform_Ticks() - start;
    }
    else
    {
        /* No memory for its picture: a flat face, so the screen still works. */
        u16 face = PackRgb(s->base.r, s->base.g, s->base.b);

        FillRect(x + 1, y, s->w - 2, s->h, face);
        FillRect(x, y + 1, s->w, s->h - 2, face);
    }
    return y + (s->state == PLATE_STATE_PRESSED ? PLATE_SINK : 0);
}

/* The mockups' plate: corners cut by a fifth of the height (3 to 5), a cream
 * end `cream` wide slanted `slope` hundredths. */
static int PlateCut(int h)
{
    return h / 5 < 3 ? 3 : h / 5 > 5 ? 5 : h / 5;
}

static int PlateEx(int x, int y, int w, int h, Rgb base, u8 state, int cream, int slope, int cut, int head)
{
    PlateSpec s;

    memset(&s, 0, sizeof(s));
    s.w = w;
    s.h = h;
    s.cream = cream;
    s.slope = slope;
    s.cut = cut;
    s.head = head;
    s.state = state;
    s.base = base;
    return DrawPlateSpec(x, y, &s);
}

static int Plate(int x, int y, int w, int h, Rgb base, u8 state)
{
    return PlateEx(x, y, w, h, base, state, 0, 30, PlateCut(h), 0);
}

/* The plate's room on screen, ring and shadow included: for partial redraws. */
static void PlateBounds(int x, int y, int w, int h, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = x - PLATE_ML;
    *y0 = y - PLATE_MT;
    *x1 = x + w + PLATE_MR;
    *y1 = y + h + PLATE_MB;
}

/* ------------------------------------------------------------------------ */
/* Text styles                                                              */
/* ------------------------------------------------------------------------ */

/*
 * The mockups' four: white with an ink outline (on coloured plates), ink
 * with a warm shadow (on cream and light ones), green for values, grey for
 * labels. A string too wide for its room takes the game's narrow cut of its
 * font (FitFont); a label that still does not fit is cut at its room's edge.
 */
enum { STYLE_WHITE, STYLE_INK, STYLE_GREEN, STYLE_GRAY, STYLE_RED, STYLE_SOFT, STYLE_MALE, STYLE_FEMALE, STYLE_COUNT };
typedef struct { u16 fg, shadow; bool8 outlined; } TextStyle;
static TextStyle sStyle[STYLE_COUNT];

static void InitStyles(void)
{
    u16 ink = PackRgb(22, 28, 38);

    sStyle[STYLE_WHITE] = (TextStyle){PackRgb(255, 255, 255), ink, TRUE};
    sStyle[STYLE_INK] = (TextStyle){PackRgb(56, 56, 64), PackRgb(208, 200, 188), FALSE};
    sStyle[STYLE_GREEN] = (TextStyle){PackRgb(40, 128, 80), PackRgb(200, 232, 208), FALSE};
    sStyle[STYLE_GRAY] = (TextStyle){PackRgb(132, 128, 124), PackRgb(228, 220, 206), FALSE};
    sStyle[STYLE_RED] = (TextStyle){PackRgb(208, 48, 40), PackRgb(248, 184, 168), FALSE};
    /* White on the move plates' light faces' neighbours: a pale blue, outlined. */
    sStyle[STYLE_SOFT] = (TextStyle){PackRgb(208, 224, 240), ink, TRUE};
    /* The gender signs, the game's blue and red. */
    sStyle[STYLE_MALE] = (TextStyle){PackRgb(64, 136, 248), PackRgb(200, 216, 248), FALSE};
    sStyle[STYLE_FEMALE] = (TextStyle){PackRgb(248, 96, 96), PackRgb(248, 200, 200), FALSE};
    BuildPhi();
}

static bool8 IsSmall(const Font *font)
{
    return font->height < 16;
}

/* The narrow cut of a font, when the string is wider than `avail`. */
static const Font *FitFont(const Font *font, const u8 *str, int avail)
{
    const Font *narrow = IsSmall(font) ? &sSmallNarrow : &sNarrow;

    if (font == &sSmallNarrow || font == &sNarrow || StrWidth(font, str) <= avail || !narrow->glyphs)
        return font;
    return narrow;
}

/* Floor division, as the mockups' layout divides. */
static int FloorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* The row to draw one line at, centred on a plate [py, py + ph) by its capitals. */
static int TextY(int py, int ph, const Font *font)
{
    int k = IsSmall(font) ? FloorDiv(ph - 8, 2) - 4 : FloorDiv(ph - 10, 2) - 3;

    return py + (k > 1 ? k : 1);
}

/* Eight pixels of a glyph's row (two bits each, the first the word's top
 * two) that a pass puts: ink (1) for passes 0 and 1, shadow (2) for pass 2;
 * pixel k at bit 7 - k. */
static inline u32 PassBits(u16 w, int pass)
{
    u32 x = (pass == 2 ? (u32)(w >> 1) & ~(u32)w : (u32)w & ~(u32)(w >> 1)) & 0x5555u;

    x = (x | x >> 1) & 0x3333u;
    x = (x | x >> 2) & 0x0F0Fu;
    return (x | x >> 4) & 0x00FFu;
}

/* One pass over a string's glyph pixels: its ink outlined (all eight
 * neighbours), or its ink and, unless outlined, its shadow. A line or a
 * glyph wholly outside the clip (a part redrawn) only moves the pen: its
 * cell, the outline's pixel around it included, would put nothing. */
static int StyledPass(const Font *font, const u8 *str, int x, int y, u16 colour, u16 shadow, int pass)
{
    int left = x;
    bool8 lineOut = y + font->height + 1 <= sClipY0 || y - 1 >= sClipY1;

    for (u16 g; (g = NextGlyph(&str)) != 0xFFFF;)
    {
        int width;

        if (g == 0xFFFE)
        {
            x = left;
            y += font->lineHeight;
            lineOut = y + font->height + 1 <= sClipY0 || y - 1 >= sClipY1;
            continue;
        }
        width = GlyphWidth(font, g) > 16 ? 16 : GlyphWidth(font, g);
        if (lineOut || x + sOX + width + 1 <= sClipX0 || x + sOX - 1 >= sClipX1)
        {
            x += GlyphWidth(font, g);
            continue;
        }
        {
            /* Each row's ink (or shadow) pixels as a mask, pixel px at bit
             * 16 - px; the outline is the ink's spread a pixel each way. */
            u32 mask[16 + 2] = {0};
            u32 keep = (0xFFFFu << (16 - width)) & 0xFFFFu;
            int rows = font->height > 16 ? 16 : font->height;

            for (int row = 0; row < rows; ++row)
            {
                u32 m = (PassBits(GlyphBits(font, g, row, 0), pass) << 8
                         | PassBits(GlyphBits(font, g, row, 1), pass)) & keep;

                mask[row + 1] = m << 1;
            }
            if (pass == 0)
            {
                u32 spread[16 + 2];

                for (int r = 0; r < rows + 2; ++r)
                {
                    u32 m = mask[r] | (r > 0 ? mask[r - 1] : 0) | (r < rows + 1 ? mask[r + 1] : 0);

                    spread[r] = m | m << 1 | m >> 1;
                }
                for (int r = 0; r < rows + 2; ++r)
                    for (u32 m = spread[r]; m; m &= m - 1)
                        Put(x + 16 - __builtin_ctz(m), y + r - 1, colour);
            }
            else
                for (int r = 1; r <= rows; ++r)
                    for (u32 m = mask[r]; m; m &= m - 1)
                        Put(x + 16 - __builtin_ctz(m), y + r - 1, pass == 1 ? colour : shadow);
        }
        x += GlyphWidth(font, g);
    }
    return x;
}

/* A string in a style; returns where it ended. */
static int StyledStr(const Font *font, const u8 *str, int x, int y, u8 style)
{
    const TextStyle *st = &sStyle[style];
    uint64_t start = CtrPlatform_Ticks();

    if (!font->glyphs || !str)
        return x;
    if (st->outlined)
        StyledPass(font, str, x, y, st->shadow, 0, 0);
    else
        StyledPass(font, str, x, y, 0, st->shadow, 2);
    x = StyledPass(font, str, x, y, st->fg, 0, 1);
    sTextTicks += CtrPlatform_Ticks() - start;
    return x;
}

static int StyledRight(const Font *font, const u8 *str, int right, int y, u8 style)
{
    return StyledStr(font, str, right - StrWidth(font, str), y, style);
}

static int StyledCentre(const Font *font, const u8 *str, int cx, int y, u8 style)
{
    return StyledStr(font, str, cx - StrWidth(font, str) / 2, y, style);
}

static void ClipPush(int saved[4], int x0, int y0, int x1, int y1);
static void ClipPop(const int saved[4]);

/* The mockups' tin(): one line on a plate [x, x + w) x [y, y + h), `pad` in
 * from its sides, aligned 'l', 'c' or 'r', in the narrow cut if it must.
 * One that does not fit even so is cut at the plate's sides; one that fits
 * is drawn as it is, unclipped. */
static int TextIn(const u8 *str, int x, int y, int w, int h, const Font *font, char align, u8 style, int pad)
{
    const Font *f = FitFont(font, str, w - 2 * pad);
    int ty = TextY(y, h, f), end, saved[4];
    bool8 cut = f->glyphs && str && StrWidth(f, str) > w - 2 * pad;

    if (cut)
        ClipPush(saved, x, 0, x + w, H);
    if (align == 'c')
        end = StyledCentre(f, str, x + w / 2, ty, style);
    else if (align == 'r')
        end = StyledRight(f, str, x + w - pad, ty, style);
    else
        end = StyledStr(f, str, x + pad, ty, style);
    if (cut)
        ClipPop(saved);
    return end;
}

/*
 * A text over up to `max` lines of [x, x + w), `lineH` apart: its own line
 * breaks kept, and a line too wide even in the narrow cut wrapped at its
 * last space that fits. Draws only with `draw`; returns the lines.
 */
static int TextLines(const u8 *str, int x, int y, int w, int lineH, int max, const Font *font, u8 style, bool8 draw)
{
    const Font *narrow = IsSmall(font) ? &sSmallNarrow : &sNarrow;
    u8 line[256];
    int lines = 0;

    if (!narrow->glyphs)
        narrow = font;
    while (str && *str != EOS && lines < max)
    {
        const u8 *p = str, *end = str, *next = str, *space = NULL;
        int thin = 0, n;

        for (;;)
        {
            const u8 *at = p;
            u16 g = NextGlyph(&p);

            if (g == 0xFFFF || g == 0xFFFE)
            {
                end = at;
                next = p;
                break;
            }
            if (g == CHAR_SPACE)
                space = at;
            thin += narrow->widths[g];
            if (thin > w && space)
            {
                end = space;
                next = space + 1;
                break;
            }
        }
        n = (int)(end - str);
        if (n > (int)sizeof(line) - 1)
            n = sizeof(line) - 1;
        memcpy(line, str, n);
        line[n] = EOS;
        if (draw)
            StyledStr(FitFont(font, line, w), line, x, y + lines * lineH, style);
        ++lines;
        str = next;
    }
    return lines;
}
