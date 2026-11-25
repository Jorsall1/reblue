<div align="center">

<img src="res/reblue.png" alt="re:Blue logo" width="112">

# re:Blue for Nintendo Switch 2

**Blue Dragon (Xbox 360), recompiled to run natively on the Nintendo Switch 2.**

[![Release](https://img.shields.io/github/v/release/Jorsall1/reblue?include_prereleases&label=release)](https://github.com/Jorsall1/reblue/releases)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue)](LICENSE)
![Nintendo Switch 2](https://img.shields.io/badge/Nintendo%20Switch%202-homebrew-E60012)
![Vulkan](https://img.shields.io/badge/graphics-Vulkan%20(OunceVK)-A41E22)

### [Download](https://github.com/Jorsall1/reblue/releases) · [Report a problem](https://github.com/Jorsall1/reblue/issues)

Switch 2 port by **[Jorsall1](https://github.com/Jorsall1)** and **Claude** ([Anthropic](https://www.anthropic.com)), built on the [Switch port](https://github.com/Ad0ky/reblue) by **Ad0ky**, which is based on [zolaware's re:Blue](https://github.com/zolaware/reblue).

Free and open source: if you paid for this, ask for a refund.

</div>

## What this is

A homebrew build of re:Blue for the Nintendo Switch 2, running under [kaiHEN](https://github.com/ouncebrew/kaihen). The game's Xbox 360 code is statically recompiled to native ARM64. Its graphics run on Vulkan through [OunceVK](https://github.com/ouncebrew/ouncevk), the Switch 2's Mesa NVK driver, and the port is built with [libounce](https://github.com/ouncebrew/libounce) from the [ounce devkit](https://github.com/ouncebrew/ounce-devkit).

You need your own Blue Dragon discs. This project contains no game files.

The game plays through the field, towns, dungeons, battles, movies and menus. Report crashes, missing effects or anything that looks or sounds different from the Xbox 360 ([how](#reporting-a-problem)).

The original Switch (Switch 1) version, with its deko3d back end, lives in [Ad0ky/reblue](https://github.com/Ad0ky/reblue). Use that one on a Switch 1 with Atmosphère.

## Performance

The Switch 2 version targets **60 FPS** (the Switch 1 version runs at 30). The game still updates at its own 30 Hz; every frame in between is fully rendered with the motion smoothed between two updates (no frame generation). If the console can't hold 36 FPS for a while, smoothing turns off and the game holds 30 FPS until there's room again.

Dynamic resolution keeps the frame rate: it lowers the 3D scene's height when the GPU runs out of time and raises it again when there's room. FSR 1 scales the scene to the screen.

Measured in handheld mode at the system's stock clocks, with the default settings, over a one-hour session:

| Scene | Frame rate | Scene resolution |
| :--- | ---: | ---: |
| Field, towns and dungeons | **58–60 FPS** | 576 to about 940 rows |
| Busy scenes, mostly right after a movie | 42–56 FPS | 576 rows (the minimum) |
| Movies | 30 FPS (the movie's own rate) | — |

Starting or leaving a battle hitches for about a tenth of a second. Docked, dynamic resolution runs from 720p up to 1440p; docked performance hasn't been benchmarked yet.

## Getting started

You need:

- a **Nintendo Switch 2** running **[kaiHEN](https://github.com/ouncebrew/kaihen)** with the Homebrew Menu (`sdmc:/hbmenu.nro`);
- a **microSD card** with about **15 GB** free (the game files take about 13 GB);
- your **Blue Dragon discs** (all three, Global release: US/EU);
- a **Windows or Linux PC**, to prepare the game files once with the [PC version of re:Blue](https://github.com/zolaware/reblue/releases/latest);
- the **[release ZIP](https://github.com/Jorsall1/reblue/releases/latest)**: the game, its settings file and its file index.

### 1. Prepare the game files on a PC

The console can't read the discs itself. The PC version's installer does it once:

1. Download the [PC version of re:Blue](https://github.com/zolaware/reblue/releases/latest) and start it.
2. Let its installer read your three discs.
3. When it finishes, find its `game` folder. It holds `default.xex`, `bd_boot.ini`, `bd_config.ini`, the `pack`, `movie` and `map` folders and the sound files.

You don't need to play the PC version.

### 2. Copy the game to the SD card

1. Extract the release ZIP to the **root** of the SD card. It creates `switch/reblue/` with `reblue.nro`, `cache/`, `profiles/` and an empty `game/` folder.
2. Copy the **whole contents** of the PC's `game` folder into `sdmc:/switch/reblue/game/`.

### SD card layout

```
sdmc:/
├── hbmenu.nro                           the Homebrew Menu (your kaiHEN setup)
└── switch/
    └── reblue/
        ├── reblue.nro                   the game (release ZIP)
        ├── cache/
        │   ├── pack_index.bin           file index (release ZIP; rebuilt if it doesn't match)
        │   └── vulkan_pipelines.bin     GPU pipeline cache (created by the game)
        ├── profiles/default/
        │   ├── reblue.toml              settings (release ZIP)
        │   └── saves/                   your saves (created by the game)
        ├── game/                        your game files (PC installer)
        │   ├── default.xex
        │   ├── bd_boot.ini
        │   ├── bd_config.ini
        │   ├── pack/
        │   ├── movie/
        │   ├── map/
        │   └── ...
        └── logs/                        created by the game
            ├── reblue_001.log           one game log per start
            └── rex/                     rex_stderr.log, rex_crash.log, ...
```

### 3. Start the game

Start the Homebrew Menu **through title takeover**: hold **R** while starting any installed game, then choose **re:Blue**.

The first start takes longer: the game indexes its files, and the GPU driver builds its pipelines while you play, so expect some stutter in the first areas. Later starts load them from `cache/vulkan_pipelines.bin`.

To quit, press **HOME** and close the game with **X**. kaiHEN's R hook keeps working afterwards.

### Updating

Replace `reblue.nro` with the new one. Keep `game/`, `cache/` and `profiles/` (your saves are in `profiles/default/saves/`; back that folder up before updating).

## Settings

Most options are in the game's own **Settings** menu. **Display → Render Resolution** also has **FSR Upscaling** and **Show FPS**. Graphics start on the **Low** preset (shadow map 1024, retail shadow distance), which leaves dynamic resolution room to work.

The rest are in `sdmc:/switch/reblue/profiles/default/reblue.toml`. Each line is `name = value`; lines starting with `#` are comments, and anything not listed keeps its default. The release's `reblue.toml` has the recommended settings:

| Option | Release value | What it does |
| :--- | :---: | :--- |
| `bd_fps_limit` | 60 | Frame-rate target. 30 plays at the game's own rate, without smoothing. |
| `bd_interp_min_fps` | 36 | Below this rate smoothing turns off and the game holds 30 FPS. |
| `bd_switch_dynamic_resolution` | true | Dynamic resolution. |
| `bd_switch_fsr` | 2 | FSR 1 upscaling (0 = plain scaling). Also in the Settings menu. |
| `bd_switch2_handheld_min_height` / `_max_height` | 576 / 1080 | Handheld scene resolution range for dynamic resolution. |
| `bd_switch2_docked_min_height` / `_max_height` | 720 / 1440 | Docked scene resolution range. |
| `bd_switch2_docked_output_height` | 1440 | Docked output to the TV: 1440 = 2560×1440, 1080 = 1920×1080. |
| `bd_switch2_max_clocks` | false | false = the system's stock game clocks. true = above stock (an overclock). |
| `bd_shadow_update_interval` | 1 | The sun shadow map is redrawn every Nth frame. 2 or more is faster, but shadows of moving characters can flicker. |
| `bd_shadow_min_pixels` | 10 | Objects smaller than this on screen cast no shadow (0 = all). Lower it if mid-sized shadows go missing. |
| `bd_ply_blink_steady` | false | After a battle the player blinks for a couple of seconds. false keeps its shadow on between blinks, as on the console; true holds the player half transparent, without a shadow, until the blink ends. |
| `bd_reflection_update_interval` | 2 | Water reflections are redrawn every Nth frame. 1 redraws them every frame. |
| `bd_reflection_min_pixels` | 10 | Objects smaller than this are left out of the reflection. |
| `bd_scene_color_r11g11b10` | true | A lighter scene color format. Set false if glows or transparency look wrong. |
| `bd_opt_audio_hints` | 0 | The game's Audio Hints: spoken and on-screen notices such as "Playable" or "Checkpoint". |
| `bd_gpu_profile` | 1 | GPU timing; dynamic resolution reads it, keep 1. |

**Recommended:** keep dynamic resolution and FSR on.

## Reporting a problem

[Open an issue](https://github.com/Jorsall1/reblue/issues) with:

- the release version;
- handheld or docked;
- your `reblue.toml`;
- what you were doing, and where in the game;
- a photo or a short video if it is a graphics problem.

Also attach these files from the SD card, from the session where it happened:

- the newest game log in `sdmc:/switch/reblue/logs/` (`reblue_001.log` or similar);
- `sdmc:/switch/reblue/logs/rex/rex_stderr.log`;
- `sdmc:/switch/reblue/logs/rex/rex_crash.log` and `rex_cuelgue.log`, if they exist.

Don't attach the game's files (`game/`).

## For developers

Building runs on Linux (or WSL) and needs devkitPro with the [ounce devkit](https://github.com/ouncebrew/ounce-devkit) (libounce, OunceVK) installed under `/opt/devkitpro`, and clang 19 for the host stage (downloaded without root if it isn't installed). The build reads `default.xex` and the game's shaders from your own game files: pass your `game` folder once as `GAME=/path/to/game` and they are extracted to `assets/` (not in the repository). Then:

```sh
bash tools/switch2/host-stage.sh       # patches, SDK deps, recompiled game code (generated/), shader cache
bash tools/switch2/build-switch2.sh    # out/switch2/reblue.nro (and reblue.elf, for crash addresses)
```

Switch 2 code is under `__SWITCH2__` (C/C++), `REX_PLATFORM_SWITCH2` (SDK) and `REXGLUE_PLATFORM_SWITCH2` (CMake), so the Switch 1 build is unchanged. The plume changes are kept as patches in `tools/switch/patches/`.

Switch 2 specifics worth knowing:

- **Memory:** kaiHEN masks the memory-mapping syscalls, so guest memory is one heap block (`Backend::Heap`), not mirrored views (`thirdparty/rexglue-sdk/src/core/guest_memory_switch2.cpp`).
- **kaiHEN's R hook:** never send `SetCurrentProcess` (or any identity request) on a session from `smGetService` on Switch 2. Those sessions are libounce overrides shared with kaiHEN, and doing so breaks the R → Homebrew Menu hook. The SD card stays on `fsOpenSdCardFileSystem`. Every exit, crash and abort goes back to `sdmc:/hbmenu.nro` through `RexSwitch2Exit()`.
- **Clears:** NVK writes every rectangle of one `vkCmdClearAttachments` into a single 64 KiB command block. More than 2730 rectangles overrun it and stop the GPU, so every clear is issued in calls of at most 80 rectangles.

[docs/SWITCH.md](docs/SWITCH.md) describes the Switch port (shared by both consoles) and its optimizations. This repository also carries the rest of upstream's source, so the PC versions still build from it. For those, use [zolaware/reblue](https://github.com/zolaware/reblue). For the Switch 1 version, use [Ad0ky/reblue](https://github.com/Ad0ky/reblue).

## Credits

- **Switch 2 port:** [Jorsall1](https://github.com/Jorsall1) and Claude ([Anthropic](https://www.anthropic.com)).
- **Switch port**, which this port is built on: [Ad0ky](https://github.com/Ad0ky), in [Ad0ky/reblue](https://github.com/Ad0ky/reblue). The file index (`pack_index.bin`) in the release ZIP comes from its [v1.3.6 release](https://github.com/Ad0ky/reblue/releases/tag/v1.3.6).
- **re:Blue**, the original recompilation: [crack](https://github.com/tomcl7) (project lead) and [rcold](https://github.com/RC0ld), with the playtesters, translators and contributors listed in [upstream's README](https://github.com/zolaware/reblue#credits).
- **Switch 2 homebrew:** [ouncebrew](https://github.com/ouncebrew) for [kaiHEN](https://github.com/ouncebrew/kaihen), [libounce](https://github.com/ouncebrew/libounce), [OunceVK](https://github.com/ouncebrew/ouncevk) and the [ounce devkit](https://github.com/ouncebrew/ounce-devkit).
- **Research and tools:** [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), [XenosRecomp](https://github.com/hedge-dev/XenosRecomp), [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp), [plume](https://github.com/renderbag/plume), [Xenia](https://github.com/xenia-project/xenia), [devkitPro](https://devkitpro.org), [libnx](https://github.com/switchbrew/libnx) and [Mesa](https://mesa3d.org) (NVK).
- **Upscaling:** thanks to AMD for [FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (MIT).

Blue Dragon and its assets belong to their respective owners. This is an unofficial, non-commercial project under the [BSD 3-Clause License](LICENSE); dependencies keep their own licenses. Use data from discs you own, and don't commit game executables, archives, textures, audio, video, generated game code or captures to this repository.
