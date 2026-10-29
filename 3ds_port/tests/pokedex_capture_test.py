"""Exercise native capture tasks, the layout bridge and software-rasterized rendering."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "patches/pokeemerald/0054-pokedex-capture-layout.patch"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def rendering_source(video, header):
    enum_start = header.rfind("enum\n{", 0, header.index("CTR_CENTRED_NONE"))
    enum_end = header.index("};", enum_start) + 2
    fill_end = video.index("int CtrPokenavList_Bg(void);")
    fill_start = video.rfind("typedef struct", 0, fill_end)
    empty_start = video.rfind("typedef struct", 0, video.index("} EmptyWalk;"))
    empty_end = video.index("static bool EmptyWalkHolds(", empty_start)
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint16_t u16;
#define CTR_GAME_WIDTH 400
#define CTR_GAME_HEIGHT 240
#define CTR_STAGE_Y 40
#define BOTTOM_WIDTH 320
#define CTR_VIEW_X 80
#define CTR_VIEW_Y 40
#define CTR_BOTTOM_WIN_X 40
#define CTR_BOTTOM_WIN_Y 40
#define VIEW_LEFT (-80)
#define VIEW_RIGHT 320
#define VIEW_TOP (-40)
#define VIEW_BOTTOM 200
#define CTR_LINE_REG_FIRST 0x10
#define CTR_LINE_REGS 35
#define GPU_SCISSOR_DISABLE 0
#define C2D_TopLeft 0
#define C2D_TopRight 1
#define C2D_BotLeft 2
#define C2D_BotRight 3
'''
    source += header[enum_start:enum_end] + "\n"
    source += r'''
static unsigned sCentredScreen;
static bool sCentred = true, sStage, sBattle, sFieldBanner, sScissored, textured;
static int sClipX0, sClipX1, sClipY0, sClipY1, sSpanShiftX, sSpanShiftY, sLayerShift;
static unsigned sLayerExclude, sLineMask;
static uint16_t registers[48], lineRegisters[CTR_LINE_REGS], tilemaps[2][1024];
static const uint16_t *sRegLine;
static struct { const uint16_t *regs; } sMemory = {registers};
static struct { unsigned tiles, frames; } sStats;
static float sZoom = 1, sOffX, sOffY;
typedef struct { unsigned width, height, bg; } Texture;
typedef struct { Texture tex; } LayerTexture;
typedef struct { u16 width, height; float left, top, right, bottom; } Tex3DS_SubTexture;
typedef struct { unsigned unused; } C2D_ImageTint;
typedef struct { Texture *tex; const Tex3DS_SubTexture *subtex; } C2D_Image;
static LayerTexture sLayers[4];
static C2D_ImageTint sTint;
static uint32_t pixels[240][320], reference[240][320];
static unsigned backdropQueries;
static int CtrPokedex_Backdrop(void) { ++backdropQueries; return -1; }
static int CtrPokenavList_Bg(void) { return -1; }
static void ViewBase(void) {}
static void C2D_Flush(void) {}
static void RestoreScissor(void) {}
static void VramSyncRange(unsigned address, unsigned bytes) { (void)address; (void)bytes; }
static void C3D_SetScissor(int mode, int left, int top, int right, int bottom) {
    (void)mode; (void)left; (void)top; (void)right; (void)bottom;
}
static void ClipToView(void) {
    sClipX0 = VIEW_LEFT; sClipX1 = VIEW_RIGHT;
    sClipY0 = VIEW_TOP; sClipY1 = VIEW_BOTTOM;
}
static uint32_t Sample(unsigned bg, int sourceX, int sourceY) {
    return (bg << 24) | ((unsigned)sourceY & 255u) * 256u | ((unsigned)sourceX & 255u);
}
static void Pixel(int screenX, int screenY, uint32_t value) {
    int pictureX = screenX - CTR_VIEW_X, pictureY = screenY - CTR_VIEW_Y;
    if (sScissored && (pictureX < sClipX0 || pictureX >= sClipX1
                   || pictureY < sClipY0 || pictureY >= sClipY1)) return;
    int bottomX = pictureX + 40, bottomY = pictureY + 40;
    if (bottomX >= 0 && bottomX < 320 && bottomY >= 0 && bottomY < 240)
        pixels[bottomY][bottomX] = value;
}
static unsigned CtrVideo_TextMapOffset(unsigned column, unsigned row, unsigned size) {
    assert(size == 0);
    return (((row & 31u) * 32u) + (column & 31u)) * 2u;
}
static unsigned Read16(unsigned address) {
    unsigned bg = address >= 15u * 0x800u ? 1u : 0u;
    unsigned map = (14u + bg) * 0x800u;
    assert(address >= map && address < map + 0x800u);
    return tilemaps[bg][(address - map) / 2u];
}
static int GetTileSlot(unsigned address, unsigned palette, bool color256) {
    (void)palette;
    assert(!color256);
    unsigned bg = address >= 0x8000u ? 2u : 3u;
    return (int)(bg * 1024u + (address - (bg == 2 ? 0x8000u : 0x4000u)) / 32u);
}
/* The field's banks of other tilesets: none on the Pokedex's screens. */
static unsigned TilemapBankAt(unsigned entryAddress, unsigned entry) {
    (void)entryAddress; (void)entry;
    return 0;
}
static int BankedTileSlot(unsigned bank, unsigned entry, unsigned chars) {
    (void)bank; (void)entry; (void)chars;
    abort();
}
static void DrawSlot(int slot, int screenX, int screenY, bool flipX, bool flipY) {
    unsigned bg = (unsigned)slot / 1024u, tile = (unsigned)slot % 1024u;
    for (int row = 0; row < 8; ++row)
        for (int column = 0; column < 8; ++column)
            Pixel(screenX + column, screenY + row, Sample(bg,
                (int)(tile % 32u) * 8 + (flipX ? 7 - column : column),
                (int)(tile / 32u) * 8 + (flipY ? 7 - row : row)));
}
static bool TileWalkBegin(void) { return false; }
static void TileWalkSlot(bool batch, int slot, float screenX, float screenY, bool flipX, bool flipY) {
    (void)batch;
    DrawSlot(slot, (int)screenX, (int)screenY, flipX, flipY);
}
static void TileWalkEnd(bool batch) { (void)batch; }
static void Error(unsigned bit, const char *reason) { (void)bit; (void)reason; abort(); }
static bool LayerDrawable(unsigned bg) { (void)bg; return textured; }
static float LayerBrightness(unsigned bg, bool *white) { (void)bg; *white = false; return 0; }
static float EdgeFade(int column, int row, int top, int bottom) {
    (void)column; (void)row; (void)top; (void)bottom; return 0;
}
static void FadeCorner(C2D_ImageTint *tint, int corner, float fade, float bright, bool white) {
    (void)tint; (void)corner; (void)fade; (void)bright; (void)white;
}
static void C2D_DrawImageAt(C2D_Image image, float screenX, float screenY, float depth,
                          C2D_ImageTint *tint, float scaleX, float scaleY) {
    (void)depth; (void)tint;
    assert(scaleX == 1 && scaleY == 1);
    const Tex3DS_SubTexture *cut = image.subtex;
    for (int row = 0; row < cut->height; ++row)
        for (int column = 0; column < cut->width; ++column) {
            int sourceX = (int)(cut->left * image.tex->width) + column;
            int sourceY = (int)((1 - cut->top) * image.tex->height) + row;
            Pixel((int)screenX + column, (int)screenY + row, Sample(image.tex->bg, sourceX, sourceY));
        }
}
static void DrawStageBg(unsigned bg) { (void)bg; abort(); }
static void DrawBattleText(unsigned bg) { (void)bg; abort(); }
static void DrawBattleBgSplit(unsigned bg) { (void)bg; abort(); }
static void DrawLayerRect(unsigned bg, int x0, int x1, int y0, int y1,
                          float sx, float sy, float dx, float dy, bool fade, int top, int bottom);
static void Compose(void);
'''
    source += video[empty_start:empty_end]
    source += r'''
static bool EmptyWalkHolds(const EmptyWalk *previous, const EmptyWalk *current) {
    (void)previous; (void)current;
    return false;
}
'''
    source += video[fill_start:fill_end]
    for signature in (
        "static unsigned Reg(", "static unsigned Min(",
        "static void DrawTextSpanAt(", "static void DrawTextSpan(",
        "static const CentredFill *CentredFillOf(", "static unsigned CentredLayers(",
        "static void DrawCentredSpan(", "static int FloorDiv8(",
        "static void DrawCentredMargins(", "static bool CentredTexture(",
        "static void DrawCentredPicture(", "static bool DrawWrapMarginsTex(",
        "static bool DrawCentredBg(", "static void DrawTextBg(",
        "static unsigned TextBackgrounds(", "static unsigned LineBackgrounds(",
        "static void DrawLayerRect(unsigned bg, int x0, int x1, int y0, int y1,\n                          float sx, float sy, float dx, float dy, bool fade, int top, int bottom)\n{",
        "static bool BottomWhole(", "static bool BottomScreen(",
        "static void StorageComposePart(", "static void WindowCompose(",
    ):
        source += function(video, signature)
    source += r'''
static void Compose(void) {
    assert(LineBackgrounds() == 0);
    for (int bg = 3; bg >= 2; --bg)
        if (!(sLayerExclude & (1u << bg))) DrawTextBg((unsigned)bg);
    if (!(sLayerExclude & 16u)) {
        Pixel(48 + CTR_VIEW_X, 56 + CTR_VIEW_Y, 0x04000001);
        Pixel(-8 + CTR_VIEW_X, 56 + CTR_VIEW_Y, 0x04000002);
    }
}
static void CheckRendering(unsigned scrollX, unsigned scrollY, bool useTexture, bool scanline) {
    memset(pixels, 0, sizeof(pixels));
    registers[0x18 / 2] = registers[0x1c / 2] = (uint16_t)scrollX;
    registers[0x1a / 2] = registers[0x1e / 2] = (uint16_t)scrollY;
    for (unsigned index = 0; index < CTR_LINE_REGS; ++index) lineRegisters[index] = 511;
    sRegLine = scanline ? lineRegisters : NULL;
    sLineMask = scanline ? 0xf0u : 0;
    textured = useTexture;
    sCentredScreen = CTR_CENTRED_POKEDEX_CAUGHT;
    assert(BottomWhole(sCentredScreen) && BottomScreen(sCentredScreen));
    const CentredFill *fill = CentredFillOf(sCentredScreen);
    assert(CentredLayers(fill) == (1u << 3) && fill->tile && !fill->wrap && !fill->backmost);
    WindowCompose();
    assert(backdropQueries == 0);
    for (int row = 0; row < 240; ++row)
        for (int column = 0; column < 320; ++column) {
            int pictureX = column - 40, pictureY = row - 40;
            bool inside = pictureX >= 0 && pictureX < 240 && pictureY >= 0 && pictureY < 160;
            uint32_t expected = inside ? Sample(2, pictureX, pictureY)
                                      : Sample(3, (pictureX + 256) % 8, 16 + (pictureY + 256) % 8);
            if (pictureX == 48 && pictureY == 56) expected = 0x04000001;
            assert(pixels[row][column] == expected);
        }
    if (useTexture) memcpy(reference, pixels, sizeof(pixels));
    else assert(memcmp(reference, pixels, sizeof(pixels)) == 0);
    assert(registers[0x18 / 2] == scrollX && registers[0x1a / 2] == scrollY);
    assert(registers[0x1c / 2] == scrollX && registers[0x1e / 2] == scrollY);
    sRegLine = NULL;
    sCentredScreen = CTR_CENTRED_POKEDEX;
    assert(!BottomWhole(sCentredScreen) && BottomScreen(sCentredScreen));
    assert(Reg(0x18) == scrollX && Reg(0x1e) == scrollY);
    assert(LineBackgrounds() == (scanline ? 12u : 0));
    assert(CentredFillOf(sCentredScreen)->backmost && backdropQueries != 0);
    backdropQueries = 0;
    sCentredScreen = CTR_CENTRED_NONE;
    assert(Reg(0x18) == scrollX && Reg(0x1e) == scrollY);
}
int main(void) {
    registers[0] = 0x0c00;
    registers[0x0c / 2] = 2u | (2u << 2) | (14u << 8);
    registers[0x0e / 2] = 3u | (1u << 2) | (15u << 8);
    for (unsigned bg = 2; bg < 4; ++bg) {
        sLayers[bg].tex = (Texture){256, 256, bg};
        for (unsigned tile = 0; tile < 1024; ++tile) tilemaps[bg - 2][tile] = (uint16_t)tile;
    }
    const unsigned scrolls[] = {0, 7, 80, 240, 256, 511};
    for (unsigned index = 0; index < sizeof(scrolls) / sizeof(scrolls[0]); ++index)
        for (unsigned scanline = 0; scanline < 2; ++scanline) {
            CheckRendering(scrolls[index], scrolls[index], true, scanline != 0);
            CheckRendering(scrolls[index], scrolls[index], false, scanline != 0);
        }
    puts("PASS Pokédex capture rendering: 320x240 pixels, BG2 text, BG3 stripe-only margins, OAM clipping, texture/tile parity, borrowed scroll, scanlines, field/battle restoration");
    return 0;
}
'''
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--bridge", type=Path, default=ROOT / "3ds_port/src/3ds_game_full.c")
    parser.add_argument("--video", type=Path, default=ROOT / "3ds_port/src/3ds_video.c")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    pokedex = (args.tree / "src/pokedex.c").read_text(encoding="utf-8")
    bridge = args.bridge.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(temporary)
        if "bool8 CtrPokedex_IsCaughtMonPage(void)" not in pokedex:
            (folder / "src").mkdir()
            copied = folder / "src/pokedex.c"
            copied.write_text(pokedex, encoding="utf-8")
            subprocess.run(["git", "apply", "--check", str(PATCH)], cwd=folder, check=True)
            subprocess.run(["git", "apply", str(PATCH)], cwd=folder, check=True)
            pokedex = copied.read_text(encoding="utf-8")
        source = r'''
#define PLATFORM_3DS 1
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef bool bool8;
typedef void (*IntrCallback)(void);
typedef void (*TaskFunc)(u8);
#define TASK_NONE 255
#define CTR_LOG_VIDEO 0
#define CtrLog_Write(...) ((void)0)
#define sFrames 0u
enum { CTR_CENTRED_NONE, CTR_CENTRED_POKEDEX, CTR_CENTRED_PLAIN, CTR_CENTRED_POKEDEX_CAUGHT };
static struct { IntrCallback vblankCallback, callback2; bool inBattle; } gMain;
static struct { bool active; } gPaletteFade;
static struct { TaskFunc func; int16_t data[16]; bool active; } gTasks[16];
static struct { struct { u8 priority, paletteNum; } oam; void (*callback)(void *); } gSprites[1];
static IntrCallback gPokedexVBlankCB;
static bool videoStage, videoStageFaded, videoStageZoomed, videoBattle, videoTransition;
static unsigned videoCentred, newEntryPrints, freedWindows, cries, battleVBlanks;
static u16 newKeys;
static const u32 monPalette[1];
static void BattleVBlank(void) { ++battleVBlanks; }
static void OtherBattleVBlank(void) { ++battleVBlanks; }
static void VBlankCB_Pokedex(void) {}
static void FieldVBlank(void) {}
static void UnrelatedTask(u8 taskId) { (void)taskId; }
static void SpriteCB_SlideCaughtMonToCenter(void *sprite) { (void)sprite; }
static void SetVBlankCallback(IntrCallback callback) { gMain.vblankCallback = callback; }
static void CtrVideo_SetStage(bool active) { videoStage = active; }
static void CtrVideo_SetStageFaded(bool faded) { videoStageFaded = faded; }
static void CtrVideo_SetStageZoomed(bool zoomed) { videoStageZoomed = zoomed; }
static void CB2_OverworldBasic(void) {}
static void CtrVideo_SetCentred(unsigned screen) { videoCentred = screen; }
static void CtrVideo_SetBattle(bool active) { videoBattle = active; }
static void CtrVideo_SetTransition(bool active) { videoTransition = active; }
static void CtrBattleHud_Warm(void) {}
static u8 CreateTask(TaskFunc func, u8 priority) {
    (void)priority;
    for (u8 taskId = 0; taskId < 16; ++taskId) {
        if (!gTasks[taskId].active) {
            memset(&gTasks[taskId], 0, sizeof(gTasks[taskId]));
            gTasks[taskId].func = func;
            gTasks[taskId].active = true;
            return taskId;
        }
    }
    abort();
}
static void DestroyTask(u8 taskId) { gTasks[taskId].active = false; }
static u8 FindTaskIdByFunc(TaskFunc func) {
    for (u8 taskId = 0; taskId < 16; ++taskId)
        if (gTasks[taskId].active && gTasks[taskId].func == func) return taskId;
    return TASK_NONE;
}
static bool8 FuncIsActiveTask(TaskFunc func) { return FindTaskIdByFunc(func) != TASK_NONE; }
#define ResetOtherVideoRegisters(...) ((void)0)
#define ResetBgsAndClearDma3BusyFlags(...) ((void)0)
#define InitBgsFromTemplates(...) ((void)0)
#define SetBgTilemapBuffer(...) ((void)0)
#define InitWindows(...) ((void)0)
#define DeactivateAllTextPrinters(...) ((void)0)
#define DecompressAndLoadBgGfxUsingHeap(...) ((void)0)
#define CopyToBgTilemapBuffer(...) ((void)0)
#define FillWindowPixelBuffer(...) ((void)0)
#define PutWindowTilemap(...) ((void)0)
#define DrawFootprint(...) ((void)0)
#define CopyWindowToVram(...) ((void)0)
#define ResetPaletteFade() ((void)0)
#define LoadPokedexBgPalette(...) ((void)0)
#define PrintMonInfo(dexNum, ...) ((void)(dexNum), ++newEntryPrints)
#define CopyBgTilemapBufferToVram(...) ((void)0)
#define CreateMonSpriteFromNationalDexNumber(...) 0
#define BeginNormalPaletteFade(...) (gPaletteFade.active = true)
#define SetGpuReg(...) ((void)0)
#define ShowBg(...) ((void)0)
#define PlayCry_Normal(...) (++cries)
#define LoadPaletteWithOffset(...) ((void)0)
#define FreeAllWindowBuffers() (++freedWindows)
#define GetBgTilemapBuffer(...) NULL
#define Free(buffer) free(buffer)
#define NationalPokedexNumToSpecies(dexNum) (dexNum)
#define GetMonSpritePalFromSpeciesAndPersonality(species, otId, personality) \
    ((void)(species), (void)(otId), (void)(personality), monPalette)
#define OBJ_PLTT_ID(paletteNum) (paletteNum)
#define PLTT_SIZE_4BPP 32
#define LoadCompressedPalette(palette, target, size) ((void)(palette), (void)(target), (void)(size))
#define A_BUTTON 1
#define B_BUTTON 2
#define JOY_NEW(mask) (newKeys & (mask))
static void Task_DisplayCaughtMonDexPage(u8);
static void Task_HandleCaughtMonPageInput(u8);
static void Task_ExitCaughtMonPage(u8);
IntrCallback CtrPokedex_VBlankCallback(void);
bool8 CtrPokedex_IsCaughtMonPage(void);
'''
        source += bridge[bridge.index("#define CTR_STAGE_CALLBACKS"):bridge.index("static void RememberCallback(")]
        for signature in (
            "static void RememberCallback(", "static void SetCentredCallback(",
            "void CtrCentred_SetVBlankCallback(",
            "void CtrCentredPokedex_SetVBlankCallback(", "void CtrBattle_SetVBlankCallback(",
            "void CtrTransition_SetVBlankCallback(", "static void UpdateSet(", "static void UpdateStage(",
        ):
            source += function(bridge, signature)
        source += "#define SetVBlankCallback CtrCentredPokedex_SetVBlankCallback\n"
        source += function(pokedex, "static void VBlankCB_CaughtMonPage(")
        source += pokedex[pokedex.index("#define tState         data[0]"):pokedex.index("u8 DisplayCaughtMonDexPage(")]
        for signature in (
            "u8 DisplayCaughtMonDexPage(", "static void Task_DisplayCaughtMonDexPage(u8 taskId)",
            "static void Task_HandleCaughtMonPageInput(u8 taskId)", "static void Task_ExitCaughtMonPage(u8 taskId)",
            "IntrCallback CtrPokedex_VBlankCallback(void)", "bool8 CtrPokedex_IsCaughtMonPage(void)",
        ):
            source += function(pokedex, signature)
        source += r'''
#undef SetVBlankCallback
static void CheckCaught(void) {
    UpdateStage();
    assert(CtrPokedex_IsCaughtMonPage());
    assert(gMain.inBattle);
    assert(videoCentred == CTR_CENTRED_POKEDEX_CAUGHT);
    assert(!videoStage && !videoStageFaded && !videoStageZoomed && !videoBattle && !videoTransition);
    assert(sCentred.count <= 1);
    if (sCentred.count) {
        assert(sCentred.callbacks[0] == VBlankCB_CaughtMonPage);
        assert(sCentred.screens[0] == CTR_CENTRED_PLAIN);
    }
}
static void CheckBattle(IntrCallback callback) {
    UpdateStage();
    assert(!CtrPokedex_IsCaughtMonPage());
    assert(gMain.inBattle && gMain.vblankCallback == callback);
    assert(videoBattle && !videoStage && !videoTransition);
    assert(videoCentred == CTR_CENTRED_NONE);
}
static void Capture(u16 exitKey, IntrCallback callback) {
    memset(gTasks, 0, sizeof(gTasks));
    memset(&sStage, 0, sizeof(sStage));
    memset(&sCentred, 0, sizeof(sCentred));
    memset(&sBattle, 0, sizeof(sBattle));
    memset(&sTransition, 0, sizeof(sTransition));
    gMain.inBattle = true;
    CtrBattle_SetVBlankCallback(callback);
    CheckBattle(callback);
    CreateTask(UnrelatedTask, 0);
    u8 taskId = DisplayCaughtMonDexPage(25, 0x12345678, 0x07654321);
    assert(taskId != 0);
    gPaletteFade.active = true;
    gTasks[taskId].func(taskId);
    CheckBattle(callback);
    assert(gTasks[taskId].data[0] == 0);
    gPaletteFade.active = false;
    gTasks[taskId].func(taskId);
    assert(gPokedexVBlankCB == callback && gMain.vblankCallback == NULL);
    CheckCaught();
    sStage.on = true;
    sTransition.on = true;
    CheckCaught();
    sStage.on = false;
    sTransition.on = false;
    for (int state = 1; state <= 5; ++state) {
        assert(gTasks[taskId].data[0] == state);
        gTasks[taskId].func(taskId);
        CheckCaught();
    }
    assert(gMain.vblankCallback == VBlankCB_CaughtMonPage);
    assert(gPokedexVBlankCB == callback);
    assert(sCentred.on && sCentred.screen == CTR_CENTRED_PLAIN);
    unsigned previousVBlanks = battleVBlanks;
    gMain.vblankCallback();
    assert(battleVBlanks == previousVBlanks + 1);
    SetVBlankCallback(NULL);
    CheckCaught();
    CtrCentred_SetVBlankCallback(VBlankCB_CaughtMonPage);
    CheckCaught();
    gTasks[taskId].func(taskId);
    CheckCaught();
    assert(gTasks[taskId].func == Task_DisplayCaughtMonDexPage);
    gPaletteFade.active = false;
    gTasks[taskId].func(taskId);
    assert(gTasks[taskId].func == Task_HandleCaughtMonPageInput);
    newKeys = 0;
    for (unsigned frame = 0; frame < 40; ++frame) {
        gTasks[taskId].func(taskId);
        CheckCaught();
    }
    newKeys = exitKey;
    gTasks[taskId].func(taskId);
    assert(gTasks[taskId].func == Task_ExitCaughtMonPage);
    gTasks[taskId].func(taskId);
    CheckCaught();
    gPaletteFade.active = false;
    gTasks[taskId].func(taskId);
    assert(!gTasks[taskId].active && freedWindows != 0);
    CheckBattle(callback);
    SetVBlankCallback(NULL);
    UpdateStage();
    assert(videoBattle && videoCentred == CTR_CENTRED_NONE);
    SetVBlankCallback(callback);
    CheckBattle(callback);
}
int main(void) {
    Capture(A_BUTTON, BattleVBlank);
    Capture(B_BUTTON, BattleVBlank);
    Capture(A_BUTTON, OtherBattleVBlank);
    assert(newEntryPrints == 3 && cries == 3 && freedWindows == 3);
    gMain.inBattle = false;
    CtrCentredPokedex_SetVBlankCallback(VBlankCB_Pokedex);
    UpdateStage();
    assert(videoCentred == CTR_CENTRED_POKEDEX && !videoBattle);
    CtrCentredPokedex_SetVBlankCallback(NULL);
    UpdateStage();
    assert(videoCentred == CTR_CENTRED_POKEDEX && !videoBattle);
    SetVBlankCallback(FieldVBlank);
    UpdateStage();
    assert(videoCentred == CTR_CENTRED_NONE && !videoBattle);
    memset(gTasks, 0, sizeof(gTasks));
    assert(!CtrPokedex_IsCaughtMonPage());
    puts("PASS Pokédex capture: native lifecycle, NULL gaps, borrowed callbacks, A/B exit, repeated capture, battle return, field Pokédex");
    return 0;
}
'''
        path = folder / "pokedex_capture.c"
        binary = folder / "pokedex_capture"
        path.write_text(source, encoding="utf-8")
        flags = sanitizers(args.cc, folder) if args.sanitize else []
        subprocess.run([args.cc, "-std=c99", "-O1", "-Wall", "-Wextra", "-Werror", *flags,
                        str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        rendering = folder / "pokedex_rendering.c"
        renderer = folder / "pokedex_rendering"
        rendering.write_text(rendering_source(args.video.read_text(encoding="utf-8"),
                                             (ROOT / "3ds_port/include/3ds_video.h").read_text(encoding="utf-8")),
                             encoding="utf-8")
        subprocess.run([args.cc, "-std=c99", "-O1", "-Wall", "-Wextra", "-Werror", *flags,
                        str(rendering), "-o", str(renderer)], check=True)
        subprocess.run([str(renderer)], check=True)


def sanitizers(cc, folder):
    """The address and undefined-behaviour checks, when the host compiler can
    link them (MinGW's gcc has neither): said, not dropped silently."""
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    probe = folder / "sanitizer_probe.c"
    probe.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    result = subprocess.run([cc, *flags, str(probe), "-o", str(folder / "sanitizer_probe")],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if result.returncode == 0:
        return flags
    if os.name != "nt":
        raise SystemExit("%s cannot link -fsanitize=address,undefined" % cc)
    print("SKIP Pokedex capture sanitizers: %s cannot link -fsanitize=address,undefined" % cc, flush=True)
    return []


if __name__ == "__main__":
    main()
