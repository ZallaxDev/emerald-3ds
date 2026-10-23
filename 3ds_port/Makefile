.SUFFIXES:
# The game compile is a preprocessor pipeline; without pipefail a failing first
# stage leaves a truncated object behind and only the link notices.
SHELL := bash
.SHELLFLAGS := -o pipefail -c
.DEFAULT_GOAL := all

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB := $(DEVKITPRO)/libctru
CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CPPFLAGS := -DPORTABLE -DPLATFORM_3DS -D__3DS__ -Iinclude -I$(CTRULIB)/include
CFLAGS := $(ARCH) -O2 -g -std=gnu99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
LDFLAGS := $(ARCH) -L$(CTRULIB)/lib
# Observe the public GX queue for upload admission; never start it early.
LDFLAGS += -Wl,--wrap=GX_BindQueue
# The GPU started early in a frame (3ds_video.c GpuStartEarly) flushes first.
LDFLAGS += -Wl,--wrap=GX_ProcessCommandList -Wl,--wrap=GX_TextureCopy -Wl,--wrap=GX_DisplayTransfer -Wl,--wrap=GX_RequestDma
# Cache flushes made by the kernel call itself, not through the GSP service (3ds_video.c).
LDFLAGS += -Wl,--wrap=GSPGPU_FlushDataCache
# Every GPU memory allocation, charged to the code that made it (3ds_gpumem.c).
GPUMEM_WRAPPED := vramAlloc vramMemAlign vramAllocAt vramMemAlignAt vramRealloc vramFree \
	linearAlloc linearMemAlign linearRealloc linearFree C3D_TexInitWithParams C3D_RenderTargetCreate C3D_RenderTargetCreateFromTex
LDFLAGS += $(foreach f,$(GPUMEM_WRAPPED),-Wl,--wrap=$(f))
LIBS := -lctru -lm
TARGET := emerald3ds
APP_TITLE := Emerald 3DS
# File names of the release executable (no spaces).
APP_FILE := Emerald3DS
APP_DESCRIPTION := Dual-screen port for Nintendo 3DS
APP_AUTHOR := Zallax
# The release this build is, shown under the title screen (3ds_bottom_ui.c);
# tools/build_release.py refuses a --version that differs.
APP_VERSION := 0.4.0
# 1: this 3DSX carries its game data in its own RomFS (development builds).
# The release target always builds an executable without it.
EMBED_DATA ?= 1
# 0 chooses at runtime (loose files, then embedded data, then the data pack);
# 1 = RomFS, 2 = loose files, 3 = data pack.
DATA_BACKEND ?= 0
PYTHON ?= python
HOSTCC ?= gcc
ROOT := ..
SCRIPTS := ../tools/port_common
EXESUFFIX := $(if $(filter Windows_NT,$(OS)),.exe,)
SOURCES := src/main_3ds.c src/3ds_log.c src/3ds_platform.c src/3ds_input.c src/3ds_input_decode.c src/3ds_fs.c src/3ds_audio.c src/3ds_icons_ptbr.c
SOURCES += src/3ds_data.c src/3ds_pak.c src/3ds_settings.c src/3ds_prof.c src/3ds_gpumem.c src/3ds_scenario.c
SOURCES += src/3ds_video.c src/3ds_video_decode.c src/3ds_bottom_screen.c
# The Pokédex on the top screen (include/3ds_dex_top.h).
SOURCES += src/3ds_dex_top.c
# Local wireless play (include/3ds_local.h).
SOURCES += src/3ds_local.c
SOURCES += src/3ds_daynight.c
SOURCES += src/3ds_clock_store.c
# The optional Nuzlocke tracker's state and save-bound record (3ds_nuzlocke_core.h).
SOURCES += src/3ds_nuzlocke_core.c src/3ds_nuzlocke_store.c
SOURCES += src/3ds_encounters.c
# The part of the overworld on screen, for the map objects.
SOURCES += src/3ds_field_view.c
LIBS := -lcitro2d -lcitro3d $(LIBS)
OBJECTS := $(patsubst src/%.c,build/%.o,$(SOURCES))
GAMEFLAGS := -DPORTABLE -DPLATFORM_3DS -D__3DS__ -DMODERN=1 -DUBFIX -D__INTELLISENSE__
GAMEFLAGS += -iquote include -iquote ../include -iquote ../include/constants -iquote ..
# Baked clock-driven lighting and contact shadows, targeting Old 3DS.
VOXEL_LIGHTING ?= 1
ifneq ($(VOXEL_LIGHTING),0)
ifneq ($(VOXEL_LIGHTING),1)
$(error VOXEL_LIGHTING must be 0 or 1)
endif
endif
CPPFLAGS += -DCTR_VOXEL_LIGHTING=$(VOXEL_LIGHTING)
GAMEFLAGS += -DCTR_VOXEL_LIGHTING=$(VOXEL_LIGHTING)
# The voxel world per eye with the 3D slider: on a New 3DS unless set.
# VOXEL_STEREO=1 forces it on (an Old 3DS, Azahar), =0 off.
ifdef VOXEL_STEREO
CPPFLAGS += -DCTR_VOXEL_STEREO=$(VOXEL_STEREO)
endif
# FORCE_SLIDER=1.0 fixes the 3D slider for an emulator that has none.
ifdef FORCE_SLIDER
CPPFLAGS += -DCTR_FORCE_SLIDER=$(FORCE_SLIDER)f
endif
# TEST_SWOOP=frame (Azahar aid for the swoop between the voxel world and the
# 2D field, 3ds_game_full.c): from that frame on the field, the 3D option off
# and on again, then a warp indoors and back out; with FRAME_DUMP=1 and
# DUMP_SWOOP=1 every frame the swoop plays is written to frames.raw.
ifdef TEST_SWOOP
GAMEFLAGS += -DCTR_TEST_SWOOP=$(TEST_SWOOP)
endif
ifdef FRAME_DUMP
CPPFLAGS += -DCTR_FRAME_DUMP=1
ifdef DUMP_SWOOP
CPPFLAGS += -DCTR_DUMP_SWOOP=1
endif
endif
# A small FPS counter in the top-left corner of the top screen, shown when
# the bottom screen's options turn it on (off by default).
SHOW_FPS ?= 1
ifneq ($(SHOW_FPS),0)
ifneq ($(SHOW_FPS),1)
$(error SHOW_FPS must be 0 or 1)
endif
endif
CPPFLAGS += -DCTR_SHOW_FPS=$(SHOW_FPS)
GAMEFLAGS += -DCTR_SHOW_FPS=$(SHOW_FPS)
# The port's own interface: en follows the game's language, pt_br shows it in
# Brazilian Portuguese over the English game (include/3ds_locale.h).
PORT_LANG ?= en
ifeq ($(PORT_LANG),pt_br)
CTR_LANG_PT_BR := 1
else ifeq ($(PORT_LANG),en)
CTR_LANG_PT_BR := 0
else
$(error PORT_LANG must be en or pt_br)
endif
CPPFLAGS += -DCTR_LANG_PT_BR=$(CTR_LANG_PT_BR)
GAMEFLAGS += -DCTR_LANG_PT_BR=$(CTR_LANG_PT_BR)
CPPFLAGS += -DCTR_DATA_BACKEND=$(DATA_BACKEND)
# Legacy code has existing warnings; platform code remains -Werror.
GAMECFLAGS := $(filter-out -Werror,$(CFLAGS)) -fsigned-char -fno-strict-aliasing
GAMECFLAGS += -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration
# Optional per-machine settings (e.g. SAMPLE_OVERLAY_DIR), never committed.
#
# Debug-only defines with no option of their own: off in every normal build.
# Turn one on in local.mk, CPPFLAGS for the port's native files and GAMEFLAGS
# for the game and bridge ones (each line says which), e.g.
#   CPPFLAGS += -DCTR_AUDIO_DUMP
# CPPFLAGS:
#   CTR_AUDIO_DUMP          3ds_audio.c: the mixer's output, 600-2400 frames, to sdmc:/3ds/emerald3ds/audio.raw.
#   CTR_STEREO_ZERO         3ds_video.c: the battle's 3D depths all 0 (the stereo path, no displacement).
#   CTR_DUMP_OBJWIN         3ds_video.c, with FRAME_DUMP=1: frames with the OBJ window up to frames.raw.
#   CTR_DUMP_BATTLE         3ds_video.c, with FRAME_DUMP=1: battle frames to frames.raw;
#                           CTR_DUMP_FROM=n with CTR_DUMP_STEP=m picks from which battle frame, every m.
#   CTR_GPU_EARLY=0         3ds_video.c: the GPU is not started early within a frame (on by default).
#   CTR_VRAM_TRUST=0        3ds_video.c: the field compares all of VRAM, not trusting announced writes.
#   CTR_VOXEL_TRACE=1       voxel/ctr_voxel.c: logs the first 48 chunk builds.
# GAMEFLAGS:
#   CTR_BOTTOM_CACHE_AUDIT  3ds_bottom_ui.c: a kept bottom screen is drawn anyway, a difference logged.
#   CTR_SPRITE_ID_AUDIT     3ds_sprite_ids.c: logs a sprite that keeps its id with another template.
#   CTR_TEST_BATTLE=frame   3ds_game_full.c: a wild Zigzagoon battle once the field is up at that frame.
#   CTR_MEASURE_AID         3ds_game_full.c: runs the warps and battles listed in measure.txt.
#   CTR_TEST_EXT_PALETTE    3ds_game_full.c: Azahar check of the ext palettes with a party icon.
#   PORT_HEAP_CHECK         ../src/malloc.c: CheckHeap walks the heap again.
# TEST_FLAGS (3ds_cycle_test.c only, touch it to rebuild):
#   CTR_BATTLE_TEST=SPECIES_x, CTR_BATTLE_TEST_LEVEL=n, CTR_BATTLE_TEST_TRAINER=TRAINER_x: a battle on demand.
-include local.mk
include full.mk
OBJECTS += $(FULL_OBJECTS)

# What the boot log says this executable is (3ds_prof.c): the commit, whether
# the tree differed from it and a digest of the difference, the libraries and
# the build options. Its own object, rewritten only when one of them changes,
# so a commit relinks without recompiling anything else.
BUILDINFO_PATHS := . ../src ../include ../data ../constants ':!*.md'
BUILDINFO_DEFINES := $(sort $(filter -D%,$(CPPFLAGS) $(GAMEFLAGS)))
OBJECTS += build/buildinfo.o
build/buildinfo.c: FORCE
	@mkdir -p build
	@sha=$$(git rev-parse HEAD 2>/dev/null || echo unknown); \
	branch=$$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown); \
	dirty=$$(git status --porcelain -- $(BUILDINFO_PATHS) 2>/dev/null | wc -l | tr -d ' '); \
	diff=$$({ git diff HEAD -- $(BUILDINFO_PATHS); git ls-files -o --exclude-standard -- $(BUILDINFO_PATHS) | xargs -r git hash-object --; } 2>/dev/null | git hash-object --stdin 2>/dev/null | cut -c1-12); \
	libs=""; for lib in ctru citro3d citro2d; do \
		libs="$$libs $$lib=$$(git hash-object $(CTRULIB)/lib/lib$$lib.a 2>/dev/null | cut -c1-12)"; done; \
	{ printf '/* Generated by the Makefile (build/buildinfo.c). */\n'; \
	  printf 'const char gCtrBuildVersion[] = "%s";\n' '$(APP_VERSION)'; \
	  printf 'const char gCtrBuildCommit[] = "%s";\n' "$$sha"; \
	  printf 'const char gCtrBuildBranch[] = "%s";\n' "$$branch"; \
	  printf 'const unsigned gCtrBuildDirty = %s;\n' "$${dirty:-0}"; \
	  printf 'const char gCtrBuildDiff[] = "%s";\n' "$${diff:-none}"; \
	  printf 'const char gCtrBuildLibs[] = "%s";\n' "$${libs# }"; \
	  printf 'const char gCtrBuildDefines[] = "%s";\n' '$(subst ",\\",$(BUILDINFO_DEFINES))'; \
	  printf 'const char gCtrBuildCompiler[] = __VERSION__;\n'; } > $@.new
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new

build/buildinfo.o: build/buildinfo.c
	$(CC) $(CFLAGS) -c $< -o $@

# Changing a stage/ABI flag cannot accidentally reuse objects from an older build.
.PHONY: FORCE
$(OBJECTS): build/config
build/config: FORCE
	@mkdir -p build
	@printf '%s\n' '$(CPPFLAGS) $(CFLAGS) $(GAMEFLAGS) $(GAMECFLAGS) $(LDFLAGS) $(LIBS)' > build/config.new
	@cmp -s build/config.new $@ || cp build/config.new $@
	@rm -f build/config.new

.PHONY: all clean verify release pak devdata
all: $(TARGET).3dsx

build/%.o: src/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# The linker script and specs are generated from emerald3ds.ld.in: the image
# link keeps .gamedata's contents, the shipped executable reserves it NOLOAD.
build/emerald3ds_image.ld: emerald3ds.ld.in
	@mkdir -p build
	sed 's/@GAMEDATA_TYPE@//' $< > $@
build/emerald3ds_release.ld: emerald3ds.ld.in
	@mkdir -p build
	sed 's/@GAMEDATA_TYPE@/(NOLOAD)/' $< > $@
build/emerald3ds_%.specs: build/emerald3ds_%.ld Makefile
	printf '%%include <sync-dmb.specs>\n\n*link:\n+ -T %s -d --emit-relocs --use-blx --gc-sections\n\n*startfile:\n3dsx_crt0%%O%%s crti%%O%%s crtbegin%%O%%s\n' '$<' > $@

# One object per song puts the link command well past what Windows will accept
# on a command line, so the object list goes through a response file.
# A fresh id for every link, in both links (the image's layout must match):
# the log and perf.bin name the executable by it. A hash of the code cannot:
# the console's 3DSX loader lays the segments out differently from the ELF,
# and the addresses the code holds differ with it.
build/linkid.c: $(OBJECTS)
	@printf 'const char gCtrLinkId[] = "E3ID-%s";\n' "$$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')" > $@
build/linkid.o: build/linkid.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJECTS) build/linkid.o build/emerald3ds_release.specs
	@mkdir -p build
	$(file >build/link.rsp,$(OBJECTS) build/linkid.o)
	$(CC) $(LDFLAGS) -specs=build/emerald3ds_release.specs -Wl,-Map,build/$(TARGET).map @build/link.rsp $(LIBS) -o $@

build/gamedata_image.elf: $(OBJECTS) build/linkid.o build/emerald3ds_image.specs
	@mkdir -p build
	$(file >build/link.rsp,$(OBJECTS) build/linkid.o)
	$(CC) $(LDFLAGS) -specs=build/emerald3ds_image.specs -Wl,-Map,build/gamedata_image.map @build/link.rsp $(LIBS) -o $@

# 3dsxtool builds its own relocation table from the ELF's; the NOLOAD region is
# relocated by the bundle loader instead, so its table stays out.
build/$(TARGET)_3dsx.elf: $(TARGET).elf
	$(DEVKITARM)/bin/arm-none-eabi-objcopy --remove-section=.rel.gamedata $< $@

$(TARGET).smdh: Makefile
	$(DEVKITPRO)/tools/bin/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" assets/artwork/icon.png $@

ROMFS_DATA_OUTS := $(ROMFS_ASSET_OUTS) $(ROMFS_MAP_OUT) $(ROMFS_SCRIPT_OUTS) $(ROMFS_SOUND_OUTS) \
	$(ROMFS_SHADER_OUTS) $(ROMFS_GAMEDATA_OUTS)

# The ABI digests every game-data file, so a data pack is accepted only by the
# executable it was generated for.
romfs/engine/abi.bin: $(ROMFS_DATA_OUTS) $(SCRIPTS)/staging.py
	"$(PYTHON)" $(SCRIPTS)/staging.py abi --romfs romfs

# Local wireless play links only identical builds (src/3ds_local.c): a SHA-256
# of the executable without its debug information and of the data's ABI.
romfs/engine/build_id.bin: build/$(TARGET)_3dsx.elf romfs/engine/abi.bin scripts/gen_build_id.py
	$(DEVKITARM)/bin/arm-none-eabi-objcopy --strip-debug $< build/$(TARGET)_id.elf
	"$(PYTHON)" scripts/gen_build_id.py build/$(TARGET)_id.elf romfs/engine/abi.bin $@

ifeq ($(EMBED_DATA),1)
EMBED_MARKER := romfs/data.embedded
romfs/data.embedded:
	printf 'embedded\n' > $@
else
EMBED_MARKER :=
$(shell rm -f romfs/data.embedded)
endif

# The follower sprites and texts (../builder/emerald3ds_builder/followers.py),
# optional data outside the ABI: from the private workspace's copy, never the
# network, and only where that copy is (the public repository has none: there
# is no follower and the visible wild Pokemon keep their icons). staging.py
# keeps them out of a release.
FOLLOWERS_SRC ?= ../private_assets/hgss_followers
FOLLOWERS_PY := ../builder/emerald3ds_builder/followers.py
ifneq ($(wildcard $(FOLLOWERS_SRC)/pokeheartgold),)
ROMFS_FOLLOWERS := romfs/followers/sources.txt
$(ROMFS_FOLLOWERS): $(FOLLOWERS_PY) ../builder/emerald3ds_builder/followers_sources.py
	"$(PYTHON)" $(FOLLOWERS_PY) --sources $(FOLLOWERS_SRC) --out romfs --offline
else
ROMFS_FOLLOWERS :=
endif

$(TARGET).3dsx: build/$(TARGET)_3dsx.elf $(TARGET).smdh romfs/boot.txt $(ROMFS_DATA_OUTS) romfs/engine/abi.bin romfs/engine/build_id.bin $(EMBED_MARKER) \
	$(ROMFS_FOLLOWERS)
	$(DEVKITPRO)/tools/bin/3dsxtool $< $@ --smdh=$(TARGET).smdh --romfs=romfs

# Release: the executable with engine files only. Its game data is the data
# pack the player's builder generates.
release: build/$(TARGET)_3dsx.elf $(TARGET).smdh romfs/boot.txt $(ROMFS_DATA_OUTS) romfs/engine/abi.bin romfs/engine/build_id.bin
	"$(PYTHON)" $(SCRIPTS)/staging.py engine --romfs romfs --out build/romfs_release
	@mkdir -p dist
	$(DEVKITPRO)/tools/bin/3dsxtool build/$(TARGET)_3dsx.elf dist/$(APP_FILE).3dsx \
		--smdh=$(TARGET).smdh --romfs=build/romfs_release
	cp $(TARGET).smdh dist/$(APP_FILE).smdh
	"$(PYTHON)" $(SCRIPTS)/staging.py list --romfs romfs > dist/staging-classification.txt

# The data pack, built from this tree's own staging: the release path, tested
# without the builder.
pak: romfs/engine/abi.bin
	"$(PYTHON)" $(SCRIPTS)/staging.py pak --romfs romfs --out build/emerald3ds.pak --game-config $(ROOT)/include/constants/global.h

# Loose game data for sdmc:/3ds/emerald3ds/devdata/ (see tools/dev_assets.py).
devdata: romfs/engine/abi.bin
	"$(PYTHON)" $(SCRIPTS)/staging.py devdata --romfs romfs --out build/devdata

verify: all verify-input
	"$(PYTHON)" scripts/verify_build.py --devkitarm "$(DEVKITARM)" --stage 9 --video 1 --full 1

.PHONY: verify-input
verify-input: build/input_test.exe
	./build/input_test.exe

build/input_test.exe: tests/input_test.c src/3ds_input_decode.c include/3ds_input.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude tests/input_test.c src/3ds_input_decode.c -o $@

verify: verify-video
verify: verify-daynight
verify: verify-clock-store
verify: verify-nuzlocke
verify: verify-night-windows
verify: verify-night-lamps
verify: verify-encounters
verify: verify-field-view verify-field-fade-hold verify-voxel-overlay-depth

.PHONY: verify-field-fade-hold verify-voxel-overlay-depth
verify-field-fade-hold:
	"$(PYTHON)" tests/field_fade_hold_test.py --cc "$(HOSTCC)"

verify-voxel-overlay-depth:
	"$(PYTHON)" tests/voxel_overlay_depth_test.py --cc "$(HOSTCC)"

.PHONY: verify-logo-ptbr
verify: verify-logo-ptbr
verify-logo-ptbr:
	"$(PYTHON)" tests/logo_ptbr_test.py --cc "$(HOSTCC)"

.PHONY: verify-rng-seeding
verify: verify-rng-seeding
.PHONY: verify-battle-speed
verify: verify-battle-speed
verify-battle-speed: build/battle_speed_test$(EXESUFFIX)
	./build/battle_speed_test$(EXESUFFIX)

build/battle_speed_test$(EXESUFFIX): tests/battle_speed_test.c src/3ds_settings.c include/3ds_platform.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=gnu99 -O2 -Wall -Wextra -Werror -Itests/settings_host -Iinclude $< -pthread -o $@
verify-rng-seeding:
	"$(PYTHON)" tests/rng_seeding_test.py --cc "$(HOSTCC)"

.PHONY: verify-bottom-taps
verify: verify-bottom-taps
verify-bottom-taps:
	"$(PYTHON)" tests/bottom_taps_test.py --cc "$(HOSTCC)"
.PHONY: verify-field-view
verify-field-view: build/field_view_test$(EXESUFFIX)
	./build/field_view_test$(EXESUFFIX)

build/field_view_test$(EXESUFFIX): tests/field_view_test.c src/3ds_field_view.c include/3ds_field_view.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -DCTR_FIELD_VIEW_HOST -Iinclude tests/field_view_test.c src/3ds_field_view.c -lm -o $@

.PHONY: verify-encounters
verify-encounters: build/encounters_test.exe
	./build/encounters_test.exe

build/encounters_test.exe: tests/encounters_test.c src/3ds_encounters.c include/3ds_encounters.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude tests/encounters_test.c src/3ds_encounters.c -o $@

.PHONY: verify-daynight
.PHONY: verify-night-windows
.PHONY: verify-night-lamps
verify-night-lamps: build/voxel_lamps_test$(EXESUFFIX)
	./build/voxel_lamps_test$(EXESUFFIX)

build/voxel_lamps_test$(EXESUFFIX): tests/voxel_lamps_test.c src/voxel/voxel_sign.c src/voxel/voxel_sign.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude -Isrc/voxel -DVOXEL_HOST_FILES -DSIGN_MASK_PATH='"build/voxel_lamps_fixture.bin"' tests/voxel_lamps_test.c src/voxel/voxel_sign.c -lm -o $@

verify-night-windows:
	"$(PYTHON)" tests/voxel_night_test.py --cc "$(HOSTCC)" --assets romfs/voxel/buildings.bin

verify-daynight: build/daynight_test.exe
	./build/daynight_test.exe
	"$(PYTHON)" tests/clock_sync_settings_test.py --cc "$(HOSTCC)"

.PHONY: verify-clock-store
verify-clock-store: build/clock_store_test$(EXESUFFIX)
	./build/clock_store_test$(EXESUFFIX)

build/clock_store_test$(EXESUFFIX): tests/clock_store_test.c src/3ds_clock_store.c src/3ds_daynight.c src/3ds_pak.c include/3ds_clock_store.h include/3ds_daynight.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Drename=CtrTestRename -Iinclude tests/clock_store_test.c src/3ds_clock_store.c src/3ds_daynight.c src/3ds_pak.c -lm -o $@

.PHONY: verify-nuzlocke
verify-nuzlocke: build/nuzlocke_test$(EXESUFFIX)
	./build/nuzlocke_test$(EXESUFFIX)

build/nuzlocke_test$(EXESUFFIX): tests/nuzlocke_test.c src/3ds_nuzlocke_core.c src/3ds_nuzlocke_store.c include/3ds_nuzlocke_core.h include/3ds_nuzlocke_store.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Drename=CtrTestRename -Iinclude tests/nuzlocke_test.c src/3ds_nuzlocke_core.c src/3ds_nuzlocke_store.c -o $@

build/daynight_test.exe: tests/daynight_test.c src/3ds_daynight.c include/3ds_daynight.h include/3ds_platform.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude tests/daynight_test.c src/3ds_daynight.c -lm -o $@

.PHONY: verify-video
verify-video: build/video_decode_test.exe
	./build/video_decode_test.exe

build/video_decode_test.exe: tests/video_decode_test.c src/3ds_video_decode.c include/3ds_video.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude tests/video_decode_test.c src/3ds_video_decode.c -o $@

verify: verify-randomizer
.PHONY: verify-randomizer
verify-randomizer: build/randomizer_test$(EXESUFFIX)
	./build/randomizer_test$(EXESUFFIX)

# Hash guards and edit positions must match the complete patched English tree.
verify: verify-randomizer-locales
.PHONY: verify-randomizer-locales
verify-randomizer-locales:
	$(PYTHON) tests/randomizer_locale_test.py --tree $(ROOT)

build/randomizer_test$(EXESUFFIX): tests/randomizer_test.c src/3ds_randomizer_core.c include/3ds_randomizer.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude tests/randomizer_test.c src/3ds_randomizer_core.c -o $@

verify: verify-pak
.PHONY: verify-pak
verify-pak: build/pak_test.exe tests/make_test_pak.py
	./build/pak_test.exe build/test.pak $$("$(PYTHON)" tests/make_test_pak.py build/test.pak)

build/pak_test.exe: tests/pak_test.c src/3ds_pak.c include/3ds_pak.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude tests/pak_test.c src/3ds_pak.c -o $@

clean:
	rm -rf build dist $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh

-include $(OBJECTS:.o=.d)
