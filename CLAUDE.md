# re:Blue Switch 2 port (kaiHEN + libounce + OunceVK): agent rules

Repo: `~/switch2-dev/reblue`, branch `main` (github.com/Jorsall1/reblue, the only branch; was `switch2-ouncevk`, from `reblueNX`, the Switch 1 port).
Reference port (done, on hardware): `~/switch2-dev/LostOdysseyRecomp`.
Game data: `~/switch2-dev/game` -> `/mnt/d/blue dragon/reblue_game` (3 discs merged).
`assets/default.xex` and the 152 shaders in `assets/shaders/` are already extracted.

## Working rules

- Never ask for permission or confirmation before editing files or running build commands. Do it.
  (Still confirm before pushing, force operations, or deleting work that is not yours.)
- At the end of every session write a concise milestone summary: `summary/summary_001.txt`,
  `summary/summary_002.txt`, ... (next free number). What changed (files), what was verified and
  how, what is open. Read the latest summary first when a session starts.
- Keep token use low: read the summaries and this file before exploring.
- Switch 1 (`reblueNX`) must keep building as before: Switch 2 code goes under `__SWITCH2__`
  (C/C++), `REX_PLATFORM_SWITCH2` (SDK), or `REXGLUE_PLATFORM_SWITCH2` (CMake).

## Toolchain (installed)

- devkitPro at `/opt/devkitpro`: devkitA64 GCC 16.1, `ounce-devkit 20261115b`, libounce 20261117
  (`/opt/devkitpro/libounce`, its `cmake/Switch2.cmake`, `switch2_cpu.mk` = `-mcpu=cortex-a78c+crypto`).
- OunceVK in `/opt/devkitpro/portlibs/switch2` (`lib/libouncevk.a`). Target: 20261123.
  Upgrade (needs root; `dkp-pacman` refuses otherwise):
  `gh release download 20261123 -R ouncebrew/ouncevk -p "ouncevk-20261123-1-any.pkg.tar.zst" -D /tmp`
  `sudo dkp-pacman -U --noconfirm --overwrite '/opt/devkitpro/portlibs/switch2/*' /tmp/ouncevk-20261123-1-any.pkg.tar.zst`
  (`--overwrite`: 20261120 was unpacked by hand, its files belong to no package.)
- Host side: native WSL Ubuntu 24.04, no Docker. clang 19 from `clang-19` on PATH, `LLVM_ROOT`, or
  unpacked without root into `out/switch2-host/llvm-19` by the host stage.

## Build commands

```sh
cd ~/switch2-dev/reblue
bash tools/switch2/host-stage.sh                        # patches, SDK deps, rexglue codegen -> generated/,
                                                        # XenosRecomp -> out/switch2-host/shader_cache.cpp
CONFIGURE_ONLY=1 bash tools/switch2/build-switch2.sh    # CMake configure only (out/switch2-build)
bash tools/switch2/build-switch2.sh                     # reblue.nro -> out/switch2/
DEPLOY=/mnt/c/Users/Jorda/Desktop/reblue.nro bash tools/switch2/build-switch2.sh
```

By hand: `cmake -S . -B out/switch2-build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/switch2-libounce.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_SCAN_FOR_MODULES=OFF`
then `cmake --build out/switch2-build --target reblue -j8` (about 1.5 GB RAM per job).

On the console: `sdmc:/switch/reblue/reblue.nro` + `game/`; start hbmenu by holding **R** while launching a game (kaiHEN title takeover).

## The three kaiHEN / OunceVK invariants (do not regress)

1. **Clean exit, R hook kept** (`thirdparty/rexglue-sdk/src/ui/switch2_exit.cpp`,
   from LO `os/switch/switch_runtime.cpp`, commits 5cde9314, c093a8db, e9726ef3):
   - Never `SetCurrentProcess` (or any identity request) on a session from `smGetService`
     (libounce overrides share kaiHEN HEN's FS root). The SD card stays on libnx's fsdev mount
     (`fsOpenSdCardFileSystem`); no raw `fsp-srv` session. No sm lookups of services that do not
     exist on Switch 2 (SaltyNX, sys-clk are stubbed: an unregistered name blocks).
   - No `ISelfController::Exit` (`__nx_applet_exit_mode` stays 0), no error applet, no
     `diagAbortWithResult`, no `svcExitProcess` (except HOME -> X after the unlock). Every exit,
     crash and abort goes through `RexSwitch2Exit()`.
   - `appletLockExit()` from start (`RexSwitch2ExitInit` in main); released right before leaving.
   - The exit runs on the main thread: next load `sdmc:/hbmenu.nro`, other threads end at their next
     `svcSleepThread` or are paused, resumed in `ResumeThenReturnToLoader` before the loader, `_Exit`.
   - HOME -> X: `RexSwitch2CloseForHomeMenu()` (unlock, let the system end the process), never the
     title teardown. A watchdog polls the applet if the UI loop stops for 3 s.
2. **Guest memory without `svcMap*`** (`thirdparty/rexglue-sdk/src/core/guest_memory_switch2.cpp`,
   LO `kernel/guest_address_space_switch.cpp`, commit 1eadf13b, `heap_backend_fix.txt`):
   Backend::Heap = one `memalign(0x1000, 4 GiB)` block, zero mapping syscalls (a masked SVC kills the
   process). No A/C/E aliases: `physical_membase_` = A view; `Memory::TranslatePhysical` uses the
   per-page home table (`SetPhysicalHome`, `xmemory.h/.cpp`) so C/E pages resolve to their own view.
   Protection is logical only (no write watches).
3. **OunceVK, no deko3d, `<= 80` rect clears** (`tools/switch/patches/plume-switch2.patch`,
   `cmake/switch2.cmake`, `cmake/PrefixOunceVK.cmake`; LO `gpu/renderer.cpp` commit 65d6b70a):
   plume Vulkan on `libouncevk.a` (vk* renamed `ouncevk_vk*`, volk from
   `ouncevk_vkGetInstanceProcAddr`), `VK_NN_vi_surface` on `nwindowGetDefault()`.
   ouncevk#2 (unfixed in 20261123): NVK `emit_clear_rects` overflows its 64 KiB command block above
   2730 rects -> PBDMA / `VK_ERROR_DEVICE_LOST`. Every `vkCmdClearAttachments` goes through
   `cmdClearAttachmentsChunked` (<= 80 rects, 480 dwords <= 512). `cmake/switch2.cmake` refuses an
   unpatched plume.
