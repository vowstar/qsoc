// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccron.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <array>
#include <optional>

namespace QSocCron {

namespace {

struct FieldRange
{
    int min;
    int max;
};

constexpr std::array<FieldRange, 5> kFieldRanges = {{
    {0, 59}, /* minute */
    {0, 23}, /* hour */
    {1, 31}, /* day-of-month */
    {1, 12}, /* month */
    {0, 6},  /* day-of-week (0=Sunday; 7 accepted as alias) */
}};

struct Fields
{
    QSet<int> minute;
    QSet<int> hour;
    QSet<int> dom;
    QSet<int> month;
    QSet<int> dow;
    bool      domWild = false;
    bool      dowWild = false;
};

/* Parse a single comma-separated field into a sorted set of matching
 * values. Returns nullopt on syntax error or out-of-range numbers. */
std::optional<QSet<int>> expandField(const QString &field, const FieldRange &range)
{
    static const QRegularExpression reStep(QStringLiteral(R"(^\*(?:/(\d+))?$)"));
    static const QRegularExpression reRange(QStringLiteral(R"(^(\d+)-(\d+)(?:/(\d+))?$)"));
    static const QRegularExpression reSingle(QStringLiteral(R"(^\d+$)"));

    const bool isDow = (range.min == 0 && range.max == 6);
    QSet<int>  out;

    const QStringList parts = field.split(QChar::fromLatin1(','), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return std::nullopt;

    for (const QString &part : parts) {
        const auto stepMatch = reStep.match(part);
        if (stepMatch.hasMatch()) {
            int        step    = 1;
            const auto stepCap = stepMatch.captured(1);
            if (!stepCap.isEmpty()) {
                bool ok = false;
                step    = stepCap.toInt(&ok);
                if (!ok || step < 1)
                    return std::nullopt;
            }
            for (int i = range.min; i <= range.max; i += step)
                out.insert(i);
            continue;
        }

        const auto rangeMatch = reRange.match(part);
        if (rangeMatch.hasMatch()) {
            bool      okLo = false, okHi = false, okStep = true;
            const int lo   = rangeMatch.captured(1).toInt(&okLo);
            const int hi   = rangeMatch.captured(2).toInt(&okHi);
            int       step = 1;
            if (!rangeMatch.captured(3).isEmpty())
                step = rangeMatch.captured(3).toInt(&okStep);
            if (!okLo || !okHi || !okStep || step < 1 || lo > hi || lo < range.min)
                return std::nullopt;
            const int effMax = isDow ? 7 : range.max;
            if (hi > effMax)
                return std::nullopt;
            for (int i = lo; i <= hi; i += step)
                out.insert((isDow && i == 7) ? 0 : i);
            continue;
        }

        const auto singleMatch = reSingle.match(part);
        if (singleMatch.hasMatch()) {
            bool ok = false;
            int  n  = part.toInt(&ok);
            if (!ok)
                return std::nullopt;
            if (isDow && n == 7)
                n = 0;
            if (n < range.min || n > range.max)
                return std::nullopt;
            out.insert(n);
            continue;
        }

        return std::nullopt;
    }

    if (out.isEmpty())
        return std::nullopt;
    return out;
}

std::optional<Fields> parseExpression(const QString &expr)
{
    const QStringList parts
        = expr.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() != 5)
        return std::nullopt;

    Fields f;
    auto   m1 = expandField(parts.at(0), kFieldRanges.at(0));
    auto   m2 = expandField(parts.at(1), kFieldRanges.at(1));
    auto   m3 = expandField(parts.at(2), kFieldRanges.at(2));
    auto   m4 = expandField(parts.at(3), kFieldRanges.at(3));
    auto   m5 = expandField(parts.at(4), kFieldRanges.at(4));
    if (!m1 || !m2 || !m3 || !m4 || !m5)
        return std::nullopt;

    f.minute  = *m1;
    f.hour    = *m2;
    f.dom     = *m3;
    f.month   = *m4;
    f.dow     = *m5;
    f.domWild = (f.dom.size() == 31);
    f.dowWild = (f.dow.size() == 7);
    return f;
}

} /* namespace */

bool isValid(const QString &expr)
{
    return parseExpression(expr).has_value();
}

qint64 nextRunMs(const QString &expr, qint64 fromMs)
{
    const auto fields = parseExpression(expr);
    if (!fields)
        return 0;
    const auto &f = *fields;

    /* Round strictly forward to the next whole minute. cron has minute
     * resolution, so any second within `fromMs` cannot be its own match. */
    QDateTime t = QDateTime::fromMSecsSinceEpoch(fromMs).toLocalTime();
    t           = t.addMSecs(-(t.time().msec()));
    t           = t.addSecs(60 - t.time().second());

    constexpr int kMaxIter = 366 * 24 * 60;
    for (int i = 0; i < kMaxIter; ++i) {
        const QDate date  = t.date();
        const QTime time  = t.time();
        const int   month = date.month();
        if (!f.month.contains(month)) {
            /* Jump to the 1st of the next month. */
            QDate nextMonth = (month == 12) ? QDate(date.year() + 1, 1, 1)
                                            : QDate(date.year(), month + 1, 1);
            t               = QDateTime(nextMonth, QTime(0, 0));
            continue;
        }

        const int  dom        = date.day();
        const int  dow        = date.dayOfWeek() % 7; /* Qt: Mon=1..Sun=7 -> 0..6 (Sun=0) */
        const bool dayMatches = (f.domWild && f.dowWild) ? true
                                : f.domWild              ? f.dow.contains(dow)
                                : f.dowWild ? f.dom.contains(dom)
                                            : (f.dom.contains(dom) || f.dow.contains(dow));
        if (!dayMatches) {
            t = QDateTime(date.addDays(1), QTime(0, 0));
            continue;
        }

        if (!f.hour.contains(time.hour())) {
            t = QDateTime(date, QTime(time.hour(), 0)).addSecs(60 * 60);
            continue;
        }

        if (!f.minute.contains(time.minute())) {
            t = t.addSecs(60);
            continue;
        }

        return t.toMSecsSinceEpoch();
    }
    return 0;
}

QString intervalToCron(const QString &token)
{
    static const QRegularExpression re(QStringLiteral("^(\\d+)([smhd])$"));
    const auto                      m = re.match(token.trimmed().toLower());
    if (!m.hasMatch())
        return QString();
    bool       ok   = false;
    qint64     n    = m.captured(1).toLongLong(&ok);
    const auto unit = m.captured(2);
    if (!ok || n <= 0)
        return QString();

    /* Seconds round up to whole minutes (>=1m), then fall through to the
     * minute branch. Anything finer than a minute makes no sense at the
     * scheduler tick rate. */
    if (unit == QLatin1String("s")) {
        n = (n + 59) / 60;
        if (n < 1)
            n = 1;
        /* re-route as minutes */
        if (n <= 59) {
            if (60 % n != 0)
                return QString();
            return QStringLiteral("*/%1 * * * *").arg(n);
        }
        /* fall through to >= 60 handling */
    }

    if (unit == QLatin1String("m") || unit == QLatin1String("s")) {
        if (n <= 59) {
            if (60 % n != 0)
                return QString();
            return QStringLiteral("*/%1 * * * *").arg(n);
        }
        if (n % 60 != 0)
            return QString();
        const qint64 hours = n / 60;
        if (hours > 23 || 24 % hours != 0)
            return QString();
        return QStringLiteral("0 */%1 * * *").arg(hours);
    }

    if (unit == QLatin1String("h")) {
        if (n > 23 || 24 % n != 0)
            return QString();
        return QStringLiteral("0 */%1 * * *").arg(n);
    }

    if (unit == QLatin1String("d")) {
        if (n < 1)
            return QString();
        return QStringLiteral("0 0 */%1 * *").arg(n);
    }

    return QString();
}

QString cronToHuman(const QString &expr)
{
    const QStringList parts
        = expr.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() != 5)
        return expr;
    const QString &m   = parts.at(0);
    const QString &h   = parts.at(1);
    const QString &dom = parts.at(2);
    const QString &mo  = parts.at(3);
    const QString &dow = parts.at(4);

    static const QRegularExpression reStarSlash(QStringLiteral("^\\*/(\\d+)$"));

    /* Every N minutes */
    {
        const auto sm = reStarSlash.match(m);
        if (sm.hasMatch() && h == "*" && dom == "*" && mo == "*" && dow == "*") {
            const int n = sm.captured(1).toInt();
            return n == 1 ? QStringLiteral("Every minute") : QStringLiteral("Every %1m").arg(n);
        }
    }

    /* Every N hours at minute X */
    {
        const auto sh    = reStarSlash.match(h);
        bool       okMin = false;
        const int  mn    = m.toInt(&okMin);
        if (sh.hasMatch() && okMin && dom == "*" && mo == "*" && dow == "*") {
            const int n = sh.captured(1).toInt();
            return QStringLiteral("Every %1h at :%2").arg(n).arg(mn, 2, 10, QChar::fromLatin1('0'));
        }
    }

    /* Daily at HH:MM (interval >= 1d) */
    {
        bool       okMin = false, okHr = false;
        const int  mn = m.toInt(&okMin);
        const int  hr = h.toInt(&okHr);
        const auto sd = reStarSlash.match(dom);
        if (okMin && okHr && sd.hasMatch() && mo == "*" && dow == "*") {
            const int     n    = sd.captured(1).toInt();
            const QString time = QStringLiteral("%1:%2")
                                     .arg(hr, 2, 10, QChar::fromLatin1('0'))
                                     .arg(mn, 2, 10, QChar::fromLatin1('0'));
            return n == 1 ? QStringLiteral("Daily %1").arg(time)
                          : QStringLiteral("Every %1d at %2").arg(n).arg(time);
        }
    }

    /* One-shot pinned: numeric m/h/dom/mo, dow=* */
    {
        bool      okMin = false, okHr = false, okDom = false, okMo = false;
        const int mn = m.toInt(&okMin);
        const int hr = h.toInt(&okHr);
        const int dd = dom.toInt(&okDom);
        const int mm = mo.toInt(&okMo);
        if (okMin && okHr && okDom && okMo && dow == "*") {
            return QStringLiteral("Once %1-%2 %3:%4")
                .arg(mm, 2, 10, QChar::fromLatin1('0'))
                .arg(dd, 2, 10, QChar::fromLatin1('0'))
                .arg(hr, 2, 10, QChar::fromLatin1('0'))
                .arg(mn, 2, 10, QChar::fromLatin1('0'));
        }
    }

    return expr;
}

} /* namespace QSocCron */
