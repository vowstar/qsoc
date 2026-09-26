// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocsmtservice.h"

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>

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

QJsonObject collect(QProcess &process, int timeout, std::stop_token stop, QElapsedTimer &elapsed)
{
    QByteArray output;
    while (process.state() != QProcess::NotRunning) {
        if (stop.stop_requested() || elapsed.elapsed() >= timeout) {
            stopProcess(process);
            return QSocSmtService::failure(
                stop.stop_requested() ? QStringLiteral("cancelled") : QStringLiteral("timeout"),
                QStringLiteral("Worker interrupted"));
        }
        process.waitForFinished(20);
        output += process.read(QSocSmtService::outputLimit - output.size() + 1);
        if (output.size() > QSocSmtService::outputLimit) {
            stopProcess(process);
            return QSocSmtService::failure("error", "Worker output exceeded the limit");
        }
    }
    output += process.read(QSocSmtService::outputLimit - output.size() + 1);
    if (stop.stop_requested()) {
        return QSocSmtService::failure("cancelled", "Request cancelled");
    }
    if (elapsed.elapsed() >= timeout) {
        return QSocSmtService::failure("timeout", "Worker exceeded the time limit");
    }
    if (output.size() > QSocSmtService::outputLimit) {
        return QSocSmtService::failure("error", "Worker output exceeded the limit");
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return QSocSmtService::failure(
            (process.exitCode() == 12 || process.exitCode() == 101)
                ? QStringLiteral("resource_limit")
                : QStringLiteral("error"),
            QStringLiteral("Worker did not finish successfully"));
    }
    QJsonParseError error;
    const auto      document = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || !validResponse(document.object())) {
        return QSocSmtService::failure("error", "Invalid worker response");
    }
    return document.object();
}

} // namespace

QString QSocSmtService::workerPath()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/qsoc-smt-worker");
}

bool QSocSmtService::supported()
{
#ifdef Q_OS_LINUX
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
#ifndef Q_OS_LINUX
    Q_UNUSED(executable)
    return failure("error", "Hard memory isolation is unavailable on this platform");
#else
    const QString path = executable.isEmpty() ? workerPath() : executable;
    if (!QFileInfo(path).isExecutable()) {
        return failure("error", "SMT worker is unavailable");
    }
    const QByteArray input = QJsonDocument(request).toJson(QJsonDocument::Compact);
    Permit           permit;
    const auto       state = permit.acquire(input.size(), stop);
    if (!state.isEmpty()) {
        return failure(state, "Worker admission did not complete");
    }
    QElapsedTimer elapsed;
    elapsed.start();
    const int timeout = request.value("timeout_ms").toInt(10000);
    QProcess  process;
    process.setProgram(path);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start();
    if (!process.waitForStarted(qMin(timeout, 2000))) {
        const auto execution = stop.stop_requested()          ? QStringLiteral("cancelled")
                               : elapsed.elapsed() >= timeout ? QStringLiteral("timeout")
                                                              : QStringLiteral("error");
        stopProcess(process);
        return failure(execution, "Could not start SMT worker");
    }
    process.write(input);
    process.closeWriteChannel();
    return collect(process, timeout, stop, elapsed);
#endif
}
