/**
 * @file        ui/switch2_exit.cpp
 * @brief       Switch 2 (kaiHEN) exit: back to hbmenu, kaiHEN's R hook kept
 *
 * Ported from LostOdysseyRecomp (LostOdysseyRecomp/os/switch/switch_runtime.cpp:
 * RunExit, CloseForHomeMenu, EndProcessToHbmenu, ScanOtherThreads,
 * ResumeThenReturnToLoader, EndThreadForExit; commits c093a8db, 6e6c3b99,
 * e9726ef3, 8de82b6c, 066bd3a2, 5cde9314; deadlock_and_hbmenu_fix.txt,
 * retroarch_quit_fix.txt, summary_016.txt). Confirmed on hardware there.
 *
 * kaiHEN's loader (ounce-hbloader) runs this NRO inside a donor game's process
 * and takes it back when libnx's exit reaches __nx_exit. What decides whether
 * hbmenu comes back and whether holding R still works on the next launch:
 *
 *  1. No ISelfController::Exit (libnx's __nx_applet_exit_mode = 1, which the
 *     Switch 1 build uses). Under title takeover am applies it to the donor
 *     game: the console drops to the HOME menu and hbmenu is gone.
 *  2. No error applet and no diagAbortWithResult: "The software was closed
 *     because an error occurred" unloads kaiHEN.
 *  3. The exit lock (appletLockExit) is held from start-up and released before
 *     the process leaves. Leaving with it held, or being force-ended after am's
 *     timeout, is a fatal application error that drops kaiHEN's R hook.
 *  4. __nx_exit must run on the process main thread: the loader resumes on the
 *     thread that calls it, and from any other thread it runs hbmenu with the
 *     wrong TLS.
 *  5. The loader parks threads the NRO leaves running, at most 32, and it
 *     cannot park a thread that is already paused. Past that it calls
 *     svcExitProcess, which ends the donor game. So: other threads end at their
 *     next sleep, the rest are paused for libnx's teardown and resumed right
 *     before the loader takes over.
 *  6. svcExitProcess ends the donor game: only HOME -> X uses it, as a fallback
 *     after the exit lock is released (the system is ending the process then).
 *  7. Never SetCurrentProcess (or any identity request) on a session from
 *     smGetService: libounce returns overrides that share kaiHEN HEN's own FS
 *     root, and a dead process ID there makes HEN's next exefs.nsp open fail
 *     (LO commit 5cde9314, summary_016). This file opens no service; the SD
 *     card stays on libnx's own fsdev mount.
 *
 * HOME -> X is different: the system is ending the process. CloseForHomeMenu
 * releases the exit lock and waits for it.
 */

#if defined(__SWITCH__) && defined(__SWITCH2__)

#include <rex/ui/switch2_exit.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <malloc.h>
#include <reent.h>
#include <unistd.h>

#include <switch.h>

extern "C" void __malloc_lock(struct _reent*);
extern "C" void __real_svcSleepThread(s64 nano);
// switch_crash_hooks.c: rex_session.txt "running" -> "ended" (a next boot that
// still finds "running" reports a silent death).
extern "C" void RexSwitchSessionEnded(int code);

namespace rex {
void FlushLogging();
}

namespace {

std::atomic<bool> g_initialized{false};
Handle g_main_thread = INVALID_HANDLE;
std::atomic<bool> g_exit_started{false};
std::atomic<bool> g_exit_requested{false};
std::atomic<bool> g_home_close{false};
std::atomic<bool> g_end_threads{false};
std::atomic<bool> g_loader_handoff{false};
std::atomic<u64> g_heartbeat{0};
std::atomic<void (*)(void)> g_wake{nullptr};
// RexSwitch2SetRelaunch: the NRO the loader runs next instead of hbmenu.
char g_relaunch_path[512] = {};
char g_relaunch_argv[1024] = {};
Handle g_exit_runner = INVALID_HANDLE;
Thread g_watchdog;

bool OnMainThread() {
  return g_main_thread != INVALID_HANDLE && threadGetCurHandle() == g_main_thread;
}

// stderr is rex_stderr.log, unbuffered (switch_crash_hooks.c). write(2) does
// not take the FILE lock a crashed or paused thread may hold.
void Mark(const char* text) {
  const ssize_t n = write(STDERR_FILENO, text, std::strlen(text));
  (void)n;
}

// rex::FlushLogging on its own thread, for at most 300 ms: a thread that died
// or will be paused can hold the logger's lock, and the exit must not hang on it.
void BoundedFlush() {
  static Thread thread;
  static std::atomic<bool> done{false};
  static bool started = false;
  if (!started) {
    started = true;
    if (R_FAILED(threadCreate(&thread, [](void*) {
          rex::FlushLogging();
          done.store(true, std::memory_order_release);
        }, nullptr, nullptr, 0x8000, 0x2C, -2)) ||
        R_FAILED(threadStart(&thread))) {
      return;
    }
  }
  for (int i = 0; i < 30 && !done.load(std::memory_order_acquire); ++i) {
    __real_svcSleepThread(10'000'000ll);
  }
}

// ---- Threads (libnx ThreadVars at the end of each 0x200-byte TLS slot, the
// way the loader finds them: handleNroThreads in ounce-hbloader) -------------
struct ExitThreadVars {
  u32 magic;
  Handle handle;
  u64 thread;
  u64 reent;
  u64 tls_tp;
};
static_assert(sizeof(ExitThreadVars) == 0x20);
constexpr u32 kThreadVarsMagic = 0x21545624;  // "!TV$"
constexpr u64 kTlsSlotSize = 0x200;

struct Census {
  u32 running = 0;
  u32 ended = 0;
  u32 paused = 0;
};

constexpr u32 kMaxPaused = 256;
Handle g_paused[kMaxPaused];
u32 g_paused_count = 0;
LoaderReturnFn g_loader_return = nullptr;

Census ScanOtherThreads(bool pause) {
  Census census;
  const u64 own_slot = reinterpret_cast<u64>(armGetTls()) & ~(kTlsSlotSize - 1);
  const Handle self = threadGetCurHandle();
  u64 address = 0;
  for (;;) {
    MemoryInfo info{};
    u32 page_info = 0;
    if (R_FAILED(svcQueryMemory(&info, &page_info, address))) break;
    if (info.type == MemType_ThreadLocal && (info.perm & Perm_R)) {
      for (u64 slot = info.addr; slot + kTlsSlotSize <= info.addr + info.size; slot += kTlsSlotSize) {
        if (slot == own_slot) continue;
        ExitThreadVars vars;
        std::memcpy(&vars, reinterpret_cast<const void*>(slot + kTlsSlotSize - sizeof(vars)), sizeof(vars));
        if (vars.magic != kThreadVarsMagic || vars.handle == INVALID_HANDLE || vars.handle == self) continue;
        u64 thread_id = 0;
        if (R_FAILED(svcGetThreadId(&thread_id, vars.handle))) continue;
        s32 index = -1;
        if (R_SUCCEEDED(svcWaitSynchronization(&index, &vars.handle, 1, 0))) {
          ++census.ended;  // a thread handle is signaled once the thread has ended
          continue;
        }
        ++census.running;
        if (pause && g_paused_count < kMaxPaused &&
            R_SUCCEEDED(svcSetThreadActivity(vars.handle, ThreadActivity_Paused))) {
          g_paused[g_paused_count++] = vars.handle;
          ++census.paused;
        }
      }
    }
    const u64 next = info.addr + info.size;
    if (next <= address) break;
    address = next;
  }
  return census;
}

// libnx's __libnx_exit ends in __nx_exit(0, envGetExitFuncPtr()): this runs
// there, after __appExit, on the main thread, right before the loader. The
// loader pauses each thread itself and treats an already paused one as a
// thread it cannot park (svcExitProcess: the donor game closes), so the
// pauses are undone first. Only system calls from here.
[[noreturn]] void ResumeThenReturnToLoader(int result) {
  g_loader_handoff.store(true, std::memory_order_release);
  for (u32 i = 0; i < g_paused_count; ++i) {
    svcSetThreadActivity(g_paused[i], ThreadActivity_Runnable);
  }
  g_loader_return(result);
  __builtin_unreachable();
}

[[noreturn]] void RunExit(int code) {
  char line[224];
  std::snprintf(line, sizeof(line), "[exit] Switch 2: leaving for hbmenu on the %s thread (code %d)\n",
                OnMainThread() ? "main" : "calling", code);
  Mark(line);
  RexSwitchSessionEnded(code);
  BoundedFlush();

  const bool next_load = envHasNextLoad();
  if (next_load) {
    Result rc = 1;
    if (g_relaunch_path[0]) {
      // Warm reboot: this NRO again, so the new setting is read at start-up.
      rc = envSetNextLoad(g_relaunch_path, g_relaunch_argv);
      std::snprintf(line, sizeof(line), "[exit] next load %s (relaunch): %s (0x%x)\n",
                    g_relaunch_path, R_SUCCEEDED(rc) ? "set" : "refused", unsigned(rc));
      Mark(line);
    }
    if (R_FAILED(rc)) {
      rc = envSetNextLoad("sdmc:/hbmenu.nro", "\"sdmc:/hbmenu.nro\"");
      std::snprintf(line, sizeof(line), "[exit] next load sdmc:/hbmenu.nro: %s (0x%x)\n",
                    R_SUCCEEDED(rc) ? "set" : "refused", unsigned(rc));
      Mark(line);
    }
  } else {
    Mark("[exit] the loader has no next-load support; ordinary libnx exit\n");
  }

  // Every other thread ends at its next svcSleepThread (rule 5). Up to 1.5 s.
  g_exit_runner = threadGetCurHandle();
  g_end_threads.store(true, std::memory_order_release);
  Census census = ScanOtherThreads(false);
  const u32 running_at_start = census.running;
  for (int tick = 0; tick < 60 && census.running; ++tick) {
    __real_svcSleepThread(25'000'000ll);
    census = ScanOtherThreads(false);
  }

  // Taken and never released before any thread is paused: a thread paused
  // inside malloc would hold the lock libnx's teardown needs. Recursive, so
  // this thread can still allocate.
  __malloc_lock(_REENT);
  census = ScanOtherThreads(true);
  if (census.paused) {
    g_loader_return = envGetExitFuncPtr();
    if (g_loader_return) {
      envSetExitFuncPtr(&ResumeThenReturnToLoader);
    } else {
      for (u32 i = 0; i < g_paused_count; ++i) svcSetThreadActivity(g_paused[i], ThreadActivity_Runnable);
    }
  }
  std::snprintf(line, sizeof(line),
                "[exit] threads: %u running at the request, %u ended, %u still running, %u paused "
                "(the loader parks up to 32; more ends the donor game)\n",
                running_at_start, census.ended, census.running, census.paused);
  Mark(line);

  // Rule 3: unlock before anything else of libnx's teardown runs.
  appletUnlockExit();
  Mark(!next_load ? "[exit] exit lock released; _Exit -> libnx exit\n"
       : g_relaunch_path[0]
           ? "[exit] exit lock released; _Exit -> libnx exit -> kaiHEN loader -> next load (relaunch)\n"
           : "[exit] exit lock released; _Exit -> libnx exit -> kaiHEN loader -> hbmenu\n");
  // Rule 1: __nx_applet_exit_mode stays 0, so libnx sends no ISelfController::Exit.
  // _Exit, not exit: libnx's own teardown (__appExit, GPU and window exit,
  // service close, __nx_exit) without static destructors or atexit handlers
  // (switch_crash_hooks.c's exit() hook would end in diagAbortWithResult).
  std::_Exit(code);
}

void WatchdogMain(void*) {
  // HOME -> X while the UI loop is stuck: am waits for the exit lock, and
  // after its timeout it ends the process as a fatal error (rule 3). When the
  // loop has not passed for 3 s of watchdog time, poll the applet here. A
  // pass that took over 1 s means the whole process was suspended (HOME menu,
  // sleep): the count starts again, so both threads never poll at once.
  u64 last_seen = 0;
  int stale = 0;
  u64 last_tick = armGetSystemTick();
  for (;;) {
    svcSleepThread(100'000'000ll);  // through the wrap: ends here once the exit starts
    const u64 now = armGetSystemTick();
    const bool suspended = armTicksToNs(now - last_tick) > 1'000'000'000ull;
    last_tick = now;
    if (g_exit_started.load(std::memory_order_acquire)) continue;
    const u64 beat = g_heartbeat.load(std::memory_order_acquire);
    if (suspended || beat != last_seen) {
      last_seen = beat;
      stale = 0;
      continue;
    }
    if (++stale < 30) continue;
    if (!appletMainLoop() || g_home_close.load(std::memory_order_acquire)) {
      Mark("[exit] HOME -> X while the UI loop is stuck (watchdog)\n");
      RexSwitch2CloseForHomeMenu();
    }
  }
}

void OnAppletHook(AppletHookType hook, void*) {
  if (hook == AppletHookType_OnExitRequest) g_home_close.store(true, std::memory_order_release);
}
AppletHookCookie g_hook_cookie;

}  // namespace

extern "C" {

void RexSwitch2ExitInit(void) {
  if (g_initialized.exchange(true)) return;
  g_main_thread = threadGetCurHandle();
  const Result rc = appletLockExit();
  char line[160];
  std::snprintf(line, sizeof(line), "[exit] Switch 2: exit lock %s (0x%x), applet type %d, next load %s\n",
                R_SUCCEEDED(rc) ? "held" : "refused", unsigned(rc), int(appletGetAppletType()),
                envHasNextLoad() ? "supported" : "not supported");
  Mark(line);
  appletHook(&g_hook_cookie, OnAppletHook, nullptr);
  // Above the game's threads so busy cores cannot starve it.
  if (R_SUCCEEDED(threadCreate(&g_watchdog, WatchdogMain, nullptr, nullptr, 0x4000, 0x2B, -2))) {
    threadStart(&g_watchdog);
  }
}

void RexSwitch2MainLoopHeartbeat(void) {
  g_heartbeat.fetch_add(1, std::memory_order_acq_rel);
}

void RexSwitch2SetMainLoopWake(void (*wake)(void)) {
  g_wake.store(wake, std::memory_order_release);
}

int RexSwitch2ExitRequested(void) {
  return g_exit_requested.load(std::memory_order_acquire) ? 1 : 0;
}

void RexSwitch2SetRelaunch(const char* path, const char* argv) {
  if (!path || !*path || std::strlen(path) >= sizeof(g_relaunch_path)) {
    g_relaunch_path[0] = 0;
    return;
  }
  std::snprintf(g_relaunch_path, sizeof(g_relaunch_path), "%s", path);
  if (argv && std::strlen(argv) < sizeof(g_relaunch_argv))
    std::snprintf(g_relaunch_argv, sizeof(g_relaunch_argv), "%s", argv);
  else
    std::snprintf(g_relaunch_argv, sizeof(g_relaunch_argv), "\"%s\"", path);
}

void RexSwitch2Exit(int code) {
  // HOME -> X came first: the system is ending the process, so let it.
  if (g_home_close.load(std::memory_order_acquire)) RexSwitch2CloseForHomeMenu();
  if (!OnMainThread() && g_main_thread != INVALID_HANDLE) {
    // Rule 4: the main thread runs the exit. It polls every 100 ms (the UI
    // loop's wait) and is woken here at once.
    if (!g_exit_requested.exchange(true, std::memory_order_acq_rel)) {
      Mark("[exit] requested on another thread; handing the exit to the main thread\n");
    }
    if (void (*wake)(void) = g_wake.load(std::memory_order_acquire)) wake();
    RexSwitch2EndThreadForExit();
  }
  if (g_exit_started.exchange(true, std::memory_order_acq_rel)) {
    RexSwitch2EndThreadForExit();  // already running (a fault during the exit)
  }
  RunExit(code);
}

void RexSwitch2CloseForHomeMenu(void) {
  if (g_exit_started.exchange(true, std::memory_order_acq_rel) && !OnMainThread()) {
    RexSwitch2EndThreadForExit();
  }
  Mark("[exit] HOME -> X: closing for the system (log flushed, exit lock released)\n");
  RexSwitchSessionEnded(0);
  BoundedFlush();
  appletUnlockExit();
  // The system ends the process within moments of the unlock. If not after
  // 3 s, end it here: the close was requested, so this is not an error.
  for (int i = 0; i < 30; ++i) __real_svcSleepThread(100'000'000ll);
  Mark("[exit] HOME -> X: still running 3 s after the unlock; svcExitProcess\n");
  svcExitProcess();
  __builtin_unreachable();
}

int RexSwitch2ShouldEndThisThread(void) {
  return g_end_threads.load(std::memory_order_acquire) && threadGetCurHandle() != g_exit_runner;
}

void RexSwitch2EndThreadForExit(void) {
  // After the handoff the loader is scanning: block where it can park us.
  if (g_loader_handoff.load(std::memory_order_acquire)) {
    for (;;) __real_svcSleepThread(INT64_MAX);
  }
  // svcExitThread, not threadExit or pthread_exit: no TLS destructors or
  // allocator calls on the way out. The loader closes the handle.
  for (;;) svcExitThread();
}

}  // extern "C"

#endif /* __SWITCH__ && __SWITCH2__ */
