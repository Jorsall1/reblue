#!/usr/bin/env bash
# Stage 1 of the Nintendo Switch 2 build, natively in WSL (Ubuntu 24.04), no
# Docker. Everything that runs on the build machine:
#   thirdparty/plume, volk, XenosRecomp    patched (plume-switch.patch + plume-switch2.patch)
#   thirdparty/rexglue-sdk/thirdparty/...  SDK third-party sources (fetched once)
#   assets/default.xex, assets/shaders/    already extracted; else from $GAME
#   out/switch2-host/sdk/                  host build of the SDK (rexglue)
#   generated/                             the recompiled game code (rexglue codegen)
#   out/switch2-host/xenos/                host build of XenosRecomp
#   out/switch2-host/shader_cache.cpp      the 152 game shaders as SPIR-V
# Then tools/switch2/build-switch2.sh cross-compiles reblue.nro.
#
#   bash tools/switch2/host-stage.sh
#
# Environment:
#   JOBS=N        parallel compile jobs (default: CPUs)
#   CLEAN=1       rebuild the host tools and redo codegen and the shader cache
#   GAME=dir      game folder for prepare_assets.py when assets/ is incomplete
#                 (default ~/switch2-dev/game)
#   LLVM_ROOT=dir a clang 19+ install (bin/clang, bin/clang++) to use instead
#                 of clang-19 on PATH
#   DIRECT_CALLS=0, PLAIN_MEMORY=1   as in tools/switch/host-stage.sh
#
# Clang 19: the SDK needs std::expected, which libstdc++ only enables for
# Clang 19+. Without clang-19 on PATH (sudo apt install clang-19) and without
# LLVM_ROOT, Ubuntu's own clang-19 packages are downloaded with apt-get
# download and unpacked into out/switch2-host/llvm-19 with dpkg -x: no root.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
JOBS="${JOBS:-$(nproc)}"
host="$root/out/switch2-host"
GAME="${GAME:-$HOME/switch2-dev/game}"
HOST_FLAGS="-march=x86-64-v2"

log() { printf '\n==== %s ====\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

if [ "${CLEAN:-0}" = 1 ]; then
    log "Clean host outputs"
    rm -rf "$host/sdk" "$host/xenos" "$host/codegen.key" "$host/shader_cache.key" "$host/shader_cache.cpp" \
        "$root/generated/codegen.build.stamp" "$root/generated/sources.cmake"
fi
mkdir -p "$host"

# ---------------------------------------------------------------- clang 19
log "Host compiler (clang 19)"
if [ -n "${LLVM_ROOT:-}" ]; then
    CC="$LLVM_ROOT/bin/clang"; CXX="$LLVM_ROOT/bin/clang++"
elif command -v clang-19 >/dev/null && command -v clang++-19 >/dev/null; then
    CC=clang-19; CXX=clang++-19
else
    llvm="$host/llvm-19"
    if [ ! -x "$llvm/bin/clang++-19" ]; then
        echo "clang-19 is not installed (sudo apt install clang-19 would do it); unpacking Ubuntu's packages into $llvm"
        command -v apt-get >/dev/null && command -v dpkg >/dev/null || die "apt-get/dpkg missing: install clang-19 or set LLVM_ROOT"
        debs="$host/llvm-19-debs"
        rm -rf "$debs" "$llvm"
        mkdir -p "$debs" "$llvm/bin"
        (cd "$debs" && apt-get download clang-19 libclang-cpp19 libclang1-19 libllvm19 libclang-common-19-dev) \
            || die "apt-get download of clang-19 failed (network, or run sudo apt-get update)"
        for deb in "$debs"/*.deb; do dpkg -x "$deb" "$llvm/root"; done
        real="$llvm/root/usr/lib/llvm-19/bin"
        [ -x "$real/clang" ] || die "the clang-19 package did not contain usr/lib/llvm-19/bin/clang"
        libs="$llvm/root/usr/lib/llvm-19/lib:$llvm/root/usr/lib/x86_64-linux-gnu"
        # Wrappers: the unpacked libLLVM/libclang-cpp are not on the loader path.
        # clang finds its resource headers relative to the real binary
        # (usr/lib/llvm-19/lib/clang/19) and the system GCC 13 libstdc++.
        for tool in clang clang++; do
            printf '#!/bin/sh\nLD_LIBRARY_PATH="%s${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "%s/%s" "$@"\n' \
                "$libs" "$real" "$tool" > "$llvm/bin/$tool-19"
            chmod +x "$llvm/bin/$tool-19"
        done
        rm -rf "$debs"
    fi
    CC="$llvm/bin/clang-19"; CXX="$llvm/bin/clang++-19"
fi
"$CXX" --version | head -n1
ver="$("$CXX" -dumpversion | cut -d. -f1)"
[ "$ver" -ge 19 ] || die "clang $ver: the SDK needs clang 19 or later"

# ---------------------------------------------------------------- X11/Wayland headers
# The SDK's UI library configures with pkg_check_modules(x11-xcb, wayland-client)
# on every Linux build, host tools included (Dockerfile.host installs
# libx11-xcb-dev libwayland-dev). Without them, and without root, Ubuntu's
# packages are unpacked into out/switch2-host/hostdeps and pkg-config pointed there.
if ! pkg-config --exists x11-xcb wayland-client 2>/dev/null; then
    deps="$host/hostdeps"
    if [ ! -f "$deps/.done" ]; then
        log "X11/Wayland development files (libx11-xcb-dev, libwayland-dev) into $deps"
        rm -rf "$deps"; mkdir -p "$deps/debs"
        (cd "$deps/debs" && apt-get download libx11-xcb-dev libx11-xcb1 libx11-dev libx11-6 x11proto-dev \
            libxcb1-dev libxcb1 libxau-dev libxau6 libxdmcp-dev libxdmcp6 libwayland-dev libwayland-client0 \
            libwayland-server0 libwayland-cursor0 libwayland-egl1 libffi-dev libffi8) \
            || die "apt-get download of the X11/Wayland dev packages failed (or: sudo apt install libx11-xcb-dev libwayland-dev)"
        for deb in "$deps"/debs/*.deb; do dpkg -x "$deb" "$deps/root"; done
        rm -rf "$deps/debs"
        # .pc files say prefix=/usr: point them at the unpacked tree.
        find "$deps/root" -name '*.pc' -exec sed -i "s|^prefix=/usr|prefix=$deps/root/usr|" {} +
        touch "$deps/.done"
    fi
    export PKG_CONFIG_PATH="$deps/root/usr/lib/x86_64-linux-gnu/pkgconfig:$deps/root/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LIBRARY_PATH="$deps/root/usr/lib/x86_64-linux-gnu${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export LD_LIBRARY_PATH="$deps/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    pkg-config --exists x11-xcb wayland-client || die "x11-xcb / wayland-client still not found through $deps"
    echo "x11-xcb $(pkg-config --modversion x11-xcb), wayland-client $(pkg-config --modversion wayland-client) from $deps"
fi

# ---------------------------------------------------------------- sources
log "Submodules"
git submodule update --init --recursive --depth 1

log "Patches"
# A chain of patches per submodule. A stamp in its git directory records the
# chain; when it matches the tree is ready, otherwise the submodule goes back
# to its pinned commit and the whole chain is applied again. Shared with the
# Switch 1 host stage: each stage re-applies its own chain.
apply_chain() { # <dir> <patch>...
    local dir="$1"; shift
    local stamp chain
    stamp="$(git -C "$dir" rev-parse --absolute-git-dir)/reblue-switch-patches"
    chain="$(git -C "$dir" rev-parse HEAD) $(cat "$@" | sha256sum | cut -d' ' -f1)"
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$chain" ]; then
        echo "$dir: already patched"
        return
    fi
    git -C "$dir" checkout -q -- .
    git -C "$dir" clean -fdq
    local patch
    for patch in "$@"; do
        git -C "$dir" apply "$patch" || die "$(basename "$patch") does not apply to $dir"
        echo "$dir: applied $(basename "$patch")"
    done
    echo "$chain" > "$stamp"
}
P="$root/tools/switch/patches"
# plume-switch2.patch: OunceVK's volk entry point and the ouncevk#2 clear split
# (vkCmdClearAttachments in calls of at most 80 rectangles).
apply_chain thirdparty/plume "$P/plume-switch.patch" "$P/plume-switch2.patch"
apply_chain thirdparty/plume/contrib/volk "$P/volk-switch.patch"
apply_chain thirdparty/XenosRecomp "$P/xenosrecomp-switch.patch"

log "SDK third-party sources"
python3 tools/switch/fetch_thirdparty.py

# ---------------------------------------------------------------- game inputs
log "Game inputs"
n_shaders=$(find assets/shaders -name '*.[vp]so' 2>/dev/null | wc -l)
if [ ! -f assets/default.xex ] || [ "$n_shaders" -lt 152 ]; then
    [ -d "$GAME" ] || die "assets/ is incomplete and the game folder $GAME does not exist (set GAME=...)"
    python3 tools/switch/prepare_assets.py "$GAME"
    n_shaders=$(find assets/shaders -name '*.[vp]so' 2>/dev/null | wc -l)
fi
[ -f assets/default.xex ] || die "assets/default.xex missing"
[ "$n_shaders" -ge 152 ] || die "assets/shaders holds $n_shaders shaders, expected 152"
echo "assets/default.xex and $n_shaders shaders"

# ---------------------------------------------------------------- rexglue
log "Host SDK (rexglue code generator)"
cmake -S thirdparty/rexglue-sdk -B "$host/sdk" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$HOST_FLAGS" -DCMAKE_CXX_FLAGS="$HOST_FLAGS" \
    -DREXGLUE_HOST_TOOLS_ONLY=ON -DREXGLUE_ENABLE_TRACY=OFF
cmake --build "$host/sdk" --target rexglue -j "$JOBS"
# The SDK may write its programs to its own out/<platform>/ folder; this
# build's folder is searched first.
rexglue="$(find "$host/sdk" thirdparty/rexglue-sdk/out -name rexglue -type f -perm -u+x 2>/dev/null | head -n1)"
[ -x "$rexglue" ] || die "rexglue was not built"
echo "rexglue: $rexglue"

codegen_key="$( { cat reblue_manifest.toml; find config -type f -print0 | sort -z | xargs -0 cat; \
                  sha256sum assets/default.xex "$rexglue"; } | sha256sum | cut -d' ' -f1)"
codegen_stamp="$host/codegen.key"
if [ -f generated/sources.cmake ] && [ -f "$codegen_stamp" ] && [ "$(cat "$codegen_stamp")" = "$codegen_key" ]; then
    log "Codegen: up to date"
else
    log "Codegen (recompiling default.xex)"
    "$rexglue" codegen reblue_manifest.toml
    echo "$codegen_key" > "$codegen_stamp"
fi
[ -f generated/sources.cmake ] || die "codegen wrote no generated/sources.cmake"

python3 tools/switch/plain_memory.py "$root" --undo
log "Direct calls (tools/switch/direct_calls.py)"
if [ "${DIRECT_CALLS:-1}" = 0 ]; then
    python3 tools/switch/direct_calls.py "$root" --undo
else
    python3 tools/switch/direct_calls.py "$root"
fi
log "Localized copies (tools/switch/localize.py)"
python3 tools/switch/localize.py "$root"
if [ "${PLAIN_MEMORY:-0}" = 1 ]; then
    log "Plain guest memory (tools/switch/plain_memory.py)"
    python3 tools/switch/plain_memory.py "$root"
fi

# ---------------------------------------------------------------- shaders
log "Host XenosRecomp"
cmake -S thirdparty/XenosRecomp -B "$host/xenos" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$HOST_FLAGS" -DCMAKE_CXX_FLAGS="$HOST_FLAGS -DREBLUE_RECOMP -DREBLUE_SPIRV_UBO"
cmake --build "$host/xenos" --target XenosRecomp -j "$JOBS"
xenos="$host/xenos/XenosRecomp/XenosRecomp"
[ -x "$xenos" ] || die "XenosRecomp was not built"

shader_key="$( { sha256sum "$xenos" thirdparty/XenosRecomp/XenosRecomp/shader_common.h; \
                 find assets/shaders -type f -print0 | sort -z | xargs -0 sha256sum; } | sha256sum | cut -d' ' -f1)"
shader_stamp="$host/shader_cache.key"
if [ -f "$host/shader_cache.cpp" ] && [ -f "$shader_stamp" ] && [ "$(cat "$shader_stamp")" = "$shader_key" ]; then
    log "Shader cache: up to date"
else
    log "Shader cache (XenosRecomp, 152 shaders)"
    LD_LIBRARY_PATH="$root/thirdparty/XenosRecomp/thirdparty/dxc-bin/lib/x64" \
        "$xenos" assets/shaders "$host/shader_cache.cpp" thirdparty/XenosRecomp/XenosRecomp/shader_common.h
    echo "$shader_key" > "$shader_stamp"
fi
# No deko3d shaders: Switch 2 renders through OunceVK only.

log "Switch 2 host stage done"
echo "next: bash tools/switch2/build-switch2.sh"
