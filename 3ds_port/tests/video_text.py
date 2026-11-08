"""src/3ds_video.c as the compiler reads it, for the tests that take code from
its text: the file with its src/video/ units in place of their #include."""
from pathlib import Path
import re

SOURCE = Path(__file__).resolve().parents[1] / 'src/3ds_video.c'
UNIT = re.compile(r'^#include "video/(\w+)\.inc"\n', re.M)


def units_of(text):
    """The units a text includes, in order."""
    return UNIT.findall(text)


def read_video(path=SOURCE):
    path = Path(path)
    text = path.read_text(encoding='utf-8')
    folder = path.parent / 'video'
    left_out = {unit.stem for unit in folder.glob('*.inc')} - set(units_of(text))
    if left_out:
        raise ValueError(f'units {path.name} does not include: {sorted(left_out)}')
    return UNIT.sub(lambda line: (folder / f'{line[1]}.inc').read_text(encoding='utf-8'), text)
