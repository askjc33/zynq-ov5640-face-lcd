# SPDX-License-Identifier: MIT
#
# PetaLinux/Yocto supplies the compiler, sysroot and search paths in its own
# OEToolchainConfig.cmake.  Its processor string is more specific than "arm"
# on this SDK, while NCNN 20260526 recognises ARM only when the string starts
# with "arm" or "aarch64".  Include the SDK configuration first, then
# normalise only the processor name used for NCNN architecture selection.

if(NOT DEFINED ENV{OECORE_NATIVE_SYSROOT})
    message(FATAL_ERROR
        "OECORE_NATIVE_SYSROOT is unset; source the PetaLinux SDK environment first")
endif()

set(_oe_toolchain
    "$ENV{OECORE_NATIVE_SYSROOT}/usr/share/cmake/OEToolchainConfig.cmake")

if(NOT EXISTS "${_oe_toolchain}")
    message(FATAL_ERROR "PetaLinux CMake toolchain not found: ${_oe_toolchain}")
endif()

include("${_oe_toolchain}")

set(CMAKE_SYSTEM_NAME Linux CACHE STRING "Target operating system" FORCE)
set(CMAKE_SYSTEM_PROCESSOR arm CACHE STRING "NCNN target processor" FORCE)
