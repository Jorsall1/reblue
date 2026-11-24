/**
 * @file    core/settings_migration.cpp
 * @brief   The config steps, in version order, and the stamp recording which
 *          of them a config has already been through.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "core/settings_migration.h"

#include <string>

#include <rex/cvar.h>
#include <rex/types.h>
#include <rex/ui/flags.h>

#include "core/logging.h"
#include "gpu/gpu.h"

REXCVAR_DEFINE_INT32(config_version, 0, "reblue",
                     "Config schema version this file was last written by")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bd {
namespace {

#if defined(__SWITCH2__)
// 3: Switch 2 graphics back on the Low preset once (session 013).
constexpr i32 kConfigVersion = 3;
#else
constexpr i32 kConfigVersion = 2;
#endif

struct Step {
  i32 version;
  const char *what;
  void (*Apply)();
};

bool ConfigWasLoaded() {
  for (const rex::cvar::FlagEntry &flag : rex::cvar::GetRegistry()) {
    if (flag.source == rex::cvar::Source::kConfig)
      return true;
  }
  return false;
}

void SplitWindowSizeFromResolution() {
  if (rex::cvar::HasNonDefaultValue("resolution") ||
      !rex::cvar::HasNonDefaultValue("window_width"))
    return;
  const i32 w = REXCVAR_GET(window_width);
  const i32 h = REXCVAR_GET(window_height);
  if (w <= 0 || h <= 0)
    return;
  rex::cvar::SetFlagByName("resolution",
                           std::to_string(w) + "x" + std::to_string(h));
}

void PreserveUngatedPostAndReflections() {
  rex::cvar::SetFlagByName(
      "bd_post_quality",
      std::to_string(static_cast<i32>(gpu::PostQuality::High)));
  rex::cvar::SetFlagByName(
      "bd_reflection_quality",
      std::to_string(static_cast<i32>(gpu::ReflectionQuality::High)));
}

#if defined(__SWITCH2__)
// The recommended reblue.toml copied with earlier builds pinned 2x shadows
// (bd_shadow_distance 2.0, bd_shadow_dimension 2048), and the Settings menu
// tests left supersampling and 8192 maps behind (log 009). The Switch 2 starts
// once from the Low preset (shadows 1x) for the GPU headroom dynamic
// resolution needs; anything changed in the menu afterwards is kept.
void Switch2LowPreset() {
  const gpu::PresetBundle &p =
      gpu::kPresets[static_cast<u32>(gpu::QualityPreset::Low)];
  rex::cvar::SetFlagByName("bd_supersampling", std::to_string(p.superSampling));
  rex::cvar::SetFlagByName("bd_msaa", std::to_string(p.msaa));
  rex::cvar::SetFlagByName("bd_anisotropy", std::to_string(p.anisotropy));
  rex::cvar::SetFlagByName("bd_shadow_distance", "1.0");
  rex::cvar::SetFlagByName("bd_shadow_dimension", std::to_string(p.shadowDimension));
  rex::cvar::SetFlagByName("bd_post_quality",
                           std::to_string(static_cast<i32>(p.postQuality)));
  rex::cvar::SetFlagByName("bd_reflection_quality",
                           std::to_string(static_cast<i32>(p.reflectionQuality)));
}
#endif

constexpr Step kSteps[] = {
    {1, "window size split from render resolution",
     SplitWindowSizeFromResolution},
    {2, "post and reflection cost held at what it was",
     PreserveUngatedPostAndReflections},
#if defined(__SWITCH2__)
    {3, "Switch 2: graphics on the Low preset (shadows 1x)", Switch2LowPreset},
#endif
};

} // namespace

void SettingsMigration::Apply() {
  const i32 from = REXCVAR_GET(config_version);
  if (from >= kConfigVersion)
    return;
  if (ConfigWasLoaded()) {
    for (const Step &step : kSteps) {
      if (step.version <= from)
        continue;
      step.Apply();
      BD_INFO("[config] migrated to v{}: {}", step.version, step.what);
    }
  }
  rex::cvar::SetFlagByName("config_version", std::to_string(kConfigVersion));
}

} // namespace bd
