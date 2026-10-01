// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCELLLIBRARY_H
#define QSOCCELLLIBRARY_H

#include <QList>
#include <QString>

/**
 * @brief The design independent cell unit output/qsoc_cell and the
 *        top file list output/qsoc.fl.
 */
namespace QSocCellLibrary {

struct Cell
{
    QString file;
    QString text;
};

inline QString unit()
{
    return QStringLiteral("qsoc_cell");
}
inline QString clockFile()
{
    return QStringLiteral("qsoc_cell_clock.v");
}
inline QString resetFile()
{
    return QStringLiteral("qsoc_cell_reset.v");
}
inline QString powerFile()
{
    return QStringLiteral("qsoc_cell_power.v");
}

/**
 * @brief Cell files in file list order: clock, reset, power.
 */
QList<Cell> cells();

/**
 * @brief Path of a cell file relative to the output directory.
 */
QString path(const QString &file);

/**
 * @brief True for names starting with qsoc_ or equal to qsoc, any case.
 */
bool isReserved(const QString &name);

/**
 * @brief Rebuild qsoc.fl from qsoc_cell and every <u> or <lib>/<module> unit list.
 * @details Entries must be plain paths relative to the output directory, and
 *          each module may be defined by one listed file only.
 * @return Error message, empty on success.
 */
QString writeFileList(const QString &outputDirectory);

/**
 * @brief Write the cell unit, then rebuild qsoc.fl.
 * @return Error message, empty on success.
 */
QString publish(const QString &outputDirectory);

} // namespace QSocCellLibrary

#endif // QSOCCELLLIBRARY_H
