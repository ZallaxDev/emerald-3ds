"""Versioned, content-addressed cache for local voxel developer tools.

These files contain pickle data and belong in a private local cache directory.
"""
import hashlib
import os
import pickle
import platform
import sys
import tempfile
from contextlib import suppress
from functools import lru_cache
from pathlib import Path

from PIL import __version__ as pillow_version

_MAGIC = b"EM3DVC01"


@lru_cache(maxsize=4)
def fingerprint(root, port, fixes):
    """Hash generator sources, map data and art once per invocation."""
    root, port, fixes = Path(root), Path(port), Path(fixes)
    digest = hashlib.sha256(_MAGIC)
    digest.update(repr((sys.implementation.name, sys.version, platform.machine(),
                        pillow_version, os.environ.get("PYTHONHASHSEED"))).encode())
    inputs = ((root, ("data/layouts", "data/maps", "data/tilesets", "src/data/tilesets",
                      "include/constants", "graphics/intro/scene_1")),
              (port, ("scripts", "assets/voxel", "src/voxel")))
    for base, folders in inputs:
        for folder in folders:
            files = sorted(p for p in (base / folder).rglob("*") if p.is_file()
                           and "__pycache__" not in p.parts and p.suffix not in (".pyc", ".pyo"))
            for path in files:
                digest.update(path.relative_to(base).as_posix().encode() + b"\0")
                with path.open("rb") as stream:
                    digest.update(hashlib.file_digest(stream, "sha256").digest())
    if fixes.is_file():
        with fixes.open("rb") as stream:
            digest.update(hashlib.file_digest(stream, "sha256").digest())
    return digest.hexdigest()


def read(path, key):
    try:
        with open(path, "rb") as stream:
            header = stream.read(len(_MAGIC) + 32)
            data = stream.read()
        if header != _MAGIC + hashlib.sha256(data).digest():
            return None
        cached_key, value = pickle.loads(data)
        return value if cached_key == key else None
    except (OSError, pickle.PickleError, AttributeError, EOFError, ValueError, ImportError, TypeError):
        return None


def write(path, key, value):
    """Publish a complete cache entry or leave the previous one usable."""
    path = Path(path)
    temporary = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        data = pickle.dumps((key, value), protocol=pickle.HIGHEST_PROTOCOL)
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(_MAGIC + hashlib.sha256(data).digest())
            stream.write(data)
            stream.flush()
        temporary.replace(path)
    except OSError:
        pass  # a developer cache is optional
    finally:
        if temporary is not None:
            with suppress(OSError):
                temporary.unlink(missing_ok=True)
