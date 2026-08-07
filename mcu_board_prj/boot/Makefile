######################################
# Bootloader + OTA module — aligns with STM32CubeIDE Debug (STM32G0B0RETx)
######################################
include ../stm32g0_toolchain.mk

rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

TARGET ?= ne503_mcu_boot
BUILD_DIR := build
LDSCRIPT := STM32G0B0RETX_FLASH.ld

######################################
# Sources (same file set as boot/Debug/*.mk)
######################################
C_SOURCES :=
C_SOURCES += Core/Src/main.c
C_SOURCES += Core/Src/stm32g0xx_it.c
C_SOURCES += Core/Src/stm32g0xx_hal_msp.c
C_SOURCES += Core/Src/system_stm32g0xx.c
C_SOURCES += Core/Src/syscalls.c
C_SOURCES += Core/Src/sysmem.c
C_SOURCES += Core/Src/gpio.c
C_SOURCES += Core/Src/dma.c
C_SOURCES += Core/Src/crc.c
C_SOURCES += Core/Src/iwdg.c
C_SOURCES += Core/Src/tim.c
C_SOURCES += Core/Src/usart.c
C_SOURCES += Core/Src/boot_ymodem.c

C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_cortex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_crc.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_crc_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_dma.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_dma_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_exti.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_flash.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_flash_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_gpio.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_iwdg.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_pwr.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_pwr_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_rcc.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_rcc_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_tim.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_tim_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_uart.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_uart_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_ll_rcc.c

C_SOURCES += ota_module/src/ota_module.c
C_SOURCES += ota_module/src/ota_record_default.c
C_SOURCES += ota_module/platform/stm32/stm32g0xx_port.c

ASM_SOURCES := Core/Startup/startup_stm32g0b0retx.s

######################################
C_DEFS := $(BOOT_DEBUG_DEFS)
C_DEFS += -DUSE_HAL_DRIVER
C_DEFS += -DSTM32G0B0xx

C_INCLUDES := \
	-ICore/Inc \
	-IDrivers/STM32G0xx_HAL_Driver/Inc \
	-IDrivers/STM32G0xx_HAL_Driver/Inc/Legacy \
	-IDrivers/CMSIS/Device/ST/STM32G0xx/Include \
	-IDrivers/CMSIS/Include \
	-Iota_module/inc

AS_DEFS := $(BOOT_DEBUG_DEFS)
AS_INCLUDES := -Iota_module/inc

LDFLAGS := $(COMMON_LDFLAGS) -T$(LDSCRIPT) \
	-Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref

######################################
OBJECTS := $(addprefix $(BUILD_DIR)/,$(C_SOURCES:.c=.o))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(ASM_SOURCES:.s=.o))

.PHONY: all clean info build-dirs

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin

build-dirs: | $(BUILD_DIR)
	@mkdir -p $(sort $(dir $(OBJECTS)))

$(OBJECTS): | build-dirs

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	@echo "CC $<"
	@$(CC) -c $(COMMON_CFLAGS) $(C_DEFS) $(C_INCLUDES) -MMD -MP -MF$(@:.o=.d) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	@echo "AS $<"
	@$(CC) -c $(COMMON_ASFLAGS) $(AS_DEFS) $(AS_INCLUDES) -x assembler-with-cpp -MMD -MP -MF$(@:.o=.d) $< -o $@

$(BUILD_DIR)/$(TARGET).list: $(OBJECTS)
	$(file > $@,$(OBJECTS))

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile $(LDSCRIPT) $(BUILD_DIR)/$(TARGET).list
	@echo "LD $@"
	@$(CC) @$(BUILD_DIR)/$(TARGET).list $(LDFLAGS) -o $@
	@$(SZ) $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	@echo "BIN $@"
	@$(BIN) $< $@

$(BUILD_DIR):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD_DIR)

info:
	@echo "TARGET=$(TARGET) BUILD_DIR=$(BUILD_DIR)"

-include $(call rwildcard,$(BUILD_DIR),*.d)
