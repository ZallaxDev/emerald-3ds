#!/usr/bin/env python3
"""Cuts src/3ds_video.c into its src/video/ units, or puts them back together.

A unit is a run of top-level text of the one translation unit, #included where
it was: the compiler reads the same text in the same order. TABLE says where
each unit starts by a line of its text, never by a number: the cut falls at
the start of that line's element, comment above it included, and only ever
between two elements, with no brace, parenthesis, comment or #if open.

Changes written against the file in one piece (a pull request from before the
cut) go on a flat copy and through here again:

    video_cut.py flat -o flat.c     the file in one piece
    video_cut.py cut --from flat.c  the units again, from that text
    video_cut.py check              the units are what the table cuts
"""
import argparse
from pathlib import Path
import re
import sys
import textwrap

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT / 'tests'))
from video_text import SOURCE, UNIT, units_of

KEEP = None  # Text that stays in 3ds_video.c.

# Unit, and how a line of its first element or of the comment above it starts.
TABLE = [
    ('gpu', 'static bool sInBattle;'),
    (KEEP, '#include "voxel/ctr_voxel.h"'),
    ('view', ' * The game lays itself out over the whole 400x240 viewport, so screen and'),
    ('vram', 'static CtrVideoMemory sMemory;'),
    ('banks', " * The field's banks of other tilesets (3ds_tile_banks.h): their tiles sit in"),
    ('frame', ' * Current clip rectangle. With windows enabled the frame is composed as a'),
    ('tiles', ' * A palette fade as a tint. The game fades by rewriting every colour each'),
    ('tiledraw', 'static void DrawTile(unsigned address, unsigned bank, bool color256,'),
    ('stage', ' * How a stage fills the screen around its 240x160 picture. A GBA background'),
    ('corner', " * The Game Corner's backdrops (3ds_corner_art.h): two small textures filled"),
    ('edges', ' * The GBA brightness effect a layer is under (BLDCNT effect 2 or 3), as the'),
    ('hud', " * The battle's info boxes and text box at the screen's resolution"),
    ('boxtext', ' * The battle text box (3ds_battle_hud.h, gen_battle_hud.py): a panel floating'),
    ('battlebg', ' * The battle terrain split at CTR_BATTLE_SIDE_SPLIT (ctr_battle_layout.h):'),
    ('centred', ' * How a centred screen fills the top screen around its 240x160 picture.'),
    ('layertex', ' * Layer textures. A text background is composed once, unscrolled, into a'),
    ('layerwalk', 'static unsigned LayerBanks(unsigned bg);'),
    ('leaves', " * The intro's leaves scene, widened with art of its own."),
    ('bg', ' * A stage background from its texture, filled around the picture exactly as'),
    ('obj', ' * Where a sprite placed on the screen, not on the map, goes in the field view'),
    ('compose', ' * The circle of light of a dark cave (CtrVideo_SetFieldLight). The game opens'),
    (KEEP, " * A top screen's target as C2D_CreateScreenTarget makes it - RGBA8, sent to"),
    ('scene', " * Composes the logical frame with this eye's parallax and blits it to one"),
    ('toggles', " * Console test builds (make BATTLE_TOGGLES=1): the compositor's nocache bits"),
    ('bscene', ' * The battle scene - everything but the text box - at SCENE_ZOOM into its own'),
    ('bottom', " * The bottom screen's surface, the whole 320x240 screen. The PokéNav uses"),
    ('slots', " * The battle's sprites in 3D (RenderBattleStereo). The sprites on screen are"),
    ('mono', ' * The battle without the 3D slider, 2D or over the voxel world. Composed'),
    ('gather', " * The groups are kept (sGatherKept): a gather's result is a function of the"),
    ('slotpack', " * Each region's skyline: runs of columns and how far down each is taken."),
    ('btext', " * The voxel battle's world for one eye: its finished picture (`world`,"),
    ('stereo', "/* The stereo battle's scene composed whole, its sprites at the scenery's depth. */"),
    ('voxel', '/* Seals a command list. Before the GPU is started early it is submitted at'),
    ('bworld', ' * The 3D battle: the battle in front of the voxel world (CtrSettings_'),
    ('transition', ' * A battle transition over the field (CtrVideo_SetTransition).'),
    ('bands', ' * How many depth planes this frame can be split into.'),
    ('overlays', '#ifndef CTR_SHOW_FPS'),
    ('report', "/* The frame report (CtrVideoFrameReport): this game frame's, and the ids of"),
    ('voxvram', " * The voxel world's VRAM follows its option (CtrVoxel_ParkVram): given back a"),
    ('framelog', '/* The GPU started early (GpuStartEarly): how many frames, and in how'),
    ('present', ' * The Pokédex on the top screen (3ds_dex_top.c) while its pages are on the'),
    (KEEP, 'void CtrVideo_Present(void)'),
]
# What each unit holds and offers, written at its top.
HEADERS = {
    'gpu': "The GX queue and the wraps of GX, GSP and Citro3D calls: flushes, draw and copy "
           "counters, the GPU started early within a frame, clears and texture cache drops. "
           "Offers the voxel uploads' room (CtrVideo_TryVoxelUpload), ClearWhenMade and "
           "TexCacheDrop.",
    'view': "The view (sViewX/Y, sZoom, the target's size), the scene surface's handles, the "
            "stage, centred and battle flags, the per-line registers, and the OAM's marks "
            "(anchors, weather, fog, screen, machine, Fly) and extended palettes. Most later "
            "units read this state.",
    'vram': "The memory the game writes and what the compositor made of it: sMemory, sStats, "
            "the atlas and surfaces, the tile cache, the palettes and VRAM's shadow with its "
            "stamps. Offers the announced writes (CtrVideo_NotifyWrite), SyncBlock and "
            "VramChangedAfter.",
    'banks': "The field's banks of other tilesets (3ds_tile_banks.h): tiles, palettes and the "
             "tilemap entries that use them. Offers TilemapBankAt and BankedTileSlot, called "
             "per cell. Ends with the libraries' flags and the FPS counters.",
    'frame': "The clip rectangle, the stereo state, the quality counts, the transition's line "
             "registers, and what every later unit calls: Reg, Read16, Min, Error, ClipToView "
             "and BlendForget.",
    'tiles': "Palette fades and uploads, the atlas's slots (GetTileSlot), DrawSlot and the "
             "blending state (Blend).",
    'tiledraw': "Tiles as quads: DrawTile, the view helpers (ViewBase, ViewAffine), TileWalk's "
                "batches, text spans, empty walks and the scissor.",
    'stage': "A stage's picture: the scroll history, the panorama of a scrolling scene, stills "
             "and what is drawn beneath it (StageUnderlay).",
    'corner': "The Game Corner's backdrops (3ds_corner_art.h): their textures and "
              "CornerUnderlay, the felt and the wall behind roulette and slots.",
    'edges': "The margins of a stage: brightness, fading tiles, DrawEdgeRegion and DrawStageBg. "
             "Offers MapEntry and EntrySlot, the tilemap reads of the later walks.",
    'hud': "The battle's info boxes at the screen's resolution (3ds_battle_hud.h): their "
           "texture and windows, HudFrame, HudDrawInScene and HudDrawCrisp.",
    'boxtext': "The battle's text box at the screen's resolution: its panel, its window's text "
               "and slide, DrawBattleText.",
    'battlebg': "The battle's backgrounds: the terrain split between the sides, a move's "
                "one-picture background (BattlePicture) and DrawBattleBg.",
    'centred': "Centred screens: what fills each one's margins (sCentredFills), DrawCentredBg, "
               "and DrawTextBg, the plain walk of a text background.",
    'layertex': "Layer textures, a background composed once into a texture of its own "
                "(sLayers): their memory, release and signatures, and the quads of their "
                "cells (FastCells).",
    'layerwalk': "Layer textures' walk: which cells changed (LayerRenderCells, per cell), "
                 "LayersRender, and the layers drawn out (DrawLayerRect, DrawEdgeBand).",
    'leaves': "The intro's leaves scene, with art of its own: its loading, texture and margins.",
    'bg': "Backgrounds from textures: stage and line backgrounds, the battle's layers (MapPlan, "
          "DrawBattleBgTex, DrawBattleTerrainTex) and DrawAffineBg.",
    'obj': "Sprites: where they go (PlaceScreenObject), the fog's lattice, their batches "
           "(SpriteBatchFlush) and DrawObjects.",
    'compose': "The frame composed: the field's light, Layers, the windows (WindowEdge, "
               "WindowPartition), the OBJ window, Compose and ComposeLines.",
    'scene': "The scene surface and its cache: SceneMake, ScenePrepare, the sprites' depths, "
             "SceneComposeInto, and the keys that say the scenery holds (SceneCacheKey, "
             "SceneHoldsSince).",
    'toggles': "Test builds only: BATTLE_TOGGLES, and the bits of nocache.txt (CtrDumpOff) that "
               "the later units ask.",
    'bscene': "The battle's scenery: BattleSceneCompose and its blending layer apart, its place "
              "on the screen (BattleScenePlace), the wireless indicator, the kept edges and "
              "RenderBattleScene.",
    'bottom': "The bottom screen's surface: its strips, the native menus and the transfer; the "
              "storage's windows; and RenderEye, the frame to one eye.",
    'slots': "The 3D battle's memory: the sprite slots, the blending layer, the large layers "
             "and who gives way to whom (BattleLayersYield, BattleLayerMake).",
    'mono': "The battle without the 3D slider: BattleSceneComposeMono and the cuts it makes.",
    'gather': "The battle's sprites of a frame gathered: groups, kept gathers, BattleSprites "
              "and LooseDraw.",
    'slotpack': "The slots packed: the skyline (SlotsPack), the kept regions' keys, SlotsCompose "
                "and SlotsPlace.",
    'btext': "BattleWorldEye, and the battle's text pass with what keeps it (BattleTextCompose, "
             "BattleTextKeep).",
    'stereo': "The scenery straight to the eyes (BattleSceneryStraight), RenderBattleStereo "
              "and its pieces, with StereoPlan, what they hand on.",
    'voxel': "The voxel world's picture: diorama, bloom, gloom, Flash's light, the 2D field "
             "over it in a swoop, and RenderVoxel.",
    'bworld': "The battle over the voxel world: curtain, shadows, the intro's blur and bars, "
              "and RenderBattleWorld.",
    'transition': "A battle transition over the field: its line bands, layers and picture, and "
                  "RenderTransition.",
    'bands': "Depth planes: their bands and surfaces, RenderBands and BlitBands.",
    'overlays': "What goes over the finished picture: the FPS, the HOME notice, the poison "
                "veil, the speed badge, the swoop's light; and the held top screen.",
    'report': "The frame report (CtrVideoFrameReport): each submission's times and the queue's "
              "drains.",
    'voxvram': "The voxel world's VRAM and building pages, once a frame: VoxelVramManage and "
               "VoxelPagesManage.",
    'framelog': "The frame's logs: a dropped frame (FrameDrops) and those of every 600 frames. "
                "Each runs where Present calls it: moved, it would count other frames, and "
                "perf_report.py reads their text.",
    'present': "The pieces of CtrVideo_Present, in the frame's order, and PresentFrame, what "
               "they hand on; RenderDexTop.",
}


def starts(lines):
    """The lines (0-based) an element starts at with nothing open before it:
    its comment's first line where it has one."""
    found = []
    comment = continued = False
    depth = conditionals = 0
    last = ';'
    for number, line in enumerate(lines):
        directive = not comment and not continued and line.lstrip().startswith('#')
        if (not comment and not continued and not depth and not conditionals and last in ';}'
                and line.strip() and (not number or not lines[number - 1].strip())):
            found.append(number)
        if directive or continued:
            if directive:
                word = re.match(r'\s*#\s*(if|ifdef|ifndef|endif)\b', line)
                if word:
                    conditionals += -1 if word[1] == 'endif' else 1
            comment = '/*' in re.sub(r'/\*.*?\*/', '', line)
            continued = line.rstrip().endswith(chr(92))
            continue
        column = 0
        while column < len(line):
            if comment:
                end = line.find('*/', column)
                column = len(line) if end < 0 else end + 2
                comment = end < 0
            elif line.startswith('/*', column):
                comment = True
                column += 2
            elif line.startswith('//', column):
                break
            elif line[column] in '"\'':
                quote = line[column]
                column += 1
                while line[column] != quote:
                    column += 2 if line[column] == chr(92) else 1
                column += 1
                last = quote
            else:
                character = line[column]
                depth += (character in '({') - (character in ')}')
                if not character.isspace():
                    last = character
                column += 1
    if depth or conditionals or comment:
        raise ValueError(f'unbalanced text: depth {depth}, #if {conditionals}, comment {comment}')
    return found


def cut(flat, wanted):
    """Splits the text in one piece: the main file and {unit: text}."""
    lines = flat.split('\n')
    legal = starts(lines)
    edges = []
    for unit, anchor in TABLE:
        hits = [number for number, line in enumerate(lines) if line.startswith(anchor)]
        if len(hits) != 1:
            raise ValueError(f'{unit}: {len(hits)} lines start with {anchor!r}')
        edges.append(max(number for number in legal if number <= hits[0]))
    if edges != sorted(set(edges)):
        raise ValueError('the table is out of order')
    main, units = lines[:edges[0]], {}
    if len(lines[-1]):
        raise ValueError('no newline at the end of the file')
    for index, (unit, _) in enumerate(TABLE):
        end = edges[index + 1] if index + 1 < len(edges) else len(lines) - 1
        run = lines[edges[index]:end]
        if unit is KEEP or unit not in wanted:
            main += run
            continue
        blanks = []
        while not run[-1].strip():
            blanks.append(run.pop())
        units[unit] = '\n'.join(run) + '\n'
        main += [f'#include "video/{unit}.inc"'] + blanks
    return '\n'.join(main) + '\n', units


def header_of(unit):
    lines = textwrap.wrap(f'Part of 3ds_video.c. {HEADERS[unit]}', 75)
    return '/*\n' + ''.join(f' * {line}\n' for line in lines) + ' */\n\n'


def flatten(main_text, read_unit):
    """The text in one piece, without the headers the table puts on the units."""
    def unit_text(line):
        text = read_unit(line[1])
        header = header_of(line[1])
        if not text.startswith(header):
            raise ValueError(f'{line[1]}: the header is not the table\'s')
        return text[len(header):]
    return UNIT.sub(unit_text, main_text)


def read_tree():
    main = SOURCE.read_text(encoding='utf-8')
    folder = SOURCE.parent / 'video'
    return main, lambda unit: (folder / f'{unit}.inc').read_text(encoding='utf-8')


def write(path, text):
    path.write_bytes(text.encode('utf-8'))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('cut', 'flat', 'check'))
    parser.add_argument('--from', dest='source', type=Path, help='flat text to cut')
    parser.add_argument('--units', help='only these units, by comma (default: all)')
    parser.add_argument('-o', '--output', type=Path, help='where flat writes')
    args = parser.parse_args()
    main_text, read_unit = read_tree()
    every = [unit for unit, _ in TABLE if unit is not KEEP]
    if args.source:
        flat = args.source.read_text(encoding='utf-8')
    else:
        flat = flatten(main_text, read_unit)
    if args.command == 'flat':
        write(args.output, flat)
    elif args.command == 'check':
        again, units = cut(flat, set(units_of(main_text)))
        if again != main_text or any(read_unit(unit) != header_of(unit) + text
                                     for unit, text in units.items()):
            sys.exit('the units are not what the table cuts')
        print(f'{len(units)} units of {len(every)}, text in one piece the same')
    else:
        wanted =set(args.units.split(',')) if args.units else set(every)
        if wanted - set(every):
            sys.exit(f'no such unit: {sorted(wanted - set(every))}')
        new_main, units = cut(flat, wanted)
        folder = SOURCE.parent / 'video'
        folder.mkdir(exist_ok=True)
        for unit, text in units.items():
            write(folder / f'{unit}.inc', header_of(unit) + text)
        write(SOURCE, new_main)
        for unit in every:
            if unit not in units:
                (folder / f'{unit}.inc').unlink(missing_ok=True)
        print(f'{len(units)} units, {len(new_main.splitlines())} lines left in {SOURCE.name}')


if __name__ == '__main__':
    main()
