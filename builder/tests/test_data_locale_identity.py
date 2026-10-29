"""The 3DS client and the builder accept the same cartridge in every language."""
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
    def test_client_checks_the_builders_rom(self):
        # a pack built from the right cartridge must never be refused as
        # "from another ROM" because the client and the builder disagree
        source = (ROOT / "3ds_port/src/3ds_data.c").read_text(encoding="utf-8")
        identities = source[source.index("#if GAME_LANGUAGE == LANGUAGE_SPANISH"):
                            source.index("typedef struct")]
        program = ('#include <stdint.h>\n#include <stdio.h>\n#include "3ds_locale.h"\n' + identities +
                   '\nint main(void) {\n'
                   '    for (unsigned i = 0; i < sizeof(sSupportedRomSha1); ++i)\n'
                   '        printf("%02x", sSupportedRomSha1[i]);\n'
                   '    printf("\\n%s\\n", SUPPORTED_ROM_DETAIL);\n'
                   '    return 0;\n}\n')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "constants").mkdir()
            (root / "constants/global.h").write_text(
                "#define LANGUAGE_ENGLISH 2\n#define LANGUAGE_FRENCH 3\n#define LANGUAGE_ITALIAN 4\n"
                "#define LANGUAGE_GERMAN 5\n#define LANGUAGE_SPANISH 7\n", encoding="utf-8")
            (root / "identity.c").write_text(program, encoding="utf-8")
            for language, portuguese, code in ((2, 0, "BPEE"), (2, 1, "BPEE"), (3, 0, "BPEF"),
                                               (4, 0, "BPEI"), (5, 0, "BPED"), (7, 0, "BPES")):
                with self.subTest(code=code, portuguese=portuguese):
                    executable = root / "identity"
                    subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                                    "-I" + str(root), "-I" + str(ROOT / "3ds_port/include"),
                                    "-DGAME_LANGUAGE=%d" % language, "-DCTR_LANG_PT_BR=%d" % portuguese,
                                    str(root / "identity.c"), "-o", str(executable)], check=True)
                    lines = subprocess.check_output([str(executable)], text=True).splitlines()
                    self.assertEqual(lines[0], ROM_PROFILES[code])
                    if code != "BPEE":
                        self.assertIn(code, "\n".join(lines[1:]))


if __name__ == "__main__":
    unittest.main()
