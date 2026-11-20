"""Match a room's pixels against its repeating 16x16 floor tiles."""


def match_floor_pixels(art, floors):
    """Return the same floor mask as comparing every tile for every pixel.

    A room repeats the same 256 floor positions. Index the possible RGBA
    values at each position once, rather than scanning the floor tiles and
    creating a generator for each pixel in the room. Keep alpha in the key:
    a transparent pixel must not match an opaque pixel of the same colour.
    """
    pixels = art.load()
    tiles = [floor.load() for floor in floors]
    colours = [[frozenset(tile[x, y] for tile in tiles) for x in range(16)]
               for y in range(16)]
    width, height = art.size
    return [[pixels[x, y] in colours[y % 16][x % 16] for x in range(width)]
            for y in range(height)]
