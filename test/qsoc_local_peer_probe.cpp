// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoclocalpeer.h"

#include <cstdio>
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScopeGuard>

#ifdef Q_OS_WIN
#include <windows.h>

namespace {
bool lowerIntegrity()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &token))
        return false;
    const auto               closeToken = qScopeGuard([token] { ::CloseHandle(token); });
    SID_IDENTIFIER_AUTHORITY authority  = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID                     sid        = nullptr;
    if (!::AllocateAndInitializeSid(
            &authority, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &sid))
        return false;
    const auto            freeSid = qScopeGuard([sid] { ::FreeSid(sid); });
    TOKEN_MANDATORY_LABEL label{{sid, SE_GROUP_INTEGRITY}};
    return ::SetTokenInformation(
        token, TokenIntegrityLevel, &label, sizeof(label) + ::GetLengthSid(sid));
}
} // namespace
#endif

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const auto       arguments = app.arguments();
    if (arguments.size() != 3)
        return 2;
#ifdef Q_OS_WIN
    if (arguments.at(2) == "low" && !lowerIntegrity())
        return 3;
#endif
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(arguments.at(1)))
        return 4;
    std::puts("ready");
    std::fflush(stdout);
    if (!server.waitForNewConnection(10000))
        return 5;
    auto *socket = server.nextPendingConnection();
    if (!socket)
        return 6;
    socket->write(QSocLocalPeer::sameUser(*socket) ? "accepted" : "rejected");
    socket->waitForBytesWritten(5000);
    if (socket->bytesAvailable() == 0 && !socket->waitForReadyRead(5000))
        return 7;
    if (socket->readAll() != "done")
        return 8;
    socket->disconnectFromServer();
    return 0;
}
