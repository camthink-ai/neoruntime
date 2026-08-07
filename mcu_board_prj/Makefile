######################################
# mcu_board_prj — root build (see ne301 layout)
######################################
PROJECT_NAME ?= mcu_board

BUILD_DIR := build
APP_DIR := app
BOOT_DIR := boot
PYTHON ?= python3

# Artifact names match our Makefiles outputs
APP_TARGET := ne503_mcu_app
BOOT_TARGET := ne503_mcu_boot

include stm32g0_toolchain.mk

export CC AS CP SZ HEX BIN
export COMMON_CFLAGS COMMON_ASFLAGS COMMON_LDFLAGS
export MCU_FLAGS RELEASE

.PHONY: all app app-build boot pack-hex bundle clean clean-app clean-boot rebuild rebuild-app rebuild-boot info help

# Default: bootloader + OTA-headed app package + combined HEX (no bare app .bin in $(BUILD_DIR)/)
all: $(BUILD_DIR) bundle
	@echo "========================================="
	@echo "Build complete (delivery artifacts)"
	@echo "  Boot:       $(BUILD_DIR)/$(BOOT_TARGET).bin"
	@echo "  OTA image:  $(BUILD_DIR)/ne503_ota_package_v*.bin (see pack_ota output)"
	@echo "  Combined:   $(BUILD_DIR)/ne503_Main_v*_YYYYMMDD.hex"
	@echo "  (raw app .bin is only under $(APP_DIR)/build/ — not copied here)"
	@echo "========================================="

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Compile application only (raw .bin under $(APP_DIR)/build/)
app-build: $(BUILD_DIR)
	@echo "======== Building app ========"
	+@$(MAKE) -C $(APP_DIR)

# Build app firmware + OTA package (ota_package_header_t + payload) → $(BUILD_DIR)/ne503_ota_package_v*.bin
app: app-build
	@echo "======== Packing OTA package ========"
	@$(PYTHON) tools/pack_ota.py --app-bin $(APP_DIR)/build/$(APP_TARGET).bin --out $(BUILD_DIR)/ne503_ota_package.bin

boot: $(BUILD_DIR)
	@echo "======== Building boot ========"
	+@$(MAKE) -C $(BOOT_DIR)
	@cp $(BOOT_DIR)/build/$(BOOT_TARGET).bin $(BUILD_DIR)/ 2>/dev/null || true
	@cp $(BOOT_DIR)/build/$(BOOT_TARGET).elf $(BUILD_DIR)/ 2>/dev/null || true

pack-hex: boot app
	@echo "======== Packing combined HEX ========"
	@$(PYTHON) tools/pack_to_hex.py -o $(BUILD_DIR)/ne503_Main.hex

# Explicit alias used by default target
bundle: pack-hex

clean-app:
	@$(MAKE) -C $(APP_DIR) clean

clean-boot:
	@$(MAKE) -C $(BOOT_DIR) clean

clean: clean-app clean-boot clean-bin

clean-bin:
	@rm -rf $(BUILD_DIR)

rebuild: clean all

rebuild-app: clean-app app

rebuild-boot: clean-boot boot

info:
	@echo "PROJECT_NAME=$(PROJECT_NAME)"
	@echo "APP_TARGET=$(APP_TARGET) BOOT_TARGET=$(BOOT_TARGET)"
	@echo "PYTHON=$(PYTHON)"
	@echo "RELEASE=$(RELEASE) (set RELEASE=1 for -Os)"
	@$(CC) --version | head -n 1

help:
	@echo "Targets: all | bundle | app | app-build | boot | clean | rebuild"
	@echo "  make             — boot + OTA package + ne503_Main_v*_YYYYMMDD.hex (no bare app.bin in $(BUILD_DIR)/)"
	@echo "  make -j8         — parallel compile where dependencies allow"
	@echo "  make app         — compile app + write $(BUILD_DIR)/ne503_ota_package_v*.bin"
	@echo "  make app-build   — compile app only ($(APP_DIR)/build/*.bin)"
	@echo "  make boot        — bootloader only ($(BUILD_DIR)/$(BOOT_TARGET).bin)"
	@echo "  RELEASE=1 make   — Release-ish (-Os, no DEBUG)"
	@echo "Optional: GCC_PATH=<toolchain bin dir> (same idea as ne301)"
