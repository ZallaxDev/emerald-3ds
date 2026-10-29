Emerald 3DS
===========

A native Nintendo 3DS port of Pokémon Emerald that uses both screens.

This download does NOT contain the game. You need your own dump of your own
Pokémon Emerald (USA, Europe), Pokémon Esmeralda (Spain), Pokémon Émeraude
(France), Pokémon Smeraldo (Italy), Pokémon Smaragd (Germany) or Pocket
Monsters Emerald (Japan) cartridge; the game is built in that ROM's language.
From the English ROM it can also be built in Brazilian Portuguese, a fan
translation by this project: choose it under "Game language". The builder reads that
ROM on this computer and generates the game's data pack from it. The ROM is
not uploaded, copied or modified. An Internet connection is only used to
download the optional follower sprites.

What you need
-------------
- A Nintendo 3DS / 2DS family console with custom firmware (Luma3DS) and the
  Homebrew Launcher.
- A clean ROM of one of these cartridges (the builder checks its SHA-1):
    Pokémon Emerald (USA, Europe)    f3ae088181bf583e55daf962a92bb46f4f1d07b7
    Pokémon Esmeralda (Spain)        fe1558a3dcb0360ab558969e09b690888b846dd9
    Pokémon Émeraude (France)        ca666651374d89ca439007bed54d839eb7bd14d0
    Pokémon Smeraldo (Italy)         1692db322400c3141c5de2db38469913ceb1f4d4
    Pokémon Smaragd (Germany)        61c2eb2b380b1a75f0c94b767a2d4c26cd7ce4e3
    Pocket Monsters Emerald (Japan)  d7cf8f156ba9c455d164e1ea780a6bf1945465c2
  A release builds the languages it was made with (its payload folder has
  one subfolder per language besides English: es, fr, it, de, ja, pt).
  Portuguese is built from the English ROM. The Japanese game is
  experimental.
- The console's SD card in this computer (or any folder, to copy by hand).

Install
-------
1. Extract this whole ZIP to a folder.
2. Run Emerald3DS-Builder.exe.
3. Choose your ROM. The builder checks that it is the supported one.
   With the English ROM, choose the game language too (English or
   Português (Brasil)).
4. Choose your SD card (it is detected when it has a "Nintendo 3DS" folder)
   or any folder.
5. Press Install. It writes:
       /3ds/emerald3ds/Emerald3DS.3dsx
       /3ds/emerald3ds/Emerald3DS.smdh
       /3ds/emerald3ds/emerald3ds.pak
6. Put the card back in the console and start Emerald 3DS from the
   Homebrew Launcher.

Saves are kept in /3ds/emerald3ds/emerald3ds.sav. Reinstalling or updating
never touches it.

Command line
------------
    emerald3ds-builder-cli.exe build   --rom "Pokemon Emerald.gba" --output out
    emerald3ds-builder-cli.exe install --rom "Pokemon Emerald.gba" --sd E:\
    emerald3ds-builder-cli.exe install --rom "Pokemon Emerald.gba" --sd E:\ --language pt
    emerald3ds-builder-cli.exe verify  --pak E:\3ds\emerald3ds\emerald3ds.pak

Updating
--------
Each release comes with its own builder. Run the new builder again with the
same ROM: a data pack only works with the release that generated it, and the
game tells you when they do not match.

Legal
-----
Emerald 3DS is an unofficial fan project, not affiliated with or endorsed by
Nintendo, Game Freak, Creatures or The Pokémon Company. Pokémon and Pokémon
Emerald are trademarks of their respective owners. See LICENSES/.
