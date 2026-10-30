"""Run real startup validation with synthetic packs, no SDK or ROM.

3ds_data.c is copied verbatim into a temporary mirror (its relative constants
include normally comes from the bootstrapped game). Only SDK/platform edges
and the unused GNU cookie-stream API on macOS are stubbed. Real stdio/stat,
CtrData_Init, error getters, and 3ds_pak.c run in a fresh process per case.
The START check compiles only the actual production printf expression, not
its compositor, input loop, or hardware console rendering.
"""
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CC = shutil.which("cc") or shutil.which("gcc")
LANGUAGES = {"en": (2, 0), "es": (7, 0), "fr": (3, 0), "it": (4, 0), "pt": (2, 1),
             "de": (5, 0), "ja": (1, 0)}
# Public compatibility identifiers, not cartridge content.
ROM_SHA1 = {
    "en": "f3ae088181bf583e55daf962a92bb46f4f1d07b7",
    "es": "fe1558a3dcb0360ab558969e09b690888b846dd9",
    "fr": "ca666651374d89ca439007bed54d839eb7bd14d0",
    "it": "1692db322400c3141c5de2db38469913ceb1f4d4",
    "de": "61c2eb2b380b1a75f0c94b767a2d4c26cd7ce4e3",
    "ja": "d7cf8f156ba9c455d164e1ea780a6bf1945465c2",
}
ROM_SHA1["pt"] = ROM_SHA1["en"]
ENGINE_ABI = 0x12ABCDEF
PACK_ABI = 0x89ABCDEF
HEADER = struct.Struct("<8sII20sIQQII")
ENTRY = struct.Struct("<QIIQIIII")
CONSTANTS = ("#define LANGUAGE_JAPANESE 1\n#define LANGUAGE_ENGLISH 2\n#define LANGUAGE_FRENCH 3\n"
             "#define LANGUAGE_ITALIAN 4\n#define LANGUAGE_GERMAN 5\n#define LANGUAGE_SPANISH 7\n")

# Pin the non-French, non-Italian output byte for byte: English, Spanish,
# Portuguese, German and Japanese must not move because of this change.
# Spanish deliberately keeps the English startup errors, Japanese has no
# port text of its own, and German keeps the texts it already had.
GOLDEN = {
    "missing": {
        "en": ('The game data pack was not found.', 'Run the builder that came with this\nrelease with your own Pokemon Emerald\nROM and install the data pack to:\n\n  /3ds/emerald3ds/emerald3ds.pak'),
        "es": ('The game data pack was not found.', 'Run the builder that came with this\nrelease with your own Pokemon Emerald\nROM and install the data pack to:\n\n  /3ds/emerald3ds/emerald3ds.pak'),
        "pt": ('O pacote de dados nao foi encontrado.', 'Rode o builder que veio com esta\nversao com a sua ROM de Pokemon\nEmerald e instale o pacote em:\n\n  /3ds/emerald3ds/emerald3ds.pak'),
        "de": ('Das Datenpaket wurde nicht gefunden.', 'Starte den Builder dieser Version\nmit deinem eigenen Smaragd-ROM und\ninstalliere das Datenpaket nach:\n\n  /3ds/emerald3ds/emerald3ds.pak'),
        "ja": ('The game data pack was not found.', 'Run the builder that came with this\nrelease with your own Pokemon Emerald\nROM and install the data pack to:\n\n  /3ds/emerald3ds/emerald3ds.pak'),
    },
    "magic": {
        "en": ('This is not a game data pack.', '/3ds/emerald3ds/emerald3ds.pak is not a\ndata pack. Run the builder again.'),
        "es": ('This is not a game data pack.', '/3ds/emerald3ds/emerald3ds.pak is not a\ndata pack. Run the builder again.'),
        "pt": ('Isto nao e um pacote de dados.', '/3ds/emerald3ds/emerald3ds.pak nao e um\npacote de dados. Rode o builder de novo.'),
        "de": ('Das ist kein Datenpaket des Spiels.', '/3ds/emerald3ds/emerald3ds.pak ist kein\nDatenpaket. Starte den Builder erneut.'),
        "ja": ('This is not a game data pack.', '/3ds/emerald3ds/emerald3ds.pak is not a\ndata pack. Run the builder again.'),
    },
    "mismatch": {
        "en": ('The data pack does not match this release.', 'It was generated for a different\nrelease. Run the builder that came\nwith this release again.\n\nEngine ABI: %08lX (schema %u)\nPack ABI:   %08lX (schema %lu)'),
        "es": ('The data pack does not match this release.', 'It was generated for a different\nrelease. Run the builder that came\nwith this release again.\n\nEngine ABI: %08lX (schema %u)\nPack ABI:   %08lX (schema %lu)'),
        "pt": ('O pacote de dados nao e desta versao.', 'Ele foi gerado para outra versao.\nRode de novo o builder que veio\ncom esta versao.\n\nABI do motor:  %08lX (schema %u)\nABI do pacote: %08lX (schema %lu)'),
        "de": ('Datenpaket passt nicht zu dieser Version.', 'Es wurde fuer eine andere Version\nerstellt. Starte den Builder dieser\nVersion erneut.\n\nEngine-ABI: %08lX (Schema %u)\nPaket-ABI:  %08lX (Schema %lu)'),
        "ja": ('The data pack does not match this release.', 'It was generated for a different\nrelease. Run the builder that came\nwith this release again.\n\nEngine ABI: %08lX (schema %u)\nPack ABI:   %08lX (schema %lu)'),
    },
    "rom": {
        "en": ('The data pack comes from another ROM.', 'Only Pokemon Emerald (USA, Europe) is\nsupported. Run the builder with that ROM.'),
        "es": ('The data pack comes from another ROM.', 'Usa la ROM de Pokemon Esmeralda\n(Espana, BPES) con este cliente.'),
        "pt": ('O pacote de dados veio de outra ROM.', 'So a ROM de Pokemon Emerald (USA,\nEurope) e aceita. Rode o builder com ela.'),
        "de": ('Datenpaket stammt von einem anderen ROM.', 'Nutze das ROM von Pokemon Smaragd\n(Deutschland, BPED) mit diesem Client.'),
        "ja": ('The data pack comes from another ROM.', 'Use the Pocket Monsters Emerald\n(Japan, BPEJ) ROM with this client.'),
    },
    "damaged": {
        "en": ('The data pack is damaged.', 'It does not pass the integrity check.\nRun the builder again.'),
        "es": ('The data pack is damaged.', 'It does not pass the integrity check.\nRun the builder again.'),
        "pt": ('O pacote de dados esta corrompido.', 'Ele nao passa na verificacao de\nintegridade. Rode o builder de novo.'),
        "de": ('Das Datenpaket ist beschaedigt.', 'Die Integritaetspruefung schlug fehl.\nStarte den Builder erneut.'),
        "ja": ('The data pack is damaged.', 'It does not pass the integrity check.\nRun the builder again.'),
    },
    "index": {
        "en": ('The data pack is damaged.', 'Its index does not pass the integrity\ncheck. Run the builder again.'),
        "es": ('The data pack is damaged.', 'Its index does not pass the integrity\ncheck. Run the builder again.'),
        "pt": ('O pacote de dados esta corrompido.', 'O indice nao passa na verificacao de\nintegridade. Rode o builder de novo.'),
        "de": ('Das Datenpaket ist beschaedigt.', 'Die Integritaetspruefung des Index\nschlug fehl. Starte den Builder erneut.'),
        "ja": ('The data pack is damaged.', 'Its index does not pass the integrity\ncheck. Run the builder again.'),
    },
    "truncated_index": {
        "en": ('The data pack could not be read.', 'The SD card could not deliver its index.'),
        "es": ('The data pack could not be read.', 'The SD card could not deliver its index.'),
        "pt": ('Nao foi possivel ler o pacote de dados.', 'O cartao SD nao entregou o seu indice.'),
        "de": ('Das Datenpaket konnte nicht gelesen werden.', 'Die SD-Karte lieferte kein Verzeichnis.'),
        "ja": ('The data pack could not be read.', 'The SD card could not deliver its index.'),
    },
    "embedded": {
        "en": ('This build has no embedded game data.', 'Install a data pack or loose data files.'),
        "es": ('This build has no embedded game data.', 'Install a data pack or loose data files.'),
        "pt": ('Esta versao nao traz dados embutidos.', 'Instale um pacote ou os dados soltos.'),
        "de": ('Diese Version enthaelt keine Spieldaten.', 'Datenpaket oder lose Dateien installieren.'),
        "ja": ('This build has no embedded game data.', 'Install a data pack or loose data files.'),
    },
}

CASES = {
    "missing": "missing", "bad_magic": "magic", "short_header": "magic",
    "bad_schema": "mismatch", "bad_abi": "mismatch", "bad_rom": "rom",
    "header_crc": "damaged", "bad_count": "damaged", "index_crc": "index",
    "invalid_index": "index", "truncated_index": "truncated_index",
    "no_embedded": "embedded",
}

SDK_STUB = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
typedef int LightLock;
static inline void LightLock_Init(LightLock *lock) { *lock = 0; }
static inline void LightLock_Lock(LightLock *lock) { (void)lock; }
static inline void LightLock_Unlock(LightLock *lock) { (void)lock; }
static inline void svcSleepThread(int64_t ns) { (void)ns; abort(); }
#ifdef __APPLE__
/* Unused by startup; fail loudly if a test ever reaches a data stream. */
typedef struct {
    ssize_t (*read)(void *, char *, size_t);
    ssize_t (*write)(void *, const char *, size_t);
    int (*seek)(void *, off_t *, int);
    int (*close)(void *);
} cookie_io_functions_t;
static inline FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io)
{ (void)cookie; (void)mode; (void)io; abort(); }
#endif
'''

HARNESS = r'''
/* 3ds_data.c sets _GNU_SOURCE itself, but the port builds it as its own
 * translation unit: here the system headers are already included, so glibc
 * would hide fopencookie's declaration and gcc rejects the call. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static const char *abiFormat = "";
static int checked_snprintf(char *dst, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static int checked_snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(dst, size, fmt, args);
    va_end(args);
    /* Check pre-truncation length, not just strlen of a clipped getter. */
    assert(length >= 0 && (size_t)length < size);
    if (strcmp(fmt, "%s") != 0)
        abiFormat = fmt;
    return length;
}
#undef snprintf
#define snprintf checked_snprintf
#include "3ds_port/src/3ds_data.c"
#undef snprintf

FILE *CtrFs_OpenAsset(const char *path)
{
    assert(strcmp(path, "engine/abi.bin") == 0);
    return fopen("engine-abi.bin", "rb");
}
void CtrLog_Write(CtrLogCategory category, const char *format, ...)
{ (void)category; (void)format; }
bool CtrPlatform_StartThread(void (*entry)(void *), void *arg, unsigned stack, int core)
{ (void)entry; (void)arg; (void)stack; (void)core; abort(); }

static void emit(const char *text)
{ fwrite(text, 1, strlen(text) + 1, stdout); }
int main(void)
{
    assert(sizeof(sErrorTitle) == 96 && sizeof(sErrorDetail) == 512);
    bool ok = CtrData_Init();
    emit(ok ? "ok" : "error");
    emit(CtrData_BackendName());
    emit(CtrData_ErrorTitle());
    emit(CtrData_ErrorDetail());
    emit(abiFormat);
    return 0;
}
'''


def synthetic_pack(language, case):
    """Only invented metadata/one-byte payload; checksums match each fault."""
    schema = 0xFFFFFFFF if case == "bad_schema" else 1
    abi = PACK_ABI if case == "bad_abi" else ENGINE_ABI
    sha1 = bytes(20) if case == "bad_rom" else bytes.fromhex(ROM_SHA1[language])
    # A compressed flag with a valid CRC exercises structural index rejection.
    index = ENTRY.pack(1, 0, int(case == "invalid_index"), 128, 1, 1,
                       zlib.crc32(b"x"), 0)
    index_crc = zlib.crc32(index) ^ int(case == "index_crc")
    header = HEADER.pack(b"EM3DPAK\0", schema, abi, sha1,
                         0 if case == "bad_count" else 1, 64, 128, index_crc, 0)
    header = header[:60] + struct.pack("<I", zlib.crc32(header[:60]))
    if case == "bad_magic":
        header = b"NOTAPAK!" + header[8:]
    if case == "header_crc":
        header = header[:60] + bytes(4)
    if case == "short_header":
        return header[:12]
    if case == "truncated_index":
        return header + index[:-1]
    return header + index + bytes(24) + b"x"


def compile_c(source, executable, root, language, backend=0, extra=()):
    assert CC is not None
    game_language, portuguese = LANGUAGES[language]
    subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(root), "-I" + str(ROOT / "3ds_port/include"),
                    "-DGAME_LANGUAGE=" + str(game_language),
                    "-DCTR_LANG_PT_BR=" + str(portuguese),
                    "-DCTR_DATA_BACKEND=" + str(backend), str(source),
                    *map(str, extra), "-o", str(executable)], check=True)


@unittest.skipUnless(CC, "requires a host C compiler")
class StartupLocaleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="startup-locale-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        constants = cls.root / "include/constants"
        constants.mkdir(parents=True)
        (constants / "global.h").write_text(CONSTANTS, encoding="utf-8")
        (cls.root / "constants").mkdir()
        (cls.root / "constants/global.h").write_text(CONSTANTS, encoding="utf-8")
        source_dir = cls.root / "3ds_port/src"
        source_dir.mkdir(parents=True)
        shutil.copyfile(ROOT / "3ds_port/src/3ds_data.c", source_dir / "3ds_data.c")
        (cls.root / "3ds.h").write_text(SDK_STUB, encoding="utf-8")
        source = cls.root / "startup.c"
        source.write_text(HARNESS, encoding="utf-8")
        cls.executables = {}
        for language in LANGUAGES:
            for backend in (0, 1):
                executable = cls.root / (language + str(backend))
                compile_c(source, executable, cls.root, language, backend,
                          [ROOT / "3ds_port/src/3ds_pak.c"])
                cls.executables[language, backend] = executable
        cls.output = {}
        for language in LANGUAGES:
            for case in (*CASES, "valid"):
                cwd = cls.root / (language + "-" + case)
                (cwd / "sdmc:/3ds/emerald3ds").mkdir(parents=True)
                (cwd / "engine-abi.bin").write_bytes(struct.pack("<I", ENGINE_ABI))
                if case not in ("missing", "no_embedded"):
                    (cwd / "sdmc:/3ds/emerald3ds/emerald3ds.pak").write_bytes(
                        synthetic_pack(language, case))
                result = subprocess.run(
                    [str(cls.executables[language, int(case == "no_embedded")])],
                    cwd=cwd, check=True, capture_output=True)
                fields = result.stdout.decode("ascii").split("\0")
                if len(fields) != 6 or fields[-1] != "":
                    raise AssertionError("invalid harness output: " + repr(result.stdout))
                cls.output[language, case] = fields[:-1]

    def test_real_init_rejects_faults_and_accepts_control(self):
        for language in LANGUAGES:
            for case in CASES:
                with self.subTest(language=language, case=case):
                    status, backend, title, detail, _ = self.output[language, case]
                    self.assertEqual((status, backend), ("error", "none"))
                    self.assertTrue(title)
                    self.assertTrue(detail)
            self.assertEqual(self.output[language, "valid"], ["ok", "pak", "", "", ""])

    def test_french_and_italian_titles_and_details_are_localized(self):
        for language in ("fr", "it"):
            for case in CASES:
                for field, name in ((2, "title"), (3, "detail")):
                    with self.subTest(language=language, case=case, field=name):
                        text = self.output[language, case][field]
                        self.assertNotEqual(text, self.output["en", case][field],
                                            name + " still falls back to English")
                        self.assertNotEqual(text, self.output[
                            "it" if language == "fr" else "fr", case][field])
            with self.subTest(language=language, guidance="ROM edition"):
                self.assertIn("BPEF" if language == "fr" else "BPEI",
                              self.output[language, "bad_rom"][3])

    def test_preserved_languages_are_unchanged(self):
        for language in ("en", "es", "pt", "de", "ja"):
            for case, category in CASES.items():
                with self.subTest(language=language, case=case):
                    title, detail = GOLDEN[category][language]
                    if category == "mismatch":
                        detail %= (ENGINE_ABI, 1, PACK_ABI if case == "bad_abi" else ENGINE_ABI,
                                   0xFFFFFFFF if case == "bad_schema" else 1)
                    self.assertEqual(self.output[language, case][2:4], [title, detail])

    def test_format_specifiers_values_buffers_and_console_width(self):
        for language in LANGUAGES:
            for case in CASES:
                with self.subTest(language=language, case=case):
                    _, _, title, detail, fmt = self.output[language, case]
                    self.assertLess(len(title.encode("ascii")), 96)
                    self.assertLess(len(detail.encode("ascii")), 512)
                    for line in (title + "\n" + detail).splitlines():
                        self.assertLessEqual(len(line), 49, repr(line))
                    if case in ("bad_abi", "bad_schema"):
                        self.assertEqual(re.findall(r"%[^\s)]+", fmt),
                                         ["%08lX", "%u", "%08lX", "%lu"])
                        self.assertIn("12ABCDEF", detail)
                        if case == "bad_abi":
                            self.assertIn("89ABCDEF", detail)
                        else:
                            self.assertIn("4294967295", detail)
                        self.assertNotIn("%", detail)
                    else:
                        self.assertEqual(fmt, "")

    def test_production_start_printf(self):
        platform = (ROOT / "3ds_port/src/3ds_platform.c").read_text(encoding="utf-8")
        body = platform.split("void CtrPlatform_ShowDataError(", 1)[1].split(
            "while (aptMainLoop())", 1)[0]
        calls = [args for args in re.findall(r"printf\((.*?)\);", body, re.S)
                 if "START" in args]
        self.assertEqual(len(calls), 1, "expected one production START printf")
        source = self.root / "start.c"
        source.write_text('#include <stdio.h>\n#include "3ds_locale.h"\n'
                          'int main(void) { printf(' + calls[0] + '); return 0; }\n',
                          encoding="utf-8")
        for language in LANGUAGES:
            with self.subTest(language=language):
                executable = self.root / ("start-" + language)
                compile_c(source, executable, self.root, language)
                text = subprocess.run([str(executable)], check=True,
                                      capture_output=True).stdout.decode("ascii")
                if language == "fr":
                    self.assertRegex(text, r"Appuie.*START.*quitter")
                elif language == "it":
                    self.assertRegex(text, r"Premi.*START.*uscire")
                else:
                    self.assertEqual(text, "\n\n Press START to exit.")
                for line in text.splitlines():
                    self.assertLessEqual(len(line), 49)


if __name__ == "__main__":
    unittest.main()
