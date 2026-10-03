// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprocessowner.h"

#include <cstdlib>
#include <limits>
#include <thread>

#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#elif defined(Q_OS_MACOS)
#include <cerrno>
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

struct QSocProcessOwner::State
{
#ifdef Q_OS_LINUX
    bool armed = false;
#elif defined(Q_OS_WIN)
    HANDLE owner = nullptr;
    HANDLE stop  = nullptr;
#elif defined(Q_OS_MACOS)
    int queue = -1;
#endif
    std::thread waiter;

    ~State()
    {
#ifdef Q_OS_LINUX
        if (armed)
            ::prctl(PR_SET_PDEATHSIG, 0);
#elif defined(Q_OS_WIN)
        if (stop != nullptr)
            ::SetEvent(stop);
#elif defined(Q_OS_MACOS)
        if (queue >= 0) {
            struct kevent event;
            EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            ::kevent(queue, &event, 1, nullptr, 0, nullptr);
        }
#endif
        if (waiter.joinable())
            waiter.join();
#ifdef Q_OS_WIN
        if (owner != nullptr)
            ::CloseHandle(owner);
        if (stop != nullptr)
            ::CloseHandle(stop);
#elif defined(Q_OS_MACOS)
        if (queue >= 0)
            ::close(queue);
#endif
    }
};

QSocProcessOwner::QSocProcessOwner()  = default;
QSocProcessOwner::~QSocProcessOwner() = default;

bool QSocProcessOwner::watch(qint64 parentPid)
{
    if (state_ || parentPid <= 1)
        return false;
#ifdef Q_OS_WIN
    if (parentPid > (std::numeric_limits<DWORD>::max)())
        return false;
#else
    if (parentPid > std::numeric_limits<int>::max())
        return false;
#endif
    auto state = std::make_unique<State>();
#ifdef Q_OS_LINUX
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
        return false;
    state->armed = true;
    if (::getppid() != parentPid)
        return false;
#elif defined(Q_OS_WIN)
    state->owner = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parentPid));
    state->stop  = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state->owner == nullptr || state->stop == nullptr
        || ::WaitForSingleObject(state->owner, 0) != WAIT_TIMEOUT)
        return false;
    const HANDLE owner = state->owner;
    const HANDLE stop  = state->stop;
    state->waiter      = std::thread([owner, stop] {
        const HANDLE handles[] = {stop, owner};
        const DWORD  result    = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0)
            std::_Exit(1);
    });
#elif defined(Q_OS_MACOS)
    if (::getppid() != parentPid)
        return false;
    state->queue = ::kqueue();
    if (state->queue < 0 || ::fcntl(state->queue, F_SETFD, FD_CLOEXEC) < 0)
        return false;
    struct kevent events[2];
    EV_SET(
        &events[0],
        static_cast<uintptr_t>(parentPid),
        EVFILT_PROC,
        EV_ADD | EV_ENABLE | EV_ONESHOT,
        NOTE_EXIT,
        0,
        nullptr);
    EV_SET(&events[1], 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(state->queue, events, 2, nullptr, 0, nullptr) < 0 || ::getppid() != parentPid)
        return false;
    const int queue = state->queue;
    state->waiter   = std::thread([queue] {
        struct kevent event;
        int           count;
        do {
            count = ::kevent(queue, nullptr, 0, &event, 1, nullptr);
        } while (count < 0 && errno == EINTR);
        if (count != 1 || event.filter != EVFILT_USER)
            std::_Exit(1);
    });
#else
    return false;
#endif
    state_ = std::move(state);
    return true;
}
