// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoclocalpeer.h"

#include <QLocalSocket>
#include <QScopeGuard>

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

#ifdef Q_OS_WIN
namespace {

QByteArray userSid(HANDLE process)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &token))
        return {};
    const auto closeToken = qScopeGuard([token] { ::CloseHandle(token); });
    DWORD      bytes      = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0)
        return {};
    QByteArray storage(static_cast<qsizetype>(bytes), Qt::Uninitialized);
    if (!::GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes))
        return {};
    const auto *user = reinterpret_cast<const TOKEN_USER *>(storage.constData());
    if (!::IsValidSid(user->User.Sid))
        return {};
    return QByteArray(static_cast<const char *>(user->User.Sid), ::GetLengthSid(user->User.Sid));
}

} // namespace
#endif

namespace QSocLocalPeer {

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
#elif defined(Q_OS_WIN)
    const qint64 pid = processId(socket);
    if (pid <= 0)
        return false;
    const HANDLE peer
        = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (peer == nullptr)
        return false;
    const auto closePeer  = qScopeGuard([peer] { ::CloseHandle(peer); });
    const auto currentSid = userSid(::GetCurrentProcess());
    const auto peerSid    = userSid(peer);
    return !currentSid.isEmpty() && currentSid == peerSid;
#else
    Q_UNUSED(socket);
    return false;
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
    const auto pipe  = reinterpret_cast<HANDLE>(socket.socketDescriptor());
    DWORD      flags = 0;
    if (!::GetNamedPipeInfo(pipe, &flags, nullptr, nullptr, nullptr))
        return -1;
    const bool found = (flags & PIPE_SERVER_END) != 0 ? ::GetNamedPipeClientProcessId(pipe, &pid)
                                                      : ::GetNamedPipeServerProcessId(pipe, &pid);
    return found ? static_cast<qint64>(pid) : -1;
#else
    Q_UNUSED(socket);
    return -1;
#endif
}

} // namespace QSocLocalPeer
