// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsocmath.h"

#include <algorithm>

namespace QSocMath {
namespace {
/* Consume complete source lines. Excluded ranges use UTF-16 offsets. */
class Scanner
{
public:
    QList<Span>         feedLine(const QString &line, const QList<Range> &excluded = {});
    std::optional<Span> unfinished() const;

private:
    int   offset        = 0;
    int   excludedIndex = 0;
    int   start         = -1;
    bool  display       = false;
    QChar closer;
};

bool escaped(const QString &text, int at)
{
    int count = 0;
    while (at > 0 && text[--at] == QLatin1Char('\\'))
        ++count;
    return count % 2 != 0;
}

bool boundary(QChar ch)
{
    return !ch.isLetterOrNumber() && ch != QLatin1Char('_') && ch != QLatin1Char('$');
}

QList<Span> Scanner::feedLine(const QString &line, const QList<Range> &excluded)
{
    QList<Span> result;
    for (int at = 0; at < line.size(); ++at) {
        const QChar ch       = line[at];
        const int   absolute = offset + at;
        if (start < 0) {
            while (excludedIndex < excluded.size() && excluded[excludedIndex].end <= absolute)
                ++excludedIndex;
            if (excludedIndex < excluded.size() && excluded[excludedIndex].begin <= absolute) {
                at = std::min(int(line.size()), excluded[excludedIndex].end - offset) - 1;
                continue;
            }
        }
        if (ch == QLatin1Char('\n') && start >= 0 && !display) {
            result.append({start, absolute, false, false});
            start  = -1;
            closer = {};
        }
        if (ch == QLatin1Char('\\') && at + 1 < line.size()) {
            const QChar next = line[at + 1];
            if (start < 0 && (next == QLatin1Char('(') || next == QLatin1Char('['))
                && !escaped(line, at)) {
                start   = absolute;
                display = next == QLatin1Char('[');
                closer  = display ? QLatin1Char(']') : QLatin1Char(')');
                ++at;
                continue;
            }
            if (start >= 0 && !closer.isNull() && next == closer && !escaped(line, at)) {
                result.append({start, absolute + 2, display, true});
                start  = -1;
                closer = {};
                ++at;
                continue;
            }
        }
        if (!closer.isNull())
            continue;
        if (ch != QLatin1Char('$') || escaped(line, at))
            continue;
        int dollars = 1;
        while (at + dollars < line.size() && line[at + dollars] == ch)
            ++dollars;
        if (dollars > 2) {
            at += dollars - 1;
            continue;
        }
        const bool pair = dollars == 2;
        if (start >= 0) {
            if (display && pair) {
                result.append({start, absolute + 2, true, true});
                start = -1;
                ++at;
            } else if (
                !display && !pair && at > 0 && !line[at - 1].isSpace()
                && (at + 1 == line.size() || boundary(line[at + 1]))) {
                result.append({start, absolute + 1, false, true});
                start = -1;
            } else if (pair) {
                ++at;
            }
            continue;
        }
        if (at > 0 && !boundary(line[at - 1]))
            continue;
        if (!pair
            && (at + 1 == line.size() || line[at + 1].isSpace()
                || QStringLiteral("{(?").contains(line[at + 1])))
            continue;
        if (!pair && line[at + 1].isLetterOrNumber()) {
            int close = line.indexOf(QLatin1Char('$'), at + 1);
            while (close >= 0 && escaped(line, close))
                close = line.indexOf(QLatin1Char('$'), close + 1);
            const bool closes = close > at + 1 && !line[close - 1].isSpace()
                                && (close + 1 == line.size() || boundary(line[close + 1]));
            if (!closes) {
                if (line[at + 1].isDigit())
                    continue;
                int end = at + 1;
                while (end < line.size()
                       && (line[end].isLetterOrNumber() || line[end] == QLatin1Char('_')))
                    ++end;
                if (end == line.size() || line[end].isSpace()
                    || QStringLiteral(".,:;!").contains(line[end])) {
                    result.append({absolute, offset + end, false, false});
                    at = end - 1;
                    continue;
                }
            }
        }
        start   = absolute;
        display = pair;
        if (pair)
            ++at;
    }
    offset += line.size();
    return result;
}

std::optional<Span> Scanner::unfinished() const
{
    return start >= 0 ? std::optional(Span{start, offset, display, false}) : std::nullopt;
}

} // namespace

QList<Span> spans(const QString &source, const QList<Range> &excluded)
{
    Scanner     scanner;
    QList<Span> result;
    int         start = 0;
    while (start < source.size()) {
        int end = source.indexOf(QLatin1Char('\n'), start);
        end     = end < 0 ? source.size() : end + 1;
        result.append(scanner.feedLine(source.mid(start, end - start), excluded));
        start = end;
    }
    if (auto tail = scanner.unfinished())
        result.append(*tail);
    return result;
}

QString body(const QString &source, const Span &span)
{
    const int delimiter = span.display || source.mid(span.begin, 2) == QStringLiteral("\\(") ? 2
                                                                                             : 1;
    QString   content   = source.mid(span.begin + delimiter, span.end - span.begin - 2 * delimiter);
    if (!span.display || !content.contains(QLatin1Char('\n')))
        return content;
    const int     lineStart  = source.lastIndexOf(QLatin1Char('\n'), span.begin) + 1;
    const QString prefix     = source.mid(lineStart, span.begin - lineStart);
    int           quoteCount = 0;
    int           prefixAt   = 0;
    while (prefixAt < prefix.size()) {
        if (prefix[prefixAt].isSpace()) {
            ++prefixAt;
            continue;
        }
        if (prefix[prefixAt] == QLatin1Char('>')) {
            ++quoteCount;
            ++prefixAt;
            continue;
        }
        int markerEnd = prefixAt;
        if (QStringLiteral("-*+").contains(prefix[markerEnd])) {
            ++markerEnd;
        } else {
            while (markerEnd < prefix.size() && prefix[markerEnd].isDigit())
                ++markerEnd;
            if (markerEnd == prefixAt || markerEnd == prefix.size()
                || !QStringLiteral(".)").contains(prefix[markerEnd]))
                break;
            ++markerEnd;
        }
        if (markerEnd == prefix.size() || !prefix[markerEnd].isSpace())
            break;
        prefixAt = markerEnd + 1;
    }
    QStringList lines = content.split(QLatin1Char('\n'));
    for (int idx = 1; idx < lines.size(); ++idx) {
        QString &line = lines[idx];
        int      at   = 0;
        for (int quote = 0; quote < quoteCount; ++quote) {
            while (at < line.size() && line[at] == QLatin1Char(' '))
                ++at;
            if (at < line.size() && line[at] == QLatin1Char('>'))
                ++at;
        }
        line = line.mid(at);
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace QSocMath
