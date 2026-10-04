// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmachine.h"

#include "common/qsocshellpath.h"

#include <QFileInfo>
#include <QStringList>
#include <QSysInfo>

QString QSocMachine::summary() const
{
    QString platform;
    switch (kind) {
    case Kind::Posix:
    case Kind::Windows:
        platform = QStringList{os, arch}.join(QLatin1Char(' ')).trimmed();
        break;
    case Kind::Unknown:
        platform = QStringLiteral("unknown");
        break;
    }
    return QStringLiteral("%1; shell: %2").arg(platform, shell.summary());
}

bool machineOffersExecTools(const QSocMachine &machine)
{
    return machine.kind != QSocMachine::Kind::Unknown && machine.shell.available();
}

QSocShellEscapeMode machineShellEscapeMode(const QSocMachine &machine)
{
    using Login = QSocMachine::LoginShell;
    if (machine.kind == QSocMachine::Kind::Windows) {
        return machine.loginShell == Login::Cmd || machine.loginShell == Login::PowerShell
                   ? QSocShellEscapeMode::Cmd
                   : QSocShellEscapeMode::Passthrough;
    }
    return machineOffersExecTools(machine) ? QSocShellEscapeMode::Executor
                                           : QSocShellEscapeMode::Passthrough;
}

QString shellEscapeShellName(const QSocMachine &machine)
{
    switch (machineShellEscapeMode(machine)) {
    case QSocShellEscapeMode::Executor:
        return machine.shell.summary();
    case QSocShellEscapeMode::Cmd:
        return QStringLiteral("cmd");
    case QSocShellEscapeMode::Passthrough:
        break;
    }
    return QStringLiteral("login shell");
}

QSocMachine localMachine()
{
    QSocMachine machine;
#ifdef Q_OS_WIN
    machine.kind       = QSocMachine::Kind::Windows;
    machine.loginShell = QSocMachine::LoginShell::Cmd;
    const QString why  = QStringLiteral("install Git for Windows or set QSOC_GIT_BASH_PATH");
#else
    machine.kind       = QSocMachine::Kind::Posix;
    machine.loginShell = QSocMachine::LoginShell::Posix;
    const QString why  = QStringLiteral("no /bin/bash, bash on PATH or /bin/sh");
#endif
    machine.os      = QSysInfo::productType() + QLatin1Char(' ') + QSysInfo::productVersion();
    machine.arch    = QSysInfo::currentCpuArchitecture();
    machine.hasProc = QFileInfo(QStringLiteral("/proc/1")).isDir();
    machine.shell   = localShellExecutor();
    if (!machine.shell.available()) {
        machine.shellError = why;
    }
    return machine;
}

QString gitBashGuidance()
{
    return QStringLiteral(
        "Windows rules for bash (Git Bash): use POSIX paths such as /c/Users/name, not "
        "C:\\Users\\name; Windows programs run by name (git, python, where.exe); files may "
        "have CRLF line endings; there is no sudo; run cmd builtins as cmd //c <builtin>.");
}

QString machineEnvironmentLines(const QSocMachine &machine, bool remote)
{
    const auto orUnknown = [](const QString &value) {
        return value.isEmpty() ? QStringLiteral("unknown") : value;
    };
    QString shell = machine.shell.summary();
    if (!machineOffersExecTools(machine)) {
        shell = QStringLiteral("unavailable (%1); bash, bash_manage and monitor are not offered")
                    .arg(orUnknown(machine.shellError));
    }
    QString lines = QStringLiteral("- OS: %1\n- Arch: %2\n- Shell: %3\n- Executor: %4\n")
                        .arg(
                            orUnknown(machine.os),
                            orUnknown(machine.arch),
                            shell,
                            remote ? QStringLiteral("remote") : QStringLiteral("local"));
    if (!machine.rootTo.isEmpty()) {
        lines += QStringLiteral("- Workspace in bash: %1 (as the host maps %2)\n")
                     .arg(machine.rootTo, machine.rootFrom);
    }
    if (machineOffersExecTools(machine) && machine.shell.kind == QSocShellExecutor::Kind::GitBash) {
        lines += QStringLiteral("- ") + gitBashGuidance() + QLatin1Char('\n');
    }
    return lines;
}

QString machineShellPath(const QSocMachine &machine, const QString &path)
{
    if (machine.kind != QSocMachine::Kind::Windows) {
        return path;
    }
    const QString &from  = machine.rootFrom;
    const bool     under = !machine.rootTo.isEmpty() && !from.isEmpty()
                           && (path.compare(from, Qt::CaseInsensitive) == 0
                               || path.startsWith(from + QLatin1Char('/'), Qt::CaseInsensitive));
    return under ? machine.rootTo + path.mid(from.size()) : QSocShellPath::toPosixPath(path);
}

QString cmdCaretEscape(const QString &line)
{
    static const QString special = QStringLiteral("&|<>^()");
    QString              out;
    bool                 quoted = false;
    for (const QChar c : line) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (!quoted && special.contains(c)) {
            out += QLatin1Char('^');
        }
        out += c;
    }
    return out;
}

QString windowsLauncher(QSocMachine::LoginShell login, const QString &exe, const QString &args)
{
    const QString native = QSocShellPath::toWindowsPath(exe);
    switch (login) {
    case QSocMachine::LoginShell::Cmd:
        return QStringLiteral("\"\"%1\" %2\"").arg(native, args);
    case QSocMachine::LoginShell::PowerShell:
        return QStringLiteral("& '%1' %2").arg(QString(native).replace("'", "''"), args);
    case QSocMachine::LoginShell::Posix:
    case QSocMachine::LoginShell::Unknown:
        break;
    }
    return {};
}

QString windowsShellEscapeLine(
    QSocMachine::LoginShell login, const QString &cwd, const QString &command)
{
    const QString line = cwd.isEmpty() ? command
                                       : QStringLiteral("cd /d \"%1\" && %2").arg(cwd, command);
    const QString args = QSocShellPath::cmdExeNativeArguments(line);
    switch (login) {
    case QSocMachine::LoginShell::Cmd:
        return QStringLiteral("cmd ") + cmdCaretEscape(args);
    case QSocMachine::LoginShell::PowerShell:
        return QStringLiteral("cmd --% ") + args;
    case QSocMachine::LoginShell::Posix:
    case QSocMachine::LoginShell::Unknown:
        break;
    }
    return {};
}
