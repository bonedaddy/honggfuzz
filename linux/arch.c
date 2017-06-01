/*
 *
 * honggfuzz - architecture dependent code (LINUX)
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2015 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "../libcommon/common.h"
#include "../libcommon/arch.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <setjmp.h>
#include <sys/cdefs.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include "../libcommon/files.h"
#include "../libcommon/log.h"
#include "../libcommon/sancov.h"
#include "../libcommon/util.h"
#include "../subproc.h"
#include "perf.h"
#include "ptrace_utils.h"

/* Size of remote pid cmdline char buffer */
#define _HF_PROC_CMDLINE_SZ 8192

static bool arch_ifaceUp(const char *ifacename)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock == -1) {
        PLOG_E("socket(AF_INET, SOCK_STREAM, IPPROTO_IP)");
        return false;
    }
    defer {
        close(sock);
    };

    struct ifreq ifr;
    memset(&ifr, '\0', sizeof(ifr));
    snprintf(ifr.ifr_name, IF_NAMESIZE, "%s", ifacename);

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == -1) {
        PLOG_E("ioctl(iface='%s', SIOCGIFFLAGS, IFF_UP)", ifacename);
        return false;
    }

    ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);

    if (ioctl(sock, SIOCSIFFLAGS, &ifr) == -1) {
        PLOG_E("ioctl(iface='%s', SIOCSIFFLAGS, IFF_UP|IFF_RUNNING)", ifacename);
        return false;
    }

    return true;
}

static inline bool arch_shouldAttach(honggfuzz_t * hfuzz, fuzzer_t * fuzzer)
{
    if (hfuzz->persistent && fuzzer->linux.attachedPid == fuzzer->pid) {
        return false;
    }
    if (hfuzz->linux.pid > 0 && fuzzer->linux.attachedPid == hfuzz->linux.pid) {
        return false;
    }
    return true;
}

pid_t arch_fork(honggfuzz_t * hfuzz, fuzzer_t * fuzzer)
{
    arch_perfClose(hfuzz, fuzzer);

    if (hfuzz->linux.cloneFlags && unshare(hfuzz->linux.cloneFlags) == -1) {
        LOG_E("unshare(%tx)", hfuzz->linux.cloneFlags);
    }
    pid_t pid = fork();
    if (pid == -1) {
        return pid;
    }
    if (pid == 0) {
        logMutexReset();
        if (prctl
            (PR_SET_PDEATHSIG, (unsigned long)SIGKILL, (unsigned long)0, (unsigned long)0,
             (unsigned long)0) == -1) {
            PLOG_W("prctl(PR_SET_PDEATHSIG, SIGKILL)");
        }
        if (hfuzz->linux.cloneFlags & CLONE_NEWNET) {
            if (arch_ifaceUp("lo") == false) {
                LOG_W("Cannot bring interface 'lo' up");
            }
        }
        return pid;
    }

    /* Parent */
    if (hfuzz->persistent) {
        const struct f_owner_ex fown = {
            .type = F_OWNER_TID,
            .pid = syscall(__NR_gettid),
        };
        if (fcntl(fuzzer->persistentSock, F_SETOWN_EX, &fown)) {
            PLOG_F("fcntl(%d, F_SETOWN_EX)", fuzzer->persistentSock);
        }
        if (fcntl(fuzzer->persistentSock, F_SETSIG, SIGIO) == -1) {
            PLOG_F("fcntl(%d, F_SETSIG, SIGIO)", fuzzer->persistentSock);
        }
        if (fcntl(fuzzer->persistentSock, F_SETFL, O_ASYNC) == -1) {
            PLOG_F("fcntl(%d, F_SETFL, O_ASYNC)", fuzzer->persistentSock);
        }
        int sndbuf = (1024 * 1024 * 2); /* 2MiB */
        if (setsockopt(fuzzer->persistentSock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) ==
            -1) {
            LOG_W("Couldn't set FD send buffer to '%d' bytes", sndbuf);
        }
    }

    pid_t perf_pid = (hfuzz->linux.pid == 0) ? pid : hfuzz->linux.pid;
    if (arch_perfOpen(perf_pid, hfuzz, fuzzer) == false) {
        return -1;
    }

    return pid;
}

bool arch_launchChild(honggfuzz_t * hfuzz, char *fileName)
{
    /*
     * Make it attach-able by ptrace()
     */
    if (prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL) == -1) {
        PLOG_E("prctl(PR_SET_DUMPABLE, 1)");
        return false;
    }

    /*
     * Kill a process which corrupts its own heap (with ABRT)
     */
    if (setenv("MALLOC_CHECK_", "7", 0) == -1) {
        PLOG_E("setenv(MALLOC_CHECK_=7) failed");
        return false;
    }

    /*
     * Disable ASLR:
     * This might fail in Docker, as Docker blocks __NR_personality. Consequently
     * it's just a debug warning
     */
    if (hfuzz->linux.disableRandomization && syscall(__NR_personality, ADDR_NO_RANDOMIZE) == -1) {
        PLOG_D("personality(ADDR_NO_RANDOMIZE) failed");
    }
#define ARGS_MAX 512
    char *args[ARGS_MAX + 2];
    char argData[PATH_MAX] = { 0 };
    int x = 0;

    for (x = 0; x < ARGS_MAX && hfuzz->cmdline[x]; x++) {
        if (!hfuzz->fuzzStdin && !hfuzz->persistent
            && strcmp(hfuzz->cmdline[x], _HF_FILE_PLACEHOLDER) == 0) {
            args[x] = (char *)fileName;
        } else if (!hfuzz->fuzzStdin && !hfuzz->persistent
                   && strstr(hfuzz->cmdline[x], _HF_FILE_PLACEHOLDER)) {
            const char *off = strstr(hfuzz->cmdline[x], _HF_FILE_PLACEHOLDER);
            snprintf(argData, PATH_MAX, "%.*s%s", (int)(off - hfuzz->cmdline[x]),
                     hfuzz->cmdline[x], fileName);
            args[x] = argData;
        } else {
            args[x] = hfuzz->cmdline[x];
        }
    }

    args[x++] = NULL;

    LOG_D("Launching '%s' on file '%s'", args[0], hfuzz->persistent ? "PERSISTENT_MODE" : fileName);

    /* alarm persists across forks, so disable it here */
    alarm(0);

    /*
     * Wait for the ptrace to attach
     */
    if (kill(syscall(__NR_getpid), SIGSTOP) == -1) {
        LOG_F("Couldn't stop itself");
    }
#if defined(__NR_execveat)
    syscall(__NR_execveat, hfuzz->linux.exeFd, "", args, environ, AT_EMPTY_PATH);
#endif                          /* defined__NR_execveat) */
    execve(args[0], args, environ);
    int errno_cpy = errno;
    alarm(1);

    LOG_E("execve('%s', fd=%d): %s", args[0], hfuzz->linux.exeFd, strerror(errno_cpy));

    return false;
}

void arch_prepareChild(honggfuzz_t * hfuzz, fuzzer_t * fuzzer)
{
    pid_t ptracePid = (hfuzz->linux.pid > 0) ? hfuzz->linux.pid : fuzzer->pid;
    pid_t childPid = fuzzer->pid;

    if (arch_shouldAttach(hfuzz, fuzzer) == true) {
        if (arch_ptraceAttach(hfuzz, ptracePid) == false) {
            LOG_E("arch_ptraceAttach(pid=%d) failed", ptracePid);
        }
        fuzzer->linux.attachedPid = ptracePid;
    }

    /* A long-lived process could have already exited, and we wouldn't know */
    if (childPid != ptracePid && kill(ptracePid, 0) == -1) {
        if (hfuzz->linux.pidFile) {
            /* If pid from file, check again for cases of auto-restart daemons that update it */
            /*
             * TODO: Investigate if we need to delay here, so that target process has
             * enough time to restart. Tricky to answer since is target dependent.
             */
            if (files_readPidFromFile(hfuzz->linux.pidFile, &hfuzz->linux.pid) == false) {
                LOG_F("Failed to read new PID from file - abort");
            } else {
                if (kill(hfuzz->linux.pid, 0) == -1) {
                    PLOG_F("Liveness of PID %d read from file questioned - abort",
                           hfuzz->linux.pid);
                } else {
                    LOG_D("Monitor PID has been updated (pid=%d)", hfuzz->linux.pid);
                    ptracePid = hfuzz->linux.pid;
                }
            }
        }
    }

    if (arch_perfEnable(hfuzz, fuzzer) == false) {
        LOG_E("Couldn't enable perf counters for pid %d", ptracePid);
    }
    if (childPid != ptracePid) {
        if (arch_ptraceWaitForPidStop(childPid) == false) {
            LOG_F("PID: %d not in a stopped state", childPid);
        }
        if (kill(childPid, SIGCONT) == -1) {
            PLOG_F("Restarting PID: %d failed", childPid);
        }
    }
}

static bool arch_checkWait(honggfuzz_t * hfuzz, fuzzer_t * fuzzer)
{
    pid_t ptracePid = (hfuzz->linux.pid > 0) ? hfuzz->linux.pid : fuzzer->pid;
    pid_t childPid = fuzzer->pid;

    /* All queued wait events must be tested */
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, __WALL | __WNOTHREAD | WNOHANG);
        if (pid == 0) {
            return false;
        }
        if (pid == -1 && errno == EINTR) {
            continue;
        }
        if (pid == -1 && errno == ECHILD) {
            LOG_D("No more processes to track");
            return true;
        }
        if (pid == -1) {
            PLOG_F("wait4() failed");
        }

        char statusStr[4096];
        LOG_D("PID '%d' returned with status: %s", pid,
              subproc_StatusToStr(status, statusStr, sizeof(statusStr)));

        if (hfuzz->persistent && pid == fuzzer->persistentPid
            && (WIFEXITED(status) || WIFSIGNALED(status))) {
            arch_ptraceAnalyze(hfuzz, status, pid, fuzzer);
            fuzzer->persistentPid = 0;
            if (ATOMIC_GET(hfuzz->terminating) == false) {
                LOG_W("Persistent mode: PID %d exited with status: %s", pid,
                      subproc_StatusToStr(status, statusStr, sizeof(statusStr)));
            }
            return true;
        }
        if (ptracePid == childPid) {
            arch_ptraceAnalyze(hfuzz, status, pid, fuzzer);
            continue;
        }
        if (pid == childPid && (WIFEXITED(status) || WIFSIGNALED(status))) {
            return true;
        }
        if (pid == childPid) {
            continue;
        }

        arch_ptraceAnalyze(hfuzz, status, pid, fuzzer);
    }
}

__thread sigset_t sset_io_chld;

void arch_reapChild(honggfuzz_t * hfuzz, fuzzer_t * fuzzer)
{
    static const struct timespec ts = {
        .tv_sec = 0L,
        .tv_nsec = 250000000L,
    };
    for (;;) {
        int sig = syscall(__NR_rt_sigtimedwait, &sset_io_chld, NULL, &ts, _NSIG / 8);
        if (sig == -1 && (errno != EAGAIN && errno != EINTR)) {
            PLOG_F("sigtimedwait(SIGIO|SIGCHLD, 0.25s)");
        }
        if (sig == -1) {
            subproc_checkTimeLimit(hfuzz, fuzzer);
            subproc_checkTermination(hfuzz, fuzzer);
        }
        if (subproc_persistentModeRoundDone(hfuzz, fuzzer)) {
            break;
        }
        if (arch_checkWait(hfuzz, fuzzer)) {
            break;
        }
    }

    if (hfuzz->enableSanitizers) {
        pid_t ptracePid = (hfuzz->linux.pid > 0) ? hfuzz->linux.pid : fuzzer->pid;
        char crashReport[PATH_MAX];
        snprintf(crashReport, sizeof(crashReport), "%s/%s.%d", hfuzz->workDir, kLOGPREFIX,
                 ptracePid);
        if (files_exists(crashReport)) {
            if (fuzzer->backtrace) {
                unlink(crashReport);
            } else {
                LOG_W
                    ("Un-handled ASan report due to compiler-rt internal error - retry with '%s' (%s)",
                     crashReport, fuzzer->fileName);

                /* Try to parse report file */
                arch_ptraceExitAnalyze(hfuzz, ptracePid, fuzzer);
            }
        }
    }

    arch_perfAnalyze(hfuzz, fuzzer);
    sancov_Analyze(hfuzz, fuzzer);
}

bool arch_archInit(honggfuzz_t * hfuzz)
{
    /* Make %'d work */
    setlocale(LC_NUMERIC, "");

    if (access(hfuzz->cmdline[0], X_OK) == -1) {
        PLOG_E("File '%s' doesn't seem to be executable", hfuzz->cmdline[0]);
        return false;
    }
    if ((hfuzz->linux.exeFd = open(hfuzz->cmdline[0], O_RDONLY | O_CLOEXEC)) == -1) {
        PLOG_E("Cannot open the executable binary: %s)", hfuzz->cmdline[0]);
        return false;
    }

    const char *(*gvs) (void) = dlsym(RTLD_DEFAULT, "gnu_get_libc_version");
    for (;;) {
        if (!gvs) {
            break;
        }
        const char *gversion = gvs();
        int major, minor;
        if (sscanf(gversion, "%d.%d", &major, &minor) != 2) {
            LOG_W("Unknown glibc version: '%s'", gversion);
            break;
        }
        if ((major < 2) || (major == 2 && minor < 24)) {
            LOG_E("Your glibc version:'%s' will most likely result in malloc()-related "
                  "deadlocks. Min. version 2.24 suggested. See "
                  "https://bugzilla.redhat.com/show_bug.cgi?id=906468 for explanation", gversion);
            break;
        }
        LOG_D("Glibc version:'%s', OK", gversion);
        break;
    }

    if (hfuzz->dynFileMethod != _HF_DYNFILE_NONE) {
        unsigned long major = 0, minor = 0;
        char *p = NULL;

        /*
         * Check that Linux kernel is compatible
         *
         * Compatibility list:
         *  1) Perf exclude_callchain_kernel requires kernel >= 3.7
         *     TODO: Runtime logic to disable it for unsupported kernels
         *           if it doesn't affect perf counters processing
         *  2) If 'PERF_TYPE_HARDWARE' is not supported by kernel, ENOENT
         *     is returned from perf_event_open(). Unfortunately, no reliable
         *     way to detect it here. libperf exports some list functions,
         *     although small guarantees it's installed. Maybe a more targeted
         *     message at perf_event_open() error handling will help.
         *  3) Intel's PT and new Intel BTS format require kernel >= 4.1
         */
        unsigned long checkMajor = 3, checkMinor = 7;
        if ((hfuzz->dynFileMethod & _HF_DYNFILE_BTS_BLOCK) ||
            (hfuzz->dynFileMethod & _HF_DYNFILE_BTS_EDGE) ||
            (hfuzz->dynFileMethod & _HF_DYNFILE_IPT_BLOCK)) {
            checkMajor = 4;
            checkMinor = 1;
        }

        struct utsname uts;
        if (uname(&uts) == -1) {
            PLOG_F("uname() failed");
            return false;
        }

        p = uts.release;
        major = strtoul(p, &p, 10);
        if (*p++ != '.') {
            LOG_F("Unsupported kernel version (%s)", uts.release);
            return false;
        }

        minor = strtoul(p, &p, 10);
        if ((major < checkMajor) || ((major == checkMajor) && (minor < checkMinor))) {
            LOG_E("Kernel version '%s' not supporting chosen perf method", uts.release);
            return false;
        }

        if (arch_perfInit(hfuzz) == false) {
            return false;
        }
    }
#if defined(__ANDROID__) && defined(__arm__)
    /*
     * For ARM kernels running Android API <= 21, if fuzzing target links to
     * libcrypto (OpenSSL), OPENSSL_cpuid_setup initialization is triggering a
     * SIGILL/ILLOPC at armv7_tick() due to  "mrrc p15, #1, r0, r1, c14)" instruction.
     * Setups using BoringSSL (API >= 22) are not affected.
     */
    if (setenv("OPENSSL_armcap", OPENSSL_ARMCAP_ABI, 1) == -1) {
        PLOG_E("setenv(OPENSSL_armcap) failed");
        return false;
    }
#endif

    /* If read PID from file enable - read current value */
    if (hfuzz->linux.pidFile) {
        if (files_readPidFromFile(hfuzz->linux.pidFile, &hfuzz->linux.pid) == false) {
            LOG_E("Failed to read PID from file");
            return false;
        }
    }

    /* If remote pid, resolve command using procfs */
    if (hfuzz->linux.pid > 0) {
        char procCmd[PATH_MAX] = { 0 };
        snprintf(procCmd, sizeof(procCmd), "/proc/%d/cmdline", hfuzz->linux.pid);

        hfuzz->linux.pidCmd = malloc(_HF_PROC_CMDLINE_SZ * sizeof(char));
        if (!hfuzz->linux.pidCmd) {
            PLOG_E("malloc(%zu) failed", (size_t) _HF_PROC_CMDLINE_SZ);
            return false;
        }

        ssize_t sz = files_readFileToBufMax(procCmd, (uint8_t *) hfuzz->linux.pidCmd,
                                            _HF_PROC_CMDLINE_SZ - 1);
        if (sz < 1) {
            LOG_E("Couldn't read '%s'", procCmd);
            free(hfuzz->linux.pidCmd);
            return false;
        }

        /* Make human readable */
        for (size_t i = 0; i < ((size_t) sz - 1); i++) {
            if (hfuzz->linux.pidCmd[i] == '\0') {
                hfuzz->linux.pidCmd[i] = ' ';
            }
        }
        hfuzz->linux.pidCmd[sz] = '\0';
    }

    /* Updates the important signal array based on input args */
    arch_ptraceSignalsInit(hfuzz);

    /*
     * If sanitizer fuzzing enabled and SIGABRT is monitored (abort_on_error=1),
     * increase number of major frames, since top 7-9 frames will be occupied
     * with sanitizer runtime library & libc symbols
     */
    if (hfuzz->enableSanitizers && hfuzz->monitorSIGABRT) {
        hfuzz->linux.numMajorFrames = 14;
    }

    return true;
}

bool arch_archThreadInit(honggfuzz_t * hfuzz UNUSED, fuzzer_t * fuzzer UNUSED)
{
    fuzzer->linux.perfMmapBuf = NULL;
    fuzzer->linux.perfMmapAux = NULL;
    fuzzer->linux.cpuInstrFd = -1;
    fuzzer->linux.cpuBranchFd = -1;
    fuzzer->linux.cpuIptBtsFd = -1;

    sigemptyset(&sset_io_chld);
    sigaddset(&sset_io_chld, SIGIO);
    sigaddset(&sset_io_chld, SIGCHLD);

    return true;
}
