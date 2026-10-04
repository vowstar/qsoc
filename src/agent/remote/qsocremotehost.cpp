// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotehost.h"

#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>

namespace {

const QString kAuto = QStringLiteral("auto");
const QString kBash = QStringLiteral("bash");
const QString kSh   = QStringLiteral("sh");

const QString kBegin = QStringLiteral("__QSOC_PROBE_BEGIN__");
const QString kEnd   = QStringLiteral("__QSOC_PROBE_END__");

/* Fields between the frame markers. Login banners and profile output land
 * outside the frame, so they cannot be read as a field. */
QHash<QString, QString> probeFields(const QString &output, bool *framed)
{
    QHash<QString, QString> fields;
    bool                    inside = false;
    *framed                        = false;
    const QStringList lines        = output.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line == kBegin) {
            inside = true;
            fields.clear();
            continue;
        }
        if (line == kEnd && inside) {
            *framed = true;
            return fields;
        }
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (inside && eq > 0) {
            fields.insert(line.left(eq), line.mid(eq + 1).trimmed());
        }
    }
    return {};
}

QSocShellExecutor probedShell(
    const QHash<QString, QString> &fields, const QString &key, QSocShellExecutor::Kind kind)
{
    QSocShellExecutor shell;
    shell.path = fields.value(key);
    if (!shell.path.startsWith(QLatin1Char('/'))) {
        return {};
    }
    shell.kind   = kind;
    shell.launch = key == QStringLiteral("bash") ? kBash : kSh;
    return shell;
}

/* The executor the preference selects among what the probe found. */
QSocShellExecutor pickShell(const QHash<QString, QString> &fields, const QString &preference)
{
    QSocShellExecutor bash = probedShell(fields, kBash, QSocShellExecutor::Kind::Bash);
    bash.login             = true;
    bash.version           = parseBashVersion(fields.value(QStringLiteral("bash_version")));
    QSocShellExecutor sh   = probedShell(fields, kSh, QSocShellExecutor::Kind::Sh);
    sh.login               = fields.contains(QStringLiteral("sh_login"));
    if (preference == kSh) {
        return sh;
    }
    if (bash.available() || preference == kBash) {
        return bash.available() ? bash : QSocShellExecutor{};
    }
    return sh;
}

} // namespace

/* Policy */

bool remoteHostOffersExecTools(const QSocRemoteHost &host)
{
    return host.kind == QSocRemoteHost::Kind::Posix && host.shell.available();
}

bool remoteHostShellEscapePassthrough(const QSocRemoteHost &host)
{
    return !remoteHostOffersExecTools(host);
}

QString remoteShellEscapePassthroughNotice()
{
    return QStringLiteral(
        "(this host has no POSIX shell: the line ran in its login shell as typed, and the "
        "workspace directory was not applied)");
}

/* Description */

QString QSocRemoteHost::summary() const
{
    QString platform;
    switch (kind) {
    case Kind::Posix:
        platform = QStringList{os, arch}.join(QLatin1Char(' ')).trimmed();
        break;
    case Kind::Windows:
        platform = os;
        break;
    case Kind::Unknown:
        platform = QStringLiteral("unknown");
        break;
    }
    return QStringLiteral("%1; shell: %2").arg(platform, shell.summary());
}

QString validateRemoteShellPreference(const QString &preference)
{
    if (preference.isEmpty() || preference == kAuto || preference == kBash || preference == kSh) {
        return {};
    }
    return QStringLiteral("shell: '%1' is not auto, bash or sh").arg(preference);
}

/* Probe */

QString remoteHostProbeCommand()
{
    const QString script = QStringLiteral(
                               "echo %1; "
                               "echo os=$(uname -s 2>/dev/null); "
                               "echo arch=$(uname -m 2>/dev/null); "
                               "b=$(command -v bash 2>/dev/null); echo bash=$b; "
                               "if [ -n \"$b\" ]; then "
                               "echo bash_version=$(\"$b\" --version 2>/dev/null | head -n 1); fi; "
                               "s=$(command -v sh 2>/dev/null); echo sh=$s; "
                               "if [ -n \"$s\" ] && \"$s\" -l -c : >/dev/null 2>&1; then "
                               "echo sh_login=1; fi; "
                               "if [ -d /proc/1 ]; then echo proc=1; fi; "
                               "echo %2")
                               .arg(kBegin, kEnd);
    return QStringLiteral("sh -c '%1'").arg(script);
}

QString remoteWindowsProbeCommand()
{
    return QStringLiteral("cmd /c ver");
}

bool remoteProbeAnswered(const QString &posixOut)
{
    bool framed = false;
    probeFields(posixOut, &framed);
    return framed;
}

QSocRemoteHost parseRemoteHostProbe(
    const QString &posixOut, const QString &windowsOut, const QString &preference)
{
    QSocRemoteHost host;
    bool           framed = false;
    const auto     fields = probeFields(posixOut, &framed);
    if (framed) {
        host.kind    = QSocRemoteHost::Kind::Posix;
        host.os      = fields.value(QStringLiteral("os"));
        host.arch    = fields.value(QStringLiteral("arch"));
        host.hasProc = fields.contains(QStringLiteral("proc"));
        host.shell   = pickShell(fields, preference);
        if (!host.shell.available()) {
            host.shellError = preference.isEmpty() || preference == kAuto
                                  ? QStringLiteral("neither bash nor sh was found")
                                  : QStringLiteral("shell: %1 was not found").arg(preference);
        }
        return host;
    }
    if (windowsOut.contains(QStringLiteral("Microsoft Windows"))) {
        static const QRegularExpression versionRe(QStringLiteral(R"(\[Version\s+([^\]]+)\])"));
        const auto                      match = versionRe.match(windowsOut);
        host.kind                             = QSocRemoteHost::Kind::Windows;
        host.os = match.hasMatch() ? QStringLiteral("Windows ") + match.captured(1).trimmed()
                                   : QStringLiteral("Windows");
        host.shellError = QStringLiteral("Windows hosts have no supported POSIX shell");
        return host;
    }
    host.shellError = QStringLiteral("shell probe got no recognizable answer");
    return host;
}

QString remoteProbeFailure(const QSocSshExec::Result &result)
{
    if (result.timedOut) {
        return QStringLiteral("shell probe timed out");
    }
    if (result.aborted) {
        return QStringLiteral("shell probe was interrupted");
    }
    if (result.transportDead) {
        return QStringLiteral("shell probe lost the link");
    }
    return {};
}

QSocRemoteHost unknownRemoteHost(const QString &reason)
{
    QSocRemoteHost host;
    host.shellError = reason;
    return host;
}

QSocRemoteHost probeRemoteHost(QSocSshSession *session, const QString &preference, int budgetMs)
{
    if (session == nullptr || !session->isConnected()) {
        return unknownRemoteHost(QStringLiteral("no live session to probe"));
    }
    const QDeadlineTimer deadline(budgetMs);
    const auto           remaining = [&deadline] {
        return static_cast<int>(qMax<qint64>(1, deadline.remainingTime()));
    };
    QSocSshExec   exec(*session);
    const auto    posix    = exec.run(remoteHostProbeCommand(), remaining());
    const QString posixOut = QString::fromUtf8(posix.stdoutBytes);
    if (remoteProbeAnswered(posixOut)) {
        return parseRemoteHostProbe(posixOut, {}, preference);
    }
    if (!remoteProbeFailure(posix).isEmpty()) {
        return unknownRemoteHost(remoteProbeFailure(posix));
    }
    const auto windows = exec.run(remoteWindowsProbeCommand(), remaining());
    const auto host
        = parseRemoteHostProbe(posixOut, QString::fromUtf8(windows.stdoutBytes), preference);
    if (host.kind == QSocRemoteHost::Kind::Unknown && !remoteProbeFailure(windows).isEmpty()) {
        return unknownRemoteHost(remoteProbeFailure(windows));
    }
    return host;
}

/* Exec */

QString remoteShellQuote(const QString &value)
{
    QString quoted = QString(value).replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QSocRemoteExec remoteScriptExec(const QSocRemoteHost &host, const QString &script, bool asLogin)
{
    if (!remoteHostOffersExecTools(host)) {
        return {};
    }
    /* One complete line: the shell parses all of it before running any of it,
     * stdin of the work is /dev/null, and `exit` ends the read. */
    const QString line
        = QStringLiteral("eval %1 </dev/null; exit $?\n").arg(remoteShellQuote(script));
    return {host.shell.invocation(asLogin) + QStringLiteral(" -s"), line.toUtf8()};
}

QSocRemoteExec remoteCommandExec(
    const QSocRemoteHost &host, const QString &cwd, const QString &command)
{
    return remoteScriptExec(
        host,
        QStringLiteral("cd -- %1 && eval %2").arg(remoteShellQuote(cwd), remoteShellQuote(command)),
        true);
}
