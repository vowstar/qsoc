// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsoctablechart.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <optional>

namespace QSocTableChart {

namespace {

constexpr int kMinRows    = 3;
constexpr int kMaxRows    = 24;
constexpr int kMaxSeries  = 4;
constexpr int kChartWidth = 640;
constexpr int kLabelWidth = 150;
constexpr int kValueWidth = 70;
constexpr int kBarHeight  = 14;
constexpr int kBarGap     = 10;
constexpr int kPlotHeight = 180;
/* Series further apart than this do not share a scale. */
constexpr double kMaxSeriesRatio = 100.0;

struct Table
{
    QStringList        header;
    QList<QStringList> rows;
};

struct Series
{
    QString       name;
    QList<double> values;
    QStringList   labels;
};

QStringList cellsOf(const QString &line)
{
    QString text = line.trimmed();
    if (text.startsWith(QLatin1Char('|')))
        text.remove(0, 1);
    if (text.endsWith(QLatin1Char('|')))
        text.chop(1);
    QStringList cells = text.split(QLatin1Char('|'));
    for (QString &cell : cells) {
        cell = cell.trimmed();
        cell.remove(QStringLiteral("**"));
        cell.remove(QLatin1Char('`'));
    }
    return cells;
}

bool isDelimiter(const QString &line)
{
    static const QRegularExpression delimiter(
        QStringLiteral(R"(^\s*\|?(\s*:?-+:?\s*\|)+\s*:?-*:?\s*$)"));
    return delimiter.match(line).hasMatch();
}

QList<Table> tablesIn(const QString &markdown)
{
    QList<Table>      tables;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (qsizetype index = 0; index + 1 < lines.size(); ++index) {
        if (!lines[index].contains(QLatin1Char('|')) || !isDelimiter(lines[index + 1]))
            continue;
        Table table;
        table.header = cellsOf(lines[index]);
        index += 2;
        while (index < lines.size() && lines[index].contains(QLatin1Char('|'))) {
            const QStringList cells = cellsOf(lines[index]);
            if (cells.size() == table.header.size())
                table.rows.append(cells);
            ++index;
        }
        tables.append(table);
    }
    return tables;
}

/* A value with one optional unit; the unit must match down the column. */
std::optional<double> numberIn(const QString &cell, QString *unit)
{
    static const QRegularExpression number(
        QStringLiteral(R"(^([-+]?)(\d[\d,]*(?:\.\d+)?|\.\d+)\s*(%|[^\d\s.,]{1,6})?$)"));
    const auto match = number.match(cell);
    if (!match.hasMatch())
        return std::nullopt;
    QString digits = match.captured(2);
    digits.remove(QLatin1Char(','));
    *unit = match.captured(3);
    return match.captured(1) == QStringLiteral("-") ? -digits.toDouble() : digits.toDouble();
}

std::optional<Series> seriesOf(const Table &table, qsizetype column)
{
    Series  series;
    QString unit;
    series.name = table.header[column];
    for (const QStringList &row : table.rows) {
        QString    rowUnit;
        const auto value = numberIn(row[column], &rowUnit);
        if (!value || (!series.values.isEmpty() && rowUnit != unit))
            return std::nullopt;
        unit = rowUnit;
        series.values.append(*value);
        series.labels.append(row[column]);
    }
    return series;
}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

double peak(const Series &series)
{
    return *std::max_element(series.values.cbegin(), series.values.cend());
}

double trough(const Series &series)
{
    return *std::min_element(series.values.cbegin(), series.values.cend());
}

/* Largest magnitude; series far apart in it do not share a scale. */
double reach(const Series &series)
{
    return std::max(std::abs(peak(series)), std::abs(trough(series)));
}

QString legend(const QList<Series> &all)
{
    QString svg;
    for (qsizetype index = 0; all.size() > 1 && index < all.size(); ++index) {
        const int x = kLabelWidth + int(index) * 110;
        svg += QStringLiteral(
                   "<rect x=\"%1\" y=\"4\" width=\"12\" height=\"12\" fill=\"var(--c%2)\"/>")
                   .arg(x)
                   .arg(index + 1);
        svg += QStringLiteral("<text x=\"%1\" y=\"15\" fill=\"var(--muted)\">%2</text>")
                   .arg(x + 16)
                   .arg(escaped(all[index].name.left(12)));
    }
    return svg;
}

/* Values along an ordered axis: one line per series, labeled at both ends. */
QByteArray drawLines(const QStringList &names, const QList<Series> &all)
{
    double low  = 0;
    double high = 0;
    for (const Series &series : all) {
        low  = std::min(low, trough(series));
        high = std::max(high, peak(series));
    }
    const int  top    = all.size() > 1 ? 28 : 10;
    const int  height = top + kPlotHeight + 26;
    const int  left   = 50;
    const int  right  = kChartWidth - 20;
    const auto xAt    = [&](qsizetype index) {
        return left + double(right - left) * double(index) / double(names.size() - 1);
    };
    const auto yAt = [&](double value) { return top + kPlotHeight * (high - value) / (high - low); };
    QString svg = QStringLiteral("<svg viewBox=\"0 0 %1 %2\" font-size=\"12\">")
                      .arg(kChartWidth)
                      .arg(height);
    svg += legend(all);
    svg += QStringLiteral("<line x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%2\" stroke=\"var(--border)\"/>")
               .arg(left)
               .arg(yAt(0))
               .arg(right);
    svg += QStringLiteral(
               "<text x=\"%1\" y=\"%2\" text-anchor=\"end\" fill=\"var(--muted)\">%3</text>")
               .arg(left - 6)
               .arg(top + 10)
               .arg(high);
    svg += QStringLiteral(
               "<text x=\"%1\" y=\"%2\" text-anchor=\"end\" fill=\"var(--muted)\">%3</text>")
               .arg(left - 6)
               .arg(top + kPlotHeight)
               .arg(low);
    for (qsizetype index = 0; index < all.size(); ++index) {
        QString points;
        for (qsizetype row = 0; row < names.size(); ++row) {
            points += QStringLiteral("%1,%2 ").arg(xAt(row)).arg(yAt(all[index].values[row]));
            svg += QStringLiteral("<circle cx=\"%1\" cy=\"%2\" r=\"2.5\" fill=\"var(--c%3)\"/>")
                       .arg(xAt(row))
                       .arg(yAt(all[index].values[row]))
                       .arg(index + 1);
        }
        svg += QStringLiteral(
                   "<polyline points=\"%1\" fill=\"none\" stroke=\"var(--c%2)\" "
                   "stroke-width=\"2\"/>")
                   .arg(points)
                   .arg(index + 1);
    }
    svg += QStringLiteral("<text x=\"%1\" y=\"%2\" fill=\"var(--fg)\">%3</text>")
               .arg(left)
               .arg(height - 6)
               .arg(escaped(names.first().left(20)));
    svg += QStringLiteral("<text x=\"%1\" y=\"%2\" text-anchor=\"end\" fill=\"var(--fg)\">%3</text>")
               .arg(right)
               .arg(height - 6)
               .arg(escaped(names.last().left(20)));
    svg += QStringLiteral("</svg>");
    return svg.toUtf8();
}

QByteArray drawBars(const QStringList &names, const QList<Series> &all)
{
    double low  = 0;
    double high = 0;
    for (const Series &series : all) {
        low  = std::min(low, trough(series));
        high = std::max(high, peak(series));
    }
    const int    rowHeight = int(all.size()) * kBarHeight + kBarGap;
    const int    top       = all.size() > 1 ? 24 : 8;
    const int    height    = top + int(names.size()) * rowHeight;
    const double perUnit   = (kChartWidth - kLabelWidth - kValueWidth) / (high - low);
    const int    zero      = kLabelWidth + int(-low * perUnit);

    QString svg = QStringLiteral("<svg viewBox=\"0 0 %1 %2\" font-size=\"13\">")
                      .arg(kChartWidth)
                      .arg(height);
    svg += legend(all);
    for (qsizetype row = 0; row < names.size(); ++row) {
        const int y = top + int(row) * rowHeight;
        svg += QStringLiteral(
                   "<text x=\"%1\" y=\"%2\" text-anchor=\"end\" fill=\"var(--fg)\">%3</text>")
                   .arg(kLabelWidth - 8)
                   .arg(y + int(all.size()) * kBarHeight / 2 + 5)
                   .arg(escaped(names[row].left(20)));
        for (qsizetype index = 0; index < all.size(); ++index) {
            const int    barY  = y + int(index) * kBarHeight;
            const double value = all[index].values[row];
            const int    width = std::max(1, int(std::abs(value) * perUnit));
            const int    x     = value < 0 ? zero - width : zero;
            svg += QStringLiteral(
                       "<rect x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\" fill=\"var(--c%5)\"/>")
                       .arg(x)
                       .arg(barY)
                       .arg(width)
                       .arg(kBarHeight - 2)
                       .arg(index + 1);
            svg += QStringLiteral(
                       "<text x=\"%1\" y=\"%2\" font-size=\"11\" fill=\"var(--muted)\">%3</text>")
                       .arg(std::max(x + width, zero) + 4)
                       .arg(barY + kBarHeight - 3)
                       .arg(escaped(all[index].labels[row]));
        }
    }
    svg += QStringLiteral("</svg>");
    return svg.toUtf8();
}

bool isIdentifierHeader(const QString &header)
{
    static const QRegularExpression identifier(
        QStringLiteral(R"(^(id|#|no\.?|num|index|idx|addr|address)$)"),
        QRegularExpression::CaseInsensitiveOption);
    return identifier.match(header).hasMatch();
}

bool isOrderedHeader(const QString &header)
{
    static const QRegularExpression ordered(
        QStringLiteral(R"(time|date|step|run|iter|epoch|version|year|month|week|day|cycle|round)"),
        QRegularExpression::CaseInsensitiveOption);
    return ordered.match(header).hasMatch();
}

} // namespace

QByteArrayList charts(const QString &markdown)
{
    QByteArrayList result;
    for (const Table &table : tablesIn(markdown)) {
        if (table.rows.size() < kMinRows || table.rows.size() > kMaxRows || table.header.size() < 2)
            continue;
        if (isIdentifierHeader(table.header.first()))
            continue;
        QStringList names;
        for (const QStringList &row : table.rows)
            names.append(row.first());
        QString       ignoredUnit;
        const bool    numericNames = numberIn(names.first(), &ignoredUnit).has_value();
        QList<Series> all;
        for (qsizetype column = 1; column < table.header.size(); ++column) {
            const auto series = seriesOf(table, column);
            if (!series || isIdentifierHeader(series->name) || peak(*series) <= trough(*series)
                || all.size() >= kMaxSeries)
                continue;
            if (all.isEmpty()
                || (reach(all.first()) <= reach(*series) * kMaxSeriesRatio
                    && reach(*series) <= reach(all.first()) * kMaxSeriesRatio))
                all.append(*series);
        }
        if (all.isEmpty())
            continue;
        result.append(
            numericNames || isOrderedHeader(table.header.first()) ? drawLines(names, all)
                                                                  : drawBars(names, all));
    }
    return result;
}

} // namespace QSocTableChart
