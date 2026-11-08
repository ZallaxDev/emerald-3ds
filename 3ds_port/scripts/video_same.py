#!/usr/bin/env python3
"""Proves that src/3ds_video.c still compiles to the same thing.

    video_same.py build DIR       preprocesses and compiles the tree, every
                                  configuration, into DIR (not into build/)
    video_same.py build DIR --calls   only what `calls` needs, no preprocessing
    video_same.py compare A B     the two DIRs, configuration by configuration
    video_same.py calls A B       function by function, for a change that moves
                                  code between functions

compare: the preprocessed text must match, and so must every section of the
object, relocations and symbols included. Debug information is left out: it
names the file and line each thing came from, and moving text between files
changes it. The .3dsx is no proof: build/buildinfo.c changes with every build.

calls: every function's calls and branches to other functions, in the order of
the code, and what it refers to (statics, globals, the text of its strings), in
the order the code first loads them. Pieces cut out of a function must be
always_inline: they then leave no function of their own. The -O0 builds are the
proof: there a function's calls are those of its source, in order. Optimised,
with or without -fno-inline, the same names must appear, but they may move or
be written more or fewer times. It sees neither arithmetic nor conditions:
frames are the proof of those.

Run from a shell with the Makefile's own tools (make, devkitARM) on hand.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parents[1]

R_ARM_ABS32, R_ARM_CALL, R_ARM_JUMP24 = 2, 28, 29

DUMP = ['-DCTR_FRAME_DUMP=1']
# Name: (flags added, flags taken away). Each #if and #elif of the code the
# text holds behind a define needs a configuration that reaches it: the dumps
# in Present are excluding branches, so they cannot go together.
CONFIGS = {
    'release': ([], []),
    'dump': (DUMP, []),
    'dump_battle': (DUMP + ['-DCTR_DUMP_BATTLE', '-DCTR_DUMP_FROM=1', '-DCTR_DUMP_STEP=30'], []),
    'dump_objwin': (DUMP + ['-DCTR_DUMP_OBJWIN'], []),
    'dump_swoop': (DUMP + ['-DCTR_DUMP_SWOOP=1'], []),
    'toggles': (['-DCTR_BATTLE_TOGGLES=1'], []),
    'trust': (['-DCTR_GPU_EARLY=0', '-DCTR_VRAM_TRUST=0'], []),
    'stereo_zero': (['-DCTR_STEREO_ZERO'], []),
    'slider': (['-DCTR_FORCE_SLIDER=1.0f'], []),
    'voxel_stereo': (['-DCTR_VOXEL_STEREO=1'], []),
    'no_fps': (['-DCTR_SHOW_FPS=0'], ['-DCTR_SHOW_FPS=1']),
    'together': (['-DCTR_GPU_EARLY=0', '-DCTR_VRAM_TRUST=0', '-DCTR_STEREO_ZERO',
                  '-DCTR_FORCE_SLIDER=1.0f', '-DCTR_VOXEL_STEREO=1', '-DCTR_SHOW_FPS=0'],
                 ['-DCTR_SHOW_FPS=1']),
}
# Each branch of the dumps once, and what is left together; then the same with
# less that moves code around a piece; then at -O0, the calls as the source has
# them. Only -O0 decides: optimised, a local that is now a field of a struct
# changes what GCC threads and copies, and so how many times a call is written.
CALLS = ('release', 'dump', 'dump_battle', 'dump_objwin', 'dump_swoop', 'toggles', 'together', 'no_fps')
NO_INLINE = '_noinline'
SOURCE = '_source'
for name in CALLS:
    CONFIGS[name + NO_INLINE] = (CONFIGS[name][0] + ['-fno-inline', '-fno-reorder-blocks'], CONFIGS[name][1])
    CONFIGS[name + SOURCE] = (CONFIGS[name][0] + ['-O0'], CONFIGS[name][1] + ['-O2'])
CALL_CONFIGS = [name for name in CONFIGS if name.removesuffix(NO_INLINE).removesuffix(SOURCE) in CALLS]
# What is being cut: their size and stack are printed whatever they are.
WATCHED = ('CtrVideo_Present', 'RenderBattleStereo')


def compile_command():
    """The Makefile's own command for the object."""
    text = subprocess.run(['make', '--no-print-directory', '-n', '-W', 'src/3ds_video.c',
                           'build/3ds_video.o'], cwd=PORT, check=True, capture_output=True,
                          text=True).stdout
    lines = [line for line in text.splitlines() if ' -c src/3ds_video.c ' in line]
    if len(lines) != 1:
        sys.exit(f'{len(lines)} compile commands for 3ds_video.c in make -n')
    words = shlex.split(lines[0])
    return words[:words.index('-MMD')] + ['-c', 'src/3ds_video.c']


def build_one(command, name, folder, calls_only):
    added, taken = CONFIGS[name]
    flags = [word for word in command if word not in taken and word != '-c']
    flags[1:1] = added
    started = time.time()
    if not calls_only:
        subprocess.run(flags + ['-E', '-P', '-o', str(folder / f'{name}.i')], cwd=PORT, check=True)
    subprocess.run(flags + ['-fstack-usage', '-c', '-o', str(folder / f'{name}.o')], cwd=PORT, check=True)
    return name, time.time() - started


def build(folder, jobs, calls_only):
    folder.mkdir(parents=True, exist_ok=True)
    command = compile_command()
    names = CALL_CONFIGS if calls_only else CONFIGS
    with ThreadPoolExecutor(jobs) as pool:
        for name, seconds in pool.map(lambda name: build_one(command, name, folder, calls_only), names):
            print(f'{name}: {seconds:.0f} s', flush=True)


def read_elf(path):
    """ARM ELF32: bytes, section names, section headers, symbols."""
    data = path.read_bytes()
    table = struct.unpack_from('<I', data, 32)[0]
    entry_size, count, names = struct.unpack_from('<HHH', data, 46)
    headers = [struct.unpack_from('<10I', data, table + index * entry_size) for index in range(count)]
    strings = headers[names]

    def text_at(base, offset):
        end = data.index(b'\0', base + offset)
        return data[base + offset:end].decode()

    section_names = [text_at(strings[4], header[0]) for header in headers]
    symbol_table = next(header for header in headers if header[1] == 2)
    string_base = headers[symbol_table[6]][4]
    symbols = []
    for offset in range(symbol_table[4], symbol_table[4] + symbol_table[5], 16):
        name, value, size, info, other, index = struct.unpack_from('<IIIBBH', data, offset)
        symbols.append((text_at(string_base, name), value, size, info, other, index))
    return data, section_names, headers, symbols


def sections(path):
    """{name: [content]}, relocations by symbol name, debug left out."""
    data, section_names, headers, symbols = read_elf(path)
    found = {}
    for name, header in zip(section_names, headers):
        kind, offset, size = header[1], header[4], header[5]
        if name.startswith(('.debug', '.rel.debug')) or kind in (0, 2, 3):
            continue
        if kind == 8:
            content = size
        elif kind == 9:
            content = [(where, symbols[info >> 8][0], info & 255) for where, info in
                       struct.iter_unpack('<II', data[offset:offset + size])]
        else:
            content = data[offset:offset + size]
        found.setdefault(name, []).append(content)
    found['(symbols)'] = [(name, value, size, info, other,
                           section_names[index] if 0 < index < len(section_names) else index)
                          for name, value, size, info, other, index in symbols]
    return found


def plain(name):
    """Without the number GCC gives a static of a function or a clone: it counts
    everything before it, so moving code changes it."""
    return re.sub(r'\.\d+$', '', name)


def functions(path):
    """{function: [{calls, uses, size}]}: a list, as clones can share their name."""
    data, section_names, headers, symbols = read_elf(path)
    code = {}
    for name, value, size, info, _, index in symbols:
        if info & 15 == 2:
            code.setdefault(index, []).append((value, size, name))

    def function_at(section, offset):
        for value, size, name in code.get(section, ()):
            if value <= offset < value + size:
                return name
        return None

    def reference(entry, addend):
        """A function, the text of a string or a name with its offset."""
        name, value, _, _, _, index = symbols[entry]
        if 0 < index < len(headers):
            flags, offset = headers[index][2], headers[index][4]
            if flags & 4:
                return plain(function_at(index, value + addend) or section_names[index])
            if flags & 0x20:
                start = offset + value + addend
                return repr(data[start:data.index(b'\0', start)])
            name = name or re.sub(r'^\.(bss|data|rodata)\.', '', section_names[index])
        return f'{plain(name)}+{addend}' if addend else plain(name)

    found = {}
    facts = {}
    pools = {}
    for section, entries in code.items():
        for value, size, function in entries:
            fact = {'calls': [], 'uses': [], 'size': size}
            facts[section, function] = fact
            found.setdefault(plain(function), []).append(fact)
    for header in headers:
        section = header[7]
        if header[1] != 9 or not 0 < section < len(headers) or not headers[section][2] & 4:
            continue
        where = headers[section][4]
        for offset, info in sorted(struct.iter_unpack('<II', data[header[4]:header[4] + header[5]])):
            owner = function_at(section, offset)
            if owner is None:
                continue
            fact = facts[section, owner]
            word = struct.unpack_from('<I', data, where + offset)[0]
            kind = info & 255
            if kind in (R_ARM_CALL, R_ARM_JUMP24):
                # The instruction's own offset, past the pipeline: 0 for a named
                # target, the place in its section for a section symbol.
                target = (((word & 0xffffff) ^ 0x800000) - 0x800000) * 4 + 8
                fact['calls'].append(('bl ' if kind == R_ARM_CALL else 'b ') + reference(info >> 8, target))
            elif kind == R_ARM_ABS32:
                target = reference(info >> 8, word)
                # A switch's table points back at its own code.
                if target != plain(owner):
                    pools.setdefault(owner, {})[section, offset] = target
            else:
                sys.exit(f'{path}: relocation {kind} in {owner}: what is it?')
    for section, entries in code.items():
        where = headers[section][4]
        for value, size, function in entries:
            pool = pools.get(function, {})
            loaded = [pc + 8 + (word & 0xfff if word & 1 << 23 else -(word & 0xfff))
                      for pc in range(value, value + size, 4)
                      for word in struct.unpack_from('<I', data, where + pc)
                      if word & 0x0f7f0000 == 0x051f0000]
            # As the code loads them, not as the pools lie, then what no load names.
            order = [pool[section, offset] for offset in loaded if (section, offset) in pool]
            facts[section, function]['uses'] = list(dict.fromkeys(order + list(pool.values())))
    return found


def stack_use(path):
    """{function: bytes}, from -fstack-usage: 'file:line:column:name<tab>bytes<tab>kind'."""
    found = {}
    for line in path.read_text().splitlines():
        place, size, _ = line.split('\t')
        name = plain(place.rsplit(':', 1)[1])
        found[name] = max(found.get(name, 0), int(size))
    return found


def preprocessed(path):
    """Without blank lines: a comment leaves some, and comments may change."""
    return [line for line in path.read_bytes().splitlines() if line.strip()]


def compare(first, second):
    differences = 0
    for name in CONFIGS:
        same_text = preprocessed(first / f'{name}.i') == preprocessed(second / f'{name}.i')
        a, b = sections(first / f'{name}.o'), sections(second / f'{name}.o')
        different = sorted(key for key in a.keys() | b.keys() if a.get(key) != b.get(key))
        digest = hashlib.sha256(repr(sorted(a.items(), key=lambda item: item[0])).encode())
        print(f'{name}: text {"same" if same_text else "DIFFERENT"}, {len(a)} sections, '
              f'{len(different)} different, {digest.hexdigest()[:12]}')
        for key in different[:20]:
            print(f'    {key}')
        differences += len(different) + (not same_text)
    print('SAME' if not differences else f'DIFFERENT: {differences}')
    return differences


def difference(first, second):
    """Where two lists of a function part ways."""
    at = next((index for index, (a, b) in enumerate(zip(first, second)) if a != b), min(len(first), len(second)))
    pick = lambda items: items[at] if at < len(items) else '(end)'
    return f'item {at} of {len(first)}/{len(second)}: {pick(first)} / {pick(second)}'


def compare_function(first, second):
    """'same', 'moved' (the same things in another order), 'copied' (the same
    things, some written more or fewer times) or what differs."""
    if len(first) != len(second):
        return f'{len(first)}/{len(second)} copies'
    verdict = 'same'
    for a, b in zip(first, second):
        for part in 'calls', 'uses':
            if set(a[part]) != set(b[part]):
                return f'{part} {difference(a[part], b[part])}'
            if sorted(a[part]) != sorted(b[part]):
                verdict = 'copied'
            elif a[part] != b[part] and verdict == 'same':
                verdict = 'moved'
    return verdict


def compare_calls(first, second):
    differences = 0
    for config in CALL_CONFIGS:
        a, b = functions(first / f'{config}.o'), functions(second / f'{config}.o')
        stack_a, stack_b = stack_use(first / f'{config}.su'), stack_use(second / f'{config}.su')
        verdicts = {name: compare_function(a.get(name, []), b.get(name, [])) for name in a.keys() | b.keys()}
        moved = sorted(name for name, verdict in verdicts.items() if verdict in ('moved', 'copied'))
        different = sorted(name for name, verdict in verdicts.items() if verdict not in ('same', 'moved', 'copied'))
        grown = sorted(name for name in (a.keys() & b.keys()) - set(WATCHED)
                       if [fact['size'] for fact in a[name]] != [fact['size'] for fact in b[name]]
                       or stack_a.get(name) != stack_b.get(name))
        print(f'{config}: {len(a)} functions, {len(moved)} moved or copied, {len(different)} different, '
              f'{len(grown)} others of another size or stack')
        for name in WATCHED:
            size_a, size_b = (sum(fact['size'] for fact in side.get(name, [])) for side in (a, b))
            calls = sum(len(fact['calls']) for fact in a.get(name, []))
            print(f'    {name}: {calls} calls, {size_a} -> {size_b} bytes, '
                  f'stack {stack_a.get(name)} -> {stack_b.get(name)}')
        for name in moved[:10]:
            for x, y in zip(a[name], b[name]):
                for part in 'calls', 'uses':
                    if x[part] != y[part]:
                        print(f'    {verdicts[name]} {name}: {part} {difference(x[part], y[part])}')
        for name in different[:20]:
            print(f'    DIFFERENT {name}: {verdicts[name]}')
        differences += len(different) + len(moved) if config.endswith(SOURCE) else 0
    print('SAME' if not differences else f'DIFFERENT: {differences}')
    return differences


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    builder = commands.add_parser('build')
    builder.add_argument('folder', type=Path)
    builder.add_argument('--jobs', type=int, default=7)
    builder.add_argument('--calls', action='store_true')
    for command in 'compare', 'calls':
        comparer = commands.add_parser(command)
        comparer.add_argument('first', type=Path)
        comparer.add_argument('second', type=Path)
    args = parser.parse_args()
    if args.command == 'build':
        build(args.folder.resolve(), args.jobs, args.calls)
    elif (compare if args.command == 'compare' else compare_calls)(args.first, args.second):
        sys.exit(1)


if __name__ == '__main__':
    main()
