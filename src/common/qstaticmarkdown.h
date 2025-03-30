// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSTATICMARKDOWN_H
#define QSTATICMARKDOWN_H

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

/**
 * @brief The QStaticMarkdown class.
 * @details This class provides utility functions for rendering Markdown documents
 *          with proper formatting using the Inja template engine.
 */
class QStaticMarkdown
{
public:
    /**
     * @brief Text alignment options for table cells
     */
    enum class Alignment {
        Left,   /**< Left-aligned text */
        Center, /**< Center-aligned text */
        Right   /**< Right-aligned text */
    };

    /**
     * @brief Default constructor for QStaticMarkdown.
     * @details Initializes a new instance of the QStaticMarkdown class.
     */
    explicit QStaticMarkdown() = default;

    /**
     * @brief Default destructor for QStaticMarkdown.
     * @details Cleanup and release resources.
     */
    ~QStaticMarkdown() = default;

    /**
     * @brief Render a JSON object as a formatted Markdown table.
     * @details Converts a JSON object containing groups of data into a properly aligned
     *          Markdown table. This function is designed to work with LLM-generated
     *          JSON responses with a specific structure.
     *
     * @param jsonResponse The JSON response string to format
     * @return QString The formatted Markdown table string
     */
    static QString formatJsonToMarkdownTable(const QString &jsonResponse);

    /**
     * @brief Generate a Markdown table from column headers and data rows.
     * @details Creates a formatted Markdown table with proper column alignment based on
     *          the content width of each column.
     *
     * @param headers A list of column header strings
     * @param rows A vector of rows, where each row is a list of cell values
     * @param defaultAlignment The default alignment to use for all columns
     * @return QString The formatted Markdown table string
     */
    static QString renderTable(
        const QStringList          &headers,
        const QVector<QStringList> &rows,
        Alignment                   defaultAlignment = Alignment::Left);

    /**
     * @brief Pads text with spaces according to the specified alignment within a given width
     *
     * @param text The text to pad
     * @param width The total width to pad to
     * @param alignment The alignment to use (left, center, or right)
     * @return Padded text
     */
    static QString padText(const QString &text, int width, Alignment alignment = Alignment::Left);

    /**
     * @brief Convert Alignment enum to string representation
     *
     * @param alignment The alignment enum value
     * @return QString The string representation ("left", "center", or "right")
     */
    static QString alignmentToString(Alignment alignment);

private:
    /**
     * @brief Calculate the required width for each column in the table.
     * @details Determines the optimal column width by examining the content
     *          of both headers and data cells.
     *
     * @param headers The column headers
     * @param rows The table data rows
     * @return QVector<int> A vector containing the width for each column
     */
    static QVector<int> calculateColumnWidths(
        const QStringList &headers, const QVector<QStringList> &rows);

    /**
     * @brief Create a separator line for the Markdown table.
     * @details Generates the line of dashes and colons that separates the header row
     *          from the data rows in a Markdown table, ensuring proper alignment.
     *
     * @param columnWidths The width of each column
     * @param alignments The alignment type for each column
     * @return QString The formatted separator line
     */
    static QString createSeparatorLine(
        const QVector<int> &columnWidths, const QVector<Alignment> &alignments);
};

#endif // QSTATICMARKDOWN_H
