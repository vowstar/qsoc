// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "smt/qsocsmtbroker.h"
#include "smt/qsocsmtservice.h"

#include <thread>
#include <utility>
#include <QHash>
#include <QJsonDocument>
#include <QList>
#include <QTimer>

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
        qsizetype    bytes    = 0;
        bool         running  = false;
        bool         canceled = false;
        std::jthread thread;
    };

    QSocSmtBroker                       *host;
    Solver                               solver;
    QHash<quint64, std::shared_ptr<Job>> jobs;
    QHash<quint64, QList<quint64>>       queues;
    QList<quint64>                       readyOwners;
    quint64                              nextId      = 0;
    qsizetype                            queuedBytes = 0;
    int                                  queueWaitMs = 120000;
    int                                  queued      = 0;
    int                                  active      = 0;
    bool                                 stopping    = false;

    State(QSocSmtBroker *broker, Solver executor)
        : host(broker)
        , solver(std::move(executor))
    {
        if (!solver) {
            solver = [](const QJsonObject &request, std::stop_token stop) {
                return QSocSmtService::solve(request, stop, QSocSmtService::workerPath());
            };
        }
    }

    void pump()
    {
        while (!stopping && active < activeLimit && !readyOwners.isEmpty()) {
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
            job->running = true;
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
        const auto reply = std::move(job->reply);
        if (reply)
            reply(
                timeout ? QSocSmtService::failure("timeout", "SMT queue wait expired")
                        : QSocSmtService::failure("cancelled", "SMT request canceled"));
        return true;
    }
};

QSocSmtBroker::QSocSmtBroker(QObject *parent, Solver solver, int queueWaitMs)
    : QObject(parent)
    , d(std::make_unique<State>(this, std::move(solver)))
{
    d->queueWaitMs = qMax(1, queueWaitMs);
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
    QTimer::singleShot(d->queueWaitMs, this, [this, id] {
        const auto queuedJob = d->jobs.value(id);
        if (queuedJob && !queuedJob->running)
            d->cancel(id, true);
    });
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
    d->stopping    = true;
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

#include "moc_qsocsmtbroker.cpp"
