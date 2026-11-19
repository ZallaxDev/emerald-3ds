import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("localize_spanish", ROOT / "tools/localize_spanish.py")
localization = importlib.util.module_from_spec(spec)
spec.loader.exec_module(localization)


SPANISH = r'static const u8 sText_PkmnTeamGainedEXP[] = _("¡El resto del equipo ganó\npuntos de experiencia!\p");'


class SpanishBattleTextTests(unittest.TestCase):
    def setUp(self):
        # the prefixes as the Spanish ROM writes them: a leading space, after
        # the name (0051-native-name-prefix-order)
        names = ["sText_WildPkmnPrefix"] + ["sText_FoePkmnPrefix" + s for s in ("", "2", "3", "4")]
        self.prefixes = "\n".join('static const u8 ' + name + '[] = _(" salvaje");' for name in names)
        self.source = 'static const u8 sText_PkmnTeamGainedEXP[] = _("original");\n' + self.prefixes

    def test_translation_leaves_the_prefixes(self):
        result = localization.localize_port_battle_text(self.source)
        self.assertIn(SPANISH, result)
        self.assertIn(self.prefixes, result)
        self.assertEqual(localization.localize_port_battle_text(result), result)

    def test_missing_definition_is_rejected(self):
        with self.assertRaises(ValueError):
            localization.localize_port_battle_text("")

    def test_language_conditional_keeps_the_others_and_localizes_the_default(self):
        """The shape of src/battle_message.c: French and Italian branches,
        the Italian one under a comment, English as the default."""
        original = 'static const u8 sText_PkmnTeamGainedEXP[] = _("original");'
        french = r'static const u8 sText_PkmnTeamGainedEXP[] = _("Le reste de votre équipe\ngagne des Points EXP.!\p");'
        italian = r'static const u8 sText_PkmnTeamGainedEXP[] = _("Il resto della squadra\nriceve punti ESP.!\p");'
        conditional = (
            "#if defined(PORT_BRIDGE) && GAME_LANGUAGE == LANGUAGE_FRENCH\n" + french + "\n"
            "#elif defined(PORT_BRIDGE) && GAME_LANGUAGE == LANGUAGE_ITALIAN\n"
            "// Italian wording of the port's own message.\n" + italian + "\n"
            "#else\n" + original + "\n#endif"
        )
        source = self.source.replace(original, conditional)
        result = localization.localize_port_battle_text(source)
        self.assertIn(french, result)
        self.assertIn(italian, result)
        self.assertIn("#else\n" + SPANISH, result)
        self.assertNotIn(original, result)
        self.assertEqual(localization.localize_port_battle_text(result), result)

    def test_duplicate_definitions_outside_conditional_are_rejected(self):
        duplicate = 'static const u8 sText_PkmnTeamGainedEXP[] = _("duplicate");'
        with self.assertRaises(ValueError):
            localization.localize_port_battle_text(self.source + "\n" + duplicate)

    @unittest.skipUnless((ROOT / "src/battle_message.c").exists(), "requires the patched source tree")
    def test_the_tree_s_battle_message_is_accepted(self):
        """The real source, as every language's tree starts from it."""
        source = (ROOT / "src/battle_message.c").read_text(encoding="utf-8")
        result = localization.localize_port_battle_text(source)
        self.assertIn("#else\n" + SPANISH, result)
        self.assertEqual(result.count("sText_PkmnTeamGainedEXP[] ="), source.count("sText_PkmnTeamGainedEXP[] ="))

    @unittest.skipUnless(shutil.which("cc"), "requires a host C compiler")
    def test_clock_accents_use_game_glyphs(self):
        source = (ROOT / "3ds_port/src/3ds_bottom_text.inc").read_text(encoding="utf-8")
        start = source.index("static const u8 *Ascii(")
        end = source.index("static const u8 *Number(", start)
        harness = '''
#include <assert.h>
#include <stdint.h>
#include "3ds_locale_text.h"
typedef uint8_t u8;
#define CHAR_N_TILDE 0x14
#define CHAR_I_ACUTE 0x5A
#define EOS 0xFF
'''
        harness += source[start:end]
        harness += '''
int main(void) {
    const u8 *morning = Ascii("MAÑANA");
    assert(morning[2] == CHAR_N_TILDE && morning[6] == EOS);
    const u8 *day = Ascii("DÍA");
    assert(day[1] == CHAR_I_ACUTE && day[3] == EOS);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            code = folder / "clock.c"
            binary = folder / "clock"
            code.write_text(harness, encoding="utf-8")
            subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "3ds_port/include"),
                            str(code), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
