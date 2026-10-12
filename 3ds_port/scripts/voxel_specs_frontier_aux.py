"""The Battle Frontier's civic buildings, using their native artwork: the
Ranking Hall and the Exchange Service Corner (both with the raised service
emblem on the roof) and the Reception Gate's two wings. The cliff and garden
tiles composited behind the raised emblems stay terrain (preserve_ground)."""
import voxel_building as vb
from voxel_building import Frustum, Prism, Proj, Strip, Tile, Band


def body(w, h=64, wall=26, rise=7):
    return Frustum('body', [(8, h), (w-8, h), (w, h-8), (w, 24+h-64),
                            (w-8, 16+h-64), (8, 16+h-64), (0, 24+h-64), (0, h-8)],
                   wall, Strip((h-wall, h), wrap=(8, 16)), rise,
                   Strip((h-wall-2*rise, h-wall), wrap=(8, 16)),
                   Strip((max(0, h-wall-2*rise-24), h-wall-2*rise), repeat=(max(1, h-63), max(2, h-62)), wrap=(12, w-12)))


def make(name, side, rect, wall=26, rise=7):
    w, h = rect[2]*16, rect[3]*16
    return dict(name=name, layout='LAYOUT_BATTLE_FRONTIER_OUTSIDE_'+side,
                rect=rect, ground=[1, 0x29f, 0x2a0], parts=lambda: [body(w, h, wall, rise)],
                match_rows=(1, rect[3]),
                exact=[(8, h-wall-8, w-8, h), (0, h-wall, w, h-8), (4, h-8, w-4, h-4), (8, h-4, w-8, h), (12, 1, w-12, h-wall-2*rise)])


class EmblemRoof:
    """A raised service emblem with a chamfered orange roof cap."""
    def emit(self, mesh):
        profile = [(40, 57), (48, 65), (64, 65), (72, 57)]
        for (a, ha), (b, hb) in zip(profile, profile[1:]):
            mesh.poly([(a, ha, 81, a, 81-ha), (b, hb, 81, b, 81-hb),
                       (b, hb, 73, b, 73-hb), (a, ha, 73, a, 73-ha)], vb.SHADE_ART, 'emblem.top~proj')
            mesh.poly([(a, 33, 81, a, 48), (b, 33, 81, b, 48),
                       (b, hb, 81, b, 81-hb), (a, ha, 81, a, 81-ha)], vb.SHADE_ART, 'emblem.front')
            vb.strip_face(mesh, [(b, 33, 73), (a, 33, 73), (a, ha, 73), (b, hb, 73)],
                          (40, 33, 73), (1, 0, 0), (0, 1, 0),
                          Strip((10, 14), repeat=(10, 14), wrap=(48, 56)), vb.SHADE_BACK, 'emblem.back')
        for x, h in [(40, 57), (72, 57)]:
            vb.strip_face(mesh, [(x, 33, 73), (x, 33, 81), (x, h, 81), (x, h, 73)],
                          (x, 33, 81), (0, 0, -1), (0, 1, 0),
                          Strip((10, 14), repeat=(10, 14), wrap=(48, 56)), vb.SHADE_EAST, 'emblem.end')


def civic(w=112):
    return [body(w, 80), EmblemRoof(),
            Prism('roof_behind_emblem', 40, 72, [(73, 33), (73, 33.01), (39, 33.01), (39, 33)],
                  edges={1: Strip((17, 17), repeat=(17, 18), wrap=(16, 32))},
                  skip=(0, 2, 3), west=False, east=False)]


SPECS = []
for name, side, rect in [('frontier_ranking_hall', 'EAST', (32, 8, 7, 5)),
                         ('frontier_exchange_service', 'EAST', (7, 24, 7, 5))]:
    s = make(name, side, rect)
    s.update(parts=civic, exact=[(8, 46, 104, 80), (0, 54, 112, 72),
                                 (12, 17, 36, 40), (76, 17, 100, 40), (40, 8, 72, 48, True)])
    SPECS.append(s)


def entrance_canopy(front=80, bottom=16, top=24, back=72):
    return Prism('entrance_canopy', 40, 72,
                 [(front, bottom), (front, top), (back, top), (back, bottom)],
                 edges={0: Proj(front-top, front-bottom), 1: Proj(back-top, front-top),
                        2: Tile(52, 52, 60, 56, top=top)}, skip=(3,),
                 caps=[Band(bottom, top, Tile(52, 52, 60, 56, top=top), front)])


# The Ranking Hall has a canopy over its entrance.
SPECS[0]['parts'] = lambda: civic()+[entrance_canopy()]


def reception():
    wings = []
    for x in [0, 64]:
        wings.append(Frustum('reception_wing',
            [(x+8, 80), (x+40, 80), (x+48, 72), (x+48, 40),
             (x+40, 32), (x+8, 32), (x, 40), (x, 72)], 26,
            Strip((54, 80), wrap=(8, 16)), 7, Strip((40, 54), wrap=(8, 16)),
            Strip((16, 40), repeat=(17, 18))))
    return wings+[EmblemRoof(), entrance_canopy(81, 21, 27, 75)]


SPECS.append(dict(name='frontier_reception_gate',
                  layout='LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST', rect=(23, 61, 7, 5),
                  ground=[1, 0x29f, 0x2a0], parts=reception,
                  exact=[(8, 46, 40, 80), (72, 46, 104, 80), (0, 54, 48, 72), (64, 54, 112, 72),
                         (12, 17, 36, 40), (76, 17, 100, 40), (40, 8, 72, 60, True)]))

# Cliff and garden tiles composited behind the raised emblems remain terrain.
for s in SPECS:
    layout = vb.LayoutArt(s['layout'])
    art = layout.building_art(*s['rect'], layout.ground_tiles(s['ground']))
    for x in range(112):
        crest = (16-(x-40) if 40 <= x < 48 else 8 if 48 <= x < 64
                 else 8+(x-64) if 64 <= x < 72 else 16)
        for y in range(crest):
            art.putpixel((x, y), (0, 0, 0, 0))
    if s['name'] == 'frontier_reception_gate':
        for x in range(48, 64):
            for y in range(60, 80):
                art.putpixel((x, y), (0, 0, 0, 0))
    own = {(x, y) for x in range(112) for y in range(80) if art.getpixel((x, y))[3] >= 128}
    s.update(art=art, own=own, preserve_ground=True,
             owned={(x//16, y//16) for x, y in own})
