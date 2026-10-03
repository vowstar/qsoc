// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "smt/qsocsmtbroker.h"
#include "common/qsocresourceusage.h"
#include "smt/qsocsmtservice.h"

#include <cmath>
#include <thread>
#include <utility>
#include <QHash>
#include <QJsonDocument>
#include <QList>
#include <QTimer>

namespace {
QSocMemoryBudget::Snapshot sampleMemory()
{
    const auto                 report = QSocResourceUsage::system();
    QSocMemoryBudget::Snapshot result;
    for (const auto &field :
         {QStringLiteral("memory_effective_available_bytes"),
          QStringLiteral("memory_available_bytes")}) {
        const auto bytes = report.value(field);
        const auto value = bytes.toDouble(-1);
        if (!bytes.isDouble() || !std::isfinite(value) || value < 0 || value > 9007199254740991.0
            || std::floor(value) != value)
            continue;
        result.availableBytes = static_cast<quint64>(value);
        const bool effective  = field == "memory_effective_available_bytes";
        result.coverage       = effective ? QSocMemoryBudget::Coverage::Effective
                                          : QSocMemoryBudget::Coverage::HostOnly;
        result.kind
            = report.value(effective ? "memory_effective_available_kind" : "memory_available_kind")
                  .toString();
        break;
    }
    const auto sampled = report.value("sampled_at_ns").toInteger(-1);
    if (sampled >= 0)
        result.sampledAt = QSocMemoryBudget::Clock::time_point(std::chrono::nanoseconds(sampled));
    return result;
}
} // namespace

struct QSocSmtBroker::State
{
    static constexpr qsizetype queueLimit   = 16 * 1024 * 1024;
    static constexpr int       activeLimit  = 2;
    static constexpr int       pendingLimit = 64;
    static constexpr int       ownerLimit   = 16;

    struct Job
    {
        quint64      owner = 0;
        QJsonObject  request;
        Reply        reply;
        qsizetype    bytes      = 0;
        bool         running    = false;
        bool         canceled   = false;
        bool         unverified = false;
        QTimer       deadline;
        std::jthread thread;
    };

    QSocSmtBroker              *host;
    Solver                      solver;
    Sampler                     sampler;
    QSocMemoryBudget::Policy    memoryPolicy;
    QSocMemoryBudget::Snapshot  memorySample;
    QSocMemoryBudget::Admission admission = QSocMemoryBudget::Admission::Unverified;
    QTimer                      resourceRetry;
    static constexpr quint64 workerMemory = quint64(QSocSmtService::memoryLimitMiB) * 1024 * 1024;
    QHash<quint64, std::shared_ptr<Job>> jobs;
    QHash<quint64, QList<quint64>>       queues;
    QList<quint64>                       readyOwners;
    quint64                              nextId      = 0;
    qsizetype                            queuedBytes = 0;
    int                                  queueWaitMs = 120000;
    int                                  queued      = 0;
    int                                  active      = 0;
    bool                                 stopping    = false;

    State(QSocSmtBroker *broker, Solver executor, QSocMemoryBudget::Policy policy, Sampler provider)
        : host(broker)
        , solver(std::move(executor))
        , sampler(provider ? std::move(provider) : sampleMemory)
        , memoryPolicy(policy)
    {
        if (!solver) {
            solver = [](const QJsonObject &request, std::stop_token stop) {
                return QSocSmtService::solve(request, stop, QSocSmtService::workerPath());
            };
        }
    }

    void pump()
    {
        resourceRetry.stop();
        while (!stopping && active < activeLimit && !readyOwners.isEmpty()) {
            try {
                memorySample = sampler();
            } catch (...) {
                memorySample = {};
            }
            admission = QSocMemoryBudget::evaluate(
                memoryPolicy,
                memorySample,
                quint64(active) * workerMemory,
                workerMemory,
                QSocMemoryBudget::Clock::now());
            if (admission == QSocMemoryBudget::Admission::WaitingMemory
                || admission == QSocMemoryBudget::Admission::WaitingMeasurement) {
                resourceRetry.start(1000);
                return;
            }
            const auto owner = readyOwners.takeFirst();
            auto      &queue = queues[owner];
            const auto id    = queue.takeFirst();
            if (queue.isEmpty())
                queues.remove(owner);
            else
                readyOwners.append(owner);
            const auto job = jobs.value(id);
            queuedBytes -= job->bytes;
            --queued;
            job->deadline.stop();
            job->running    = true;
            job->unverified = admission != QSocMemoryBudget::Admission::Admitted;
            ++active;
            job->thread = std::jthread([this, id, request = job->request](std::stop_token stop) {
                QJsonObject result;
                try {
                    result = solver(request, stop);
                } catch (...) {
                    result = QSocSmtService::failure("error", "SMT worker service failed");
                }
                QMetaObject::invokeMethod(
                    host, [this, id, result] { complete(id, result); }, Qt::QueuedConnection);
            });
        }
    }

    void complete(quint64 id, const QJsonObject &result)
    {
        const auto job = jobs.take(id);
        if (!job)
            return;
        if (job->thread.joinable())
            job->thread.join();
        --active;
        const auto reply    = std::move(job->reply);
        const auto terminal = job->canceled
                                  ? QSocSmtService::failure("cancelled", "SMT request canceled")
                                  : result;
        pump();
        if (reply)
            reply(terminal);
    }

    bool cancel(quint64 id, bool timeout = false)
    {
        const auto job = jobs.value(id);
        if (!job)
            return false;
        job->canceled = true;
        if (job->running) {
            job->thread.request_stop();
            return true;
        }
        auto &queue = queues[job->owner];
        queue.removeOne(id);
        if (queue.isEmpty()) {
            queues.remove(job->owner);
            readyOwners.removeOne(job->owner);
        }
        queuedBytes -= job->bytes;
        --queued;
        jobs.remove(id);
        if (queued == 0)
            resourceRetry.stop();
        const auto    reply = std::move(job->reply);
        const QString reason
            = admission == QSocMemoryBudget::Admission::WaitingMemory
                  ? QStringLiteral("SMT queue wait expired: insufficient memory budget")
              : admission == QSocMemoryBudget::Admission::WaitingMeasurement
                  ? QStringLiteral("SMT queue wait expired: memory availability is unverified")
                  : QStringLiteral("SMT queue wait expired");
        if (reply)
            reply(
                timeout ? QSocSmtService::failure("timeout", reason)
                        : QSocSmtService::failure("cancelled", "SMT request canceled"));
        return true;
    }
};

QSocSmtBroker::QSocSmtBroker(
    QObject                 *parent,
    Solver                   solver,
    int                      queueWaitMs,
    QSocMemoryBudget::Policy memoryPolicy,
    Sampler                  sampler)
    : QObject(parent)
    , d(std::make_unique<State>(this, std::move(solver), memoryPolicy, std::move(sampler)))
{
    d->queueWaitMs = qMax(1, queueWaitMs);
    d->resourceRetry.setSingleShot(true);
    connect(&d->resourceRetry, &QTimer::timeout, this, [this] { d->pump(); });
}

QSocSmtBroker::~QSocSmtBroker()
{
    shutdown();
}

quint64 QSocSmtBroker::submit(quint64 owner, const QJsonObject &request, Reply reply)
{
    const auto validation = QSocSmtService::validateRequest(request);
    if (!validation.isEmpty()) {
        reply(QSocSmtService::failure("error", validation));
        return 0;
    }
    const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact).size();
    int        owned = 0;
    for (const auto &job : std::as_const(d->jobs)) {
        if (job->owner == owner)
            ++owned;
    }
    if (d->stopping || d->queued >= State::pendingLimit
        || d->queuedBytes + bytes > State::queueLimit || owned >= State::ownerLimit) {
        reply(QSocSmtService::failure("busy", "SMT queue is full"));
        return 0;
    }
    auto job      = std::make_shared<State::Job>();
    job->owner    = owner;
    job->request  = request;
    job->reply    = std::move(reply);
    job->bytes    = bytes;
    const auto id = ++d->nextId;
    d->jobs.insert(id, job);
    auto &queue = d->queues[owner];
    if (queue.isEmpty())
        d->readyOwners.append(owner);
    queue.append(id);
    d->queuedBytes += bytes;
    ++d->queued;
    job->deadline.setSingleShot(true);
    connect(
        &job->deadline,
        &QTimer::timeout,
        this,
        [this, id] {
            const auto queuedJob = d->jobs.value(id);
            if (queuedJob && !queuedJob->running)
                d->cancel(id, true);
        },
        Qt::QueuedConnection);
    job->deadline.start(d->queueWaitMs);
    d->pump();
    return id;
}

bool QSocSmtBroker::cancel(quint64 task)
{
    return d->cancel(task);
}

void QSocSmtBroker::removeOwner(quint64 owner)
{
    const auto ids = d->jobs.keys();
    for (auto id : ids) {
        const auto job = d->jobs.value(id);
        if (job && job->owner == owner)
            d->cancel(id);
    }
}

void QSocSmtBroker::shutdown()
{
    if (d->stopping)
        return;
    d->stopping = true;
    d->resourceRetry.stop();
    const auto ids = d->jobs.keys();
    for (auto id : ids) {
        d->jobs.value(id)->reply = {};
        d->cancel(id);
    }
    for (const auto &job : std::as_const(d->jobs)) {
        if (job->thread.joinable())
            job->thread.join();
    }
    d->jobs.clear();
    d->queues.clear();
    d->readyOwners.clear();
    d->active      = 0;
    d->queued      = 0;
    d->queuedBytes = 0;
}

int QSocSmtBroker::activeCount() const
{
    return d->active;
}
int QSocSmtBroker::queuedCount() const
{
    return d->queued;
}

QJsonObject QSocSmtBroker::resourceStatus() const
{
    const bool sampleFresh
        = QSocMemoryBudget::fresh(d->memorySample, QSocMemoryBudget::Clock::now());
    int unverified = 0;
    for (const auto &job : std::as_const(d->jobs))
        unverified += job->running && job->unverified;
    return {
        {"admission", QSocMemoryBudget::name(d->admission)},
        {"scope", "daemon"},
        {"reserved_bytes", static_cast<qint64>(d->active * State::workerMemory)},
        {"host_reserve_bytes", static_cast<double>(d->memoryPolicy.reserveBytes)},
        {"strict_sampling", d->memoryPolicy.strictSampling},
        {"available_bytes",
         sampleFresh && d->memorySample.availableBytes
             ? QJsonValue(static_cast<double>(*d->memorySample.availableBytes))
             : QJsonValue()},
        {"sample_fresh", sampleFresh},
        {"availability_kind", d->memorySample.kind},
        {"availability_coverage",
         d->memorySample.coverage == QSocMemoryBudget::Coverage::Effective ? "visible_limits"
                                                                           : "host_only"},
        {"active_count", d->active},
        {"queued_count", d->queued},
        {"unverified_active", unverified}};
}

#include "moc_qsocsmtbroker.cpp"
