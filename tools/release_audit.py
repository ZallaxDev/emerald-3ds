#!/usr/bin/env python3
"""Refuse to publish anything that must not leave the maintainer's machine.

Run it on the public repository, on an export directory or on a release ZIP:

    python tools/release_audit.py --repo . --strict
    python tools/release_audit.py --zip dist/Emerald3DS-v0.1.0-Windows.zip --strict
    python tools/release_audit.py --repo . --strict --rom baserom.gba
    python tools/release_audit.py --zip dist/Emerald3DS-WebPayload.zip --strict --web-payload

What fails the audit:

* ROMs, saves, dumps, console executables of other platforms, logs, Python
  bytecode, editor/agent state (project skills under .claude/skills/ are
  documents and pass), session transcripts, generated data packs;
* files under a validation/ directory or generated RomFS content;
* files larger than 1 MiB that are not listed in the allowlist;
* media files (images, audio, tile data) with no provenance entry;
* absolute paths of a local machine, tokens and private keys;
* anything that looks like a GBA ROM (header, logo) or a Emerald 3DS data pack;
* third_party/ directories without a licence and a provenance entry;
* any pattern of the local denylist (`.public-denylist`, never versioned);
* with --rom: any file that contains a run of the ROM's own bytes;
* with --web-payload: a web payload whose web-manifest.json is missing or
  invalid, whose files differ from the manifest's hashes, or that holds files
  outside payload/, python/ and licenses/.

The allowlist is tools/release_audit_allow.toml. The denylist is a plain text
file, one case-insensitive regular expression per line, read from
`.public-denylist` next to this repository or from --denylist; it exists only on
the maintainer's machine, so the export never carries the list of names it
keeps out.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import io
import os
import re
import subprocess
import sys
import tomllib
import zipfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
DEFAULT_ALLOW = HERE / "release_audit_allow.toml"
MAX_BYTES = 1024 * 1024
# Runs of ROM bytes a file may share by coincidence (small constant tables in
# compiled code). Anything larger is reported.
ROM_TOLERANCE = 256

FORBIDDEN_SUFFIXES = {
    ".gba", ".agb", ".nds", ".3ds", ".cci", ".cia", ".cxi", ".sav", ".srm", ".sgm",
    ".ml1", ".ml2", ".ml3", ".log", ".pyc", ".pyo", ".pak", ".elf", ".map", ".nsbtx",
}
FORBIDDEN_DIRS = {"__pycache__", ".claude", ".agents", "validation", "romfs_generated",
                  "private_assets", "hgss_followers"}
# HeartGold/SoulSilver's follower sprites, as fetched (BTX0) or converted
# (FMN1, builder/emerald3ds_builder/followers.py): the builder downloads them
# on the player's computer; no repository or release carries them.
FORBIDDEN_MAGIC = (b"BTX0", b"FMN1")
FORBIDDEN_NAMES = [re.compile(r"^session-.*\.md$", re.I), re.compile(r"^baserom.*", re.I)]
# RomFS may only carry these hand-written files; anything else there is output.
ROMFS_ALLOWED = {".gitignore", "boot.txt"}
MEDIA_SUFFIXES = {
    ".png", ".bmp", ".gif", ".jpg", ".jpeg", ".webp", ".ico", ".tga",
    ".wav", ".aif", ".aiff", ".mid", ".midi", ".ogg", ".mp3", ".bcstm", ".bcwav",
    ".4bpp", ".8bpp", ".1bpp", ".gbapal", ".pal", ".lz", ".rl", ".bin", ".rgba5551",
}
TEXT_SUFFIXES = {
    ".c", ".h", ".s", ".inc", ".py", ".md", ".txt", ".toml", ".json", ".yml", ".yaml",
    ".mk", ".ps1", ".sh", ".cfg", ".ini", ".pica", ".v", ".patch", ".lock", ".spec", "",
}
LOCAL_PATH = re.compile(
    rb"(?:[A-Za-z]:\\\\?Users\\\\?[^\\\s\"'<>]+|/home/[a-z_][a-z0-9_-]*/|/Users/[A-Za-z0-9._-]+/)")
SECRETS = [
    (re.compile(rb"ghp_[A-Za-z0-9]{36}"), "GitHub token"),
    (re.compile(rb"github_pat_[A-Za-z0-9_]{40,}"), "GitHub token"),
    (re.compile(rb"AKIA[0-9A-Z]{16}"), "AWS key"),
    (re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH |)PRIVATE KEY-----"), "private key"),
    (re.compile(rb"xox[baprs]-[A-Za-z0-9-]{10,}"), "Slack token"),
]
GBA_LOGO_PREFIX = bytes.fromhex("24FFAE51699AA2213D84820A84E409AD")
PAK_MAGIC = b"EM3DPAK\0"
KNOWN_ROM_SHA1 = {"f3ae088181bf583e55daf962a92bb46f4f1d07b7",
                  "fe1558a3dcb0360ab558969e09b690888b846dd9",
                  "ca666651374d89ca439007bed54d839eb7bd14d0",
                  "1692db322400c3141c5de2db38469913ceb1f4d4"}


@dataclass
class Finding:
    path: str
    rule: str
    detail: str = ""


@dataclass
class Allow:
    large: set[str] = field(default_factory=set)
    media: dict[str, str] = field(default_factory=dict)
    local_paths: set[str] = field(default_factory=set)
    skip: list[str] = field(default_factory=list)
    runtime: list[str] = field(default_factory=list)

    @classmethod
    def load(cls, path: Path | None) -> "Allow":
        if path is None or not path.exists():
            return cls()
        data = tomllib.loads(path.read_text(encoding="utf-8"))
        return cls(large=set(data.get("large", {}).get("paths", [])),
                   media={k: v for k, v in data.get("media", {}).items()},
                   local_paths=set(data.get("local_paths", {}).get("paths", [])),
                   skip=list(data.get("skip", {}).get("globs", [])),
                   runtime=list(data.get("runtime", {}).get("globs", [])))

    def matches(self, patterns, path: str) -> bool:
        return any(fnmatch.fnmatchcase(path, p) for p in patterns)


class RomIndex:
    """Sampled 32-byte windows of the ROM, for spotting copied runs.

    Every 8th window is indexed, so any copied run of 40 bytes or more is
    found. Low-entropy windows (padding, fills) are ignored on both sides.
    """

    K = 32
    STRIDE = 8

    def __init__(self, rom: bytes):
        self.hashes: set[int] = set()
        for off in range(0, len(rom) - self.K, self.STRIDE):
            window = rom[off:off + self.K]
            if self.interesting(window):
                self.hashes.add(hash(window))

    @staticmethod
    def interesting(window: bytes) -> bool:
        # A byte ramp (0x00, 0x01, 0x02...) is a counting table that any
        # program may hold (the builder's C runtime does), not game content.
        if all((b - a) & 0xFF == 1 for a, b in zip(window, window[1:])):
            return False
        return len(set(window)) >= 10

    def copied_bytes(self, data: bytes) -> int:
        hits = 0
        last = -self.K
        view = data
        hashes = self.hashes
        k = self.K
        for off in range(0, len(view) - k + 1):
            window = view[off:off + k]
            if hash(window) in hashes and self.interesting(window):
                hits += min(k, off - last)
                last = off
        return hits


def load_denylist(path: Path | None) -> list[re.Pattern]:
    if path is None or not path.exists():
        return []
    patterns = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            patterns.append(re.compile(line.encode("utf-8"), re.I))
    return patterns


def repo_files(repo: Path) -> list[tuple[str, callable]]:
    """The index of a repository root, or every file of a plain directory."""
    names = None
    if (repo / ".git").exists():
        try:
            out = subprocess.check_output(["git", "-C", str(repo), "ls-files", "-z"],
                                          stderr=subprocess.DEVNULL)
            names = [n for n in out.decode("utf-8").split("\0") if n]
        except (subprocess.CalledProcessError, FileNotFoundError):
            names = None
    if names is None:
        names = [p.relative_to(repo).as_posix() for p in repo.rglob("*")
                 if p.is_file() and ".git" not in p.relative_to(repo).parts]
    return [(n, (lambda p=repo / n: p.read_bytes() if p.exists() else b"")) for n in names]


def zip_files(archive: Path) -> list[tuple[str, callable]]:
    zf = zipfile.ZipFile(archive)
    return [(i.filename, (lambda i=i: zf.read(i))) for i in zf.infolist() if not i.is_dir()]


def agent_skill(parts: list[str], suffix: str) -> bool:
    """A project skill (.claude/skills/<name>/*.md at the root): written
    instructions for coding agents, like any other document. Everything else
    under .claude (settings, sessions, worktrees) stays forbidden."""
    return (len(parts) >= 4 and parts[0] == ".claude" and parts[1] == "skills"
            and suffix == ".md" and not any(p in FORBIDDEN_DIRS for p in parts[2:-1]))


def audit(entries, allow: Allow, denylist, rom_index: RomIndex | None,
          context: str) -> list[Finding]:
    findings: list[Finding] = []
    third_party_dirs: set[str] = set()
    names = {name for name, _ in entries}

    for name, read in entries:
        path = PurePosixPath(name)
        if allow.matches(allow.skip, name):
            continue
        parts = [p.lower() for p in path.parts]
        suffix = path.suffix.lower()

        if suffix in FORBIDDEN_SUFFIXES:
            findings.append(Finding(name, "forbidden-type", suffix))
        if any(p in FORBIDDEN_DIRS for p in parts[:-1]) and not agent_skill(parts, suffix):
            findings.append(Finding(name, "forbidden-dir"))
        if any(rx.match(path.name) for rx in FORBIDDEN_NAMES):
            findings.append(Finding(name, "forbidden-name"))
        if read()[:4] in FORBIDDEN_MAGIC:
            findings.append(Finding(name, "follower-sprite"))
        if "romfs" in parts[:-1] and path.name not in ROMFS_ALLOWED:
            findings.append(Finding(name, "generated-romfs"))
        if "third_party" in parts[:-1]:
            idx = parts.index("third_party")
            if idx + 1 < len(parts) - 1:
                third_party_dirs.add("/".join(path.parts[:idx + 2]))

        data = read()
        # Unmodified third-party runtime (the bundled Python, Tk, Pillow): its
        # size, its strings and its generic constant tables are not ours to
        # judge; the checks for a ROM, a data pack or a secret still apply.
        runtime = allow.matches(allow.runtime, name)
        if len(data) > MAX_BYTES and not allow.matches(allow.large, name) and not runtime:
            findings.append(Finding(name, "too-large", "%d bytes" % len(data)))
        if suffix in MEDIA_SUFFIXES and context == "repo" and not allow.matches(allow.media, name):
            findings.append(Finding(name, "media-without-provenance"))

        if len(data) >= 0xC0 and data[4:4 + len(GBA_LOGO_PREFIX)] == GBA_LOGO_PREFIX:
            findings.append(Finding(name, "gba-rom-header"))
        if data[:8] == PAK_MAGIC:
            findings.append(Finding(name, "data-pack"))
        if len(data) in (8 << 20, 16 << 20, 32 << 20):
            if hashlib.sha1(data).hexdigest() in KNOWN_ROM_SHA1:
                findings.append(Finding(name, "known-rom"))

        textual = suffix in TEXT_SUFFIXES or context == "repo"
        if textual and name not in allow.local_paths:
            m = LOCAL_PATH.search(data)
            if m:
                findings.append(Finding(name, "local-path", m.group(0)[:60].decode("latin-1")))
        for rx, label in SECRETS:
            if rx.search(data):
                findings.append(Finding(name, "secret", label))
        for rx in ([] if runtime else denylist):
            if rx.search(data) or rx.search(name.encode("utf-8")):
                findings.append(Finding(name, "denylist", "(local pattern)"))
                break
        if data[:8] == b"EM3DRCP1":
            findings += audit_programs(name, data)
        if rom_index is not None:
            scanned = data
            if data[:8] == b"EM3DRCP1":
                # A recipe: scan what it would reveal, not its LZMA stream.
                import lzma
                size = int.from_bytes(data[8:12], "little")
                scanned = lzma.decompress(data[12:12 + size]) + lzma.decompress(data[12 + size:])
            copied = rom_index.copied_bytes(scanned)
            if copied > ROM_TOLERANCE and not runtime:
                findings.append(Finding(name, "rom-content", "%d bytes match the ROM" % copied))
            elif copied:
                print("release_audit: note: %s shares %d incidental bytes with the ROM" % (name, copied))

    for tp in sorted(third_party_dirs):
        has_license = any(n.startswith(tp + "/") and PurePosixPath(n).name.upper().startswith(("LICENSE", "COPYING"))
                          for n in names)
        if not has_license:
            findings.append(Finding(tp, "third-party-without-license"))
    return findings


def audit_programs(name: str, data: bytes) -> list[Finding]:
    """A recipe's precompiled voxel files (builder/emerald3ds_builder/
    precompiled.py): their texels must all be references into the ROM. A
    texel written literally is a picture the release would carry."""
    sys.path.insert(0, str(HERE.parent / "builder"))
    from emerald3ds_builder import precompiled
    from emerald3ds_builder.recipe import Recipe
    from emerald3ds_builder.recipe import RecipeError
    found = []
    try:
        recipe = Recipe.from_bytes(data)
    except RecipeError as exc:
        return [Finding(name, "precompiled", "unreadable recipe: %s" % exc)]
    for entry in recipe.generated:
        for op in entry.get("ops", ()):
            if op[0] != "V" or len(op) != 3:
                found.append(Finding(name, "precompiled", "%s: operation %r" % (entry["path"], op[0])))
                continue
            try:
                info = precompiled.summary(recipe.literals[op[1]:op[1] + op[2]])
            except precompiled.ProgramError as exc:
                found.append(Finding(name, "precompiled", "%s: %s" % (entry["path"], exc)))
                continue
            if info["own texels"]:
                found.append(Finding(name, "precompiled-texels", "%s: %d texels written literally"
                                     % (entry["path"], info["own texels"])))
            stray = sorted(info["own glass values"] - precompiled.OWN_GLASS)
            if stray:
                found.append(Finding(name, "precompiled-texels", "%s: window glass written literally "
                                     "(levels %s)" % (entry["path"], stray[:8])))
            print("release_audit: note: %s %s: %d own bytes, %d texels and %d pixels from the ROM"
                  % (name, entry["path"], info["literal"] + info["zero"] + info["copy"],
                     info["tile texels"] + info["palette texels"] + info["colours"] + info["glass texels"],
                     info["pixels"]))
    return found


WEB_PAYLOAD_DIRS = ("payload/", "python/", "licenses/")


def audit_web_payload(entries) -> list[Finding]:
    """The release contract of Emerald3DS-WebPayload.zip (see
    builder/emerald3ds_builder/webmanifest.py), on top of the generic checks."""
    import json
    sys.path.insert(0, str(HERE.parent / "builder"))
    from emerald3ds_builder.webmanifest import MANIFEST_NAME, ManifestError, validate

    findings: list[Finding] = []
    reads = dict(entries)
    if MANIFEST_NAME not in reads:
        return [Finding(MANIFEST_NAME, "web-manifest-missing")]
    try:
        manifest = validate(json.loads(reads[MANIFEST_NAME]()))
    except (ValueError, ManifestError) as exc:
        return [Finding(MANIFEST_NAME, "web-manifest-invalid", str(exc))]
    listed = {item["path"]: item for item in manifest["files"]}
    for name in reads:
        if name == MANIFEST_NAME:
            continue
        if not name.startswith(WEB_PAYLOAD_DIRS):
            findings.append(Finding(name, "web-payload-unexpected-file"))
        if name not in listed:
            findings.append(Finding(name, "web-payload-unlisted-file"))
    for path, item in listed.items():
        if path not in reads:
            findings.append(Finding(path, "web-payload-missing-file"))
            continue
        data = reads[path]()
        if len(data) != item["size"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
            findings.append(Finding(path, "web-payload-hash-mismatch"))
    return findings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    target = ap.add_mutually_exclusive_group(required=True)
    target.add_argument("--repo", type=Path, help="repository or export directory")
    target.add_argument("--zip", type=Path, help="release archive")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any finding")
    ap.add_argument("--allow", type=Path, default=None, help="allowlist (TOML)")
    ap.add_argument("--denylist", type=Path, default=None, help="local denylist")
    ap.add_argument("--rom", type=Path, default=None,
                    help="also fail on any run of this ROM's bytes (local use only)")
    ap.add_argument("--web-payload", action="store_true",
                    help="the ZIP is Emerald3DS-WebPayload.zip: also check its manifest and hashes")
    args = ap.parse_args(argv)
    if args.web_payload and not args.zip:
        ap.error("--web-payload needs --zip")

    if args.repo:
        root = args.repo.resolve()
        allow_path = args.allow or (root / "tools" / "release_audit_allow.toml")
        if not allow_path.exists():
            allow_path = DEFAULT_ALLOW
        entries = repo_files(root)
        context = "repo"
        deny_path = args.denylist or next((p for p in (root / ".public-denylist",
                                                        HERE.parent / ".public-denylist")
                                           if p.exists()), None)
    else:
        allow_path = args.allow or DEFAULT_ALLOW
        entries = zip_files(args.zip)
        context = "zip"
        deny_path = args.denylist or (HERE.parent / ".public-denylist")

    allow = Allow.load(allow_path)
    denylist = load_denylist(deny_path)
    rom_index = None
    if args.rom:
        print("release_audit: indexing ROM windows...", flush=True)
        rom_index = RomIndex(args.rom.read_bytes())

    findings = audit(entries, allow, denylist, rom_index, context)
    if args.web_payload:
        findings += audit_web_payload(entries)
    label = str(args.repo or args.zip)
    print("release_audit: %d files checked in %s (%d denylist patterns%s)"
          % (len(entries), label, len(denylist), ", ROM scan" if rom_index else ""))
    for f in findings:
        print("  %-28s %s%s" % (f.rule, f.path, (" - " + f.detail) if f.detail else ""))
    if findings:
        print("release_audit: %d finding(s)" % len(findings))
        return 1 if args.strict else 0
    print("release_audit: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
