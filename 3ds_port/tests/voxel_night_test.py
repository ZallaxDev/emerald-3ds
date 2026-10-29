"""Validate authored glass masks, the VXB8 page reader and the masks' VRAM."""

import argparse
import functools
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from voxel_night import window_mask, building_mask, NIGHT_WINDOWS



@functools.cache
def sanitize(cc):
    """Address and undefined-behaviour checks where the host compiler has
    them: MinGW's has neither."""
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        (temporary / "probe.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
        flags = ["-fsanitize=address,undefined"]
        probe = subprocess.run([cc, *flags, str(temporary / "probe.c"), "-o", str(temporary / "probe")],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return flags if probe.returncode == 0 else []


def mask_tests():
    art = Image.new("RGBA", (8, 8), (65, 74, 106, 255))
    art.putpixel((2, 2), (189, 205, 230, 255))
    art.putpixel((3, 2), (156, 180, 222, 255))
    art.putpixel((4, 2), (213, 197, 106, 255))
    art.putpixel((2, 3), (189, 205, 230, 0))
    art.putpixel((7, 7), (189, 205, 230, 255))
    original = art.tobytes()
    mask = window_mask(art, ((1, 1, 6, 6),))
    assert [index for index, value in enumerate(mask) if value] == [18, 19]
    assert mask[18] > mask[19] > 0
    assert not any(window_mask(art, ()))
    assert original == art.tobytes()
    try:
        window_mask(art, ((0, 0, 9, 8),))
    except ValueError:
        pass
    else:
        raise AssertionError("out-of-bounds window accepted")
    assert "kit_house_4" in NIGHT_WINDOWS and "kit_house_5" in NIGHT_WINDOWS
    assert NIGHT_WINDOWS["littleroot_house_e"][0][0] == 49
    for name in ("pokemon_center", "poke_mart", "gym", "gym_rustboro", "devon_corporation"):
        assert name in NIGHT_WINDOWS
    house = Image.new("RGBA", (80, 80), (65, 74, 106, 255))
    house.putpixel((49, 62), (189, 205, 230, 255))
    assert building_mask(house, "littleroot_house_e")[62 * 80 + 49] != 0
    assert building_mask(house, "littleroot_house_e_bare") == building_mask(house, "littleroot_house_e")
    assert not any(building_mask(house, "unannotated_house"))
    stone = Image.new("RGBA", (80, 80), (139, 148, 156, 255))
    for ypos in range(53, 63):
        for xpos in range(19, 29):
            stone.putpixel((xpos, ypos), (65, 74, 106, 255)
                           if xpos in (19, 23, 24, 28) or ypos in (57, 62)
                           else (156, 180, 213, 255))
    stone_mask = building_mask(stone, "rustboro_stone_31_15")
    assert any(stone_mask)
    assert not any(stone_mask[:53 * 80])
    assert stone_mask[54 * 80 + 20] > 0 and stone_mask[54 * 80 + 23] == 0
    assert stone_mask[54 * 80 + 18] == 0


def reader_tests(cc):
    source = r'''
#include <assert.h>
#include <stdint.h>
#include "voxel_building.h"
#include "voxel_relief.h"
void VoxelGrade_Texels(uint16_t *pixels, unsigned count) { (void)pixels; (void)count; }
/* What the loader never calls: linked without --gc-sections (MinGW). */
void VoxelBuilder_Tri(VoxelBuilder *builder, const VoxelVertex *a, const VoxelVertex *b,
                      const VoxelVertex *c) { (void)builder, (void)a, (void)b, (void)c; }
void VoxelBuilder_Quad(VoxelBuilder *builder, const VoxelVertex *a, const VoxelVertex *b,
                       const VoxelVertex *c, const VoxelVertex *d)
{ (void)builder, (void)a, (void)b, (void)c, (void)d; }
float VoxelBuilder_StretchZ(const VoxelBuilder *builder, float z) { (void)builder; return z; }
float VoxelRelief_CellLift(const VoxelMapInstance *inst, int x, int y) { (void)inst, (void)x, (void)y; return 0.0f; }
float VoxelRelief_CellShift(const VoxelMapInstance *inst, int x, int y) { (void)inst, (void)x, (void)y; return 0.0f; }
int VoxelWorld_GetMetatileId(int worldX, int worldY) { (void)worldX, (void)worldY; return 0; }
int main(int argc, char **argv)
{
    unsigned width, height, rows;
    uint16_t pixels[64];
    uint8_t mask[64];
    assert(argc == 2);
    if (argv[1][0] == 'r')
        return VoxelBuildings_Init() ? 1 : 0;
    assert(VoxelBuildings_Init());
    assert(VoxelBuildings_PageSize(0, &width, &height, &rows) && width == 8 && height == 8 && rows == 8);
    assert(!VoxelBuildings_PageSize(1, &width, &height, &rows));
    assert(VoxelBuildings_ReadPage(0, 0, 64, pixels));
    assert(pixels[0] == 0xFFFF && pixels[63] == 0xFFFF);
    assert(!VoxelBuildings_ReadPage(0, UINT32_MAX, 2, pixels));
    assert(VoxelBuildings_PageHasGlow(0) == (argv[1][0] != '0'));
    assert(!VoxelBuildings_PageHasGlow(1));
    if (argv[1][0] == '1')
    {
        assert(VoxelBuildings_ReadGlow(0, 0, 64, mask));
        for (unsigned index = 0; index < 64; ++index) assert(mask[index] == index);
        assert(VoxelBuildings_ReadGlow(0, 17, 9, mask) && mask[0] == 17 && mask[8] == 25);
        assert(!VoxelBuildings_ReadGlow(0, UINT32_MAX, 2, mask));
        assert(!VoxelBuildings_ReadGlow(1, 0, 1, mask));
    }
    else if (argv[1][0] == '2') assert(!VoxelBuildings_ReadGlow(0, 0, 64, mask));
    else assert(!VoxelBuildings_ReadGlow(0, 0, 1, mask));
    VoxelBuildings_Shutdown();
    assert(!VoxelBuildings_PageHasGlow(0));
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        (temporary / "test.c").write_text(source, encoding="utf-8")
        subprocess.run([
            cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
            "-fdata-sections", *sanitize(cc), "-DVOXEL_HOST_FILES",
            "-DPORT_LOG(...)=((void)0)", '-DVOXEL_BUILDINGS_PATH="buildings.bin"',
            "-I" + str(ROOT / "src/voxel"), "-I" + str(ROOT / "include"),
            str(temporary / "test.c"), str(ROOT / "src/voxel/voxel_building.c"),
            "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
            "-o", str(temporary / "test"), "-lm",
        ], check=True)
        # VXB7, before the u16 decks and the pages of their own rows, is not read
        for magic, flag, truncated in ((b"VXB7", False, False), (b"VXB8", False, False),
                                       (b"VXB8", True, False), (b"VXB8", True, True)):
            header = magic + struct.pack("<HHHHHHIHH", 1, 0, 0, 0, 0, 0, 0, 0, 0)
            page = struct.pack("<HHI", 8, 8 | (0x8000 if flag else 0), 32)
            data = struct.pack("<64H", *([0xFFFF] * 64))
            if flag and not truncated:
                data += bytes(range(64))
            (temporary / "buildings.bin").write_bytes(header + page + data)
            subprocess.run([str(temporary / "test"), "r" if magic != b"VXB8" else
                            "1" if flag and not truncated else "2" if flag else "0"],
                           cwd=temporary, check=True)


def extract(text, start):
    """The C definition at `start` through its closing brace."""
    position = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[position] == "{") - (text[position] == "}")
        position += 1
    return text[start:position]


def glow_arena_tests(cc):
    """The window masks live in the page arena: taken only while wanted and
    where they fit, never drawn from half in, freed two frames after the last
    draw that could read them, and given up before any page is."""
    text = (ROOT / "src/voxel/ctr_voxel.c").read_text(encoding="utf-8")
    slot_end = text.index("} BuildingPageSlot;")
    slot = text[text.rindex("typedef struct", 0, slot_end):slot_end] + "} BuildingPageSlot;"
    functions = [extract(text, text.index(signature)) for signature in (
        "static void PageTexInit(", "static bool StreamHolds(", "static void GlowFree(",
        "static void GlowDrop(", "static bool GlowRelease(", "static bool GlowDropOthers(",
        "static void GlowWant(", "static bool GlowPending(", "static C3D_Tex *GlowTex(")]
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "voxel_arena.h"
typedef uint16_t u16;
enum { GPU_RGBA5551, GPU_L8, GPU_TEX_2D, GPU_NEAREST, GPU_CLAMP_TO_EDGE, CTR_LOG_VIDEO };
#define GPU_TEXTURE_MODE(mode) (mode)
typedef struct { void *data; unsigned fmt, size; u16 width, height; unsigned param, maxLevel; } C3D_Tex;
static void C3D_TexSetFilter(C3D_Tex *tex, int min, int mag) { (void)tex; (void)min; (void)mag; }
static void C3D_TexSetWrap(C3D_Tex *tex, int first, int second) { (void)tex; (void)first; (void)second; }
#define VOXEL_BUILDING_PAGES 4
#define VOXEL_MAX_PAGES 256
#define VOXEL_PAGE_RETRY_FRAMES 120u
enum { STREAM_IDLE, STREAM_READING, STREAM_DONE, STREAM_FAILED };
'''
    source += slot + r'''
static BuildingPageSlot sPageSlots[VOXEL_BUILDING_PAGES];
static struct { BuildingPageSlot *slot; int state; bool copyQueued; } sStream;
static uint32_t sFrame;
static bool sGlowWanted, sGlowNoRoomLogged[VOXEL_MAX_PAGES];
static VoxelArena sPageArena;
static unsigned sLogs;
static void CtrLog_Write(int kind, const char *format, ...) { (void)kind; (void)format; ++sLogs; }
/* page 3 has no glass */
static bool VoxelBuildings_PageHasGlow(unsigned page) { return page != 3; }
'''
    source += "\n".join(functions)
    source += r'''
static uint8_t sMemory[96 * 1024] __attribute__((aligned(128)));
static VoxelArenaPiece sPieces[VOXEL_BUILDING_PAGES * 2];

/* A page of `w` x `h` texels in its texture of `th` rows, all of them in. */
static BuildingPageSlot *Page(unsigned index, int page, unsigned w, unsigned h, unsigned th)
{
    BuildingPageSlot *slot = &sPageSlots[index];
    void *block = VoxelArena_Alloc(&sPageArena, w * th * 2u, false);

    assert(block != NULL);
    slot->page = page;
    slot->w = w;
    slot->h = h;
    PageTexInit(&slot->tex, block, w, th);
    slot->loaded = w * h;
    slot->used = sFrame;
    return slot;
}

int main(void)
{
    BuildingPageSlot *a, *b, *c;
    void *rest[2] = {NULL, NULL}, *mask;

    VoxelArena_Init(&sPageArena, sMemory, sizeof(sMemory), 128, sPieces, VOXEL_BUILDING_PAGES * 2);
    for (unsigned i = 0; i < VOXEL_BUILDING_PAGES; ++i)
        sPageSlots[i].page = -1;
    sFrame = 10;
    a = Page(0, 0, 64, 40, 64);          /* 8 KiB of texels */

    /* Not wanted (DAY / NIGHT off, indoors, daytime): nothing taken. */
    GlowWant(a);
    assert(a->glow.data == NULL && GlowTex(a) == NULL);

    /* Wanted: an L8 texture as tall as the page's, from the arena. */
    sGlowWanted = true;
    GlowWant(a);
    assert(a->glow.data != NULL && a->glow.fmt == GPU_L8 && a->glow.size == 64u * 64u);
    assert(a->glow.width == 64 && a->glow.height == 64 && a->glow.param == GPU_TEX_2D);
    assert((uint8_t *)a->glow.data >= sMemory && (uintptr_t)a->glow.data % 128u == 0);
    assert(sPageArena.used == 64u * 64u * 2u + 64u * 64u);
    mask = a->glow.data;
    GlowWant(a);                          /* once */
    assert(a->glow.data == mask && sPageArena.count == 2);

    /* Streamed in after the texels; never drawn from half in. */
    assert(GlowPending(a) && GlowTex(a) == NULL);
    a->loaded = 0;
    assert(!GlowPending(a));
    a->loaded = 64u * 40u;
    a->glowLoaded = 64u * 40u - 512u;
    assert(GlowPending(a) && GlowTex(a) == NULL);
    a->glowLoaded = 64u * 40u;
    assert(!GlowPending(a));
    sFrame = 11;
    assert(GlowTex(a) == &a->glow && a->glowUsed == 11);

    /* A page without glass takes nothing. */
    b = Page(1, 3, 64, 64, 64);
    GlowWant(b);
    assert(b->glow.data == NULL);

    /* Dropped: drawn from no more at once, its piece kept while a queued
     * draw or the stream may still read it, freed two frames on. */
    GlowDrop(a);
    assert(GlowTex(a) == NULL && !GlowPending(a) && a->glow.data != NULL);
    sFrame = 12;
    assert(!GlowRelease(false) && a->glow.data != NULL);
    sFrame = 13;
    sStream.slot = a;
    sStream.state = STREAM_READING;
    assert(!GlowRelease(false) && a->glow.data != NULL);
    sStream.state = STREAM_IDLE;
    sStream.copyQueued = true;
    assert(!GlowRelease(false) && a->glow.data != NULL);
    sStream.copyQueued = false;
    assert(GlowRelease(false) && a->glow.data == NULL && a->glowLoaded == 0 && !a->glowDropping);
    assert(sPageArena.used == 64u * 64u * 2u * 2u);

    /* ...and not asked for again at once: the room was wanted. */
    GlowWant(a);
    assert(a->glow.data == NULL);
    sFrame += VOXEL_PAGE_RETRY_FRAMES;
    GlowWant(a);
    assert(a->glow.data != NULL && a->glowLoaded == 0);

    /* For a page: idle masks go first, drawn ones stay. */
    a->glowLoaded = 64u * 40u;
    assert(GlowTex(a) == &a->glow);
    ++sFrame;
    assert(!GlowRelease(true) && a->glow.data != NULL);
    sFrame += 2;
    assert(GlowRelease(true) && a->glow.data == NULL);

    /* No room: no page pushed out, asked for again a while later, the
     * line written once. */
    c = Page(2, 2, 128, 128, 128);        /* 32 KiB */
    for (unsigned i = 0; VoxelArena_LargestGap(&sPageArena) != 0; ++i)
        assert(i < 2 && (rest[i] = VoxelArena_Alloc(&sPageArena,
                                                    VoxelArena_LargestGap(&sPageArena), false)));
    sFrame += VOXEL_PAGE_RETRY_FRAMES;
    sLogs = 0;
    GlowWant(c);
    assert(c->glow.data == NULL && sLogs == 1 && c->glowRetry == sFrame + VOXEL_PAGE_RETRY_FRAMES);
    assert(c->tex.data != NULL && sPageSlots[0].tex.data != NULL && sPageSlots[1].tex.data != NULL);
    sFrame += VOXEL_PAGE_RETRY_FRAMES;
    GlowWant(c);
    assert(c->glow.data == NULL && sLogs == 1);
    VoxelArena_Free(&sPageArena, rest[0]);
    VoxelArena_Free(&sPageArena, rest[1]);
    sFrame += VOXEL_PAGE_RETRY_FRAMES;
    GlowWant(c);
    assert(c->glow.data != NULL && c->glow.size == 128u * 128u);

    /* A page in view finding no room: every other mask stops being drawn. */
    GlowWant(a);
    assert(a->glow.data != NULL);
    assert(GlowDropOthers(c));
    assert(a->glowDropping && !c->glowDropping && GlowTex(a) == NULL);
    sFrame += 2;
    GlowRelease(false);
    assert(a->glow.data == NULL && c->glow.data != NULL);

    /* No longer wanted (dawn, indoors): dropped like the rest. */
    sFrame += VOXEL_PAGE_RETRY_FRAMES;
    GlowWant(a);
    assert(a->glow.data != NULL);
    sGlowWanted = false;
    GlowWant(a);
    assert(a->glowDropping && GlowTex(a) == NULL);
    sFrame += 2;
    GlowRelease(false);
    assert(a->glow.data == NULL);

    /* Freeing a page frees its mask. */
    sGlowWanted = true;
    sFrame += VOXEL_PAGE_RETRY_FRAMES;
    GlowWant(a);
    assert(a->glow.data != NULL);
    GlowFree(a);
    GlowFree(c);
    assert(a->glow.data == NULL && c->glow.data == NULL);
    VoxelArena_Free(&sPageArena, a->tex.data);
    VoxelArena_Free(&sPageArena, b->tex.data);
    VoxelArena_Free(&sPageArena, c->tex.data);
    assert(sPageArena.used == 0 && sPageArena.count == 0);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        (temporary / "glow.c").write_text(source, encoding="utf-8")
        subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        *sanitize(cc), "-I" + str(ROOT / "src/voxel"),
                        str(temporary / "glow.c"), str(ROOT / "src/voxel/voxel_arena.c"),
                        "-o", str(temporary / "glow")], check=True)
        subprocess.run([str(temporary / "glow")], check=True)
    # one pool: the masks' pieces are counted in the arena's, and no other
    # memory is claimed for them
    assert "VoxelArena_Init(&sPageArena" in text
    assert text.count("VoxelArena_Init(&sPageArena") == text.count("sPagePieces, VOXEL_PAGE_PIECES)")
    assert "C3D_TexDelete(&slot->glow)" not in text
    assert "C3D_TexInit(&victim->glow" not in text
    assert "linearAlloc(VOXEL_GLOW" not in text
    print("PASS window masks: page arena, wanted only while lit, streamed whole, freed after use, pages first")


def renderer_tests(cc):
    text = (ROOT / "src/voxel/ctr_voxel.c").read_text(encoding="utf-8")
    functions = []
    for name in ("MoonlightColour", "TerrainTexEnv", "ReleaseGlowUnit", "HazeTexEnv", "FadeColour",
                 "FadeTexEnv", "BuildingGlowEnv", "LampTexEnv"):
        result_type = "uint32_t" if name in ("MoonlightColour", "FadeColour") else "void"
        start = text.index("static " + result_type + " " + name + "(")
        position = text.index("{", start) + 1
        depth = 1
        while depth:
            depth += (text[position] == "{") - (text[position] == "}")
            position += 1
        functions.append(text[start:position])
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
typedef struct { unsigned placeholder; } C3D_Tex;
typedef struct { int rgb[3], alpha[3], alphaOp[3], rgbOp[3], rgbFunc, alphaFunc; uint32_t colour; } C3D_TexEnv;
enum { C3D_RGB, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE2, GPU_CONSTANT, GPU_PREVIOUS,
       GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_MODULATE, GPU_TEVOP_RGB_SRC_COLOR,
       GPU_TEVOP_RGB_SRC_ALPHA, GPU_INTERPOLATE, GPU_REPLACE, GPU_PRIMARY_COLOR, GPU_TEVSCALE_2,
       GPU_TEXTURE1, GPU_MULTIPLY_ADD };
enum { GPU_BLEND_ADD = 1, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_COLOR, GPU_ONE, GPU_ZERO };
static int sBlend[6];
static void C3D_AlphaBlend(int colorEq, int alphaEq, int srcClr, int dstClr, int srcAlpha, int dstAlpha)
{
    sBlend[0] = colorEq; sBlend[1] = alphaEq; sBlend[2] = srcClr;
    sBlend[3] = dstClr; sBlend[4] = srcAlpha; sBlend[5] = dstAlpha;
}
typedef struct { float dappleLow, dappleHigh; } VoxelLight;
static float sMoonlight;
static uint32_t sMoonlightColour;
static uint32_t DappleConstant(const VoxelLight *light) { (void)light; return 0x12345678u; }
static float sHazeRgb[3];
static uint8_t __C3D_Context[512] __attribute__((aligned(16)));
#define C3DI_CONTEXT_FLAGS 0x20u
#define C3DI_CONTEXT_TEX 0x118u
#define C3DI_FLAG_TEX(unit) (1u << (23 + (unit)))
static C3D_Tex sTreeAtlas, *sGlowBound;
static C3D_TexEnv sEnvs[6];
static int sBinds;
static int sAlphaScale;
static C3D_TexEnv *C3D_GetTexEnv(unsigned index) { return &sEnvs[index]; }
static void C3D_TexBind(int unit, C3D_Tex *texture)
{
    assert(texture != NULL && unit == 2);
    ((C3D_Tex **)(void *)(__C3D_Context + C3DI_CONTEXT_TEX))[unit] = texture;
    ++sBinds;
}
static void C3D_TexEnvSrc(C3D_TexEnv *env, int mode, int first, int second, int third)
{
    int *values = mode == C3D_RGB ? env->rgb : env->alpha;
    values[0] = first; values[1] = second; values[2] = third;
}
static void C3D_TexEnvOpAlpha(C3D_TexEnv *env, int first, int second, int third)
{ env->alphaOp[0] = first; env->alphaOp[1] = second; env->alphaOp[2] = third; }
static void C3D_TexEnvOpRgb(C3D_TexEnv *env, int first, int second, int third)
{ env->rgbOp[0] = first; env->rgbOp[1] = second; env->rgbOp[2] = third; }
static void C3D_TexEnvFunc(C3D_TexEnv *env, int mode, int function)
{ if (mode == C3D_RGB) env->rgbFunc = function; else env->alphaFunc = function; }
static void C3D_TexEnvColor(C3D_TexEnv *env, uint32_t colour) { env->colour = colour; }
static void C3D_TexEnvInit(C3D_TexEnv *env) { memset(env, 0, sizeof(*env)); sAlphaScale = 0; }
static void C3D_TexEnvScale(C3D_TexEnv *env, int mode, int scale)
{ (void)env; if (mode == C3D_Alpha) sAlphaScale = scale; }
'''
    source += re.search(r"#define VOXEL_LAMP_POOL_GAIN .*", text).group(0) + "\n"
    source += re.search(r"typedef struct\s*\{\s*float amount, rgb\[3\];\s*\} VoxelFade;", text).group(0) + "\n"
    source += "static VoxelFade sWorldFade;\n"
    source += "\n".join(functions)
    source += r'''
int main(void)
{
    VoxelLight dapple = {0.96f, 1.04f};
    TerrainTexEnv(NULL);
    assert(sEnvs[0].rgbFunc == GPU_MODULATE && sEnvs[0].alphaFunc == GPU_REPLACE);
    sMoonlight = 0.06f;
    sMoonlightColour = 0xFF040302u;
    assert(MoonlightColour(0.0f) == 0xFF040302u);
    assert(MoonlightColour(0.5f) == 0xFF020201u);
    assert(MoonlightColour(1.0f) == 0xFF000000u);
    TerrainTexEnv(NULL);
    assert(sEnvs[0].rgbFunc == GPU_MULTIPLY_ADD && sEnvs[0].rgb[2] == GPU_CONSTANT);
    assert(sEnvs[0].colour == sMoonlightColour && sEnvs[0].alpha[0] == GPU_TEXTURE0);
    TerrainTexEnv(&dapple);
    assert(sEnvs[0].rgbFunc == GPU_INTERPOLATE && sEnvs[0].colour == 0x12345678u);
    assert(sEnvs[2].rgbFunc == GPU_MULTIPLY_ADD && sEnvs[2].colour == sMoonlightColour);
    assert(sEnvs[2].alphaFunc == GPU_REPLACE && sEnvs[2].alpha[0] == GPU_PREVIOUS);
    C3D_Tex mask, other;
    C3D_Tex **bound = (C3D_Tex **)(void *)(__C3D_Context + C3DI_CONTEXT_TEX);
    uint32_t *flags = (uint32_t *)(void *)(__C3D_Context + C3DI_CONTEXT_FLAGS);
    BuildingGlowEnv(NULL, 1.0f, NULL);
    BuildingGlowEnv(&mask, 0.0f, NULL);
    assert(sBinds == 0 && sGlowBound == NULL);
    BuildingGlowEnv(&mask, 0.5f, NULL);
    assert(sBinds == 1 && bound[2] == &mask && sGlowBound == &mask);
    assert(sEnvs[1].alpha[0] == GPU_TEXTURE2 && sEnvs[1].alpha[1] == GPU_CONSTANT);
    assert(sEnvs[1].alphaOp[0] == GPU_TEVOP_A_SRC_R);
    assert(sEnvs[1].alphaFunc == GPU_MODULATE && sEnvs[1].colour == 0x80000000u);
    assert(sEnvs[2].rgbFunc == GPU_INTERPOLATE && sEnvs[2].rgb[0] == GPU_CONSTANT);
    assert(sEnvs[2].rgb[1] == GPU_PREVIOUS && sEnvs[2].rgbOp[2] == GPU_TEVOP_RGB_SRC_ALPHA);
    assert(sEnvs[2].alpha[0] == GPU_TEXTURE0 && sEnvs[2].alphaFunc == GPU_REPLACE);
    assert(sEnvs[2].colour == 0xFFA0D8FFu);
    assert(sEnvs[0].rgbFunc == GPU_MULTIPLY_ADD && sEnvs[0].colour == sMoonlightColour);
    assert(sEnvs[3].colour == 0 && sEnvs[4].colour == 0);
    ReleaseGlowUnit();
    assert(bound[2] == NULL && sGlowBound == NULL && *flags == C3DI_FLAG_TEX(2));
    BuildingGlowEnv(&mask, 1.0f, NULL);
    bound[2] = &other;
    ReleaseGlowUnit();
    assert(bound[2] == &sTreeAtlas && sGlowBound == NULL);
    BuildingGlowEnv(&mask, 1.0f, NULL);
    BuildingGlowEnv(NULL, 1.0f, NULL);
    assert(bound[2] == NULL && sGlowBound == NULL && sEnvs[2].colour == 0);
    /* the glow over the dapples: their stages 0-2 kept, the glow in 3, the
       haze and the fade moved on to 4 and 5 */
    sHazeRgb[0] = 0.25f; sHazeRgb[1] = 0.5f; sHazeRgb[2] = 1.0f;
    sWorldFade.amount = 0.5f;
    sWorldFade.rgb[0] = 1.0f;
    BuildingGlowEnv(&mask, 0.5f, &dapple);
    assert(bound[2] == &mask && sGlowBound == &mask);
    assert(sEnvs[0].rgbFunc == GPU_INTERPOLATE && sEnvs[0].rgb[0] == GPU_TEXTURE1);
    assert(sEnvs[0].colour == 0x12345678u && sEnvs[0].alpha[0] == GPU_TEXTURE0);
    assert(sEnvs[1].rgbFunc == GPU_MODULATE && sEnvs[1].rgb[0] == GPU_PREVIOUS);
    assert(sEnvs[1].rgb[1] == GPU_PRIMARY_COLOR);
    assert(sEnvs[1].alpha[0] == GPU_TEXTURE2 && sEnvs[1].alpha[1] == GPU_CONSTANT);
    assert(sEnvs[1].alphaOp[0] == GPU_TEVOP_A_SRC_R);
    assert(sEnvs[1].alphaFunc == GPU_MODULATE && sEnvs[1].colour == 0x80000000u);
    assert(sEnvs[2].rgbFunc == GPU_MULTIPLY_ADD && sEnvs[2].rgb[0] == GPU_TEXTURE0);
    assert(sEnvs[2].rgb[1] == GPU_PREVIOUS && sEnvs[2].colour == sMoonlightColour);
    assert(sEnvs[2].alpha[0] == GPU_PREVIOUS && sEnvs[2].alphaFunc == GPU_REPLACE);
    assert(sEnvs[3].rgbFunc == GPU_INTERPOLATE && sEnvs[3].rgb[0] == GPU_CONSTANT);
    assert(sEnvs[3].rgb[1] == GPU_PREVIOUS && sEnvs[3].rgbOp[2] == GPU_TEVOP_RGB_SRC_ALPHA);
    assert(sEnvs[3].alpha[0] == GPU_TEXTURE0 && sEnvs[3].alphaFunc == GPU_REPLACE);
    assert(sEnvs[3].colour == 0xFFA0D8FFu);
    assert(sEnvs[4].rgbFunc == GPU_INTERPOLATE && sEnvs[4].rgb[0] == GPU_PREVIOUS);
    assert(sEnvs[4].rgb[2] == GPU_PRIMARY_COLOR && sEnvs[4].colour == FadeColour(1.0f, sHazeRgb));
    assert(sEnvs[5].rgbFunc == GPU_INTERPOLATE && sEnvs[5].rgb[1] == GPU_PREVIOUS);
    assert(sEnvs[5].colour == FadeColour(0.5f, sWorldFade.rgb));
    /* a page without a mask, over the dapples: haze and fade back in 3 and 4 */
    BuildingGlowEnv(NULL, 1.0f, &dapple);
    assert(bound[2] == NULL && sGlowBound == NULL);
    assert(sEnvs[0].rgbFunc == GPU_INTERPOLATE && sEnvs[0].colour == 0x12345678u);
    assert(sEnvs[1].alpha[0] == GPU_PREVIOUS && sEnvs[2].alpha[0] == GPU_PREVIOUS);
    assert(sEnvs[3].rgb[2] == GPU_PRIMARY_COLOR && sEnvs[3].colour == FadeColour(1.0f, sHazeRgb));
    assert(sEnvs[4].colour == FadeColour(0.5f, sWorldFade.rgb));
    assert(sEnvs[5].rgbFunc == 0 && sEnvs[5].colour == 0);
    /* the glow: a flat colour over the scene by its alpha */
    LampTexEnv(1.0f, false);
    assert(sEnvs[0].rgbFunc == GPU_REPLACE && sEnvs[0].alphaFunc == GPU_REPLACE);
    assert(sEnvs[0].colour == 0xDC6BBFFFu && sAlphaScale == 0);
    assert(sBlend[2] == GPU_SRC_ALPHA && sBlend[3] == GPU_ONE_MINUS_SRC_ALPHA);
    /* the pool: the falloff texture times a warm colour, added to the
       ground in proportion to it (dst * src + dst) */
    LampTexEnv(1.0f, true);
    assert(sEnvs[0].rgb[0] == GPU_TEXTURE0 && sEnvs[0].rgbFunc == GPU_MODULATE);
    assert(sEnvs[0].alphaFunc == GPU_REPLACE && sAlphaScale == 0);
    assert(sBlend[0] == GPU_BLEND_ADD && sBlend[2] == GPU_DST_COLOR && sBlend[3] == GPU_ONE);
    assert(sEnvs[0].colour == (0xFF000000u | (uint32_t)(VOXEL_LAMP_POOL_GAIN * 60.0f + 0.5f) << 16
                               | (uint32_t)(VOXEL_LAMP_POOL_GAIN * 150.0f + 0.5f) << 8
                               | (uint32_t)(VOXEL_LAMP_POOL_GAIN * 255.0f + 0.5f)));
    LampTexEnv(0.0f, true);
    assert((sEnvs[0].colour & 0x00FFFFFFu) == 0);
    sMoonlight = 0.0f;
    TerrainTexEnv(NULL);
    assert(sEnvs[0].rgbFunc == GPU_MODULATE && sEnvs[2].colour == 0);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        (temporary / "render.c").write_text(source, encoding="utf-8")
        subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        *sanitize(cc), str(temporary / "render.c"),
                        "-o", str(temporary / "render")], check=True)
        subprocess.run([str(temporary / "render")], check=True)
    shader = (ROOT / "src/voxel/voxel.v.pica").read_text(encoding="utf-8")
    assert ".out outtc2 texcoord2" in shader and "mov outtc2, intex" in shader
    assert "C3D_TexBind(2, NULL)" not in text


def compositor_tests(cc):
    video = (ROOT / "src/3ds_video.c").read_text(encoding="utf-8")
    start = video.index("    if (night)\n", video.index("static void Blend("))
    end = video.index("    /* A semi-transparent sprite", start)
    cycle = (ROOT / "src/3ds_daynight.c").read_text(encoding="utf-8")
    moon = cycle[cycle.index("CtrDayNightBlend CtrDayNight_Moonlight("):
                 cycle.index("static unsigned SunKey(")]
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "3ds_daynight.h"
typedef struct { uint32_t colour; int function; } C3D_TexEnv;
enum { C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_MODULATE, GPU_INTERPOLATE,
       GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA };
static C3D_TexEnv sEnvs[6];
static bool sDayNightEnv;
static int sTint;
static float sPaletteFade;
static unsigned sBrightness;
static unsigned Reg(unsigned address) { assert(address == 0x54); return sBrightness; }
static unsigned Min(unsigned first, unsigned second) { return first < second ? first : second; }
static uint32_t C2D_Color32(unsigned red, unsigned green, unsigned blue, unsigned alpha)
{ return (red & 255u) | ((green & 255u) << 8) | ((blue & 255u) << 16) | ((alpha & 255u) << 24); }
static C3D_TexEnv *C3D_GetTexEnv(unsigned index) { return &sEnvs[index]; }
static void C3D_TexEnvSrc(C3D_TexEnv *env, int mode, int first, int second, int third)
{ (void)env; (void)mode; (void)first; (void)second; (void)third; }
static void C3D_TexEnvOpRgb(C3D_TexEnv *env, int first, int second, int third)
{ (void)env; (void)first; (void)second; (void)third; }
static void C3D_TexEnvFunc(C3D_TexEnv *env, int mode, int function)
{ (void)mode; env->function = function; }
static void C3D_TexEnvColor(C3D_TexEnv *env, uint32_t colour) { env->colour = colour; }
static void C2D_PlainImageTint(int *tint, uint32_t colour, float amount)
{ (void)tint; (void)colour; assert(amount == 0); }
'''
    source += moon
    source += '''
static void Compose(bool night, const CtrDayNightLight *light, bool paletteTint,
                    bool effects, unsigned control, unsigned effect, unsigned layer,
                    uint32_t fadeTo)
{
''' + video[start:end] + r'''
}
int main(void)
{
    CtrDayNightLight light = {{0.66f, 0.74f, 0.92f}, 0, 1};
    Compose(false, &light, false, false, 0, 0, 0, 0);
    assert(!sDayNightEnv && sEnvs[4].colour == 0 && sEnvs[5].colour == 0);
    Compose(true, &light, false, false, 0, 0, 0, 0);
    assert(sDayNightEnv && sEnvs[4].function == GPU_MODULATE);
    assert(sEnvs[5].function == GPU_INTERPOLATE && sEnvs[5].colour == 0x0F885C46u);
    sPaletteFade = 1;
    Compose(true, &light, true, false, 0, 0, 0, 0);
    assert(sEnvs[5].colour == 0xFF000000u);
    Compose(true, &light, true, false, 0, 0, 0, 0xFFFFFFFFu);
    assert(sEnvs[5].colour == 0xFFFFFFFFu);
    Compose(true, &light, false, false, 0, 0, 4, 0);
    assert((sEnvs[5].colour >> 24) == 0);
    sPaletteFade = 0;
    sBrightness = 16;
    Compose(true, &light, false, true, 1, 2, 0, 0);
    assert(sEnvs[5].colour == 0xFFFFFFFFu);
    Compose(true, &light, false, true, 1, 3, 0, 0);
    assert(sEnvs[5].colour == 0xFF000000u);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        (temporary / "compositor.c").write_text(source, encoding="utf-8")
        subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        *sanitize(cc), "-I" + str(ROOT / "include"),
                        str(temporary / "compositor.c"), "-o", str(temporary / "compositor")],
                       check=True)
        subprocess.run([str(temporary / "compositor")], check=True)


def asset_tests(path):
    data = path.read_bytes()
    assert data[:4] in (b"VXB8", b"VXB9")
    head = 28 if data[:4] == b"VXB9" else 24    # VXB9: the conditions' count
    page_count = struct.unpack_from("<H", data, 4)[0]
    lit_count = 0
    for index in range(page_count):
        width, flagged_height, offset = struct.unpack_from("<HHI", data, head + index * 8)
        height = flagged_height & 0x7FFF
        pixels = width * height
        assert offset + pixels * 2 <= len(data)
        if flagged_height & 0x8000:
            lit_count += 1
            mask = data[offset + pixels * 2:offset + pixels * 3]
            assert len(mask) == pixels and any(mask)
            for pixel, value in enumerate(mask):
                if value:
                    assert struct.unpack_from("<H", data, offset + pixel * 2)[0] & 1
    assert lit_count > 0
    print("PASS night assets: %d glowing pages, opaque glass only" % lit_count)


def production_tests(tree):
    sys.path.insert(0, str(tree / "3ds_port/scripts"))
    import gen_voxel_buildings as generator
    import voxel_building as building
    import voxel_building_specs as specs

    checked = 0
    for spec in specs.SPECS:
        expanded = generator.kit_specs(spec) if "kit" in spec else [spec]
        for current in expanded:
            name = current["name"]
            if name not in NIGHT_WINDOWS and not name.startswith(("rustboro_stone_", "rustboro_olive_")):
                continue
            layout = building.LayoutArt(current["layout"])
            xpos, ypos, width, height = current["rect"]
            art = layout.building_art(xpos, ypos, width, height,
                                      layout.ground_tiles(current["ground"]))
            original = art.tobytes()
            mask = building_mask(art, name)
            assert any(mask), name
            assert art.tobytes() == original
            for index, value in enumerate(mask):
                if value:
                    assert art.getpixel((index % art.width, index // art.width))[3] >= 128
            if name.startswith(("rustboro_stone_", "rustboro_olive_")):
                assert not any(mask[:48 * art.width]), name
            assert building_mask(art, name + "_bare") == mask
            checked += 1
    assert checked == 22
    print("PASS production city windows: %d exterior models, opaque panes, unchanged art, dark roofs" % checked)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--tree", type=Path)
    arguments = parser.parse_args()
    mask_tests()
    reader_tests(arguments.cc)
    glow_arena_tests(arguments.cc)
    renderer_tests(arguments.cc)
    compositor_tests(arguments.cc)
    if arguments.assets:
        asset_tests(arguments.assets)
    if arguments.tree:
        production_tests(arguments.tree)
    print("PASS night windows: masks, unchanged art, VXB8 pages only, streaming slices, truncation, bounds")


if __name__ == "__main__":
    main()
