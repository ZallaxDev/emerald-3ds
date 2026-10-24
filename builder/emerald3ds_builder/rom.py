"""Recognise the player's ROM.

Clean English (BPEE), Spanish (BPES), French (BPEF), Italian (BPEI) and German (BPED) Pokémon
Emerald dumps are recognised (16 MiB, by SHA-1). Building still requires a recipe and
executable made for the same ROM. A trimmed dump (trailing 0xFF removed) is padded back before it is checked; a .zip holding a single .gba is
opened directly. The ROM is only ever read into memory: it is never copied,
written or sent anywhere.
"""

from __future__ import annotations

import hashlib
import io
import zipfile
from dataclasses import dataclass
from pathlib import Path

from .errors import BuilderError

SUPPORTED_SHA1 = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"
SPANISH_SHA1 = "fe1558a3dcb0360ab558969e09b690888b846dd9"
FRENCH_SHA1 = "ca666651374d89ca439007bed54d839eb7bd14d0"
ITALIAN_SHA1 = "1692db322400c3141c5de2db38469913ceb1f4d4"
GERMAN_SHA1 = "61c2eb2b380b1a75f0c94b767a2d4c26cd7ce4e3"
# Fingerprints: libretro-database/metadat/no-intro/Nintendo - Game Boy Advance.dat
ROM_PROFILES = {"BPEE": SUPPORTED_SHA1, "BPES": SPANISH_SHA1, "BPEF": FRENCH_SHA1,
                "BPEI": ITALIAN_SHA1, "BPED": GERMAN_SHA1}
ROM_SIZE = 16 * 1024 * 1024
KNOWN_CODES = {
    "BPEE": "Pokemon Emerald (USA, Europe)",
    "BPEJ": "Pokemon Emerald (Japan)",
    "BPES": "Pokemon Emerald (Spain)",
    "BPED": "Pokemon Emerald (Germany)",
    "BPEF": "Pokemon Emerald (France)",
    "BPEI": "Pokemon Emerald (Italy)",
    "AXVE": "Pokemon Ruby",
    "AXVJ": "Pokemon Ruby (Japan)",
    "AXVS": "Pokemon Ruby (Spain)",
    "AXVD": "Pokemon Ruby (Germany)",
    "AXVF": "Pokemon Ruby (France)",
    "AXVI": "Pokemon Ruby (Italy)",
    "AXPE": "Pokemon Sapphire",
    "AXPJ": "Pokemon Sapphire (Japan)",
    "AXPS": "Pokemon Sapphire (Spain)",
    "AXPD": "Pokemon Sapphire (Germany)",
    "AXPF": "Pokemon Sapphire (France)",
    "AXPI": "Pokemon Sapphire (Italy)",
    "BPRE": "Pokemon FireRed",
    "BPRJ": "Pokemon FireRed (Japan)",
    "BPRS": "Pokemon FireRed (Spain)",
    "BPRD": "Pokemon FireRed (Germany)",
    "BPRF": "Pokemon FireRed (France)",
    "BPRI": "Pokemon FireRed (Italy)",
    "BPGE": "Pokemon LeafGreen",
    "BPGJ": "Pokemon LeafGreen (Japan)",
    "BPGS": "Pokemon LeafGreen (Spain)",
    "BPGD": "Pokemon LeafGreen (Germany)",
    "BPGF": "Pokemon LeafGreen (France)",
    "BPGI": "Pokemon LeafGreen (Italy)",
}


@dataclass
class Rom:
    data: bytes
    sha1: str
    title: str
    code: str
    source: Path


def _unzip(data: bytes) -> bytes:
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        names = [n for n in zf.namelist() if n.lower().endswith((".gba", ".agb", ".bin"))]
        if len(names) != 1:
            raise BuilderError("The ZIP file must contain exactly one .gba file.", code="rom_zip_contents")
        return zf.read(names[0])


def header(data: bytes) -> tuple[str, str]:
    if len(data) < 0xC0:
        return "", ""
    title = data[0xA0:0xAC].split(b"\0")[0].decode("ascii", "replace")
    code = data[0xAC:0xB0].decode("ascii", "replace")
    return title, code


def check_rom_bytes(data: bytes, source: Path, supported: tuple[str, ...] | None = None) -> Rom:
    """Recognise ROM bytes already in memory (the web builder has no file)."""
    if source.suffix.lower() == ".zip":
        try:
            data = _unzip(data)
        except zipfile.BadZipFile as exc:
            raise BuilderError("The ROM file could not be read.", str(exc), code="rom_unreadable") from exc
    if len(data) > ROM_SIZE:
        raise BuilderError("This file is larger than a GBA cartridge; it is not the supported ROM.",
                           code="rom_too_large")
    if len(data) < ROM_SIZE:
        # A trimmed dump: the cartridge's trailing 0xFF fill was cut off.
        data = data + b"\xff" * (ROM_SIZE - len(data))
    title, code = header(data)
    sha1 = hashlib.sha1(data).hexdigest()
    if supported is None:
        supported = tuple(ROM_PROFILES.values())
    if sha1 not in supported:
        codes = [c for c, s in ROM_PROFILES.items() if s in supported] or ["BPEE"]
        names = " or ".join(KNOWN_CODES[c] for c in codes)
        what = KNOWN_CODES.get(code)
        if what and code not in codes:
            raise BuilderError("This ROM is %s. Only %s is supported." % (what, names),
                               code="rom_wrong_game")
        if code in codes:
            raise BuilderError(
                "This is a %s ROM, but not an unmodified one." % what,
                "Patched, hacked or bad dumps are not supported. Use a clean dump of your cartridge "
                "(SHA-1 %s)." % ROM_PROFILES[code], code="rom_modified")
        raise BuilderError("This file is not the supported ROM.",
                           "Expected %s, SHA-1 %s." % (names, " or ".join(ROM_PROFILES[c] for c in codes)),
                           code="rom_unsupported")
    return Rom(data=data, sha1=sha1, title=title, code=code, source=source)


def load_rom(path: Path, supported: tuple[str, ...] | None = None) -> Rom:
    path = Path(path)
    if not path.is_file():
        raise BuilderError("The ROM file was not found:\n%s" % path.name, code="rom_not_found")
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise BuilderError("The ROM file could not be read.", str(exc), code="rom_unreadable") from exc
    return check_rom_bytes(data, path, supported)
