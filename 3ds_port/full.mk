# The whole game tree for ARM11, with resources outside the executable.
#
# Graphics and map payloads leave the executable through link-time stubs and
# are resolved at runtime (3ds_assets.c, 3ds_map_loader.c); script bytecode and
# MP2K song data, whose pointers sit at unaligned offsets, are bundled into
# reserved regions and relocated at load (scripts/ctr_bundle.py). Map headers,
# the asset index and the sound bank stay resident.
#
# Game translation units are built in ARM state, like the native backend: GCC
# has no Thumb-1 hard-float VFP ABI on ARMv6K, and libctru is hard-float.

PREPROC := $(abspath $(ROOT)/tools/preproc/preproc$(EXESUFFIX))
MAPJSON := $(abspath $(ROOT)/tools/mapjson/mapjson$(EXESUFFIX))
WAV2AGB := $(abspath $(ROOT)/tools/wav2agb/wav2agb$(EXESUFFIX))
GAME_ARCH := $(ARCH)
ASM_PSEUDO_OP_CONV := sed -e 's/\.4byte/\.int/g;s/\.2byte/\.short/g'
AS := $(DEVKITARM)/bin/arm-none-eabi-as
CPPTOOL := $(DEVKITARM)/bin/arm-none-eabi-cpp
ASFLAGS := -mcpu=mpcore --defsym MODERN=1 --defsym PORTABLE=1 --defsym UBFIX=1 -I$(ROOT)

# Game translation units see the port bridge (compat/port_platform.h): the
# hooks in the shared tree that route resources, scripts and saves to the
# ARM11 backend are compiled in with PORT_BRIDGE.
BRIDGE_FLAGS := -DPORT_BRIDGE

# The compile runs from the repository root (preproc resolves charmap and
# include paths there), so every search path is absolute.
FULL_INCLUDES := -iquote $(abspath compat) -iquote $(abspath include) -iquote $(abspath $(ROOT)/include)
FULL_INCLUDES += -iquote $(abspath $(ROOT)/include/constants) -iquote $(abspath $(ROOT))
FULLFLAGS := $(filter-out -iquote include -iquote ../include -iquote ../include/constants -iquote ..,$(GAMEFLAGS)) $(FULL_INCLUDES) $(BRIDGE_FLAGS)
FULLCFLAGS := $(GAME_ARCH) $(filter-out $(ARCH),$(GAMECFLAGS))

ROOT_C_SRCS := $(wildcard $(ROOT)/src/*.c $(ROOT)/src/*/*.c $(ROOT)/src/*/*/*.c)
# GBA-only link/multiboot programs; 3ds_compat.c provides their entry points.
ROOT_C_SRCS := $(filter-out $(ROOT)/src/berry_fix_program.c $(ROOT)/src/ereader_screen.c $(ROOT)/src/mystery_gift_menu.c $(ROOT)/src/mystery_event_menu.c,$(ROOT_C_SRCS))

ROOT_DATA_SRCS := $(wildcard $(ROOT)/data/*.s)
ROOT_DATA_SRCS := $(filter-out $(ROOT)/data/multiboot_berry_glitch_fix.s $(ROOT)/data/multiboot_ereader.s \
	$(ROOT)/data/multiboot_pokemon_colosseum.s $(ROOT)/data/mystery_gift.s,$(ROOT_DATA_SRCS))
# maps.s is regenerated with externalised layout payloads.
ROOT_DATA_SRCS := $(filter-out $(ROOT)/data/maps.s,$(ROOT_DATA_SRCS))
# Script sections are assembled but not linked: their pointers are unaligned,
# so they move to RomFS and land in a reserved region (gen_script_bundle.py).
SCRIPT_DATA_SRCS := battle_ai_scripts battle_anim_scripts battle_scripts_1 battle_scripts_2 	contest_ai_scripts event_scripts field_effect_scripts
SCRIPT_DATA_OBJS := $(patsubst %,build/root/data/%.o,$(SCRIPT_DATA_SRCS))
ROOT_DATA_SRCS := $(filter-out $(patsubst %,$(ROOT)/data/%.s,$(SCRIPT_DATA_SRCS)),$(ROOT_DATA_SRCS))

# Voice groups, the song table and the samples come from data/sound_data.s; the
# song headers themselves are one object per tune, referenced from that table.
ROOT_MID_SRCS := $(patsubst %.mid,%.s,$(wildcard $(ROOT)/sound/songs/midi/*.mid))
ROOT_SONG_SRCS := $(wildcard $(ROOT)/sound/songs/*.s) $(ROOT_MID_SRCS)
ROOT_SONG_OBJS := $(patsubst $(ROOT)/%.s,build/root/%.o,$(ROOT_SONG_SRCS))
ROOT_SOUND_WAV_SRCS := $(wildcard $(ROOT)/sound/direct_sound_samples/*.wav $(ROOT)/sound/direct_sound_samples/*/*.wav)
ROOT_SOUND_BIN_SRCS := $(patsubst %.wav,%.bin,$(ROOT_SOUND_WAV_SRCS))
# Optional: a directory of replacement sample .bin files (same names as
# sound/direct_sound_samples/*.bin). Each file found there is assembled in place
# of the original sample. Empty by default: the build uses the original samples.
SAMPLE_OVERLAY_DIR ?=
SAMPLE_OVERLAY := $(if $(SAMPLE_OVERLAY_DIR),$(wildcard $(abspath $(SAMPLE_OVERLAY_DIR))/*.bin))

FULL_C_OBJS := $(patsubst $(ROOT)/%.c,build/root/%.o,$(ROOT_C_SRCS))
FULL_DATA_OBJS := $(patsubst $(ROOT)/%.s,build/root/%.o,$(ROOT_DATA_SRCS)) build/root/data/maps.o
FULL_DATA_OBJS += build/root/3ds_script_blob.o build/root/3ds_script_keep.o
# The song objects are assembled but not linked, for the same reason the script
# sections are not: a PATT command puts a 32-bit pointer at whatever offset the
# preceding bytes leave it on, and 3dsxtool rejects unaligned relocations.
FULL_DATA_OBJS += build/root/3ds_song_blob.o
BACKEND_SRCS := src/3ds_assets.c src/3ds_map_loader.c src/3ds_compat.c
BACKEND_SRCS += src/3ds_game_full.c src/3ds_game_bridge.c src/3ds_script_loader.c
BACKEND_SRCS += src/3ds_bottom_ui.c
build/bridge/3ds_bottom_ui.o: FULLCFLAGS += -DCTR_APP_VERSION='"$(APP_VERSION)"'
# The ENHANCEMENTS and CHEATS pages of OPTIONS (include/3ds_extras.h).
BACKEND_SRCS += src/3ds_extras.c
BACKEND_SRCS += src/3ds_field_icon.c src/3ds_follower.c src/3ds_follower_trail.c
# The frame profiler times the engine's own routines through linker wrappers.
BACKEND_SRCS += src/3ds_prof_wrap.c
PROF_WRAPPED := RunTasks AnimateSprites BuildOamBuffer ProcessDma3Requests \
	LZ77UnCompWram LZ77UnCompVram RLUnCompWram RLUnCompVram CpuSet CpuFastSet DmaSet UpdatePaletteFade
# The sound engine runs on a worker core; its entry points take the worker's
# lock (src/3ds_sound.c).
BACKEND_SRCS += src/3ds_sound.c
SOUND_WRAPPED := m4aSoundVSync m4aSoundMain m4aSoundInit m4aSoundMode m4aSoundVSyncOn m4aSoundVSyncOff \
	m4aSongNumStart m4aSongNumStartOrChange m4aSongNumStartOrContinue m4aSongNumStop m4aSongNumContinue \
	m4aMPlayAllStop m4aMPlayAllContinue m4aMPlayStop m4aMPlayContinue m4aMPlayFadeOut \
	m4aMPlayFadeOutTemporarily m4aMPlayFadeIn m4aMPlayImmInit m4aMPlayTempoControl \
	m4aMPlayVolumeControl m4aMPlayPitchControl m4aMPlayPanpotControl m4aMPlayModDepthSet \
	m4aMPlayLFOSpeedSet MPlayStart MPlayContinue MPlayFadeOut SetPokemonCryVolume \
	SetPokemonCryPanpot SetPokemonCryPitch SetPokemonCryLength SetPokemonCryRelease \
	SetPokemonCryProgress SetPokemonCryChorus SetPokemonCryStereo SetPokemonCryPriority \
	SetPokemonCryTone IsPokemonCryPlaying
LDFLAGS += $(foreach f,$(PROF_WRAPPED) $(SOUND_WRAPPED),-Wl,--wrap=$(f))
BACKEND_OBJS := $(patsubst src/%.c,build/bridge/%.o,$(BACKEND_SRCS))

# The voxel overworld is split along the same SDK boundary as the rest of the
# port: the map, camera, atlas and geometry modules are game translation units
# (they read gMapHeader and resolve RomFS payloads, and never see libctru),
# while ctr_voxel.c is native and owns the Citro3D objects.
ifeq ($(VOXEL),1)
VOXEL_GAME_SRCS := src/voxel/voxel_world.c src/voxel/voxel_camera.c \
	src/voxel/voxel_atlas.c src/voxel/voxel_mesh_builder.c src/voxel/voxel_entities.c \
	src/voxel/voxel_regions.c src/voxel/voxel_tree.c src/voxel/voxel_sign.c \
	src/voxel/voxel_building.c src/voxel/voxel_relief.c \
	src/voxel/voxel_arena.c src/voxel/voxel_grade.c src/voxel/voxel_battle.c
ifeq ($(VOXEL_LIGHTING),1)
VOXEL_GAME_SRCS += src/voxel/voxel_lighting.c
endif
VOXEL_GAME_OBJS := $(patsubst src/voxel/%.c,build/voxel/%.o,$(VOXEL_GAME_SRCS))
BACKEND_OBJS += $(VOXEL_GAME_OBJS) build/ctr_voxel.o
ROMFS_SHADER_OUTS := romfs/shaders/voxel.shbin romfs/voxel/regions.bin
ROMFS_SHADER_OUTS += romfs/voxel/trees.rgba5551
ROMFS_SHADER_OUTS += romfs/voxel/signposts.bin
ROMFS_SHADER_OUTS += romfs/voxel/buildings.bin
ROMFS_SHADER_OUTS += romfs/voxel/relief.bin
endif
# New art around the intro's leaves scene (scripts/gen_intro_margins.py).
ROMFS_SHADER_OUTS += romfs/stage/leaves.bin
# The bottom screen's battle menus: the port's own art (scripts/gen_battle_art.py).
ROMFS_SHADER_OUTS += romfs/bottom/battle.bin

FULL_OBJECTS := $(FULL_C_OBJS) $(FULL_DATA_OBJS) $(BACKEND_OBJS)
# The keep table is only referenced from the payload, which is not linked,
# so it has to be a garbage-collection root or it takes the handlers with it.
LDFLAGS += -Wl,--undefined=__ctr_script_keep

ROMFS_ASSET_OUTS := romfs/assets/asset_index.bin romfs/assets/asset_map.txt \
	romfs/assets/asset_ptr_index.bin romfs/assets/asset_ptr_map.txt
ROMFS_MAP_OUT := romfs/maps/layouts.bin
ROMFS_SCRIPT_OUTS := romfs/scripts/scripts.bin romfs/scripts/scripts.rel
ROMFS_SOUND_OUTS := romfs/sound/songs.bin romfs/sound/songs.rel
ROMFS_GAMEDATA_OUTS := romfs/gamedata/gamedata.bin romfs/gamedata/gamedata.rel

$(FULL_OBJECTS): build/config

build/root/%.o: $(ROOT)/%.c Makefile full.mk
	@mkdir -p $(dir $@)
	cd $(ROOT) && "$(CC)" $(FULLFLAGS) $(FULLCFLAGS) -E -x c $(patsubst $(ROOT)/%,%,$<) \
		| "$(PREPROC)" -i $(patsubst $(ROOT)/%,%,$<) charmap.txt \
		| "$(CC)" $(FULLFLAGS) $(FULLCFLAGS) -x c -c -o $(CURDIR)/$@ -

# Screens staged as one whole GBA picture: GBA geometry, and their VBlank
# callbacks reported to the bridge (compat/ctr_gba_stage.h).
CTR_GBA_STAGE_SRCS := intro title_screen credits intro_credits_graphics
CTR_GBA_STAGE_OBJS := $(patsubst %,build/root/src/%.o,$(CTR_GBA_STAGE_SRCS))
$(CTR_GBA_STAGE_OBJS): FULLCFLAGS += -DCTR_GBA_STAGE -include $(abspath compat/ctr_gba_stage.h)
$(CTR_GBA_STAGE_OBJS): compat/ctr_gba_stage.h $(ROOT)/include/gba/defines.h
# GBA screens shown centred, margins only from layers that wrap on the GBA
# (compat/ctr_gba_centred.h).
CTR_GBA_CENTRED_SRCS := region_map field_region_map main_menu naming_screen wallclock \
	pokemon_summary_screen starter_choose shop
CTR_GBA_CENTRED_OBJS := $(patsubst %,build/root/src/%.o,$(CTR_GBA_CENTRED_SRCS))
$(CTR_GBA_CENTRED_OBJS): FULLCFLAGS += -DCTR_GBA_STAGE -include $(abspath compat/ctr_gba_centred.h)
$(CTR_GBA_CENTRED_OBJS): compat/ctr_gba_centred.h $(ROOT)/include/gba/defines.h
# Which centred screen each of those is, for how its margins are filled.
build/root/src/main_menu.o: FULLCFLAGS += -DCTR_CENTRED_MAIN_MENU
build/root/src/naming_screen.o: FULLCFLAGS += -DCTR_CENTRED_NAMING
build/root/src/wallclock.o: FULLCFLAGS += -DCTR_CENTRED_CLOCK
build/root/src/starter_choose.o: FULLCFLAGS += -DCTR_CENTRED_STARTER
build/root/src/shop.o: FULLCFLAGS += -DCTR_CENTRED_SHOP
# On the bottom screen, as the PC's boxes it is mostly opened from.
build/root/src/pokemon_summary_screen.o: FULLCFLAGS += -DCTR_CENTRED_SUMMARY
# The PokéNav: every screen of it laid out for the GBA screen, shown whole on
# the bottom screen. pokenav.c installs all of its VBlank callbacks.
CTR_GBA_POKENAV_SRCS := $(patsubst $(ROOT)/src/%.c,%,$(wildcard $(ROOT)/src/pokenav*.c))
CTR_GBA_POKENAV_OBJS := $(patsubst %,build/root/src/%.o,$(CTR_GBA_POKENAV_SRCS))
$(CTR_GBA_POKENAV_OBJS): FULLCFLAGS += -DCTR_GBA_STAGE
$(CTR_GBA_POKENAV_OBJS): $(ROOT)/include/gba/defines.h
build/root/src/pokenav.o: FULLCFLAGS += -DCTR_CENTRED_POKENAV -include $(abspath compat/ctr_gba_centred.h)
build/root/src/pokenav.o: compat/ctr_gba_centred.h
# The PC's boxes: laid out for the GBA screen, shown on the bottom screen too.
build/root/src/pokemon_storage_system.o: FULLCFLAGS += -DCTR_GBA_STAGE -DCTR_CENTRED_STORAGE \
	-include $(abspath compat/ctr_gba_centred.h)
build/root/src/pokemon_storage_system.o: compat/ctr_gba_centred.h $(ROOT)/include/gba/defines.h
# The bag as the game draws it, on the bottom screen: left of the column when
# opened from the field, over the whole screen from a battle, a shop, the PC.
build/root/src/item_menu.o: FULLCFLAGS += -DCTR_GBA_STAGE -DCTR_CENTRED_BAG \
	-include $(abspath compat/ctr_gba_centred.h)
build/root/src/item_menu.o: compat/ctr_gba_centred.h $(ROOT)/include/gba/defines.h
# The party menu as the game draws it, on the bottom screen: left of the
# column from the field, over the whole screen from a battle, a contest or a
# facility.
build/root/src/party_menu.o: FULLCFLAGS += -DCTR_GBA_STAGE -DCTR_CENTRED_PARTY \
	-include $(abspath compat/ctr_gba_centred.h)
build/root/src/party_menu.o: compat/ctr_gba_centred.h $(ROOT)/include/gba/defines.h
# The Pokédex as the game draws it, left of the column: its list, search,
# entries, area, cry and size screens. pokedex.c installs their VBlank
# callbacks.
CTR_GBA_DEX_OBJS := $(patsubst %,build/root/src/%.o,pokedex pokedex_area_screen pokedex_area_region_map \
	pokedex_cry_screen)
$(CTR_GBA_DEX_OBJS): FULLCFLAGS += -DCTR_GBA_STAGE
$(CTR_GBA_DEX_OBJS): $(ROOT)/include/gba/defines.h
build/root/src/pokedex.o: FULLCFLAGS += -DCTR_CENTRED_POKEDEX -include $(abspath compat/ctr_gba_centred.h)
build/root/src/pokedex.o: compat/ctr_gba_centred.h
# The battle scene: one 240x160 composition whose sprites, windows, scanline
# tables and BG pages are all laid out for the GBA screen, shown 1:1 and filled
# out to the whole top screen by the compositor (compat/ctr_gba_battle.h).
# The evolution scene is the battle's: its text box, its backgrounds and the
# mon at the middle of a 240x160 screen. Built as a wide screen it was drawn
# off the left of the top screen, half of the mon on a black screen.
CTR_GBA_BATTLE_SRCS := battle_main battle_bg battle_intro battle_interface battle_gfx_sfx_util \
	battle_controllers battle_controller_player battle_controller_opponent \
	battle_controller_link_opponent battle_controller_link_partner battle_controller_player_partner \
	battle_controller_recorded_opponent battle_controller_recorded_player battle_controller_safari \
	battle_controller_wally battle_message battle_script_commands battle_util battle_util2 \
	battle_tv battle_arena battle_palace battle_ai_script_commands battle_ai_switch_items \
	pokeball reshow_battle_screen evolution_scene evolution_graphics \
	$(patsubst $(ROOT)/src/%.c,%,$(wildcard $(ROOT)/src/battle_anim*.c))
CTR_GBA_BATTLE_OBJS := $(patsubst %,build/root/src/%.o,$(CTR_GBA_BATTLE_SRCS))
$(CTR_GBA_BATTLE_OBJS): FULLCFLAGS += -DCTR_GBA_STAGE -include $(abspath compat/ctr_gba_battle.h)
$(CTR_GBA_BATTLE_OBJS): compat/ctr_gba_battle.h $(ROOT)/include/gba/defines.h
# The battle transitions: GBA windows and scanline tables, shown over the field
# at the battle scene's scale (compat/ctr_gba_transition.h).
CTR_GBA_TRANSITION_SRCS := battle_transition battle_transition_frontier
CTR_GBA_TRANSITION_OBJS := $(patsubst %,build/root/src/%.o,$(CTR_GBA_TRANSITION_SRCS))
$(CTR_GBA_TRANSITION_OBJS): FULLCFLAGS += -DCTR_GBA_STAGE -include $(abspath compat/ctr_gba_transition.h)
$(CTR_GBA_TRANSITION_OBJS): compat/ctr_gba_transition.h $(ROOT)/include/gba/defines.h
# preproc truncates this translation unit to zero bytes (a known tool bug also
# hit by the DS build). It has no charmap string literals, so it is safe to skip.
build/root/src/field_camera.o: $(ROOT)/src/field_camera.c Makefile full.mk
	@mkdir -p $(dir $@)
	"$(CC)" $(FULLFLAGS) $(FULLCFLAGS) -c $< -o $@

build/root/data/maps.o: build/maps.s build/map_layouts.inc $(ROOT)/data/layouts/layouts_table.inc \
		$(ROOT)/data/maps/headers.inc $(ROOT)/data/maps/groups.inc $(ROOT)/data/maps/connections.inc
	@mkdir -p $(dir $@)
	cd $(ROOT) && "$(PREPROC)" 3ds_port/build/maps.s charmap.txt | "$(CPPTOOL)" -Iinclude -I. - \
		| "$(PREPROC)" -ie 3ds_port/build/maps.s charmap.txt | $(ASM_PSEUDO_OP_CONV) - \
		| "$(AS)" $(ASFLAGS) -o $(CURDIR)/$@

build/root/data/%.o: $(ROOT)/data/%.s Makefile full.mk
	@mkdir -p $(dir $@)
	cd $(ROOT) && "$(PREPROC)" data/$*.s charmap.txt | "$(CPPTOOL)" -Iinclude -I. - \
		| "$(PREPROC)" -ie data/$*.s charmap.txt | $(ASM_PSEUDO_OP_CONV) - \
		| "$(AS)" $(ASFLAGS) -o $(CURDIR)/$@

# The samples are .incbin'd from paths relative to the repository root, and the
# generic data rule already assembles from there. Only the dependency is new,
# unless a sample overlay redirects some of the .incbin paths.
ifeq ($(SAMPLE_OVERLAY),)
build/root/data/sound_data.o: $(ROOT_SOUND_BIN_SRCS)
else
SAMPLE_OVERLAY_SED := $(foreach f,$(SAMPLE_OVERLAY),-e 's\#"sound/direct_sound_samples/\(cries/\)\{0,1\}$(notdir $(f))"\#"$(f)"\#')
build/root/data/sound_data.o: $(ROOT)/data/sound_data.s $(ROOT_SOUND_BIN_SRCS) $(SAMPLE_OVERLAY) Makefile full.mk
	@mkdir -p $(dir $@)
	cd $(ROOT) && "$(PREPROC)" data/sound_data.s charmap.txt | "$(CPPTOOL)" -Iinclude -I. - 		| "$(PREPROC)" -ie data/sound_data.s charmap.txt | $(ASM_PSEUDO_OP_CONV) - 		| sed $(SAMPLE_OVERLAY_SED) | "$(AS)" $(ASFLAGS) -o $(CURDIR)/$@
endif

build/root/sound/songs/%.o: $(ROOT)/sound/songs/%.s
	@mkdir -p $(dir $@)
	$(ASM_PSEUDO_OP_CONV) $< | "$(AS)" $(ASFLAGS) -I$(ROOT)/sound -o $@

# mid2agb takes per-song options from sound/songs/midi/midi.cfg, which the root
# Makefile already parses. Duplicating that parsing here would be a second place
# for the options to drift, so the conversion is delegated to it - in one call
# for every song, because each recursive make parses the whole root Makefile.
ROOT_MID_DEPS := $(wildcard $(ROOT)/sound/songs/midi/*.mid) $(ROOT)/sound/songs/midi/midi.cfg
build/midi.stamp: $(ROOT_MID_DEPS)
	+$(MAKE) -C $(ROOT) $(patsubst $(ROOT)/%,%,$(ROOT_MID_SRCS))
	@mkdir -p build
	@touch $@
$(ROOT_MID_SRCS): build/midi.stamp ;
.SECONDARY: $(ROOT_MID_SRCS)

# Per-map header/events/connections includes, generated by mapjson for any map
# that does not have them yet (a fresh upstream tree has none).
build/map_includes.stamp: $(wildcard $(ROOT)/data/maps/*/map.json) $(ROOT)/data/layouts/layouts.json
	"$(PYTHON)" $(SCRIPTS)/gen_missing_map_includes.py
	@mkdir -p build
	@touch $@
build/root/data/map_events.o build/root/data/maps.o: build/map_includes.stamp

# Sample conversion, with the flags the root audio_rules.mk uses.
$(WAV2AGB): $(wildcard $(ROOT)/tools/wav2agb/*.cpp $(ROOT)/tools/wav2agb/*.h)
	$(MAKE) -C $(ROOT)/tools/wav2agb

$(ROOT)/sound/direct_sound_samples/cries/%.bin: $(ROOT)/sound/direct_sound_samples/cries/%.wav | $(WAV2AGB)
	"$(WAV2AGB)" -b -c -l 1 --no-pad $< $@

$(ROOT)/sound/direct_sound_samples/%.bin: $(ROOT)/sound/direct_sound_samples/%.wav | $(WAV2AGB)
	"$(WAV2AGB)" -b $< $@

build/root/data/map_events.o: $(ROOT)/data/map_events.s $(ROOT)/data/maps/events.inc
build/root/data/event_scripts.o: $(ROOT)/data/script_cmd_table.inc $(ROOT)/data/specials.inc
build/root/data/battle_anim_scripts.o: $(ROOT)/asm/macros/battle_anim_script.inc $(ROOT)/include/constants/battle_anim.h

build/bridge/%.o: src/%.c Makefile full.mk include/3ds_assets.h compat/port_platform.h
	@mkdir -p $(dir $@)
	"$(CC)" $(FULLFLAGS) $(FULLCFLAGS) -MMD -MP -c $< -o $@

build/voxel/%.o: src/voxel/%.c Makefile full.mk
	@mkdir -p $(dir $@)
	"$(CC)" $(FULLFLAGS) $(FULLCFLAGS) -iquote $(abspath src/voxel) -MMD -MP -c $< -o $@

# Native translation unit: libctru headers, no game headers, -Werror like the
# rest of the platform code.
build/ctr_voxel.o: src/voxel/ctr_voxel.c Makefile full.mk
	@mkdir -p $(dir $@)
	"$(CC)" $(CPPFLAGS) $(CFLAGS) -Isrc/voxel -MMD -MP -c $< -o $@

romfs/shaders/voxel.shbin: src/voxel/voxel.v.pica
	@mkdir -p $(dir $@)
	$(DEVKITPRO)/tools/bin/picasso -o $@ $<

build/maps.s build/map_layouts.inc $(ROMFS_MAP_OUT) &: $(SCRIPTS)/gen_map_data.py \
		$(ROOT)/data/maps.s $(ROOT)/data/layouts/layouts.inc
	@mkdir -p build romfs/maps
	"$(PYTHON)" $(SCRIPTS)/gen_map_data.py --port-dir . --fs-dir romfs

# The asset index maps the linker's INCBIN stub addresses to RomFS files, so it
# is generated from the finished ELF and does not feed back into the link.
$(ROMFS_ASSET_OUTS) &: $(TARGET).elf $(SCRIPTS)/gen_asset_table.py
	@mkdir -p romfs/assets
	"$(PYTHON)" $(SCRIPTS)/gen_asset_table.py --port-dir . --map build/$(TARGET).map \
		--fs-dir romfs --uri-prefix ""
	"$(PYTHON)" scripts/verify_assets.py --fs-dir romfs

# The reserved region must exist before the link; the payload needs the link.
build/3ds_script_blob.s build/3ds_script_keep.s &: $(SCRIPT_DATA_OBJS) scripts/gen_script_bundle.py 		scripts/ctr_bundle.py
	"$(PYTHON)" scripts/gen_script_bundle.py --port-dir . --blob

build/3ds_song_blob.s: $(ROOT_SONG_OBJS) scripts/gen_sound_bundle.py scripts/ctr_bundle.py
	"$(PYTHON)" scripts/gen_sound_bundle.py --port-dir . --blob

build/root/3ds_script_%.o: build/3ds_script_%.s
	@mkdir -p $(dir $@)
	"$(AS)" $(ASFLAGS) -o $@ $<

build/root/3ds_song_blob.o: build/3ds_song_blob.s
	@mkdir -p $(dir $@)
	"$(AS)" $(ASFLAGS) -o $@ $<

$(ROMFS_SCRIPT_OUTS) &: $(TARGET).elf $(SCRIPT_DATA_OBJS) scripts/gen_script_bundle.py
	@mkdir -p romfs/scripts
	"$(PYTHON)" scripts/gen_script_bundle.py --port-dir . --bundle --out-dir romfs/scripts
	"$(PYTHON)" scripts/verify_scripts.py --elf $(TARGET).elf --out-dir romfs/scripts

$(ROMFS_SOUND_OUTS) &: $(TARGET).elf $(ROOT_SONG_OBJS) scripts/gen_sound_bundle.py
	@mkdir -p romfs/sound
	"$(PYTHON)" scripts/gen_sound_bundle.py --port-dir . --bundle --out-dir romfs/sound
	"$(PYTHON)" scripts/verify_scripts.py --elf $(TARGET).elf --out-dir romfs/sound 		--tag verify_songs --magic C3AB --payload songs.bin --reloc songs.rel 		--blob-symbol __ctr_song_blob --require mus_dummy --require mus_littleroot

# The game's linked read-only data: identical layout in both links, contents
# from the image, reserved NOLOAD in the executable.
$(ROMFS_GAMEDATA_OUTS) &: build/gamedata_image.elf $(TARGET).elf scripts/gen_gamedata_bundle.py scripts/ctr_bundle.py
	"$(PYTHON)" scripts/gen_gamedata_bundle.py --image build/gamedata_image.elf --elf $(TARGET).elf \
		--out-dir romfs/gamedata

# Host world/atlas fixtures include global.h's generated map constants.
$(MAPJSON): $(wildcard $(ROOT)/tools/mapjson/*.cpp $(ROOT)/tools/mapjson/*.h)
	$(MAKE) -C $(ROOT)/tools/mapjson

$(ROOT)/include/constants/map_groups.h: $(ROOT)/data/maps/map_groups.json $(wildcard $(ROOT)/data/maps/*/map.json) | $(MAPJSON)
	"$(MAPJSON)" groups emerald $< $(ROOT)/data/maps $(ROOT)/include/constants

$(ROOT)/include/constants/layouts.h: $(ROOT)/data/layouts/layouts.json | $(MAPJSON)
	"$(MAPJSON)" layouts emerald $< $(ROOT)/data/layouts $(ROOT)/include/constants

HOST_MAP_HEADERS := $(ROOT)/include/constants/map_groups.h $(ROOT)/include/constants/layouts.h

.PHONY: map-includes
map-includes:
	"$(PYTHON)" $(SCRIPTS)/gen_missing_map_includes.py

-include $(BACKEND_OBJS:.o=.d)

# Voxel generators read converted tiles/palettes directly. On a fresh checkout
# those files do not exist until the asset stage, which runs after the link.
# Build the shared inputs before any generator (one recursive make, not one
# process per palette). Include the intro image for the same reason.
VOXEL_GFX_SOURCES := $(wildcard $(ROOT)/data/tilesets/primary/*/tiles.png $(ROOT)/data/tilesets/secondary/*/tiles.png)
VOXEL_GFX_SOURCES += $(wildcard $(ROOT)/data/tilesets/primary/*/palettes/*.pal $(ROOT)/data/tilesets/secondary/*/palettes/*.pal)
VOXEL_GFX_SOURCES += $(wildcard $(ROOT)/graphics/intro/scene_1/bg.png)
VOXEL_GFX_OUTPUTS := $(patsubst %.png,%.4bpp,$(filter %.png,$(VOXEL_GFX_SOURCES)))
VOXEL_GFX_OUTPUTS += $(patsubst %.pal,%.gbapal,$(filter %.pal,$(VOXEL_GFX_SOURCES)))
ifneq ($(strip $(VOXEL_GFX_OUTPUTS)),)
$(VOXEL_GFX_OUTPUTS) &: $(VOXEL_GFX_SOURCES)
	+$(MAKE) -C $(ROOT) $(patsubst $(ROOT)/%,%,$(VOXEL_GFX_OUTPUTS))
romfs/voxel/regions.bin romfs/voxel/signposts.bin romfs/voxel/buildings.bin romfs/voxel/relief.bin romfs/stage/leaves.bin: $(VOXEL_GFX_OUTPUTS)
endif

ifeq ($(VOXEL),1)
# What every cell of every layout IS (the console reads its signposts).
# Solved on the host because it needs the map's warps, its neighbours and a
# look at every metatile's own pixels, and none of that fits in a frame.
# Depends on the header because the generator parses the role names out of it
# rather than keeping a copy that could drift.
romfs/voxel/regions.bin: scripts/gen_voxel_regions.py scripts/voxel_cells.py scripts/voxel_art.py \
		src/voxel/voxel_regions.h \
		$(ROOT)/data/layouts/layouts.json
	@mkdir -p $(@D)
	"$(PYTHON)" scripts/gen_voxel_regions.py

romfs/voxel/signposts.bin: scripts/gen_voxel_sign_masks.py scripts/voxel_sign_mask.py \
		scripts/voxel_cells.py scripts/voxel_art.py romfs/voxel/regions.bin
	@mkdir -p $(@D)
	"$(PYTHON)" scripts/gen_voxel_sign_masks.py

romfs/voxel/trees.rgba5551: scripts/gen_voxel_trees.py \
		assets/voxel/trees/tree_crown.png assets/voxel/trees/tree_trunk.png \
		assets/voxel/trees/tree_small_crown.png assets/voxel/trees/tree_small_trunk.png 		assets/voxel/trees/tree_2_small_crown.png assets/voxel/trees/tree_2_small_trunk.png
	@mkdir -p $(@D)
	"$(PYTHON)" scripts/gen_voxel_trees.py --output $@

# Buildings modelled from their own drawing. The generator renders every model
# in the GBA's projection and refuses to write one that differs from its art
# by a single pixel, so a spec that stops matching fails the build here
# (--verify: the release builder only exports, checked by the CRC of this
# file). verify-voxel-buildings runs the proofs alone, even when the file is
# up to date: the release build asks for it.
romfs/voxel/buildings.bin: scripts/gen_voxel_buildings.py scripts/voxel_building.py \
		scripts/voxel_building_specs.py scripts/dump_region_art.py scripts/voxel_props.py \
		scripts/voxel_relief_fixes.py assets/voxel/relief_fixes.json \
		$(ROOT)/data/layouts/layouts.json
	@mkdir -p $(@D)
	"$(PYTHON)" scripts/gen_voxel_buildings.py --verify --output $@

.PHONY: verify-voxel-buildings
verify-voxel-buildings:
	"$(PYTHON)" scripts/gen_voxel_buildings.py --verify

# Terrain relief read off the drawing: gen_voxel_relief.py explains it.
romfs/voxel/relief.bin: scripts/gen_voxel_relief.py scripts/voxel_cells.py scripts/voxel_props.py \
		scripts/voxel_art.py scripts/voxel_building.py scripts/dump_region_art.py \
		scripts/voxel_relief_fixes.py scripts/voxel_terraces.py assets/voxel/relief_fixes.json \
		$(ROOT)/data/layouts/layouts.json
	@mkdir -p $(@D)
	"$(PYTHON)" scripts/gen_voxel_relief.py --output $@

romfs/bottom/battle.bin: scripts/gen_battle_art.py
	@mkdir -p $(@D)
	"$(PYTHON)" scripts/gen_battle_art.py --output $@

romfs/stage/leaves.bin: scripts/gen_intro_margins.py $(ROOT)/graphics/intro/scene_1/bg.4bpp \
		$(wildcard $(ROOT)/graphics/intro/scene_1/bg?_map.bin)
	@mkdir -p $(@D)
	"$(PYTHON)" scripts/gen_intro_margins.py --output $@

HOST_VOXEL_DEFS := -D'PORT_LOG(...)=((void)0)' -DVOXEL_HOST_FILES

.PHONY: verify-voxel-trees
verify: verify-voxel-world
.PHONY: verify-voxel-world
verify-voxel-world: build/voxel_world_hash_test.exe
	./build/voxel_world_hash_test.exe

build/voxel_world_hash_test.exe: tests/voxel_world_hash_test.c src/voxel/voxel_world.c src/voxel/voxel_world.h $(HOST_MAP_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=gnu99 -O2 -DPORTABLE -DMODERN=1 -D__INTELLISENSE__ \
		-iquote compat -iquote ../include -Isrc/voxel tests/voxel_world_hash_test.c -o $@

verify: verify-voxel-arena
.PHONY: verify-voxel-arena
verify-voxel-arena: build/voxel_arena_test.exe
	./build/voxel_arena_test.exe

build/voxel_arena_test.exe: tests/voxel_arena_test.c src/voxel/voxel_arena.c src/voxel/voxel_arena.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror tests/voxel_arena_test.c -o $@

verify: verify-voxel-atlas
.PHONY: verify-voxel-atlas
verify-voxel-atlas: build/voxel_atlas_test.exe
	./build/voxel_atlas_test.exe

build/voxel_atlas_test.exe: tests/voxel_atlas_test.c src/voxel/voxel_atlas.c src/voxel/voxel_atlas.h \
        src/3ds_video_decode.c src/voxel/voxel_grade.c src/voxel/voxel_grade.h $(HOST_MAP_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=gnu99 -O2 -DPORTABLE -DMODERN=1 -D__INTELLISENSE__ \
		-iquote compat -iquote ../include -Iinclude -Isrc/voxel tests/voxel_atlas_test.c src/voxel/voxel_grade.c -o $@

verify: verify-voxel-trees
verify-voxel-trees: build/voxel_tree_test.exe
	./build/voxel_tree_test.exe

build/voxel_tree_test.exe: tests/voxel_tree_test.c src/voxel/voxel_tree.c \
		src/voxel/voxel_mesh_builder.c src/voxel/voxel_sign.c \
		src/voxel/voxel_tree.h src/voxel/voxel_mesh_builder.h \
		src/voxel/voxel_world.h src/voxel/voxel_atlas.h src/voxel/voxel_regions.h \
		src/voxel/voxel_building.c src/voxel/voxel_relief.c src/voxel/voxel_grade.c src/3ds_video_decode.c
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude -Isrc/voxel \
		tests/voxel_tree_test.c src/voxel/voxel_tree.c \
		src/voxel/voxel_mesh_builder.c src/voxel/voxel_sign.c \
		src/voxel/voxel_building.c src/voxel/voxel_relief.c src/voxel/voxel_grade.c src/3ds_video_decode.c $(HOST_VOXEL_DEFS) -lm -o $@

.PHONY: verify-voxel-lighting
verify: verify-voxel-lighting
verify-voxel-lighting: build/voxel_lighting_on_test.exe build/voxel_lighting_off_test.exe
	./build/voxel_lighting_on_test.exe
	./build/voxel_lighting_off_test.exe

LIGHTING_TEST_SRCS := tests/voxel_lighting_test.c src/voxel/voxel_lighting.c src/voxel/voxel_sign.c \
	src/voxel/voxel_mesh_builder.c src/voxel/voxel_tree.c src/voxel/voxel_relief.c
LIGHTING_TEST_HEADERS := src/voxel/voxel_lighting.h src/voxel/voxel_mesh_builder.h \
	src/voxel/voxel_world.h src/voxel/voxel_regions.h src/voxel/voxel_atlas.h src/voxel/voxel_tree.h \
	src/voxel/voxel_building.h
build/voxel_lighting_on_test.exe: $(LIGHTING_TEST_SRCS) $(LIGHTING_TEST_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -DCTR_VOXEL_LIGHTING=1 -DVOXEL_LIGHTING_TESTS -Isrc/voxel \
		$(HOST_VOXEL_DEFS) $(LIGHTING_TEST_SRCS) -lm -o $@
build/voxel_lighting_off_test.exe: $(LIGHTING_TEST_SRCS) $(LIGHTING_TEST_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -DCTR_VOXEL_LIGHTING=0 -Isrc/voxel \
		$(HOST_VOXEL_DEFS) $(LIGHTING_TEST_SRCS) -lm -o $@

.PHONY: verify-voxel-structures
verify: verify-voxel-structures
verify-voxel-structures: build/voxel_ground_mesh_test.exe build/voxel_sign_test.exe \
		romfs/voxel/regions.bin romfs/voxel/signposts.bin
	./build/voxel_ground_mesh_test.exe
	./build/voxel_sign_test.exe romfs/voxel/signposts.bin
	"$(PYTHON)" -B tests/voxel_signpost_test.py

GROUND_TEST_SRCS := tests/voxel_ground_mesh_test.c \
	src/voxel/voxel_mesh_builder.c src/voxel/voxel_tree.c src/voxel/voxel_sign.c \
	src/voxel/voxel_building.c src/voxel/voxel_relief.c src/voxel/voxel_grade.c src/3ds_video_decode.c
build/voxel_ground_mesh_test.exe: $(GROUND_TEST_SRCS) $(LIGHTING_TEST_HEADERS) src/voxel/voxel_sign.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude -Isrc/voxel $(HOST_VOXEL_DEFS) \
		-DVOXEL_BUILDINGS_PATH=\"romfs/voxel/buildings.bin\" $(GROUND_TEST_SRCS) -lm -o $@

build/voxel_sign_test.exe: tests/voxel_sign_test.c src/voxel/voxel_sign.c src/voxel/voxel_sign.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude -Isrc/voxel \
		-DVOXEL_HOST_FILES -DSIGN_MASK_PATH='"romfs/voxel/signposts.bin"' \
		tests/voxel_sign_test.c src/voxel/voxel_sign.c -o $@

# The mountains' geometry against what the projection allows, every drawn
# group (scripts/voxel_relief_check.py), compared with the run before.
.PHONY: check-voxel-relief
check-voxel-relief:
	"$(PYTHON)" scripts/voxel_relief_check.py --images build/relief_check \
		--against build/relief_check.pkl --save build/relief_check.pkl

.PHONY: verify-voxel-relief
verify: verify-voxel-relief
verify-voxel-relief: build/voxel_relief_test.exe romfs/voxel/relief.bin
	./build/voxel_relief_test.exe $$("$(PYTHON)" -c "import json; l = json.load(open('$(ROOT)/data/layouts/layouts.json'))['layouts']; ids = [e['id'] for e in l]; print(ids.index('LAYOUT_ROUTE101') + 1, ids.index('LAYOUT_ROUTE116') + 1)")

build/voxel_relief_test.exe: tests/voxel_relief_test.c src/voxel/voxel_relief.c src/voxel/voxel_relief.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude -Isrc/voxel $(HOST_VOXEL_DEFS) \
		-DVOXEL_RELIEF_PATH='"romfs/voxel/relief.bin"' \
		tests/voxel_relief_test.c src/voxel/voxel_relief.c -lm -o $@
endif

.PHONY: verify-settings
verify: verify-settings
verify-settings: build/settings_shutdown_test.exe
	./build/settings_shutdown_test.exe
	./build/settings_shutdown_test.exe 1
	./build/settings_shutdown_test.exe 2

build/settings_shutdown_test.exe: tests/settings_shutdown_test.c tests/settings_host/3ds.h src/3ds_settings.c
	@mkdir -p $(@D)
	$(HOSTCC) -std=gnu99 -O2 -Wall -Wextra -Werror -Itests/settings_host -Iinclude $< -pthread -o $@
.PHONY: verify-voxel-runtime
verify: verify-voxel-runtime
verify-voxel-runtime:
	"$(PYTHON)" tests/voxel_runtime_test.py --cc "$(HOSTCC)"

.PHONY: verify-voxel-draft
verify: verify-voxel-draft
verify-voxel-draft:
	"$(PYTHON)" -B tests/voxel_draft_test.py --cc "$(HOSTCC)"

.PHONY: verify-bottom-map
verify: verify-bottom-map
verify-bottom-map:
	"$(PYTHON)" -B tests/bottom_map_test.py --cc "$(HOSTCC)"

.PHONY: verify-voxel-regions
verify: verify-voxel-regions
verify-voxel-regions: romfs/voxel/regions.bin
	"$(PYTHON)" -B tests/voxel_regions_test.py --cc "$(HOSTCC)"

# The follower's path has no game/SDK dependency and is checked on the host.
verify: verify-follower
.PHONY: verify-follower
verify-follower: build/follower_trail_test.exe build/follower_runtime_test.exe build/follower_voxel_1_test.exe build/follower_voxel_0_test.exe
	./build/follower_trail_test.exe
	./build/follower_runtime_test.exe
	./build/follower_voxel_1_test.exe
	./build/follower_voxel_0_test.exe
build/follower_trail_test.exe: tests/follower_trail_test.c src/3ds_follower_trail.c include/3ds_follower_trail.h
	@mkdir -p build
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Iinclude tests/follower_trail_test.c src/3ds_follower_trail.c -o $@

build/follower_runtime_test.exe: tests/follower_runtime_test.c src/3ds_follower.c src/3ds_field_icon.c src/3ds_follower_trail.c include/3ds_follower_trail.h include/3ds_field_icon.h include/3ds_follower.h $(ROOT)/src/sprite.c build/follower_movement_test.hostobj build/follower_metatile_test.hostobj
	@mkdir -p build
	$(HOSTCC) -std=gnu99 -O2 -Wall -Wextra -Werror -Wno-ignored-qualifiers -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
		-D_FORTIFY_SOURCE=0 -DPORTABLE -DMODERN=1 -DUBFIX -D__INTELLISENSE__ -DPLATFORM_3DS -DPORT_BRIDGE \
		-iquote compat -iquote $(ROOT)/include -iquote $(ROOT) -Iinclude -ffunction-sections -fdata-sections $(FOLLOWER_HOST_GC) \
		tests/follower_runtime_test.c src/3ds_follower.c src/3ds_field_icon.c src/3ds_follower_trail.c build/follower_movement_test.hostobj build/follower_metatile_test.hostobj -o $@

# Link the real game collision/elevation code, keeping unrelated game functions out.
build/follower_movement_test.hostobj: $(ROOT)/src/event_object_movement.c include/3ds_follower.h
	@mkdir -p build
	$(HOSTCC) -std=gnu99 -O2 -Wall -Wextra -Werror -Wno-ignored-qualifiers -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
		-Wno-pointer-sign -Wno-missing-field-initializers -Wno-missing-braces -Wno-unused-parameter -Wno-unused-const-variable \
		-Wno-type-limits -Wno-implicit-fallthrough \
		-D_FORTIFY_SOURCE=0 -DPORTABLE -DMODERN=1 -DUBFIX -D__INTELLISENSE__ -DPLATFORM_3DS -DPORT_BRIDGE \
		-DSetSpritePosToMapCoords=CtrTest_UnusedSetSpritePosToMapCoords \
		-iquote compat -iquote $(ROOT)/include -iquote $(ROOT) -Iinclude -ffunction-sections -fdata-sections -c $< -o $@

build/follower_metatile_test.hostobj: $(ROOT)/src/metatile_behavior.c
	@mkdir -p build
	$(HOSTCC) -std=gnu99 -O2 -Wall -Wextra -Werror -Wno-ignored-qualifiers -Wno-unused-parameter \
		-D_FORTIFY_SOURCE=0 -DPORTABLE -DMODERN=1 -D__INTELLISENSE__ \
		-iquote compat -iquote $(ROOT)/include -ffunction-sections -fdata-sections -c $< -o $@

FOLLOWER_HOST_GC := -Wl,--gc-sections
ifeq ($(shell uname -s),Darwin)
FOLLOWER_HOST_GC := -Wl,-dead_strip
endif
build/follower_voxel_%_test.exe: tests/follower_voxel_test.c src/voxel/voxel_entities.c include/3ds_follower.h
	@mkdir -p build
	$(HOSTCC) -std=gnu99 -O2 -Wall -Wextra -Werror -Wno-ignored-qualifiers -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
		-D_FORTIFY_SOURCE=0 -DPORTABLE -DMODERN=1 -D__INTELLISENSE__ -DPLATFORM_3DS -DPORT_BRIDGE -DCTR_VOXEL_LIGHTING=$* \
		-iquote compat -iquote $(ROOT)/include -Iinclude -Isrc/voxel -ffunction-sections -fdata-sections \
		$(FOLLOWER_HOST_GC) tests/follower_voxel_test.c -lm -o $@
