"""Check native locale columns, indexed tables and the production selector.

Run from the repository with: python3 3ds_port/tests/locale_column_test.py
The optional --base-ref compares all five original language columns with git.
"""
import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def calls(source):
    # Include literal calls inside macro definitions, but not their parameters.
    source = re.sub(r'#define\s+CTR_TEXT(?:_PT)?\([^\n]*', '', source)
    for match in re.finditer(r'\bCTR_TEXT(_PT)?\s*\(', source):
        start = match.end()
        depth, quote, escape, args = 1, None, False, []
        for i in range(start, len(source)):
            char = source[i]
            if quote:
                if escape:
                    escape = False
                elif char == '\\':
                    escape = True
                elif char == quote:
                    quote = None
            elif char in ('"', "'"):
                quote = char
            elif char == '(':
                depth += 1
            elif char == ')':
                depth -= 1
                if not depth:
                    args.append(source[start:i].strip())
                    if match.group(1):
                        assert len(args) == 2
                        args = [args[0]] * 4 + [args[1]]
                    yield args
                    break
            elif char == ',' and depth == 1:
                args.append(source[start:i].strip())
                start = i + 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--base-ref')
    args = parser.parse_args()
    total, preserved = 0, 0
    for path in sorted(ROOT.rglob('*')):
        if path.suffix not in ('.c', '.h', '.inc'):
            continue
        current = list(calls(path.read_text(encoding='utf-8')))
        for call in current:
            assert len(call) in (5, 6), (path, call)
            # Five is permitted only for the expanded two-argument PT helper.
        direct = path.read_text(encoding='utf-8')
        assert not any(len(a) != 6 for a in calls(re.sub(r'CTR_TEXT_PT\s*\(', 'IGNORED_PT(', direct))), path
        total += len(current)
        if args.base_ref:
            relative = path.relative_to(ROOT.parent)
            old = subprocess.check_output(['git', 'show', f'{args.base_ref}:{relative}'], cwd=ROOT.parent, text=True)
            before = Counter(tuple(a[:5]) for a in calls(old))
            after = Counter(tuple(a[:4] + [re.sub(r"\[5\]$", "[4]", a[5])]) if len(a) == 6 else tuple(a) for a in current)
            assert before == after, (relative, before - after, after - before)
            preserved += sum(before.values())
    game_source = ROOT.parent / 'src'
    if game_source.is_dir():
        for path in sorted(game_source.rglob('*.c')):
            for call in calls(path.read_text(encoding='utf-8')):
                assert len(call) == 6, (path, call)
    game = (ROOT / 'src/3ds_bottom_game.inc').read_text(encoding='utf-8')
    tables = re.findall(r'static const char \*const (?:names|labels)\[(?:|5|CTR_DEX_PARAMS|3)\]\[6\] = \{(.*?)\};', game, re.S)
    assert len(tables) == 4
    rows = 0
    for table in tables:
        for row in re.findall(r'\{([^{}]*)\}', table):
            assert len(list(calls('CTR_TEXT(' + row + ')'))[0]) == 6, row
            rows += 1
    assert rows == 18
    if args.base_ref:
        original = subprocess.check_output(['git', 'show', f'{args.base_ref}:3ds_port/src/3ds_bottom_game.inc'], cwd=ROOT.parent, text=True)
        old_tables = re.findall(r'static const char \*const (?:names|labels)\[(?:|5|CTR_DEX_PARAMS|3)\]\[5\] = \{(.*?)\};', original, re.S)
        assert len(old_tables) == 4
        for before, after in zip(old_tables, tables):
            old_rows = [next(calls('CTR_TEXT(' + row + ')')) for row in re.findall(r'\{([^{}]*)\}', before)]
            new_rows = [(lambda a: a[:4] + a[5:6])(next(calls('CTR_TEXT(' + row + ')'))) for row in re.findall(r'\{([^{}]*)\}', after)]
            assert old_rows == new_rows, 'Indexed table changed an original language'
        print('PASS indexed preservation: 18 rows / 90 original arguments unchanged')
    header = (ROOT / 'include/3ds_locale.h').read_text(encoding='utf-8')
    selector = header[header.index('#if CTR_LANG_PT_BR'):header.rindex('#endif')]
    with tempfile.TemporaryDirectory(prefix='locale-columns-') as directory:
        source = Path(directory) / 'test.c'
        binary = Path(directory) / 'test'
        for language, pt, expected in [(0, 0, 'en'), (1, 0, 'es'), (2, 0, 'fr'), (3, 0, 'it'), (4, 0, 'de'), (4, 1, 'pt')]:
            source.write_text('#include <assert.h>\n#include <string.h>\n'
                              '#define LANGUAGE_SPANISH 1\n#define LANGUAGE_FRENCH 2\n'
                              '#define LANGUAGE_ITALIAN 3\n#define LANGUAGE_GERMAN 4\n'
                              f'#define GAME_LANGUAGE {language}\n#define CTR_LANG_PT_BR {pt}\n' + selector +
                              '\nint main(void) { assert(!strcmp(CTR_TEXT("en", "es", "fr", "it", "de", "pt"), "' + expected + '")); }\n')
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    print(f'PASS locale columns: {total} calls, 4 indexed tables / {rows} rows, 6 compiled selectors')
    if args.base_ref:
        print(f'PASS preservation against {args.base_ref}: {preserved} calls / {preserved * 5} original arguments unchanged')


if __name__ == '__main__':
    main()
