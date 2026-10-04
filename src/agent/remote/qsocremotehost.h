// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEHOST_H
#define QSOCREMOTEHOST_H

#include "agent/remote/qsocsshexec.h"
#include "common/qsocmachine.h"

#include <QByteArray>
#include <QString>

#include <functional>

class QSocSftpClient;

/** @brief One exec request: the line the login shell parses, and stdin. */
struct QSocRemoteExec
{
    QString    command; /**< Fixed text; carries no caller-supplied bytes. */
    QByteArray input;   /**< Script the interpreter reads from stdin. */

    bool isValid() const { return !command.isEmpty(); }
};

/** @brief Notice that leads a passthrough `!` result. */
QString remoteShellEscapePassthroughNotice();

/**
 * @brief Check a host.yml `shell:` value.
 * @details Accepts empty, `auto`, `bash`, `sh`, or an absolute Windows path to
 *          a `bash.exe`, which applies to Windows hosts only.
 * @return Empty when valid, else the reason.
 */
QString validateRemoteShellPreference(const QString &preference);

/**
 * @brief The POSIX probe, as a line any login shell parses the same way.
 * @details `sh -c '<script>'` with no `!`, no newline and no single quote
 *          inside, so csh-family history and quoting rules do not apply. The
 *          fields it prints sit between two frame lines.
 */
QString remoteHostProbeCommand();

/**
 * @brief The Windows probe, one `echo` that cmd and PowerShell both run.
 * @details Each field is `NAME=%NAME%${env:NAME}`: cmd expands only the
 *          first, PowerShell only the second, so the answer names the login
 *          shell as well as the values. Runs only when the POSIX probe found
 *          no POSIX host.
 */
QString remoteWindowsProbeCommand();

/**
 * @brief Turn probe output into a machine description.
 * @param posixOut Stdout of @ref remoteHostProbeCommand.
 * @param windowsOut Stdout of @ref remoteWindowsProbeCommand.
 * @param preference Validated `shell:` value.
 * @param exists Whether a Windows path names a file on the host; without
 *        it no Git Bash is found.
 * @details A POSIX frame whose `uname -s` starts with MINGW, MSYS or CYGWIN
 *          is a Windows host. Git Bash candidates come from
 *          QSocShellPath::windowsGitBashCandidates over the probed values.
 */
QSocMachine parseRemoteHostProbe(
    const QString                              &posixOut,
    const QString                              &windowsOut,
    const QString                              &preference,
    const std::function<bool(const QString &)> &exists = {});

/** @brief Whether @p posixOut carries a complete probe frame. */
bool remoteProbeAnswered(const QString &posixOut);

/** @brief Why one probe exec gave no answer (timeout, abort, dead link), empty otherwise. */
QString remoteProbeFailure(const QSocSshExec::Result &result);

/** @brief A host the probe could not classify, with @p reason as its shell error. */
QSocMachine unknownRemoteHost(const QString &reason);

/**
 * @brief Run both probes over @p session within @p budgetMs.
 * @details The Windows probe runs only when the POSIX one printed no marker.
 *          A probe that times out or answers nothing usable yields an
 *          unknown host with the reason; the session is left as the exec
 *          left it, and the next adopt probes again.
 */
QSocMachine probeRemoteHost(
    QSocSshSession *session, QSocSftpClient *sftp, const QString &preference, int budgetMs);

/**
 * @brief Record how the executor spells the workspace root.
 * @details @p cygpathOut is the host's `cygpath -u` answer for @p sftpRoot.
 *          When it differs from the string mapping, it becomes the root
 *          mapping @ref machineShellPath uses.
 */
void applyRemoteShellRoot(QSocMachine *host, const QString &sftpRoot, const QString &cygpathOut);

/**
 * @brief Ask a Windows host's Git Bash how it spells @p sftpRoot.
 * @details One `cygpath -u` through the executor; applies the answer with
 *          @ref applyRemoteShellRoot. A POSIX host or a failed exec leaves
 *          @p host unchanged.
 */
void verifyRemoteShellRoot(
    QSocSshSession *session, QSocMachine *host, const QString &sftpRoot, int budgetMs);

/** @brief POSIX single-quote @p value. */
QString remoteShellQuote(const QString &value);

/**
 * @brief Run a POSIX script under the host's executor, script on stdin.
 * @details On a POSIX host the exec line names the interpreter, so the
 *          host's PATH finds it; on Windows it is the fixed
 *          @ref windowsLauncher for the login shell. Stdin carries one line
 *          that evaluates the quoted script with stdin from /dev/null and
 *          then exits.
 * @param asLogin Pass `-l` when the executor accepts it.
 * @return An invalid request when the host has no executor.
 */
QSocRemoteExec remoteScriptExec(const QSocMachine &host, const QString &script, bool asLogin);

/**
 * @brief Run a user command from @p cwd under the host's login executor.
 * @details `cd -- <cwd> && eval <command>`, so a command that fails to reach
 *          @p cwd never runs. @p cwd is mapped with @ref machineShellPath.
 * @return An invalid request when the host has no executor.
 */
QSocRemoteExec remoteCommandExec(const QSocMachine &host, const QString &cwd, const QString &command);

/**
 * @brief The exec request for a `!` line on @p host.
 * @details Executor: @ref remoteCommandExec. Cmd: the fixed
 *          @ref windowsShellEscapeLine with @p cwd in Windows form. An
 *          invalid request for a passthrough host.
 */
QSocRemoteExec remoteShellEscapeExec(
    const QSocMachine &host, const QString &cwd, const QString &command);

#endif // QSOCREMOTEHOST_H
