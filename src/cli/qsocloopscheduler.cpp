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
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

constexpr qint64 kTickMs   = 1000;
constexpr qint64 kSecondMs = 1000;
constexpr qint64 kMinMs    = qint64{60} * kSecondMs;
constexpr qint64 kHourMs   = qint64{60} * kMinMs;
constexpr qint64 kDayMs    = qint64{24} * kHourMs;
/* QLockFile staleness: 0 means "dead owner pid is the only staleness
 * signal". A live REPL owner is never reclaimed regardless of how many
 * hours it holds the lock; a crashed owner is reclaimed on the very
 * next probe with no grace period. Qt explicitly recommends 0 for
 * locks meant to live as long as the application itself. */
constexpr int kLockStaleMs   = 0;
constexpr int kSchemaVersion = 1;

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
    timer_.setInterval(static_cast<int>(kTickMs));
    connect(&timer_, &QTimer::timeout, this, &QSocLoopScheduler::tick);
    timer_.start();
}

QSocLoopScheduler::~QSocLoopScheduler()
{
    /* releaseLock() is a no-op when there is no lockFile_, so the gate
     * doesn't need to know about owner/project state. Mirroring this
     * pattern in setProjectDir() and the dtor avoids the V2 bug where
     * a non-owner skipped releaseLock() and leaked the QLockFile. */
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
    /* Always drop any lockFile_ left over from the previous project.
     * Earlier this only ran when we were the owner, which left non-owner
     * sessions holding a QLockFile pointed at the old path; subsequent
     * tryAcquireLock() would be a no-op (lockFile_ already non-null) and
     * the new project would silently share the stale lock object. */
    releaseLock();
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
        /* recurring/enabled default to true so old loops.json files
         * (written before these fields existed) still load as before. */
        job.recurring = obj.value(QStringLiteral("recurring")).toBool(true);
        job.enabled   = obj.value(QStringLiteral("enabled")).toBool(true);
        if (job.name.isEmpty() || job.intervalMs <= 0 || job.prompt.isEmpty())
            continue;
        /* Anchor next fire from lastFiredAt when present so a recurring
         * task that fired 30s before shutdown still waits the remaining
         * 30s after restart, not the full interval. If qsoc was down for
         * longer than the interval, advance nextFireAt past `now` in
         * whole intervals so the loop resumes on cadence without
         * back-firing every missed slot at startup (the plan calls for
         * no catch-up on restart). */
        const qint64 anchor = job.lastFiredAt > 0 ? job.lastFiredAt : job.createdAt;
        const qint64 now    = QDateTime::currentMSecsSinceEpoch();
        qint64       next   = anchor + job.intervalMs;
        if (next <= now && job.intervalMs > 0) {
            const qint64 missed = ((now - anchor) / job.intervalMs) + 1;
            next                = anchor + (missed * job.intervalMs);
        }
        job.nextFireAt = next;
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

bool QSocLoopScheduler::persist()
{
    if (projectDir_.isEmpty())
        return true;
    QJsonArray arr;
    for (const auto &job : jobs_) {
        QJsonObject obj;
        obj[QStringLiteral("id")]          = job.name;
        obj[QStringLiteral("prompt")]      = job.prompt;
        obj[QStringLiteral("intervalMs")]  = QString::number(job.intervalMs);
        obj[QStringLiteral("createdAt")]   = QString::number(job.createdAt);
        obj[QStringLiteral("lastFiredAt")] = QString::number(job.lastFiredAt);
        obj[QStringLiteral("recurring")]   = job.recurring;
        obj[QStringLiteral("enabled")]     = job.enabled;
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
    /* Any successful write clears the tick-path failure latch, so a
     * user-triggered /loop add succeeding after the disk recovered also
     * silences the periodic-fire warning at next tick. */
    persistDegraded_ = false;
    return true;
}

bool QSocLoopScheduler::tryAcquireLock()
{
    if (projectDir_.isEmpty())
        return true;
    /* Lazy-construct so a single instance reuses the same QLockFile
     * across reacquire-after-loss cycles. QLockFile owns the OS-level
     * atomicity (O_EXCL on Unix, similar on Windows); we don't need to
     * read-decide-write the lock file ourselves. */
    if (!lockFile_) {
        lockFile_ = std::make_unique<QLockFile>(lockPath());
        lockFile_->setStaleLockTime(kLockStaleMs);
    }
    if (lockFile_->isLocked())
        return true;
    /* tryLock(0) is non-blocking. With staleLockTime(0) a live owner is
     * never reclaimed solely because the lock file is old; a losing
     * instance gets false here and the tick() loop probes again on the
     * next iteration. Takeover only happens when QLockFile sees the
     * previous owner's pid is gone, or when the owner unlocks normally. */
    return lockFile_->tryLock(0);
}

void QSocLoopScheduler::releaseLock()
{
    if (lockFile_ && lockFile_->isLocked()) {
        lockFile_->unlock();
    }
    lockFile_.reset();
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
        /* Plan rule: round up to whole minutes, minimum 1 minute. The
         * scheduler tick is 1s but the cadence we expose to users is
         * minute-aligned because anything finer is rarely meaningful and
         * easy to abuse. */
        const qint64 minutes = (n + 59) / 60;
        return (minutes < 1 ? 1 : minutes) * kMinMs;
    }
    if (unit == "m")
        return n * kMinMs;
    if (unit == "h")
        return n * kHourMs;
    if (unit == "d")
        return n * kDayMs;
    return 0;
}

bool QSocLoopScheduler::scheduledInputRequiresCliDispatch(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return false;
    const QChar first = trimmed.at(0);
    return first == QLatin1Char('/') || first == QLatin1Char('!');
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
    /* In durable mode, the on-disk loops.json is the source of truth.
     * A non-owner that mutates would either lose the write (the owner
     * never reloads) or stomp the owner's view at next refresh; both
     * leave the user with a UI that disagrees with reality. Refuse here
     * and let the slash dispatch surface the reason. */
    if (!projectDir_.isEmpty() && !isOwner_)
        return QString();
    Job job;
    job.name        = allocateName();
    job.prompt      = prompt;
    job.intervalMs  = intervalMs;
    job.createdAt   = QDateTime::currentMSecsSinceEpoch();
    job.lastFiredAt = 0;
    job.nextFireAt  = job.createdAt + intervalMs;
    jobs_.append(job);
    /* Roll the in-memory append back if the disk write failed; otherwise
     * the user would see the job in /loop list this session but lose it
     * after restart, and a non-owner would never see it. */
    if (!persist()) {
        jobs_.removeLast();
        --nameCounter_;
        return QString();
    }
    return job.name;
}

bool QSocLoopScheduler::removeJob(const QString &name)
{
    if (!projectDir_.isEmpty() && !isOwner_)
        return false;
    for (int i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i].name == name) {
            const Job stash = jobs_[i];
            jobs_.removeAt(i);
            if (!persist()) {
                jobs_.insert(i, stash);
                return false;
            }
            return true;
        }
    }
    return false;
}

bool QSocLoopScheduler::clearJobs()
{
    if (!projectDir_.isEmpty() && !isOwner_)
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
    /* In durable mode the owner's in-memory copy is authoritative; its
     * tick() rewrote nextFireAt mid-tick and the file may be a few ms
     * behind. A non-owner is a passive observer: reread loops.json on
     * every call. An mtime gate would skip same-second updates because
     * mtime resolution is 1s on most filesystems, and /loop list is a
     * user-driven command on a small file, not a hot path. */
    if (!projectDir_.isEmpty() && !isOwner_) {
        jobs_.clear();
        nameCounter_ = 0;
        loadFromDisk();
    }
    return jobs_;
}

void QSocLoopScheduler::tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    /* When bound to a project dir: only the QLockFile owner fires.
     * Non-owners silently re-probe each tick so a crashed owner is
     * taken over without a restart. QLockFile holds the lock for the
     * process lifetime, so no periodic refresh is needed. */
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
    }

    if (jobs_.isEmpty())
        return;

    /* Snapshot ids up front: emit() may re-enter (e.g. fire path calls back
     * into addJob/removeJob via /loop dispatch). Always reschedule before
     * emit so a re-entrant tick cannot double-fire. */
    QList<QPair<QString, QString>> due;
    bool                           anyFired = false;
    for (auto &job : jobs_) {
        if (!job.enabled || now < job.nextFireAt)
            continue;
        job.lastFiredAt = now;
        job.nextFireAt  = now + job.intervalMs;
        anyFired        = true;
        due.append(qMakePair(job.prompt, job.name));
    }
    if (anyFired && !persist() && !persistDegraded_) {
        /* Latch on first failure so REPL gets one warning per outage,
         * not one per tick. persist() clears the latch on next success. */
        persistDegraded_ = true;
        emit persistFailed(tasksPath());
    }
    /* Emit promptDue regardless of persist outcome: silently halting a
     * recurring loop because the disk hiccupped is worse than the disk
     * drift, which load-time missed-task logic absorbs at restart. */
    for (const auto &pair : due)
        emit promptDue(pair.first, pair.second);
}
