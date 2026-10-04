// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/client/qsocdaemonconnection.h"
#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include "common/qsocinterrupt.h"
#include "smt/qsocsmtservice.h"
#include <cstdio>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTimer>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

bool QSocCliWorker::parseSmt(const QStringList &appArguments)
{
    parser.clearPositionalArguments();
    parser.addPositionalArgument("file", "SMT-LIB file, or - for standard input.", "[file|-]");
    parser.addOptions(
        {{"connect", "Connect to an existing local daemon.", "socket"},
         {"mode", "Solve mode: check or optimize.", "mode", "check"},
         {"timeout-ms", "Worker deadline in milliseconds (1-120000).", "milliseconds", "10000"},
         {"no-model", "Omit the satisfying model."},
         {"no-unsat-core", "Omit the unsatisfiable core."}});
    if (!parser.parse(appArguments))
        return showErrorWithHelp(2, parser.errorText());
    if (parser.isSet("help"))
        return showHelp(0);
    const auto finish = [this](const QJsonObject &result) {
        const QString execution = result.value("execution").toString();
        exitCode                = execution == "completed"   ? 0
                                  : execution == "cancelled" ? 130
                                  : execution == "timeout"   ? 124
                                                             : 2;
        QSocConsole::out() << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        return exitCode == 0;
    };
    const auto fail = [&](const QString &reason) {
        return finish(QSocSmtService::failure("error", reason));
    };
    const QStringList paths = parser.positionalArguments();
    if (paths.size() > 1 || (parser.isSet("connect") && parser.value("connect").isEmpty()))
        return fail("Expected one input file and a nonempty daemon endpoint.");
    bool      validTimeout = false;
    const int timeout      = parser.value("timeout-ms").toInt(&validTimeout);
    if (!validTimeout || timeout < 1 || timeout > 120000)
        return fail("timeout-ms must be from 1 to 120000.");
    const QString mode = parser.value("mode");
    if (mode != "check" && mode != "optimize")
        return fail("mode must be check or optimize.");
    QFile         input;
    const QString path = paths.value(0, QStringLiteral("-"));
#ifdef Q_OS_WIN
    const int  stdinDescriptor   = ::_fileno(stdin);
    int        previousInputMode = -1;
    const auto restoreInputMode  = qScopeGuard([&] {
        if (previousInputMode >= 0)
            ::_setmode(stdinDescriptor, previousInputMode);
    });
#endif
    if (path == "-") {
#ifdef Q_OS_WIN
        const int descriptor = stdinDescriptor;
        if (descriptor < 0)
            return fail("Standard input is unavailable.");
        if (!::_isatty(descriptor)) {
            previousInputMode = ::_setmode(descriptor, _O_BINARY);
            if (previousInputMode < 0)
                return fail("Could not read standard input as bytes.");
        }
#else
        const int descriptor = ::fileno(stdin);
#endif
        if (!input.open(descriptor, QIODevice::ReadOnly))
            return fail(input.errorString());
    } else {
        input.setFileName(path);
        if (!input.open(QIODevice::ReadOnly))
            return fail(input.errorString());
    }
    QByteArray bytes;
    while (bytes.size() <= QSocSmtService::inputLimit) {
        const QByteArray chunk = input.read(QSocSmtService::inputLimit + 1 - bytes.size());
        if (chunk.isEmpty() && input.error() != QFileDevice::NoError)
            return fail(input.errorString());
        if (chunk.isEmpty())
            break;
        bytes += chunk;
    }
    const QString source = QString::fromUtf8(bytes);
    if (bytes.size() > QSocSmtService::inputLimit || source.toUtf8() != bytes)
        return fail("SMT-LIB input exceeds 256 KiB or is not valid UTF-8.");
    const QJsonObject request{
        {"smtlib", source},
        {"mode", mode},
        {"timeout_ms", timeout},
        {"return_model", !parser.isSet("no-model")},
        {"return_unsat_core", !parser.isSet("no-unsat-core")}};
    const QString invalid = QSocSmtService::validateRequest(request);
    if (!invalid.isEmpty())
        return fail(invalid);
    if (!QSocInterrupt::installBridge())
        return fail("Could not install the interrupt handler.");
    QSocInterrupt::clearRequest();
    const auto resetInterrupt = qScopeGuard([] { QSocInterrupt::finishForegroundHandoff(); });
    QSocDaemonConnection connection(parser.value("connect"));
    if (!connection.start())
        return fail(connection.error());
    auto &client = connection.client();
    if (!client.hasCapability("smt"))
        return fail("SMT is unsupported by this daemon.");
    QEventLoop loop;
    QTimer     deadline, interrupt;
    deadline.setSingleShot(true);
    QJsonObject  result;
    bool         cancelSent = false;
    const qint64 solveId    = client.nextId();
    connect(&client, &QSocAgentDaemonClient::replyReceived, &loop, [&](const QJsonObject &reply) {
        if (reply.value("id").toDouble() != solveId)
            return;
        result = reply.value("result").toObject();
        if (!QSocSmtService::isValidResponse(result))
            result = QSocSmtService::failure(
                "error", reply.value("error").toString("Invalid SMT response."));
        loop.quit();
    });
    connect(&client, &QSocAgentDaemonClient::disconnected, &loop, [&] {
        if (result.isEmpty())
            result = QSocSmtService::failure(
                "error", "Daemon disconnected; the request was not replayed.");
        loop.quit();
    });
    connect(&deadline, &QTimer::timeout, &loop, [&] {
        result = QSocSmtService::failure(
            "timeout",
            cancelSent ? "Cancelled request did not finish before the deadline."
                       : "Daemon response timed out.");
        client.disconnectFromDaemon();
        loop.quit();
    });
    connect(&interrupt, &QTimer::timeout, &loop, [&] {
        if (!QSocInterrupt::requested() || cancelSent)
            return;
        QSocInterrupt::drainSignalPipe();
        cancelSent = true;
        client.send(
            {{"id", client.nextId()},
             {"method", "smt.cancel"},
             {"params", QJsonObject{{"request_id", solveId}}}});
        deadline.start(5000);
    });
    client.send({{"id", solveId}, {"method", "smt.solve"}, {"params", request}});
    deadline.start(130000 + timeout);
    interrupt.start(20);
    if (result.isEmpty() && client.isConnected())
        loop.exec();
    if (result.isEmpty())
        result = QSocSmtService::failure("error", "The daemon connection closed before solving.");
    return finish(result);
}
