#!/usr/bin/env python3
"""Prove Dewford exterior coverage and original art using a read-only tree."""

import argparse
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch


PORT = pathlib.Path(__file__).resolve().parents[1]
TOWN = "LAYOUT_DEWFORD_TOWN"
EXPECTED = {
    "dewford_house": {(7, 5), (16, 11)},
    "dewford_hall": {(1, 0)},
    "dewford_gym_exterior": {(5, 13)},
    "pokemon_center": {(1, 7)},
}
GLASS = {
    "dewford_house": ((34, 46, 39, 52), (41, 46, 46, 52)),
    "dewford_hall": ((50, 46, 55, 52), (57, 46, 62, 52)),
    "dewford_gym_exterior": ((11, 50, 21, 53), (27, 50, 37, 53), (75, 50, 85, 53),
                             (49, 62, 55, 77), (57, 62, 63, 77)),
}
GLASS_COUNTS = {"dewford_house": 60, "dewford_hall": 60, "dewford_gym_exterior": 270}


def check_glass(model, night):
    width, height = model.art.size
    mask = night.building_mask(model.art, model.name)
    assert len(mask) == width * height
    if model.name not in GLASS:
        return
    expected = {(xpos, ypos) for left, top, right, bottom in GLASS[model.name]
                for ypos in range(top, bottom) for xpos in range(left, right)}
    actual = {(xpos, ypos) for ypos in range(height) for xpos in range(width)
              if mask[ypos * width + xpos]}
    assert actual == expected, (model.name, actual.symmetric_difference(expected))
    assert len(actual) == GLASS_COUNTS[model.name]
    for xpos, ypos in actual:
        red, green, blue, alpha = model.art.getpixel((xpos, ypos))
        assert alpha == 255 and red >= 90 and green > red and blue > red + 12
    print("PASS", model.name, "authored glass:", len(actual), "pixels, no roof/frame/solid-door glow")


def source_module(name, tree):
    source = PORT / "scripts" / (name + ".py")
    module = types.ModuleType(name)
    module.__file__ = str(tree / "3ds_port" / "scripts" / source.name)
    sys.modules[name] = module
    exec(compile(source.read_text(encoding="utf-8"), str(source), "exec"),
         module.__dict__)
    return module


def check_sign_fixture(tree):
    cells = source_module("voxel_cells", tree)
    source_module("voxel_sign_mask", tree)
    signs = source_module("gen_voxel_sign_masks", tree)
    from voxel_sign_mask import event_board_masks, head_mask

    layouts = cells.load_layouts()
    layout_index, entry = next((index, entry) for index, entry in enumerate(layouts, 1)
                               if entry["id"] == TOWN)
    assert layout_index == 12
    layout = cells.Layout(entry, cells.MapEvents())
    pair = cells.pair_for(layout.primary, layout.secondary)
    assert (11, 16) in layout.signs
    # A post here (voxel_cells.is_signpost: drawn as one, walkable to its
    # south), drawn by sign_record; the event board reads it the same.
    assert layout.role_at(11, 16) == "signpost"
    assert event_board_masks(pair, layout, 11, 16) == signs.sign_record(layout, pair, 11, 16)[:3]
    assert [layout.metatile(11, row) for row in (15, 16, 17)] == [0x208, 0x210, 0x124]
    records = signs.layout_records(layout_index, layout, pair)
    fixture = [record for record in records if record[1:3] == (11, 16)]
    assert len(fixture) == 1
    _, _, _, rows, head, ground, cell = fixture[0]
    assert rows == [0xFFFE] * 10 + [0x7FFC, 0x0FE0, 0x3FF8, 0x7FFC, 0x3FF8, 0x0FE0]
    assert head == [0] * 9 + [0x3FF8, 0x7FFC] + [0xFFFE] * 5
    assert ground == 0x124 and cell == 0x124
    assert sum(row.bit_count() for row in rows) == 212
    assert sum(row.bit_count() for row in head) == 99
    # the board's top, 0x208 north of it, read as its head
    assert head_mask(pair, rows, 0x208) == head
    for xpos, ypos in ((3, 10), (4, 10)):
        assert event_board_masks(pair, layout, xpos, ypos) is None
    with patch.object(layout, "signs", layout.signs - {(11, 16)}):
        assert event_board_masks(pair, layout, 11, 16) is None
    with patch.object(layout, "outdoor", False):
        assert event_board_masks(pair, layout, 11, 16) is None
    compiler = shutil.which("cc")
    assert compiler, "A host C compiler is required for the sign runtime fixture"
    with tempfile.TemporaryDirectory(prefix="dewford-sign-") as temporary:
        binary = pathlib.Path(temporary) / "signposts.bin"
        executable = pathlib.Path(temporary) / "voxel_sign_test"
        signs.write_records(records, binary)
        data = binary.read_bytes()
        assert data[:4] == b"VXS3" and len(data) == 8 + len(records) * 74
        assert struct.unpack_from("<I", data, 4)[0] == len(records)
        subprocess.run([compiler, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(PORT / "include"), "-I" + str(PORT / "src/voxel"),
                        "-DVOXEL_HOST_FILES", '-DSIGN_MASK_PATH="' + binary.as_posix() + '"',
                        str(PORT / "tests/voxel_sign_test.c"), str(PORT / "src/voxel/voxel_sign.c"),
                        "-lm", "-o", str(executable)], check=True)
        subprocess.run([str(executable), str(binary), "--dewford"], check=True)
    print("PASS Dewford gym sign: original 0x208/0x210 art, 212+99 pixels, sand donor 0x124")
    return {(11, 15), (11, 16)}


def check_binary(path, models, layouts, generator):
    data = path.read_bytes()
    assert data[:4] == b"VXB9"
    pages, count, page_models, placements, height_bytes, masks, vertices, variants, decks, \
        conditions, _ = struct.unpack_from("<HHHHHHIHHHH", data, 4)
    assert count == len(models)
    pages_offset = 28                   # VXB9: the header grew by the conditions count
    records_offset = pages_offset + pages * 8
    page_models_offset = records_offset + count * 16
    placements_offset = page_models_offset + page_models * 8
    heights_offset = placements_offset + placements * 16
    vertex_offset = heights_offset + height_bytes + height_bytes % 2
    vertex_offset += height_bytes * 2 + masks * 32 + height_bytes + height_bytes % 2
    vertex_offset += variants * 6 + decks * 8 + conditions * 6
    vertex_offset = (vertex_offset + 3) & ~3
    assert vertex_offset + vertices * 24 <= len(data)
    town_index = next(index + 1 for index, entry in enumerate(layouts)
                      if entry["id"] == TOWN)
    found = {name: set() for name in EXPECTED}
    for index in range(placements):
        layout, page_model, xpos, ypos, ground, extra_count, extra_first = (
            struct.unpack_from("<HHHHHHI", data, placements_offset + index * 16))
        if layout != town_index:
            continue
        model_index, page, origin_x, origin_y = struct.unpack_from(
            "<HHhh", data, page_models_offset + page_model * 8)
        assert page < pages
        assert ground == 0x124
        assert extra_first + extra_count <= vertices
        model = models[model_index]
        page_width, page_height, texture_offset = struct.unpack_from("<HHI", data, pages_offset + page * 8)
        assert page_height & 0x8000
        glow_offset = texture_offset + page_width * (page_height & 0x7FFF) * 2
        # the glass as exported: moved with the drawing's compaction
        crop = model.art.getbbox() or (0, 0, 1, 1)
        for texture_y in range(crop[1], crop[3]):
            for texture_x in range(crop[0], crop[2]):
                pixel_offset = generator.texel_offset(
                    origin_x + texture_x, origin_y + texture_y, page_width)
                glass = model.glass.getpixel((texture_x, texture_y)) if model.glass else 0
                assert data[glow_offset + pixel_offset] == glass
        found[model.name].add((xpos, ypos))
        width, height, _, first, vertex_count, height_start = struct.unpack_from(
            "<BBHIII", data, records_offset + model_index * 16)
        assert (width, height) == model.cells
        assert vertex_count > 0 and first + vertex_count <= vertices
        assert tuple(data[heights_offset + height_start:
                         heights_offset + height_start + width * height]) == (
                             generator.cell_heights(model))
        elevations = [struct.unpack_from("<f", data, vertex_offset + offset * 24 + 4)[0]
                      for offset in range(first, first + vertex_count)]
        assert max(elevations) - min(elevations) > 1.5
    assert found == EXPECTED, found
    for index in range(pages):
        width, flagged_height, offset = struct.unpack_from("<HHI", data, pages_offset + index * 8)
        height = flagged_height & 0x7FFF
        assert width <= 1024 and height <= 1024
        size = width * height * (3 if flagged_height & 0x8000 else 2)
        assert offset + size <= len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=pathlib.Path,
                        default=PORT.parent / "build" / "upstream")
    parser.add_argument("--preview", type=pathlib.Path)
    parser.add_argument("--sign-only", action="store_true")
    args = parser.parse_args()
    tree = args.tree.resolve()
    sys.path.insert(0, str(PORT / "scripts"))
    source_module("dump_region_art", tree)
    source_module("voxel_art", tree)
    sign_cells = check_sign_fixture(tree)
    if args.sign_only:
        return
    sys.path.insert(0, str(PORT / "scripts"))
    import voxel_building as buildings
    import voxel_building_specs as specs
    import gen_voxel_buildings as generator
    import voxel_night as night

    buildings.ROOT = str(tree)
    selected = [spec for spec in specs.SPECS if spec.get("name") in EXPECTED]
    assert {spec["name"] for spec in selected} == set(EXPECTED)
    with patch.object(specs, "SPECS", selected), patch.object(generator, "reuse_everywhere"):
        models = generator.build_models()
    layouts = buildings._layouts_json()
    town = buildings.LayoutArt(TOWN)
    occupied = set()
    for model in models:
        check_glass(model, night)
        assert buildings.ortho_check(model, exact=model.spec["exact"]) == (0, 0, 0), model.name
        assert not buildings.density_check(model), model.name
        assert model.triangle_count() > 12
        assert max(generator.cell_heights(model)) > 24
        placements = [entry for entry in generator.find_placements(model, layouts)
                      if entry[4] == TOWN]
        assert {(entry[1], entry[2]) for entry in placements} == EXPECTED[model.name]
        for _, xpos, ypos, ground, _, odd in placements:
            assert ground == 0x124
            width, height = model.cells
            cells = {(xpos + col, ypos + row) for row in range(height)
                     for col in range(width)}
            assert not occupied.intersection(cells)
            occupied.update(cells)
            first, last = model.spec.get("match_rows", (0, height))
            for row in range(first, last):
                for col in range(width):
                    metatile = town.metatile(xpos + col, ypos + row)
                    assert generator.same_building_pixels(model, TOWN, metatile, col, row)
            if model.name.startswith("dewford_"):
                assert all(entry[4] == TOWN for entry in generator.find_placements(model, layouts))
        print("PASS", model.name, "original pixels, texel density, solid geometry, placements")
    map_data = json.loads((tree / "data/maps/DewfordTown/map.json").read_text())
    assert not occupied.intersection(sign_cells)
    assert len(map_data["warp_events"]) == 5
    assert all((warp["x"], warp["y"]) in occupied for warp in map_data["warp_events"])
    with tempfile.TemporaryDirectory(prefix="dewford-3d-") as temporary:
        binary = pathlib.Path(temporary) / "buildings.bin"
        generator.export(models, str(binary))
        check_binary(binary, models, layouts, generator)
    if args.preview:
        args.preview.mkdir(parents=True, exist_ok=True)
        for model in models:
            buildings.ortho_check(model, str(args.preview / (model.name + "_ortho.png")),
                                 exact=model.spec["exact"])
            generator.preview(model, str(args.preview), ["game", "yaw35"])
    print("PASS Dewford: all five entrances covered; VXB9 geometry/pages/placements validated")


if __name__ == "__main__":
    main()
