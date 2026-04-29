// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocloopscheduler.h"

#include "common/qsoccron.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

namespace {

constexpr qint64 kTickMs        = 1000;
constexpr int    kLockStaleMs   = 0;
constexpr int    kSchemaVersion = 2;

QString unitWordToSuffix(const QString &word)
{
    const QString w = word.toLower();
    if (w == "s" || w == "sec" || w == "secs" || w == "second" || w == "seconds")
        return QStringLiteral("s");
    if (w == "m" || w == "min" || w == "mins" || w == "minute" || w == "minutes")
        return QStringLiteral("m");
    if (w == "h" || w == "hr" || w == "hrs" || w == "hour" || w == "hours")
        return QStringLiteral("h");
    if (w == "d" || w == "day" || w == "days")
        return QStringLiteral("d");
    return QString();
}

QString intervalErrorMessage(const QString &token)
{
    return QStringLiteral(
               "interval '%1' does not cleanly divide its unit; "
               "use a clean step like 5m, 10m, 15m, 30m, 1h, 2h, 1d")
        .arg(token);
}

} /* namespace */

QSocLoopScheduler::QSocLoopScheduler(QObject *parent)
    : QObject(parent)
{
    timer_.setInterval(static_cast<int>(kTickMs));
    connect(&timer_, &QTimer::timeout, this, &QSocLoopScheduler::tick);
    timer_.start();
}

QSocLoopScheduler::~QSocLoopScheduler()
{
    releaseLock();
}

bool QSocLoopScheduler::isOwner() const
{
    return isOwner_;
}

QString QSocLoopScheduler::tasksPath() const
{
    return QDir(projectDir_).filePath(QStringLiteral(".qsoc/loops.json"));
}

QString QSocLoopScheduler::lockPath() const
{
    return QDir(projectDir_).filePath(QStringLiteral(".qsoc/loops.lock"));
}

void QSocLoopScheduler::setProjectDir(const QString &dir)
{
    if (dir == projectDir_)
        return;
    releaseLock();
    projectDir_ = dir;
    /* Drop both durable and session-only tasks on rebind. Session-only
     * tasks were created against the previous project's REPL context;
     * carrying them over silently would surprise the user. */
    jobs_.clear();
    isOwner_ = projectDir_.isEmpty();
    if (projectDir_.isEmpty())
        return;
    QDir(projectDir_).mkpath(QStringLiteral(".qsoc"));
    loadFromDisk();
    isOwner_ = tryAcquireLock();
}

void QSocLoopScheduler::loadFromDisk()
{
    QFile file(tasksPath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = file.readAll();
    file.close();
    QJsonParseError parse{};
    const auto      doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
        return;
    const QJsonObject root   = doc.object();
    const int         schema = root.value(QStringLiteral("schema")).toInt(0);
    if (schema != kSchemaVersion) {
        qDebug().noquote() << QStringLiteral(
                                  "[loop] dropping legacy schema-%1 loops.json (no released "
                                  "users; new schema is %2)")
                                  .arg(schema)
                                  .arg(kSchemaVersion);
        return;
    }

    const QJsonArray arr = root.value(QStringLiteral("tasks")).toArray();
    for (const auto &val : arr) {
        const auto obj = val.toObject();
        Job        job;
        job.id          = obj.value(QStringLiteral("id")).toString();
        job.prompt      = obj.value(QStringLiteral("prompt")).toString();
        job.cron        = obj.value(QStringLiteral("cron")).toString();
        job.recurring   = obj.value(QStringLiteral("recurring")).toBool(false);
        job.durable     = true; /* Anything on disk is durable by definition. */
        job.createdAt   = obj.value(QStringLiteral("createdAt")).toVariant().toLongLong();
        job.lastFiredAt = obj.value(QStringLiteral("lastFiredAt")).toVariant().toLongLong();
        if (job.id.isEmpty() || job.prompt.isEmpty() || !QSocCron::isValid(job.cron)) {
            qDebug().noquote() << QStringLiteral("[loop] skipping malformed task entry");
            continue;
        }
        jobs_.append(job);
    }
}

bool QSocLoopScheduler::persist()
{
    if (projectDir_.isEmpty())
        return true;
    QJsonArray arr;
    for (const auto &job : jobs_) {
        if (!job.durable)
            continue; /* session-only tasks never touch disk */
        QJsonObject obj;
        obj[QStringLiteral("id")]          = job.id;
        obj[QStringLiteral("prompt")]      = job.prompt;
        obj[QStringLiteral("cron")]        = job.cron;
        obj[QStringLiteral("recurring")]   = job.recurring;
        obj[QStringLiteral("createdAt")]   = QString::number(job.createdAt);
        obj[QStringLiteral("lastFiredAt")] = QString::number(job.lastFiredAt);
        arr.append(obj);
    }
    QJsonObject root;
    root[QStringLiteral("schema")] = kSchemaVersion;
    root[QStringLiteral("tasks")]  = arr;
    QSaveFile        file(tasksPath());
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit())
        return false;
    persistDegraded_ = false;
    return true;
}

bool QSocLoopScheduler::tryAcquireLock()
{
    if (projectDir_.isEmpty())
        return true;
    if (!lockFile_) {
        lockFile_ = std::make_unique<QLockFile>(lockPath());
        lockFile_->setStaleLockTime(kLockStaleMs);
    }
    if (lockFile_->isLocked())
        return true;
    return lockFile_->tryLock(0);
}

void QSocLoopScheduler::releaseLock()
{
    if (lockFile_ && lockFile_->isLocked()) {
        lockFile_->unlock();
    }
    lockFile_.reset();
}

bool QSocLoopScheduler::scheduledInputRequiresCliDispatch(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return false;
    const QChar first = trimmed.at(0);
    return first == QLatin1Char('/') || first == QLatin1Char('!');
}

void QSocLoopScheduler::parseLoopArgs(
    const QString &args,
    const QString &defaultCron,
    QString       &outCron,
    QString       &outPrompt,
    QString       &outErrorIfAny)
{
    const QString trimmed = args.trimmed();
    outCron.clear();
    outPrompt.clear();
    outErrorIfAny.clear();

    if (trimmed.isEmpty()) {
        outCron = defaultCron;
        return;
    }

    /* Rule 1: leading token is an interval (e.g. "5m foo"). */
    static const QRegularExpression reIntervalLead(QStringLiteral("^(\\d+[smhd])(?:\\s+(.*))?$"));
    {
        const auto m = reIntervalLead.match(trimmed);
        if (m.hasMatch()) {
            const QString token = m.captured(1);
            const QString cron  = QSocCron::intervalToCron(token);
            if (cron.isEmpty()) {
                outErrorIfAny = intervalErrorMessage(token);
                return;
            }
            outCron   = cron;
            outPrompt = m.captured(2).trimmed();
            return;
        }
    }

    /* Rule 2: trailing "every Nx" or "every N <unit-word>". */
    static const QRegularExpression reEvery1(QStringLiteral("(?i)\\s+every\\s+(\\d+)([smhd])\\s*$"));
    static const QRegularExpression reEvery2(QStringLiteral(
        "(?i)\\s+every\\s+(\\d+)\\s+(seconds?|secs?|minutes?|mins?|"
        "hours?|hrs?|days?|s|m|h|d)\\s*$"));
    {
        const auto m1 = reEvery1.match(trimmed);
        if (m1.hasMatch()) {
            const QString token = m1.captured(1) + m1.captured(2);
            const QString cron  = QSocCron::intervalToCron(token);
            if (cron.isEmpty()) {
                outErrorIfAny = intervalErrorMessage(token);
                return;
            }
            outCron   = cron;
            outPrompt = trimmed.left(m1.capturedStart(0)).trimmed();
            return;
        }
        const auto m2 = reEvery2.match(trimmed);
        if (m2.hasMatch()) {
            const QString suffix = unitWordToSuffix(m2.captured(2));
            if (!suffix.isEmpty()) {
                const QString token = m2.captured(1) + suffix;
                const QString cron  = QSocCron::intervalToCron(token);
                if (cron.isEmpty()) {
                    outErrorIfAny = intervalErrorMessage(token);
                    return;
                }
                outCron   = cron;
                outPrompt = trimmed.left(m2.capturedStart(0)).trimmed();
                return;
            }
        }
    }

    /* Rule 3: default. */
    outCron   = defaultCron;
    outPrompt = trimmed;
}

QString QSocLoopScheduler::allocateId()
{
    /* 8 hex chars from a v4 UUID: stable across restarts, uniformly
     * distributed, and short enough to type into /loop stop. */
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

QString QSocLoopScheduler::addJob(
    const QString &cron, const QString &prompt, bool recurring, bool durable)
{
    if (prompt.trimmed().isEmpty())
        return QString();
    if (!QSocCron::isValid(cron))
        return QString();
    if (jobs_.size() >= kMaxJobs)
        return QString();
    /* Non-owner cannot mutate durable on-disk state; session-only is fine
     * because non-owners run their own in-memory list and never persist. */
    if (durable && !projectDir_.isEmpty() && !isOwner_)
        return QString();

    Job job;
    job.id          = allocateId();
    job.prompt      = prompt;
    job.cron        = cron;
    job.recurring   = recurring;
    job.durable     = durable;
    job.createdAt   = QDateTime::currentMSecsSinceEpoch();
    job.lastFiredAt = 0;
    jobs_.append(job);
    if (durable && !persist()) {
        jobs_.removeLast();
        return QString();
    }
    return job.id;
}

bool QSocLoopScheduler::removeJob(const QString &id)
{
    for (int i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i].id != id)
            continue;
        const Job stash = jobs_[i];
        if (stash.durable && !projectDir_.isEmpty() && !isOwner_)
            return false;
        jobs_.removeAt(i);
        if (stash.durable && !persist()) {
            jobs_.insert(i, stash);
            return false;
        }
        return true;
    }
    return false;
}

bool QSocLoopScheduler::clearJobs()
{
    /* If any durable task exists in durable mode, only the owner can clear. */
    bool anyDurable = false;
    for (const auto &job : jobs_) {
        if (job.durable) {
            anyDurable = true;
            break;
        }
    }
    if (anyDurable && !projectDir_.isEmpty() && !isOwner_)
        return false;
    QList<Job> stash = jobs_;
    jobs_.clear();
    if (!persist()) {
        jobs_ = stash;
        return false;
    }
    return true;
}

QList<QSocLoopScheduler::Job> QSocLoopScheduler::listJobs()
{
    if (!projectDir_.isEmpty() && !isOwner_) {
        /* Re-read durable tasks from disk; preserve session-only ones
         * (which only exist in our memory) by stashing them across the
         * reload. */
        QList<Job> sessionOnly;
        for (const auto &job : jobs_) {
            if (!job.durable)
                sessionOnly.append(job);
        }
        jobs_.clear();
        loadFromDisk();
        for (const auto &job : sessionOnly)
            jobs_.append(job);
    }
    return jobs_;
}

void QSocLoopScheduler::tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    /* When bound to a project dir: only the QLockFile owner fires durable
     * tasks. Non-owners silently re-probe each tick so a crashed owner is
     * taken over without a restart. Session-only tasks always fire (they
     * are private to this process). */
    if (!projectDir_.isEmpty() && !isOwner_) {
        if (tryAcquireLock()) {
            isOwner_ = true;
            QList<Job> sessionOnly;
            for (const auto &job : jobs_) {
                if (!job.durable)
                    sessionOnly.append(job);
            }
            jobs_.clear();
            loadFromDisk();
            for (const auto &job : sessionOnly)
                jobs_.append(job);
        }
        /* Even if we just took over, fall through to evaluate this tick. */
    }

    if (jobs_.isEmpty())
        return;

    /* Snapshot due payloads up front: emit may re-enter (e.g. via
     * /loop dispatch). One-shot tasks must be erased from jobs_ BEFORE
     * emit so a re-entrant tick cannot fire them again with lastFiredAt
     * advanced; recurring tasks update lastFiredAt to anchor next match. */
    QList<QPair<QString, QString>> due;
    QList<QString>                 oneShotsToErase;
    bool                           anyMutation = false;
    for (auto &job : jobs_) {
        const qint64 anchor = job.lastFiredAt > 0 ? job.lastFiredAt : job.createdAt;
        const qint64 next   = QSocCron::nextRunMs(job.cron, anchor);
        if (next == 0 || now < next)
            continue;
        due.append(qMakePair(job.prompt, job.id));
        if (job.recurring) {
            job.lastFiredAt = now;
            anyMutation     = true;
        } else {
            oneShotsToErase.append(job.id);
        }
    }
    for (const QString &eraseId : oneShotsToErase) {
        for (int i = 0; i < jobs_.size(); ++i) {
            if (jobs_[i].id == eraseId) {
                jobs_.removeAt(i);
                anyMutation = true;
                break;
            }
        }
    }
    if (anyMutation && !persist() && !persistDegraded_) {
        persistDegraded_ = true;
        emit persistFailed(tasksPath());
    }
    for (const auto &pair : due)
        emit promptDue(pair.first, pair.second);
}
