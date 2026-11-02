ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

APP_NAME := NooDSU
APP_SHORTNAME := NooDSU
APP_AUTHOR := michyprima, Hydr8gon
APP_ICON := ../icon/icon-wiiu.png

include $(DEVKITPRO)/wut/share/wut_rules

NAME := noodsu
BUILD := build-wiiu
SRCS := src src/core src/core/arm src/core/gpu src/core/hle src/core/io src/core/memory src/ui src/ui/console
DATA := src/ui/console/images src/ui/console/shaders
LIBS := -lwut -lSDL2
INCS := $(PORTLIBS) $(WUT_ROOT)

CXXFLAGS := -g -Ofast -fno-allow-store-data-races -flto -ffunction-sections $(MACHDEP) $(INCLUDE) \
    -D__WIIU__ -D__WUT__ -DENDIAN_BIG -DNO_FDOPEN -DLOG_LEVEL=0
LDFLAGS = -g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

ifneq ($(BUILD),$(notdir $(CURDIR)))

# The tag of this commit if it has one, otherwise its short hash, marked if tracked files have uncommitted changes
VERSION ?= $(shell git describe --tags --exact-match 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)$(shell \
    git diff --quiet HEAD 2>/dev/null; [ $$? -eq 1 ] && echo -dev)

export LD := $(CXX)
export OUTPUT := $(CURDIR)/$(NAME)
export DEPSDIR := $(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(INCS),-L$(dir)/lib)
export VPATH := $(foreach dir,$(SRCS),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export INCLUDE := $(foreach dir,$(SRCS),-I$(CURDIR)/$(dir)) $(foreach dir,$(INCS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)

CPPFILES := $(foreach dir,$(SRCS),$(notdir $(wildcard $(dir)/*.cpp)))
BMPFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bmp)))
GSHFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.gsh)))
export OFILES := $(CPPFILES:.cpp=.o) $(BMPFILES:.bmp=.o) $(addsuffix .o,$(GSHFILES))

.PHONY: $(BUILD)
all: $(BUILD)

$(BUILD):
	mkdir -p $@
	@echo '#define NOODSU_VERSION "$(VERSION)"' > $@/version.tmp
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

%.o: %.bmp
	@echo $(notdir $<)
	@cd ..; $(PREFIX)ld -r -b binary -o $(BUILD)/$@ src/ui/console/images/$*.bmp

%.gsh.o: %.gsh
	@echo $(notdir $<)
	@$(bin2o)

DEPENDS := $(OFILES:.o=.d)
-include $(DEPENDS)

endif
