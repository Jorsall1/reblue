/**
 * @file    ui/perf_hud.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "ui/perf_hud.h"

#if defined(__SWITCH2__)
#include <cstdio>

#include <imgui.h>

#include "gpu/device.h"

namespace bd::ui {

PerfHudOverlay::PerfHudOverlay(rex::ui::ImGuiDrawer *drawer)
    : rex::ui::ImGuiDialog(drawer) {}

PerfHudOverlay::~PerfHudOverlay() = default;

void PerfHudOverlay::OnDraw(ImGuiIO &io) {
  (void)io;
  float fps = 0.0f;
  u32 w = 0, h = 0;
  bool fsr = false;
  bd::gpu::Video::Switch2HudStats(fps, w, h, fsr);

  char line[64];
  if (w == 0 || h == 0)
    std::snprintf(line, sizeof(line), "%.0f FPS", double(fps));
  else
    std::snprintf(line, sizeof(line), "%.0f FPS | %ux%u%s", double(fps),
                  unsigned(w), unsigned(h), fsr ? " FSR" : "");

  constexpr float kPad = 12.0f;
  constexpr ImU32 kText = IM_COL32(255, 255, 255, 230);
  constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 190);
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  dl->AddText(ImVec2(kPad + 1.5f, kPad + 1.5f), kShadow, line);
  dl->AddText(ImVec2(kPad, kPad), kText, line);
}

} // namespace bd::ui
#endif
