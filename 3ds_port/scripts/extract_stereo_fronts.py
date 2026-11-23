#!/usr/bin/env python3
"""Extract exact front poses from a verified local Emerald ROM; no network calls.

Locate compiled upstream compressed sprites by unique byte match, then decode ROM
bytes independently and prove palette indices, 15-bit colors, and tiles match.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'builder'))
from emerald3ds_builder.rom import load_rom, SUPPORTED_SHA1


def sha(data):
    return hashlib.sha256(data).hexdigest()


def unlz(data, offset=0):
    if offset < 0 or offset + 4 > len(data) or data[offset] != 0x10:
        raise ValueError('Invalid GBA LZ77 header')
    size = int.from_bytes(data[offset+1:offset+4], 'little')
    if not 0 < size <= 1024 * 1024:
        raise ValueError('Invalid decompressed size')
    pos, out = offset + 4, bytearray()
    def take():
        nonlocal pos
        if pos >= len(data):
            raise ValueError('Truncated LZ77 stream')
        v = data[pos]; pos += 1
        return v
    while len(out) < size:
        flags = take()
        for bit in range(7, -1, -1):
            if len(out) == size:
                break
            if flags & (1 << bit):
                a, b = take(), take()
                count, distance = (a >> 4) + 3, ((a & 15) << 8 | b) + 1
                if distance > len(out) or len(out) + count > size:
                    raise ValueError('Invalid LZ77 back-reference')
                for _ in range(count):
                    out.append(out[-distance])
            else:
                out.append(take())
    return bytes(out), pos - offset


def untile(data, width=64):
    if width <= 0 or width % 8 or len(data) % (width * 4):
        raise ValueError('Invalid 4bpp tile dimensions')
    height = len(data) * 2 // width
    out = bytearray(width * height)
    for ty in range(height // 8):
        for tx in range(width // 8):
            base = (ty * (width // 8) + tx) * 32
            for y in range(8):
                for x in range(8):
                    value = data[base + y * 4 + x // 2]
                    out[(ty*8+y)*width+tx*8+x] = (value >> ((x % 2)*4)) & 15
    return bytes(out), height


def locate(rom, asset, allow_identical_duplicates=False):
    ref = asset.read_bytes()
    offset = rom.find(ref)
    if offset < 0:
        raise ValueError(f'No ROM match: {asset}')
    matches=[]; pos=offset
    while pos>=0:
        matches.append(pos); pos=rom.find(ref,pos+1)
    if len(matches)>1 and not allow_identical_duplicates:
        raise ValueError(f'Expected unique ROM match: {asset}')
    raw, consumed = unlz(rom, offset)
    return raw, dict(identical_match_offsets=matches, offset=offset, address=hex(0x08000000+offset),
                    compressed_sha256=sha(ref), compressed_size=len(ref),
                    stream_size=consumed, decoded_sha256=sha(raw),
                    locator=str(asset.resolve()))


def extract(rom_path, upstream, out, names):
    rom = load_rom(rom_path, supported=(SUPPORTED_SHA1,))
    records = []
    for dex, name in names:
        base = upstream / 'graphics/pokemon' / name
        src = base / 'a' if name == 'unown' else base
        palette_src = base / 'normal.gbapal.lz'
        for required in ('anim_front.4bpp.lz','anim_front.png','anim_front.4bpp'):
            if not (src/required).is_file():
                raise ValueError(f'{name}: unsupported layout or missing compiled asset {required}; alternate forms need explicit support')
        tiles, tm = locate(rom.data, src / 'anim_front.4bpp.lz')
        pal, pm = locate(rom.data, palette_src, allow_identical_duplicates=True)
        if len(tiles) != 4096 or len(pal) != 32:
            raise ValueError(f'{name}: unsupported dimensions; requires exactly two 64x64 poses and 16 colors (got {len(tiles)} tile bytes and {len(pal)} palette bytes)')
        indices, height = untile(tiles)
        words = struct.unpack('<16H', pal)
        colors = [( (w & 31)*8, ((w >> 5)&31)*8, ((w >> 10)&31)*8) for w in words]
        reference = Image.open(src / 'anim_front.png')
        if reference.mode != 'P' or reference.size != (64, height):
            raise ValueError('Reference must be indexed with matching dimensions')
        index_mismatches = sum(a != b for a,b in zip(indices, reference.tobytes()))
        rp = reference.getpalette()
        expected = [(rp[i*3] >> 3) | ((rp[i*3+1] >> 3)<<5) | ((rp[i*3+2] >> 3)<<10) for i in range(16)]
        palette_mismatches = sum(a != b for a,b in zip(words, expected))
        reference_tiles = (src/'anim_front.4bpp').read_bytes()
        if len(reference_tiles) != len(tiles):
            raise ValueError(f'{name}: reference tile length differs from ROM')
        tile_mismatches = sum(a != b for a,b in zip(tiles, reference_tiles))
        if index_mismatches or palette_mismatches or tile_mismatches:
            raise ValueError(f'ROM/reference mismatch: {name}')
        dest = out / 'root/graphics/pokemon' / name
        dest.mkdir(parents=True, exist_ok=True)
        img = Image.frombytes('P', (64, height), indices)
        img.putpalette([c for rgb in colors for c in rgb] + [0]*720)
        img.info['transparency'] = 0
        img.save(dest/'anim_front.png', transparency=0)
        (dest/'normal.gbapal').write_bytes(pal)
        (dest/'anim_front.4bpp').write_bytes(tiles)
        # JASC palette consumed by the existing exporter.
        (dest/'normal.pal').write_text('JASC-PAL\n0100\n16\n' + ''.join('%d %d %d\n' % rgb for rgb in colors))
        for pose in range(2):
            img.convert('RGBA').crop((0, pose*64,64,(pose+1)*64)).save(dest/f'pose-{pose}.png')
        (out/'sprites').mkdir(parents=True, exist_ok=True)
        frames=[img.crop((0,p*64,64,(p+1)*64)) for p in range(2)]
        for frame in frames:
            frame.putpalette([c for rgb in colors for c in rgb] + [0]*720)
        frames[0].save(out/'sprites'/f'{dex}.gif',save_all=True,append_images=frames[1:],duration=200,loop=0,transparency=0,disposal=2,optimize=False)
        gif = Image.open(out/'sprites'/f'{dex}.gif')
        if gif.n_frames != 2 or gif.size != (64,64):
            raise ValueError('GIF dimensions/frame count changed')
        for pose in range(2):
            gif.seek(pose)
            if gif.convert('RGBA').tobytes() != frames[pose].convert('RGBA').tobytes():
                raise ValueError('GIF altered ROM pixels')
        records.append(dict(dex=dex, name=name, form=('A' if name=='unown' else 'default'), dimensions=[64,height], sprite=tm, palette=pm,
                            gif_rgba_mismatches=0,index_mismatches=index_mismatches,palette_15bit_mismatches=palette_mismatches,
                            tile_byte_mismatches=tile_mismatches, indices_sha256=sha(indices),
                            reference_png_sha256=sha((src/'anim_front.png').read_bytes()),
                            extracted_png_sha256=sha((dest/'anim_front.png').read_bytes()),
                            palette_rendering='5-bit values shifted left 3; transparent index 0',
                            source=str(dest/'anim_front.png')))
    (out/'root/include/constants').mkdir(parents=True,exist_ok=True)
    shutil.copyfile(upstream/'include/constants/pokedex.h',out/'root/include/constants/pokedex.h')
    manifest=dict(rom_sha1=rom.sha1,rom_code=rom.code, method='Exact compressed-byte lookup; sprites unique, identical palette matches recorded; independently decoded from verified ROM',species=records)
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


if __name__ == '__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--ids',type=int,nargs='+',default=[12,68],help='National dex IDs, default: 12 68')
    ap.add_argument('--rom',type=Path,default=ROOT/'Pokemon - Emerald Version (USA, Europe).gba')
    ap.add_argument('--upstream',type=Path,default=ROOT/'build/upstream')
    ap.add_argument('--out',type=Path,default=ROOT/'output/stereo-rom-pilot/extraction')
    args=ap.parse_args()
    dex_names = re.findall(r'^\s*NATIONAL_DEX_([A-Z0-9_]+),', (args.upstream/'include/constants/pokedex.h').read_text(),re.M)
    names=[]
    for dex in dict.fromkeys(args.ids):
        if not 1 <= dex <= 386 or dex >= len(dex_names):
            ap.error(f'Unsupported national dex ID {dex}; expected 1..386')
        name=dex_names[dex].lower()
        names.append((dex,name))
    try:
        print(json.dumps(extract(args.rom,args.upstream,args.out,names),indent=2))
    except ValueError as exc:
        ap.error(str(exc))
