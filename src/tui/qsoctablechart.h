// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTABLECHART_H
#define QSOCTABLECHART_H

#include <QByteArrayList>
#include <QString>

/**
 * @brief Charts for numeric Markdown tables.
 * @details A table qualifies with at least three rows and columns of comparable
 *          numbers that are not all equal. An ordered first column gives a line
 *          chart and any other a bar chart. Identifier columns are not charted.
 */
namespace QSocTableChart {

/** @return One SVG document per charted table in @p markdown, in order. */
QByteArrayList charts(const QString &markdown);

} // namespace QSocTableChart

#endif // QSOCTABLECHART_H
