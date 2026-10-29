"""ROM -> data pack: the whole build, independent of any user interface."""

from __future__ import annotations

import shutil
import sys
import tempfile
import time
import zlib
from dataclasses import dataclass
from pathlib import Path

from . import followers, pak, precompiled
from .errors import BuilderError
from .recipe import Recipe, RecipeError, build_entry, recipe_rom_sha1
from .rom import Rom, load_rom
from .voxel import LABELS, STEPS, run_generators
from .vtree import build_tree

RECIPE_NAME = "emerald3ds.recipe"
EXECUTABLE_NAMES = ("Emerald3DS.3dsx", "Emerald3DS.smdh")
DEFAULT_LANGUAGE = "en"       # the default variant's (payload/ itself)
# Each game language in itself (the window's choice) and in English (messages).
LANGUAGE_NAMES = {"en": "English", "es": "Español", "fr": "Français", "it": "Italiano", "de": "Deutsch",
                  "ja": "日本語", "pt": "Português (Brasil)"}
LANGUAGE_NAMES_EN = {"en": "English", "es": "Spanish", "fr": "French", "it": "Italian", "de": "German",
                     "ja": "Japanese", "pt": "Brazilian Portuguese"}


def language_code(language: str) -> str:
    """"pt_BR", "pt-br" -> "pt": the language, without its region."""
    return language.strip().lower().replace("-", "_").split("_")[0]


def default_payload() -> Path:
    """payload/ next to the executable (release) or next to the package."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent / "payload"
    return Path(__file__).resolve().parent.parent / "payload"


@dataclass
class Payload:
    """A release's payload/ folder.

    payload/ holds the default variant (the English game: executable and
    recipe) and the voxel generators every variant shares. Each other
    language is a subfolder with its own executable and recipe, e.g.
    payload/es/ for the Spanish ROM; `for_rom` picks the one made from the
    player's ROM."""
    root: Path
    shared: Path | None = None   # the payload/ folder, for a language subfolder

    @property
    def recipe(self) -> Path:
        return self.root / RECIPE_NAME

    @property
    def voxelgen(self) -> Path:
        return (self.shared or self.root) / "voxelgen"

    def variants(self) -> list["Payload"]:
        """The default variant first, then every language subfolder."""
        base = self.shared or self.root
        if not base.is_dir():
            return [Payload(base)]
        subs = sorted(p for p in base.iterdir() if p.is_dir() and (p / RECIPE_NAME).is_file())
        return [Payload(base)] + [Payload(p, base) for p in subs]

    def for_rom(self, sha1: str, language: str | None = None) -> "Payload":
        """The variant whose recipe was made from this ROM (self when none is).

        One ROM can make more than one language: the English ROM makes the
        English game and the Portuguese fan translation (payload/pt/). With
        `language` ("pt", "pt_BR") only that language's variant matches;
        without, the first does, so the default variant (the English game)
        before the subfolders."""
        matches = self.matching(sha1)
        if language:
            want = language_code(language)
            matches = [v for v in matches if v.game_language == want]
        return matches[0] if matches else self

    def matching(self, sha1: str) -> list["Payload"]:
        """Every variant whose recipe was made from this ROM, default first."""
        out = []
        for variant in self.variants():
            try:
                if variant.recipe.is_file() and recipe_rom_sha1(variant.recipe) == sha1:
                    out.append(variant)
            except (OSError, ValueError, KeyError, RecipeError):
                continue
        return out

    def languages(self, sha1: str) -> list[str]:
        """The game languages this ROM makes ("en", "pt"), default first."""
        return [v.game_language for v in self.matching(sha1)]

    def for_pak(self, sha1: str, abi: int) -> "Payload":
        """The variant a pack was built as: its ROM, and its data ABI when two
        languages share that ROM (self when none matches)."""
        matches = self.matching(sha1)
        for variant in matches:
            try:
                if Recipe.load(variant.recipe).engine_abi == abi:
                    return variant
            except (OSError, RecipeError):
                continue
        return matches[0] if matches else self

    @property
    def game_language(self) -> str:
        """The game language this variant builds: its subfolder's name, or
        English for the default variant."""
        return language_code(self.language or DEFAULT_LANGUAGE)

    @property
    def language(self) -> str | None:
        """The language subfolder's name (e.g. "es"); None for the default variant."""
        return None if self.shared is None else self.root.name

    def executables(self) -> list[Path]:
        return [self.root / name for name in EXECUTABLE_NAMES]

    def check(self, need_executables: bool = True) -> None:
        missing = [p for p in ([self.recipe] + (self.executables() if need_executables else []))
                   if not p.exists()]
        if not missing:
            return
        if self.language is None:
            raise BuilderError("This release is incomplete (%s missing next to the builder)."
                               % ", ".join(p.name for p in missing),
                               "Extract the whole ZIP again and run the builder from there.",
                               code="payload_incomplete")
        # Name the files under payload/ so the player can see which language is short.
        raise BuilderError("This release is incomplete for the '%s' language of your ROM "
                           "(%s missing from %s next to the builder)."
                           % (self.language, ", ".join("%s/%s" % (self.language, p.name) for p in missing),
                              self.shared.name),
                           "Extract the whole ZIP again, including the %s/%s folder, and run the "
                           "builder from there." % (self.shared.name, self.language),
                           code="payload_incomplete")


def select_payload(payload: Payload, rom_sha1: str, need_executables: bool = False,
                   language: str | None = None) -> Payload:
    """The variant made from this ROM, checked to hold what the caller will use.

    `language` picks among the variants of one ROM (Payload.for_rom); when
    the release has none in that language for this ROM, nothing is built
    ("rom_mismatch"): another language never stands in for it.

    With `need_executables` (the desktop build and install, which copy the
    executables next to the pack) a variant without its own executables fails
    here with "payload_incomplete", before any slow work and before anything
    is written: the default variant's executables never stand in for it. When
    no variant matches the ROM nothing is checked, so the build still reports
    "rom_mismatch"."""
    if language and payload.matching(rom_sha1) and language_code(language) not in payload.languages(rom_sha1):
        name = LANGUAGE_NAMES_EN.get(language_code(language), repr(language))
        raise BuilderError("This release has no %s version made from this ROM." % name,
                           "It builds %s from it." % " and ".join(LANGUAGE_NAMES_EN.get(code, code)
                                                                  for code in payload.languages(rom_sha1)),
                           code="rom_mismatch")
    variant = payload.for_rom(rom_sha1, language)
    if need_executables:
        try:
            matches = variant.recipe.is_file() and recipe_rom_sha1(variant.recipe) == rom_sha1
        except (OSError, ValueError, KeyError, RecipeError):
            matches = False   # Recipe.load in build_pack reports it as unreadable
        if matches:
            variant.check(need_executables=True)
    return variant


class Progress:
    """Maps sub-task progress onto one bar: callback(fraction, message)."""

    def __init__(self, callback=None):
        self.callback = callback

    def __call__(self, fraction: float, message: str = "") -> None:
        if self.callback:
            self.callback(max(0.0, min(1.0, fraction)), message)

    def span(self, start: float, end: float, message: str):
        """A sub-task's own 0..1, optionally with a detail: "message: detail"."""
        return lambda f, detail=None: self(start + (end - start) * f,
                                           message if detail is None else "%s: %s" % (message, detail))


def input_sources(recipe: Recipe, rom: bytes) -> precompiled.Sources:
    """The recipe's voxel inputs, rebuilt from the ROM when a program reads them."""
    by_path = {entry["path"]: entry for entry in recipe.inputs}

    def read(path: str) -> bytes | None:
        entry = by_path.get(path)
        return None if entry is None else build_entry(entry, rom, recipe.literals, recipe.bitmaps)
    return precompiled.Sources(read)


def build_generated(recipe: Recipe, rom: bytes, progress=None, timings: list | None = None,
                    steps: int | None = None) -> dict[str, bytes]:
    """The generated files the release precompiled ("ops" in the recipe), from
    their programs: the generators' work done, only the ROM's tiles and
    palettes left to read. Each is checked against its CRC, as the
    generators' outputs are. Files without a program are not in the result.
    `steps` is the count the progress numbers them out of (all the voxel
    files, when the generators make the rest)."""
    entries = [e for e in recipe.generated if e.get("ops")]
    sources = input_sources(recipe, rom)
    # in the generators' order, labelled as they are
    order = {step[2].split("/romfs/", 1)[-1]: (i, step[0]) for i, step in enumerate(STEPS)}
    entries.sort(key=lambda e: order.get(e["path"], (len(STEPS), e["path"]))[0])
    out: dict[str, bytes] = {}
    for i, entry in enumerate(entries):
        script = order.get(entry["path"], (0, entry["path"]))[1]
        label = "%s (%d/%d)" % (LABELS.get(script, entry["path"]), i + 1, steps or len(entries))
        if progress:
            progress(i / len(entries), label)
        start = time.perf_counter()
        try:
            out[entry["path"]] = build_entry(entry, rom, recipe.literals, recipe.bitmaps, sources)
        except RecipeError as exc:
            raise BuilderError("A voxel data file could not be rebuilt from the ROM.", str(exc),
                               code="generator_output_mismatch") from exc
        took = time.perf_counter() - start
        if timings is not None:
            timings.append((script, took))
        if progress:
            progress((i + 1) / len(entries), "%s, done in %.1f s" % (label, took))
    return out


def build_pack(rom_path: Path, payload: Payload, out_pak: Path, progress=None,
               keep_workdir: bool = False, runner: str = "subprocess", rom: Rom | None = None,
               follower_fetcher: "followers.Fetcher | None | bool" = True,
               need_executables: bool = False, precompiled_ok: bool = True,
               language: str | None = None) -> dict:
    """Generate emerald3ds.pak from the ROM. Returns a summary.

    `runner` picks how the voxel generators run (see voxel.py); `rom` lets a
    caller that already holds the checked ROM in memory skip reading it again.
    The summary's `timings` holds (step, seconds) for the slow parts.

    `language` picks the game language when the ROM makes more than one
    (the English ROM: English, or Portuguese from payload/pt/); without it the
    ROM's own language is built.

    `precompiled_ok=False` runs the generators even for the files the
    release precompiled (a check of the programs against the generators).

    `need_executables` makes the chosen language variant's own executables
    required (see `select_payload`), checked before extraction and before
    `out_pak` is written. The web builder leaves it off: it only needs the
    recipe, and gets the executables from its manifest's assets.

    `follower_fetcher` gets the optional follower sprites (followers.py): True
    for the default one (local copies, the cache, the network), False for
    none, or a Fetcher. They go into the pack but not into its ABI; what
    cannot be had is left out and the build goes on."""
    report = Progress(progress)
    timings: list[tuple[str, float]] = []
    payload.check(need_executables=False)
    report(0.0, "Checking the ROM")
    if rom is None:
        rom = load_rom(rom_path)
    payload = select_payload(payload, rom.sha1, need_executables, language)
    try:
        recipe = Recipe.load(payload.recipe)
    except (OSError, RecipeError) as exc:
        raise BuilderError("The release's recipe could not be read.", str(exc),
                           code="recipe_unreadable") from exc
    if recipe.rom_sha1 != rom.sha1:
        raise BuilderError("This release has no game data for %s (%s)." % (rom.title, rom.code),
                           "Each language needs its own executable and recipe in the release; "
                           "this one does not include them for that ROM.",
                           code="rom_mismatch")

    files: dict[str, bytes] = {}
    start = time.perf_counter()
    total = len(recipe.entries)
    for i, entry in enumerate(recipe.entries):
        try:
            files[entry["path"]] = build_entry(entry, rom.data, recipe.literals, recipe.bitmaps)
        except RecipeError as exc:
            raise BuilderError("A game data file could not be rebuilt from the ROM.", str(exc),
                               code="data_rebuild_failed") from exc
        if i % 100 == 0:
            report(0.05 + 0.45 * i / max(total, 1), "Extracting game data")
    timings.append(("extract", time.perf_counter() - start))

    # The voxel data: from the release's programs where it precompiled them
    # (no generator runs), from the generators for the rest.
    ready: dict[str, bytes] = {}
    programs = [e for e in recipe.generated if precompiled_ok and e.get("ops")]
    rest = [e for e in recipe.generated if e not in programs]
    if recipe.generated:
        report(0.5, "Preparing the 3D scenery inputs")
    # the bar's 0.6-0.9 shared by file: the programs first, then the generators
    split = 0.6 + 0.3 * len(programs) / max(1, len(recipe.generated))
    if programs:
        ready = build_generated(recipe, rom.data, report.span(0.6, split, "Generating the 3D scenery"),
                                timings, steps=len(recipe.generated) if rest else None)
        files.update(ready)
    if rest:
        work = Path(tempfile.mkdtemp(prefix="emerald3ds-"))
        try:
            start = time.perf_counter()
            # as before when nothing is precompiled: the tree 0.5-0.6, the generators 0.6-0.9
            tree_end = split + (0.9 - split) / 6 if programs else 0.6
            build_tree(rom.data, recipe, work, report.span(0.5 if not programs else split, tree_end,
                                                           "Preparing the 3D scenery inputs"))
            timings.append(("tree", time.perf_counter() - start))
            outputs = run_generators(work, payload.voxelgen, rest,
                                     report.span(tree_end, 0.9, "Generating the 3D scenery"),
                                     runner=runner, timings=timings, ready=ready,
                                     numbering=(len(programs), len(recipe.generated)) if programs else None)
            files.update(outputs)
        finally:
            if not keep_workdir:
                shutil.rmtree(work, ignore_errors=True)

    items = [(p, len(d), zlib.crc32(d) & 0xFFFFFFFF) for p, d in files.items()]
    abi = pak.engine_abi(items)
    if abi != recipe.engine_abi:
        raise BuilderError("The generated data does not match this release (ABI %08x, expected %08x)."
                           % (abi, recipe.engine_abi), code="abi_mismatch")

    optional: dict[str, bytes] = {}
    follower_info = None
    if follower_fetcher is True:
        follower_fetcher = followers.default_fetcher()
    if follower_fetcher:
        report(0.9, "Fetching the follower sprites")
        start = time.perf_counter()
        optional, follower_info = followers.build_section(
            follower_fetcher, report.span(0.9, 0.92, "Fetching the follower sprites"),
            languages=[followers.ROM_LANGUAGES.get(rom.code[3:4], "en")])
        timings.append(("followers", time.perf_counter() - start))
    report(0.92, "Writing the data pack")
    info = pak.write_pak(out_pak, sorted({**files, **optional}.items()), abi, bytes.fromhex(rom.sha1))
    report(0.96, "Verifying the data pack")
    with pak.PakReader(out_pak) as reader:
        reader.verify()
    report(1.0, "Done")
    return {"entries": info["entries"], "bytes": info["bytes"], "abi": abi, "release": recipe.release,
            "rom": rom.code, "timings": [(name, round(sec, 2)) for name, sec in timings],
            "followers": follower_info, "payload": payload}
