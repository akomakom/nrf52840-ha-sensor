# ================================================================
#  Nice!Nano (nRF52840) – arduino-cli build system
#
#  Targets:
#    make configure   – set up arduino-cli (run once)
#    make build       – compile sketch
#    make upload      – build + merge SoftDevice + copy to board
#    make all         – same as upload
#    make monitor     – open serial monitor (Ctrl-C to exit)
#    make clean       – remove build artifacts
#    make help        – show this list
# ================================================================

SKETCH_NAME := nice_nano_test
FQBN        := community_nrf52:nrf52:nice_nano
BUILD_DIR   := .build
MERGE_SCRIPT:= make_combined_uf2.py
COMBINED_UF2:= $(BUILD_DIR)/combined.uf2
MOUNT       := /media/$(USER)/NICENANO
PORT        := /dev/ttyACM0
BAUD        := 115200

# Auto-detect the installed BSP version so this survives upgrades
SD_HEX := $(firstword $(wildcard \
    $(HOME)/.arduino15/packages/community_nrf52/hardware/nrf52/*/bootloader/nice_nano/nice_nano_bootloader-*_s140_*.hex))

HEX_FILE := $(BUILD_DIR)/$(SKETCH_NAME).ino.hex

BOARD_URLS := https://arduino.esp8266.com/stable/package_esp8266com_index.json,https://espressif.github.io/arduino-esp32/package_esp32_index.json,https://adafruit.github.io/arduino-board-index/package_adafruit_index.json

# ----------------------------------------------------------------

.PHONY: all build merge upload monitor configure clean help

all: upload

# ---- Build -----------------------------------------------------

$(HEX_FILE): $(SKETCH_NAME).ino
	@mkdir -p $(BUILD_DIR)
	arduino-cli compile --fqbn $(FQBN) --output-dir $(BUILD_DIR) .

build: $(HEX_FILE)

# ---- Merge SoftDevice + app ------------------------------------

$(COMBINED_UF2): $(HEX_FILE)
	@test -n "$(SD_HEX)" || { \
		echo "ERROR: SoftDevice hex not found – run 'make configure' first"; exit 1; }
	python3 $(MERGE_SCRIPT) \
		--app $(HEX_FILE) \
		--sd  "$(SD_HEX)" \
		--out $(COMBINED_UF2)

merge: $(COMBINED_UF2)

# ---- Upload ----------------------------------------------------

upload: $(COMBINED_UF2)
	@if [ ! -d "$(MOUNT)" ]; then \
		echo ""; \
		echo "NICENANO drive not found at $(MOUNT)."; \
		echo "  --> Double-tap the reset button on the board, then press Enter"; \
		read dummy; \
	fi
	@test -d "$(MOUNT)" || { \
		echo "ERROR: $(MOUNT) still not found – check the mount path"; exit 1; }
	@echo "Copying $(COMBINED_UF2) → $(MOUNT)/"
	cp $(COMBINED_UF2) $(MOUNT)/ && sync
	@echo "Done. Board should reboot into new firmware."

# ---- Serial monitor --------------------------------------------

monitor:
	arduino-cli monitor -p $(PORT) -c baudrate=$(BAUD)

# ---- Configure arduino-cli (run once) --------------------------

configure:
	@echo "Initialising arduino-cli config..."
	arduino-cli config init --overwrite
	arduino-cli config set board_manager.additional_urls "$(BOARD_URLS)"
	@echo "Updating board index (network errors here are non-fatal)..."
	arduino-cli core update-index || true
	@echo "Installing Adafruit nRF52 core..."
	arduino-cli core install adafruit:nrf52 || true
	@echo "Registering Community nRF52 core (already installed by Arduino IDE)..."
	@# The community_nrf52 index URL is defunct but packages are already present
	@# in ~/.arduino15/packages — copy the cached index so arduino-cli finds it
	@if [ -f $(HOME)/.arduino15/package_jpconstantineau_boards_index.json ]; then \
		echo "  Found cached community index — OK"; \
	else \
		echo "WARNING: community_nrf52 index not found; open Arduino IDE and install 'Community Add on to Adafruit nRF52' via Board Manager first"; \
	fi
	@echo ""
	@echo "Configuration complete. Verify with:"
	@echo "  arduino-cli board listall | grep -i nice"

# ---- Clean -----------------------------------------------------

clean:
	rm -rf $(BUILD_DIR)

# ---- Help ------------------------------------------------------

help:
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "  configure   Set up arduino-cli with required board packages (run once)"
	@echo "  build       Compile the sketch"
	@echo "  upload      Build, merge SoftDevice, and flash to board (default)"
	@echo "  monitor     Open serial monitor on $(PORT) at $(BAUD) baud"
	@echo "  clean       Remove build artifacts in $(BUILD_DIR)/"
	@echo "  help        Show this message"
	@echo ""
	@echo "Overrides:  make upload PORT=/dev/ttyACM1"
	@echo "            make upload MOUNT=/media/user/NICENANO"
	@echo ""
