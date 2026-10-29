"""Follower sprites and texts: HeartGold/SoulSilver's, fetched and converted.

The sprites are not the player's ROM's and never ship with the port: the
builder fetches them from a fork of pret/pokeheartgold at the commit that
followers_sources.py pins, checks each file's size and SHA-256 against it,
and converts them for the pack. Nothing fetched reaches the SD card as it
is.

The pack section is optional: its files go into the pack but not into the
engine ABI (staging.py, OPTIONAL_PATTERNS), so a build that cannot fetch them
(offline, a refused download) still makes a working pack: without a sprite
there is no follower, the visible wild Pokemon keep their icons, and without
the texts the follower only cries.

Pack files:

    followers/mon/KEY.bin   one per sprite, KEY = national dex number "025",
                            "025_f" (female), "201_03" (Unown's letter 3)
    followers/text/LL.bin   the follower messages in one language (en, es,
                            fr, it, de): HGSS's message bank 265
    followers/sources.txt   the pinned commits

A .bin (little endian):

    0   magic   b"FMN1"
    4   width   u8, pixels (32 or 64)
    5   height  u8
    6   frames  u8, 8: up, up, down, down, left, left, right, right
    7   flags   u8, bit 0 too tall, bit 1 HGSS's tp_param column 3, bit 2 hover
    8   palette u16 x 16, normal (BGR555 as the GBA's, colour 0 transparent)
    40  palette u16 x 16, shiny
    72  tiles   frames x width * height / 2 bytes, each frame in 8x8 4bpp
                tiles row by row, as an OBJ in the GBA's one-dimensional
                mapping

A text .bin (little endian), the 759 messages of HGSS's bank 265 in its order:

    0   magic   b"FMT1"
    4   count   u16
    6   0       u16
    8   offsets u32 x count, from the start of the file
        strings in the game's character set, each ending in 0xFF. The
        Pokemon's name is STR_VAR_1, the player's PLAYER, an item's
        STR_VAR_2; HGSS's line breaks are spaces (the game wraps the text to
        its own box, names expanded) and its page breaks the game's
        paragraph code. HGSS's yes/no marker is dropped.

Run as a script (no package imports) by the port's build for a development
RomFS: python followers.py --sources DIR --out ROMFS_DIR
"""

from __future__ import annotations

import hashlib
import os
import re
import struct
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PREFIX = "followers/"
MAGIC = b"FMN1"
TEXT_MAGIC = b"FMT1"
TEXT_BANK = 265
TEXT_COUNT = 759
LANGUAGES = ("en", "es", "fr", "it", "de")
# The fourth letter of the game code: the ROM's language.
ROM_LANGUAGES = {"E": "en", "S": "es", "F": "fr", "I": "it", "D": "de"}
FRAMES = 8
HOSTS = ("https://raw.githubusercontent.com/{repo}/{commit}/{path}",
         "https://cdn.jsdelivr.net/gh/{repo}@{commit}/{path}")


class FollowersError(Exception):
    pass


def load_sources() -> dict:
    try:
        from .followers_sources import SOURCES
    except ImportError:  # run as a script
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from followers_sources import SOURCES
    return SOURCES


def sprite_files(sources: dict) -> list[dict]:
    return [f for f in sources["files"] if f.get("key")]


def text_files(sources: dict, languages=LANGUAGES) -> list[dict]:
    return [f for f in sources["files"] if f.get("language") in languages]


def pack_paths(sources: dict | None = None) -> list[str]:
    """Every path the section can hold."""
    sources = sources or load_sources()
    return (["%smon/%s.bin" % (PREFIX, f["key"]) for f in sprite_files(sources)]
            + ["%stext/%s.bin" % (PREFIX, lang) for lang in LANGUAGES] + [PREFIX + "sources.txt"])


def urls(sources: dict, entry: dict, hosts=HOSTS) -> list[str]:
    src = sources["sources"][entry["source"]]
    return [h.format(repo=src["repo"], commit=src["commit"], path=entry["path"]) for h in hosts]


def check(entry: dict, data: bytes) -> bool:
    return len(data) == entry["size"] and hashlib.sha256(data).hexdigest() == entry["sha256"]


# --- NSBTX ------------------------------------------------------------------

def _dictionary(buf: bytes, off: int) -> list[tuple[str, bytes]]:
    """A Nitro info dictionary: [(name, entry bytes)]."""
    _, count, _ = struct.unpack_from("<BBH", buf, off)
    p = off + 4 + 8 + 4 * count
    size, _ = struct.unpack_from("<HH", buf, p)
    p += 4
    entries = [buf[p + i * size:p + (i + 1) * size] for i in range(count)]
    p += size * count
    names = [buf[p + i * 16:p + (i + 1) * 16].rstrip(b"\0").decode("latin-1") for i in range(count)]
    return list(zip(names, entries))


def parse_nsbtx(buf: bytes) -> tuple[list[dict], list[list[int]]]:
    """The textures (w, h, fmt, t0, data) and 16-colour palettes of a BTX0."""
    if buf[:4] != b"BTX0":
        raise FollowersError("not a BTX0 file")
    t = struct.unpack_from("<I", buf, 0x10)[0]
    if buf[t:t + 4] != b"TEX0":
        raise FollowersError("no TEX0 block")
    tex_info = struct.unpack_from("<H", buf, t + 0x0E)[0]
    tex_data = struct.unpack_from("<I", buf, t + 0x14)[0]
    pal_size = struct.unpack_from("<I", buf, t + 0x30)[0] << 3
    pal_info = struct.unpack_from("<I", buf, t + 0x34)[0]
    pal_data = struct.unpack_from("<I", buf, t + 0x38)[0]
    textures = []
    for _, e in _dictionary(buf, t + tex_info):
        param = struct.unpack_from("<I", e, 0)[0]
        w, h = 8 << ((param >> 20) & 7), 8 << ((param >> 23) & 7)
        start = t + tex_data + ((param & 0xFFFF) << 3)
        textures.append({"w": w, "h": h, "fmt": (param >> 26) & 7, "t0": (param >> 29) & 1,
                         "data": buf[start:start + w * h // 2]})
    palettes = []
    for _, e in _dictionary(buf, t + pal_info):
        start = t + pal_data + (struct.unpack_from("<H", e, 0)[0] << 3)
        raw = buf[start:t + pal_data + pal_size]
        if len(raw) < 32:
            raise FollowersError("short palette")
        palettes.append([c & 0x7FFF for c in struct.unpack_from("<16H", raw, 0)])
    return textures, palettes


def gba_tiles(linear: bytes, w: int, h: int) -> bytes:
    """A 4bpp linear texture (low nibble = left pixel, as the GBA's) in 8x8 tiles."""
    row = w // 2
    out = bytearray()
    for ty in range(0, h, 8):
        for tx in range(0, row, 4):
            for y in range(ty, ty + 8):
                out += linear[y * row + tx:y * row + tx + 4]
    return bytes(out)


def convert(nsbtx: bytes, flags: int = 0) -> bytes:
    textures, palettes = parse_nsbtx(nsbtx)
    if len(textures) != FRAMES or len(palettes) < 2:
        raise FollowersError("%d textures, %d palettes" % (len(textures), len(palettes)))
    w, h = textures[0]["w"], textures[0]["h"]
    for tex in textures:
        if (tex["w"], tex["h"], tex["fmt"], tex["t0"]) != (w, h, 3, 1) or len(tex["data"]) != w * h // 2:
            raise FollowersError("unexpected texture %dx%d format %d" % (tex["w"], tex["h"], tex["fmt"]))
    head = MAGIC + bytes((w, h, FRAMES, flags))
    return (head + struct.pack("<16H", *palettes[0]) + struct.pack("<16H", *palettes[1])
            + b"".join(gba_tiles(tex["data"], w, h) for tex in textures))


def param_flags(csv_text: str) -> list[int]:
    """tp_param.csv, row = follower index: too_tall, column 3, hover."""
    flags = []
    for line in csv_text.splitlines()[1:]:
        cols = line.strip().split(",")
        if len(cols) >= 5:
            flags.append((cols[2] == "true") | (cols[3] == "true") << 1 | (cols[4] == "true") << 2)
    return flags


# --- fetching ---------------------------------------------------------------

# The game's character set (charmap.txt) for what the five languages' bank 265
# holds. Letters and digits are computed; anything else unknown becomes "?".
_CHARS = {
    " ": 0x00, "Ç": 0x04, "È": 0x05, "à": 0x16, "á": 0x17, "ç": 0x19, "è": 0x1A, "é": 0x1B,
    "ê": 0x1C, "î": 0x20, "ò": 0x22, "ó": 0x23, "ô": 0x24, "œ": 0x25, "ù": 0x26, "ú": 0x27,
    "û": 0x28, "ñ": 0x29, "¿": 0x51, "¡": 0x52, "â": 0x68, "í": 0x6F, "!": 0xAB, "?": 0xAC,
    ".": 0xAD, "-": 0xAE, "'": 0xB4, "’": 0xB4, ",": 0xB8, ":": 0xF0, "ü": 0xF6,
    "ß": 0x15, "Ä": 0xF1, "Ö": 0xF2, "Ü": 0xF3, "ä": 0xF4, "ö": 0xF5,
}
_VARS = {"0101": b"\xFD\x02", "0103": b"\xFD\x01", "0108": b"\xFD\x03", "0200": b""}
_TOKEN = re.compile(r"\[VAR ([0-9A-F]{4})\([^\]]*\)\]|\\(.)|(.)", re.S)


def encode_message(text: str) -> bytes:
    """One corpus line in the game's character set (see the module's text
    format)."""
    out = bytearray()
    for var, esc, ch in _TOKEN.findall(text):
        if var:
            if var not in _VARS:
                raise FollowersError("unknown text variable %s" % var)
            out += _VARS[var]
        elif esc:
            out.append(0xFB if esc == "c" else 0x00)
        elif "0" <= ch <= "9":
            out.append(0xA1 + ord(ch) - ord("0"))
        elif "A" <= ch <= "Z":
            out.append(0xBB + ord(ch) - ord("A"))
        elif "a" <= ch <= "z":
            out.append(0xD5 + ord(ch) - ord("a"))
        else:
            out.append(_CHARS.get(ch, 0xAC))
    # No space next to a page break, nor two in a row.
    data = re.sub(rb"\x00*\xFB\x00*", b"\xFB", bytes(out))
    data = re.sub(rb"\x00{2,}", b"\x00", data).strip(b"\x00")
    return data + b"\xFF"


def bank_messages(corpus: str, bank: int = TEXT_BANK) -> list[str]:
    """One bank of a poke-corpus *_msg.txt, a message a line."""
    lines = corpus.replace("\r\n", "\n").split("\n")
    try:
        start = lines.index("Text File : %d" % bank) + 2
    except ValueError:
        raise FollowersError("bank %d is not in the text" % bank)
    end = start
    while end < len(lines) and not lines[end].startswith("~~~"):
        end += 1
    return lines[start:end]


def convert_texts(corpus: str) -> bytes:
    """A corpus file's bank 265 as a text .bin."""
    messages = bank_messages(corpus)
    if len(messages) != TEXT_COUNT:
        raise FollowersError("bank %d has %d messages, not %d" % (TEXT_BANK, len(messages), TEXT_COUNT))
    blobs = [encode_message(m) for m in messages]
    offsets, at = [], 8 + 4 * len(blobs)
    for b in blobs:
        offsets.append(at)
        at += len(b)
    return (TEXT_MAGIC + struct.pack("<HH", len(blobs), 0) + struct.pack("<%dI" % len(blobs), *offsets)
            + b"".join(blobs))


def default_cache() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_CACHE_HOME") \
        or os.path.join(os.path.expanduser("~"), ".cache")
    return Path(base) / "Emerald3DS" / "cache"


def xhr_fetch(url: str) -> bytes:
    """A synchronous GET from Pyodide in a Web Worker (the web builder)."""
    from js import XMLHttpRequest  # noqa: PLC0415 - only under Pyodide
    request = XMLHttpRequest.new()
    request.open("GET", url, False)
    request.responseType = "arraybuffer"
    request.send(None)
    if request.status != 200:
        raise OSError("HTTP %d" % request.status)
    return bytes(request.response.to_py())


def urllib_fetch(url: str) -> bytes:
    import urllib.request
    with urllib.request.urlopen(url, timeout=30) as response:
        return response.read()


class Fetcher:
    """Gets each listed file from a local copy, the cache or the network,
    keeping only what matches its pinned size and SHA-256."""

    def __init__(self, sources: dict, local: list[Path] = (), cache: Path | None = None,
                 fetch=urllib_fetch, network: bool = True, workers: int = 8, hosts=HOSTS):
        self.sources, self.local, self.cache = sources, [Path(p) for p in local], cache
        self.fetch, self.network, self.workers, self.hosts = fetch, network, workers, hosts
        self.downloaded = 0
        self._lock = threading.Lock()

    def get(self, entry: dict) -> bytes | None:
        for root in self.local:
            path = root / entry["source"] / entry["path"]
            if path.is_file():
                data = path.read_bytes()
                if check(entry, data):
                    return data
        cached = self.cache / entry["sha256"] if self.cache else None
        if cached is not None and cached.is_file():
            data = cached.read_bytes()
            if check(entry, data):
                return data
        if not self.network:
            return None
        for url in urls(self.sources, entry, self.hosts):
            try:
                data = self.fetch(url)
            except Exception:  # noqa: BLE001 - any failure: next host
                continue
            if check(entry, data):
                with self._lock:
                    self.downloaded += 1
                if cached is not None:
                    try:
                        cached.parent.mkdir(parents=True, exist_ok=True)
                        tmp = cached.with_suffix(".tmp")
                        tmp.write_bytes(data)
                        tmp.replace(cached)
                    except OSError:
                        pass
                return data
        return None


def default_fetcher() -> Fetcher:
    """Local copies first (EMERALD3DS_FOLLOWERS_DIR, the private workspace's
    private_assets/hgss_followers), then the cache, then the network."""
    local = [Path(p) for p in os.environ.get("EMERALD3DS_FOLLOWERS_DIR", "").split(os.pathsep) if p]
    workspace = Path(__file__).resolve().parents[2] / "private_assets" / "hgss_followers"
    if workspace.is_dir():
        local.append(workspace)
    return Fetcher(load_sources(), local, default_cache())


def build_section(fetcher: Fetcher, progress=None, languages=LANGUAGES) -> tuple[dict[str, bytes], dict]:
    """The pack's followers/ files, and a summary {sprites, texts, missing,
    downloaded}. `languages`: the texts to fetch (the ROM's own for a
    player's pack, all four for a development build).

    A file that cannot be had is left out (a species without its sprite has
    no follower, a language without its text a follower that does not speak);
    the build goes on."""
    sources = fetcher.sources
    params = next(f for f in sources["files"] if f["path"].endswith("tp_param.csv"))
    csv = fetcher.get(params)
    flags = param_flags(csv.decode("utf-8")) if csv else []
    entries = sprite_files(sources)
    files: dict[str, bytes] = {}
    missing = []

    def one(entry):
        return entry, fetcher.get(entry)

    done = 0
    if fetcher.workers > 1:
        with ThreadPoolExecutor(fetcher.workers) as pool:
            results = pool.map(one, entries)
            for entry, data in results:
                done += 1
                _collect(entry, data, flags, files, missing)
                if progress and done % 20 == 0:
                    progress(done / len(entries))
    else:
        for entry in entries:
            entry, data = one(entry)
            done += 1
            _collect(entry, data, flags, files, missing)
            if progress and done % 20 == 0:
                progress(done / len(entries))
    texts = 0
    for entry in text_files(sources, languages):
        data = fetcher.get(entry)
        try:
            if data is None:
                raise FollowersError("not fetched")
            files["%stext/%s.bin" % (PREFIX, entry["language"])] = convert_texts(data.decode("utf-8"))
            texts += 1
        except (FollowersError, UnicodeDecodeError):
            missing.append("text_" + entry["language"])
    if files:
        files[PREFIX + "sources.txt"] = "".join(
            "%s %s %s\n" % (name, s["repo"], s["commit"]) for name, s in sorted(sources["sources"].items())
        ).encode("ascii")
    return files, {"sprites": len(files) - texts - (1 if files else 0), "texts": texts,
                   "missing": missing, "downloaded": fetcher.downloaded}


def _collect(entry, data, flags, files, missing):
    if data is None:
        missing.append(entry["key"])
        return
    index = entry["follower_index"]
    try:
        files["%smon/%s.bin" % (PREFIX, entry["key"])] = convert(data, flags[index] if index < len(flags) else 0)
    except (FollowersError, struct.error):
        missing.append(entry["key"])


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Convert the follower sprites into a RomFS staging folder.")
    ap.add_argument("--sources", type=Path, action="append", default=[],
                    help="a local copy laid out as private_assets/hgss_followers")
    ap.add_argument("--out", type=Path, required=True, help="the RomFS folder (followers/ goes in it)")
    ap.add_argument("--offline", action="store_true", help="never use the network")
    args = ap.parse_args(argv)
    fetcher = default_fetcher()
    fetcher.local[:0] = args.sources
    fetcher.network = not args.offline
    files, info = build_section(fetcher)
    for rel, data in files.items():
        path = args.out / rel
        if path.is_file() and path.read_bytes() == data:
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    stamp = args.out / (PREFIX + "sources.txt")
    if stamp.is_file():
        stamp.touch()  # the build's target: newer than its inputs
    print("followers: %d sprites, %d texts, %d missing, %d downloaded"
          % (info["sprites"], info["texts"], len(info["missing"]), info["downloaded"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
