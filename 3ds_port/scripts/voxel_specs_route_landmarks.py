"""Route landmarks read as roof and facade bands: the Safari Zone's two gates
and the Weather Institute (with its two round roof housings)."""
from functools import partial
from PIL import Image
from voxel_building import Band, LayoutArt, Prism, Proj, Strip, Tile, Mesh, Raster, SHADE_ART


def roof_and_facade(width, rows, split, side_x=4, rise=8):
    """Projected front roof and wall, with repeated own-art side/back bands."""
    zf = rows
    top = zf - split
    side = Tile(side_x, split, side_x + 4, rows, top=top)
    wall = Prism('facade', 0, width,
                 [(zf, 0), (zf, top), (16, top), (16, 0)],
                 edges={0: Proj(split, rows), 2: side}, skip=(1, 3),
                 caps=[Band(0, top + 1, side, zf)])
    roof = Prism('roof', 0, width,
                 [(zf, top), (top + rise, top + rise), (16, top)],
                 edges={0: Proj(0, split),
                        1: Strip((rows, rows+16), repeat=(rows, rows+16), wrap=(0, 16))},
                 skip=(2,), caps=[Band(top, top+rise+1,
                                      Tile(0, rows, 16, rows+16, top=top+rise), zf)])
    support = Prism('opaque_roof_cover', 0, width,
                    [(zf, top-0.125), (top+rise, top+rise-0.125), (16, top-0.125)],
                    edges={0: Strip((rows, rows+16), repeat=(rows, rows+16), wrap=(0, 16))},
                    skip=(1, 2))
    return [wall, roof, HiddenParts([support])]


class HiddenParts:
    """Opaque structural backing behind a front drawing with cut-out texels."""
    def __init__(self, parts): self.parts = parts
    def emit(self, mesh):
        from voxel_building import Mesh, emit_prism
        local = Mesh()
        for part in self.parts:
            emit_prism(local, part)
        for tri, shade, tag in local.tris:
            mesh.tris.append((tri, shade, tag+'~behind'))


class ArtOffset:
    """Keep object texels separate from terrain texels on mixed metatiles."""
    def __init__(self, parts, offset):
        self.parts, self.offset = parts, offset
    def emit(self, mesh):
        from voxel_building import Mesh, emit_prism
        local = Mesh()
        for part in self.parts:
            if isinstance(part, Prism): emit_prism(local, part)
            else: part.emit(local)
        for tri, shade, tag in local.tris:
            mesh.tris.append((tuple((x, y, z, u, v+self.offset) for x, y, z, u, v in tri), shade, tag))


def clean_art(layout, rect, ground, terrain=(), trim=None):
    l = LayoutArt(layout); x, y, w, h = rect
    art = l.building_art(x, y, w, h, l.ground_tiles(ground))
    colors = set()
    for tx, ty in terrain:
        colors.update(l.cell_image(l.metatile(tx, ty)).getdata())
    px = art.load()
    for v in range(h*16):
        for u in range(w*16):
            if px[u, v] in colors or (trim and not trim(u, v)):
                px[u, v] = (0, 0, 0, 0)
    if terrain:
        original = l.building_art(x, y, w, h, set())
        for u in range(w*16):
            span = [v for v in range(h*16) if px[u, v][3]]
            if span:
                for v in range(min(span), max(span)+1):
                    px[u, v] = original.getpixel((u, v))
        for v in range(h*16):
            if layout == 'LAYOUT_ROUTE119' and v < 8: continue
            span = [u for u in range(w*16) if px[u, v][3]]
            if span:
                for u in range(min(span), max(span)+1):
                    px[u, v] = original.getpixel((u, v))
    if layout == 'LAYOUT_ROUTE119':
        cliff = {(98, 65, 82), (131, 90, 90), (156, 115, 115)}
        for v in range(h*16):
            for u in range(w*16):
                r, g, b, a = px[u, v]
                if (r, g, b) in cliff or (r > g+30 and r > b+25) or (g > r+20 and b < r):
                    px[u, v] = (0, 0, 0, 0)
    return art


def make(name, layout, rect, split, ground=(1,), terrain=(), trim=None, side_x=4):
    w, h = rect[2:]
    drawing = clean_art(layout, rect, ground, terrain, trim)
    # Reverse slopes and gable ends need opaque material even where the front
    # roof drawing has notched/transparent corners. Repeat an intact own-art
    # roof block in a dedicated band, preserving one texel per surface pixel.
    candidates = []
    for v in range(max(0, split-7)):
        for u in range(max(0, w*16-7)):
            tile = drawing.crop((u, v, u+8, v+8))
            if tile.getchannel('A').getextrema() == (255, 255):
                candidates.append((len(set(tile.getdata())), abs(u-w*8), u, v, tile))
    tile = min(candidates, key=lambda item: item[:4])[-1]
    art = Image.new('RGBA', (w*16, h*16+16))
    art.paste(drawing, (0, 0))
    for v in range(0, 16, 8):
        for u in range(0, w*16, 8):
            art.paste(tile, (u, h*16+v))
    return {'name': name, 'layout': layout, 'rect': rect, 'ground': list(ground),
            'art': art, 'drawing': drawing,
            'parts': partial(roof_and_facade, w*16, h*16, split, side_x),
            'exact': [(0, 0, w*16, split, True), (0, split, w*16, h*16)]}


SPECS = [
    make('route121_safari_entrance', 'LAYOUT_ROUTE121', (35, 0, 5, 6), 64, side_x=8),
    make('safari_zone_south_entrance', 'LAYOUT_SAFARI_ZONE_SOUTH', (30, 29, 10, 5), 48,
         trim=lambda u, v: v < 74, side_x=8),
    make('route119_weather_institute', 'LAYOUT_ROUTE119', (2, 26, 9, 7), 54,
         terrain=((1, 26), (1, 27)), trim=lambda u, v: v < 108, side_x=36),
]

# Do not reserve cells containing only surrounding terrain (the notched roof
# corners).
for _spec in SPECS:
    _art = _spec["art"]
    _w, _h = _spec["rect"][2:]
    _spec["owned"] = {(x, y) for y in range(_h) for x in range(_w)
                      if _art.crop((16*x, 16*y, 16*x+16, 16*y+16)).getchannel("A").getbbox()}


class WeatherRoofInstallations:
    """The two round roof housings: broad tapered drums and raised lids."""
    def emit(self, mesh):
        import math
        for cx in (16, 128):
            rings = []
            for y, cz, rx, rz in ((62, 96, 20, 18), (74, 98, 12, 12)):
                rings.append([(cx+rx*math.cos(k*math.pi/16), y,
                               cz+rz*math.sin(k*math.pi/16)) for k in range(32)])
            mesh.poly([p+(p[0], p[2]-p[1]) for p in rings[1]], SHADE_ART,
                      'weather_round_lid~proj')
            for k in range(32):
                a, b = rings[0][k], rings[0][(k+1) % 32]
                c, d = rings[1][(k+1) % 32], rings[1][k]
                front = (a[2]+b[2])/2 >= 96
                if front:
                    uv = [p+(p[0], p[2]-p[1]) for p in (a, b, c, d)]
                    tag = 'weather_tapered_drum~proj'
                else:
                    # Own cream housing material for the unseen reverse flank.
                    width = math.hypot(b[0]-a[0], b[2]-a[2])
                    uv = [a+(cx-4, 30), b+(cx-4+width, 30),
                          c+(cx-4+width, 18), d+(cx-4, 18)]
                    tag = 'weather_tapered_drum~behind~proj'
                mesh.poly(uv, SHADE_ART, tag)


_weather = next(s for s in SPECS if s['name'] == 'route119_weather_institute')
_weather_original = _weather['drawing']
_weather_mesh = Mesh()
WeatherRoofInstallations().emit(_weather_mesh)
_weather_raster = Raster(144, 112, bg=(0, 0, 0))
for _tri, _shade, _tag in _weather_mesh.tris:
    if '~behind' in _tag: continue
    _weather_raster.draw([(x, z-y, y+z, 1, u, v) for x, y, z, u, v in _tri],
                         _weather_original, 1, _tag)
_weather_texture = Image.new('RGBA', (144, 240))
_weather_texture.paste(_weather['art'], (0, 0))
_weather_texture.paste(_weather_original, (0, 128))
for _v in range(112):
    for _u in range(144):
        if _weather_raster.owner[_v*144+_u] is not None:
            _weather_texture.putpixel((_u, _v), (0, 0, 0, 0))
_weather['art'] = _weather_texture
_weather['parts'] = partial(lambda base: base()+[ArtOffset([WeatherRoofInstallations()], 128)],
                            _weather['parts'])
