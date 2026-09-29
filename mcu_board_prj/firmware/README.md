# mcu_board_prj/firmware

Firmware delivery artifacts for the ne503 MCU (STM32G0, Cortex-M0+).
Both files are produced and copied here automatically by `make mcu-firmware`.

## Files

### `ne503_ota_package_v<version>.bin`
OTA download image. Layout: `ota_package_header_t` (56 bytes) + padding +
raw application payload. This is the image the device's OTA module consumes
(`ota_module_ota_download_start` / `ota_module_ota_download_finish`) to
perform an over-the-air update in the field.

### `ne503_Main_v<version>_<YYYYMMDD>.hex`
Combined Intel HEX for factory / bench programming via ST-Link
(e.g. STM32CubeProgrammer). Memory layout:
- Bootloader      @ `0x08000000`
- OTA record area @ `0x0800E000` (erased, `0xFF`)
- OTA package     @ `0x08010000`

## Which to use

- OTA update in the field        -> `ne503_ota_package_v*.bin`
- Full flash / factory programming -> `ne503_Main_v*.hex`
