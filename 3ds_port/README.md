# 3ds_port

The Nintendo 3DS port. It is built inside a bootstrapped upstream tree
(`python tools/bootstrap.py`), where it lives at `3ds_port/` next to the
decompilation it compiles.

| Path | What |
|---|---|
| `src/main_3ds.c`, `3ds_platform.c` | process, heaps, frame pacing, lifecycle |
| `src/3ds_video.c`, `src/video/*.inc`, `3ds_video_decode.c` | GPU compositor: GBA registers/VRAM/OAM to Citro3D; one translation unit, its parts in `src/video/` |
| `src/3ds_audio.c` | mixer frames to NDSP |
| `src/3ds_input*.c` | buttons, Circle Pad, touch |
| `src/3ds_data.c`, `3ds_pak.c` | game data backends (romfs, loose, pack) |
| `src/3ds_assets.c`, `3ds_map_loader.c`, `3ds_script_loader.c` | INCBIN stubs, map payloads, bundled regions |
| `src/3ds_compat.c`, `3ds_game_full.c`, `3ds_game_bridge.c` | the bridge: saves, fonts, entry into `AgbMain` |
| `src/3ds_bottom_ui.c`, `3ds_bottom_screen.c` | bottom-screen interface |
| `src/voxel/` | voxel overworld |
| `compat/` | headers game translation units see (`port_platform.h`, staging modes) |
| `scripts/` | build-time generators and checks (bundles, voxel data) |
| `tests/` | host tests (`make verify`) |
| `emerald3ds.ld.in` | linker script with the `.gamedata` section |

See `docs/ARCHITECTURE.md` and `docs/DEVELOPMENT.md` at the repository root.
