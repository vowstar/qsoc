// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocinterrupt.h"

#include <QtGlobal>

#include <atomic>
#include <csignal>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {
static_assert(std::atomic<unsigned int>::is_always_lock_free);
constexpr unsigned int    kEdgeMask = 0x3U;
constexpr unsigned int    kLatch    = 0x4U;
std::atomic<unsigned int> g_interruptState{0};
bool                      g_handlerReady      = false;
bool                      g_bridgeReady       = false;
bool                      g_byteFallbackReady = false;

#ifndef Q_OS_WIN
int  g_pipeReadFd                = -1;
int  g_pipeWriteFd               = -1;
bool g_unblockSigintAfterInstall = false;

bool makeNonBlockingCloexec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }
    const int fdFlags = ::fcntl(fd, F_GETFD, 0);
    return fdFlags >= 0 && ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) == 0;
}

void closeSignalPipe()
{
    if (g_pipeReadFd >= 0) {
        ::close(g_pipeReadFd);
    }
    if (g_pipeWriteFd >= 0) {
        ::close(g_pipeWriteFd);
    }
    g_pipeReadFd  = -1;
    g_pipeWriteFd = -1;
}

bool blockSigintAfterInstallFailure()
{
    sigset_t signalSet;
    sigset_t previousSet;
    if (::sigemptyset(&signalSet) != 0 || ::sigaddset(&signalSet, SIGINT) != 0
        || ::sigprocmask(SIG_BLOCK, &signalSet, &previousSet) != 0) {
        return false;
    }
    if (::sigismember(&previousSet, SIGINT) == 0) {
        g_unblockSigintAfterInstall = true;
    }
    g_byteFallbackReady = true;
    return true;
}

bool failBridgeInstall()
{
    closeSignalPipe();
    (void) blockSigintAfterInstallFailure();
    return false;
}

bool restoreSigintMaskAfterInstall()
{
    if (!g_unblockSigintAfterInstall) {
        return true;
    }
    sigset_t signalSet;
    if (::sigemptyset(&signalSet) != 0 || ::sigaddset(&signalSet, SIGINT) != 0
        || ::sigprocmask(SIG_UNBLOCK, &signalSet, nullptr) != 0) {
        return false;
    }
    g_unblockSigintAfterInstall = false;
    return true;
}
#endif

#ifdef Q_OS_WIN
BOOL WINAPI consoleCtrlHandler(DWORD event)
{
    if (event != CTRL_C_EVENT) {
        return FALSE;
    }
    QSocInterrupt::request();
    return TRUE;
}
#else
void sigintHandler(int)
{
    QSocInterrupt::request();
}
#endif
} // namespace

namespace QSocInterrupt {

void request()
{
#ifndef Q_OS_WIN
    const int savedErrno = errno;
#else
    unsigned int state = g_interruptState.load(std::memory_order_relaxed);
    while (true) {
        const unsigned int edges = state & kEdgeMask;
        const unsigned int next  = (state | kLatch) + static_cast<unsigned int>(edges < 2U);
        if (g_interruptState.compare_exchange_weak(
                state, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
            break;
        }
    }
#endif
#ifndef Q_OS_WIN
    g_interruptState.fetch_or(kLatch, std::memory_order_relaxed);
    if (g_pipeWriteFd >= 0) {
        const char edge = 1;
        ssize_t    written;
        do {
            written = ::write(g_pipeWriteFd, &edge, 1);
        } while (written < 0 && errno == EINTR);
        (void) written;
    }
    errno = savedErrno;
#endif
}

void acknowledge()
{
#ifdef Q_OS_WIN
    unsigned int state = g_interruptState.load(std::memory_order_relaxed);
    while ((state & kEdgeMask) > 0U
           && !g_interruptState.compare_exchange_weak(
               state, state - 1U, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
#endif
}

bool requested()
{
    return (g_interruptState.load(std::memory_order_relaxed) & kLatch) != 0U;
}

void clearRequest()
{
    g_interruptState.fetch_and(~kLatch, std::memory_order_relaxed);
}

bool installBridge()
{
    if (g_handlerReady) {
        return true;
    }
#ifndef Q_OS_WIN
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return failBridgeInstall();
    }
    g_pipeReadFd  = fds[0];
    g_pipeWriteFd = fds[1];
    if (!makeNonBlockingCloexec(g_pipeReadFd) || !makeNonBlockingCloexec(g_pipeWriteFd)) {
        return failBridgeInstall();
    }

    struct sigaction action = {};
    action.sa_handler       = sigintHandler;
    if (::sigemptyset(&action.sa_mask) != 0) {
        return failBridgeInstall();
    }
    action.sa_flags = 0;
    if (::sigaction(SIGINT, &action, nullptr) != 0) {
        return failBridgeInstall();
    }
    if (!restoreSigintMaskAfterInstall()) {
        return failBridgeInstall();
    }
    g_handlerReady      = true;
    g_bridgeReady       = true;
    g_byteFallbackReady = false;
    return true;
#else
    if (!::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE)) {
        return false;
    }
    g_handlerReady = true;
    return true;
#endif
}

bool bridgeReady()
{
    return g_bridgeReady;
}

bool handlerReady()
{
    return g_handlerReady;
}

bool byteFallbackReady()
{
    return g_byteFallbackReady;
}

int signalReadFd()
{
#ifndef Q_OS_WIN
    return g_bridgeReady ? g_pipeReadFd : -1;
#else
    return -1;
#endif
}

int drainSignalPipe()
{
#ifndef Q_OS_WIN
    if (!g_bridgeReady) {
        return 0;
    }
    char buf[64];
    int  edges = 0;
    while (true) {
        const ssize_t got = ::read(g_pipeReadFd, buf, sizeof(buf));
        if (got > 0) {
            edges = qMin(2, edges + static_cast<int>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        if (got == 0) {
            return -1;
        }
        return edges;
    }
#else
    const unsigned int state = g_interruptState.fetch_and(~kEdgeMask, std::memory_order_relaxed);
    return static_cast<int>(state & kEdgeMask);
#endif
}

bool finishForegroundHandoff(bool *interrupted)
{
    const bool pending = requested();
    const int  result  = drainSignalPipe();
    clearRequest();
    if (interrupted != nullptr) {
        *interrupted = pending || result > 0;
    }
    return result >= 0;
}

TerminalInterruptGuard::TerminalInterruptGuard()
{
    if (!g_handlerReady) {
        return;
    }
#ifndef Q_OS_WIN
    if (::isatty(STDIN_FILENO) != 1) {
        ready_ = true;
        return;
    }
    struct termios terminal{};
    if (::tcgetattr(STDIN_FILENO, &terminal) != 0) {
        return;
    }
    originalIntr_ = terminal.c_cc[VINTR];
    originalIsig_ = (terminal.c_lflag & ISIG) != 0;
    if (originalIntr_ == 0x03 && originalIsig_) {
        ready_ = true;
        return;
    }
    terminal.c_cc[VINTR] = 0x03;
    terminal.c_lflag |= ISIG;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &terminal) != 0) {
        return;
    }
    changed_ = true;
#endif
    ready_ = true;
}

TerminalInterruptGuard::~TerminalInterruptGuard()
{
    (void) restore();
}

bool TerminalInterruptGuard::restore()
{
#ifndef Q_OS_WIN
    if (!changed_) {
        return ready_;
    }
    struct termios terminal{};
    if (::tcgetattr(STDIN_FILENO, &terminal) != 0) {
        return false;
    }
    terminal.c_cc[VINTR] = originalIntr_;
    if (originalIsig_) {
        terminal.c_lflag |= ISIG;
    } else {
        terminal.c_lflag &= ~static_cast<tcflag_t>(ISIG);
    }
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &terminal) != 0) {
        return false;
    }
    changed_ = false;
#endif
    return ready_;
}

} // namespace QSocInterrupt
