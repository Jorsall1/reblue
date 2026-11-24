/**
 * @file    platform/switch/switch_tuning.cpp
 * @brief   Nintendo Switch performance settings, see platform/switch_tuning.h.
 *
 *          GPU clock: in handheld mode a program starts with the GPU at
 *          307.2 MHz. Horizon has an official 460.8 MHz handheld profile,
 *          asked for through apm like any commercial game does (not an
 *          overclock). The SDK's switch_apm.cpp (from nfsmw-nx) picks the
 *          configuration that keeps the memory at 1331.2 MHz. Measured on the
 *          console at 307.2 MHz the field ran at 7.7 fps with the GPU the
 *          limit, so this is +50 % on the limiting part.
 *
 *          Handheld resolution: the game renders at 960x540 in handheld mode
 *          (as the Lost Odyssey port does) and the frame is scaled to the
 *          1280x720 screen: 44 % fewer pixels to shade.
 * @license BSD 3-Clause, see LICENSE
 */
#include "platform/switch_tuning.h"

#include <switch.h>

#include <rex/cvar.h>
#include <rex/ui/switch_apm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>

#include "gpu/shaders/deko3d_shaders.h"
#endif

#include "core/app_root.h"
#include "core/logging.h"
#if defined(__SWITCH2__)
#include "engine/settings.h"
#endif

// Four-core mode (SDK core/threading_switch.cpp).
extern "C" int RexSwitchSystemCore(void);
extern "C" bool RexSwitchSystemCoreCompiles(void);
extern "C" void RexSwitchSystemCoreDescribe(char* out, size_t out_len);
// The localized copies' keys and their "[hot]" line (hot_natives.cpp).
extern "C" void ReblueHotInit();

namespace plume {
void VulkanSetPipelineCachePath(const std::string &path);
#if defined(__SWITCH2__)
void VulkanSetSwapChainSize(uint32_t width, uint32_t height);
#endif
}

// constant_buffers.cpp: whether the vertex constants are bound with exactly 4096 bytes (bd_deko3d_bone_row_zero).
REXCVAR_DECLARE(bool, bd_deko3d_delta_constants);

// Defaults that differ on the Switch 2 (LostOdysseyRecomp's Switch 2 behaviour:
// settings/config.h, gpu/render_resolution.h, README "Render resolution"):
// - no handheld GPU profile request: the apm configurations in switch_apm.cpp
//   (0x92220008 ...) are the Switch 1's, and the Switch 2 picks its own clocks;
// - dynamic resolution on in both modes, FSR 1 whenever the scene is smaller
//   than the screen, a 60 fps frame budget (the 33.3 ms one was 30 fps).
#if defined(__SWITCH2__)
constexpr int kDefaultGpuMhz = 0;
constexpr bool kDefaultDynamicResolution = true;
constexpr int kDefaultFsr = 2;
// 15.8 of the 16.7 ms frame: logs 006/008 held a steady 60.0 fps (worst frame
// 17-19 ms) at 15.2 ms of GPU work, so 15.5 left resolution unused.
constexpr double kDefaultDynamicGpuMs = 15.8;
#else
constexpr int kDefaultGpuMhz = 460;
constexpr bool kDefaultDynamicResolution = false;
constexpr int kDefaultFsr = 1;
constexpr double kDefaultDynamicGpuMs = 33.3;
#endif

REXCVAR_DEFINE_INT32(bd_switch_gpu_mhz, kDefaultGpuMhz, "Switch",
                     "GPU clock profile to ask for in handheld mode: 0 = leave "
                     "the system default (307.2 MHz), 384 or 460. Docked is "
                     "never touched.")
    .range(0, 460);

REXCVAR_DEFINE_INT32(bd_switch_handheld_height, 540, "Switch",
                     "Render height in handheld mode (16:9): 540 = 960x540, "
                     "720 or 0 = the full 1280x720. Applied at start.")
    .range(0, 1080);

// Dynamic resolution (2026-11-12, the user's request; the picture is not the
// same while it acts). From rr6-nx / the renut-nx base's
// banjo_renut_dynamic_resolution in spirit: steps of 36 lines (16:9 widths stay
// whole), down when the GPU's time a frame stays over a budget, up when the next
// step is predicted to fit. Each change rebuilds the game's render targets at
// the next frame submit (the path a window resize takes on the PC), a short
// hitch, so changes are at least 2 s apart.
REXCVAR_DEFINE_BOOL(bd_switch_dynamic_resolution, kDefaultDynamicResolution, "Switch",
                    "Switch 1: handheld only, the render height moves between "
                    "bd_switch_dynamic_low_height and bd_switch_handheld_height. "
                    "Switch 2: both modes, between bd_switch2_<mode>_min_height "
                    "and bd_switch2_<mode>_max_height. In steps of 36 lines with "
                    "the GPU's load (the picture is softer while it is lower); "
                    "FSR 1 scales it up (bd_switch_fsr)");
#if defined(__SWITCH2__)
REXCVAR_DEFINE_INT32(bd_switch2_handheld_max_height, 1080, "Switch",
                     "Switch 2 handheld: render height (16:9), the highest with "
                     "dynamic resolution. 1080 = 1920x1080, the screen")
    .range(360, 1440);
REXCVAR_DEFINE_INT32(bd_switch2_handheld_min_height, 576, "Switch",
                     "Switch 2 handheld: the lowest dynamic resolution height "
                     "(576 = 1024x576)")
    .range(288, 1440);
REXCVAR_DEFINE_INT32(bd_switch2_docked_max_height, 1440, "Switch",
                     "Switch 2 docked: render height (16:9), the highest with "
                     "dynamic resolution. 1440 = 2560x1440")
    .range(360, 1440);
// The output (swapchain) size; the render size above is scaled to it by the
// present pass. LostOdysseyRecomp: 1920x1080 handheld, 2560x1440 docked
// (settings/config.cpp Validate, hbmenu_1440p_fix.txt).
REXCVAR_DEFINE_INT32(bd_switch2_docked_output_height, 1440, "Switch",
                     "Switch 2 docked: output resolution sent to the TV, 1440 = "
                     "2560x1440 or 1080 = 1920x1080 (handheld is always the "
                     "1920x1080 screen). Falls back to 1080 if the display "
                     "refuses 1440")
    .range(1080, 1440);

namespace {
// The current mode's output size, for plume's next swapchain resize.
void ApplySwitch2OutputSize(const char *why) {
  const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  const bool tall =
      docked && REXCVAR_GET(bd_switch2_docked_output_height) >= 1440;
  const uint32_t w = tall ? 2560 : 1920;
  const uint32_t h = tall ? 1440 : 1080;
  plume::VulkanSetSwapChainSize(w, h);
  BD_INFO("Switch 2: {} output {}x{} ({})", docked ? "docked" : "handheld", w,
          h, why);
}
} // namespace
REXCVAR_DEFINE_INT32(bd_switch2_docked_min_height, 720, "Switch",
                     "Switch 2 docked: the lowest dynamic resolution height "
                     "(720 = 1280x720)")
    .range(288, 1440);
// Off by default, as LostOdysseyRecomp: the system's own game clocks, the
// Switch 2's stock maximum for applications (measured there: handheld CPU
// 1101 / GPU 561 / memory 2133 MHz, docked CPU 998 / GPU 1007 MHz; its
// summary_036). true goes above them: an overclock.
REXCVAR_DEFINE_BOOL(bd_switch2_max_clocks, false, "Switch",
                    "Switch 2: false (default) = the system's stock game clocks "
                    "(handheld GPU 561 MHz, docked 1007 MHz). true = CPU, GPU "
                    "and memory at the highest rate their clock tables list, "
                    "set again after every dock, undock or sleep: an overclock "
                    "(more heat and battery use; may be refused under kaiHEN)");
REXCVAR_DEFINE_INT32(bd_switch2_cpu_cap_mhz, 0, "Switch",
                     "Switch 2 max clocks: CPU ceiling in MHz (0 = the table's "
                     "highest)")
    .range(0, 4000);
REXCVAR_DEFINE_INT32(bd_switch2_gpu_handheld_cap_mhz, 0, "Switch",
                     "Switch 2 max clocks: GPU ceiling in handheld mode in MHz "
                     "(0 = the table's highest)")
    .range(0, 4000);
REXCVAR_DEFINE_INT32(bd_switch2_gpu_docked_cap_mhz, 0, "Switch",
                     "Switch 2 max clocks: GPU ceiling docked in MHz (0 = the "
                     "table's highest)")
    .range(0, 4000);
REXCVAR_DEFINE_INT32(bd_switch2_emc_cap_mhz, 0, "Switch",
                     "Switch 2 max clocks: memory ceiling in MHz (0 = the "
                     "table's highest)")
    .range(0, 8000);
#endif
REXCVAR_DEFINE_INT32(bd_switch_dynamic_low_height, 432, "Switch",
                     "bd_switch_dynamic_resolution: the lowest render height "
                     "(432 = 768x432)")
    .range(288, 1080);
REXCVAR_DEFINE_DOUBLE(bd_switch_dynamic_gpu_ms, kDefaultDynamicGpuMs, "Switch",
                      "bd_switch_dynamic_resolution: the GPU time a frame (ms, "
                      "a moving average) above which the height steps down; it "
                      "steps up when the next height is predicted under 93% of it "
                      "(Switch 2; 95% on Switch 1). Switch 2 default 15.8");
REXCVAR_DEFINE_INT32(bd_switch_dynamic_test_s, 0, "Switch",
                     "bd_switch_dynamic_resolution test: alternate the highest "
                     "and lowest heights every N seconds whatever the load (0 off)")
    .range(0, 600);
REXCVAR_DEFINE_INT32(bd_switch_fsr, kDefaultFsr, "Switch",
                     "Upscale the rendered image to the screen with AMD FSR 1 "
                     "(EASU) instead of bilinear filtering: 0 never, 1 while "
                     "bd_switch_dynamic_resolution has lowered the height "
                     "(at bd_switch_handheld_height the present is as before), "
                     "2 whenever the image is smaller than the screen "
                     "(handheld 960x540 too). It costs ~5.7 ms a frame on the "
                     "console against ~0.9 for the bilinear present")
    .range(0, 2);

#if defined(__SWITCH2__)
REXCVAR_DEFINE_BOOL(bd_switch2_show_fps, false, "Switch",
                    "Switch 2: a small line in the top-left corner with the "
                    "frame rate and the live render resolution (\"60 FPS | "
                    "1152x648 FSR\"). Display page of the Settings menu");
#endif

#if defined(PLUME_DEKO3D)
REXCVAR_DEFINE_INT32(bd_deko3d_uam_flags, 431, "Switch",
                     "deko3d: uam's extra code generation passes (plume "
                     "Deko3DOptions::uamFlags). 431 = banjo-nx's 447 without 16, "
                     "the second compile per pixel shader that picks the "
                     "cheaper code: on the console that compile never ended "
                     "for one of the game's shaders. Each value has its own "
                     "shader cache file.")
    .range(0, 1023);

REXCVAR_DEFINE_BOOL(bd_deko3d_zcull, true, "Switch",
                    "deko3d: hierarchical depth culling (Zcull). The GPU drops "
                    "blocks of pixels hidden behind depth already drawn before "
                    "running their pixel shader. Set false if decals or other "
                    "surfaces go missing in 4x8-pixel blocks. Read at start.");

REXCVAR_DEFINE_BOOL(bd_deko3d_depth_compression, true, "Switch",
                    "deko3d: depth/stencil targets with hardware compression, "
                    "as colour targets already have: less memory traffic for "
                    "depth tests. Read at start.");
REXCVAR_DEFINE_BOOL(bd_deko3d_merge_barriers, false, "Switch",
                    "deko3d: barriers recorded one after another with no GPU "
                    "work between them are sent as one (the strongest wait and "
                    "every cache invalidation of them), right before the next "
                    "command that does work: the same order, one drain instead "
                    "of several. Read at start.");
REXCVAR_DEFINE_BOOL(bd_deko3d_fold_vs_masks, true, "Switch",
                    "deko3d: each pipeline's vertex shader gets its vertex "
                    "declaration's byte-swap masks as literals instead of "
                    "reading them per vertex (the same values, so the same "
                    "picture; fewer registers, e.g. bd_toon_vs_env 54 -> 40). "
                    "Each shader and declaration pair is compiled once on the "
                    "console, then kept in the shader cache. Read at start.");

REXCVAR_DEFINE_BOOL(bd_deko3d_depth_only_vs, false, "Switch",
                    "deko3d: a pipeline that only writes depth (the game draws "
                    "the castle street's crowd first depth only, then in "
                    "colour) gets its vertex shader with gl_Position only; "
                    "every vertex shader is compiled with an invariant "
                    "position so both passes give the same depth. If "
                    "characters show holes or flicker, set false. Off: measured "
                    "no gain in the castle street (2026-11-15). Read at start.");

REXCVAR_DEFINE_BOOL(bd_deko3d_bone_row_zero, true, "Switch",
                    "deko3d: skinning's bone row reads skip their range check "
                    "(the row past the last one is read as zero by the GPU, "
                    "the value the check gives), in the vertex shaders the "
                    "build marked (glsl_bonerow.py). Only with "
                    "bd_deko3d_delta_constants (constants bound with exactly "
                    "4096 bytes). Read at start.");

REXCVAR_DEFINE_BOOL(bd_deko3d_lean_barriers, false, "Switch",
                    "deko3d: a texture about to be rendered to (not read) waits "
                    "for the work before it but keeps the texture cache; the "
                    "transition that reads it later still drops the cache. "
                    "Read at start.");
REXCVAR_DEFINE_INT32(bd_deko3d_diag, 0, "Switch",
                     "deko3d GPU cost experiments, for measuring only: they "
                     "change or break the picture. Bits: 1 draws not sent, 2 "
                     "every scissor one pixel (no pixel work), 4 fragment "
                     "barriers do not wait, 8 every draw cut to one triangle, 16 "
                     "every vertex buffer at stride 0 (vertex shaders only), 32 "
                     "every bone read of the vertex shaders reads bone 0. It runs every other 10 s GPU report "
                     "window ([gpu-passes] says which), so the screen shows "
                     "between them. 0 in play. Read at start.");
#endif

namespace bd::platform {

#if defined(PLUME_DEKO3D)
namespace {

// The deko3d back end's compiled shaders (DKSH, by the hash of the GLSL variant
// uam compiled) in one file, <install>/cache/deko3d_shaders.bin: records of
// [u32 magic][u64 hash][u32 size][size bytes], appended as uam makes them, all
// read at start. A torn last record (power cut mid-write) ends the read and is
// overwritten by the next append. Changing uam's options changes the file name.
struct DkshCache {
  std::mutex mutex;
  std::unordered_map<uint64_t, std::vector<uint8_t>> entries;
  FILE *file = nullptr;
  size_t bytes = 0;
};
constexpr uint32_t kDkshRecordMagic = 0x48534B44; // "DKSH"

DkshCache &Dksh() {
  static DkshCache c;
  return c;
}

// Records [magic][hash][size][DKSH] from data into the cache; the bytes read up to the last whole record.
size_t ReadDkshRecords(const uint8_t *data, size_t size, size_t *added) {
  auto &c = Dksh();
  size_t at = 0;
  while (size - at >= 16) {
    uint32_t magic = 0, n = 0;
    uint64_t hash = 0;
    std::memcpy(&magic, data + at, 4);
    std::memcpy(&hash, data + at + 4, 8);
    std::memcpy(&n, data + at + 12, 4);
    if (magic != kDkshRecordMagic || n == 0 || n > size - at - 16)
      break;
    if (c.entries.emplace(hash, std::vector<uint8_t>(data + at + 16, data + at + 16 + n)).second) {
      c.bytes += n;
      ++*added;
    }
    at += 16 + n;
  }
  return at;
}

void OpenDkshCache(const std::string &path, unsigned uam_flags) {
  auto &c = Dksh();
  {
    // The build's variants first (make_dk_shaders.py --uam-variants): with them the console compiles only what
    // the build could not foresee.
    std::vector<uint8_t> built;
    size_t added = 0;
    if (bd::gpu::Deko3DBuiltinDksh(uam_flags, built))
      ReadDkshRecords(built.data(), built.size(), &added);
    BD_INFO("Switch: deko3d shaders compiled by the build: {} (uam flags {}, the build's {})", added, uam_flags,
            bd::gpu::g_deko3dDkshFlags);
  }
  const size_t from_build = c.entries.size();
  long good_end = 0;
  if (FILE *f = std::fopen(path.c_str(), "rb")) {
    for (;;) {
      uint32_t magic = 0, size = 0;
      uint64_t hash = 0;
      if (std::fread(&magic, 4, 1, f) != 1 || magic != kDkshRecordMagic ||
          std::fread(&hash, 8, 1, f) != 1 || std::fread(&size, 4, 1, f) != 1 ||
          size == 0 || size > (16u << 20))
        break;
      std::vector<uint8_t> data(size);
      if (std::fread(data.data(), 1, size, f) != size)
        break;
      if (c.entries.emplace(hash, std::move(data)).second)
        c.bytes += size;
      good_end = std::ftell(f);
    }
    std::fclose(f);
  }
  // r+ keeps the records, then the torn tail (if any) is overwritten.
  c.file = std::fopen(path.c_str(), good_end ? "r+b" : "wb");
  if (c.file)
    std::fseek(c.file, good_end, SEEK_SET);
  BD_INFO("Switch: deko3d shader cache {}: {} shaders ({} from the build), {} KB{}", path,
          c.entries.size(), from_build, c.bytes >> 10, c.file ? "" : " (read only)");
  plume::SetDeko3DShaderCache(
      [](uint64_t hash, std::vector<uint8_t> &dksh) {
        auto &c = Dksh();
        std::lock_guard lock(c.mutex);
        auto it = c.entries.find(hash);
        if (it == c.entries.end())
          return false;
        dksh = it->second;
        return true;
      },
      [](uint64_t hash, const std::vector<uint8_t> &dksh) {
        auto &c = Dksh();
        std::lock_guard lock(c.mutex);
        if (dksh.empty() || !c.entries.emplace(hash, dksh).second || !c.file)
          return;
        const uint32_t size = static_cast<uint32_t>(dksh.size());
        std::fwrite(&kDkshRecordMagic, 4, 1, c.file);
        std::fwrite(&hash, 8, 1, c.file);
        std::fwrite(&size, 4, 1, c.file);
        std::fwrite(dksh.data(), 1, size, c.file);
        std::fflush(c.file);
      });
}

} // namespace
#endif

#if defined(__SWITCH2__)
namespace {

// Switch 2 maximum clocks. The Switch 1 path (switch_apm.cpp) asks apm for
// one of the Switch 1's handheld performance configurations (GPU 460.8 MHz);
// those ids mean nothing on the Switch 2. Here each clock domain (CPU, GPU,
// memory) gets the highest rate its pcv table lists (clkrst
// GetPossibleClockRates), optionally capped per mode by the cvars below, the
// way sys-clk sets clocks. The system puts its own rates back on a dock or
// undock (and after sleep), so the thread checks once a second and sets them
// again. Its own thread: if clkrst blocks or is refused, the game is not held.
// It ends at the exit with every other thread (svcSleepThread wrap).
struct ClockDomain {
  PcvModuleId id;
  const char *name;
  ClkrstSession session;
  bool open;
  u32 rates[64];
  s32 count;
  PcvClockRatesListType type;
};

// The highest listed rate <= cap_hz (cap 0: the highest listed). A range list
// is [min, max]: the cap itself when it falls inside.
u32 PickRate(const ClockDomain &d, u32 cap_hz) {
  u32 best = 0;
  if (d.type == PcvClockRatesListType_Range && d.count >= 2) {
    const u32 hi = std::max(d.rates[0], d.rates[1]);
    const u32 lo = std::min(d.rates[0], d.rates[1]);
    return cap_hz ? std::clamp(cap_hz, lo, hi) : hi;
  }
  for (s32 i = 0; i < d.count; ++i)
    if ((cap_hz == 0 || d.rates[i] <= cap_hz) && d.rates[i] > best)
      best = d.rates[i];
  return best;
}

Thread g_clock_thread;

void MaxClocksThread(void *) {
  if (R_FAILED(clkrstInitialize())) {
    BD_WARN("Switch 2: clkrst not available: clocks left to the system");
    return;
  }
  ClockDomain domains[3] = {{PcvModuleId_CpuBus, "CPU", {}, false, {}, 0, {}},
                            {PcvModuleId_GPU, "GPU", {}, false, {}, 0, {}},
                            {PcvModuleId_EMC, "memory", {}, false, {}, 0, {}}};
  for (ClockDomain &d : domains) {
    d.open = R_SUCCEEDED(clkrstOpenSession(&d.session, d.id, 3));
    if (d.open &&
        R_FAILED(clkrstGetPossibleClockRates(&d.session, d.rates, 64, &d.type,
                                             &d.count)))
      d.count = 0;
    BD_INFO("Switch 2: {} clock table: {} rates, highest {:.1f} MHz{}", d.name,
            d.count, PickRate(d, 0) / 1e6, d.open ? "" : " (no clkrst session)");
  }
  int last_mode = -1;
  bool refused[3] = {false, false, false};
  for (;;) {
    if (REXCVAR_GET(bd_switch2_max_clocks)) {
      const int mode = int(appletGetOperationMode());
      const bool docked = mode == AppletOperationMode_Console;
      const u32 caps[3] = {
          u32(REXCVAR_GET(bd_switch2_cpu_cap_mhz)) * 1000000u,
          u32(docked ? REXCVAR_GET(bd_switch2_gpu_docked_cap_mhz)
                     : REXCVAR_GET(bd_switch2_gpu_handheld_cap_mhz)) * 1000000u,
          u32(REXCVAR_GET(bd_switch2_emc_cap_mhz)) * 1000000u};
      const bool report = mode != last_mode;
      last_mode = mode;
      std::string line;
      for (int i = 0; i < 3; ++i) {
        ClockDomain &d = domains[i];
        if (!d.open || d.count <= 0 || refused[i])
          continue;
        const u32 target = PickRate(d, caps[i]);
        u32 now = 0;
        if (!target || R_FAILED(clkrstGetClockRate(&d.session, &now)))
          continue;
        Result rc = 0;
        if (now + 1000000u < target) {
          rc = clkrstSetClockRate(&d.session, target);
          // Refused (no permission under this donor title): stop asking.
          if (R_FAILED(rc))
            refused[i] = true;
        }
        if (report || R_FAILED(rc))
          line += fmt::format("{}{} {:.1f} -> {:.1f} MHz{}", line.empty() ? "" : ", ",
                              d.name, now / 1e6, target / 1e6,
                              R_FAILED(rc) ? fmt::format(" (refused {:#x})", rc) : "");
      }
      if (!line.empty())
        BD_INFO("Switch 2: {} clocks: {}", docked ? "docked" : "handheld", line);
    }
    svcSleepThread(1'000'000'000ll);
  }
}

void StartMaxClocks() {
  if (!REXCVAR_GET(bd_switch2_max_clocks)) {
    BD_INFO("Switch 2: bd_switch2_max_clocks off: clocks left to the system");
    return;
  }
  if (R_SUCCEEDED(threadCreate(&g_clock_thread, MaxClocksThread, nullptr,
                               nullptr, 0x8000, 0x3B, -2)))
    threadStart(&g_clock_thread);
}

} // namespace
#endif

void ApplySwitchClocks() {
  ReblueHotInit();
#if defined(__SWITCH2__)
  StartMaxClocks();
#endif
  {
    // Four-core mode (re:Blue 2026-11-12, from renut-nx; SDK core/threading_switch.h).
    char cores[320];
    RexSwitchSystemCoreDescribe(cores, sizeof(cores));
    BD_INFO("Switch: {}", cores);
  }
#if defined(__SWITCH2__)
  // Never on Switch 2: switch_apm.cpp's candidates are Switch 1
  // PerformanceConfiguration ids; on this console they name other clock sets.
  // Switch 2 clocks only go through StartMaxClocks (pcv's own rate table).
  const int mhz = 0;
#else
  const int mhz = REXCVAR_GET(bd_switch_gpu_mhz);
#endif
  RexSwitchApmPedirGpuMhz(mhz);
  RexSwitchApmPermitirRam1600(0);
  RexSwitchApmAplicar();
  BD_INFO("Switch: handheld GPU profile requested: {}",
          mhz ? std::to_string(mhz) + " MHz" : std::string("system default"));

  // The Vulkan pipeline cache in <install>/cache/, read when the device is
  // created. re:Blue already pre-builds its known pipelines at start on a
  // background pool; with the cache those come from the SD card after the
  // first run instead of being compiled again.
  const std::filesystem::path dir = bd::CacheRootFor(bd::AppRootFolder());
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string file = (dir / "vulkan_pipelines.bin").string();
  plume::VulkanSetPipelineCachePath(file);
  BD_INFO("Switch: Vulkan pipeline cache at {}", file);
#if defined(__SWITCH2__)
  ApplySwitch2OutputSize("start");
#endif
#if defined(PLUME_DEKO3D)
  {
    plume::Deko3DOptions options = plume::Deko3DGetOptions();
    options.uamFlags = static_cast<unsigned>(REXCVAR_GET(bd_deko3d_uam_flags));
    options.zcull = REXCVAR_GET(bd_deko3d_zcull);
    options.depthCompression = REXCVAR_GET(bd_deko3d_depth_compression);
    options.mergeBarriers = REXCVAR_GET(bd_deko3d_merge_barriers);
    options.leanBarriers = REXCVAR_GET(bd_deko3d_lean_barriers);
    options.foldVsMasks = REXCVAR_GET(bd_deko3d_fold_vs_masks);
    options.depthOnlyVs = REXCVAR_GET(bd_deko3d_depth_only_vs);
    options.boneRowZero =
        REXCVAR_GET(bd_deko3d_bone_row_zero) && REXCVAR_GET(bd_deko3d_delta_constants);
    options.diag = static_cast<unsigned>(REXCVAR_GET(bd_deko3d_diag));
    // Four-core mode (from renut-nx): the uam compile thread on the system core when there is one, at 0x2C: below
    // the audio output (0x2A) and the Audio Worker (0x2B), above the XAudio engine threads moved there (0x2D). At
    // the 0x3B it had otherwise, an engine thread busy with the reverb after a load (switch_system_core_mixer = 1)
    // kept it from running and the game hung at a warp waiting for its shaders (2026-11-12).
    if (RexSwitchSystemCoreCompiles()) {
      options.compileCore = RexSwitchSystemCore();
      options.compilePriority = 0x2C;
    }
    plume::Deko3DSetOptions(options);
    BD_INFO("Switch: deko3d barriers: merged {}, lean {}; vertex declaration masks folded into the vertex shaders: {}; position-only vertex shaders for depth-only pipelines: {}; bone rows without range checks: {}{}",
            options.mergeBarriers ? "on" : "off", options.leanBarriers ? "on" : "off",
            options.foldVsMasks ? "on" : "off", options.depthOnlyVs ? "on" : "off",
            options.boneRowZero ? "on" : "off",
            options.diag ? " | GPU COST EXPERIMENT " + std::to_string(options.diag) +
                               " (bd_deko3d_diag) every other 10 s window: the picture is wrong in those"
                         : std::string());
    // A shader whose compile does not end is written here (uam_stuck_*.glsl).
    static const std::string logs = (bd::AppRootFolder() / "logs").string();
    plume::Deko3DSetDumpFolder(logs.c_str());
  }
  const unsigned uam_flags = plume::Deko3DGetOptions().uamFlags;
  OpenDkshCache(
      (dir / ("deko3d_shaders_" + std::to_string(uam_flags) + ".bin")).string(),
      uam_flags);
#endif
}

namespace {
// The height dynamic resolution has chosen (0: none yet, the configured one).
std::atomic<int> g_dynamic_height{0};
// steady_clock time (ns) of the last movie frame copied by the game, 0: none.
std::atomic<long long> g_last_movie_frame_ns{0};
// The same for the reblue Settings menu (SwitchNoteMenuFrame).
std::atomic<long long> g_last_menu_frame_ns{0};
long long SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if defined(__SWITCH2__)
// Switch 2 heights are whole multiples of 72 lines: the render rect is then
// exact in both directions (72 lines = 128 columns, both multiples of 8).
// With 36 every other step was rounded (612 -> a 1088x608 rect) and the game,
// which derives a width from the height in places, asked 1080x608 targets
// under 1088x608 surfaces.
constexpr int kSwitch2HeightStep = 72;
// The current mode's render height range, both whole multiples of the
// dynamic resolution step (16:9 widths stay whole: 576 -> 1024, 1440 -> 2560).
struct HeightRange {
  int high;
  int low;
};
// The limits in use: a copy of the four cvars taken at start-up and again when
// a change from the Settings menu is applied (outside the menu, see
// SwitchDynamicResolutionFrame). A cvar written while the menu is open does not
// reach the render size before then, even through another setting's
// Output::Recompute (aspect ratio).
std::atomic<int> g_applied_limits[4] = {-1, -1, -1, -1};
std::atomic<bool> g_limits_changed{false};
int CvarLimit(Switch2HeightLimit which) {
  switch (which) {
  case Switch2HeightLimit::HandheldMax:
    return REXCVAR_GET(bd_switch2_handheld_max_height);
  case Switch2HeightLimit::HandheldMin:
    return REXCVAR_GET(bd_switch2_handheld_min_height);
  case Switch2HeightLimit::DockedMax:
    return REXCVAR_GET(bd_switch2_docked_max_height);
  case Switch2HeightLimit::DockedMin:
    return REXCVAR_GET(bd_switch2_docked_min_height);
  }
  return 0;
}
void SnapshotLimits() {
  for (int i = 0; i < 4; ++i)
    g_applied_limits[i].store(CvarLimit(Switch2HeightLimit(i)),
                              std::memory_order_relaxed);
}
int AppliedLimit(Switch2HeightLimit which) {
  int v = g_applied_limits[int(which)].load(std::memory_order_relaxed);
  if (v < 0) {
    SnapshotLimits();
    v = g_applied_limits[int(which)].load(std::memory_order_relaxed);
  }
  return v;
}
HeightRange Switch2Range() {
  const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  int high = AppliedLimit(docked ? Switch2HeightLimit::DockedMax
                                 : Switch2HeightLimit::HandheldMax);
  high = std::clamp(high, 360, 1440);
  high -= high % kSwitch2HeightStep;
  int low = AppliedLimit(docked ? Switch2HeightLimit::DockedMin
                                : Switch2HeightLimit::HandheldMin);
  low = std::clamp(low, 288, high);
  low = std::max(low - low % kSwitch2HeightStep, kSwitch2HeightStep * 4);
  return {high, low};
}
#endif
} // namespace

unsigned SwitchProcessCoreCount() {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
    return 0;
  return static_cast<unsigned>(__builtin_popcountll(mask));
}

bool SwitchHandheldRenderSize(u32 &w, u32 &h) {
#if defined(__SWITCH2__)
  // Both modes: the mode's maximum, or the height dynamic resolution chose,
  // kept inside the current mode's range (a dock or undock changes it).
  const HeightRange range = Switch2Range();
  const int dynamic = g_dynamic_height.load(std::memory_order_relaxed);
  const int height = (dynamic > 0 && REXCVAR_GET(bd_switch_dynamic_resolution))
                         ? std::clamp(dynamic, range.low, range.high)
                         : range.high;
  h = static_cast<u32>(height) & ~1u;
  w = ((h * 16 + 8) / 9) & ~1u; // 1080 -> 1920, 1440 -> 2560, 576 -> 1024
  return true;
#else
  const int configured = REXCVAR_GET(bd_switch_handheld_height);
  const int dynamic = g_dynamic_height.load(std::memory_order_relaxed);
  const int height = (dynamic > 0 && REXCVAR_GET(bd_switch_dynamic_resolution))
                         ? dynamic
                         : configured;
  if (height <= 0 || height >= 720)
    return false;
  if (appletGetOperationMode() != AppletOperationMode_Handheld)
    return false;
  h = static_cast<u32>(height) & ~1u;
  w = ((h * 16 + 8) / 9) & ~1u; // 540 -> 960
  return true;
#endif
}

void Switch2OutputModeTick() {
#if defined(__SWITCH2__)
  // Every present, before the swapchain resize check: a dock or undock gives
  // plume the new mode's output size, and needsResize() then rebuilds it.
  static int s_mode = -1;
  const int mode = int(appletGetOperationMode());
  if (mode == s_mode)
    return;
  const bool first = s_mode < 0;
  s_mode = mode;
  if (!first)
    ApplySwitch2OutputSize("dock change");
#endif
}

void SwitchNoteMovieFrame() {
  g_last_movie_frame_ns.store(SteadyNowNs(), std::memory_order_relaxed);
}

void SwitchNoteMenuFrame() {
  g_last_menu_frame_ns.store(SteadyNowNs(), std::memory_order_relaxed);
}

#if defined(__SWITCH2__)
int Switch2ResolutionHeight(Switch2HeightLimit which) {
  return CvarLimit(which);
}

bool SetSwitch2ResolutionHeight(Switch2HeightLimit which, int height) {
  height = std::clamp(height, 288, 1440);
  height -= height % kSwitch2HeightStep;
  const auto set = [](Switch2HeightLimit w, int v) {
    const char *name = w == Switch2HeightLimit::HandheldMax ? "bd_switch2_handheld_max_height"
                       : w == Switch2HeightLimit::HandheldMin ? "bd_switch2_handheld_min_height"
                       : w == Switch2HeightLimit::DockedMax   ? "bd_switch2_docked_max_height"
                                                              : "bd_switch2_docked_min_height";
    return rex::cvar::SetFlagByName(name, std::to_string(v));
  };
  const bool is_max = which == Switch2HeightLimit::HandheldMax ||
                      which == Switch2HeightLimit::DockedMax;
  const bool handheld = which == Switch2HeightLimit::HandheldMax ||
                        which == Switch2HeightLimit::HandheldMin;
  const Switch2HeightLimit other =
      handheld ? (is_max ? Switch2HeightLimit::HandheldMin : Switch2HeightLimit::HandheldMax)
               : (is_max ? Switch2HeightLimit::DockedMin : Switch2HeightLimit::DockedMax);
  if (is_max)
    height = std::max(height, 360); // Switch2Range's floor for a maximum
  if (!set(which, height))
    return false;
  // Keep min <= max: the other end follows.
  const int o = CvarLimit(other);
  if (is_max && o > height)
    set(other, height);
  else if (!is_max && o < height)
    set(other, height);
  g_limits_changed.store(true, std::memory_order_release);
  BD_INFO("[dynres] Settings: {} {} height {} (applied when the menu closes)",
          handheld ? "handheld" : "docked", is_max ? "max" : "min", height);
  return true;
}

bool Switch2ShowFps() { return REXCVAR_GET(bd_switch2_show_fps); }

bool SetSwitch2ShowFps(bool on) {
  return rex::cvar::SetFlagByName("bd_switch2_show_fps", on ? "true" : "false");
}

bool SwitchFsrEnabled() { return REXCVAR_GET(bd_switch_fsr) >= 1; }

bool SetSwitchFsrEnabled(bool on) {
  // On = 2 (the Switch 2 default: whenever the image is smaller than the
  // screen), off = 0 (bilinear). Read by every present, so it is live.
  const bool ok = rex::cvar::SetFlagByName("bd_switch_fsr", on ? "2" : "0");
  BD_INFO("[fsr] Settings: FSR 1 upscaling {}", on ? "on (bd_switch_fsr 2)" : "off (bd_switch_fsr 0)");
  return ok;
}
#endif

bool SwitchDynamicResolutionFrame(double gpu_ms, double fixed_ms) {
#if defined(__SWITCH2__)
  // Dock or undock: the render size starts again at the new mode's maximum
  // (the output size follows in Switch2OutputModeTick, from the present).
  {
    static int s_mode = -1;
    const int mode = int(appletGetOperationMode());
    if (mode != s_mode) {
      const bool first = s_mode < 0;
      s_mode = mode;
      if (!first) {
        g_dynamic_height.store(0, std::memory_order_relaxed);
        BD_INFO("[dynres] {} mode: render height range {}-{}",
                mode == AppletOperationMode_Console ? "docked" : "handheld",
                Switch2Range().low, Switch2Range().high);
        return true;
      }
    }
  }
  // Limits changed in the Settings menu: applied 1 s after its last frame (the
  // first field frames after it closes), never under it. The render size
  // restarts at the new maximum; dynamic resolution steps down from there.
  {
    const long long menu_ns = g_last_menu_frame_ns.load(std::memory_order_relaxed);
    const bool menu_recent =
        menu_ns != 0 && SteadyNowNs() - menu_ns < 1'000'000'000ll;
    if (!menu_recent && g_limits_changed.exchange(false, std::memory_order_acq_rel)) {
      SnapshotLimits();
      g_dynamic_height.store(0, std::memory_order_relaxed);
      const HeightRange r = Switch2Range();
      BD_INFO("[dynres] Settings applied: render height range {}-{} ({})", r.low,
              r.high,
              appletGetOperationMode() == AppletOperationMode_Console ? "docked"
                                                                      : "handheld");
      return true;
    }
  }
#endif
  if (!REXCVAR_GET(bd_switch_dynamic_resolution) || !(gpu_ms > 0.0))
    return false;
#if defined(__SWITCH2__)
  // Both modes. Steps of 36 lines, not straight to either end: on Switch 1 FSR
  // cost more than a step saved (~4.8 ms against ~2.5); on the Switch 2's GPU
  // a jump from 1440 to 720 would halve the picture for a small overload.
  constexpr bool kSnapToEnds = false;
  // FSR 1's cost over the plain present, measured on Switch 2 (logs 006, 008):
  // the present pass is 1.7 ms below the output height, 0.9-1.0 at it.
  constexpr double kFsrExtraMs = 0.8;
  const HeightRange range = Switch2Range();
  const int high = range.high;
  const int low = range.low;
  constexpr int kStep = kSwitch2HeightStep;
  // A step down may skip steps (up to 3) to the height predicted to fit:
  // one visible change for a heavier scene instead of one every few seconds.
  constexpr int kMaxDownSteps = 3;
#else
  constexpr int kStep = 36;
  constexpr int kMaxDownSteps = 1;
  fixed_ms = 0.0; // Switch 1: the whole GPU time scales (as before)
  constexpr bool kSnapToEnds = true;
  constexpr double kFsrExtraMs = 4.8;
  if (appletGetOperationMode() != AppletOperationMode_Handheld)
    return false;
  int high = REXCVAR_GET(bd_switch_handheld_height);
  if (high <= 0 || high >= 720)
    high = 720;
  high -= high % 36;
  int low = std::clamp(REXCVAR_GET(bd_switch_dynamic_low_height), 288, high);
  low -= low % 36;
#endif
#if defined(__SWITCH2__)
  // bd_switch_dynamic_gpu_ms is the 60 fps budget; it follows the frame-rate
  // cap (bd_fps_limit 30-120): 31.6 ms at 30, 7.9 at 120. The old fixed 15.8
  // let a 90 / 120 fps cap run the GPU at the 60 fps size.
  const int target_fps =
      std::clamp(int(bd::engine::Settings::Get().FPSLimit()), 30, 120);
  const double budget = REXCVAR_GET(bd_switch_dynamic_gpu_ms) * 60.0 / double(target_fps);
  {
    static int s_logged_fps = 0;
    if (s_logged_fps != target_fps) {
      s_logged_fps = target_fps;
      BD_INFO("[dynres] target {} fps: GPU budget {:.1f} ms a frame", target_fps, budget);
    }
  }
#else
  const double budget = REXCVAR_GET(bd_switch_dynamic_gpu_ms);
#endif
  const int test_s = REXCVAR_GET(bd_switch_dynamic_test_s);

  using Clock = std::chrono::steady_clock;
  static Clock::time_point last_change{}, over_since{}, fit_since{};
  static double ema = 0.0;
  static bool settled = false;
  const Clock::time_point now = Clock::now();
  const auto seconds = [&](Clock::time_point t) {
    return std::chrono::duration<double>(now - t).count();
  };
  int cur = g_dynamic_height.load(std::memory_order_relaxed);
  if (cur <= 0)
    cur = high;
  cur = std::clamp(cur, low, high); // Switch 2: the range follows the mode
  cur -= (cur - low) % kStep;        // a height from an older step size
  static double ema_fixed = 0.0;
#if defined(__SWITCH2__)
  // A fast average (~3 frames) next to the 10-frame one: a scene that jumps
  // over the budget (log 013 14:07:39 and 14:08:18: 1080 at 19.9 ms) is
  // stepped down within ~0.25 s instead of after 1.5 s of missed frames.
  static double fast = 0.0;
  static Clock::time_point fast_over_since{};
#endif
  fixed_ms = std::clamp(fixed_ms, 0.0, gpu_ms);
  if (!settled) {
    settled = true;
    last_change = now;
    ema = gpu_ms;
    ema_fixed = fixed_ms;
#if defined(__SWITCH2__)
    fast = gpu_ms;
#endif
  }
#if defined(__SWITCH2__)
  // No change while a movie plays and for 3 s after its last frame. A movie
  // costs a fraction of the field's GPU time, so the height only went up
  // during one, and both silent deaths (sessions 010 and 011) came within
  // seconds of a movie that had a step up in it: the surfaces rebuilt under
  // the movie's draws, then the field resumed on them. The field's average
  // is kept (not fed movie frames), and the 4 s gap starts after the hold.
  // The same for the reblue Settings menu (log 006: four rebuilds under it,
  // the last a step up, then the silent death while Depth of Field was being
  // changed). Its GPU time is not the field's either.
  {
    static int held = 0; // 0 none, 1 movie, 2 Settings menu
    const auto recent = [&](const std::atomic<long long> &stamp) {
      const long long ns = stamp.load(std::memory_order_relaxed);
      return ns != 0 && now.time_since_epoch() - std::chrono::nanoseconds(ns) <
                            std::chrono::seconds(3);
    };
    const int hold = recent(g_last_movie_frame_ns)  ? 1
                     : recent(g_last_menu_frame_ns) ? 2
                                                    : 0;
    if (hold) {
      if (held != hold)
        BD_INFO("[dynres] held at height {} while {}", cur,
                hold == 1 ? "a movie plays" : "the Settings menu is open");
      held = hold;
      last_change = now;
      over_since = {};
      fit_since = {};
      fast_over_since = {};
      return false;
    }
    if (held)
      BD_INFO("[dynres] {} over: height {}, may change again in 4 s",
              held == 1 ? "movie" : "Settings menu", cur);
    held = 0;
  }
#endif
  ema = ema * 0.9 + gpu_ms * 0.1;
  ema_fixed = ema_fixed * 0.9 + fixed_ms * 0.1;
#if defined(__SWITCH2__)
  fast = fast * 0.65 + gpu_ms * 0.35;
#endif
  // The GPU time split in two: the start of the frame, the shadow map and the
  // present do not follow the render size (~5 ms of 14 at 1152x648 on the
  // Switch 2: scaling the whole time by the pixel ratio overstated a step up
  // and understated a step down); the rest scales with the pixel count.
  const double fixed = std::min(ema_fixed, ema);
  const double scaled = ema - fixed;

#if defined(__SWITCH2__)
  // Each change rebuilds the game's surfaces (a hitch and a visible size
  // change), so the Switch 2 waits longer: 4 s between changes, 1.5 s over
  // the budget to step down, 4 s predicted under 90% of it to step up, and no
  // step up within 10 s of a step down (no 1080 <-> 1044 flip-flop).
  // Step up below 93% of the budget (14.7 of 15.8 ms): still more than 1 ms of
  // hysteresis against the step down, the 10 s window's spread is ~0.3 ms.
  // A step down no longer waits the 4 s gap nor 1.5 s over the budget (each
  // of those frames already missed the vblank): 1 s after any change, 0.5 s
  // over (the 10-frame average), or 0.25 s with the fast average 10% over.
  // Steps up keep the 4 s gap and the 4 s fit.
  constexpr double kMinGapS = 4.0, kOverS = 0.5, kFitS = 4.0, kFitShare = 0.93;
  constexpr double kDownGapS = 1.0, kFastOverS = 0.25, kFastOverShare = 1.10;
  constexpr double kUpAfterDownS = 10.0;
#else
  constexpr double kMinGapS = 2.0, kOverS = 1.0, kFitS = 2.0, kFitShare = 0.95;
  constexpr double kUpAfterDownS = 0.0;
#endif
  static Clock::time_point last_down{};
#if defined(__SWITCH2__)
  // Back-off (session 013, log 009: 576 -> 648 -> 576 -> 648 -> 720 -> 576 in
  // 60 s, each change a 50-100 ms surface rebuild): a step up undone by a step
  // down within 20 s doubles the wait before the next step up (10 s -> 20 ->
  // 40, at most 80 s); a height held for 60 s resets it.
  static Clock::time_point last_up{};
  static double up_after_down_s = kUpAfterDownS;
  if (up_after_down_s > kUpAfterDownS && seconds(last_change) >= 60.0)
    up_after_down_s = kUpAfterDownS;
#else
  const double up_after_down_s = kUpAfterDownS;
#endif
  int next = cur;
#if defined(__SWITCH2__)
  if (test_s == 0 && cur > low && seconds(last_change) >= kDownGapS) {
    const bool slow_over = ema > budget;
    const bool fast_over = fast > budget * kFastOverShare;
    if (slow_over) {
      if (over_since == Clock::time_point{})
        over_since = now;
    } else {
      over_since = {};
    }
    if (fast_over) {
      if (fast_over_since == Clock::time_point{})
        fast_over_since = now;
    } else {
      fast_over_since = {};
    }
    const bool go = (slow_over && seconds(over_since) >= kOverS) ||
                    (fast_over && seconds(fast_over_since) >= kFastOverS);
    if (go) {
      // Predict from the larger of the two averages, so a sudden load gets
      // the steps it needs in one change.
      const double load = std::max(ema, fast);
      const double fx = std::min(ema_fixed, load);
      const double sc = load - fx;
      // Leaving the output height turns FSR on (bd_switch_fsr >= 1): its
      // cost comes on top of the smaller size.
      const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
      const int out_h =
          (docked && REXCVAR_GET(bd_switch2_docked_output_height) >= 1440) ? 1440 : 1080;
      const double fsr_add =
          (REXCVAR_GET(bd_switch_fsr) >= 1 && cur >= out_h) ? kFsrExtraMs : 0.0;
      next = cur - kStep;
      for (int i = 1; i < kMaxDownSteps && next - kStep >= low; ++i) {
        const double r = double(next) / double(cur);
        if (fx + fsr_add + sc * r * r <= budget * kFitShare)
          break;
        next -= kStep;
      }
    }
  } else if (test_s == 0) {
    over_since = {};
    fast_over_since = {};
  }
  if (next != cur) {
    // a step down: logged and applied below
  } else if (test_s > 0) {
#else
  if (test_s > 0) {
#endif
    if (seconds(last_change) >= test_s)
      next = cur == high ? low : high;
  } else if (seconds(last_change) >= kMinGapS) {
    // Down: over the budget for kOverS (Switch 2: handled above).
#if !defined(__SWITCH2__)
    if (ema > budget && cur > low) {
      if (over_since == Clock::time_point{})
        over_since = now;
      if (seconds(over_since) >= kOverS) {
        next = cur - kStep;
        // Switch 2: further down (up to kMaxDownSteps) when one step is
        // predicted to stay over 95% of the budget.
        // Aim for the step-up share (93% on Switch 2), not 95%: log 009 went
        // 1080 -> 936 -> 792 -> 648 in 12 s, three rebuilds for one scene.
        for (int i = 1; i < kMaxDownSteps && next - kStep >= low; ++i) {
          const double r = double(next) / double(cur);
          if (fixed + scaled * r * r <= budget * kFitShare)
            break;
          next -= kStep;
        }
      }
    } else {
      over_since = {};
    }
#endif
    // Up: the next height predicted under kFitShare of the budget for kFitS.
    const int up = (kSnapToEnds && REXCVAR_GET(bd_switch_fsr) >= 1)
                       ? high
                       : std::min(cur + kStep, high);
    // FSR's extra cost (measured ~4.8 ms over the bilinear present on
    // Switch 1) goes away at the configured height (bd_switch_fsr 1).
#if defined(__SWITCH2__)
    // FSR runs only below the output height (SwitchFsrWanted), with
    // bd_switch_fsr 1 or 2 (the Switch 2 default): a step that reaches it
    // drops the upscale's cost.
    const bool docked_now = appletGetOperationMode() == AppletOperationMode_Console;
    const int output_h =
        (docked_now && REXCVAR_GET(bd_switch2_docked_output_height) >= 1440) ? 1440 : 1080;
    const double fsr_extra =
        (REXCVAR_GET(bd_switch_fsr) >= 1 && cur < output_h && up >= output_h) ? kFsrExtraMs : 0.0;
#else
    const double fsr_extra =
        (REXCVAR_GET(bd_switch_fsr) == 1 && cur < high && up == high) ? kFsrExtraMs : 0.0;
#endif
    const double predicted = fixed + std::max(0.0, scaled - fsr_extra) *
                             (double(up) * double(up)) / (double(cur) * double(cur));
    const bool up_allowed = last_down == Clock::time_point{} ||
                            seconds(last_down) >= up_after_down_s;
    if (next == cur && up != cur && up_allowed && predicted < budget * kFitShare) {
      if (fit_since == Clock::time_point{})
        fit_since = now;
      if (seconds(fit_since) >= kFitS)
        next = up;
    } else {
      fit_since = {};
    }
  }
  // With FSR (bd_switch_fsr >= 1) the steps between cost more than they save
  // (FSR's ~4.8 ms over the bilinear present against ~2.5 ms for a step), so
  // the height goes straight to either end.
  if (kSnapToEnds && next != cur && test_s == 0 && REXCVAR_GET(bd_switch_fsr) >= 1)
    next = next < cur ? low : high;
  if (next == cur)
    return false;
  BD_INFO("[dynres] render height {} -> {} (GPU {:.1f} ms a frame, {:.1f} of it "
          "size-independent, budget {:.1f}{})",
          cur, next, ema, fixed, budget, test_s > 0 ? ", test mode" : "");
  g_dynamic_height.store(next, std::memory_order_relaxed);
#if defined(__SWITCH2__)
  if (next < cur && test_s == 0 && last_up != Clock::time_point{} &&
      seconds(last_up) < 20.0) {
    up_after_down_s = std::min(up_after_down_s * 2.0, 80.0);
    BD_INFO("[dynres] step up undone after {:.0f} s: next step up waits {:.0f} s",
            seconds(last_up), up_after_down_s);
  }
  if (next > cur)
    last_up = now;
#endif
  if (next < cur)
    last_down = now;
  last_change = now;
  over_since = {};
  fit_since = {};
#if defined(__SWITCH2__)
  fast_over_since = {};
  // The new size's GPU time is not known yet: start both averages at the
  // prediction so the next decision does not act on the old size's load.
  {
    const double r = double(next) / double(cur);
    const double fx = std::min(ema_fixed, ema);
    ema = fx + (ema - fx) * r * r;
    fast = std::min(fast, ema * 1.05);
  }
#endif
  return true;
}

bool SwitchFsrWanted(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h) {
  if (src_w >= dst_w && src_h >= dst_h)
    return false;
  switch (REXCVAR_GET(bd_switch_fsr)) {
  case 2:
    return true;
  case 1: {
    // Only below the configured height: there the present stays the
    // bilinear one it always was.
#if defined(__SWITCH2__)
    const int high = Switch2Range().high;
#else
    int high = REXCVAR_GET(bd_switch_handheld_height);
    if (high <= 0 || high >= 720)
      high = 720;
#endif
    return REXCVAR_GET(bd_switch_dynamic_resolution) && src_h + 8 < u32(high);
  }
  default:
    return false;
  }
}

void SpreadCurrentThread(int core) {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ||
      mask == 0)
    return;
  if (core < 0 || (mask & (u64(1) << core)) == 0)
    core = __builtin_ctzll(mask);
  const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask);
  if (R_FAILED(rc))
    BD_WARN("Switch: svcSetThreadCoreMask(core {}, mask {:#x}) failed: {:#x}",
            core, mask, rc);
}

void SetCurrentThreadPriority(int priority) {
  const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, u32(priority));
  if (R_FAILED(rc))
    BD_WARN("Switch: svcSetThreadPriority({:#x}) failed: {:#x}", priority, rc);
}

} // namespace bd::platform
