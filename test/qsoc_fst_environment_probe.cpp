// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocipc.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const auto       arguments = application.arguments();
    if (arguments.size() != 5 || arguments[1] != "--socket" || arguments[3] != "--owner-pid")
        return 14;
    for (const char *name :
         {"QSOC_FST_ENV_CREDENTIAL_PROBE",
          "OPENAI_API_KEY",
          "SSH_AUTH_SOCK",
          "LD_PRELOAD",
          "DYLD_INSERT_LIBRARIES"}) {
        if (qEnvironmentVariableIsSet(name))
            return 21;
    }
    const auto directory = QFileInfo(QDir::currentPath()).canonicalFilePath();
    for (const char *name : {"HOME", "USERPROFILE", "TMPDIR", "TEMP", "TMP"}) {
        if (directory.isEmpty()
            || QFileInfo(qEnvironmentVariable(name)).canonicalFilePath() != directory)
            return 22;
    }
    QLocalSocket socket;
    socket.connectToServer(arguments[2]);
    if (!socket.waitForConnected(2000))
        return 17;
    socket.write(QSocIpc::frame(QJsonObject{{"type", "hello"}, {"protocol", 2}}));
    socket.waitForBytesWritten(2000);
    QByteArray    input;
    QElapsedTimer elapsed;
    elapsed.start();
    QJsonObject request;
    while (elapsed.elapsed() < 2000) {
        input += socket.read(1024);
        const auto decoded = QSocIpc::decode(input, request, 1024);
        if (decoded == QSocIpc::DecodeResult::Complete)
            break;
        if (decoded == QSocIpc::DecodeResult::Invalid)
            return 14;
        socket.waitForReadyRead(20);
    }
    if (request.value("type") != "open")
        return 14;
    socket.write(QSocIpc::frame(QJsonObject{{"type", "complete"}}));
    elapsed.restart();
    while (socket.bytesToWrite() > 0 && elapsed.elapsed() < 2000)
        socket.waitForBytesWritten(20);
    return socket.bytesToWrite() == 0 ? 0 : 16;
}
