// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolsmt.h"
#include "common/qsocsmtservice.h"

#include <mutex>
#include <thread>
#include <QEventLoop>
#include <QJsonDocument>

namespace {
struct ToolBudget
{
    std::mutex mutex;
    int        jobs  = 0;
    qsizetype  bytes = 0;
};
ToolBudget toolBudget;

class Reservation
{
public:
    ~Reservation()
    {
        if (!bytes_)
            return;
        const std::lock_guard lock(toolBudget.mutex);
        --toolBudget.jobs;
        toolBudget.bytes -= bytes_;
    }
    bool acquire(qsizetype bytes)
    {
        const std::lock_guard lock(toolBudget.mutex);
        if (toolBudget.jobs >= 66 || toolBudget.bytes + bytes > 16 * 1024 * 1024)
            return false;
        ++toolBudget.jobs;
        toolBudget.bytes += bytes;
        bytes_ = bytes;
        return true;
    }

private:
    qsizetype bytes_ = 0;
};

QString encode(const QJsonObject &result)
{
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

QSocToolResultStatus status(const QJsonObject &result)
{
    return result.value("execution").toString() == "completed" ? QSocToolResultStatus::Ok
                                                               : QSocToolResultStatus::Failed;
}
} // namespace

struct QSocToolSmt::Job
{
    Reservation                          reservation;
    QPointer<QSocToolCallContext>        context;
    std::function<void(const QString &)> synchronousDone;
    std::jthread                         thread;
};

QSocToolSmt::QSocToolSmt(QObject *parent, QString executable)
    : QSocTool(parent)
    , executable_(std::move(executable))
{}

QSocToolSmt::~QSocToolSmt()
{
    for (const auto &[id, job] : jobs_) {
        Q_UNUSED(id)
        job->thread.request_stop();
    }
    for (const auto &[id, job] : jobs_) {
        Q_UNUSED(id)
        if (job->thread.joinable())
            job->thread.join();
        if (job->synchronousDone)
            job->synchronousDone(encode(QSocSmtService::failure("cancelled", "Tool destroyed")));
    }
}

bool QSocToolSmt::supported()
{
    return QSocSmtService::supported();
}

QString QSocToolSmt::finishImmediately(const QJsonObject &result)
{
    if (const auto context = currentCallContext())
        context->setResultStatus(status(result));
    return encode(result);
}

QString QSocToolSmt::getName() const
{
    return QStringLiteral("z3_solve");
}

QString QSocToolSmt::getDescription() const
{
    return QStringLiteral(
        "Check SMT-LIB constraints or optimize 1 to 16 lexicographic objectives in an isolated "
        "local worker. "
        "Accepts literal formulas, including in remote workspaces. Returns exact models, unsat "
        "cores, bounds, and separate feasibility and optimality states.");
}

json QSocToolSmt::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"smtlib",
           {{"type", "string"},
            {"description",
             "SMT-LIB declarations and assertions, at most 256 KiB UTF-8. Optimize also accepts "
             "minimize/maximize. No commands for files, output, options, stacks, or recursive "
             "definitions."}}},
          {"mode",
           {{"type", "string"}, {"enum", json::array({"check", "optimize"})}, {"default", "check"}}},
          {"priority",
           {{"type", "string"},
            {"enum", json::array({"lex"})},
            {"description", "Optimize mode only. Objectives follow declaration order."}}},
          {"timeout_ms",
           {{"type", "integer"}, {"minimum", 1}, {"maximum", 120000}, {"default", 10000}}},
          {"return_model", {{"type", "boolean"}, {"default", true}}},
          {"return_unsat_core", {{"type", "boolean"}, {"default", true}}}}},
        {"required", json::array({"smtlib"})}};
}

QString QSocToolSmt::execute(const json &arguments)
{
    if (!arguments.is_object())
        return finishImmediately(QSocSmtService::failure("error", "Expected an object"));
    QByteArray bytes;
    try {
        bytes = QByteArray::fromStdString(arguments.dump());
    } catch (const json::exception &) {
        return finishImmediately(QSocSmtService::failure("error", "Invalid request encoding"));
    }
    if (bytes.size() > 1024 * 1024)
        return finishImmediately(QSocSmtService::failure("error", "Request exceeds the byte limit"));
    const auto    request = QJsonDocument::fromJson(bytes).object();
    const QString error   = QSocSmtService::validateRequest(request);
    if (!error.isEmpty())
        return finishImmediately(QSocSmtService::failure("error", error));
    const QPointer<QSocToolCallContext> context = currentCallContext();
    if (context && context->isCancellationRequested())
        return finishImmediately(QSocSmtService::failure("cancelled", "Request cancelled"));
    auto job = std::make_unique<Job>();
    if (!job->reservation.acquire(bytes.size()))
        return finishImmediately(QSocSmtService::failure("busy", "Tool request queue is full"));
    job->context = context;
    QEventLoop loop;
    QString    result;
    const bool deferred = context && context->canDefer();
    if (!deferred)
        job->synchronousDone = [&loop, &result](const QString &value) {
            result = value;
            loop.quit();
        };
    const quint64 id = ++nextId_;
    jobs_.emplace(id, std::move(job));
    const QString executable = executable_;
    try {
        jobs_.at(id)->thread = std::jthread([this, id, request, executable](std::stop_token stop) {
            QJsonObject value;
            try {
                value = QSocSmtService::solve(request, stop, executable);
            } catch (const std::exception &) {
                value = QSocSmtService::failure("error", "Worker task failed");
            }
            /* The destructor joins workers before QObject releases queued invocations. */
            QMetaObject::invokeMethod(
                this, [this, id, value] { completeJob(id, value); }, Qt::QueuedConnection);
        });
    } catch (const std::system_error &) {
        jobs_.erase(id);
        return finishImmediately(QSocSmtService::failure("busy", "Could not start the worker task"));
    }
    if (context) {
        const auto cancel = [stop = jobs_.at(id)->thread.get_stop_source()]() mutable {
            stop.request_stop();
        };
        connect(context, &QSocToolCallContext::cancellationRequested, this, cancel);
        connect(context, &QObject::destroyed, this, cancel);
    }
    if (deferred) {
        context->defer();
        return {};
    }
    loop.exec();
    return result;
}

void QSocToolSmt::completeJob(quint64 id, QJsonObject result)
{
    const auto active = jobs_.find(id);
    if (active == jobs_.end())
        return;
    auto job = std::move(active->second);
    jobs_.erase(active);
    if (job->thread.joinable())
        job->thread.join();
    if (job->context && job->context->isCancellationRequested())
        result = QSocSmtService::failure("cancelled", "Request cancelled");
    if (job->context)
        job->context->setResultStatus(status(result));
    const auto encoded = encode(result);
    if (job->synchronousDone)
        job->synchronousDone(encoded);
    else if (job->context)
        job->context->completeDeferred(encoded);
}
