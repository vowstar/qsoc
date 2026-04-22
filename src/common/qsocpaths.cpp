// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocpaths.h"

#include <QDir>
#include <QProcessEnvironment>
#include <QSet>

namespace QSocPaths {

QString envRoot()
{
    return QProcessEnvironment::systemEnvironment().value(QStringLiteral("QSOC_HOME"));
}

QString projectRoot(const QString &projectPath)
{
    if (projectPath.isEmpty()) {
        return {};
    }
    return QDir(projectPath).filePath(QStringLiteral(".qsoc"));
}

QString userRoot()
{
    const QProcessEnvironment env  = QProcessEnvironment::systemEnvironment();
    QString                   base = env.value(QStringLiteral("XDG_CONFIG_HOME"));
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.config");
    }
    return QDir(base).filePath(QStringLiteral("qsoc"));
}

QString systemRoot()
{
#if defined(Q_OS_LINUX)
    return QStringLiteral("/etc/qsoc");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("/Library/Application Support/qsoc");
#elif defined(Q_OS_WIN)
    QString base = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PROGRAMDATA"));
    if (base.isEmpty()) {
        base = QStringLiteral("C:/ProgramData");
    }
    return QDir(QDir::fromNativeSeparators(base)).filePath(QStringLiteral("qsoc"));
#else
    return QStringLiteral("/etc/qsoc");
#endif
}

QStringList resourceDirs(const QString &subdir, const QString &projectPath)
{
    QStringList roots;
    roots << envRoot();
    roots << projectRoot(projectPath);
    roots << userRoot();
    roots << systemRoot();

    QStringList   out;
    QSet<QString> seen;
    for (const QString &root : roots) {
        if (root.isEmpty()) {
            continue;
        }
        const QString dir = subdir.isEmpty() ? root : QDir(root).filePath(subdir);
        /* Canonicalize if the path exists; otherwise fall back to cleanPath
         * so non-existent directories still compare consistently. */
        QString key = QDir(dir).canonicalPath();
        if (key.isEmpty()) {
            key = QDir::cleanPath(dir);
        }
        if (!seen.contains(key)) {
            seen.insert(key);
            out << dir;
        }
    }
    return out;
}

} // namespace QSocPaths
