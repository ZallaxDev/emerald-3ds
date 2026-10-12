"""City landmarks built from their own wall and roof artwork: Lavaridge's gym
and Sootopolis's seven houses (the peaked ones composited into the cliff keep
the cliff as terrain, preserve_ground).

Import after voxel_building_specs defines its shared gym architecture.
"""
import voxel_building as _vb
from voxel_building import Band, Frustum, Prism, Proj, Strip, Tile
from voxel_building_specs import gym, GYM_EXACT


def roofed_block(width, height, facade, rim, back=16, run=None,
                 repeat=None, side_x=8, roof_x=8):
    """Facade and eave at their drawn depth, a pitched roof and tiled back.

    Each projected roof row is one texel. Extra depth is filled with courses
    from the same roof, while side walls use an undecorated facade column.
    The optional slope run is measured in world pixels, not texture pixels.
    """
    front = height
    wall = height - facade
    eave = height - rim
    side = Tile(side_x, facade, side_x + 4, height, top=wall)
    rim_tile = Tile(roof_x, rim, roof_x + 8, facade, top=eave)
    body = Prism('walls', 0, width,
                 [(front, 0), (front, wall), (back, wall), (back, 0)],
                 edges={0: Proj(facade, height), 2: side}, skip=(1, 3),
                 caps=[Band(0, wall + 1, side, front)])
    run = rim if run is None else run
    zcrest, ycrest = front - run, eave + rim - run
    roof_side = Tile(side_x, facade, side_x + 4, height, top=ycrest)
    if rim == facade == 0:
        roof = Prism('roof', 0, width,
                     [(front, wall), (front, wall+2), (back, wall+2), (back, wall)],
                     edges={0: Proj(0, 1), 1: Strip((0, 0), repeat=(0, 1)), 2: side},
                     skip=(3,), caps=[Band(wall, wall+3, side, front)])
        return [body, roof]
    roof = Prism('roof', 0, width,
                 [(front, wall), (front, eave), (zcrest, ycrest),
                  (back, eave), (back, wall)],
                 edges={0: Proj(rim, facade), 1: Proj(0, rim),
                        2: Strip((max(0, rim - 8), rim), repeat=repeat, wrap=(roof_x, roof_x+8)), 3: rim_tile},
                 skip=(4,), caps=[Band(wall, ycrest + 1, roof_side, front)])
    return [body, roof]


def spec(name, layout, rect, facade, rim, **kwargs):
    ground = kwargs.pop('ground', [1])
    w, h = rect[2]*16, rect[3]*16
    return dict(name=name, layout='LAYOUT_'+layout, rect=rect, ground=ground,
                parts=lambda: roofed_block(w, h, facade, rim, **kwargs),
                exact=[(0, 0, w, h, True)])


def sootopolis_peaked_house():
    plan = [(8, 80), (40, 80), (48, 72), (48, 24), (40, 16), (8, 16), (0, 24), (0, 72)]
    body = Frustum('octagonal_house', plan, wall_top=24,
        wall_side=Strip((56, 80), wrap=(1, 5)), band_rise=8,
        band_side=Strip((40, 56), wrap=(12, 20)),
        top=Strip((32, 40), repeat=(40, 41), wrap=(12, 36)))
    cone_plan = [(16, 64), (32, 64), (40, 56), (40, 40), (32, 32), (16, 32), (8, 40), (8, 56)]
    cone = Frustum('peaked_roof', cone_plan, wall_top=32,
        wall_side=Strip((32, 40), wrap=(12, 20)), band_rise=12,
        band_side=Strip((8, 32), wrap=(16, 24)),
        top=Strip((8, 8), repeat=(8, 9), wrap=(20, 28)))
    return [body, cone]


SPECS = [
    dict(name='gym_lavaridge_exterior', layout='LAYOUT_LAVARIDGE_TOWN', rect=(2, 11, 6, 5),
         ground=[1, 0x20c], parts=gym, exact=GYM_EXACT),
]

# Door feet identify occupied houses, including cliff-composited variants.
SOOTOPOLIS_HOUSES = [(8, 2, 5), (44, 3, 4), (8, 13, 5), (43, 13, 5),
                     (47, 22, 4), (8, 23, 4), (52, 25, 4)]
for x, y, rows in SOOTOPOLIS_HOUSES:
    building = spec('sootopolis_house_%d_%d' % (x, y), 'SOOTOPOLIS_CITY',
                    (x, y, 3, rows), 32 if rows == 4 else 56, 28 if rows == 4 else 52,
                    run=28 if rows == 4 else 36, repeat=(8, 16), side_x=1, roof_x=16,
                    ground=[1, 0x231])
    if rows == 5:
        layout = _vb.LayoutArt(building['layout'])
        art = layout.building_art(*building['rect'], layout.ground_tiles(building['ground']))
        for xx in range(48):
            for yy in range(8):
                art.putpixel((xx, yy), (0, 0, 0, 0))
        building.update(parts=sootopolis_peaked_house, art=art, preserve_ground=True,
                        exact=[(8, 56, 40, 80), (12, 32, 36, 56), (20, 8, 28, 32)])
        roof_ink = {(65, 74, 106), (82, 90, 82), (98, 106, 98), (123, 131, 123)}
        own = set()
        for xx in range(48):
            top = next((yy for yy in range(8, 56) if art.getpixel((xx, yy))[:3] in roof_ink), 56)
            own.update((xx, yy) for yy in range(max(8, top-1), 80)
                       if art.getpixel((xx, yy))[3] >= 128)
        for x0, y0, x1, y1 in building['exact']:
            own.update((xx, yy) for xx in range(x0, x1) for yy in range(y0, y1)
                       if art.getpixel((xx, yy))[3] >= 128)
        building['own'] = own
    SPECS.append(building)
