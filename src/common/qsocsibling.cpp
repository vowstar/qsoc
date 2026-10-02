// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocsibling.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QStringList>

namespace {

QStringList searchDirectories()
{
    const QString binDir = qEnvironmentVariable("QSOC_BIN_DIR");
    const QString base   = binDir.isEmpty() ? QCoreApplication::applicationDirPath() : binDir;
    QStringList   dirs{base};
#ifdef Q_OS_MACOS
    for (const QString &bundle : {QStringLiteral("QSoC.app"), QStringLiteral("qsoc-gui.app")})
        dirs.append(QDir(base).filePath(bundle + QStringLiteral("/Contents/MacOS")));
#endif
    return dirs;
}

} // namespace

namespace QSocSibling {

QString path(const QString &name)
{
    return QStandardPaths::findExecutable(name, searchDirectories());
}

QString missingMessage(const QString &name)
{
    return QStringLiteral("%1 was not found in %2. Reinstall QSoC or set QSOC_BIN_DIR.")
        .arg(name, searchDirectories().join(QStringLiteral(", ")));
}

} // namespace QSocSibling
