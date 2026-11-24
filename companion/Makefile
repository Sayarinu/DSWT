# DS Companion: a DSi-mode BlocksDS ROM, ARM9 code with a stock ARM7 core.
# Adapted from BlocksDS's rom_arm9 template (CC0-1.0). Build it through
# scripts/build-companion.sh, which runs it in tools/companion-builder.
#
#   make TLS_DIR=<Mbed TLS source> CONNECTION_HEADER=<configured header>
#
# Every output goes under BUILDDIR, so the sources may be read-only.

BLOCKSDS		?= /opt/blocksds/core
WONDERFUL_TOOLCHAIN	?= /opt/wonderful
ARM_NONE_EABI_PATH	?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/

NAME			:= dswt-companion
GAME_TITLE		:= DSWT Companion
GAME_SUBTITLE		:= Wonder Trade
GAME_AUTHOR		:= DSWT
GAME_ICON		:= $(BLOCKSDS)/sys/icon.gif

BUILDDIR		?= build
ROM			?= $(BUILDDIR)/$(NAME).nds
# scripts/prepare-tls.py prints this directory.
TLS_DIR			?= ../game/mbedtls-4.1.1
# scripts/configure-connection.py writes this header from a profile.
CONNECTION_HEADER	?= $(BUILDDIR)/dswt-connection.h

# The ARM7 core only starts the hardware; Wi-Fi comes in a later change.
ARM7ELF			:= $(BLOCKSDS)/sys/arm7/main_core/arm7_minimal.elf

# The game-independent client core, compiled in place. The Nitro-only
# flash.c, network.c and entropy_game.c and the per-game folders stay out.
# boot.c stays out until the companion's game adapter supplies WtGame_*
# (LUNE-869).
CLIENT			:= ../client
CORE_SOURCES		:= $(addprefix $(CLIENT)/src/wonder_trade/,\
			   engine.c journal.c wire.c http.c tls.c seed.c \
			   recovery.c entropy.c pokemon_codec.c)

# The same Mbed TLS sources and include paths as the ROM builds
# (client/src/wonder_trade/meson.build).
TLS_SOURCE_DIRS		:= library tf-psa-crypto/core tf-psa-crypto/extras \
			   tf-psa-crypto/platform tf-psa-crypto/utilities \
			   tf-psa-crypto/drivers/builtin/src
TLS_INCLUDE_DIRS	:= include library tf-psa-crypto/include \
			   tf-psa-crypto/core tf-psa-crypto/dispatch \
			   tf-psa-crypto/extras tf-psa-crypto/platform \
			   tf-psa-crypto/utilities \
			   tf-psa-crypto/drivers/builtin/include \
			   tf-psa-crypto/drivers/builtin/src \
			   tf-psa-crypto/drivers/p256-m \
			   tf-psa-crypto/drivers/pqcp/include \
			   tf-psa-crypto/drivers/everest/include
TLS_SOURCES		:= $(sort $(foreach dir,$(TLS_SOURCE_DIRS),\
			   $(wildcard $(TLS_DIR)/$(dir)/*.c)))

APP_SOURCES		:= $(shell find src -name '*.c' | sort)

PREFIX			:= $(ARM_NONE_EABI_PATH)arm-none-eabi-
CC			:= $(PREFIX)gcc
NDSTOOL			:= $(BLOCKSDS)/tools/ndstool/ndstool

ARCH			:= -mthumb -mcpu=arm946e-s+nofp
SPECS			:= $(BLOCKSDS)/sys/crts/ds_arm9.specs
DEFINES			:= -D__NDS__ -D__BLOCKSDS__ -DARM9 \
			   -DWT_GAME_FAMILY=WT_FAMILY_GEN4 \
			   -DMBEDTLS_CONFIG_FILE='"wonder_trade/tls_config.h"' \
			   -DTF_PSA_CRYPTO_CONFIG_FILE='"wonder_trade/crypto_config.h"'
INCLUDES		:= -Isrc -I$(CLIENT)/include \
			   $(addprefix -I$(TLS_DIR)/,$(TLS_INCLUDE_DIRS)) \
			   -I$(BLOCKSDS)/libs/libnds/include
BASE_CFLAGS		:= $(ARCH) -std=gnu11 -O2 -ffunction-sections \
			   -fdata-sections -specs=$(SPECS) $(DEFINES) $(INCLUDES)
LDFLAGS			:= $(ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib
LDLIBS			:= -Wl,--start-group $(BUILDDIR)/libwt_tls.a -lnds9 -lc -Wl,--end-group

APP_OBJS		:= $(patsubst %.c,$(BUILDDIR)/app/%.o,$(APP_SOURCES))
CORE_OBJS		:= $(patsubst $(CLIENT)/src/wonder_trade/%.c,$(BUILDDIR)/core/%.o,$(CORE_SOURCES))
TLS_OBJS		:= $(patsubst $(TLS_DIR)/%.c,$(BUILDDIR)/tls/%.o,$(TLS_SOURCES))
TLS_LIB			:= $(BUILDDIR)/libwt_tls.a
ELF			:= $(BUILDDIR)/$(NAME).elf
LINK_CHECK		:= $(BUILDDIR)/link-check.elf

ifeq ($(VERBOSE),1)
V			:=
else
V			:= @
endif

.PHONY: all clean check-inputs

all: $(ROM)

check-inputs:
	@test -f $(TLS_DIR)/include/mbedtls/ssl.h || { echo "TLS_DIR must name the Mbed TLS source from scripts/prepare-tls.py" >&2; exit 2; }
	@test -f $(CONNECTION_HEADER) || { echo "CONNECTION_HEADER must name a header from scripts/configure-connection.py" >&2; exit 2; }

# unitcode 2 runs on DS and DSi; the DSi header asks for DSi mode, which the
# companion needs for the SD card and WPA2.
$(ROM): $(ELF)
	@echo "  NDSTOOL $@"
	$(V)$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $(ELF) -uc 2 \
		-b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUBTITLE);$(GAME_AUTHOR)"

# The specs link with --gc-sections, which drops unused code before the linker
# looks for its symbols. This link keeps every section, so a core symbol the
# companion does not provide yet fails the build.
$(LINK_CHECK): $(APP_OBJS) $(CORE_OBJS) $(TLS_LIB)
	@echo "  LD      $@"
	$(V)$(CC) -o $@ $(APP_OBJS) $(CORE_OBJS) $(LDFLAGS) -Wl,--no-gc-sections $(LDLIBS)

$(ELF): $(APP_OBJS) $(CORE_OBJS) $(TLS_LIB) $(LINK_CHECK)
	@echo "  LD      $@"
	$(V)$(CC) -o $@ $(APP_OBJS) $(CORE_OBJS) $(LDFLAGS) \
		-Wl,-Map,$(BUILDDIR)/$(NAME).map $(LDLIBS)

$(TLS_LIB): $(TLS_OBJS)
	@echo "  AR      $@"
	$(V)rm -f $@
	$(V)$(PREFIX)ar rcs $@ $^

$(BUILDDIR)/app/%.o: %.c | check-inputs
	@echo "  CC      $<"
	@mkdir -p $(@D)
	$(V)$(CC) $(BASE_CFLAGS) -include $(CONNECTION_HEADER) -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILDDIR)/core/%.o: $(CLIENT)/src/wonder_trade/%.c | check-inputs
	@echo "  CC      $<"
	@mkdir -p $(@D)
	$(V)$(CC) $(BASE_CFLAGS) -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILDDIR)/tls/%.o: $(TLS_DIR)/%.c | check-inputs
	@echo "  CC      $<"
	@mkdir -p $(@D)
	$(V)$(CC) $(BASE_CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)

-include $(APP_OBJS:.o=.d) $(CORE_OBJS:.o=.d) $(TLS_OBJS:.o=.d)
