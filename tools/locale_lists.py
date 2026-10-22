#!/usr/bin/env python3
"""Easy Chat's move and Pokémon word groups in the order of the player's ROM.

These four groups are bare lists of move and species IDs, kept in alphabetical
order of their names, so a translated ROM lists the same IDs in another order.
The text manifests only rewrite strings, so without this step a translated build
kept the English order (and its recipe carried that English table as literal
bytes). The order is read from the player's ROM at the symbol the language's
reference symbols give; it must hold exactly the IDs the source list holds.
Nothing from the ROM is stored in the repository.
"""
from __future__ import annotations

import gzip
import re
import struct
from collections import Counter
from pathlib import Path

GROUPS = (
    ("gEasyChatGroup_Move1", "src/data/easy_chat/easy_chat_group_move_1.h", "include/constants/moves.h"),
    ("gEasyChatGroup_Move2", "src/data/easy_chat/easy_chat_group_move_2.h", "include/constants/moves.h"),
    ("gEasyChatGroup_Pokemon", "src/data/easy_chat/easy_chat_group_pokemon.h", "include/constants/species.h"),
    ("gEasyChatGroup_Pokemon2", "src/data/easy_chat/easy_chat_group_pokemon2.h", "include/constants/species.h"),
)


def _symbol(symbols: str, name: str) -> tuple[int, int]:
    address = re.search(r"^\.equ %s, (\d+)$" % name, symbols, re.M)
    size = re.search(r"^\.size %s, (\d+)$" % name, symbols, re.M)
    if not address or not size:
        raise ValueError("Reference symbols lack " + name)
    return int(address.group(1)) - 0x08000000, int(size.group(1))


def stage_id_lists(tree: Path, source: bytes, symbols_path: Path, writes: dict) -> None:
    symbols = gzip.decompress(symbols_path.read_bytes()).decode()
    offsets = {name: _symbol(symbols, name) for name, relative, _ in GROUPS
               if (tree / relative).is_file()}
    stage_id_lists_at_offsets(tree, source, offsets, writes)


def stage_id_lists_at_offsets(tree: Path, source: bytes, offsets: dict, writes: dict) -> None:
    """Use verified ROM coordinates when a complete reference symbol file is unavailable."""
    for name, relative, constants in GROUPS:
        path = tree / relative
        if not path.is_file():
            # A partial tree (the staging tests' synthetic ones): nothing to order.
            continue
        text = path.read_text(encoding="utf-8")
        names = re.findall(r"^[ \t]*((?:MOVE|SPECIES)_\w+),[ \t]*$", text, re.M)
        defines = dict(re.findall(r"^#define\s+((?:MOVE|SPECIES)_\w+)\s+(\d+)\b",
                                  (tree / constants).read_text(encoding="utf-8"), re.M))
        by_id = {}
        for entry in names:
            if entry not in defines:
                raise ValueError("%s: %s has no numeric value" % (relative, entry))
            by_id[int(defines[entry])] = entry
        location = offsets.get(name)
        if (not isinstance(location, (list, tuple)) or len(location) != 2
                or any(type(value) is not int for value in location)):
            raise ValueError("Missing or invalid ROM list coordinates: " + name)
        offset, size = location
        if size != 2 * len(names) or not 0 <= offset <= len(source) - size:
            raise ValueError("%s: the ROM's list does not fit the source list" % name)
        ids = list(struct.unpack_from("<%dH" % len(names), source, offset))
        if Counter(ids) != Counter(int(defines[entry]) for entry in names):
            raise ValueError("%s: the ROM's list holds other IDs than the source list" % name)
        order = [by_id[value] for value in ids]
        if order == names:
            continue
        lines = iter(order)
        writes[path] = re.sub(r"^([ \t]*)(?:MOVE|SPECIES)_\w+,[ \t]*$",
                              lambda m: m.group(1) + next(lines) + ",", text, flags=re.M).encode()
