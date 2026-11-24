# cmake/PrefixOunceVK.cmake
# From LostOdysseyRecomp (cmake/PrefixOunceVK.cmake), unchanged apart from this
# line. reblue runs it at configure time from cmake/switch2.cmake.
# Run with `cmake -P` (a build step, not included): writes a symbol-renamed copy
# of the statically linked Switch 2 Vulkan driver so its entry points do not
# collide with volk's function-pointer variables of the same name.
#
#   cmake -DNM=... -DOBJCOPY=... -DINPUT=libouncevk.a -DOUTPUT=libouncevk_prefixed.a
#         -DSYMS=libouncevk_prefixed.syms -DPREFIX=ouncevk_ -P cmake/PrefixOunceVK.cmake
#
# Why: plume vendors volk, which defines every Vulkan entry point as a global
# function-pointer *variable* (`PFN_vkCreateInstance vkCreateInstance;`, ...).
# Those definitions satisfy every reference to vkCreateInstance/vkCreateViSurfaceNN
# at link time, so the linker never pulls vulkan.o out of libouncevk.a and the
# image ends up with empty dispatch slots instead of the driver. Prefixing the
# driver's exported vk* symbols gives it its own namespace, so the archive is
# pulled by the reference to ouncevk_vkGetInstanceProcAddr and volk can fill its
# slots from it.
#
# objcopy rewrites the symbol table and the relocations together, so references
# between the driver's own functions stay consistent. Only names that the
# archive defines and exports are renamed, which leaves everything the driver
# still needs from libounce and libc untouched (`U gsvOpen`, `U viOpenDefault`,
# `U _Unwind_*`, ...).

foreach(_lo_required IN ITEMS NM OBJCOPY INPUT OUTPUT SYMS PREFIX)
    if(NOT DEFINED ${_lo_required} OR "${${_lo_required}}" STREQUAL "")
        message(FATAL_ERROR "PrefixOunceVK.cmake: ${_lo_required} is required")
    endif()
endforeach()

# Every defined, externally visible symbol the archive exports.
execute_process(
    COMMAND "${NM}" --defined-only --extern-only "${INPUT}"
    OUTPUT_VARIABLE _lo_nm_output
    ERROR_VARIABLE _lo_nm_error
    RESULT_VARIABLE _lo_nm_result)
if(NOT _lo_nm_result EQUAL 0)
    message(FATAL_ERROR "PrefixOunceVK.cmake: ${NM} on ${INPUT} failed (${_lo_nm_result}): ${_lo_nm_error}")
endif()

string(REPLACE "\r\n" "\n" _lo_nm_output "${_lo_nm_output}")
string(REPLACE "\n" ";" _lo_nm_lines "${_lo_nm_output}")

# Vulkan entry points (and any driver-internal vk* helper) become <PREFIX>vk*.
# `vk_icd*`-style lowercase names are deliberately not matched: there is no ICD
# trampoline in an OunceVK build.
set(_lo_renames "")
foreach(_lo_line IN LISTS _lo_nm_lines)
    if(_lo_line MATCHES "[ \t]([A-Za-z]) ([A-Za-z_][A-Za-z0-9_]*)$")
        set(_lo_type "${CMAKE_MATCH_1}")
        set(_lo_name "${CMAKE_MATCH_2}")
        if(_lo_name MATCHES "^vk[A-Z]" AND _lo_type MATCHES "^[TDBRWVGSC]$")
            string(APPEND _lo_renames "${_lo_name} ${PREFIX}${_lo_name}\n")
        endif()
    endif()
endforeach()

if(_lo_renames STREQUAL "")
    message(FATAL_ERROR "PrefixOunceVK.cmake: no vk* entry points found in ${INPUT}")
endif()

string(REGEX MATCHALL "[^\n]+" _lo_prefix_names "${_lo_renames}")
list(LENGTH _lo_prefix_names _lo_prefix_count)

# cmake keeps the file if objcopy fails; build into a temporary name so a
# failed rename never looks like an up-to-date output.
file(WRITE "${SYMS}" "${_lo_renames}")
file(REMOVE "${OUTPUT}.tmp")
execute_process(
    COMMAND "${OBJCOPY}" "--redefine-syms=${SYMS}" "${INPUT}" "${OUTPUT}.tmp"
    ERROR_VARIABLE _lo_objcopy_error
    RESULT_VARIABLE _lo_objcopy_result)
if(NOT _lo_objcopy_result EQUAL 0)
    file(REMOVE "${OUTPUT}.tmp")
    message(FATAL_ERROR "PrefixOunceVK.cmake: objcopy failed (${_lo_objcopy_result}): ${_lo_objcopy_error}")
endif()
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")

message(STATUS "PrefixOunceVK: ${_lo_prefix_count} entry points prefixed with '${PREFIX}' -> ${OUTPUT}")
