/**
 * @file    platform/switch_tuning.h
 * @brief   Nintendo Switch performance settings: the handheld GPU clock
 *          profile and the scene resolution in handheld mode. No-ops on the
 *          other platforms.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <rex/types.h>

namespace bd::platform {

#if defined(__SWITCH__)
// Asks the system for the handheld GPU profile (bd_switch_gpu_mhz) and points
// plume at the pipeline cache file. Once, after the config is loaded and before
// the GPU device is created.
void ApplySwitchClocks();

// The size the game renders at. Switch 1: in handheld mode when
// bd_switch_handheld_height is set (540 by default): false docked, or when it is
// 0. Switch 2: always, in both modes, between the mode's minimum and maximum
// height (handheld 576-1080, docked 720-1440; bd_switch2_*_height), the
// maximum without dynamic resolution. The swapchain stays at the display size
// (1920x1080 on Switch 2) and the frame is scaled to it (FSR 1 up, bilinear
// down).
bool SwitchHandheldRenderSize(u32 &w, u32 &h);

// Dynamic resolution (bd_switch_dynamic_resolution; Switch 1 handheld only,
// Switch 2 both modes): fed each frame's measured GPU time; true when the
// render size it wants changed, for the caller to apply
// (gpu::Output::Recompute, then the game's render targets are rebuilt at the
// next frame submit).
// fixed_ms: the part of gpu_ms that does not follow the render size (Switch 2:
// the start of the frame, the shadow map, the present), 0 if unknown.
bool SwitchDynamicResolutionFrame(double gpu_ms, double fixed_ms = 0.0);
// A movie frame is being copied into the game's textures (any thread). On
// Switch 2 dynamic resolution holds still while movies play and for a while
// after: their GPU time says nothing about the field's.
void SwitchNoteMovieFrame();
// The reblue Settings menu drew a frame (any thread). On Switch 2 dynamic
// resolution holds still while it is open and for a while after: the surfaces
// were rebuilt four times under it in log 006, the last one a step up, and the
// process died 7 s later (the "crash when changing Depth of Field").
void SwitchNoteMenuFrame();
#if defined(__SWITCH2__)
// The four Switch 2 render height limits (bd_switch2_<mode>_<max|min>_height),
// for the Settings menu. The setter keeps min <= max (moving the other limit
// when needed), saves nothing itself (the menu does) and asks for the render
// size to be recomputed: applied at the first frame after the Settings menu
// closes (never under it, see SwitchNoteMenuFrame).
enum class Switch2HeightLimit : int { HandheldMax, HandheldMin, DockedMax, DockedMin };
int Switch2ResolutionHeight(Switch2HeightLimit which);
bool SetSwitch2ResolutionHeight(Switch2HeightLimit which, int height);
// The top-left FPS / render resolution line (bd_switch2_show_fps).
bool Switch2ShowFps();
bool SetSwitch2ShowFps(bool on);
// FSR 1 upscaling on/off for the Settings menu: on is bd_switch_fsr 2, off 0.
bool SwitchFsrEnabled();
bool SetSwitchFsrEnabled(bool on);
#endif
// Switch 2: on a dock or undock, hands plume the new mode's output size
// (1920x1080 handheld, 2560x1440 docked). Called from the present before the
// swapchain resize check. Nothing on Switch 1.
void Switch2OutputModeTick();
// The number of cores this process may use (svcGetInfo InfoType_CoreMask): 3
// on Switch 1, 6 on Switch 2 under kaiHEN (0x3F).
unsigned SwitchProcessCoreCount();
// Whether the present pass upscales with FSR 1 (bd_switch_fsr) for an image of
// src_w x src_h shown at dst_w x dst_h.
bool SwitchFsrWanted(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h);

// For a host worker thread (the movie decoder): lets the calling thread run
// on every core of the process, starting on 'core'. A thread made with
// std::thread on Horizon is otherwise confined to the core it was created on.
void SpreadCurrentThread(int core);
// Horizon priority of the calling thread (0x2C..0x3B for an application).
void SetCurrentThreadPriority(int priority);
#else
inline void ApplySwitchClocks() {}
inline bool SwitchHandheldRenderSize(u32 &, u32 &) { return false; }
inline bool SwitchDynamicResolutionFrame(double, double = 0.0) { return false; }
inline void SwitchNoteMovieFrame() {}
inline void SwitchNoteMenuFrame() {}
inline void Switch2OutputModeTick() {}
inline unsigned SwitchProcessCoreCount() { return 0; }
inline bool SwitchFsrWanted(u32, u32, u32, u32) { return false; }
inline void SpreadCurrentThread(int) {}
inline void SetCurrentThreadPriority(int) {}
#endif

} // namespace bd::platform
