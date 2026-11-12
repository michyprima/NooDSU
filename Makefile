ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

APP_NAME := NooDS U
APP_SHORTNAME := NooDS U
APP_AUTHOR := michyprima, Hydr8gon
APP_ICON := ../icon/icon-wiiu.png

include $(DEVKITPRO)/wut/share/wut_rules

NAME := noodsu
BUILD := build-wiiu
SRCS := src src/core src/core/arm src/core/gpu src/core/hle src/core/io src/core/memory src/core/net src/core/teakra src/ui src/ui/console
SLIRP := src/core/net/libslirp
SLIRPSRCS := $(SLIRP)/src $(SLIRP)/glib
FAAD := src/core/faad2
DATA := src/ui/console/images src/ui/console/shaders
LIBS := -lwut -lSDL2
INCS := $(PORTLIBS) $(WUT_ROOT)

CXXFLAGS := -g -Ofast -fno-allow-store-data-races -flto -ffunction-sections $(MACHDEP) $(INCLUDE) \
    -D__WIIU__ -D__WUT__ -DENDIAN_BIG -DNO_FDOPEN -DLOG_LEVEL=0
LDFLAGS = -g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

# libslirp is C, built with its own include folders so its header names can't clash with others; they're added even
# when variant builds set CFLAGS
CFLAGS := -g -O2 -ffunction-sections $(MACHDEP) -D__WIIU__ -D__WUT__ $(foreach dir,$(INCS),-I$(dir)/include)
override CFLAGS += -DLIBSLIRP_STATIC_BUILD -DBUILDING_LIBSLIRP '-DG_LOG_DOMAIN="Slirp"' $(SLIRPINCLUDE) -include compat.h

# faad2, the AAC decoder for the DSP's high-level AAC program, is C with its own flags: only AAC LC, as the DSP decodes
FAADFLAGS := -g -O2 -w -ffunction-sections $(MACHDEP) -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 \
    -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 '-DPACKAGE_VERSION="2.11.4"' \
    -DAPPLY_DRC -DLC_ONLY_DECODER -DDISABLE_SBR

ifneq ($(BUILD),$(notdir $(CURDIR)))

# The tag of this commit if it has one, otherwise its short hash; with uncommitted changes to tracked files, it's the
# short hash marked as a dev build, which also shows the build's date and time to the minute so each one can be told apart.
# Only git's "changes found" status counts: in CI's container git fails outright, and those builds aren't dev builds
DEV := $(shell git diff --quiet HEAD 2>/dev/null; [ $$? -eq 1 ] && echo 1 || echo 0)
ifeq ($(DEV),1)
VERSION ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)-dev
else
VERSION ?= $(shell git describe --tags --exact-match 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)
endif
BUILT := $(shell date +%y%m%d-%H%M)

export LD := $(CXX)
export OUTPUT := $(CURDIR)/$(NAME)
export DEPSDIR := $(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(INCS),-L$(dir)/lib)
export VPATH := $(foreach dir,$(SRCS) $(SLIRPSRCS) $(FAAD),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export FAADINCLUDE := -I$(CURDIR)/$(FAAD)
export SLIRPINCLUDE := -I$(CURDIR)/$(SLIRP)/src -I$(CURDIR)/$(SLIRP)/glib -I$(CURDIR)/$(SLIRP)/wiiu
export INCLUDE := $(foreach dir,$(SRCS),-I$(CURDIR)/$(dir)) $(foreach dir,$(INCS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)

CPPFILES := $(foreach dir,$(SRCS),$(notdir $(wildcard $(dir)/*.cpp)))
CFILES := $(foreach dir,$(SLIRPSRCS),$(notdir $(wildcard $(dir)/*.c)))
export FAADOFILES := $(patsubst %.c,%.o,$(notdir $(wildcard $(FAAD)/*.c)))
BMPFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bmp)))
GSHFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.gsh)))
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(FAADOFILES) $(BMPFILES:.bmp=.o) $(addsuffix .o,$(GSHFILES))

.PHONY: $(BUILD)
all: $(BUILD)

$(BUILD):
	mkdir -p $@
	@printf '#define NOODSU_VERSION "%s"\n#define NOODSU_BUILT "%s"\n#define NOODSU_DEV %s\n' '$(VERSION)' '$(BUILT)' \
		'$(DEV)' > $@/version.tmp
	@cmp -s $@/version.tmp $@/version.h && rm $@/version.tmp || mv $@/version.tmp $@/version.h
	$(MAKE) -C $(BUILD) -f $(CURDIR)/Makefile

build-shader:
	cd src/ui/console/shaders; ./latte-assembler compile shader_wiiu.gsh --vsh shader_wiiu.vsh --psh shader_wiiu.psh

clean:
	rm -rf $(BUILD) $(NAME).wuhb $(NAME).rpx $(NAME).elf

else

all: $(OUTPUT).wuhb
$(OUTPUT).wuhb: $(OUTPUT).rpx
$(OUTPUT).rpx: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(FAADOFILES): %.o: %.c
	@echo $(notdir $<)
	@$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(FAADFLAGS) $(FAADINCLUDE) -c $< -o $@

%.o: %.bmp
	@echo $(notdir $<)
	@cd ..; $(PREFIX)ld -r -b binary -o $(BUILD)/$@ src/ui/console/images/$*.bmp

%.gsh.o: %.gsh
	@echo $(notdir $<)
	@$(bin2o)

DEPENDS := $(OFILES:.o=.d)
-include $(DEPENDS)

endif
