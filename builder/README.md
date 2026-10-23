# Emerald 3DS Builder

Turns the player's own Pokémon Emerald (USA, Europe), Pokémon Esmeralda
(Spain), Pokémon Émeraude (France), Pokémon Smeraldo (Italy) or Pocket
Monsters Emerald (Japan) ROM into `emerald3ds.pak` and installs the game on an
SD card, in that ROM's language.

Release users get a standalone Windows executable (no Python needed). From
source (any OS, Python 3.11+ and Pillow):

```
python -m emerald3ds_builder                          # window
python -m emerald3ds_builder --payload DIR build --rom ROM --output OUT
python -m emerald3ds_builder --payload DIR install --rom ROM --sd /media/me/3DS
python -m emerald3ds_builder --payload DIR verify --pak FILE
python -m emerald3ds_builder detect
```

`DIR` is a release's `payload/` folder (executable, `emerald3ds.recipe`,
`voxelgen/`); by default the builder looks next to itself.

| Module | Role |
|---|---|
| `rom.py` | recognises the supported ROM |
| `recipe.py` | recipe format; rebuilds one file from the ROM and checks it |
| `vtree.py` | rebuilds the voxel generators' inputs from the ROM |
| `voxel.py` | runs the bundled generators (in child processes, or in-process for the web) |
| `pak.py` | writes and reads the data pack |
| `build.py` | ROM → data pack |
| `install.py` | SD card detection and installation |
| `cli.py`, `gui.py` | the two desktop front ends |
| `web.py` | entry point of the web builder (Pyodide in the browser) |
| `webmanifest.py` | `web-manifest.json`, the contract between a release and the website |

Privacy: the ROM is read into memory and never copied, uploaded or modified;
temporary files are removed even when a step fails.

Saves: before an install replaces any game file, an existing
`emerald3ds.sav` and whichever of its optional companions exist are copied:
`emerald3ds.clock`, `emerald3ds.clock.tmp`, `emerald3ds.clock.bak`,
`emerald3ds.nuzlocke`, `emerald3ds.nuzlocke.tmp`, and `emerald3ds.nuzlocke.bak`.
Temporary and backup companions are included because the game can recover
progress from them. They are copied into a new
`3ds/emerald3ds/backup/<YYYYMMDD-HHMMSS>[-N]/`
folder (counted in the free-space check). The copy is staged in a `.partial`
folder and renamed only once complete. If it fails, nothing is replaced and
cleanup is attempted; any leftovers remain marked `.partial` and must not be
used as a complete backup. The save itself is never
modified, and older backups are never deleted (remove them yourself when no
longer needed). With no save, no backup is made. If `3ds/dspfirm.cdc` is
missing on the card, both front ends show a warning (the game may have no
sound) but the install still succeeds.

## Web builder

The website runs this same package in the browser (Pyodide, in a Web Worker)
from the release's `Emerald3DS-WebPayload.zip`, through
`web.run_web_build(rom, payload, out_pak, progress, manifest)`. The only
difference with the desktop build is that the voxel generators run in-process
(`voxel.run_step_inprocess`): each script gets fresh sibling modules, argv,
working directory and import path, exactly as a new process would. Outputs are
checked against the same CRCs and ABI. `tools/build_web_payload.py --synthetic
DIR` writes a synthetic payload and "ROM" to exercise it without game data.

Tests: `python -m unittest discover -s builder/tests` (synthetic data only).
