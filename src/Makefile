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
MBEDTLS_DIR  := deps/mbedtls
MBEDTLS_DEFS := -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR) \
                -DMBEDTLS_USER_CONFIG_FILE='"naohac_mbedtls_config.h"'

CFLAGS    := $(ARCHFLAGS) -O2 -Wall -I$(CORE_DIR)/src/burner/libretro -Ideps \
             -I$(VITASDK)/arm-vita-eabi/include/freetype2 $(MBEDTLS_DEFS)
LDFLAGS   := -Wl,-q,--pic-veneer

MAME_ICE_WORKAROUND_CFLAGS := -fno-thread-jumps

# same commit and flags as the vdpm vitaGL package, minus the boot splash:
# its second gxm context crashes Vita3K
VITAGL_DIR    := deps/vitaGL
VITAGL_COMMIT := 3dddc43eff782a7ae7cc4b26498aba58fcf875c2
VITAGL_LIB    := $(VITAGL_DIR)/libvitaGL.a

VITA_LIBS := -lvitaGL -lvitashark -lSceShaccCgExt -lmathneon -ltaihen_stub \
             -lSceShaccCg_stub -lSceKernelDmacMgr_stub \
             -lfreetype -lpng -lz -lbz2 \
             -lSceCommonDialog_stub -lSceAppMgr_stub -lSceAppUtil_stub \
             -lSceNet_stub -lSceNetCtl_stub \
             -lSceDisplay_stub -lSceGxm_stub -lSceSysmodule_stub \
             -lSceCtrl_stub -lSceAudio_stub -lScePower_stub \
             -lm -lc -lSceTouch_stub

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

MBEDTLS_OBJS := $(patsubst %.c,%.o,$(wildcard $(MBEDTLS_DIR)/library/*.c))

OBJS := main.o gfx.o netplay.o mqtt.o rng.o deps/monocypher/monocypher.o $(MBEDTLS_OBJS)

all: $(TARGET).vpk

# Core build rules: compile without bloat
$(CORE_DIR)/makefile.libretro $(MAME_DIR)/Makefile:
	sh cores/fetch.sh

$(CORE_LIB_RAW): | $(CORE_DIR)/makefile.libretro
	$(MAKE) -C $(CORE_DIR) -f makefile.libretro platform=vita

$(MAME_LIB_RAW): | $(MAME_DIR)/Makefile
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

main.o: main.c gfx.h netplay.h deps/stb_vorbis.c | $(CORE_DIR)/makefile.libretro
	$(CC) $(CFLAGS) -c $< -o $@

gfx.o: gfx.c gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

netplay.o: netplay.c netplay.h mqtt.h rng.h deps/monocypher/monocypher.h
	$(CC) $(CFLAGS) -c $< -o $@

mqtt.o: mqtt.c mqtt.h lobby_ca.h $(MBEDTLS_DIR)/naohac_mbedtls_config.h
	$(CC) $(CFLAGS) -c $< -o $@

rng.o: rng.c rng.h
	$(CC) $(CFLAGS) -c $< -o $@

$(MBEDTLS_DIR)/library/%.o: $(MBEDTLS_DIR)/library/%.c $(MBEDTLS_DIR)/naohac_mbedtls_config.h
	$(CC) $(ARCHFLAGS) -O2 $(MBEDTLS_DEFS) -c $< -o $@

$(VITAGL_LIB):
	rm -rf $(VITAGL_DIR)
	git clone -q https://github.com/Rinnegatamante/vitaGL.git $(VITAGL_DIR)
	git -C $(VITAGL_DIR) checkout -q $(VITAGL_COMMIT)
	$(MAKE) -C $(VITAGL_DIR) NO_DEBUG=1 NO_SPLASHSCREEN=1

deps/monocypher/monocypher.o: deps/monocypher/monocypher.c deps/monocypher/monocypher.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) $(CORE_LIB) $(MAME_LIB) $(VITAGL_LIB)
	$(CXX) $(ARCHFLAGS) $(LDFLAGS) -L$(VITAGL_DIR) $(OBJS) -o $@ \
		-Wl,--start-group $(CORE_LIB) $(MAME_LIB) -Wl,--end-group \
		$(VITA_LIBS)

$(TARGET).velf: $(TARGET).elf
	vita-elf-create $< $@

eboot.bin: $(TARGET).velf
	vita-make-fself -s $< eboot.bin

param.sfo: Makefile
	vita-mksfoex -s TITLE_ID=$(TITLE_ID) "$(TITLE)" param.sfo

# the repo only has placeholder art, point this at a folder with the real
# assets/ and sce_sys/ to pack those instead (relative path, no spaces)
ASSET_ROOT ?= .

ASSET_FILES := $(notdir $(wildcard $(ASSET_ROOT)/assets/*))
ASSET_ARGS  := $(foreach f,$(ASSET_FILES),-a $(ASSET_ROOT)/assets/$(f)=assets/$(f))

SCE_SYS_FILES := sce_sys/icon0.png sce_sys/pic0.png \
                  sce_sys/livearea/contents/bg.png \
                  sce_sys/livearea/contents/startup.png \
                  sce_sys/livearea/contents/template.xml
ASSET_ARGS += $(foreach f,$(SCE_SYS_FILES),-a $(ASSET_ROOT)/$(f)=$(f))

# always repacked, it's quick and ASSET_ROOT can change between builds
$(TARGET).vpk: eboot.bin param.sfo FORCE
	rm -f $(TARGET).vpk
	vita-pack-vpk -s param.sfo -b eboot.bin $(ASSET_ARGS) $(TARGET).vpk

FORCE:

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).velf eboot.bin param.sfo $(TARGET).vpk
	rm -f $(CORE_LIB) $(MAME_LIB) $(MAME_SYMS)
	$(MAKE) -C $(CORE_DIR) -f makefile.libretro platform=vita clean
	$(MAKE) -C $(MAME_DIR) -f Makefile platform=vita clean

.PHONY: all clean FORCE
