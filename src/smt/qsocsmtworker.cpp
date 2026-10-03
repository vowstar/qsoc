// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocsmtworker.h"
#include "common/qsocipc.h"
#include "common/qsoclocalpeer.h"
#include "common/qsocprocessowner.h"
#include "qsocsmtservice.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalSocket>

namespace {
constexpr int wireLimit = 2 * 1024 * 1024;

int runSocket(const QString &endpoint, qint64 parentPid, const QSocSmtWorker::Executor &execute)
{
#ifndef Q_OS_WIN
    QSocProcessOwner owner;
    if (!owner.watch(parentPid))
        return 13;
#endif
    QLocalSocket socket;
    socket.setReadBufferSize(wireLimit + QSocIpc::headerBytes);
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(2000) || !QSocLocalPeer::sameUser(socket))
        return 17;
    const qint64 peerPid = QSocLocalPeer::processId(socket);
    if (peerPid > 0 && peerPid != parentPid)
        return 17;
    socket.write(
        QSocIpc::frame(
            QJsonObject{
                {"service", "qsoc-smt-worker"},
                {"protocol", 1},
                {"pid", QCoreApplication::applicationPid()}}));
    QByteArray    input;
    QJsonObject   request;
    QElapsedTimer elapsed;
    elapsed.start();
    while (true) {
        input += socket.read(wireLimit + QSocIpc::headerBytes - input.size());
        const auto decoded = QSocIpc::decode(input, request, wireLimit);
        if (decoded == QSocIpc::DecodeResult::Complete)
            break;
        if (decoded == QSocIpc::DecodeResult::Invalid || elapsed.elapsed() >= 2000
            || socket.state() != QLocalSocket::ConnectedState)
            return 14;
        socket.waitForReadyRead(20);
    }
    if (!input.isEmpty() || request.value("id").toInt() != 1
        || request.value("method").toString() != "smt.solve" || !request.value("params").isObject())
        return 14;
    const auto result = execute(request.value("params").toObject());
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > QSocSmtService::outputLimit)
        return 15;
    const auto output = QSocIpc::frame(QJsonObject{{"id", 1}, {"result", result}}, wireLimit);
    if (socket.write(output) != output.size())
        return 16;
    elapsed.restart();
    while (socket.bytesToWrite() > 0) {
        if (elapsed.elapsed() >= 2000 || !socket.waitForBytesWritten(20)) {
            if (socket.state() != QLocalSocket::ConnectedState || elapsed.elapsed() >= 2000)
                return 16;
        }
    }
    socket.disconnectFromServer();
    return 0;
}

int runStdin(const QSocSmtWorker::Executor &execute)
{
    QByteArray input;
    char       buffer[4096];
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), stdin)) {
        input.append(buffer, static_cast<qsizetype>(count));
        if (input.size() > wireLimit)
            return 14;
    }
    QJsonParseError error;
    const auto      request = QJsonDocument::fromJson(input, &error);
    const auto      result  = error.error == QJsonParseError::NoError && request.isObject()
                                  ? execute(request.object())
                                  : QSocSmtService::failure("error", "Invalid worker request");
    const auto      output  = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (output.size() > QSocSmtService::outputLimit)
        return 15;
    const auto written
        = std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
    std::fflush(stdout);
    return written == static_cast<size_t>(output.size()) ? 0 : 16;
}
} // namespace

int QSocSmtWorker::run(int argc, char **argv, const Executor &execute)
{
    try {
        QCoreApplication application(argc, argv);
        const auto       arguments = application.arguments();
        if (arguments.size() == 1)
            return runStdin(execute);
        if (arguments.size() != 5 || arguments[1] != "--socket" || arguments[3] != "--owner-pid")
            return 14;
        bool       ok  = false;
        const auto pid = arguments[4].toLongLong(&ok);
        return ok ? runSocket(arguments[2], pid, execute) : 14;
    } catch (const std::bad_alloc &) {
        std::_Exit(12);
    }
}
