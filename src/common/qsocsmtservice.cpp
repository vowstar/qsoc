// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocsmtservice.h"
#include "common/qsocsibling.h"
#include "qsocipc.h"
#include "qsoclocalendpoint.h"
#include "qsoclocalpeer.h"
#include "qsocprocesslimits.h"

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>

namespace {

struct Admission
{
    std::mutex                  mutex;
    std::condition_variable_any changed;
    int                         active = 0;
    int                         queued = 0;
    qsizetype                   bytes  = 0;
};

Admission admission;

class Permit
{
public:
    ~Permit()
    {
        if (held) {
            const std::lock_guard lock(admission.mutex);
            --admission.active;
            admission.changed.notify_all();
        }
    }

    QString acquire(qsizetype bytes, std::stop_token stop)
    {
        std::unique_lock lock(admission.mutex);
        if (admission.queued >= 64 || admission.bytes + bytes > 16 * 1024 * 1024) {
            return QStringLiteral("busy");
        }
        ++admission.queued;
        admission.bytes += bytes;
        const bool ready = admission.changed.wait(lock, stop, [] { return admission.active < 2; });
        --admission.queued;
        admission.bytes -= bytes;
        if (!ready || stop.stop_requested()) {
            return QStringLiteral("cancelled");
        }
        ++admission.active;
        held = true;
        return {};
    }

private:
    bool held = false;
};

void stopProcess(QProcess &process)
{
    if (process.state() == QProcess::NotRunning) {
        return;
    }
    process.terminate();
    if (!process.waitForFinished(100)) {
        process.kill();
        process.waitForFinished(1900);
    }
}

bool validResponse(const QJsonObject &result)
{
    static const QSet<QString> executions
        = {"completed", "timeout", "cancelled", "resource_limit", "error", "busy"};
    static const QSet<QString> feasibility = {"feasible", "infeasible", "unknown"};
    static const QSet<QString> optimality
        = {"optimal", "unbounded", "limit", "not_proven", "not_applicable"};
    static const QSet<QString> status = {"sat", "unsat", "unknown"};
    const auto                 solver = result.value("solver_status");
    return result.value("protocol").toDouble() == 1
           && executions.contains(result.value("execution").toString())
           && feasibility.contains(result.value("feasibility").toString())
           && optimality.contains(result.value("optimality").toString())
           && result.value("truncated").isBool()
           && (solver.isNull() || status.contains(solver.toString()));
}

constexpr int wireLimit = 2 * 1024 * 1024;

QJsonObject receive(
    QLocalSocket   &socket,
    int             timeout,
    std::stop_token stop,
    QElapsedTimer  &elapsed,
    QProcess       *process = nullptr)
{
    QByteArray buffer;
    while (true) {
        buffer += socket.read(wireLimit + QSocIpc::headerBytes - buffer.size());
        QJsonObject message;
        const auto  decoded = QSocIpc::decode(buffer, message, wireLimit);
        if (decoded == QSocIpc::DecodeResult::Complete) {
            if (!buffer.isEmpty())
                return {};
            return message;
        }
        if (decoded == QSocIpc::DecodeResult::Invalid || stop.stop_requested()
            || elapsed.elapsed() >= timeout || socket.state() != QLocalSocket::ConnectedState)
            return {};
        socket.waitForReadyRead(20);
        if (process != nullptr)
            process->waitForFinished(0);
    }
}

QJsonObject interruption(std::stop_token stop, const QElapsedTimer &elapsed, int timeout)
{
    return QSocSmtService::failure(
        stop.stop_requested()          ? QStringLiteral("cancelled")
        : elapsed.elapsed() >= timeout ? QStringLiteral("timeout")
                                       : QStringLiteral("error"),
        QStringLiteral("SMT transport interrupted"));
}

QJsonObject response(const QJsonObject &message)
{
    const auto result = message.value("result").toObject();
    if (message.value("id").toInt() != 1 || !validResponse(result)
        || QJsonDocument(result).toJson(QJsonDocument::Compact).size() > QSocSmtService::outputLimit)
        return QSocSmtService::failure("error", "Invalid worker response");
    return result;
}

QJsonObject runWorker(
    QProcess          &process,
    QLocalServer      &server,
    const QJsonObject &request,
    int                timeout,
    std::stop_token    stop,
    QElapsedTimer     &elapsed)
{
    const int                     startupTimeout = qMin(timeout, 2000);
    std::unique_ptr<QLocalSocket> socket;
    while (!socket && elapsed.elapsed() < startupTimeout && !stop.stop_requested()) {
        server.waitForNewConnection(20);
        socket.reset(server.nextPendingConnection());
        process.waitForFinished(0);
        if (process.state() == QProcess::NotRunning && !socket)
            return QSocSmtService::failure("error", "Worker exited before connecting");
    }
    if (!socket)
        return interruption(stop, elapsed, startupTimeout);
    socket->setReadBufferSize(wireLimit + QSocIpc::headerBytes);
    const auto peerPid = QSocLocalPeer::processId(*socket);
    if (!QSocLocalPeer::sameUser(*socket) || (peerPid > 0 && peerPid != process.processId()))
        return QSocSmtService::failure("error", "Worker identity mismatch");
    const auto hello = receive(*socket, startupTimeout, stop, elapsed, &process);
    if (hello.value("service").toString() != "qsoc-smt-worker"
        || hello.value("protocol").toInt() != 1)
        return interruption(stop, elapsed, startupTimeout);
    socket->write(
        QSocIpc::frame(
            QJsonObject{{"id", 1}, {"method", "smt.solve"}, {"params", request}}, wireLimit));
    const auto message = receive(*socket, timeout, stop, elapsed, &process);
    if (stop.stop_requested() || elapsed.elapsed() >= timeout)
        return interruption(stop, elapsed, timeout);
    if (message.isEmpty()) {
        process.waitForFinished(100);
        return QSocSmtService::failure(
            process.exitCode() == 12 || process.exitCode() == 101 ? "resource_limit" : "error",
            "Worker did not finish successfully");
    }
    while (process.state() != QProcess::NotRunning && !stop.stop_requested()
           && elapsed.elapsed() < timeout)
        process.waitForFinished(20);
    if (process.state() != QProcess::NotRunning)
        return interruption(stop, elapsed, timeout);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return QSocSmtService::failure("error", "Worker did not exit successfully");
    return response(message);
}

} // namespace

QString QSocSmtService::workerPath()
{
    const QString found = QSocSibling::path(QStringLiteral("qsoc-smt-worker"));
    return found.isEmpty()
               ? QCoreApplication::applicationDirPath() + QStringLiteral("/qsoc-smt-worker")
               : found;
}

bool QSocSmtService::supported()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

QJsonObject QSocSmtService::failure(const QString &execution, const QString &reason)
{
    return {
        {"protocol", 1},
        {"execution", execution},
        {"reason", reason.left(4096)},
        {"solver_status", QJsonValue::Null},
        {"feasibility", "unknown"},
        {"optimality", "not_proven"},
        {"model_smtlib", QJsonValue::Null},
        {"truncated", false}};
}

QString QSocSmtService::validateRequest(const QJsonObject &request)
{
    static const QSet<QString> fields
        = {"smtlib", "mode", "priority", "timeout_ms", "return_model", "return_unsat_core"};
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (!fields.contains(field.key())) {
            return QStringLiteral("Unknown request field");
        }
    }
    if (!request.value("smtlib").isString()) {
        return QStringLiteral("smtlib must be a string");
    }
    const auto bytes = request.value("smtlib").toString().toUtf8();
    if (bytes.isEmpty() || bytes.size() > inputLimit || bytes.contains('\0')
        || QString::fromUtf8(bytes) != request.value("smtlib").toString()) {
        return QStringLiteral("Invalid SMT input size or NUL byte");
    }
    const auto mode = request.value("mode").toString(QStringLiteral("check"));
    if ((request.contains("mode") && !request.value("mode").isString())
        || (mode != "check" && mode != "optimize")) {
        return QStringLiteral("Unsupported mode");
    }
    if (request.contains("priority")
        && (mode != "optimize" || request.value("priority").toString() != "lex")) {
        return QStringLiteral("Only lex priority is supported in optimize mode");
    }
    const auto timeout = request.value("timeout_ms");
    if (!timeout.isUndefined()
        && (!timeout.isDouble() || timeout.toDouble() < 1 || timeout.toDouble() > 120000
            || std::floor(timeout.toDouble()) != timeout.toDouble())) {
        return QStringLiteral("timeout_ms must be an integer from 1 to 120000");
    }
    for (const auto *name : {"return_model", "return_unsat_core"}) {
        if (request.contains(name) && !request.value(name).isBool()) {
            return QStringLiteral("Result options must be booleans");
        }
    }
    return {};
}

QJsonObject QSocSmtService::solve(
    const QJsonObject &request, std::stop_token stop, const QString &executable)
{
    const QString validation = validateRequest(request);
    if (!validation.isEmpty()) {
        return failure("error", validation);
    }
    if (stop.stop_requested()) {
        return failure("cancelled", "Request cancelled");
    }
    if (executable.isEmpty()) {
        const auto endpoint = qEnvironmentVariable("QSOC_SMT_SOCKET");
        if (!endpoint.isEmpty())
            return solveRemote(request, endpoint, stop);
    }
    const QString path = executable.isEmpty() ? workerPath() : executable;
    if (!QFileInfo(path).isExecutable())
        return failure("error", "SMT worker is unavailable");
    const QByteArray input = QJsonDocument(request).toJson(QJsonDocument::Compact);
    Permit           permit;
    const auto       state = permit.acquire(input.size(), stop);
    if (!state.isEmpty())
        return failure(state, "Worker admission did not complete");
    QTemporaryDir directory;
    if (!directory.isValid())
        return failure("error", "Could not prepare worker socket directory");
    const auto endpoint = QSocLocalEndpoint::resolve(directory.filePath("smt.sock"));
    QString    error;
    if (!QSocLocalEndpoint::prepareDirectory(endpoint, &error))
        return failure("error", error);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.setMaxPendingConnections(1);
    if (!server.listen(endpoint))
        return failure("error", "Could not listen for SMT worker");
    QElapsedTimer elapsed;
    elapsed.start();
    const int         timeout = request.value("timeout_ms").toInt(10000);
    QSocProcessLimits limits;
    QProcess          process;
    if (!limits.configure(process, memoryLimitMiB * 1024ULL * 1024ULL))
        return failure("error", "Could not configure worker limits");
    process.setProgram(path);
    process.setArguments(
        {"--socket", endpoint, "--owner-pid", QString::number(QCoreApplication::applicationPid())});
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start();
    if (!process.waitForStarted(qMin(timeout, 2000))) {
        stopProcess(process);
        return interruption(stop, elapsed, timeout);
    }
    const auto result = runWorker(process, server, request, timeout, stop, elapsed);
    stopProcess(process);
    return result;
}

QJsonObject QSocSmtService::solveRemote(
    const QJsonObject &request, const QString &endpoint, std::stop_token stop)
{
    const auto validation = validateRequest(request);
    if (!validation.isEmpty())
        return failure("error", validation);
    if (stop.stop_requested())
        return failure("cancelled", "Request cancelled");
    QLocalSocket socket;
    socket.setReadBufferSize(wireLimit + QSocIpc::headerBytes);
    socket.connectToServer(QSocLocalEndpoint::resolve(endpoint));
    QElapsedTimer elapsed;
    elapsed.start();
    while (socket.state() == QLocalSocket::ConnectingState && !stop.stop_requested()
           && elapsed.elapsed() < 2000)
        socket.waitForConnected(20);
    if (socket.state() != QLocalSocket::ConnectedState || !QSocLocalPeer::sameUser(socket))
        return interruption(stop, elapsed, 2000);
    const auto hello = receive(socket, 2000, stop, elapsed);
    if (hello.isEmpty())
        return interruption(stop, elapsed, 2000);
    if (hello.value("daemon").toString() != "qsoc-agentd" || hello.value("protocol").toInt() != 1
        || !hello.value("capabilities").toArray().contains("smt"))
        return failure("error", "Incompatible SMT daemon");
    socket.write(
        QSocIpc::frame(
            QJsonObject{{"id", 1}, {"method", "smt.solve"}, {"params", request}}, wireLimit));
    const int  timeout = 130000 + request.value("timeout_ms").toInt(10000);
    const auto message = receive(socket, timeout, stop, elapsed);
    if (message.isEmpty())
        return interruption(stop, elapsed, timeout);
    return response(message);
}
