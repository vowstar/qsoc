// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocshellpath.h"

#include "common/qsocconsole.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtGlobal>

namespace QSocShellPath {

namespace {

/**
 * @brief Check that a candidate is a real executable outside the cwd.
 * @details Rejecting cwd-resident executables prevents a checked-out
 *          repository from planting a `bash`/`git` that a PATH entry
 *          like `.` would otherwise resolve first.
 */
bool isUsableCandidate(const QString &candidate)
{
    if (candidate.isEmpty()) {
        return false;
    }
    const QFileInfo info(candidate);
    if (!info.isAbsolute() || !info.exists() || !info.isFile() || !info.isExecutable()) {
        return false;
    }
    const QString canonical = info.canonicalFilePath();
    const QString cwd       = QDir::current().canonicalPath();
    if (!cwd.isEmpty()
        && (canonical == cwd || canonical.startsWith(cwd + QDir::separator())
            || canonical.startsWith(cwd + QLatin1Char('/')))) {
        QSocConsole::warn() << "Ignoring shell candidate inside the working directory:"
                            << candidate;
        return false;
    }
    return true;
}

QString resolveBashPath()
{
    /* Explicit override: trusted but fail-closed. A pinned interpreter
     * that is missing must not silently degrade to a different one. */
    const QString override = qEnvironmentVariable("QSOC_GIT_BASH_PATH");
    if (!override.isEmpty()) {
        const QFileInfo info(override);
        if (info.isAbsolute() && info.exists() && info.isFile() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
        QSocConsole::warn() << "QSOC_GIT_BASH_PATH is set but not a usable executable:" << override;
        return {};
    }

#ifdef Q_OS_WIN
    /* Never take `bash` straight from PATH on Windows: System32\bash.exe
     * is the WSL launcher and runs commands in a different OS image.
     * Only a git-bash derived from the git executable is acceptable. */
    const QString gitExe = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (!gitExe.isEmpty()) {
        for (const QString &candidate : gitBashCandidates(gitExe)) {
            if (isUsableCandidate(candidate)) {
                return QFileInfo(candidate).absoluteFilePath();
            }
        }
    }
    QSocConsole::warn() << "No git-bash found. Install Git for Windows or set"
                        << "QSOC_GIT_BASH_PATH to your bash.exe.";
    return {};
#else
    const QString systemBash = QStringLiteral("/bin/bash");
    if (isUsableCandidate(systemBash)) {
        return systemBash;
    }
    const QString pathBash = QStandardPaths::findExecutable(QStringLiteral("bash"));
    if (isUsableCandidate(pathBash)) {
        return pathBash;
    }
    const QString systemSh = QStringLiteral("/bin/sh");
    if (isUsableCandidate(systemSh)) {
        return systemSh;
    }
    QSocConsole::warn() << "No POSIX shell found (/bin/bash, bash on PATH, /bin/sh).";
    return {};
#endif
}

QString cachedBash;
bool    cacheValid = false;

} // namespace

QString bashPath()
{
    if (!cacheValid) {
        cachedBash = resolveBashPath();
        cacheValid = true;
    }
    return cachedBash;
}

void resetCache()
{
    cachedBash.clear();
    cacheValid = false;
}

QStringList gitBashCandidates(const QString &gitExePath)
{
    if (gitExePath.isEmpty()) {
        return {};
    }
    /* Git for Windows: git.exe lives in <root>/cmd or <root>/mingw64/bin;
     * bash.exe lives in <root>/bin and <root>/usr/bin. Walk up from the
     * executable's directory as a string (QDir::cdUp needs the directory
     * to exist) and probe both layouts at each level. */
    QString     dir = QFileInfo(gitExePath).path();
    QStringList out;
    for (int up = 0; up < 3; ++up) {
        const int slash = static_cast<int>(dir.lastIndexOf(QLatin1Char('/')));
        if (slash <= 0) {
            break;
        }
        dir = dir.left(slash);
        out << dir + QStringLiteral("/bin/bash.exe") << dir + QStringLiteral("/usr/bin/bash.exe");
    }
    out.removeDuplicates();
    return out;
}

QString toPosixPath(const QString &path)
{
    if (path.isEmpty()) {
        return path;
    }
    /* UNC \\server\share -> //server/share */
    if (path.startsWith(QStringLiteral("\\\\"))) {
        QString out = path;
        out.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return out;
    }
    /* Drive letter C:\x or C:/x -> /c/x */
    static const QRegularExpression driveRe(QStringLiteral(R"(^([A-Za-z]):[/\\])"));
    const QRegularExpressionMatch   match = driveRe.match(path);
    if (match.hasMatch()) {
        QString rest = path.mid(2);
        rest.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return QStringLiteral("/") + match.captured(1).toLower() + rest;
    }
    /* Already POSIX or relative: flip any stray backslashes */
    QString out = path;
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return out;
}

QString toShellPath(const QString &path)
{
#ifdef Q_OS_WIN
    return toPosixPath(path);
#else
    return path;
#endif
}

} // namespace QSocShellPath
