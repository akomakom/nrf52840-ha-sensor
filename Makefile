# ================================================================
#  nice!nano — nRF Connect SDK / Zephyr build system
#
#  Targets:
#    make / make build  — compile (output: build/zephyr/zephyr.uf2)
#    make flash         — build + copy UF2 to board (double-tap reset first)
#    make monitor       — open serial monitor (Ctrl-A K to exit)
#    make clean         — remove build directory
#    make pristine      — remove build dir + force full CMake reconfigure
#    make help          — show this list
#
#  Overrides:
#    make flash   MOUNT=/media/youruser/NICENANO
#    make monitor PORT=/dev/ttyACM1
# ================================================================

# west board target — must include the SoC suffix for nRF Connect SDK 2.7+
BOARD     := nice_nano/nrf52840
BUILD_DIR := build
PORT      := /dev/ttyACM0
BAUD      := 115200

UF2 := $(BUILD_DIR)/zephyr/zephyr.uf2

# Auto-detect the bootloader mount point.
# Checks /media/$USER/NICENANO then /run/media/$USER/NICENANO.
# Override with: make flash MOUNT=/path/to/NICENANO
MOUNT ?= $(shell \
	for base in /media/$(USER) /run/media/$(USER); do \
		mp="$$base/NICENANO"; \
		if [ -d "$$mp" ]; then echo "$$mp"; break; fi; \
	done)

.PHONY: all build flash pflash _copy_uf2 monitor clean pristine help

all: build

# ---- Build -----------------------------------------------------
# Delete the UF2 first so that if west build silently does nothing
# (incremental no-op), the flash step will fail loudly instead of
# copying a stale file.

build:
	@rm -f $(UF2)
	west build -b $(BOARD) -d $(BUILD_DIR)
	@test -f $(UF2) || { echo "ERROR: build finished but $(UF2) was not produced!"; exit 1; }

# ---- Flash -----------------------------------------------------
# Double-tap reset first, wait for LED to pulse, then press Enter.

flash: build
	@echo ""
	@echo "┌─────────────────────────────────────────────────────┐"
	@echo "│  Double-tap RESET on the nice!nano NOW              │"
	@echo "│  Wait for the LED to pulse, then press ENTER        │"
	@echo "└─────────────────────────────────────────────────────┘"
	@read _dummy
	@$(MAKE) --no-print-directory _copy_uf2

_copy_uf2:
	@if [ -z "$(MOUNT)" ]; then \
		echo ""; \
		echo "ERROR: NICENANO bootloader drive not found."; \
		echo "  Did you double-tap reset? (LED should be pulsing)"; \
		echo "  Or specify the path: make flash MOUNT=/media/you/NICENANO"; \
		exit 1; \
	fi
	@echo "Copying $(UF2) → $(MOUNT)/"
	cp "$(UF2)" "$(MOUNT)/"
	sync
	@echo "Done — device rebooting into firmware."

# ---- Serial monitor --------------------------------------------

monitor:
	@if [ ! -c $(PORT) ]; then \
		echo "$(PORT) not found — device not plugged in or not running firmware?"; \
		exit 1; \
	fi
	@echo "Opening $(PORT) at $(BAUD) baud  (Ctrl-A then K to quit)"
	screen $(PORT) $(BAUD)

# ---- Clean / Pristine ------------------------------------------

clean:
	rm -rf $(BUILD_DIR)

# A pristine rebuild forces CMake to reconfigure from scratch — useful
# after changing prj.conf, app.overlay, or board Kconfig/DTS.
# Use 'make pristine' to build only, or 'make pflash' to build + flash.
pristine:
	@rm -f $(UF2)
	west build -b $(BOARD) -d $(BUILD_DIR) --pristine=always
	@test -f $(UF2) || { echo "ERROR: pristine build finished but $(UF2) was not produced!"; exit 1; }

# Pristine build + flash in one step (skips the incremental build).
pflash: pristine
	@echo ""
	@echo "┌─────────────────────────────────────────────────────┐"
	@echo "│  Double-tap RESET on the nice!nano NOW              │"
	@echo "│  Wait for the LED to pulse, then press ENTER        │"
	@echo "└─────────────────────────────────────────────────────┘"
	@read _dummy
	@$(MAKE) --no-print-directory _copy_uf2

# ---- Help ------------------------------------------------------

help:
	@echo ""
	@echo "  make / make build   Compile firmware (incremental)"
	@echo "  make flash          Incremental build + copy UF2 to board"
	@echo "  make pristine       Full CMake reconfigure + rebuild (no flash)"
	@echo "  make pflash         Full CMake reconfigure + rebuild + flash"
	@echo "  make monitor        Open serial monitor on $(PORT) at $(BAUD) baud"
	@echo "  make clean          Remove $(BUILD_DIR)/"
	@echo ""
	@echo "  Overrides:"
	@echo "    make flash   MOUNT=/media/youruser/NICENANO"
	@echo "    make monitor PORT=/dev/ttyACM1"
	@echo ""
