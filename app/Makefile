.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "Set PSL1GHT in your environment, e.g. export PSL1GHT=/opt/ps3dev")
endif

export ICON0 := $(CURDIR)/ICON0.PNG
export PKGFILES := $(CURDIR)/pkgfiles
# CATEGORY in here picks the XMB column: AV tries for the Video column, HG is the Game column.
export SFOXML := $(CURDIR)/sfo.xml

include $(PSL1GHT)/ppu_rules

TARGET    := anime_app
BUILD     := build
SOURCES   := source
SHADERS   := shaders

TITLE     := Nami
APPID     := ANIMEPS30
CONTENTID := UP0001-$(APPID)_00-0000000000000000

CFLAGS    = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
LDFLAGS   = $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS      := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lfaad -lvdec -laudio -lfreetype -ljpgdec -lpngdec -lz -lnet -lrsx \
             -lsysmodule -lgcm_sys -lio -lsysutil -lrt -llv2 -lm

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(SHADERS),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

VCGFILES := $(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.vcg)))
FCGFILES := $(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.fcg)))
VPOFILES := $(VCGFILES:.vcg=.vpo)
FPOFILES := $(FCGFILES:.fcg=.fpo)

export LD       := $(CC)
export OFILES   := $(addsuffix .o,$(VPOFILES)) $(addsuffix .o,$(FPOFILES)) $(CFILES:.c=.o)
export INCLUDE  := $(LIBPSL1GHT_INC) -I$(PORTLIBS)/include -I$(PORTLIBS)/include/freetype2 -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(LIBPSL1GHT_LIB) -L$(PORTLIBS)/lib

.PHONY: $(BUILD) clean pkg

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).pkg $(OUTPUT).gnpdrm.pkg

pkg: $(BUILD) $(OUTPUT).pkg

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
$(OFILES): $(addsuffix .o,$(VPOFILES)) $(addsuffix .o,$(FPOFILES))

%.vpo.o: %.vpo
	@echo $(notdir $<)
	@$(bin2o)

%.fpo.o: %.fpo
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
