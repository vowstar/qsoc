// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSTATICMARKDOWN_H
#define QSTATICMARKDOWN_H

#include <nlohmann/json.hpp>
#include <QMetaEnum>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

using json = nlohmann::json;

/**
 * @brief The QStaticMarkdown class.
 * @details This class provides utility functions for rendering Markdown documents
 *          with proper formatting using the Inja template engine.
 */
class QStaticMarkdown : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Text alignment options for table cells
     */
    enum class Alignment {
        Left,   /**< Left-aligned text */
        Center, /**< Center-aligned text */
        Right   /**< Right-aligned text */
    };
    Q_ENUM(Alignment)

    /**
     * @brief Get the static instance of this object.
     * @details This function will return the static instance of this object.
     * @return The static instance of this object.
     */
    static QStaticMarkdown &instance()
    {
        static QStaticMarkdown instance;
        return instance;
    }

    /**
     * @brief Default destructor for QStaticMarkdown.
     * @details Cleanup and release resources.
     */
    ~QStaticMarkdown() = default;

public slots:
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

    /**
     * @brief Constructor.
     * @details This is a private constructor for this class to prevent
     *          instantiation. Making the constructor private ensures that no
     *          objects of this class can be created from outside the class,
     *          enforcing a static-only usage pattern.
     */
    QStaticMarkdown() {}
};

#endif // QSTATICMARKDOWN_H
