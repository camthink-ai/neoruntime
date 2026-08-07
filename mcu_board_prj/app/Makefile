######################################
# Application — aligns with STM32CubeIDE Debug (STM32G0B0RETx + FreeRTOS)
######################################
include ../stm32g0_toolchain.mk

rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

TARGET ?= ne503_mcu_app
BUILD_DIR := build
LDSCRIPT := STM32G0B0RETX_FLASH.ld

######################################
# Sources (same file set as app/Debug/*.mk)
######################################
C_SOURCES :=
# Core
C_SOURCES += Core/Src/main.c
C_SOURCES += Core/Src/stm32g0xx_it.c
C_SOURCES += Core/Src/app_freertos.c
C_SOURCES += Core/Src/stm32g0xx_hal_msp.c
C_SOURCES += Core/Src/stm32g0xx_hal_timebase_tim.c
C_SOURCES += Core/Src/system_stm32g0xx.c
C_SOURCES += Core/Src/syscalls.c
C_SOURCES += Core/Src/sysmem.c
C_SOURCES += Core/Src/gpio.c
C_SOURCES += Core/Src/dma.c
C_SOURCES += Core/Src/adc.c
C_SOURCES += Core/Src/crc.c
C_SOURCES += Core/Src/spi.c
C_SOURCES += Core/Src/iwdg.c
C_SOURCES += Core/Src/rtc.c
C_SOURCES += Core/Src/tim.c
C_SOURCES += Core/Src/usart.c

# FreeRTOS
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/croutine.c
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/event_groups.c
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/list.c
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/queue.c
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/tasks.c
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/timers.c
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM0/port.c
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c
C_SOURCES += Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c

# HAL / LL (Cube-selected drivers)
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_adc.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_adc_ex.c
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
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_rtc.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_rtc_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_spi.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_spi_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_tim.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_tim_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_uart.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_uart_ex.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_ll_adc.c
C_SOURCES += Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_ll_rcc.c

# Custom (Cube Debug excludes POSIX host_link / ms_bridging / an41908 sources)
C_SOURCES += Custom/User/app.c
C_SOURCES += Custom/User/cmd_test.c
C_SOURCES += Custom/User/host_link_app.c
C_SOURCES += Custom/Lib/wic_log/wic_log.c
C_SOURCES += Custom/Lib/nr_micro_shell/src/nr_micro_shell_core.c
C_SOURCES += Custom/Lib/nr_micro_shell/src/nr_micro_shell_cmds.c
C_SOURCES += Custom/Lib/cJSON/cJSON.c
C_SOURCES += Custom/Components/rs485_driver/rs485_driver.c
C_SOURCES += Custom/Components/ms41908m/ms41908m.c
C_SOURCES += Custom/Components/ms41908m/ms41908m_port.c
C_SOURCES += Custom/Components/host_link/host_link_core.c
C_SOURCES += Custom/Components/host_link/host_link_port_freertos.c
C_SOURCES += Custom/Components/cmd_console/cmd_console.c
C_SOURCES += Custom/Components/bsp_ctrl/bsp_ctrl.c

# OTA (shared with boot)
C_SOURCES += ../boot/ota_module/src/ota_module.c
C_SOURCES += ../boot/ota_module/src/ota_record_default.c
C_SOURCES += ../boot/ota_module/platform/stm32/stm32g0xx_port.c
C_SOURCES += Custom/Components/ota_host/ota_host.c

ASM_SOURCES := Core/Startup/startup_stm32g0b0retx.s

######################################
# Defines / includes
######################################
C_DEFS := $(APP_DEBUG_DEFS)
C_DEFS += '-DCMSIS_device_header=<stm32g0xx.h>'
C_DEFS += -DUSE_HAL_DRIVER
C_DEFS += -DSTM32G0B0xx

C_INCLUDES := \
	-ICore/Inc \
	-IDrivers/STM32G0xx_HAL_Driver/Inc \
	-IDrivers/STM32G0xx_HAL_Driver/Inc/Legacy \
	-IMiddlewares/Third_Party/FreeRTOS/Source/include \
	-IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 \
	-IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM0 \
	-IDrivers/CMSIS/Device/ST/STM32G0xx/Include \
	-IDrivers/CMSIS/Include \
	-ICustom/Components/cmd_console \
	-ICustom/Lib/nr_micro_shell/inc \
	-ICustom/Lib/wic_log \
	-ICustom/User \
	-ICustom/Components/bsp_ctrl \
	-ICustom/Components/an41908a_af \
	-ICustom/Components/ms41908m \
	-ICustom/Components/host_link \
	-ICustom/Components/rs485_driver \
	-ICustom/Components/ota_host \
	-I../boot/ota_module/inc

AS_DEFS := $(APP_DEBUG_DEFS)

AS_INCLUDES := \
	-ICore/Inc \
	-IDrivers/STM32G0xx_HAL_Driver/Inc \
	-IDrivers/STM32G0xx_HAL_Driver/Inc/Legacy \
	-IMiddlewares/Third_Party/FreeRTOS/Source/include \
	-IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 \
	-IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM0 \
	-IDrivers/CMSIS/Device/ST/STM32G0xx/Include \
	-IDrivers/CMSIS/Include \
	-ICustom/Components/cmd_console \
	-ICustom/Lib/nr_micro_shell/inc \
	-ICustom/Lib/wic_log \
	-ICustom/User \
	-ICustom/Components/bsp_ctrl \
	-ICustom/Components/an41908a_af \
	-ICustom/Components/ms41908m \
	-ICustom/Components/host_link \
	-ICustom/Components/rs485_driver \
	-ICustom/Components/ota_host \
	-I../boot/ota_module/inc

LDFLAGS := $(COMMON_LDFLAGS) -T$(LDSCRIPT) \
	-Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref \
	-u _printf_float

######################################
# Objects / rules
######################################
#
# NOTE:
# Some sources are referenced via "../boot/..." (shared ota_module).
# If we naively do "build/../boot/..." the build outputs end up inside the source tree
# at "app/boot/...", polluting the repo. We therefore map every source file to a
# *mangled* object path under $(BUILD_DIR)/obj/.
#
OBJ_DIR := $(BUILD_DIR)/obj

define mangle_path
$(subst :,_,$(subst ..,UP,$(subst /,_,$(1))))
endef

OBJECTS := $(foreach s,$(C_SOURCES),$(OBJ_DIR)/$(call mangle_path,$(s:.c=)).o)
OBJECTS += $(foreach s,$(ASM_SOURCES),$(OBJ_DIR)/$(call mangle_path,$(s:.s=)).o)

.PHONY: all clean info build-dirs

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin

build-dirs: | $(BUILD_DIR)
	@mkdir -p $(sort $(dir $(OBJECTS)))

$(OBJECTS): | build-dirs

define CC_RULE
$(2): $(1) Makefile | build-dirs
	@echo "CC $(1)"
	@$(CC) -c $(COMMON_CFLAGS) $(C_DEFS) $(C_INCLUDES) -MMD -MP -MF$(2:.o=.d) $(1) -o $(2)
endef

define AS_RULE
$(2): $(1) Makefile | build-dirs
	@echo "AS $(1)"
	@$(CC) -c $(COMMON_ASFLAGS) $(AS_DEFS) $(AS_INCLUDES) -x assembler-with-cpp -MMD -MP -MF$(2:.o=.d) $(1) -o $(2)
endef

$(foreach s,$(C_SOURCES),$(eval $(call CC_RULE,$(s),$(OBJ_DIR)/$(call mangle_path,$(s:.c=)).o)))
$(foreach s,$(ASM_SOURCES),$(eval $(call AS_RULE,$(s),$(OBJ_DIR)/$(call mangle_path,$(s:.s=)).o)))

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
	@echo "C sources: $(words $(C_SOURCES))  ASM: $(words $(ASM_SOURCES))"

-include $(call rwildcard,$(BUILD_DIR),*.d)
