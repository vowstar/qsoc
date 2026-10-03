// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/runtime/qsocagentpeer.h"

#include <QLocalSocket>

#if defined(Q_OS_LINUX)
#include <sys/socket.h>
#include <unistd.h>
#elif defined(Q_OS_MACOS)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace QSocAgentPeer {

bool sameUser(const QLocalSocket &socket)
{
#if defined(Q_OS_LINUX)
    struct ucred cred = {};
    socklen_t    size = sizeof(cred);
    const int    fd   = static_cast<int>(socket.socketDescriptor());
    return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) == 0 && cred.uid == ::geteuid();
#elif defined(Q_OS_MACOS)
    uid_t     uid = 0;
    gid_t     gid = 0;
    const int fd  = static_cast<int>(socket.socketDescriptor());
    return ::getpeereid(fd, &uid, &gid) == 0 && uid == ::geteuid();
#else
    Q_UNUSED(socket);
    return true;
#endif
}

qint64 processId(const QLocalSocket &socket)
{
#if defined(Q_OS_LINUX)
    struct ucred cred = {};
    socklen_t    size = sizeof(cred);
    const int    fd   = static_cast<int>(socket.socketDescriptor());
    return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) == 0 ? cred.pid : -1;
#elif defined(Q_OS_MACOS)
    pid_t     pid  = 0;
    socklen_t size = sizeof(pid);
    const int fd   = static_cast<int>(socket.socketDescriptor());
    return ::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &size) == 0 ? pid : -1;
#elif defined(Q_OS_WIN)
    ULONG pid = 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto pipe = reinterpret_cast<HANDLE>(socket.socketDescriptor());
    return ::GetNamedPipeServerProcessId(pipe, &pid) ? static_cast<qint64>(pid) : -1;
#else
    Q_UNUSED(socket);
    return -1;
#endif
}

} // namespace QSocAgentPeer
