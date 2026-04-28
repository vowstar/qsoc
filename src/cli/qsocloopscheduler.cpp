// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocloopscheduler.h"

#include <QDateTime>
#include <QRegularExpression>

namespace {

constexpr qint64 kTickMs   = 1000;
constexpr qint64 kSecondMs = 1000;
constexpr qint64 kMinMs    = qint64{60} * kSecondMs;
constexpr qint64 kHourMs   = qint64{60} * kMinMs;
constexpr qint64 kDayMs    = qint64{24} * kHourMs;

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
    return j.name;
}

bool QSocLoopScheduler::removeJob(const QString &name)
{
    for (int i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i].name == name) {
            jobs_.removeAt(i);
            return true;
        }
    }
    return false;
}

void QSocLoopScheduler::clearJobs()
{
    jobs_.clear();
}

QList<QSocLoopScheduler::Job> QSocLoopScheduler::listJobs() const
{
    return jobs_;
}

void QSocLoopScheduler::tick()
{
    if (jobs_.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    /* Snapshot ids up front: emit() may re-enter (e.g. fire path calls back
     * into addJob/removeJob via /loop dispatch). Always reschedule before
     * emit so a re-entrant tick cannot double-fire. */
    QList<QPair<QString, QString>> due;
    for (auto &j : jobs_) {
        if (now < j.nextFireAt)
            continue;
        j.lastFiredAt = now;
        j.nextFireAt  = now + j.intervalMs;
        due.append(qMakePair(j.prompt, j.name));
    }
    for (const auto &p : due)
        emit promptDue(p.first, p.second);
}
