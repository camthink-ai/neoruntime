# CMake Toolchain file for Hailo SDK (Yocto Poky) cross-compilation
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-hailo.cmake \
#         -DHAILO_SDK_PATH=/opt/poky/4.0.23 ..
#
# Or set env: export HAILO_SDK_PATH=/opt/poky/4.0.23
#
# SDK directory layout (Yocto Poky):
#   <SDK>/sysroots/x86_64-pokysdk-linux/usr/bin/aarch64-poky-linux/  (host tools)
#   <SDK>/sysroots/armv8a-poky-linux/                                 (target sysroot)

# ---------- Resolve SDK path ----------

if(NOT DEFINED HAILO_SDK_PATH)
    if(DEFINED ENV{HAILO_SDK_PATH})
        set(HAILO_SDK_PATH "$ENV{HAILO_SDK_PATH}")
    else()
        message(FATAL_ERROR
            "HAILO_SDK_PATH not set. Pass -DHAILO_SDK_PATH=... or export HAILO_SDK_PATH=...")
    endif()
endif()

# Allow customization of sysroot names (defaults match Hailo SDK 4.0.x)
if(NOT DEFINED HAILO_HOST_SYSROOT_NAME)
    set(HAILO_HOST_SYSROOT_NAME "x86_64-pokysdk-linux")
endif()
if(NOT DEFINED HAILO_TARGET_SYSROOT_NAME)
    set(HAILO_TARGET_SYSROOT_NAME "armv8a-poky-linux")
endif()

set(HAILO_HOST_TOOLS "${HAILO_SDK_PATH}/sysroots/${HAILO_HOST_SYSROOT_NAME}/usr/bin/aarch64-poky-linux")
set(HAILO_TARGET_SYSROOT "${HAILO_SDK_PATH}/sysroots/${HAILO_TARGET_SYSROOT_NAME}")

# ---------- System ----------

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ---------- Sysroot ----------

set(CMAKE_SYSROOT "${HAILO_TARGET_SYSROOT}")

# ---------- Compilers ----------

set(CMAKE_C_COMPILER   "${HAILO_HOST_TOOLS}/aarch64-poky-linux-gcc")
set(CMAKE_CXX_COMPILER "${HAILO_HOST_TOOLS}/aarch64-poky-linux-g++")
set(CMAKE_AR           "${HAILO_HOST_TOOLS}/aarch64-poky-linux-ar")
set(CMAKE_RANLIB       "${HAILO_HOST_TOOLS}/aarch64-poky-linux-ranlib")
set(CMAKE_STRIP        "${HAILO_HOST_TOOLS}/aarch64-poky-linux-strip")
set(CMAKE_LINKER       "${HAILO_HOST_TOOLS}/aarch64-poky-linux-ld")
set(CMAKE_NM           "${HAILO_HOST_TOOLS}/aarch64-poky-linux-nm")
set(CMAKE_OBJCOPY      "${HAILO_HOST_TOOLS}/aarch64-poky-linux-objcopy")
set(CMAKE_OBJDUMP      "${HAILO_HOST_TOOLS}/aarch64-poky-linux-objdump")

# ---------- Compiler flags (match Yocto defaults) ----------

set(HAILO_COMMON_FLAGS "-mbranch-protection=standard -fstack-protector-strong -O2 -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Werror=format-security")

set(CMAKE_C_FLAGS_INIT   "${HAILO_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${HAILO_COMMON_FLAGS}")

# ---------- Search path configuration ----------

# Only search for libraries and headers in the target sysroot
set(CMAKE_FIND_ROOT_PATH "${HAILO_TARGET_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---------- RPATH for installed binaries ----------

set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib/hal")
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
