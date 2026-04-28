// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocloopscheduler.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

constexpr qint64 kTickMs        = 1000;
constexpr qint64 kSecondMs      = 1000;
constexpr qint64 kMinMs         = qint64{60} * kSecondMs;
constexpr qint64 kHourMs        = qint64{60} * kMinMs;
constexpr qint64 kDayMs         = qint64{24} * kHourMs;
constexpr qint64 kLockRefreshMs = 3000;
constexpr qint64 kLockStaleMs   = 15000;
constexpr int    kSchemaVersion = 1;

bool pidIsAlive(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_UNIX
    /* signal 0 is the canonical liveness probe: no signal sent, but the
     * permission/existence check still runs. ESRCH means the pid is gone. */
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#else
    /* Without a portable way to check, fall back to the staleness window. */
    return true;
#endif
}

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

} /* namespace */

QSocLoopScheduler::QSocLoopScheduler(QObject *parent)
    : QObject(parent)
{
    /* Per-process owner key. Pid alone is not enough because a pid can
     * recycle after a crash; pairing it with a UUID disambiguates. */
    sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    timer_.setInterval(static_cast<int>(kTickMs));
    connect(&timer_, &QTimer::timeout, this, &QSocLoopScheduler::tick);
    timer_.start();
}

QSocLoopScheduler::~QSocLoopScheduler()
{
    if (!projectDir_.isEmpty() && isOwner_) {
        releaseLock();
    }
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
    if (!projectDir_.isEmpty() && isOwner_) {
        releaseLock();
    }
    projectDir_ = dir;
    jobs_.clear();
    nameCounter_ = 0;
    isOwner_     = projectDir_.isEmpty();
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

    const QJsonArray arr = doc.object().value(QStringLiteral("tasks")).toArray();
    for (const auto &val : arr) {
        const auto obj = val.toObject();
        Job        job;
        job.name        = obj.value(QStringLiteral("id")).toString();
        job.prompt      = obj.value(QStringLiteral("prompt")).toString();
        job.intervalMs  = obj.value(QStringLiteral("intervalMs")).toVariant().toLongLong();
        job.createdAt   = obj.value(QStringLiteral("createdAt")).toVariant().toLongLong();
        job.lastFiredAt = obj.value(QStringLiteral("lastFiredAt")).toVariant().toLongLong();
        if (job.name.isEmpty() || job.intervalMs <= 0 || job.prompt.isEmpty())
            continue;
        /* Anchor next fire from lastFiredAt when present so a recurring
         * task that fired 30s before shutdown still waits the remaining
         * 30s after restart, not the full interval. */
        const qint64 anchor = job.lastFiredAt > 0 ? job.lastFiredAt : job.createdAt;
        job.nextFireAt      = anchor + job.intervalMs;
        jobs_.append(job);
        /* Bump the seq counter past any persisted L<n> id so new ids
         * never collide. */
        if (job.name.startsWith(QLatin1Char('L'))) {
            bool       ok  = false;
            const auto num = job.name.mid(1).toInt(&ok);
            if (ok && num > nameCounter_)
                nameCounter_ = num;
        }
    }
}

void QSocLoopScheduler::persist()
{
    if (projectDir_.isEmpty())
        return;
    QJsonArray arr;
    for (const auto &job : jobs_) {
        QJsonObject obj;
        obj[QStringLiteral("id")]          = job.name;
        obj[QStringLiteral("prompt")]      = job.prompt;
        obj[QStringLiteral("intervalMs")]  = QString::number(job.intervalMs);
        obj[QStringLiteral("createdAt")]   = QString::number(job.createdAt);
        obj[QStringLiteral("lastFiredAt")] = QString::number(job.lastFiredAt);
        arr.append(obj);
    }
    QJsonObject root;
    root[QStringLiteral("schema")] = kSchemaVersion;
    root[QStringLiteral("tasks")]  = arr;
    QSaveFile file(tasksPath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

bool QSocLoopScheduler::tryAcquireLock()
{
    if (projectDir_.isEmpty())
        return true;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    /* Inspect the existing lock first. A lock is "stale" when its owner
     * pid is gone OR its updatedAt is older than kLockStaleMs. The
     * pid-liveness check matters because a fresh lock from a process that
     * died of SIGKILL keeps a recent updatedAt. */
    QFile probe(lockPath());
    if (probe.exists() && probe.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(probe.readAll());
        probe.close();
        if (doc.isObject()) {
            const auto    obj         = doc.object();
            const QString existingSid = obj.value(QStringLiteral("sessionId")).toString();
            const qint64  pid         = obj.value(QStringLiteral("pid")).toVariant().toLongLong();
            const qint64 updatedAt = obj.value(QStringLiteral("updatedAt")).toVariant().toLongLong();
            const bool weAlreadyOwn = existingSid == sessionId_;
            const bool stale        = (now - updatedAt > kLockStaleMs) || !pidIsAlive(pid);
            if (!weAlreadyOwn && !stale) {
                return false;
            }
        }
    }

    /* Take or refresh the lock. Atomic write so a concurrent reader
     * sees either the old contents or the fully-written new ones. */
    QJsonObject obj;
    obj[QStringLiteral("pid")]       = QString::number(QCoreApplication::applicationPid());
    obj[QStringLiteral("sessionId")] = sessionId_;
    obj[QStringLiteral("updatedAt")] = QString::number(now);
    QSaveFile file(lockPath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    if (!file.commit())
        return false;
    lastLockRefresh_ = now;
    return true;
}

void QSocLoopScheduler::releaseLock()
{
    if (projectDir_.isEmpty())
        return;
    /* Only delete the lock if it still names us. Otherwise another session
     * already took over and the file is theirs. */
    QFile probe(lockPath());
    if (!probe.exists() || !probe.open(QIODevice::ReadOnly))
        return;
    const auto doc = QJsonDocument::fromJson(probe.readAll());
    probe.close();
    if (!doc.isObject())
        return;
    if (doc.object().value(QStringLiteral("sessionId")).toString() != sessionId_)
        return;
    QFile::remove(lockPath());
}

qint64 QSocLoopScheduler::parseInterval(const QString &token)
{
    static const QRegularExpression re(QStringLiteral("^(\\d+)([smhd])$"));
    const auto                      m = re.match(token.trimmed().toLower());
    if (!m.hasMatch())
        return 0;
    bool       ok = false;
    const auto n  = m.captured(1).toLongLong(&ok);
    if (!ok || n <= 0)
        return 0;
    const QString unit = m.captured(2);
    if (unit == "s") {
        const qint64 ms = n * kSecondMs;
        return ms < kMinMs ? kMinMs : ms;
    }
    if (unit == "m")
        return n * kMinMs;
    if (unit == "h")
        return n * kHourMs;
    if (unit == "d")
        return n * kDayMs;
    return 0;
}

QString QSocLoopScheduler::formatInterval(qint64 intervalMs)
{
    if (intervalMs <= 0)
        return QStringLiteral("?");
    if (intervalMs % kDayMs == 0)
        return QStringLiteral("%1d").arg(intervalMs / kDayMs);
    if (intervalMs % kHourMs == 0)
        return QStringLiteral("%1h").arg(intervalMs / kHourMs);
    if (intervalMs % kMinMs == 0)
        return QStringLiteral("%1m").arg(intervalMs / kMinMs);
    return QStringLiteral("%1s").arg(intervalMs / kSecondMs);
}

void QSocLoopScheduler::parseLoopArgs(
    const QString &args, qint64 defaultMs, qint64 &outIntervalMs, QString &outPrompt)
{
    const QString trimmed = args.trimmed();
    outIntervalMs         = 0;
    outPrompt.clear();

    if (trimmed.isEmpty()) {
        outIntervalMs = defaultMs;
        return;
    }

    /* Rule 1: leading token is an interval (e.g. "5m foo"). */
    const auto firstSpace = trimmed.indexOf(QRegularExpression("\\s"));
    {
        const QString head = firstSpace < 0 ? trimmed : trimmed.left(firstSpace);
        const qint64  ms   = parseInterval(head);
        if (ms > 0) {
            outIntervalMs = ms;
            outPrompt     = firstSpace < 0 ? QString() : trimmed.mid(firstSpace + 1).trimmed();
            return;
        }
    }

    /* Rule 2: trailing "every Nx" or "every N <unit-word>". The
     * unit-word form lets users write "every 5 minutes". */
    static const QRegularExpression reEvery1(QStringLiteral("(?i)\\s+every\\s+(\\d+)([smhd])\\s*$"));
    static const QRegularExpression reEvery2(QStringLiteral(
        "(?i)\\s+every\\s+(\\d+)\\s+(seconds?|secs?|minutes?|mins?|hours?|hrs?|days?"
        "|s|m|h|d)\\s*$"));
    auto                            m1 = reEvery1.match(trimmed);
    if (m1.hasMatch()) {
        const qint64 ms = parseInterval(m1.captured(1) + m1.captured(2));
        if (ms > 0) {
            outIntervalMs = ms;
            outPrompt     = trimmed.left(m1.capturedStart()).trimmed();
            return;
        }
    }
    auto m2 = reEvery2.match(trimmed);
    if (m2.hasMatch()) {
        const QString suffix = unitWordToSuffix(m2.captured(2));
        if (!suffix.isEmpty()) {
            const qint64 ms = parseInterval(m2.captured(1) + suffix);
            if (ms > 0) {
                outIntervalMs = ms;
                outPrompt     = trimmed.left(m2.capturedStart()).trimmed();
                return;
            }
        }
    }

    /* Rule 3: default. */
    outIntervalMs = defaultMs;
    outPrompt     = trimmed;
}

QString QSocLoopScheduler::allocateName()
{
    /* Short numeric ids are easier to type into /loop stop than UUIDs. */
    return QStringLiteral("L%1").arg(++nameCounter_);
}

QString QSocLoopScheduler::addJob(const QString &prompt, qint64 intervalMs)
{
    Job j;
    j.name        = allocateName();
    j.prompt      = prompt;
    j.intervalMs  = intervalMs;
    j.createdAt   = QDateTime::currentMSecsSinceEpoch();
    j.lastFiredAt = 0;
    j.nextFireAt  = j.createdAt + intervalMs;
    jobs_.append(j);
    persist();
    return j.name;
}

bool QSocLoopScheduler::removeJob(const QString &name)
{
    for (int i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i].name == name) {
            jobs_.removeAt(i);
            persist();
            return true;
        }
    }
    return false;
}

void QSocLoopScheduler::clearJobs()
{
    jobs_.clear();
    persist();
}

QList<QSocLoopScheduler::Job> QSocLoopScheduler::listJobs() const
{
    return jobs_;
}

void QSocLoopScheduler::tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    /* When bound to a project dir: only the lock owner fires. Non-owners
     * silently re-probe the lock periodically so a crashed owner is taken
     * over without restarting the surviving session. */
    if (!projectDir_.isEmpty()) {
        if (!isOwner_) {
            if (tryAcquireLock()) {
                isOwner_ = true;
                /* Re-load tasks: while we were a non-owner the previous
                 * owner may have added or removed jobs in the file. */
                jobs_.clear();
                nameCounter_ = 0;
                loadFromDisk();
            } else {
                return;
            }
        }
        if (now - lastLockRefresh_ >= kLockRefreshMs) {
            if (!tryAcquireLock()) {
                /* Another session took over while we were running.
                 * Stand down until the next probe. */
                isOwner_ = false;
                return;
            }
        }
    }

    if (jobs_.isEmpty())
        return;

    /* Snapshot ids up front: emit() may re-enter (e.g. fire path calls back
     * into addJob/removeJob via /loop dispatch). Always reschedule before
     * emit so a re-entrant tick cannot double-fire. */
    QList<QPair<QString, QString>> due;
    bool                           anyFired = false;
    for (auto &j : jobs_) {
        if (now < j.nextFireAt)
            continue;
        j.lastFiredAt = now;
        j.nextFireAt  = now + j.intervalMs;
        anyFired      = true;
        due.append(qMakePair(j.prompt, j.name));
    }
    if (anyFired)
        persist();
    for (const auto &p : due)
        emit promptDue(p.first, p.second);
}
