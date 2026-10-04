// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocshellexecutor.h"

#include "common/qsocshellpath.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace {

std::function<QSocShellExecutor()> &resolverSlot()
{
    static std::function<QSocShellExecutor()> resolver;
    return resolver;
}

QString probeVersion(const QString &path)
{
    QProcess process;
    process.start(path, {QStringLiteral("--version")});
    if (!process.waitForStarted(2000) || !process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished(500);
        return {};
    }
    const QString text = QString::fromLocal8Bit(process.readAllStandardOutput());
    return parseBashVersion(text.section(QLatin1Char('\n'), 0, 0).trimmed());
}

QSocShellExecutor resolveLocal()
{
    QSocShellExecutor shell;
    shell.path = QSocShellPath::bashPath();
    if (shell.path.isEmpty()) {
        return shell;
    }
#ifdef Q_OS_WIN
    shell.kind = classifyShellPath(shell.path, true);
#else
    shell.kind = classifyShellPath(shell.path, false);
#endif
    shell.login = shell.kind != QSocShellExecutor::Kind::Sh;
    if (shell.kind != QSocShellExecutor::Kind::Sh) {
        shell.version = probeVersion(shell.path);
    }
    return shell;
}

} // namespace

QString QSocShellExecutor::invocation(bool asLogin) const
{
    const QString program = launch.isEmpty() ? path : launch;
    return asLogin && login ? program + QStringLiteral(" -l") : program;
}

QString QSocShellExecutor::summary() const
{
    const QString ver = version.isEmpty() ? QString() : QLatin1Char(' ') + version;
    switch (kind) {
    case Kind::Bash:
        return QStringLiteral("bash") + ver;
    case Kind::Sh:
        return QStringLiteral("sh (POSIX only, no bash extensions)");
    case Kind::GitBash:
        return QStringLiteral("Git Bash (MSYS)%1, use /c/... paths").arg(ver);
    case Kind::None:
        break;
    }
    return QStringLiteral("none");
}

QString parseBashVersion(const QString &firstLine)
{
    static const QRegularExpression versionRe(QStringLiteral(R"(version\s+(\S+))"));
    const auto                      match = versionRe.match(firstLine);
    return match.hasMatch() ? match.captured(1) : firstLine.trimmed();
}

QSocShellExecutor::Kind classifyShellPath(const QString &path, bool windows)
{
    if (path.isEmpty()) {
        return QSocShellExecutor::Kind::None;
    }
    const QString name = QFileInfo(path).fileName().toLower();
    if (!name.startsWith(QStringLiteral("bash"))) {
        return QSocShellExecutor::Kind::Sh;
    }
    return windows ? QSocShellExecutor::Kind::GitBash : QSocShellExecutor::Kind::Bash;
}

QSocShellExecutor localShellExecutor()
{
    if (resolverSlot()) {
        return resolverSlot()();
    }
    static QSocShellExecutor cached;
    static QString           cachedFor;
    static bool              valid = false;
    /* Follows bashPath(), whose cache tests reset, so a stale answer is
     * never served for a different interpreter. */
    const QString path = QSocShellPath::bashPath();
    if (!valid || cachedFor != path) {
        cached    = resolveLocal();
        cachedFor = path;
        valid     = true;
    }
    return cached;
}

void setLocalShellResolver(std::function<QSocShellExecutor()> resolver)
{
    resolverSlot() = std::move(resolver);
}
