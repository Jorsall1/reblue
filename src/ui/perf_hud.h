/**
 * @file    ui/perf_hud.h
 * @brief   Switch 2: top-left frame rate and render resolution line
 *          (bd_switch2_show_fps).
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#if defined(__SWITCH2__)
#include <rex/ui/imgui_dialog.h>

struct ImGuiIO;

namespace rex::ui {
class ImGuiDrawer;
} // namespace rex::ui

namespace bd::ui {

// Two text draws into the foreground list a frame, from a fixed buffer: no
// allocation, no GPU work beyond ImGui's own vertex upload. The numbers come
// from the present (gpu::Video::Switch2HudStats), so the resolution is the one
// really shown, dynamic resolution included.
class PerfHudOverlay final : public rex::ui::ImGuiDialog {
public:
  explicit PerfHudOverlay(rex::ui::ImGuiDrawer *drawer);
  ~PerfHudOverlay();

protected:
  void OnDraw(ImGuiIO &io) override;
};

} // namespace bd::ui
#endif
