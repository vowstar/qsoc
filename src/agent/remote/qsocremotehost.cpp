// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotehost.h"

#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"
#include "common/qsocshellpath.h"

#include <QDeadlineTimer>
#include <QHash>
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

const QString kWinBegin = QStringLiteral("__QSOC_WIN__");
const QString kWinEnd   = QStringLiteral("__QSOC_WIN_END__");

const QStringList &windowsProbeNames()
{
    static const QStringList names{
        QStringLiteral("OS"),
        QStringLiteral("PROCESSOR_ARCHITECTURE"),
        QStringLiteral("GIT_INSTALL_ROOT"),
        QStringLiteral("ProgramFiles"),
        QStringLiteral("ProgramFiles(x86)"),
        QStringLiteral("LOCALAPPDATA"),
        QStringLiteral("SCOOP"),
        QStringLiteral("USERPROFILE"),
    };
    return names;
}

/* What the Windows probe printed: the login shell that expanded it and the
 * values by name. */
struct WindowsAnswer
{
    QSocMachine::LoginShell login = QSocMachine::LoginShell::Unknown;
    QHash<QString, QString> env;
};

/* cmd leaves `${env:NAME}` behind and an undefined `%NAME%` as typed;
 * PowerShell leaves `%NAME%` behind. */
QString windowsValue(QSocMachine::LoginShell login, const QString &name, const QString &raw)
{
    const QString cmdTail = QStringLiteral("${env:%1}").arg(name);
    const QString psHead  = QStringLiteral("%%1%").arg(name);
    QString       value   = raw;
    if (login == QSocMachine::LoginShell::Cmd) {
        value.chop(cmdTail.size());
        return value == psHead ? QString() : value;
    }
    return value.mid(psHead.size());
}

WindowsAnswer windowsAnswer(const QString &output)
{
    WindowsAnswer   answer;
    const qsizetype begin = output.indexOf(kWinBegin + QLatin1Char('|'));
    const qsizetype end   = output.indexOf(QLatin1Char('|') + kWinEnd, begin);
    if (begin < 0 || end < 0) {
        return answer;
    }
    const qsizetype         from   = begin + kWinBegin.size() + 1;
    const QStringList       fields = output.mid(from, end - from).split(QLatin1Char('|'));
    QHash<QString, QString> raw;
    for (const QString &field : fields) {
        const qsizetype eq = field.indexOf(QLatin1Char('='));
        if (eq > 0) {
            raw.insert(field.left(eq), field.mid(eq + 1));
        }
    }
    const QString os = raw.value(QStringLiteral("OS"));
    if (os.endsWith(QStringLiteral("${env:OS}"))) {
        answer.login = QSocMachine::LoginShell::Cmd;
    } else if (os.startsWith(QStringLiteral("%OS%")) && os.size() > 4) {
        answer.login = QSocMachine::LoginShell::PowerShell;
    } else {
        return answer;
    }
    for (const QString &name : windowsProbeNames()) {
        answer.env.insert(name, windowsValue(answer.login, name, raw.value(name)));
    }
    return answer;
}

bool isWindowsUname(const QString &uname)
{
    for (const char *prefix : {"MINGW", "MSYS", "CYGWIN"}) {
        if (uname.startsWith(QLatin1String(prefix), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

/* Git Bash among the shared candidates, checked on the host. */
void pickGitBash(
    QSocMachine                                *host,
    const QHash<QString, QString>              &env,
    const QString                              &preference,
    const std::function<bool(const QString &)> &exists)
{
    using Login = QSocMachine::LoginShell;
    if (preference == kSh) {
        host->shellError = QStringLiteral("shell: sh does not apply to Windows hosts");
        return;
    }
    if (host->loginShell != Login::Cmd && host->loginShell != Login::PowerShell) {
        host->shellError = QStringLiteral(
            "the login shell is neither cmd nor PowerShell, so Git Bash cannot be started");
        return;
    }
    const QString override = QSocShellPath::isWindowsBashPath(preference) ? preference : QString();
    for (const QString &candidate : QSocShellPath::windowsGitBashCandidates(override, {}, env)) {
        if (exists && exists(candidate)) {
            host->shell.kind   = QSocShellExecutor::Kind::GitBash;
            host->shell.path   = QSocShellPath::toWindowsPath(candidate);
            host->shell.launch = kBash;
            host->shell.login  = true;
            return;
        }
    }
    host->shellError = override.isEmpty()
                           ? QStringLiteral(
                                 "no Git Bash found; install Git for Windows on the host or "
                                 "set shell: in host.yml")
                           : QStringLiteral("shell: %1 was not found").arg(override);
}

} // namespace

/* Policy */

QString remoteShellEscapePassthroughNotice()
{
    return QStringLiteral(
        "(this host has no usable shell: the line ran in its login shell as typed, and the "
        "workspace directory was not applied)");
}

/* Description */

QString validateRemoteShellPreference(const QString &preference)
{
    if (preference.isEmpty() || preference == kAuto || preference == kBash || preference == kSh
        || QSocShellPath::isWindowsBashPath(preference)) {
        return {};
    }
    return QStringLiteral("shell: '%1' is not auto, bash, sh or a Windows path to bash.exe")
        .arg(preference);
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
    QStringList fields{kWinBegin};
    for (const QString &name : windowsProbeNames()) {
        fields << QStringLiteral("%1=%%1%${env:%1}").arg(name);
    }
    fields << kWinEnd;
    return QStringLiteral("echo \"%1\"").arg(fields.join(QLatin1Char('|')));
}

bool remoteProbeAnswered(const QString &posixOut)
{
    bool framed = false;
    probeFields(posixOut, &framed);
    return framed;
}

QSocMachine parseRemoteHostProbe(
    const QString                              &posixOut,
    const QString                              &windowsOut,
    const QString                              &preference,
    const std::function<bool(const QString &)> &exists)
{
    QSocMachine host;
    bool        framed = false;
    const auto  fields = probeFields(posixOut, &framed);
    const bool  msys   = framed && isWindowsUname(fields.value(QStringLiteral("os")));
    if (framed && !msys) {
        host.kind       = QSocMachine::Kind::Posix;
        host.loginShell = QSocMachine::LoginShell::Posix;
        host.os         = fields.value(QStringLiteral("os"));
        host.arch       = fields.value(QStringLiteral("arch"));
        host.hasProc    = fields.contains(QStringLiteral("proc"));
        if (QSocShellPath::isWindowsBashPath(preference)) {
            host.shellError
                = QStringLiteral("shell: %1 applies to Windows hosts only").arg(preference);
            return host;
        }
        host.shell = pickShell(fields, preference);
        if (!host.shell.available()) {
            host.shellError = preference.isEmpty() || preference == kAuto
                                  ? QStringLiteral("neither bash nor sh was found")
                                  : QStringLiteral("shell: %1 was not found").arg(preference);
        }
        return host;
    }
    const WindowsAnswer answer = windowsAnswer(windowsOut);
    if (!msys && answer.env.value(QStringLiteral("OS")) != QStringLiteral("Windows_NT")) {
        host.shellError = QStringLiteral("shell probe got no recognizable answer");
        return host;
    }
    host.kind       = QSocMachine::Kind::Windows;
    host.loginShell = answer.login;
    host.os         = QStringLiteral("Windows");
    host.arch       = answer.env.value(QStringLiteral("PROCESSOR_ARCHITECTURE"));
    if (host.arch.isEmpty()) {
        host.arch = fields.value(QStringLiteral("arch"));
    }
    pickGitBash(&host, answer.env, preference, exists);
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

QSocMachine unknownRemoteHost(const QString &reason)
{
    QSocMachine host;
    host.shellError = reason;
    return host;
}

QSocMachine probeRemoteHost(
    QSocSshSession *session, QSocSftpClient *sftp, const QString &preference, int budgetMs)
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
        const QSocMachine host = parseRemoteHostProbe(posixOut, {}, preference);
        if (host.kind == QSocMachine::Kind::Posix) {
            return host;
        }
    } else if (!remoteProbeFailure(posix).isEmpty()) {
        return unknownRemoteHost(remoteProbeFailure(posix));
    }
    const auto exists = [sftp](const QString &path) {
        return sftp != nullptr
               && sftp->presence(QSocShellPath::toSftpPath(path))
                      == QSocSftpClient::Presence::Present;
    };
    const auto        windows = exec.run(remoteWindowsProbeCommand(), remaining());
    const QSocMachine host
        = parseRemoteHostProbe(posixOut, QString::fromUtf8(windows.stdoutBytes), preference, exists);
    if (host.kind == QSocMachine::Kind::Unknown && !remoteProbeFailure(windows).isEmpty()) {
        return unknownRemoteHost(remoteProbeFailure(windows));
    }
    return host;
}

void applyRemoteShellRoot(QSocMachine *host, const QString &sftpRoot, const QString &cygpathOut)
{
    host->rootFrom.clear();
    host->rootTo.clear();
    const QString answer = cygpathOut.trimmed();
    if (answer.startsWith(QLatin1Char('/')) && !answer.contains(QLatin1Char('\n'))
        && answer != QSocShellPath::toPosixPath(sftpRoot)) {
        host->rootFrom = sftpRoot;
        host->rootTo   = answer;
    }
}

void verifyRemoteShellRoot(
    QSocSshSession *session, QSocMachine *host, const QString &sftpRoot, int budgetMs)
{
    if (session == nullptr || host->kind != QSocMachine::Kind::Windows
        || !machineOffersExecTools(*host)) {
        return;
    }
    const QSocRemoteExec request = remoteScriptExec(
        *host,
        QStringLiteral("cygpath -u -- %1")
            .arg(remoteShellQuote(QSocShellPath::toWindowsPath(sftpRoot))),
        false);
    QSocSshExec exec(*session);
    const auto  result = exec.run(request.command, budgetMs, request.input);
    if (result.exitCode == 0) {
        applyRemoteShellRoot(host, sftpRoot, QString::fromUtf8(result.stdoutBytes));
    }
}

/* Exec */

QString remoteShellQuote(const QString &value)
{
    QString quoted = QString(value).replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QSocRemoteExec remoteScriptExec(const QSocMachine &host, const QString &script, bool asLogin)
{
    if (!machineOffersExecTools(host)) {
        return {};
    }
    const QString launcher = host.kind == QSocMachine::Kind::Windows
                                 ? windowsLauncher(
                                       host.loginShell,
                                       host.shell.path,
                                       asLogin ? QStringLiteral("-l -s") : QStringLiteral("-s"))
                                 : host.shell.invocation(asLogin) + QStringLiteral(" -s");
    if (launcher.isEmpty()) {
        return {};
    }
    /* One complete line: the shell parses all of it before running any of it,
     * stdin of the work is /dev/null, and `exit` ends the read. */
    const QString line
        = QStringLiteral("eval %1 </dev/null; exit $?\n").arg(remoteShellQuote(script));
    return {launcher, line.toUtf8()};
}

QSocRemoteExec remoteCommandExec(const QSocMachine &host, const QString &cwd, const QString &command)
{
    return remoteScriptExec(
        host,
        QStringLiteral("cd -- %1 && eval %2")
            .arg(remoteShellQuote(machineShellPath(host, cwd)), remoteShellQuote(command)),
        true);
}

QSocRemoteExec remoteShellEscapeExec(
    const QSocMachine &host, const QString &cwd, const QString &command)
{
    switch (machineShellEscapeMode(host)) {
    case QSocShellEscapeMode::Executor:
        return remoteCommandExec(host, cwd, command);
    case QSocShellEscapeMode::Cmd:
        return {
            windowsShellEscapeLine(host.loginShell, QSocShellPath::toWindowsPath(cwd), command), {}};
    case QSocShellEscapeMode::Passthrough:
        break;
    }
    return {};
}
