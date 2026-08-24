TARGET   := naoh_arcade_collection
TITLE_ID := NAOH00001
TITLE    := NAOH™: Arcade Collection

SHELL := /bin/bash

# Point to the main repo, but rename the files with _cps2 to avoid collisions
CORE_DIR := cores/fbalpha2012/svn-current/trunk
CORE_LIB_RAW := $(CORE_DIR)/fbalpha2012_libretro_vita.a
CORE_LIB     := $(CORE_DIR)/fbalpha2012_libretro_vita_renamed.a

MAME_DIR     := cores/mame2000
MAME_LIB_RAW := $(MAME_DIR)/mame2000_libretro_vita.a
MAME_LIB     := $(MAME_DIR)/mame2000_libretro_vita_renamed.a
MAME_SYMS    := $(MAME_DIR)/mame_redefine.syms

ifeq ($(strip $(VITASDK)),)
$(error VITASDK is not set, export VITASDK=<path to>vitasdk)
endif

PREFIX  := $(VITASDK)/bin/arm-vita-eabi
CC      := $(PREFIX)-gcc
CXX     := $(PREFIX)-g++
AR      := $(PREFIX)-ar
OBJCOPY := $(PREFIX)-objcopy
NM      := $(PREFIX)-nm

ARCHFLAGS := -march=armv7-a -mfpu=neon -mfloat-abi=hard -DVITA
CFLAGS    := $(ARCHFLAGS) -O2 -Wall -I$(CORE_DIR)/src/burner/libretro -Ideps
LDFLAGS   := -Wl,-q,--pic-veneer

MAME_ICE_WORKAROUND_CFLAGS := -fno-thread-jumps

VITA_LIBS := -lvita2d -lfreetype -lpng -ljpeg -lz -lbz2 \
             -lSceCommonDialog_stub -lSceAppMgr_stub \
             -lSceDisplay_stub -lSceGxm_stub -lSceSysmodule_stub \
             -lSceCtrl_stub -lSceAudio_stub -lScePower_stub \
             -lScePgf_stub -lScePvf_stub -lm -lc -lSceTouch_stub

RETRO_SYMS := retro_init retro_deinit retro_api_version retro_get_system_info \
              retro_get_system_av_info retro_set_environment retro_set_video_refresh \
              retro_set_audio_sample retro_set_audio_sample_batch retro_set_input_poll \
              retro_set_input_state retro_set_controller_port_device retro_reset retro_run \
              retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset \
              retro_cheat_set retro_load_game retro_load_game_special retro_unload_game \
              retro_get_region retro_get_memory_data retro_get_memory_size

MAME_EXTRA_SYMS := logerror gp2x_printf gp2x_set_video_mode gp2x_video_setpalette \
                   osd_init osd_exit screen_reinit hook_audio_done hook_video_done \
                   retro_save_directory retro_system_directory

FBA_REDEFS := $(foreach s,$(RETRO_SYMS),--redefine-sym=$(s)=fba_$(s))

OBJS := main.o

all: $(TARGET).vpk

# Core build rules: compile without bloat
$(CORE_LIB_RAW):
	$(MAKE) -C $(CORE_DIR) -f makefile.libretro platform=vita

$(MAME_LIB_RAW):
	$(MAKE) -C $(MAME_DIR) -f Makefile platform=vita EXTRA_CFLAGS="$(MAME_ICE_WORKAROUND_CFLAGS)"

$(CORE_LIB): $(CORE_LIB_RAW)
	$(OBJCOPY) $(FBA_REDEFS) $< $@

$(MAME_SYMS): $(CORE_LIB_RAW) $(MAME_LIB_RAW)
	@{ \
		for s in $(RETRO_SYMS) $(MAME_EXTRA_SYMS); do echo "$$s mame_$$s"; done; \
		comm -12 \
			<($(NM) --defined-only -g $(CORE_LIB_RAW) 2>/dev/null | awk 'NF==3{print $$3}' | sort -u) \
			<($(NM) --defined-only -g $(MAME_LIB_RAW) 2>/dev/null | awk 'NF==3{print $$3}' | sort -u) \
		| grep -vxFf <(printf '%s\n' $(RETRO_SYMS) $(MAME_EXTRA_SYMS)) \
		| awk '{print $$0, "mame_"$$0}'; \
	} > $@

$(MAME_LIB): $(MAME_LIB_RAW) $(MAME_SYMS)
	$(OBJCOPY) --redefine-syms=$(MAME_SYMS) $< $@

main.o: main.c deps/stb_vorbis.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) $(CORE_LIB) $(MAME_LIB)
	$(CXX) $(ARCHFLAGS) $(LDFLAGS) $(OBJS) -o $@ \
		-Wl,--start-group $(CORE_LIB) $(MAME_LIB) -Wl,--end-group \
		$(VITA_LIBS)

$(TARGET).velf: $(TARGET).elf
	vita-elf-create $< $@

eboot.bin: $(TARGET).velf
	vita-make-fself -s $< eboot.bin

param.sfo: Makefile
	vita-mksfoex -s TITLE_ID=$(TITLE_ID) "$(TITLE)" param.sfo

ASSET_FILES := $(wildcard assets/*)
ASSET_ARGS  := $(foreach f,$(ASSET_FILES),-a $(f)=$(f))

SCE_SYS_FILES := sce_sys/icon0.png sce_sys/pic0.png \
                  sce_sys/livearea/contents/bg.png \
                  sce_sys/livearea/contents/startup.png \
                  sce_sys/livearea/contents/template.xml
ASSET_ARGS += $(foreach f,$(SCE_SYS_FILES),-a $(f)=$(f))

$(TARGET).vpk: eboot.bin param.sfo $(SCE_SYS_FILES) $(ASSET_FILES)
	rm -f $(TARGET).vpk
	vita-pack-vpk -s param.sfo -b eboot.bin $(ASSET_ARGS) $(TARGET).vpk

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).velf eboot.bin param.sfo $(TARGET).vpk
	rm -f $(CORE_LIB) $(MAME_LIB) $(MAME_SYMS)
	$(MAKE) -C $(CORE_DIR) -f makefile.libretro platform=vita clean
	$(MAKE) -C $(MAME_DIR) -f Makefile platform=vita clean

.PHONY: all clean
