######################################
# Shared STM32G0 (Cortex-M0+) toolchain flags
# Included by root Makefile (export) and used as fallback by app/boot.
######################################
# Use := so we do not keep Make's default CC=cc (see GNU Make implicit vars).
PREFIX := arm-none-eabi-

ifdef GCC_PATH
CC := $(GCC_PATH)/$(PREFIX)gcc
AS := $(GCC_PATH)/$(PREFIX)gcc -x assembler-with-cpp
CP := $(GCC_PATH)/$(PREFIX)objcopy
SZ := $(GCC_PATH)/$(PREFIX)size
else
CC := $(PREFIX)gcc
AS := $(PREFIX)gcc -x assembler-with-cpp
CP := $(PREFIX)objcopy
SZ := $(PREFIX)size
endif

HEX := $(CP) -O ihex
BIN := $(CP) -O binary

RELEASE ?= 0
ifeq ($(RELEASE),1)
OPT_CFLAGS := -g0 -Os
OPT_ASFLAGS := -g0
APP_DEBUG_DEFS :=
BOOT_DEBUG_DEFS :=
else
OPT_CFLAGS := -g3 -O0
OPT_ASFLAGS := -g3
APP_DEBUG_DEFS := -DDEBUG
BOOT_DEBUG_DEFS := -DDEBUG
endif

MCU_CPU := cortex-m0plus
MCU_FLAGS := -mcpu=$(MCU_CPU) -mthumb -mfloat-abi=soft

# Match STM32CubeIDE (GNU Tools for STM32): nano.specs, sections, warnings
COMMON_CFLAGS := $(MCU_FLAGS) $(OPT_CFLAGS) -std=gnu11 \
	-ffunction-sections -fdata-sections -Wall -fstack-usage \
	--specs=nano.specs

COMMON_ASFLAGS := $(MCU_FLAGS) $(OPT_ASFLAGS) --specs=nano.specs

# nosys + nano per Cube-generated link lines
COMMON_LDFLAGS := $(MCU_FLAGS) --specs=nosys.specs --specs=nano.specs \
	-static -Wl,--gc-sections -Wl,--start-group -lc -lm -Wl,--end-group
