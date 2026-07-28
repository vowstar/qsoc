// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocpaths.h"

#include <QDir>
#include <QFileInfo>
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

ArtifactPathResult resolveArtifactPath(const QString &outputDirectory, const QString &requestedPath)
{
    if (requestedPath.isEmpty()) {
        return {{}, QStringLiteral("Artifact path is empty.")};
    }
    if (requestedPath.contains(QChar::Null)) {
        return {{}, QStringLiteral("Artifact path contains a null character.")};
    }

    const QFileInfo outputInfo(outputDirectory);
    if (!outputInfo.exists() || !outputInfo.isDir()) {
        return {{}, QStringLiteral("Artifact output directory does not exist.")};
    }
    const QString canonicalOutput = outputInfo.canonicalFilePath();
    if (canonicalOutput.isEmpty()) {
        return {{}, QStringLiteral("Artifact output directory cannot be resolved.")};
    }

    const QString   candidatePath = QDir(canonicalOutput).absoluteFilePath(requestedPath);
    const QFileInfo candidateInfo(candidatePath);
    if (candidateInfo.exists() && !candidateInfo.isFile()) {
        return {{}, QStringLiteral("Artifact target is not a regular file.")};
    }

    const QFileInfo parentInfo(candidateInfo.path());
    if (!parentInfo.exists() || !parentInfo.isDir()) {
        return {{}, QStringLiteral("Artifact parent directory does not exist.")};
    }
    const QString canonicalParent = QDir(candidateInfo.path()).canonicalPath();
    if (canonicalParent.isEmpty()) {
        return {{}, QStringLiteral("Artifact parent directory cannot be resolved.")};
    }

    const QString normalizedOutput = QDir::fromNativeSeparators(canonicalOutput);
    const QString normalizedParent = QDir::fromNativeSeparators(canonicalParent);
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    QString outputPrefix = normalizedOutput;
    if (!outputPrefix.endsWith('/')) {
        outputPrefix += '/';
    }
    if (normalizedParent.compare(normalizedOutput, pathCase) != 0
        && !normalizedParent.startsWith(outputPrefix, pathCase)) {
        return {{}, QStringLiteral("Artifact target is outside the output directory.")};
    }

    return {QDir(canonicalParent).filePath(candidateInfo.fileName()), {}};
}

} // namespace QSocPaths
