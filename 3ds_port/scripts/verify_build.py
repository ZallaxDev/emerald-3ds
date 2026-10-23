"""Verify native ARM11 output and inventory link dependencies for each group.

No undefined symbols are synthesized. References in discarded game sections are
listed separately from the strict final ELF link (which must resolve everything).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--devkitarm', required=True)
    parser.add_argument('--stage', type=int, required=True)
    parser.add_argument('--video', type=int, default=0)
    # Only the full game is built now; the staged bring-up targets are gone.
    parser.add_argument('--full', type=int, choices=(1,), default=1)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    nm = Path(args.devkitarm) / 'bin/arm-none-eabi-nm'
    readelf = Path(args.devkitarm) / 'bin/arm-none-eabi-readelf'

    def run(tool, *arguments):
        return subprocess.check_output([str(tool), *map(str, arguments)], text=True)

    elf = root / 'emerald3ds.elf'
    binary = (root / 'emerald3ds.3dsx').read_bytes()
    assert binary[:4] == b'3DSX', 'Invalid .3dsx magic'
    assert struct.unpack_from('<H', binary, 4)[0] >= 0x20, 'Invalid .3dsx header'
    header = run(readelf, '-h', elf)
    attributes = run(readelf, '-A', elf)
    assert 'ELF32' in header and 'ARM' in header and 'hard-float ABI' in header
    assert 'VFP registers' in attributes
    # libctru/newlib archives can raise the merged architecture attribute.
    # Verify the architecture of every object compiled by this target itself.
    for obj in (root / 'build').rglob('*.o'):
        obj_attributes = run(readelf, '-A', obj)
        assert 'Tag_CPU_arch: v6K' in obj_attributes, str(obj)
    undefined = run(nm, '-u', elf)
    assert not re.search(r'^\s*U\s', undefined, re.M), undefined
    symbols = run(nm, '--defined-only', elf)
    names = {line.split()[-1] for line in symbols.splitlines() if line.split()}

    required = ['CtrGame_Init', 'CpuSet', 'DmaSet', 'Random', 'AllocZeroed', 'CtrInput_Update',
                'AgbMain', 'CtrGame_VBlank', 'CB2_Overworld', 'CtrScripts_Init',
                'CtrMaps_Init', 'CtrSongs_Init', 'Port_ResolveAssetPointer']
    if args.video:
        required += ['CtrVideo_Present', 'C2D_DrawImage', 'C3D_FrameBegin', 'SpriteCallback_RedArrowCursor']
        assert not any(name in names for name in ('DrawFrame', 'DrawScanline', 'DrawFrameFast'))
    groups = {
        2: ['SetMainCallback2', 'InitKeys', 'gMain', 'CopyBufferedValuesToGpuRegs'],
        4: ['RunTasks', 'CreateTask'],
        5: ['AnimateSprites', 'BuildOamBuffer', 'LoadPalette'],
        6: ['IncrementGameStat', 'GetGameStat'],
        7: ['RunScriptCommand', 'ScriptReadHalfword'],
        8: ['SwapTurnOrder'],
        # The full game inlines ConsumeTrackByte and keeps the music player.
        9: ['SampleMixer', 'MP2KPlayerMain', 'MidiKeyToFreq'],
    }
    for stage, functions in groups.items():
        if args.stage >= stage:
            required.extend(functions)
    assert set(required) <= names, f'Missing actual game symbols: {set(required) - names}'

    # The game's INCBIN stubs are RomFS lookup keys: validate their index.
    subprocess.check_call([sys.executable, str(root / 'scripts/verify_assets.py')])

    objects = [root / name for name in (root / 'build/link.rsp').read_text().split()]
    definitions = {}
    for obj in objects:
        for line in run(nm, '--defined-only', '--extern-only', obj).splitlines():
            fields = line.split()
            if len(fields) >= 3:
                definitions[fields[-1]] = str(obj.relative_to(root))
    references = []
    for obj in objects:
        for line in run(nm, '-u', obj).splitlines():
            fields = line.split()
            if not fields:
                continue
            name = fields[-1]
            if name in definitions:
                resolution = definitions[name]
                category = 'portable/backend'
            elif name in names:
                resolution = 'final ELF: libctru/newlib/backend'
                category = 'platform'
            else:
                # These are references in unselected, resource-dependent
                # functions. They are NOT stubs and are NOT executable.
                resolution = 'discarded section; not part of final ELF'
                category = 'resource' if name.startswith(('g', 's', 'EventScript_', 'BattleScript_')) else 'unsupported-functionality'
            references.append({'object': str(obj.relative_to(root)), 'symbol': name,
                               'category': category, 'resolution': resolution})

    report = {
        'stage': args.stage, 'full_game': True, 'gpu_video': bool(args.video), 'elf_strong_undefined': [],
        'required_game_symbols': required, 'nds_dependencies': [],
        'retained_asset_placeholders': None,
        'romfs_assets_verified': True, 'references': references,
        'sha256_3dsx': hashlib.sha256(binary).hexdigest(), 'bytes_3dsx': len(binary),
        'runtime': 'REQUIRES EMULATOR; build verification is not runtime evidence',
    }
    output = root / 'build' / f'link-audit-stage-{args.stage}.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS stage {args.stage}: ARM11 hard-float, {len(required)} required game symbols, '
          f'no strong undefined symbol; '
          f'RomFS assets verified; {len(binary)} bytes')
    print(f'Symbol inventory: {output}')


if __name__ == '__main__':
    main()
