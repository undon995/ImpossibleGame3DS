ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set. Use devkitPro MSYS2)
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Use devkitPro MSYS2)
endif
include $(DEVKITARM)/3ds_rules
TARGET := ImpossibleGame3DS
BUILD := build
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -std=gnu11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -mword-relocations $(ARCH) -D__3DS__ -I$(CTRULIB)/include
LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,-Map,$(TARGET).map -Wl,--gc-sections
LIBS := -L$(CTRULIB)/lib -lctru -lm
.PHONY: all clean ready-stages
all: ready-stages $(TARGET).3dsx
ready-stages:
	@python prepare_levels.py --download
$(BUILD):
	@mkdir -p $(BUILD)
$(BUILD)/main.o: source/main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(TARGET).elf: $(BUILD)/main.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@
$(TARGET).smdh: icon.png
	smdhtool --create "The Impossible Game 3DS" "Unofficial personal homebrew port" "Homebrew" icon.png $@
$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh ready-stages
	@if [ ! -f romfs/level5.bin ] || [ ! -f romfs/sprites/player.rgba ]; then \
	  echo "ERROR: Missing original graphics. Run prepare_assets.py with your original game ZIP"; exit 1; fi
	@for st in 0 1 2 3 4; do \
	  if [ ! -f romfs/levels/stage$$st.bin ]; then \
	    echo "ERROR: Missing stage $$st"; exit 1; fi; \
	done
	3dsxtool $< $@ --smdh=$(TARGET).smdh --romfs=romfs
clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh $(TARGET).map
