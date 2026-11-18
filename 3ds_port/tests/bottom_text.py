"""src/3ds_bottom_ui.c as the compiler reads it, for the tests that take code from
its text: the file with its 3ds_bottom_*.inc pieces in place of their #include."""
from pathlib import Path
import re

SOURCE = Path(__file__).resolve().parents[1] / 'src/3ds_bottom_ui.c'
PIECE = re.compile(r'^#include "3ds_bottom_(\w+)\.inc"\n', re.M)


def pieces_of(text):
    """The pieces a text includes, in order."""
    return PIECE.findall(text)


def read_bottom(path=SOURCE):
    path = Path(path)
    text = path.read_text(encoding='utf-8')
    folder = path.parent
    left_out = {piece.stem[len('3ds_bottom_'):] for piece in folder.glob('3ds_bottom_*.inc')} - set(pieces_of(text))
    if left_out:
        raise ValueError(f'pieces {path.name} does not include: {sorted(left_out)}')
    return PIECE.sub(lambda line: (folder / f'3ds_bottom_{line[1]}.inc').read_text(encoding='utf-8'), text)
