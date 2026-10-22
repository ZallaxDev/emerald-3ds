"""Compile the real data backend's ROM identity branches without the 3DS SDK."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder.rom import ROM_PROFILES

CC = shutil.which("cc") or shutil.which("gcc")


@unittest.skipUnless(CC, "requires a host C compiler")
class DataLocaleIdentityTests(unittest.TestCase):
    def test_compiled_identity_matches_each_builder_profile(self):
        assert CC is not None
        source = (ROOT / "3ds_port/src/3ds_data.c").read_text()
        identities = source[source.index("#if GAME_LANGUAGE == LANGUAGE_SPANISH"):
                            source.index("typedef struct")]
        program = ('#include <stdint.h>\n#include <stdio.h>\n#include "3ds_locale.h"\n'
                   + identities
                   + '\nint main(void) {\n'
                     'for (unsigned i = 0; i < sizeof(sSupportedRomSha1); ++i)\n'
                     '    printf("%02x", sSupportedRomSha1[i]);\n'
                     'printf("\\n%s\\n", SUPPORTED_ROM_DETAIL);\n'
                     'return 0;\n}\n')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "constants").mkdir()
            (root / "constants/global.h").write_text(
                "#define LANGUAGE_ENGLISH 2\n#define LANGUAGE_FRENCH 3\n"
                "#define LANGUAGE_ITALIAN 4\n#define LANGUAGE_GERMAN 5\n"
                "#define LANGUAGE_SPANISH 7\n")
            path = root / "identity.c"
            path.write_text(program)
            for language, portuguese, code in ((2, 0, "BPEE"), (3, 0, "BPEF"),
                                               (4, 0, "BPEI"), (5, 0, "BPED"),
                                               (7, 0, "BPES"), (2, 1, "BPEE")):
                with self.subTest(code=code, portuguese=portuguese):
                    executable = root / "identity"
                    subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                                    "-I" + str(root), "-I" + str(ROOT / "3ds_port/include"),
                                    "-DGAME_LANGUAGE=" + str(language),
                                    "-DCTR_LANG_PT_BR=" + str(portuguese), str(path),
                                    "-o", str(executable)], check=True)
                    lines = subprocess.check_output([str(executable)], text=True).splitlines()
                    self.assertEqual(lines[0], ROM_PROFILES[code])
                    if code == "BPED":
                        self.assertIn("BPED", "\n".join(lines[1:]))
