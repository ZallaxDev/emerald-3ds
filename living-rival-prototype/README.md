# Living Rival Prototype (Phase 2.5, Layer A)

Portable, deterministic simulation core for the Living Rival system.
Target: Nintendo 3DS (ARM11), with possible PC/mobile ports. Not a GBA target.

## Build and test

GCC lives at `C:\msys64\usr\bin\gcc.exe` and is not on PATH:

```powershell
$env:PATH = 'C:\msys64\usr\bin;' + $env:PATH
make test
```

Each `test/test_*.c` is a standalone executable. `make test` builds and runs all of them.

## Rules

- C99, no platform headers, no dynamic allocation, fixed-size arrays.
- All exported symbols are `Lr`-prefixed.
- Same inputs + same seed must produce bitwise-identical state.
