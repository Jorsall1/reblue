# cmake/switch2.cmake
# Nintendo Switch 2 (kaiHEN, libounce 20261117, OunceVK 20261123) settings.
# Included from the top-level CMakeLists.txt in place of cmake/switch.cmake
# when the toolchain (cmake/toolchains/switch2-libounce.cmake) sets
# REXGLUE_PLATFORM_SWITCH2. It sets the Switch 2 defaults, includes
# switch.cmake for everything the two consoles share, then defines the GPU
# library the SDK links into the title: OunceVK, symbol-prefixed for volk.
#
# What differs from the Switch 1 build (reblueNX), and where:
#   guest memory   Backend::Heap, one 4 GiB memalign block, no svcMap* syscalls
#                  (thirdparty/rexglue-sdk/src/core/guest_memory_switch2.cpp)
#   exit           back to hbmenu through kaiHEN's loader, R hook kept
#                  (thirdparty/rexglue-sdk/src/ui/switch2_exit.cpp)
#   GPU            plume Vulkan on OunceVK, VK_NN_vi_surface on nwindowGetDefault();
#                  deko3d gone; vkCmdClearAttachments split into <= 80 rects
#                  (tools/switch/patches/plume-switch2.patch, ouncevk#2)
# See AGENTS.md for the invariants.

if(NOT REXGLUE_PLATFORM_SWITCH2)
    message(FATAL_ERROR "cmake/switch2.cmake is for the Switch 2 toolchain (switch2-libounce.cmake)")
endif()

# Host-stage outputs of tools/switch2/host-stage.sh (native WSL, no Docker).
set(REBLUE_SWITCH_HOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/out/switch2-host" CACHE PATH
    "Output folder of the Switch 2 host stage (shader cache, host tools)")
set(REBLUE_SWITCH_SHADER_CACHE "${REBLUE_SWITCH_HOST_DIR}/shader_cache.cpp" CACHE FILEPATH
    "XenosRecomp output made on the host")
set(REBLUE_SWITCH_APP_TITLE "re:Blue (Switch 2)" CACHE STRING "NRO title")

# No deko3d on Switch 2 (the toolchain forces these too).
set(REBLUE_SWITCH_DEKO3D OFF CACHE BOOL "No deko3d on Switch 2" FORCE)
set(REXGLUE_SWITCH_DEKO3D OFF CACHE BOOL "No deko3d on Switch 2" FORCE)

include("${CMAKE_CURRENT_LIST_DIR}/switch.cmake")

# ---------------------------------------------------------------- OunceVK
# libvulkan.a in portlibs/switch2 is only INPUT(-louncevk -lstdc++ -lm). The
# archive cannot be linked as is: plume vendors volk, whose global
# function-pointer variables (PFN_vkCreateInstance vkCreateInstance; ...)
# satisfy every vk* reference, so the linker never pulls the driver out of
# libouncevk.a and the image keeps empty dispatch slots. A copy with the
# driver's vk* exports renamed to ouncevk_vk* is linked instead, every entry
# point is kept with -u (volk resolves them at run time, --gc-sections would
# drop them), and plume initialises volk from ouncevk_vkGetInstanceProcAddr.
# Same scheme as LostOdysseyRecomp (cmake/LoSwitch.cmake).
set(REBLUE_OUNCEVK_ARCHIVE "${DEVKITPRO}/portlibs/switch2/lib/libouncevk.a" CACHE FILEPATH
    "OunceVK static Vulkan driver (libouncevk.a)")
set(REBLUE_OUNCEVK_PREFIX "ouncevk_" CACHE STRING "Symbol prefix for OunceVK's vk* entry points")
if(NOT EXISTS "${REBLUE_OUNCEVK_ARCHIVE}")
    message(FATAL_ERROR "OunceVK not found: ${REBLUE_OUNCEVK_ARCHIVE} (dkp-pacman -U ouncevk-20261123-1-any.pkg.tar.zst)")
endif()

set(_reblue_ouncevk_dir "${CMAKE_BINARY_DIR}/ouncevk")
set(REBLUE_OUNCEVK_PREFIXED "${_reblue_ouncevk_dir}/libouncevk_prefixed.a")
set(_reblue_ouncevk_syms "${_reblue_ouncevk_dir}/libouncevk_prefixed.syms")
file(MAKE_DIRECTORY "${_reblue_ouncevk_dir}")
# Regenerated at configure time whenever the installed archive changes (an
# OunceVK upgrade reconfigures through CMAKE_CONFIGURE_DEPENDS). Keyed on the
# archive's SHA-256, not its mtime: dkp-pacman keeps the package's file times,
# so an upgraded libouncevk.a can be older than the copy prefixed from the
# previous release.
set(_reblue_ouncevk_stamp "${_reblue_ouncevk_dir}/libouncevk_prefixed.sha256")
file(SHA256 "${REBLUE_OUNCEVK_ARCHIVE}" _reblue_ouncevk_sha)
set(_reblue_ouncevk_old_sha "")
if(EXISTS "${_reblue_ouncevk_stamp}")
    file(READ "${_reblue_ouncevk_stamp}" _reblue_ouncevk_old_sha)
    string(STRIP "${_reblue_ouncevk_old_sha}" _reblue_ouncevk_old_sha)
endif()
if(NOT EXISTS "${REBLUE_OUNCEVK_PREFIXED}" OR NOT EXISTS "${_reblue_ouncevk_syms}" OR
   NOT _reblue_ouncevk_old_sha STREQUAL _reblue_ouncevk_sha)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${CMAKE_NM}" "-DOBJCOPY=${CMAKE_OBJCOPY}"
            "-DINPUT=${REBLUE_OUNCEVK_ARCHIVE}" "-DOUTPUT=${REBLUE_OUNCEVK_PREFIXED}"
            "-DSYMS=${_reblue_ouncevk_syms}" "-DPREFIX=${REBLUE_OUNCEVK_PREFIX}"
            -P "${CMAKE_CURRENT_LIST_DIR}/PrefixOunceVK.cmake"
        RESULT_VARIABLE _reblue_prefix_result)
    if(NOT _reblue_prefix_result EQUAL 0)
        message(FATAL_ERROR "PrefixOunceVK.cmake failed (${_reblue_prefix_result})")
    endif()
    file(WRITE "${_reblue_ouncevk_stamp}" "${_reblue_ouncevk_sha}\n")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REBLUE_OUNCEVK_ARCHIVE}")

# -u for every renamed entry point (the second column of the syms file).
file(STRINGS "${_reblue_ouncevk_syms}" _reblue_ouncevk_lines)
set(_reblue_ouncevk_keep "")
foreach(_line IN LISTS _reblue_ouncevk_lines)
    if(_line MATCHES "^[^ ]+ ([^ ]+)$")
        list(APPEND _reblue_ouncevk_keep "LINKER:-u,${CMAKE_MATCH_1}")
    endif()
endforeach()
list(LENGTH _reblue_ouncevk_keep _reblue_ouncevk_count)
if(_reblue_ouncevk_count EQUAL 0)
    message(FATAL_ERROR "No OunceVK entry points in ${_reblue_ouncevk_syms}")
endif()

# rex::switch_gpu: what rexglue_switch_configure_target links into the title
# (thirdparty/rexglue-sdk/cmake/rexglue_switch.cmake skips Mesa NVK on Switch 2).
add_library(reblue_ouncevk INTERFACE)
add_library(rex::switch_gpu ALIAS reblue_ouncevk)
target_link_libraries(reblue_ouncevk INTERFACE "${REBLUE_OUNCEVK_PREFIXED}" nx stdc++ m)
target_link_options(reblue_ouncevk INTERFACE -pthread ${_reblue_ouncevk_keep})

# The ouncevk#2 clear split lives in plume (plume-switch2.patch): refuse a plume
# tree the host stage has not patched, rather than ship the PBDMA crash.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/plume/plume_vulkan.cpp" _reblue_clear_split
    REGEX "cmdClearAttachmentsChunked" LIMIT_COUNT 1)
if(NOT _reblue_clear_split)
    message(FATAL_ERROR
        "thirdparty/plume is missing plume-switch2.patch (the <= 80 rect vkCmdClearAttachments split, ouncevk#2). "
        "Run tools/switch2/host-stage.sh first.")
endif()

message(STATUS "reblue: Nintendo Switch 2 build (libounce, OunceVK ${REBLUE_OUNCEVK_ARCHIVE} "
    "sha256 ${_reblue_ouncevk_sha}, ${_reblue_ouncevk_count} entry points prefixed '${REBLUE_OUNCEVK_PREFIX}')")
