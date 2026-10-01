// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_TEST_TIMESCALE_H
#define QSOC_TEST_TIMESCALE_H

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QStringList>

/**
 * @brief Generated Verilog below a root, split by whether the first line of
 * code is the timescale every generated file carries.
 */
struct QSocTimescaleScan
{
    QStringList checked; /**< Every .v and .sv, relative to the root */
    QStringList missing; /**< Those whose first code line is something else */
};

/**
 * @brief The first line of a Verilog file that is neither blank nor comment.
 */
inline QString qsocFirstCodeLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    bool comment = false;
    for (const QString &raw : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (comment || line.startsWith(QStringLiteral("/*"))) {
            comment = !line.contains(QStringLiteral("*/"));
            continue;
        }
        if (!line.isEmpty() && !line.startsWith(QStringLiteral("//"))) {
            return line;
        }
    }
    return {};
}

/* The copied UVM library is upstream source, not generated output. */
inline QSocTimescaleScan qsocScanTimescale(const QString &root)
{
    QSocTimescaleScan result;
    const QDir        base(root);
    QDirIterator      entries(
        root,
        {QStringLiteral("*.v"), QStringLiteral("*.sv")},
        QDir::Files,
        QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QString relative = base.relativeFilePath(entries.next());
        if (relative.split(QLatin1Char('/')).contains(QStringLiteral("uvm-core"))) {
            continue;
        }
        result.checked.append(relative);
        const QString first = qsocFirstCodeLine(base.filePath(relative));
        if (first != QStringLiteral("`timescale 1ns / 1ps")) {
            result.missing.append(relative + QStringLiteral(": ") + first);
        }
    }
    result.checked.sort();
    return result;
}

#endif // QSOC_TEST_TIMESCALE_H
