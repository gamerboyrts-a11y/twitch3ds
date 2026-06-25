#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
APP_TITLE   := Twitch3DS
APP_DESC    := Twitch client for New 3DS
APP_AUTHOR  := gamerboyrts

TARGET      := twitch3ds
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include
ROMFS       := romfs
ICON        := $(TOPDIR)/icon.png

#---------------------------------------------------------------------------------
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS   := -g -Wall -O2 -fomit-frame-pointer -ffunction-sections \
             $(ARCH) $(INCLUDE) -D__3DS__ -DNEW3DS

CFLAGS   += $(foreach dir,$(LIBDIRS),-I$(dir)/include)

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS  := -g $(ARCH)

LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# -lz must come AFTER -lcurl
#---------------------------------------------------------------------------------
LIBS     := -lcitro2d -lcitro3d \
            -lcurl -lmbedtls -lmbedx509 -lmbedcrypto \
            -lz \
            -lctru -lm

#---------------------------------------------------------------------------------
LIBDIRS  := $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)

export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD := $(CC)

export OFILES_BIN   := $(addsuffix .o,$(BINFILES))
export OFILES_SRC   := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   := $(patsubst %.bin,%.h,$(filter %.bin,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS) --smdh=$(CURDIR)/$(TARGET).smdh

.PHONY: $(BUILD) clean all

all: $(TARGET).smdh $(BUILD)

$(TARGET).smdh: $(ICON)
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" $< $@

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
DEPENDS := $(OFILES:.o=.d)

# Build 3dsx directly from elf — no smdh dependency
$(OUTPUT).3dsx: $(OUTPUT).elf
	@echo built ... $(notdir $@)
	@3dsxtool $< $@ $(_3DSXFLAGS)

$(OFILES_SRC): $(HFILES_BIN)

$(OUTPUT).elf: $(OFILES)

%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
