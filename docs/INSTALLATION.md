# Install and update Emerald 3DS

These instructions apply to the latest
[release](https://github.com/ZallaxDev/emerald-3ds/releases/latest).
Download the ZIP attached to the release, not the automatically generated
source-code archives (those are only needed for the builder on Linux and
macOS, below).

The download **does not contain the game**. You need a clean dump of your own
Pokémon Emerald (USA, Europe), Pokémon Esmeralda (Spain), Pokémon Émeraude
(France) or Pokémon Smeraldo (Italy) cartridge; the game is built in its
language. The builder generates the game's data pack locally from your ROM; it
does not upload, copy or modify the ROM, and it needs an Internet connection
only to download the optional follower sprites (without one, the game is built
without them).

## What you need

- A Nintendo 3DS / 2DS family console with custom firmware (Luma3DS) and the
  Homebrew Launcher.
- A clean Pokémon Emerald (USA, Europe) ROM with SHA-1
  `f3ae088181bf583e55daf962a92bb46f4f1d07b7`.
- Your console's SD card connected to your computer, or a folder whose
  contents you can copy to the root of that card.

## Install on Windows

1. Download `Emerald3DS-vX.Y.Z-Windows.zip` from the release and extract the
   **whole** ZIP to a folder.
2. Run `Emerald3DS-Builder.exe` from the extracted folder.
3. Select your ROM. The builder checks that it is the supported version.
4. Select the root of your SD card (automatically detected if it contains a
   `Nintendo 3DS` folder), or select another folder.
5. Press **Install**. The builder writes these files under the selected destination:

   ```text
   3ds/emerald3ds/Emerald3DS.3dsx
   3ds/emerald3ds/Emerald3DS.smdh
   3ds/emerald3ds/emerald3ds.pak
   ```

6. If you selected a folder instead of the SD card, copy its `3ds` folder to
   the **root** of your SD card, merging it with the existing `3ds` folder.
7. Make sure `SD:/3ds/dspfirm.cdc` is present for sound (see below). Put the
   card back in the console and launch **Emerald 3DS** from
   the Homebrew Launcher.

### Sound

Audio requires `dspfirm.cdc` at **`SD:/3ds/dspfirm.cdc`**, directly inside the
SD card's `3ds` folder, **not** inside `3ds/emerald3ds/`. This file is not
included with the release. If it is missing, the game may otherwise run
normally but without sound.

## Update an existing installation

There are two ways, depending on the release. The release notes say which one
applies; the website <https://emerald-3ds.com> also tells you.

**Quick Update: replace only `Emerald3DS.3dsx`.** When a release keeps the game's
data format, you do not have to generate anything again: your
`emerald3ds.pak` keeps working. Download `Emerald3DS.3dsx` from the release
(or scan its QR code on the website with FBI's *Remote Install → Scan QR
Code*) and replace `SD:/3ds/emerald3ds/Emerald3DS.3dsx`. The website compares
the two releases for you and shows this path only when it is safe; the game
also refuses an incompatible pack when it starts.

**Full update: generate the pack again.** When a release changes the data
(most of them while the game is in beta):

1. Either use the **web builder at <https://emerald-3ds.com>**, where you select your ROM
   and the pack is built inside your browser: the ROM is read locally and never
   leaves your computer. It gives you the ZIP to copy to the SD card, or download and extract the **new release's**
   Windows ZIP.
2. With the Windows builder: run its `Emerald3DS-Builder.exe` with your ROM and
   the same SD card (or another folder to copy to the card). Press **Install**
   again.
3. If you installed to a folder, copy its `3ds` folder to the root of the SD
   card as above. Launch the updated game from the Homebrew Launcher.

A full update always needs the builder (or web builder) of the release you are installing: regenerate `emerald3ds.pak` with the
builder from the release you are installing: a data pack only works with its
matching game version, and the game will tell you if they do not match. Your
save is stored at `SD:/3ds/emerald3ds/emerald3ds.sav`; installing or updating
does not overwrite it.

## HOME Menu shortcut (optional)

Releases after 0.1.2 include `Emerald3DS-Forwarder.cia`. Install it once with
FBI (copy it to the SD card, or use FBI's *Remote Install → Scan QR Code* with
the QR code on the website) and the game gets its own icon on the HOME Menu.
The shortcut needs Luma3DS: it starts the installed
`SD:/3ds/emerald3ds/Emerald3DS.3dsx`, so install the game first as above.
Updates keep working as described above and never need the CIA again.

## Command line and other systems

The Windows ZIP also includes `emerald3ds-builder-cli.exe`:

```text
emerald3ds-builder-cli.exe build   --rom "Pokemon Emerald.gba" --output out
emerald3ds-builder-cli.exe install --rom "Pokemon Emerald.gba" --sd E:\
emerald3ds-builder-cli.exe verify  --pak E:\3ds\emerald3ds\emerald3ds.pak
```

### Linux and macOS

The easiest way is the **web builder at <https://emerald-3ds.com>**: it runs in
the browser and needs nothing installed. To use the builder on your own
computer instead:

1. Download `Emerald3DS-vX.Y.Z-Windows.zip` from the release and extract it.
   You only need its `payload/` folder; it works on every system.
2. Download the **source code** of the same release (*Source code (zip)* or
   *(tar.gz)* under the release's assets, or
   `git clone --branch vX.Y.Z https://github.com/ZallaxDev/emerald-3ds`) and
   extract it. The builder is its `builder/` folder. Use the source of the
   release you install: an older or newer builder does not match the payload.
3. Install Python 3.11 or newer and Pillow, for example in a virtual
   environment inside the source folder:

   ```sh
   cd emerald-3ds-X.Y.Z
   python3 -m venv .venv
   . .venv/bin/activate
   python -m pip install "Pillow>=10"
   ```

4. Still in the source folder, run the builder with `PYTHONPATH=builder`
   (or run it from inside `builder/`). Point `--sd` at the mounted SD card,
   usually `/media/$USER/<name>` or `/run/media/$USER/<name>` on Linux and
   `/Volumes/<name>` on macOS, or at any folder to copy to the card later:

   ```sh
   PYTHONPATH=builder python -m emerald3ds_builder \
       --payload /path/to/Emerald3DS-vX.Y.Z-Windows/payload \
       install --rom /path/to/rom.gba --sd /media/$USER/3DS
   ```

   Generating the 3D scenery takes several minutes; let it finish.

5. If you installed to a folder, copy its `3ds` folder to the root of the SD
   card. Add `SD:/3ds/dspfirm.cdc` for sound (see [Sound](#sound)), eject the
   card and launch **Emerald 3DS** from the Homebrew Launcher.

See the [builder documentation](../builder/README.md) for other source-builder commands.

Emerald 3DS is an unofficial fan project, not affiliated
with or endorsed by Nintendo, Game Freak, Creatures or The Pokémon Company.
Pokémon and Pokémon Emerald are trademarks of their respective owners. See
`LICENSES/` in the release ZIP.
