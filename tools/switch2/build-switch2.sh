#!/usr/bin/env bash
# Stage 2 of the Nintendo Switch 2 build, natively in WSL: cross-compiles
# reblue.nro with devkitA64 + libounce + OunceVK, after
# tools/switch2/host-stage.sh has made generated/ and the shader cache.
#
#   bash tools/switch2/build-switch2.sh             configure + build
#   CONFIGURE_ONLY=1 bash tools/switch2/build-switch2.sh
#
# Environment:
#   JOBS=N          parallel jobs (default: CPUs, capped at ~1.5 GB of free RAM per job)
#   BUILD_TYPE=...  Release (default) or RelWithDebInfo
#   LTO=1           link-time optimisation of the game code
#   CLEAN=1         delete the build folder first
#   CONFIGURE_ONLY=1  stop after CMake configure
#   DEPLOY=path     copy reblue.nro there after the build
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO
BUILD_TYPE="${BUILD_TYPE:-Release}"
LTO="${LTO:-0}"
build="$root/out/switch2-build"
if [ -z "${JOBS:-}" ]; then
    avail_kb="$(awk '/MemAvailable/ {print $2}' /proc/meminfo)"
    JOBS=$(( avail_kb / 1572864 ))
    [ "$JOBS" -ge 1 ] || JOBS=1
    [ "$JOBS" -le "$(nproc)" ] || JOBS="$(nproc)"
fi

log() { printf '\n==== %s ====\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ -x "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++" ] || die "devkitA64 not found under $DEVKITPRO"
[ -f "$DEVKITPRO/libounce/lib/libounce.a" ] || die "libounce not found under $DEVKITPRO/libounce"
[ -f "$DEVKITPRO/portlibs/switch2/lib/libouncevk.a" ] || die "OunceVK not found (portlibs/switch2/lib/libouncevk.a)"
echo "devkitA64: $("$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++" --version | head -n1)"
echo "devkit:    $(tr '\n' ';' < "$DEVKITPRO/ounce-devkit-version.txt" 2>/dev/null)"
if pkg="$(dkp-pacman -Q ouncevk 2>/dev/null)"; then echo "OunceVK:   $pkg"; fi

[ -f generated/sources.cmake ] || die "generated/ is empty: run tools/switch2/host-stage.sh first"
[ -f out/switch2-host/shader_cache.cpp ] || die "out/switch2-host/shader_cache.cpp missing: run tools/switch2/host-stage.sh first"

if [ "${CLEAN:-0}" = 1 ]; then rm -rf "$build"; fi
log "Configure ($BUILD_TYPE)"
cmake -S . -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/switch2-libounce.cmake" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DDEVKITPRO="$DEVKITPRO" \
    -DREBLUE_SWITCH_LTO="$([ "$LTO" = 1 ] && echo ON || echo OFF)"
[ "${CONFIGURE_ONLY:-0}" = 1 ] && { log "Configured: $build"; exit 0; }

log "Build ($JOBS jobs)"
cmake --build "$build" --target reblue -j "$JOBS" -- -k 0

out="$root/out/switch2"
mkdir -p "$out"
cp "$build/reblue.nro" "$out/"
cp "$build/reblue.elf" "$out/" 2>/dev/null || cp "$build/reblue" "$out/reblue.elf"
if [ -n "${DEPLOY:-}" ]; then cp -f "$out/reblue.nro" "$DEPLOY"; echo "copied to $DEPLOY"; fi
log "Done"
echo "NRO: out/switch2/reblue.nro  (sdmc:/switch/reblue/reblue.nro; start hbmenu by holding R on a game)"
echo "ELF: out/switch2/reblue.elf  (crash addresses resolve against it)"
