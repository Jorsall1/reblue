/**
 * @file        rex/ui/switch2_exit.h
 * @brief       Switch 2 (kaiHEN) exit: back to hbmenu without breaking the R hook
 *
 * Every way out of the process on the Switch 2 build goes through here: the
 * title's quit, HOME -> X, std::terminate, abort(), assert and fatal CPU
 * exceptions. Implemented in src/ui/switch2_exit.cpp, ported from
 * LostOdysseyRecomp/os/switch/switch_runtime.cpp (RunExit, CloseForHomeMenu,
 * EndProcessToHbmenu). Plain C so switch_crash_hooks.c and the libc supplement
 * can call it; no switch.h types (they collide with the SDK's).
 */
#pragma once

#if defined(__SWITCH__) && defined(__SWITCH2__)

#ifdef __cplusplus
extern "C" {
#endif

/* Main thread, first thing in main(): records the thread, takes appletLockExit,
 * starts the HOME -> X watchdog. */
void RexSwitch2ExitInit(void);

/* The UI loop calls this once per pass (the watchdog takes over HOME -> X
 * handling only while it stops). */
void RexSwitch2MainLoopHeartbeat(void);

/* Called by the UI loop to be woken when another thread asks for the exit. */
void RexSwitch2SetMainLoopWake(void (*wake)(void));

/* An exit was requested from another thread: the UI loop must call RexSwitch2Exit. */
int RexSwitch2ExitRequested(void);

/* Leave for hbmenu. On the main thread it runs the exit; on any other thread it
 * hands the exit to the main thread and ends the calling thread. */
__attribute__((noreturn)) void RexSwitch2Exit(int code);

/* A warm reboot (a restart-bound setting such as the text language): the next
 * RexSwitch2Exit asks the loader for this NRO (path, argv as envSetNextLoad
 * takes them; copied) instead of hbmenu. Falls back to hbmenu if the loader
 * refuses it. Same exit otherwise: R hook, exit lock and thread rules kept. */
void RexSwitch2SetRelaunch(const char* path, const char* argv);

/* HOME -> X: the system is closing the game. Release the exit lock and let it. */
__attribute__((noreturn)) void RexSwitch2CloseForHomeMenu(void);

/* Used by the svcSleepThread wrap (switch_perf.cpp): once the exit has started,
 * every thread but the one running it ends at its next sleep. */
int RexSwitch2ShouldEndThisThread(void);
__attribute__((noreturn)) void RexSwitch2EndThreadForExit(void);

#ifdef __cplusplus
}
#endif

#endif /* __SWITCH__ && __SWITCH2__ */
