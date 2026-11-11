"""Recognise the player's ROM.

Clean English (BPEE), Spanish (BPES), French (BPEF), Italian (BPEI), German (BPED) and
Japanese (BPEJ) Pokémon Emerald dumps are recognised (16 MiB, by SHA-1). Building still requires a recipe and
executable made for the same ROM. A trimmed dump (trailing 0xFF removed) is padded back before it is checked; a .zip holding a single .gba, .agb or .bin is
opened directly. The ROM is only ever read into memory: it is never copied,
written or sent anywhere. Clean English Ruby/Sapphire revisions 0-2 are
identified by fingerprint but always rejected as not yet supported. English
FireRed/LeafGreen revisions 0-1 are identified and rejected as not supported.
"""

from __future__ import annotations

import hashlib
import io
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path

from .errors import BuilderError

SUPPORTED_SHA1 = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"
SPANISH_SHA1 = "fe1558a3dcb0360ab558969e09b690888b846dd9"
FRENCH_SHA1 = "ca666651374d89ca439007bed54d839eb7bd14d0"
ITALIAN_SHA1 = "1692db322400c3141c5de2db38469913ceb1f4d4"
GERMAN_SHA1 = "61c2eb2b380b1a75f0c94b767a2d4c26cd7ce4e3"
JAPANESE_SHA1 = "d7cf8f156ba9c455d164e1ea780a6bf1945465c2"


@dataclass(frozen=True)
class RomProfile:
    """Verified input identity, separate from a payload's output language.

    A profile does not supply an engine: a matching recipe and executable
    are still required. Keep header-only recognition in KNOWN_CODES.
    """
    code: str
    name: str
    sha1: str
    region: str
    language: str
    game: str


# Fingerprints: libretro-database/metadat/no-intro/Nintendo - Game Boy Advance.dat
SUPPORTED_PROFILES = (
    RomProfile("BPEE", "Pokemon Emerald (USA, Europe)", SUPPORTED_SHA1, "USA, Europe", "en", "emerald"),
    RomProfile("BPES", "Pokemon Emerald (Spain)", SPANISH_SHA1, "Spain", "es", "emerald"),
    RomProfile("BPEF", "Pokemon Emerald (France)", FRENCH_SHA1, "France", "fr", "emerald"),
    RomProfile("BPEI", "Pokemon Emerald (Italy)", ITALIAN_SHA1, "Italy", "it", "emerald"),
    RomProfile("BPED", "Pokemon Emerald (Germany)", GERMAN_SHA1, "Germany", "de", "emerald"),
    RomProfile("BPEJ", "Pokemon Emerald (Japan)", JAPANESE_SHA1, "Japan", "ja", "emerald"),
)
# Recognition only: these must never enter ROM_PROFILES or a supported release.
# Source: pret/pokeruby at 5784633ce4ef7ade1a7f2d2d0c288e3d5e6cdd7f,
# ruby{,_rev1,_rev2}.sha1 and sapphire{,_rev1,_rev2}.sha1.
UNSUPPORTED_PROFILES = (
    RomProfile("AXVE", "Pokemon Ruby (English, Rev 0)",
               "f28b6ffc97847e94a6c21a63cacf633ee5c8df1e", "", "en", "ruby"),
    RomProfile("AXVE", "Pokemon Ruby (English, Rev 1)",
               "610b96a9c9a7d03d2bafb655e7560ccff1a6d894", "", "en", "ruby"),
    RomProfile("AXVE", "Pokemon Ruby (English, Rev 2)",
               "5b64eacf892920518db4ec664e62a086dd5f5bc8", "", "en", "ruby"),
    RomProfile("AXPE", "Pokemon Sapphire (English, Rev 0)",
               "3ccbbd45f8553c36463f13b938e833f652b793e4", "", "en", "sapphire"),
    RomProfile("AXPE", "Pokemon Sapphire (English, Rev 1)",
               "4722efb8cd45772ca32555b98fd3b9719f8e60a9", "", "en", "sapphire"),
    RomProfile("AXPE", "Pokemon Sapphire (English, Rev 2)",
               "89b45fb172e6b55d51fc0e61989775187f6fe63c", "", "en", "sapphire"),

    # Source: pret/pokefirered at ce6406d8c2eaf46d543c8a2606de0c3cb1951982,
    # firered{,_rev1}.sha1 and leafgreen{,_rev1}.sha1 (English GBA releases).
    RomProfile("BPRE", "Pokemon FireRed (English, Rev 0)",
               "41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc", "", "en", "firered"),
    RomProfile("BPRE", "Pokemon FireRed (English, Rev 1)",
               "dd5945db9b930750cb39d00c84da8571feebf417", "", "en", "firered"),
    RomProfile("BPGE", "Pokemon LeafGreen (English, Rev 0)",
               "574fa542ffebb14be69902d1d36f1ec0a4afd71e", "", "en", "leafgreen"),
    RomProfile("BPGE", "Pokemon LeafGreen (English, Rev 1)",
               "7862c67bdecbe21d1d69ce082ce34327e1c6ed5e", "", "en", "leafgreen"),
)


def unsupported_status(profile: RomProfile) -> str:
    """Only Ruby/Sapphire are candidates for the expansion in issue #247."""
    return "not yet supported" if profile.game in ("ruby", "sapphire") else "not supported"


def unsupported_profile(sha1: str) -> RomProfile | None:
    """Identify a verified ROM without granting permission to build it."""
    return next((profile for profile in UNSUPPORTED_PROFILES if profile.sha1 == sha1), None)


def reject_unsupported_rom(sha1: str) -> None:
    """Keep recognition separate from both validation and pack construction."""
    recognised = unsupported_profile(sha1)
    if recognised is not None:
        status = unsupported_status(recognised)
        if status == "not yet supported":
            detail = "Ruby/Sapphire support requires its own game engine and data recipe."
            error_code = "rom_not_yet_supported"
        else:
            detail = "FireRed/LeafGreen are outside the current scope of this port."
            error_code = "rom_not_supported"
        raise BuilderError("This ROM is %s, which is recognised but %s." % (recognised.name, status),
                           detail + " An Emerald recipe cannot be used with this ROM.", code=error_code)


# Keep the fingerprint mapping used by the builder and staging tools.
ROM_PROFILES = {profile.code: profile.sha1 for profile in SUPPORTED_PROFILES}
ROM_SIZE = 16 * 1024 * 1024
KNOWN_CODES = {
    **{profile.code: profile.name for profile in SUPPORTED_PROFILES},
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
        members = [info for info in zf.infolist()
                   if not info.is_dir() and info.filename.lower().endswith((".gba", ".agb", ".bin"))]
        if len(members) != 1:
            raise BuilderError("The ZIP file must contain exactly one ROM (.gba, .agb or .bin).",
                               code="rom_zip_contents")
        member = members[0]
        if member.file_size > ROM_SIZE:
            raise BuilderError("The ROM inside the ZIP file is larger than the supported 16 MiB.",
                               code="rom_too_large")
        if member.flag_bits & 1:
            raise BuilderError("The ROM inside the ZIP file is password-protected.",
                               "Extract it first, then select the ROM file.", code="rom_zip_encrypted")
        try:
            # Bound extraction as well as checking the archive's declared size.
            with zf.open(member) as stream:
                return stream.read(ROM_SIZE + 1)
        except (NotImplementedError, RuntimeError) as exc:
            raise BuilderError("The ZIP file uses an unsupported compression or encryption method.",
                               "Extract it first, then select the ROM file.",
                               code="rom_zip_unsupported") from exc


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
        except (zipfile.BadZipFile, EOFError, zlib.error) as exc:
            raise BuilderError("The ROM file could not be read.", str(exc), code="rom_unreadable") from exc
    if len(data) > ROM_SIZE:
        raise BuilderError("This file is larger than a GBA cartridge; it is not the supported ROM.",
                           code="rom_too_large")
    if len(data) < ROM_SIZE:
        # A trimmed dump: the cartridge's trailing 0xFF fill was cut off.
        data = data + b"\xff" * (ROM_SIZE - len(data))
    title, code = header(data)
    sha1 = hashlib.sha1(data).hexdigest()
    reject_unsupported_rom(sha1)
    if supported is None:
        supported = tuple(ROM_PROFILES.values())
    if sha1 not in supported:
        codes = [c for c, s in ROM_PROFILES.items() if s in supported] or ["BPEE"]
        names = " or ".join(KNOWN_CODES[c] for c in codes)
        what = KNOWN_CODES.get(code)
        if what and code.startswith(("BPR", "BPG")):
            raise BuilderError("This ROM is %s, which is not supported." % what,
                               "FireRed/LeafGreen are outside the current scope of this port.",
                               code="rom_wrong_game")
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
