/*
 * libschrodinger.c — LD_PRELOAD fatal-signal crash-dialog library
 *
 * Intercepts only fatal signals (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT)
 * and, on a crash, shows a Microsoft-style Chinese error dialog via a
 * clean Qt helper process:
 *   - 确定 ("OK")        -> restore the previous disposition and re-deliver
 *                          the original signal (normal termination / core).
 *   - 取消 ("Cancel")    -> close the dialog, launch gdb in a terminal
 *                          attached to the still-frozen process, then, once
 *                          gdb exits, terminate through the original signal.
 *
 * Ordinary errno messages, normal exits, SIGINT and SIGTERM are untouched.
 *
 * Build: make
 * Use:   LD_PRELOAD=$PWD/build/libschrodinger.so <program>
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

/* Crash-category codes shared with the Qt helper (see schrodinger-dialog.cpp). */
typedef enum
{
    KIND_SEGV_READ = 0,
    KIND_SEGV_WRITTEN,
    KIND_BUS_ALIGNED,
    KIND_BUS_READ,
    KIND_ILL_EXECUTE,
    KIND_FPE_INT_ZERO,
    KIND_FPE_INT_OVERFLOW,
    KIND_FPE_FLT_DIVIDE,
    KIND_FPE_FLT_OVERFLOW,
    KIND_FPE_FLT_UNDERFLOW,
    KIND_FPE_FLT_INVALID,
    KIND_FPE_FLT_SUBSCRIPT,
    KIND_FPE_GENERIC,
    KIND_ABORT,
    KIND_COUNT,
} CrashKind;

/* The exact set of fatal signals we handle, in a fixed order shared by every
 * index-based lookup. Ordinary termination and user-interaction signals are
 * deliberately absent. */
static const int g_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
#define NSIG_HANDLED (sizeof(g_signals) / sizeof(g_signals[0]))

/* Original dispositions cached at install time, restored before re-delivery. */
static struct sigaction g_old[NSIG_HANDLED];

/* Precomputed at load time (constructor) so the async-signal handler never
 * has to allocate or touch libc formatting routines. */
static char g_helper_path[PATH_MAX];
static char g_program[NAME_MAX + 1];
static char g_konsole[PATH_MAX];
static char g_kitty[PATH_MAX];
static char g_gdb[PATH_MAX];
static char g_pwndbg[PATH_MAX];

/* LD_PRELOAD-free environment snapshot, built at load time. unsetenv() is NOT
 * async-signal-safe (it locks environ), so removing the preload at crash time
 * would risk deadlock if another thread held that lock when the crash hit. */
static char **g_clean_envp;

/* Alternate signal stack: a stack-overflow SIGSEGV leaves no usable stack for
 * the handler, so it runs here (16 KiB) via SA_ONSTACK. */
static char g_alt_stack[16 * 1024];

/* Reentrancy guard: a second fatal signal arriving mid-handler is fatal. */
static volatile sig_atomic_t g_in_handler = 0;

/* Crash-time argument buffers (fixed, async-signal-safe formatting only). */
static char g_sigstr[16];
static char g_codestr[16];
static char g_ipstr[19]; /* "0x" + 16 hex digits + NUL */
static char g_faultstr[19];
static char g_kindstr[8];

/* Format an unsigned address as a fixed-width "0x0000000000000000" string.
 * Async-signal-safe: pure arithmetic, no allocation. */
static void fmt_hex(char *dst, uintptr_t value)
{
    static const char hexdigits[] = "0123456789abcdef";
    dst[0] = '0';
    dst[1] = 'x';
    for (int i = 17; i >= 2; --i)
    {
        dst[i] = hexdigits[value & 0x0FU];
        value >>= 4;
    }
    dst[18] = '\0';
}

/* Format a (possibly negative) int as decimal. Async-signal-safe. */
static void fmt_dec(char *dst, int value)
{
    char tmp[16];
    int i = 0;
    unsigned u = value < 0 ? (unsigned)(-(long)value) : (unsigned)value;
    do
    {
        tmp[i++] = (char)('0' + (u % 10U));
        u /= 10U;
    } while (u != 0U);
    char *w = dst;
    if (value < 0)
    {
        *w++ = '-';
    }
    while (i > 0)
    {
        *w++ = tmp[--i];
    }
    *w = '\0';
}

static int sig_index(int sig)
{
    for (size_t i = 0; i < NSIG_HANDLED; ++i)
    {
        if (g_signals[i] == sig)
        {
            return (int)i;
        }
    }
    return -1;
}

/* Restore a disposition, unblock the signal, deliver it, and only reach
 * _exit if delivery unexpectedly returns (e.g. the previous disposition
 * ignored the signal). _Noreturn. */
static _Noreturn void redeliver(int sig, int use_default)
{
    struct sigaction sa;
    int idx = sig_index(sig);
    if (use_default || idx < 0)
    {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
    }
    else
    {
        sa = g_old[idx];
    }
    sigaction(sig, &sa, NULL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    raise(sig);
    _exit(128 + sig);
}

/* Extract the faulting instruction pointer from the signal context. */
static uintptr_t instruction_pointer(void *uap)
{
    ucontext_t *uc = (ucontext_t *)uap;
#ifdef __x86_64__
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.pc;
#else
    (void)uc;
    return 0;
#endif
}

/* x86-64 page-fault error code (REG_ERR): bit 0 = protection vs not-present,
 * bit 1 = write vs read. Only meaningful for SIGSEGV/SIGBUS; 0 elsewhere. */
static uintptr_t fault_error_code(void *uap)
{
    ucontext_t *uc = (ucontext_t *)uap;
#ifdef __x86_64__
    return (uintptr_t)uc->uc_mcontext.gregs[REG_ERR];
#else
    (void)uc;
    return 0;
#endif
}

/* The "referenced address" shown by the access-violation and illegal
 * instruction templates. SIGFPE/SIGABRT never expose a fake fault address. */
static uintptr_t fault_address(int sig, const siginfo_t *info, uintptr_t ip)
{
    switch (sig)
    {
    case SIGSEGV:
    case SIGBUS:
        return (uintptr_t)info->si_addr;
    case SIGILL:
        return info->si_addr != NULL ? (uintptr_t)info->si_addr : ip;
    default:
        return 0;
    }
}

/* Map (signal, si_code, page-fault error code) to a dialog category.
 *
 * The "read"/"written" wording is about the ACCESS TYPE, which si_code alone
 * cannot tell: reading a PROT_NONE page is SEGV_ACCERR, writing an unmapped
 * page is SEGV_MAPERR. On x86-64 we use the page-fault error code's write
 * bit; elsewhere we fall back to the fixed "read". BUS_ADRALN stays the
 * intentionally broken "aligned". */
static int classify(int sig, int code, uintptr_t fault_err)
{
    switch (sig)
    {
    case SIGSEGV:
        return (fault_err & 0x2U) != 0 ? KIND_SEGV_WRITTEN : KIND_SEGV_READ;
    case SIGBUS:
        return code == BUS_ADRALN ? KIND_BUS_ALIGNED : KIND_BUS_READ;
    case SIGILL:
        return KIND_ILL_EXECUTE;
    case SIGFPE:
        switch (code)
        {
        case FPE_INTDIV:
            return KIND_FPE_INT_ZERO;
        case FPE_INTOVF:
            return KIND_FPE_INT_OVERFLOW;
        case FPE_FLTDIV:
            return KIND_FPE_FLT_DIVIDE;
        case FPE_FLTOVF:
            return KIND_FPE_FLT_OVERFLOW;
        case FPE_FLTUND:
            return KIND_FPE_FLT_UNDERFLOW;
        case FPE_FLTINV:
            return KIND_FPE_FLT_INVALID;
        case FPE_FLTSUB:
            return KIND_FPE_FLT_SUBSCRIPT;
        default:
            return KIND_FPE_GENERIC;
        }
    case SIGABRT:
        return KIND_ABORT;
    default:
        return KIND_FPE_GENERIC;
    }
}

/* Raw fork that skips glibc's atfork handlers: the signal may have
 * interrupted a thread holding an internal lock, and a locked allocator in
 * the child would deadlock. x86-64 has a dedicated fork syscall; aarch64
 * (like glibc itself) goes through clone. */
static pid_t crash_fork(void)
{
#ifdef __x86_64__
    return (pid_t)syscall(SYS_fork);
#elif defined(__aarch64__)
    return (pid_t)syscall(SYS_clone, SIGCHLD, 0);
#else
    return fork();
#endif
}

/* execve with the LD_PRELOAD-free environment snapshot, falling back to
 * execv (which just reads environ, no lock) if the snapshot is unavailable. */
static void exec_clean(const char *path, char *const argv[])
{
    if (g_clean_envp != NULL)
    {
        execve(path, argv, g_clean_envp);
    }
    execv(path, argv);
}

/* Stage 2 (取消 only): fork a child that launches gdb -p <pid> in a
 * terminal, wait for it to exit, then terminate through the original
 * signal. The parent stays frozen in the handler while gdb attaches. */
static _Noreturn void launch_debugger(int sig)
{
    char pidstr[16];
    fmt_dec(pidstr, (int)getpid());

    pid_t dbg = crash_fork();
    if (dbg < 0)
    {
        redeliver(sig, 0);
    }
    if (dbg == 0)
    {
        /* Child: exec through the LD_PRELOAD-free environment so neither the
         * terminal, debugger, nor any inferior inherits this crash handler.
         * Prefer pwndbg over gdb, and kitty over konsole. */
        char *d = g_pwndbg[0] != '\0' ? g_pwndbg : g_gdb;
        if (d[0] != '\0' && g_kitty[0] != '\0')
        {
            char *kitty_argv[] = {g_kitty, d, "-p", pidstr, NULL};
            exec_clean(g_kitty, kitty_argv);
        }
        if (d[0] != '\0' && g_konsole[0] != '\0')
        {
            char *konsole_argv[] = {g_konsole, "-e", d, "-p", pidstr, NULL};
            exec_clean(g_konsole, konsole_argv);
        }
        _exit(126);
    }

    int status;
    if (waitpid(dbg, &status, 0) < 0)
    {
        redeliver(sig, 0);
    }
    /* gdb (and its terminal) closed; finish through the original signal. */
    redeliver(sig, 0);
}

static void crash_handler(int sig, siginfo_t *info, void *uap)
{
    if (g_in_handler)
    {
        /* Re-entered by a second fatal signal: do not attempt a second
         * dialog. Restore the default disposition and die now. */
        redeliver(sig, 1);
    }
    g_in_handler = 1;

    if (g_helper_path[0] == '\0')
    {
        redeliver(sig, 0);
    }

    uintptr_t ip = instruction_pointer(uap);
    uintptr_t fault = fault_address(sig, info, ip);
    uintptr_t fault_err = fault_error_code(uap);
    int kind = classify(sig, info->si_code, fault_err);

    fmt_hex(g_ipstr, ip);
    fmt_hex(g_faultstr, fault);
    fmt_dec(g_sigstr, sig);
    fmt_dec(g_codestr, info->si_code);
    fmt_dec(g_kindstr, kind);

    char *dialog_argv[] = {
        g_helper_path, g_program, g_sigstr, g_codestr, g_ipstr, g_faultstr, g_kindstr, NULL,
    };

    /* Stage 1: fork a clean dialog child. */
    pid_t dlg = crash_fork();
    if (dlg < 0)
    {
        redeliver(sig, 0);
    }
    if (dlg == 0)
    {
        exec_clean(g_helper_path, dialog_argv);
        _exit(127);
    }

    int status;
    if (waitpid(dlg, &status, 0) < 0)
    {
        redeliver(sig, 0);
    }

    if (WIFEXITED(status))
    {
        int code = WEXITSTATUS(status);
        if (code == 1)
        {
            /* 取消: dialog already closed, launch the debugger. */
            launch_debugger(sig);
        }
        /* 0 (确定), 125 (helper failure), or any other exit: terminate. */
    }
    /* Abnormal child exit (signal / no usable dialog): terminate. */
    redeliver(sig, 0);
}

/* Resolve the executable basename once, before any crash can occur. */
static void resolve_program(void)
{
    ssize_t n = readlink("/proc/self/exe", g_program, sizeof(g_program) - 1);
    if (n > 0 && (size_t)n < sizeof(g_program))
    {
        g_program[n] = '\0';
        const char *base = strrchr(g_program, '/');
        if (base != NULL)
        {
            memmove(g_program, base + 1, strlen(base + 1) + 1);
        }
    }
    else if (program_invocation_short_name != NULL && *program_invocation_short_name != '\0')
    {
        strncpy(g_program, program_invocation_short_name, sizeof(g_program) - 1);
        g_program[sizeof(g_program) - 1] = '\0';
    }
    else
    {
        strcpy(g_program, "unknown");
    }
}

/* Resolve the absolute sibling helper path from the loaded library location,
 * so the runtime behavior never depends on the current working directory. */
static void resolve_helper_path(void)
{
    Dl_info info;
    if (dladdr(g_helper_path, &info) != 0 && info.dli_fname != NULL)
    {
        const char *fname = info.dli_fname;
        const char *slash = strrchr(fname, '/');
        if (slash != NULL)
        {
            size_t dirlen = (size_t)(slash - fname);
            if (dirlen + sizeof("schrodinger-dialog") <= sizeof(g_helper_path))
            {
                memcpy(g_helper_path, fname, dirlen);
                memcpy(g_helper_path + dirlen, "/schrodinger-dialog", sizeof("schrodinger-dialog"));
                return;
            }
        }
    }
    g_helper_path[0] = '\0';
}

/* Locate a binary by name on PATH (constructor-time; allocation is safe). */
static void find_executable(const char *name, char *out, size_t outlen)
{
    out[0] = '\0';
    const char *path = getenv("PATH");
    if (path == NULL || *path == '\0')
    {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    const char *p = path;
    while (*p != '\0')
    {
        const char *colon = strchr(p, ':');
        size_t dirlen = colon != NULL ? (size_t)(colon - p) : strlen(p);
        if (dirlen > 0)
        {
            char candidate[PATH_MAX];
            if (dirlen + 1 + strlen(name) + 1 <= sizeof(candidate))
            {
                memcpy(candidate, p, dirlen);
                candidate[dirlen] = '/';
                memcpy(candidate + dirlen + 1, name, strlen(name) + 1);
                if (access(candidate, X_OK) == 0)
                {
                    strncpy(out, candidate, outlen - 1);
                    out[outlen - 1] = '\0';
                    return;
                }
            }
        }
        if (colon == NULL)
        {
            break;
        }
        p = colon + 1;
    }
}

static void resolve_tools(void)
{
    find_executable("kitty", g_kitty, sizeof(g_kitty));
    find_executable("konsole", g_konsole, sizeof(g_konsole));
    find_executable("pwndbg", g_pwndbg, sizeof(g_pwndbg));
    find_executable("gdb", g_gdb, sizeof(g_gdb));
}

/* Snapshot environ without LD_PRELOAD, at load time when allocation and
 * strncmp are safe, so the crash child can execve without touching environ. */
static void build_clean_envp(void)
{
    size_t n = 0;
    for (char **e = environ; *e != NULL; ++e)
    {
        ++n;
    }
    char **envp = (char **)malloc((n + 1) * sizeof(char *));
    if (envp == NULL)
    {
        return;
    }
    size_t j = 0;
    for (char **e = environ; *e != NULL; ++e)
    {
        if (strncmp(*e, "LD_PRELOAD=", 11) != 0)
        {
            envp[j++] = *e;
        }
    }
    envp[j] = NULL;
    g_clean_envp = envp;
}

__attribute__((constructor)) static void schrodinger_init(void)
{
    resolve_program();
    resolve_helper_path();
    resolve_tools();
    build_clean_envp();

    stack_t ss;
    ss.ss_sp = g_alt_stack;
    ss.ss_size = sizeof(g_alt_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    for (size_t i = 0; i < NSIG_HANDLED; ++i)
    {
        sigaction(g_signals[i], &sa, &g_old[i]);
    }
}
