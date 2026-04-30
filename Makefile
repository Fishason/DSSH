#---------------------------------------------------------------------------------
# 3dssh - Nintendo 3DS SSH client with Chinese IME
#
# M0 (this Makefile): minimal hello-world to verify toolchain.
# M1+ will add libssh2, mbedtls, citro2d, romfs etc.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM. Run via: tools/dkp.sh make")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
TARGET		:=	3dssh
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
ROMFS		:=	romfs

APP_TITLE	:=	3DS SSH Client
APP_DESCRIPTION	:=	SSH terminal with Chinese IME
APP_AUTHOR	:=	exdekotive

#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# M3: citro2d/citro3d for top-screen ANSI terminal rendering.
LIBS	:=	-lssh2 \
			-lmbedtls -lmbedx509 -lmbedcrypto \
			-lcitro2d -lcitro3d \
			-lctru -lm

LIBDIRS	:=	$(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_SOURCES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN		:=	$(addsuffix .o,$(BINFILES))
export OFILES			:=	$(OFILES_BIN) $(OFILES_SOURCES)
export HFILES			:=	$(addsuffix .h,$(BINFILES))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all ime-dict test-ime cia cia-tools cia-clean

all: $(BUILD)

# Regenerate the pinyin IME binary dict (rime-ice top-300k entries).
# Output goes into romfs/pinyin_dict.bin and is bundled by --romfs.
ime-dict:
	@bash tools/fetch_pinyin_dict.sh
	@python3 tools/gen_pinyin_dict.py

# Host-side smoke test for the IME engine — much faster than 3dslink.
test-ime:
	@bash tools/test_ime.sh

# ── M9: CIA packaging ───────────────────────────────────────────────
#
# `make cia` produces DSSH.cia from the already-built ELF + romfs.
# Pipeline: gen_cia_assets.py → bannertool makesmdh / makebanner →
# makerom -f cia.  bannertool + makerom live in $HOME/bin (install via
# `make cia-tools` once); we put $HOME/bin first on PATH so the rule
# works in a fresh shell with no system-wide install.
CIA_BIN          := $(HOME)/bin
CIA_PATH_PREPEND := PATH=$(CIA_BIN):$$PATH
CIA_ASSETS       := $(CURDIR)/cia_assets
CIA_TARGET       := $(CURDIR)/DSSH.cia

cia-tools:
	@bash tools/install_cia_tools.sh

# Generate icon (project root, used by the .3dsx too) + banner +
# silent.wav (CIA-only) from the source 162x102 logo.
icon.png $(CIA_ASSETS)/banner.png $(CIA_ASSETS)/silent.wav: \
		tools/gen_cia_assets.py 69633.PNG
	@python3 tools/gen_cia_assets.py
	@test -f $(CIA_ASSETS)/silent.wav || python3 -c "import wave,struct; \
		w=wave.open('$(CIA_ASSETS)/silent.wav','wb'); \
		w.setnchannels(2); w.setsampwidth(2); w.setframerate(22050); \
		w.writeframes(b'\\\\x00'*44100); w.close()"

# Run bannertool to package the SMDH (icon + metadata) and banner
# (selected-card image + audio).
$(BUILD)/dssh.smdh: icon.png
	@$(CIA_PATH_PREPEND) bannertool makesmdh \
		-s "DSSH" \
		-l "DSSH — SSH client with Chinese IME" \
		-p "exdekotive" \
		-i icon.png \
		-o $@ >/dev/null

$(BUILD)/dssh.bnr: $(CIA_ASSETS)/banner.png $(CIA_ASSETS)/silent.wav
	@$(CIA_PATH_PREPEND) bannertool makebanner \
		-i $(CIA_ASSETS)/banner.png \
		-a $(CIA_ASSETS)/silent.wav \
		-o $@ >/dev/null

# The CIA depends on the ELF, the SMDH, the banner, the RSF, and the
# romfs blob (pinyin dict).  makerom rebuilds everything every time;
# romfs is read fresh so any dict change picks up automatically.
$(CIA_TARGET): $(OUTPUT).elf $(BUILD)/dssh.smdh $(BUILD)/dssh.bnr app.rsf \
		romfs/pinyin_dict.bin
	@$(CIA_PATH_PREPEND) makerom \
		-f cia -target t \
		-elf $(OUTPUT).elf -rsf app.rsf \
		-icon $(BUILD)/dssh.smdh -banner $(BUILD)/dssh.bnr \
		-o $@
	@echo "built ... $(notdir $@)"

cia: $(CIA_TARGET)

cia-clean:
	@rm -rf $(CIA_ASSETS) icon.png $(BUILD)/dssh.smdh $(BUILD)/dssh.bnr $(CIA_TARGET)

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)
$(OFILES_SOURCES) : $(HFILES)
$(OUTPUT).elf	:	$(OFILES)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
