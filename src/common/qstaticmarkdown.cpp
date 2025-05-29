// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qstaticmarkdown.h"
#include <algorithm>
#include <inja/inja.hpp>
#include <nlohmann/json.hpp>
#include <QDebug>

QString QStaticMarkdown::renderTable(
    const QStringList          &headers,
    const QVector<QStringList> &rows,
    QStaticMarkdown::Alignment  defaultAlignment)
{
    /* Calculate column widths based on content */
    QVector<int> columnWidths = calculateColumnWidths(headers, rows);

    /* Convert data to nlohmann::json for Inja template */
    nlohmann::json data;

    /* Add columns with their headers and widths */
    data["columns"] = nlohmann::json::array();
    for (int i = 0; i < headers.size(); ++i) {
        nlohmann::json column;
        column["name"]  = headers[i].toStdString();
        column["width"] = columnWidths[i];
        data["columns"].push_back(column);
    }

    /* Add rows with their cell values */
    data["rows"] = nlohmann::json::array();
    for (const QStringList &row : rows) {
        nlohmann::json jsonRow = nlohmann::json::array();
        for (int i = 0; i < row.size() && i < headers.size(); ++i) {
            jsonRow.push_back(row[i].toStdString());
        }
        data["rows"].push_back(jsonRow);
    }

    /* Define column alignments (all columns use the default alignment) */
    QVector<Alignment> alignments(headers.size(), defaultAlignment);

    /* Create Inja template environment */
    inja::Environment env;

    /* Disable line statements */
    env.set_line_statement("");

    /* Add padding function for proper alignment */
    env.add_callback("pad", 3, [](inja::Arguments &args) {
        const std::string text  = args.at(0)->get<std::string>();
        const int         width = args.at(1)->get<int>();
        const std::string align = args.at(2)->get<std::string>();

        /* Calculate padding needed */
        const int padding = std::max(0, width - static_cast<int>(text.length()));

        if (align == "left") {
            return text + std::string(padding, ' ');
        }

        if (align == "right") {
            return std::string(padding, ' ') + text;
        }

        /* Center alignment */
        const int leftPad  = padding / 2;
        const int rightPad = padding - leftPad;
        return std::string(leftPad, ' ') + text + std::string(rightPad, ' ');
    });

    /* Create a manual table string instead of using Inja template which has issues with pipe characters */
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
