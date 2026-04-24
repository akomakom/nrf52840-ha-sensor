# ================================================================
#  nice!nano — nRF Connect SDK / Zephyr build system
#
#  Targets:
#    make / make build       — compile (output: build/zephyr/zephyr.uf2)
#    make flash              — build + copy UF2 to board (double-tap reset first)
#    make flash-debug        — build DEBUG firmware + flash (USB logging, ~2.5mA)
#    make flash-prod         — build PRODUCTION firmware + flash (<1mA sleep)
#    make monitor            — open serial monitor (auto-detect port)
#    make clean              — remove build directory
#    make pristine           — remove build dir + force full CMake reconfigure
#    make erase              — erase Zigbee NVRAM (factory reset)
#    make help               — show this list
#
#  Build Modes:
#    DEBUG=y                 — USB logging enabled, PM_DEVICE disabled (~2.5mA)
#    DEBUG=n (default)       — Production mode, PM_DEVICE enabled (<1mA)
#
#  Overrides:
#    make flash   MOUNT=/media/youruser/NICENANO
#    make monitor PORT=/dev/ttyACM1
# ================================================================

# west board target — must include the SoC suffix for nRF Connect SDK 2.7+
BOARD     := nice_nano/nrf52840
BUILD_DIR := build
BAUD      := 115200
VENV      := ~/ncs/.venv

# Build mode: DEBUG=y for USB logging, DEBUG=n for production battery mode
DEBUG ?= n

UF2 := $(BUILD_DIR)/zephyr/zephyr.uf2

# Auto-detect serial port: find most recently created /dev/ttyACM* device
PORT ?= $(shell ls -t /dev/ttyACM* 2>/dev/null | head -n 1)

# Auto-detect the bootloader mount point.
# Checks /media/$USER/NICENANO then /run/media/$USER/NICENANO.
# Override with: make flash MOUNT=/path/to/NICENANO
MOUNT ?= $(shell \
	for base in /media/$(USER) /run/media/$(USER); do \
		mp="$$base/NICENANO"; \
		if [ -d "$$mp" ]; then echo "$$mp"; break; fi; \
	done)

.PHONY: all build flash flash-debug flash-prod pflash _copy_uf2 monitor clean pristine erase help _set_debug_mode _set_prod_mode

all: build

# ---- Build Mode Configuration ----------------------------------

_set_debug_mode:
	@echo "==> Configuring DEBUG mode (USB logging enabled, PM_DEVICE disabled)"
	@sed -i 's/^CONFIG_USB_DEVICE_STACK=n/CONFIG_USB_DEVICE_STACK=y/' prj.conf
	@sed -i 's/^CONFIG_LOG=n/CONFIG_LOG=y/' prj.conf
	@sed -i 's/^CONFIG_PM_DEVICE=y/# CONFIG_PM_DEVICE=y/' prj.conf

_set_prod_mode:
	@echo "==> Configuring PRODUCTION mode (battery only, USB/logging disabled)"
	@sed -i 's/^CONFIG_USB_DEVICE_STACK=y/CONFIG_USB_DEVICE_STACK=n/' prj.conf
	@sed -i 's/^CONFIG_LOG=y/CONFIG_LOG=n/' prj.conf
	@sed -i 's/^# CONFIG_PM_DEVICE=y/CONFIG_PM_DEVICE=y/' prj.conf

# ---- Build -----------------------------------------------------

$(VENV):
	python3 -m venv $(VENV)
	source $(VENV)/bin/activate && pip3 install -r requirements.txt
	@echo ""
	@echo "Virtual environment created. To activate manually:"
	@echo "  source $(VENV)/bin/activate"

build: $(VENV)
	@if [ "$(DEBUG)" = "y" ]; then \
		$(MAKE) --no-print-directory _set_debug_mode; \
	else \
		$(MAKE) --no-print-directory _set_prod_mode; \
	fi
	@rm -f $(UF2)
	source $(VENV)/bin/activate && west build -b $(BOARD) -d $(BUILD_DIR)
	@test -f $(UF2) || { echo "ERROR: build finished but $(UF2) was not produced!"; exit 1; }
	@echo ""
	@if [ "$(DEBUG)" = "y" ]; then \
		echo "✓ DEBUG firmware built (USB logging, ~2.5mA idle)"; \
	else \
		echo "✓ PRODUCTION firmware built (battery mode, <1mA idle)"; \
	fi

# ---- Flash -----------------------------------------------------

flash: build
	@echo ""
	@echo "┌─────────────────────────────────────────────────────┐"
	@echo "│  Double-tap RESET on the nice!nano NOW              │"
	@echo "│  Wait for the LED to pulse, then press ENTER        │"
	@echo "└─────────────────────────────────────────────────────┘"
	@read _dummy
	@$(MAKE) --no-print-directory _copy_uf2

flash-debug:
	@$(MAKE) --no-print-directory flash DEBUG=y

flash-prod:
	@$(MAKE) --no-print-directory flash DEBUG=n

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
	@if [ -z "$(PORT)" ]; then \
		echo "ERROR: No /dev/ttyACM* device found."; \
		echo "  Is the device plugged in and running firmware?"; \
		exit 1; \
	fi
	@echo "Opening $(PORT) at $(BAUD) baud  (Ctrl-A then K to quit)"
	screen $(PORT) $(BAUD)

# ---- Clean / Pristine ------------------------------------------

clean:
	rm -rf $(BUILD_DIR)

# Erase Zigbee NVRAM + settings so the device forgets its network credentials.
# Does NOT touch firmware.  Requires nrfjprog (J-Link tools).
# Use this if the device is stuck in a rejoin loop with stale credentials.
#
# Partition layout (from pm_static.yml):
#   0xa6000–0xadfff  zboss_nvram        (32 KB)
#   0xae000–0xaefff  zboss_product_config (4 KB)
#   0xaf000–0xb0fff  settings_storage   (8 KB)
erase:
	@echo "Erasing Zigbee NVRAM and settings..."
	nrfjprog --eraserange 0xa6000 0xb0fff --snr $(shell nrfjprog --com | head -1 | awk '{print $$1}')
	@echo "Done. Device will rejoin network on next boot."

# A pristine rebuild forces CMake to reconfigure from scratch — useful
# after changing prj.conf, app.overlay, or board Kconfig/DTS.
pristine:
	@if [ "$(DEBUG)" = "y" ]; then \
		$(MAKE) --no-print-directory _set_debug_mode; \
	else \
		$(MAKE) --no-print-directory _set_prod_mode; \
	fi
	@rm -f $(UF2)
	source $(VENV)/bin/activate && west build -b $(BOARD) -d $(BUILD_DIR) --pristine=always
	@test -f $(UF2) || { echo "ERROR: pristine build finished but $(UF2) was not produced!"; exit 1; }
	@echo ""
	@if [ "$(DEBUG)" = "y" ]; then \
		echo "✓ DEBUG firmware built (USB logging, ~2.5mA idle)"; \
	else \
		echo "✓ PRODUCTION firmware built (battery mode, <1mA idle)"; \
	fi

# Pristine build + flash in one step
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
	@echo "╔═══════════════════════════════════════════════════════════════╗"
	@echo "║  nice!nano Zigbee Sensor — Build System                      ║"
	@echo "╚═══════════════════════════════════════════════════════════════╝"
	@echo ""
	@echo "Build & Flash:"
	@echo "  make build          Compile firmware (incremental)"
	@echo "  make flash          Build + flash (uses current mode)"
	@echo "  make flash-debug    Build + flash DEBUG firmware (USB logging)"
	@echo "  make flash-prod     Build + flash PRODUCTION firmware (battery)"
	@echo ""
	@echo "Development:"
	@echo "  make monitor        Open serial monitor (auto-detect port)"
	@echo "  make clean          Remove build directory"
	@echo "  make pristine       Full CMake reconfigure + rebuild"
	@echo "  make erase          Factory reset (erase Zigbee NVRAM)"
	@echo ""
	@echo "Build Modes:"
	@echo "  DEBUG=y             USB logging enabled, ~2.5mA idle"
	@echo "  DEBUG=n (default)   Battery optimized, <1mA idle"
	@echo ""
	@echo "Examples:"
	@echo "  make flash-debug              # Build & flash debug firmware"
	@echo "  make flash-prod               # Build & flash production firmware"
	@echo "  make build DEBUG=y            # Build debug firmware only"
	@echo "  make monitor                  # Open serial (auto-detect port)"
	@echo "  make monitor PORT=/dev/ttyACM2  # Override port"
	@echo ""
	@echo "Current Configuration:"
	@if grep -q '^CONFIG_PM_DEVICE=y' prj.conf 2>/dev/null; then \
		echo "  Mode: PRODUCTION (battery optimized, <1mA)"; \
	else \
		echo "  Mode: DEBUG (USB logging, ~2.5mA)"; \
	fi
	@if [ -n "$(PORT)" ]; then \
		echo "  Serial Port: $(PORT)"; \
	else \
		echo "  Serial Port: (none detected)"; \
	fi
	@if [ -n "$(MOUNT)" ]; then \
		echo "  Bootloader: $(MOUNT)"; \
	else \
		echo "  Bootloader: (not mounted)"; \
	fi
	@echo ""