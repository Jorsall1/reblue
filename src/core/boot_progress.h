/**
 * @file    core/boot_progress.h
 * @brief   Boot loading bar state (Switch 2): what the loading code reports
 *          and what the bar thread in reblue_app.cpp draws.
 *
 * The boot is a few long phases on the main thread (the game: device mount,
 * the pack index, the renderer) with nothing on screen. Each phase owns a
 * slice [lo, hi] of the bar. Code inside a phase that knows how far it is
 * reports a fraction of it; one that does not is covered by a time estimate
 * that creeps toward 90% of the slice. The bar never moves backwards.
 *
 * Outside Switch 2 every call is a no-op.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace bd::boot_progress {

#if defined(__SWITCH2__)
namespace detail {
inline std::atomic<float> g_lo{0.0f};
inline std::atomic<float> g_hi{0.0f};
inline std::atomic<float> g_part{0.0f};     // reported fraction of the phase
inline std::atomic<float> g_expect_s{1.0f}; // expected phase length
inline std::atomic<long long> g_start_ns{0};

inline long long NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
} // namespace detail

// Enters the phase that fills [lo, hi] of the bar in about expect_s seconds.
inline void Phase(float lo, float hi, float expect_s) {
  detail::g_part.store(0.0f, std::memory_order_relaxed);
  detail::g_expect_s.store(std::max(expect_s, 0.1f), std::memory_order_relaxed);
  detail::g_start_ns.store(detail::NowNs(), std::memory_order_relaxed);
  detail::g_lo.store(lo, std::memory_order_relaxed);
  detail::g_hi.store(hi, std::memory_order_release);
}

// done / total of the current phase.
inline void Report(unsigned done, unsigned total) {
  if (total == 0)
    return;
  const float f = std::clamp(float(done) / float(total), 0.0f, 1.0f);
  detail::g_part.store(f, std::memory_order_relaxed);
}

// The bar position, 0..1. Called by the drawing thread only.
inline float Current() {
  static float shown = 0.0f;
  const float hi = detail::g_hi.load(std::memory_order_acquire);
  const float lo = detail::g_lo.load(std::memory_order_relaxed);
  const float t =
      float(detail::NowNs() - detail::g_start_ns.load(std::memory_order_relaxed)) /
      1e9f;
  const float guess =
      0.9f * (1.0f - std::exp(-t / detail::g_expect_s.load(std::memory_order_relaxed)));
  const float part =
      std::max(detail::g_part.load(std::memory_order_relaxed), guess);
  shown = std::max(shown, lo + (hi - lo) * part);
  return std::clamp(shown, 0.0f, 1.0f);
}
#else
inline void Phase(float, float, float) {}
inline void Report(unsigned, unsigned) {}
inline float Current() { return 0.0f; }
#endif

} // namespace bd::boot_progress
