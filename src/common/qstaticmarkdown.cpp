// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qstaticmarkdown.h"
#include <algorithm>

QString QStaticMarkdown::renderTable(
    const QStringList          &headers,
    const QVector<QStringList> &rows,
    QStaticMarkdown::Alignment  defaultAlignment)
{
    /* Calculate column widths based on content */
    QVector<int> columnWidths = calculateColumnWidths(headers, rows);

    /* Define column alignments (all columns use the default alignment) */
    QVector<Alignment> alignments(headers.size(), defaultAlignment);

    QString table;

    /* Header row */
    for (int i = 0; i < headers.size(); ++i) {
        const QString paddedHeader = padText(headers[i], columnWidths[i], alignments[i]);
        table += "|" + paddedHeader;
    }
    table += "|\n";

    /* Separator row */
    table += createSeparatorLine(columnWidths, alignments) + "\n";

    /* Data rows */
    for (const auto &row : rows) {
        for (int i = 0; i < row.size() && i < headers.size(); ++i) {
            const QString paddedCell = padText(row[i], columnWidths[i], alignments[i]);
            table += "|" + paddedCell;
        }
        table += "|\n";
    }

    return table;
}

QVector<int> QStaticMarkdown::calculateColumnWidths(
    const QStringList &headers, const QVector<QStringList> &rows)
{
    const auto   columnCount = static_cast<int>(headers.size());
    QVector<int> widths(columnCount, 0);

    /* Check header widths */
    for (int i = 0; i < columnCount; ++i) {
        widths[i] = std::max(widths[i], static_cast<int>(headers[i].length()));
    }

    /* Check data widths */
    for (const QStringList &row : rows) {
        for (int i = 0; i < row.size() && i < columnCount; ++i) {
            widths[i] = std::max(widths[i], static_cast<int>(row[i].length()));
        }
    }

    /* Add padding for better readability */
    for (int i = 0; i < columnCount; ++i) {
        /* Add 2 spaces padding (one on each side) */
        widths[i] += 2;
    }

    return widths;
}

QString QStaticMarkdown::createSeparatorLine(
    const QVector<int> &columnWidths, const QVector<Alignment> &alignments)
{
    QString separator;

    for (int i = 0; i < columnWidths.size(); ++i) {
        const Alignment align = i < alignments.size() ? alignments[i] : Alignment::Left;
        const int       width = columnWidths[i];

        if (align == Alignment::Left) {
            separator += "|:" + QString(width - 1, '-');
        } else if (align == Alignment::Right) {
            separator += "|" + QString(width - 1, '-') + ":";
        } else {
            /* Default is center alignment */
            separator += "|:" + QString(width - 2, '-') + ":";
        }
    }

    separator += "|";
    return separator;
}

QString QStaticMarkdown::padText(const QString &text, int width, QStaticMarkdown::Alignment alignment)
{
    const int padding = std::max(0, width - static_cast<int>(text.length()));

    if (alignment == Alignment::Left) {
        /* Left alignment: spaces on the right */
        return text + QString(padding, ' ');
    }

    if (alignment == Alignment::Right) {
        /* Right alignment: spaces on the left */
        return QString(padding, ' ') + text;
    }

    /* Center alignment: spaces evenly distributed */
    const int leftPad  = padding / 2;
    const int rightPad = padding - leftPad;
    return QString(leftPad, ' ') + text + QString(rightPad, ' ');
}
