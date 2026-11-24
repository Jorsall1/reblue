# Nintendo Switch 2 (kaiHEN) cross toolchain: devkitA64 + libounce, GPU through
# OunceVK (Mesa NVK for Switch 2, linked statically). See AGENTS.md and
# cmake/switch2.cmake.
#
#   export DEVKITPRO=/opt/devkitpro
#   cmake -S . -B out/switch2-build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/switch2-libounce.cmake \
#     -DCMAKE_BUILD_TYPE=Release
#   (tools/switch2/build-switch2.sh does this)
#
# Modeled on LostOdysseyRecomp's cmake/toolchains/switch2-libounce.cmake and
# on libounce's own $DEVKITPRO/libounce/cmake/Switch2.cmake. Every Switch 2
# value comes from libounce, as Switch2.cmake does: the platform root
# (libounce in place of libnx), its linker specs, the CPU flags in
# libounce/switch2_cpu.mk, __SWITCH2__, and the Switch 2 portlibs (OunceVK)
# before the Switch 1 portlibs.
#
# Unlike Switch2.cmake (and like reblue's Switch 1 toolchain,
# switch-devkitA64.cmake), CMAKE_SYSTEM_NAME stays Generic: the ReXGlue SDK's
# CMake keys its Switch branches off REXGLUE_PLATFORM_SWITCH with neither UNIX
# nor WIN32 set, and devkitPro's NintendoSwitch platform would change that.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(REXGLUE_PLATFORM_SWITCH ON CACHE BOOL "Build rexglue for Nintendo Switch (libnx API: libounce)")
set(REXGLUE_PLATFORM_SWITCH2 ON CACHE BOOL "Build rexglue for Nintendo Switch 2 (libounce + OunceVK, kaiHEN)")
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Tracy is unavailable on Switch" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "No perf counters on Switch" FORCE)
# OunceVK is the only renderer on Switch 2: deko3d (the Switch 1 GPU API) is gone.
set(REXGLUE_SWITCH_DEKO3D OFF CACHE BOOL "No deko3d on Switch 2" FORCE)
set(REBLUE_SWITCH_DEKO3D OFF CACHE BOOL "No deko3d on Switch 2" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "OunceVK" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "No D3D12 on Switch" FORCE)

if(NOT DEFINED DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "devkitPro root")
    else()
        set(DEVKITPRO "/opt/devkitpro" CACHE PATH "devkitPro root")
    endif()
endif()
set(_LIBOUNCE "${DEVKITPRO}/libounce")
set(_OUNCE_PORTLIBS "${DEVKITPRO}/portlibs/switch2")
if(NOT EXISTS "${_LIBOUNCE}/lib/libounce.a")
    message(FATAL_ERROR "libounce not found under ${_LIBOUNCE} (install the Switch 2 devkit or set DEVKITPRO)")
endif()

set(_DEVKITA64 "${DEVKITPRO}/devkitA64")
set(_DEVKITA64_BIN "${_DEVKITA64}/bin")
set(_DEVKITA64_PREFIX "aarch64-none-elf-")
set(CMAKE_C_COMPILER "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}g++" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc-ranlib" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}nm" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}objcopy" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}strip" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}" CACHE FILEPATH "" FORCE)

# The Switch 2's CPU (Cortex-A78C), from the one place libounce sets it.
file(STRINGS "${_LIBOUNCE}/switch2_cpu.mk" _SWITCH2_CPU REGEX "^SWITCH2_CPU[ \t]*:=")
string(REGEX REPLACE "^SWITCH2_CPU[ \t]*:=[ \t]*" "" _SWITCH2_CPU "${_SWITCH2_CPU}")
if(NOT _SWITCH2_CPU)
    message(FATAL_ERROR "No SWITCH2_CPU in ${_LIBOUNCE}/switch2_cpu.mk")
endif()

# libounce ABI (as switch2vars.sh: -mtp=soft, local-exec TLS). __SWITCH__ stays
# defined (libounce is libnx's API), __SWITCH2__ selects the kaiHEN code paths.
# -fPIE as the Switch 1 build: the NRO is a static PIE.
set(_SWITCH2_ARCH_FLAGS "${_SWITCH2_CPU} -mtp=soft -ftls-model=local-exec -fPIE -D__SWITCH__ -D__SWITCH2__ -DNX -D_GNU_SOURCE -DSPDLOG_NO_TZ_OFFSET")
set(CMAKE_C_FLAGS_INIT "-ffunction-sections -fdata-sections ${_SWITCH2_ARCH_FLAGS} -Wno-psabi")
set(CMAKE_CXX_FLAGS_INIT "-ffunction-sections -fdata-sections ${_SWITCH2_ARCH_FLAGS} -Wno-psabi")
set(CMAKE_ASM_FLAGS_INIT "${_SWITCH2_ARCH_FLAGS}")

# libounce's switch.specs resolves switch.ld through GCC's getenv spec
# function: write a build-local copy with the absolute path, as the Switch 1
# toolchain does for libnx's.
file(READ "${_LIBOUNCE}/switch.specs" _SWITCH2_SPECS_CONTENT)
string(REPLACE "%:getenv(DEVKITPRO /libounce/switch.ld)" "${_LIBOUNCE}/switch.ld"
    _SWITCH2_SPECS_CONTENT "${_SWITCH2_SPECS_CONTENT}")
set(_SWITCH2_SPECS_CONFIGURED "${CMAKE_BINARY_DIR}/switch2.specs")
file(WRITE "${_SWITCH2_SPECS_CONFIGURED}" "${_SWITCH2_SPECS_CONTENT}")

# --allow-multiple-definition: as on Switch 1 (OunceVK is Mesa, and its Rust
# static libraries each bundle the Rust runtime).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${_SWITCH2_SPECS_CONFIGURED} -L${_OUNCE_PORTLIBS}/lib -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--no-relax")
set(CMAKE_DL_LIBS "")

# libounce, then the Switch 2 portlibs (OunceVK's Vulkan headers), then the
# Switch 1 portlibs for libraries that only use the C library.
include_directories(SYSTEM
    "${_DEVKITA64}/include"
    "${_LIBOUNCE}/include"
    "${_OUNCE_PORTLIBS}/include"
    "${DEVKITPRO}/portlibs/switch/include")
link_directories(
    "${_DEVKITA64}/lib"
    "${_LIBOUNCE}/lib"
    "${_OUNCE_PORTLIBS}/lib"
    "${DEVKITPRO}/portlibs/switch/lib")

set(CMAKE_FIND_ROOT_PATH "${_DEVKITA64}" "${_LIBOUNCE}" "${_OUNCE_PORTLIBS}" "${DEVKITPRO}/portlibs/switch")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
