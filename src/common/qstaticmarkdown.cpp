// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qstaticmarkdown.h"
#include <algorithm>
#include <inja/inja.hpp>
#include <nlohmann/json.hpp>
#include <QDebug>

QString QStaticMarkdown::formatJsonToMarkdownTable(const QString &jsonResponse)
{
    /* Try to parse the JSON response */
    QJsonDocument doc = QJsonDocument::fromJson(jsonResponse.toUtf8());
    if (doc.isNull()) {
        qWarning() << "Failed to parse JSON response";
        return jsonResponse; /* Return original response if parsing fails */
    }

    QJsonObject root = doc.object();
    if (!root.contains("groups") || !root["groups"].isArray()) {
        qWarning() << "Invalid JSON structure: missing or invalid 'groups' array";
        return jsonResponse;
    }

    QJsonArray groups = root["groups"].toArray();
    if (groups.isEmpty()) {
        return "No potential bus interface groups found.";
    }

    /* Define table headers */
    QStringList headers
        = {"Group Name",
           "Type",
           "Data Width",
           "Address Width",
           "ID Width",
           "Burst Length",
           "Write",
           "Read"};

    /* Build table rows from JSON data */
    QVector<QStringList> rows;
    for (const QJsonValue &groupValue : groups) {
        QJsonObject group = groupValue.toObject();

        /* Extract values with fallbacks */
        QString name    = group["name"].toString();
        QString type    = group["type"].toString();
        QString wData   = group["wData"].toString();
        QString wAddr   = group["wAddr"].toString();
        QString wID     = group["wID"].toString();
        QString wLen    = group["wLen"].toString();
        bool    enWrite = group["enWrite"].toBool();
        bool    enRead  = group["enRead"].toBool();

        /* Add row to table data */
        rows.append({name, type, wData, wAddr, wID, wLen, enWrite ? "✓" : "✗", enRead ? "✓" : "✗"});
    }

    /* Render the markdown table using Inja templates */
    return renderTable(headers, rows);
}

QString QStaticMarkdown::renderTable(const QStringList &headers, const QVector<QStringList> &rows)
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

    /* Define column alignments (center alignment for all columns) */
    QVector<QString> alignments(headers.size(), "center");

    /* Create Inja template environment */
    inja::Environment env;

    /* Add padding function for proper alignment */
    env.add_callback("pad", 2, [](inja::Arguments &args) {
        std::string text  = args.at(0)->get<std::string>();
        int         width = args.at(1)->get<int>();

        /* Calculate padding needed */
        int padding  = std::max(0, width - static_cast<int>(text.length()));
        int leftPad  = padding / 2;
        int rightPad = padding - leftPad;

        /* Apply padding */
        return std::string(leftPad, ' ') + text + std::string(rightPad, ' ');
    });

    /* Create a manual table string instead of using Inja template which has issues with pipe characters */
    QString table;

    /* Header row */
    for (int i = 0; i < headers.size(); ++i) {
        QString paddedHeader = padText(headers[i], columnWidths[i]);
        table += "|" + paddedHeader;
    }
    table += "|\n";

    /* Separator row */
    table += createSeparatorLine(columnWidths, alignments) + "\n";

    /* Data rows */
    for (int j = 0; j < rows.size(); ++j) {
        const QStringList &row = rows[j];
        for (int i = 0; i < row.size() && i < headers.size(); ++i) {
            QString paddedCell = padText(row[i], columnWidths[i]);
            table += "|" + paddedCell;
        }
        table += "|\n";
    }

    return table;
}

QVector<int> QStaticMarkdown::calculateColumnWidths(
    const QStringList &headers, const QVector<QStringList> &rows)
{
    int          columnCount = headers.size();
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

    /* Add padding for better readability (reduced from 2 to 1) */
    for (int i = 0; i < columnCount; ++i) {
        widths[i] += 2; /* Add 2 spaces padding (one on each side) */
    }

    return widths;
}

QString QStaticMarkdown::createSeparatorLine(
    const QVector<int> &columnWidths, const QVector<QString> &alignment)
{
    QString separator;

    for (int i = 0; i < columnWidths.size(); ++i) {
        QString align = i < alignment.size() ? alignment[i].toLower() : "center";
        int     width = columnWidths[i];

        if (align == "left") {
            separator += "|:" + QString(width - 1, '-');
        } else if (align == "right") {
            separator += "|" + QString(width - 1, '-') + ":";
        } else {
            /* Default is center alignment */
            /* Use exact same width calculation as in the template */
            separator += "|:" + QString(width - 2, '-') + ":";
        }
    }

    separator += "|";
    return separator;
}

QString QStaticMarkdown::padText(const QString &text, int width)
{
    int padding  = std::max(0, width - static_cast<int>(text.length()));
    int leftPad  = padding / 2;
    int rightPad = padding - leftPad;

    return QString(leftPad, ' ') + text + QString(rightPad, ' ');
}