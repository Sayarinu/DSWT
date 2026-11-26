# DS Companion: a DSi-mode BlocksDS ROM, ARM9 app and DSWiFi ARM7 core.
# Adapted from BlocksDS's rom_arm9 template (CC0-1.0). Build it through
# scripts/build-companion.sh, which runs it in tools/companion-builder.
#
# From the repository root:
#   scripts/build-companion.sh
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

# The DSWiFi core also serves the microphone and Slot-1 power requests.
ARM7_SOURCE := src/platform/nds/arm7/main.c
ARM7ELF := $(BUILDDIR)/arm7.elf

# The game-independent client core, compiled in place. The Nitro-only
# flash.c, network.c and entropy_game.c and the per-game folders stay out;
# src/platform/nds supplies what they would (boot_glue.c, net.c, worker.c).
# journal.c, the one family-dependent source, is built once per family with
# renamed symbols (src/core/journal_family.h); family-check proves the rest
# preprocess to the same text for every family.
CLIENT			:= ../client
CORE_SOURCES		:= $(addprefix $(CLIENT)/src/wonder_trade/,\
			   engine.c wire.c http.c tls.c seed.c \
			   recovery.c entropy.c pokemon_codec.c boot.c session.c)
JOURNAL_SOURCE		:= $(CLIENT)/src/wonder_trade/journal.c

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

APP_SOURCES		:= $(filter-out $(ARM7_SOURCE),$(shell find src -name '*.c' | sort))

PREFIX			:= $(ARM_NONE_EABI_PATH)arm-none-eabi-
CC			:= $(PREFIX)gcc
NDSTOOL			:= $(BLOCKSDS)/tools/ndstool/ndstool

ARCH			:= -mthumb -mcpu=arm946e-s+nofp
SPECS			:= $(BLOCKSDS)/sys/crts/ds_arm9.specs
# WT_ABANDON_UNSENT: the engine may abandon an escrow the stock game changed
# and the server never received (LUNE-902, client wonder_trade/engine.h).
DEFINES			:= -D__NDS__ -D__BLOCKSDS__ -DARM9 \
			   -DWT_GAME_FAMILY=WT_FAMILY_GEN4 -DWT_ABANDON_UNSENT \
			   -DMBEDTLS_CONFIG_FILE='"wonder_trade/tls_config.h"' \
			   -DTF_PSA_CRYPTO_CONFIG_FILE='"wonder_trade/crypto_config.h"'
INCLUDES		:= -Isrc -I$(CLIENT)/include \
			   $(addprefix -I$(TLS_DIR)/,$(TLS_INCLUDE_DIRS)) \
			   -I$(BLOCKSDS)/libs/libnds/include \
			   -I$(BLOCKSDS)/libs/dswifi/include
BASE_CFLAGS		:= $(ARCH) -std=gnu11 -O2 -ffunction-sections \
			   -fdata-sections -specs=$(SPECS) $(DEFINES) $(INCLUDES)
LDFLAGS			:= $(ARCH) -specs=$(SPECS) -L$(BLOCKSDS)/libs/libnds/lib \
			   -L$(BLOCKSDS)/libs/dswifi/lib
LDLIBS			:= -Wl,--start-group $(BUILDDIR)/libwt_tls.a -ldswifi9 -lnds9 -lc -Wl,--end-group

APP_OBJS		:= $(patsubst %.c,$(BUILDDIR)/app/%.o,$(APP_SOURCES))
CORE_OBJS		:= $(patsubst $(CLIENT)/src/wonder_trade/%.c,$(BUILDDIR)/core/%.o,$(CORE_SOURCES)) \
			   $(BUILDDIR)/core/journal_gen4.o $(BUILDDIR)/core/journal_gen5.o
FAMILY_CHECK		:= $(BUILDDIR)/core/family-check.stamp
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

$(ARM7ELF): $(ARM7_SOURCE)
	@mkdir -p $(@D)
	$(V)$(CC) -mthumb -mcpu=arm7tdmi -std=gnu11 -Os -Wall -Wextra -Werror \
		-specs=$(BLOCKSDS)/sys/crts/ds_arm7.specs -I$(BLOCKSDS)/libs/libnds/include \
		-I$(BLOCKSDS)/libs/dswifi/include -L$(BLOCKSDS)/libs/dswifi/lib \
		-L$(BLOCKSDS)/libs/libnds/lib -o $@ $< -Wl,--start-group -ldswifi7 -lnds7 -lc -Wl,--end-group

# unitcode 2 runs on DS and DSi; the DSi header asks for DSi mode, which the
# companion needs for the SD card and WPA2.
$(ROM): $(ELF) $(ARM7ELF)
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

# journal.c for Generation IV and for Generation V (Black/White and Black
# 2/White 2 build the same journal).
$(BUILDDIR)/core/journal_gen4.o: $(JOURNAL_SOURCE) src/core/journal_rename.h | check-inputs
	@echo "  CC      $< (Gen 4)"
	@mkdir -p $(@D)
	$(V)$(CC) $(BASE_CFLAGS) -DWT_GAME_FAMILY=WT_FAMILY_GEN4 -DCP_JOURNAL_PREFIX=CpJournalGen4 \
		-include src/core/journal_rename.h -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILDDIR)/core/journal_gen5.o: $(JOURNAL_SOURCE) src/core/journal_rename.h | check-inputs
	@echo "  CC      $< (Gen 5)"
	@mkdir -p $(@D)
	$(V)$(CC) $(filter-out -DWT_GAME_FAMILY=%,$(BASE_CFLAGS)) -DWT_GAME_FAMILY=WT_FAMILY_BW \
		-DCP_JOURNAL_PREFIX=CpJournalGen5 -include src/core/journal_rename.h -Wall -Wextra -Werror -MMD -MP \
		-c -o $@ $<

# Every other core source is family-independent: its preprocessed text is the
# same for each family, so the one build serves every game.
$(FAMILY_CHECK): $(CORE_SOURCES) | check-inputs
	@echo "  CHECK   core family independence"
	@mkdir -p $(@D)
	$(V)for source in $(CORE_SOURCES); do \
		for family in WT_FAMILY_BW WT_FAMILY_B2W2; do \
			$(CC) $(BASE_CFLAGS) -E -P $$source -o $(@D)/gen4.i && \
			$(CC) $(filter-out -DWT_GAME_FAMILY=%,$(BASE_CFLAGS)) -DWT_GAME_FAMILY=$$family -E -P $$source \
				-o $(@D)/gen5.i && cmp -s $(@D)/gen4.i $(@D)/gen5.i || \
				{ echo "$$source depends on the game family" >&2; exit 1; }; \
		done; \
	done
	$(V)touch $@

$(ELF) $(LINK_CHECK): $(FAMILY_CHECK)

$(BUILDDIR)/tls/%.o: $(TLS_DIR)/%.c | check-inputs
	@echo "  CC      $<"
	@mkdir -p $(@D)
	$(V)$(CC) $(BASE_CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)

-include $(APP_OBJS:.o=.d) $(CORE_OBJS:.o=.d) $(TLS_OBJS:.o=.d)

# LUNE-868: tables/font are prepared by scripts/build-companion.sh on the host.
# The no-gc link check above verifies every generated symbol even before UI use.
TEXT_DIR                := $(BUILDDIR)/text
TEXT_OBJ                := $(TEXT_DIR)/companion_text.o
APP_OBJS                += $(TEXT_OBJ)
BASE_CFLAGS             += -I$(TEXT_DIR)
# Retain the complete text interface before the UI starts referencing it.
TEXT_SYMBOLS            := Companion_Text Companion_FontCodepoints Companion_FontGlyphs \
                           Companion_FontBitmap Companion_FontGlyphCount Companion_FontBitmapSize \
                           Companion_FontAscent Companion_FontLineHeight
LDFLAGS                 += $(foreach symbol,$(TEXT_SYMBOLS),-Wl,--undefined=$(symbol))
$(ELF) $(LINK_CHECK): $(TEXT_OBJ)
$(TEXT_OBJ): $(TEXT_DIR)/companion_text.c $(TEXT_DIR)/companion_text.h
	@echo "  CC      $<"
	$(V)$(CC) $(BASE_CFLAGS) -Wall -Wextra -Werror -c -o $@ $<

check-inputs: $(TEXT_DIR)/companion_text.c $(TEXT_DIR)/companion_text.h
$(TEXT_DIR)/companion_text.c $(TEXT_DIR)/companion_text.h:
	@echo "Missing $@; run scripts/build-companion.sh from the repository root, or generate text with: python3 scripts/companion-text.py --output $(abspath $(TEXT_DIR))" >&2
	@exit 2

# LUNE-872: the screens' project-original 8x8 tiles (res/gfx/*.png, each with
# its .grit options) become ui_<name>Tiles through grit.
GRIT                    := $(BLOCKSDS)/tools/grit/grit
GFX_DIR                 := $(BUILDDIR)/gfx
GFX_NAMES               := $(basename $(notdir $(wildcard res/gfx/*.png)))
GFX_OBJS                := $(GFX_NAMES:%=$(GFX_DIR)/%.o)
GFX_HEADERS             := $(GFX_NAMES:%=$(GFX_DIR)/%.h)
APP_OBJS                += $(GFX_OBJS)
BASE_CFLAGS             += -I$(GFX_DIR)
$(ELF) $(LINK_CHECK): $(GFX_OBJS)
check-inputs: $(GFX_HEADERS)
$(GFX_DIR)/%.s $(GFX_DIR)/%.h: res/gfx/%.png res/gfx/%.grit
	@echo "  GRIT    $<"
	@mkdir -p $(@D)
	$(V)$(GRIT) $< -ff res/gfx/$*.grit -fts -o$(GFX_DIR)/$*
$(GFX_DIR)/%.o: $(GFX_DIR)/%.s
	@echo "  AS      $<"
	$(V)$(CC) $(ARCH) -c -o $@ $<

# A build that opens the screens with made-up data: CP_UI_DEMO=1 follows the
# buttons, CP_UI_DEMO=2 walks through every screen by itself.
ifdef CP_UI_DEMO
BASE_CFLAGS             += -DCP_UI_DEMO=$(CP_UI_DEMO)
endif
