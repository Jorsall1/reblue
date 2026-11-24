/*
 * Silent deaths on Switch: abort(), exit() and whatever goes to stderr.
 *
 * newlib's abort() returns to the HOME menu without a word (measured on the first
 * boot of the SDK, see switch_libc_supplement.c). rex::FatalError ends there, as do
 * std::terminate (an uncaught exception, a std::thread destroyed without join) and
 * Mesa's aborts. And they all warn on stderr first, which goes nowhere in an NRO.
 *
 * It has to be in the executable, in rexglue_switch_startup, and not in a library:
 * the definition of abort has to reach the link before the one in libc.a to
 * replace it.
 *
 *   stderr  -> <NRO folder>/logs/rex/rex_stderr.log, unbuffered, rewritten on every boot
 *   abort() -> <NRO folder>/logs/rex/rex_crash.log with the stack, and error 2345-0101
 *   exit()  -> the same, error 2345-0103. The orderly shutdown does not go through exit()
 *              (see windowed_app_main_switch.cpp), so getting here means some library
 *              decided to end the process on its own.
 *
 * The rexglue logs go to <NRO folder>/logs/rex/ and not to sdmc:/switch/, so they do not mix with
 * those of other programs or with the game's, which go to <NRO folder>/logs/. The folder comes from
 * argv[0], which libnx fills in before the constructors. Without argv[0], sdmc:/switch/ is used.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

/* Filled in by libnx (argv.c), which does not declare them in its headers. */
extern int __system_argc;
extern char** __system_argv;

/* Ends in '/'. Computed once at startup and never changes: the fault handler reads it without allocating memory. */
static char g_rex_log_dir[FS_MAX_PATH] = "sdmc:/switch/";

const char* RexSwitchLogDir(void) {
    return g_rex_log_dir;
}

static void RexSwitchInitLogDir(void) {
    if (__system_argc < 1 || !__system_argv || !__system_argv[0]) {
        return;
    }
    const char* nro = __system_argv[0];
    const char* barra = strrchr(nro, '/');
    if (!barra) {
        return;
    }
    /* without a device ("/switch/..."), the SD one: the path must also work for fsdevTranslatePath */
    const char* dispositivo = strchr(nro, ':') ? "" : "sdmc:";
    char dir[FS_MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%s%.*slogs", dispositivo, (int)(barra - nro + 1), nro);
    if (n <= 0 || (size_t)n + sizeof("/rex/") > sizeof(dir)) {
        return;
    }
    mkdir(dir, 0777);
    strcat(dir, "/rex");
    mkdir(dir, 0777);
    struct stat st;
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        strcat(dir, "/");
        strcpy(g_rex_log_dir, dir);
    }
}

void RexSwitchCrashLog(const char* reason, const ThreadExceptionDump* ctx, u64 stack, u64 pc);
#if defined(__SWITCH2__)
__attribute__((noreturn)) void RexSwitch2Exit(int code); /* rex/ui/switch2_exit.h */
#endif

void abort(void) {
    RexSwitchCrashLog("abort()", NULL, (u64)__builtin_frame_address(0),
                      (u64)__builtin_return_address(0));
#if defined(__SWITCH2__)
    /* kaiHEN: the error screen unloads kaiHEN. Back to hbmenu (switch2_exit.cpp). */
    RexSwitch2Exit(1);
#else
    diagAbortWithResult(MAKERESULT(Module_Libnx, 101));
#endif
}

#if defined(__SWITCH2__)
/*
 * libnx/libounce's own aborts. Its diagAbortWithResult is weak and ends in svcBreak,
 * which ends the process on the spot: no rex_crash.log, no [exit] line, and
 * kaiHEN's error screen. libounce calls it from hid (hidGetNpadStates*,
 * hidInitializeNpad, padConfigureInput), the applet (appletGetMessage,
 * appletProcessMessage), threadExit, __syscall_getreent, randomGet, virtmem and
 * __appInit. This strong definition writes the report and leaves through
 * RexSwitch2Exit like every other crash.
 */
void diagAbortWithResult(Result res) {
    char reason[96];
    snprintf(reason, sizeof(reason),
             "diagAbortWithResult 0x%x (modulo %u, descripcion %u) desde libnx/libounce",
             (unsigned)res, (unsigned)R_MODULE(res), (unsigned)R_DESCRIPTION(res));
    RexSwitchCrashLog(reason, NULL, (u64)__builtin_frame_address(0),
                      (u64)__builtin_return_address(0));
    RexSwitch2Exit(1);
}

/*
 * Session marker: <log dir>/rex_session.txt says "running" from start-up until the
 * exit (switch2_exit.cpp calls RexSwitchSessionEnded). If the next boot still finds
 * "running", the process was ended from outside with nothing written: a masked
 * system call, the system, a power loss. The previous run's snapshots and stderr
 * are kept as rex_prev_*.log before this run overwrites them, and
 * rex_prev_session.log says so.
 */
static int RexSwitchOpenLog(const char* name, int flags) {
    char ruta[FS_MAX_PATH];
    snprintf(ruta, sizeof(ruta), "%s%s", g_rex_log_dir, name);
    return open(ruta, flags, 0644);
}

static void RexSwitchKeepPrevious(const char* name) {
    char from[FS_MAX_PATH], to[FS_MAX_PATH];
    snprintf(from, sizeof(from), "%s%s", g_rex_log_dir, name);
    snprintf(to, sizeof(to), "%srex_prev_%s", g_rex_log_dir, name + 4); /* "rex_x" -> "rex_prev_x" */
    remove(to);           /* libnx fsdev rename does not overwrite */
    rename(from, to);
}

static void RexSwitchWriteSession(const char* text) {
    int fd = RexSwitchOpenLog("rex_session.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    ssize_t n = write(fd, text, strlen(text));
    (void)n;
    fsync(fd);
    close(fd);
}

static void RexSwitchSessionStart(void) {
    char prev[64] = {0};
    int fd = RexSwitchOpenLog("rex_session.txt", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, prev, sizeof(prev) - 1);
        (void)n;
        close(fd);
    }
    if (strncmp(prev, "running", 7) == 0) {
        RexSwitchKeepPrevious("rex_ultimo_a.log");
        RexSwitchKeepPrevious("rex_ultimo_b.log");
        RexSwitchKeepPrevious("rex_stderr.log");
        RexSwitchKeepPrevious("rex_perfil.log");
        static const char kNote[] =
            "The previous session ended without a report: the process was ended from outside\n"
            "(nothing ran: no fault handler, no abort, no [exit] line). Likely a masked system call,\n"
            "a GPU or system fault, or the power. (If rex_crash.log has a report from that time, the\n"
            "process died while leaving after it.) Its last state is kept here:\n"
            "  rex_prev_ultimo_a.log / rex_prev_ultimo_b.log  every thread's pc and stack, once a second\n"
            "                                                  (the one with the higher snapshot number is last)\n"
            "  rex_prev_stderr.log, rex_prev_perfil.log        its stderr and profile\n"
            "  ../reblue_NNN.log                               its game log (the second newest)\n";
        int out = RexSwitchOpenLog("rex_prev_session.log", O_WRONLY | O_CREAT | O_TRUNC);
        if (out >= 0) {
            ssize_t n = write(out, kNote, sizeof(kNote) - 1);
            (void)n;
            close(out);
        }
    }
    RexSwitchWriteSession("running\n");
}

void RexSwitchSessionEnded(int code) {
    char text[48];
    snprintf(text, sizeof(text), "ended, code %d\n", code);
    RexSwitchWriteSession(text);
}
#endif

static void RexSwitchExitHook(void) {
    RexSwitchCrashLog("exit() fuera del cierre ordenado", NULL, (u64)__builtin_frame_address(0),
                      (u64)__builtin_return_address(0));
#if defined(__SWITCH2__)
    RexSwitch2Exit(1);
#else
    diagAbortWithResult(MAKERESULT(Module_Libnx, 103));
#endif
}

/*
 * Priority 101, the first one allowed: before any static constructor of the
 * SDK, which can also die. The SD is already mounted: __appInit runs before the
 * constructors.
 */
__attribute__((constructor(101)))
static void RexSwitchCrashHooksInit(void) {
    RexSwitchInitLogDir();
#if defined(__SWITCH2__)
    RexSwitchSessionStart(); /* before stderr is reopened: it keeps the old one first */
#endif
    char ruta[FS_MAX_PATH];
    snprintf(ruta, sizeof(ruta), "%srex_stderr.log", g_rex_log_dir);
    if (freopen(ruta, "w", stderr)) {
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    atexit(RexSwitchExitHook);
}
