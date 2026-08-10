// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsoctoolremote.h"

#include "agent/qsocfilehistory.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotejobs.h"
#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace {

/** @brief One resolved path argument, or the refusal to hand back. */
struct ResolvedPath
{
    QString path;
    QString error;
};

/**
 * @brief Turn a tool's path argument into the name the host will operate on.
 * @details Lexical normalization first, then host-side canonicalization, so
 *          every tool downstream (the containment check, the write, the
 *          read-before-edit bookkeeping and the message) names one path. The
 *          lexical form alone is not enough: a directory the host reaches
 *          through a symlink keeps its in-workspace spelling while the write
 *          lands wherever the link points.
 */
ResolvedPath remoteResolve(QSocRemotePathContext *ctx, QSocRemoteConnection *conn, const QString &raw)
{
    if (ctx == nullptr) {
        return {{}, QStringLiteral("Error: remote path context is not configured")};
    }
    if (conn == nullptr || conn->sftp() == nullptr) {
        return {{}, QStringLiteral("Error: remote SFTP client is not connected")};
    }
    const QString lexical = ctx->normalize(raw);
    QString       canonical;
    QString       err;
    switch (conn->sftp()->canonicalize(lexical, &canonical, &err)) {
    case QSocSftpClient::Canonical::Ok:
        return {canonical, {}};
    case QSocSftpClient::Canonical::Unresolvable:
    case QSocSftpClient::Canonical::Unknown:
        break;
    }
    return {{}, QStringLiteral("Error: %1").arg(err)};
}

/**
 * @brief Empty when @p canonicalPath may be written, the refusal otherwise.
 * @details Both sides are canonicalized on the host: comparing a canonical
 *          path against a lexical writable directory refuses every write in a
 *          workspace that is itself reached through a symlink. A writable
 *          directory the host cannot resolve grants nothing, and a host that
 *          cannot answer at all refuses rather than guesses.
 */
QString writableRefusal(
    QSocRemotePathContext *ctx, QSocSftpClient *sftp, const QString &canonicalPath)
{
    QStringList dirs;
    for (const QString &dir : ctx->writableDirs()) {
        QString canonical;
        QString err;
        switch (sftp->canonicalize(dir, &canonical, &err)) {
        case QSocSftpClient::Canonical::Ok:
            dirs.append(canonical);
            break;
        case QSocSftpClient::Canonical::Unresolvable:
            break;
        case QSocSftpClient::Canonical::Unknown:
            return QStringLiteral("Error: %1").arg(err);
        }
    }
    if (QSocRemotePathContext::isWithinAny(canonicalPath, dirs)) {
        return {};
    }
    return QStringLiteral("Error: remote path is outside writable directories: %1")
        .arg(canonicalPath);
}

/* Wrap a user command for a remote POSIX shell running under bash -lc. */
QString buildBashCommand(const QString &cwd, const QString &userCommand)
{
    auto shellEscape = [](const QString &value) {
        QString result = QStringLiteral("'");
        for (const QChar ch : value) {
            if (ch == QLatin1Char('\'')) {
                result += QStringLiteral("'\\''");
            } else {
                result += ch;
            }
        }
        result += QLatin1Char('\'');
        return result;
    };
    const QString cwdEscaped = shellEscape(cwd);
    const QString cmdEscaped = shellEscape(userCommand);
    return QStringLiteral("cd %1 && /bin/bash -lc %2").arg(cwdEscaped, cmdEscaped);
}

/* A command whose fate we know is ok or failed; one that was cut off is
 * uncertain, because the remote side may well have run it to completion.
 * A non-zero exit is a real answer, not a broken call. */
/* Refusal text for a session that cannot serve a call. "Not connected" is
 * wrong for a session whose socket is fine but whose protocol was stranded
 * mid-request, and neither case tells the reader what to do next. */
QString sessionRefusal(QSocRemoteConnection *conn)
{
    if (conn == nullptr) {
        return QStringLiteral("Error: SSH session is not connected");
    }
    return QStringLiteral("Error: %1; reconnect with /ssh").arg(conn->unusableText());
}

/* A write we could not confirm is uncertain, not failed. Reporting it as
 * failed invites a retry, and a retry of a write that actually landed is a
 * second application of the same change. */
QString sftpWriteError(QSocSftpClient *sftp, const QString &err)
{
    if (sftp != nullptr && sftp->lastFailureUncertain()) {
        return QSocTool::statusLine(QSocTool::ResultStatus::Uncertain) + QStringLiteral("error: ")
               + err + QLatin1Char('\n');
    }
    return QStringLiteral("Error: %1").arg(err);
}

/* Host detail the signal script captured from `kill`, empty when it said
 * nothing. */
QString scriptDetail(const QString &scriptOutput)
{
    const QStringList lines = scriptOutput.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("detail="))) {
            return trimmed.mid(QStringLiteral("detail=").size());
        }
    }
    return {};
}

QSocTool::ResultStatus remoteRunStatus(const QSocSshExec::Result &result)
{
    if (result.transportDead || result.timedOut || result.aborted) {
        return QSocTool::ResultStatus::Uncertain;
    }
    if (!result.errorText.isEmpty()) {
        return QSocTool::ResultStatus::Failed;
    }
    /* A killed process is a definite failure, not an uncertain one: the
     * command ran and did not finish. Its fate is known. */
    if (!result.exitSignal.isEmpty()) {
        return QSocTool::ResultStatus::Failed;
    }
    /* No signal, no error text, no flag, and still no status: the close
     * handshake never completed, so the command's fate was never reported.
     * Ok is derived from the absence of failure flags, so without this any
     * future path that leaves exitCode at -1 would read as a clean run. */
    if (result.exitCode < 0) {
        return QSocTool::ResultStatus::Uncertain;
    }
    return QSocTool::ResultStatus::Ok;
}

} // namespace

/* read_file */

QSocToolRemoteFileRead::QSocToolRemoteFileRead(
    QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_conn(conn)
    , m_pathCtx(pathCtx)
{}

QString QSocToolRemoteFileRead::getName() const
{
    return QStringLiteral("read_file");
}

QString QSocToolRemoteFileRead::getDescription() const
{
    return QStringLiteral(
        "Read the contents of a file on the remote workspace via SFTP. "
        "Paths are resolved against the remote working directory.");
}

json QSocToolRemoteFileRead::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"},
            {"description", "Remote path (absolute or relative to remote cwd)"}}},
          {"max_lines",
           {{"type", "integer"}, {"description", "Maximum number of lines to read (default: 500)"}}},
          {"offset",
           {{"type", "integer"},
            {"description", "Line number to start reading from (0-indexed, default: 0)"}}}}},
        {"required", json::array({"file_path"})}};
}

QString QSocToolRemoteFileRead::execute(const json &arguments)
{
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string()) {
        return QStringLiteral("Error: file_path is required");
    }
    const QString      raw      = QString::fromStdString(arguments["file_path"].get<std::string>());
    const ResolvedPath resolved = remoteResolve(m_pathCtx, m_conn, raw);
    if (!resolved.error.isEmpty()) {
        return resolved.error;
    }
    const QString remotePath = resolved.path;

    int maxLines = 500;
    int offset   = 0;
    if (arguments.contains("max_lines") && arguments["max_lines"].is_number_integer()) {
        maxLines = arguments["max_lines"].get<int>();
        if (maxLines <= 0) {
            maxLines = 500;
        }
    }
    if (arguments.contains("offset") && arguments["offset"].is_number_integer()) {
        offset = arguments["offset"].get<int>();
        if (offset < 0) {
            offset = 0;
        }
    }

    QString    err;
    QByteArray bytes = m_conn->sftp()->readFile(remotePath, 0, &err);
    if (bytes.isNull() && !err.isEmpty()) {
        return QStringLiteral("Error: %1").arg(err);
    }

    const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    QString           snippet;
    int               lineNum   = 0;
    int               emitted   = 0;
    bool              truncated = false;
    for (const QString &line : lines) {
        if (lineNum >= offset) {
            if (emitted >= maxLines) {
                truncated = true;
                break;
            }
            snippet += line + QLatin1Char('\n');
            ++emitted;
        }
        ++lineNum;
    }

    /* Record a full read so the remote edit_file / write_file tools can
     * enforce read-before-edit and detect on-disk changes. Partial / offset
     * reads do not qualify: the agent has not seen the whole file. */
    if (m_pathCtx && offset == 0 && !truncated) {
        m_pathCtx->readState().recordRead(remotePath, QString::fromUtf8(bytes));
    }

    if (snippet.isEmpty()) {
        return QStringLiteral("File is empty or offset beyond file length: %1").arg(remotePath);
    }
    return snippet;
}

/* write_file */

QSocToolRemoteFileWrite::QSocToolRemoteFileWrite(
    QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_conn(conn)
    , m_pathCtx(pathCtx)
{}

QString QSocToolRemoteFileWrite::getName() const
{
    return QStringLiteral("write_file");
}

QString QSocToolRemoteFileWrite::getDescription() const
{
    return QStringLiteral(
        "Write (or overwrite) a remote file via SFTP. The parent directory is "
        "created if missing. Overwriting an existing file requires reading it "
        "first with read_file; a file changed since the read is rejected. "
        "Writes are restricted to configured writable directories, checked "
        "against the path the host resolves, so a symlink leading out of them "
        "is refused.");
}

json QSocToolRemoteFileWrite::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"}, {"description", "Remote path (absolute or relative to cwd)"}}},
          {"content", {{"type", "string"}, {"description", "File content as UTF-8 text"}}}}},
        {"required", json::array({"file_path", "content"})}};
}

QString QSocToolRemoteFileWrite::execute(const json &arguments)
{
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string()) {
        return QStringLiteral("Error: file_path is required");
    }
    if (!arguments.contains("content") || !arguments["content"].is_string()) {
        return QStringLiteral("Error: content is required");
    }
    const QString      raw      = QString::fromStdString(arguments["file_path"].get<std::string>());
    const ResolvedPath resolved = remoteResolve(m_pathCtx, m_conn, raw);
    if (!resolved.error.isEmpty()) {
        return resolved.error;
    }
    const QString remotePath = resolved.path;
    const QString refusal    = writableRefusal(m_pathCtx, m_conn->sftp(), remotePath);
    if (!refusal.isEmpty()) {
        return refusal;
    }

    /* Read-before-overwrite + stale guard for EXISTING remote files: an
     * overwrite must not clobber content the agent never read or a
     * concurrent change. New files need no prior read. A stat we could not
     * complete must not be read as "new file", or the guard is skipped
     * exactly when the link is least trustworthy. */
    QString    presenceErr;
    const auto before = m_conn->sftp()->presence(remotePath, &presenceErr);
    if (before == QSocSftpClient::Presence::Unknown) {
        return QStringLiteral("Error: %1").arg(presenceErr);
    }
    const bool existedBefore = before == QSocSftpClient::Presence::Present;
    QString    beforeContent;
    if (existedBefore) {
        if (!m_pathCtx->readState().wasRead(remotePath)) {
            return QStringLiteral(
                       "Error: File not read yet: %1. Read it with read_file "
                       "before overwriting.")
                .arg(remotePath);
        }
        QString          readErr;
        const QByteArray current = m_conn->sftp()->readFile(remotePath, 0, &readErr);
        if (current.isNull() && !readErr.isEmpty()) {
            /* A transport error must not be misread as a concurrent change. */
            return QStringLiteral("Error: %1").arg(readErr);
        }
        beforeContent = QString::fromUtf8(current);
        if (m_pathCtx->readState().changedSinceRead(remotePath, beforeContent)) {
            return QStringLiteral(
                       "Error: File changed on disk since last read: %1. "
                       "Read it again before overwriting.")
                .arg(remotePath);
        }
    }

    /* Checkpoint the pre-write state so rewind can restore the remote file. */
    if (m_fileHistory != nullptr) {
        m_fileHistory->trackEdit(remotePath, existedBefore, beforeContent);
    }

    const QString content = QString::fromStdString(arguments["content"].get<std::string>());
    QString       err;
    if (!m_conn->sftp()->writeFile(remotePath, content.toUtf8(), &err)) {
        return sftpWriteError(m_conn->sftp(), err);
    }
    /* The written content is now the agent's known state. */
    m_pathCtx->readState().recordRead(remotePath, content);
    return QStringLiteral("Wrote %1 (%2 bytes) on remote")
        .arg(remotePath)
        .arg(content.toUtf8().size());
}

/* list_files */

QSocToolRemoteFileList::QSocToolRemoteFileList(
    QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_conn(conn)
    , m_pathCtx(pathCtx)
{}

QString QSocToolRemoteFileList::getName() const
{
    return QStringLiteral("list_files");
}

QString QSocToolRemoteFileList::getDescription() const
{
    return QStringLiteral("List files in a remote directory via SFTP.");
}

json QSocToolRemoteFileList::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"directory_path",
           {{"type", "string"},
            {"description", "Remote directory path (absolute or relative to cwd)"}}},
          {"limit",
           {{"type", "integer"}, {"description", "Maximum number of entries (default 200)"}}}}},
        {"required", json::array({"directory_path"})}};
}

QString QSocToolRemoteFileList::execute(const json &arguments)
{
    if (!arguments.contains("directory_path") || !arguments["directory_path"].is_string()) {
        return QStringLiteral("Error: directory_path is required");
    }
    const QString      raw = QString::fromStdString(arguments["directory_path"].get<std::string>());
    const ResolvedPath resolved = remoteResolve(m_pathCtx, m_conn, raw);
    if (!resolved.error.isEmpty()) {
        return resolved.error;
    }
    const QString remotePath = resolved.path;
    int           limit      = 200;
    if (arguments.contains("limit") && arguments["limit"].is_number_integer()) {
        limit = arguments["limit"].get<int>();
        if (limit <= 0) {
            limit = 200;
        }
    }
    QString    err;
    const auto entries = m_conn->sftp()->listDir(remotePath, limit, &err);
    if (entries.isEmpty() && !err.isEmpty()) {
        return QStringLiteral("Error: %1").arg(err);
    }
    QString out = QStringLiteral("Remote directory: %1\n").arg(remotePath);
    for (const auto &entry : entries) {
        out += QStringLiteral("%1 %2 %3\n")
                   .arg(entry.isDirectory ? QStringLiteral("d") : QStringLiteral("-"))
                   .arg(entry.size, 10)
                   .arg(entry.name);
    }
    return out;
}

/* edit_file */

QSocToolRemoteFileEdit::QSocToolRemoteFileEdit(
    QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_conn(conn)
    , m_pathCtx(pathCtx)
{}

QString QSocToolRemoteFileEdit::getName() const
{
    return QStringLiteral("edit_file");
}

QString QSocToolRemoteFileEdit::getDescription() const
{
    return QStringLiteral(
        "Edit a remote file by replacing a unique substring. Read the file with "
        "read_file first: editing an unread file, or one changed since the read, "
        "is rejected. Fails if the old string is missing or appears more than "
        "once, or if the path resolves outside the writable directories.");
}

json QSocToolRemoteFileEdit::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path", {{"type", "string"}, {"description", "Remote file path"}}},
          {"old_string", {{"type", "string"}, {"description", "Exact text to replace"}}},
          {"new_string", {{"type", "string"}, {"description", "Replacement text"}}}}},
        {"required", json::array({"file_path", "old_string", "new_string"})}};
}

QString QSocToolRemoteFileEdit::execute(const json &arguments)
{
    for (const char *key : {"file_path", "old_string", "new_string"}) {
        if (!arguments.contains(key) || !arguments[key].is_string()) {
            return QStringLiteral("Error: %1 is required").arg(QString::fromLatin1(key));
        }
    }
    const QString raw       = QString::fromStdString(arguments["file_path"].get<std::string>());
    const QString oldString = QString::fromStdString(arguments["old_string"].get<std::string>());
    const QString newString = QString::fromStdString(arguments["new_string"].get<std::string>());
    if (oldString == newString) {
        return QStringLiteral("Error: old_string and new_string are identical");
    }
    const ResolvedPath resolved = remoteResolve(m_pathCtx, m_conn, raw);
    if (!resolved.error.isEmpty()) {
        return resolved.error;
    }
    const QString remotePath = resolved.path;
    const QString refusal    = writableRefusal(m_pathCtx, m_conn->sftp(), remotePath);
    if (!refusal.isEmpty()) {
        return refusal;
    }
    QString          err;
    const QByteArray bytes = m_conn->sftp()->readFile(remotePath, 0, &err);
    if (bytes.isNull() && !err.isEmpty()) {
        return QStringLiteral("Error: %1").arg(err);
    }
    QString content = QString::fromUtf8(bytes);

    /* Read-before-edit + stale-on-disk guard: the agent must have read this
     * exact remote file first, and it must not have changed since, so an
     * edit never blindly clobbers unseen content or a concurrent change. */
    if (!m_pathCtx->readState().wasRead(remotePath)) {
        return QStringLiteral(
                   "Error: File not read yet: %1. Read it with read_file "
                   "before editing.")
            .arg(remotePath);
    }
    if (m_pathCtx->readState().changedSinceRead(remotePath, content)) {
        return QStringLiteral(
                   "Error: File changed on disk since last read: %1. "
                   "Read it again before editing.")
            .arg(remotePath);
    }

    const int first = content.indexOf(oldString);
    if (first < 0) {
        return QStringLiteral("Error: old_string not found in %1").arg(remotePath);
    }
    const int second = content.indexOf(oldString, first + oldString.size());
    if (second >= 0) {
        return QStringLiteral("Error: old_string is not unique in %1 (add more surrounding context)")
            .arg(remotePath);
    }
    /* Checkpoint the pre-edit content so rewind can restore the remote file. */
    if (m_fileHistory != nullptr) {
        m_fileHistory->trackEdit(remotePath, true, content);
    }
    content.replace(first, oldString.size(), newString);
    if (!m_conn->sftp()->writeFile(remotePath, content.toUtf8(), &err)) {
        return sftpWriteError(m_conn->sftp(), err);
    }
    /* The agent now knows the post-edit content. */
    m_pathCtx->readState().recordRead(remotePath, content);
    return QStringLiteral("Edited %1 on remote").arg(remotePath);
}

/* bash (shell) */

QSocToolRemoteShellBash::QSocToolRemoteShellBash(
    QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_conn(conn)
    , m_pathCtx(pathCtx)
{}

QString QSocToolRemoteShellBash::getName() const
{
    return QStringLiteral("bash");
}

QString QSocToolRemoteShellBash::getDescription() const
{
    return QStringLiteral("Execute a shell command on the remote workspace via SSH.");
}

json QSocToolRemoteShellBash::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"command", {{"type", "string"}, {"description", "Shell command to execute on remote"}}},
          {"timeout_ms",
           {{"type", "integer"},
            {"description", "Per-call timeout in milliseconds (default 60000)"}}},
          {"background",
           {{"type", "boolean"},
            {"description",
             "Run detached; return job_id. Use bash_manage for status/output/kill."}}}}},
        {"required", json::array({"command"})}};
}

QString QSocToolRemoteShellBash::execute(const json &arguments)
{
    if (m_conn == nullptr || !m_conn->isUsable()) {
        return sessionRefusal(m_conn);
    }
    if (!arguments.contains("command") || !arguments["command"].is_string()) {
        return QStringLiteral("Error: command is required");
    }
    const QString cmd       = QString::fromStdString(arguments["command"].get<std::string>());
    int           timeoutMs = 60000;
    if (arguments.contains("timeout_ms") && arguments["timeout_ms"].is_number_integer()) {
        timeoutMs = arguments["timeout_ms"].get<int>();
        if (timeoutMs <= 0) {
            timeoutMs = 60000;
        }
    }

    const QString cwd = (m_pathCtx != nullptr && !m_pathCtx->cwd().isEmpty()) ? m_pathCtx->cwd()
                                                                              : QStringLiteral("/");

    /* Background mode: spawn a detached job under
     * `<workspace>/.qsoc-agent/jobs/<id>/`, return job_id immediately. The
     * wrapper writes `pid`, `boot_id`, `pid_start`, `output.log` and
     * `exit_code`; bash_manage reads them back later and checks the recorded
     * identity before it signals anything. */
    const bool background = arguments.contains("background") && arguments["background"].is_boolean()
                            && arguments["background"].get<bool>();
    if (background) {
        if (m_pathCtx == nullptr || m_pathCtx->root().isEmpty()) {
            return QStringLiteral("Error: workspace root is not configured");
        }
        const QString jobsRoot = m_pathCtx->root() + QStringLiteral("/.qsoc-agent/jobs");
        /* The transport stamp keeps two jobs launched in the same millisecond
         * across a reconnect from sharing a directory. */
        const QString jobId  = QStringLiteral("%1-%2")
                                   .arg(m_conn->generation())
                                   .arg(QDateTime::currentMSecsSinceEpoch());
        const QString jobDir = jobsRoot + QLatin1Char('/') + jobId;

        QSocSshExec exec(*m_conn->session());
        m_running         = &exec;
        const auto result = exec.run(jobLaunchScript(jobDir, cwd, jobId, cmd), 10000);
        m_running         = nullptr;

        if (remoteRunStatus(result) == QSocTool::ResultStatus::Uncertain) {
            return composeJobUncertain(
                jobId,
                QStringLiteral(
                    "the launch did not complete over this link, so the job may or may "
                    "not be running"),
                QStringLiteral(
                    "check bash_manage(action=status) before launching the same work "
                    "again"));
        }
        if (result.exitCode != 0 || !result.errorText.isEmpty()) {
            return QStringLiteral("Error: job launch failed (exit %1) %2")
                .arg(result.exitCode)
                .arg(result.errorText);
        }
        const auto report = parseJobLaunchOutput(QString::fromUtf8(result.stdoutBytes));
        if (report.pid <= 0) {
            return composeJobUncertain(
                jobId,
                QStringLiteral("the host started the job but reported no pid for it"),
                QStringLiteral("read bash_manage(action=output); this job cannot be signalled"));
        }
        QSocRemoteJobRecord record;
        record.jobId        = jobId;
        record.commandLine  = cmd;
        record.generation   = m_conn->generation();
        record.bootIdentity = report.bootIdentity;
        record.pidStart     = report.pidStart;
        record.pid          = report.pid;
        record.launchedMs   = QDateTime::currentMSecsSinceEpoch();
        QString launched    = composeJobLaunchResult(record, jobDir);
        /* Recorded here or the id is unusable: bash_manage identifies a job by
         * what the ledger holds, and a re-observation brief names the ids it
         * wants checked from the same place. */
        if (!m_conn->jobs()->note(record)) {
            launched += jobLedgerFullNote();
        }
        return launched;
    }

    const QString wrapped = buildBashCommand(cwd, cmd);

    QSocSshExec exec(*m_conn->session());
    m_running         = &exec;
    const auto result = exec.run(wrapped, timeoutMs);
    m_running         = nullptr;

    /* Every field the reader needs to judge the call goes ahead of the
     * body. Metadata after an unbounded stdout is metadata nobody can find:
     * a truncated read stops before it and the call looks clean. */
    QString out = QSocTool::statusLine(remoteRunStatus(result));
    /* -1 is the header's "fate unknown", not a status the command returned,
     * so it is spelled out rather than printed as a number a reader would
     * take for one. */
    out += result.exitCode < 0 ? QStringLiteral("exit_code: unknown\n")
                               : QStringLiteral("exit_code: %1\n").arg(result.exitCode);
    if (!result.exitSignal.isEmpty()) {
        out += QStringLiteral("exit_signal: SIG%1\n").arg(result.exitSignal);
    }
    if (result.timedOut) {
        out += QStringLiteral("timed_out: true\n");
    }
    if (result.aborted) {
        out += QStringLiteral("aborted: true\n");
    }
    if (result.transportDead) {
        out += QStringLiteral("transport_dead: true\n");
    }
    if (!result.errorText.isEmpty()) {
        out += QStringLiteral("error: ") + result.errorText + QLatin1Char('\n');
    }
    if (!result.stdoutBytes.isEmpty()) {
        out += QStringLiteral("stdout:\n") + QString::fromUtf8(result.stdoutBytes);
        if (!out.endsWith(QLatin1Char('\n'))) {
            out += QLatin1Char('\n');
        }
    }
    if (!result.stderrBytes.isEmpty()) {
        out += QStringLiteral("stderr:\n") + QString::fromUtf8(result.stderrBytes);
        if (!out.endsWith(QLatin1Char('\n'))) {
            out += QLatin1Char('\n');
        }
    }
    return out;
}

void QSocToolRemoteShellBash::abort()
{
    if (m_running != nullptr) {
        m_running->requestAbort();
    }
}

/* path_context */

QSocToolRemotePath::QSocToolRemotePath(
    QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_conn(conn)
    , m_pathCtx(pathCtx)
{}

QString QSocToolRemotePath::getName() const
{
    return QStringLiteral("path_context");
}

QString QSocToolRemotePath::getDescription() const
{
    return QStringLiteral(
        "Report the remote workspace root, working directory, and writable "
        "directories. Action \"cwd\" changes the remote working directory.");
}

json QSocToolRemotePath::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", json::array({"show", "cwd"})},
            {"description", "\"show\" to report, \"cwd\" to change working dir"}}},
          {"path", {{"type", "string"}, {"description", "New cwd when action=cwd"}}}}}};
}

QString QSocToolRemotePath::execute(const json &arguments)
{
    if (m_pathCtx == nullptr) {
        return QStringLiteral("Error: remote path context is not configured");
    }
    const QString action = arguments.contains("action") && arguments["action"].is_string()
                               ? QString::fromStdString(arguments["action"].get<std::string>())
                               : QStringLiteral("show");
    if (action == QStringLiteral("cwd")) {
        if (!arguments.contains("path") || !arguments["path"].is_string()) {
            return QStringLiteral("Error: path is required for action=cwd");
        }
        if (m_conn == nullptr) {
            return QStringLiteral("Error: no remote workspace is bound");
        }
        const QString requested = QString::fromStdString(arguments["path"].get<std::string>());
        QString       why;
        /* Every refusal reads the same to the model, and they must: what it
         * has to know is that the move did not happen and where it still is.
         * Which of them it was lives in @p why. */
        if (m_conn->setWorkingDirectory(requested, &why)
            != QSocRemoteConnection::CwdChange::Changed) {
            return QStringLiteral("Error: %1. The working directory is unchanged and still %2.")
                .arg(why, m_pathCtx->cwd());
        }
    }
    QString out;
    out += QStringLiteral("remote_root: ") + m_pathCtx->root() + QLatin1Char('\n');
    out += QStringLiteral("remote_cwd : ") + m_pathCtx->cwd() + QLatin1Char('\n');
    const QStringList dirs = m_pathCtx->writableDirs();
    out += QStringLiteral("writable   :\n");
    for (const QString &dir : dirs) {
        out += QStringLiteral("  - ") + dir + QLatin1Char('\n');
    }
    return out;
}

/* bash_manage */

QSocToolRemoteBashManage::QSocToolRemoteBashManage(
    QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_conn(conn)
    , m_pathCtx(pathCtx)
{}

QString QSocToolRemoteBashManage::getName() const
{
    return QStringLiteral("bash_manage");
}

QString QSocToolRemoteBashManage::getDescription() const
{
    return QStringLiteral(
        "Manage a backgrounded remote command by job_id from bash(background=true). "
        "Actions: status, output, terminate (SIGTERM), kill (SIGKILL). A signal is sent "
        "only when the host still reports the boot identity and the process start time "
        "recorded at launch; otherwise the answer is uncertain and nothing is signalled.");
}

json QSocToolRemoteBashManage::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"job_id",
           {{"type", "string"}, {"description", "Job id returned by bash tool in background mode"}}},
          {"action",
           {{"type", "string"},
            {"enum", json::array({"status", "output", "terminate", "kill"})},
            {"description", "status / output / terminate / kill"}}},
          {"max_lines",
           {{"type", "integer"},
            {"description", "Max output lines to return for action=output (default 200)"}}}}},
        {"required", json::array({"job_id", "action"})}};
}

QString QSocToolRemoteBashManage::execute(const json &arguments)
{
    if (m_conn == nullptr || !m_conn->isUsable()) {
        return sessionRefusal(m_conn);
    }
    if (m_pathCtx == nullptr || m_pathCtx->root().isEmpty()) {
        return QStringLiteral("Error: workspace root is not configured");
    }
    if (!arguments.contains("job_id") || !arguments["job_id"].is_string()) {
        return QStringLiteral("Error: job_id is required");
    }
    if (!arguments.contains("action") || !arguments["action"].is_string()) {
        return QStringLiteral("Error: action is required");
    }
    const QString jobId  = QString::fromStdString(arguments["job_id"].get<std::string>());
    const QString action = QString::fromStdString(arguments["action"].get<std::string>());

    /* Reject path-escape attempts. Job ids are opaque tokens; a legitimate
     * id has no '/' or '..'. */
    if (jobId.contains(QLatin1Char('/')) || jobId.contains(QStringLiteral(".."))) {
        return QStringLiteral("Error: invalid job_id");
    }
    const QString jobDir = m_pathCtx->root() + QStringLiteral("/.qsoc-agent/jobs/") + jobId;
    /* What this session recorded for the id. An id it never handed out yields
     * a default record, which the scripts report as unverifiable. */
    const QSocRemoteJobRecord record = m_conn->jobs()->record(jobId);

    auto runShell = [&](const QString &shell, int timeoutMs) {
        QSocSshExec exec(*m_conn->session());
        return exec.run(shell, timeoutMs);
    };

    /* A job query that never reached the remote host says nothing about the
     * job. Reporting the empty stdout would read as "no output yet" or, for
     * terminate, as a kill that happened. */
    auto queryFailure = [](const QSocSshExec::Result &result) -> QString {
        const auto status = remoteRunStatus(result);
        if (status == QSocTool::ResultStatus::Ok) {
            return {};
        }
        QString detail = result.errorText;
        if (detail.isEmpty()) {
            detail = result.timedOut ? QStringLiteral("job query timed out")
                                     : QStringLiteral("job query did not complete");
        }
        return QSocTool::statusLine(status) + QStringLiteral("error: ") + detail
               + QStringLiteral("; job state is unknown\n");
    };

    /* Host stderr is evidence, so it follows the verdict rather than
     * replacing it. */
    auto stderrTail = [](const QSocSshExec::Result &result) {
        return result.stderrBytes.isEmpty()
                   ? QString()
                   : QStringLiteral("stderr:\n") + QString::fromUtf8(result.stderrBytes);
    };

    if (action == QStringLiteral("status")) {
        const auto    result  = runShell(jobStatusScript(jobDir, record), 5000);
        const QString failure = queryFailure(result);
        if (!failure.isEmpty()) {
            return failure;
        }
        const QString observed = QString::fromUtf8(result.stdoutBytes);
        const auto    token    = parseJobToken(observed);
        /* An exit code the host reported for a job it could identify is the one
         * thing that settles a record, and only a settled record can be evicted
         * to make room for the next launch. */
        if (token == QSocRemoteJobToken::Absent && !parseJobStatusExitCode(observed).isEmpty()) {
            m_conn->jobs()->markSettled(jobId);
        }
        const QString verdict = token == QSocRemoteJobToken::Absent
                                    ? QSocTool::statusLine(QSocTool::ResultStatus::Ok)
                                          + QStringLiteral("job_id: %1\n").arg(jobId)
                                    : composeJobRefusal(jobId, token);
        return verdict + jobScriptEvidence(observed) + stderrTail(result);
    }
    if (action == QStringLiteral("output")) {
        int maxLines = 200;
        if (arguments.contains("max_lines") && arguments["max_lines"].is_number_integer()) {
            maxLines = arguments["max_lines"].get<int>();
            if (maxLines <= 0) {
                maxLines = 200;
            }
        }
        const auto    result  = runShell(jobOutputScript(jobDir, maxLines, record), 10000);
        const QString failure = queryFailure(result);
        if (!failure.isEmpty()) {
            return failure;
        }
        /* A log that happens to open with "Error:" must not make the call read
         * as failed, so the verdict always leads. */
        const QString observed = QString::fromUtf8(result.stdoutBytes);
        const auto    token    = parseJobToken(observed);
        const QString verdict  = token == QSocRemoteJobToken::Absent
                                     ? QSocTool::statusLine(QSocTool::ResultStatus::Ok)
                                           + QStringLiteral("job_id: %1\n").arg(jobId)
                                     : composeJobRefusal(jobId, token);
        return verdict + jobScriptEvidence(observed);
    }
    if (action == QStringLiteral("terminate") || action == QStringLiteral("kill")) {
        const bool    hard       = action == QStringLiteral("kill");
        const QString signal     = hard ? QStringLiteral("-KILL") : QStringLiteral("-TERM");
        const QString signalName = hard ? QStringLiteral("SIGKILL") : QStringLiteral("SIGTERM");
        /* An id this session never handed out cannot be tied to a pid, and
         * refusing it here costs no round trip. */
        const QString unrecorded = refuseUnrecordedJob(jobId, *m_conn->jobs());
        if (!unrecorded.isEmpty()) {
            return unrecorded;
        }
        const auto    result  = runShell(jobSignalScript(record, signal), 5000);
        const QString failure = queryFailure(result);
        if (!failure.isEmpty()) {
            return failure;
        }
        const QString observed = QString::fromUtf8(result.stdoutBytes);
        const auto    judged
            = judgeSignal(jobId, signalName, parseJobToken(observed), scriptDetail(observed));
        /* The host compared the identities, so its verdict is what the ledger
         * follows. A proven restart means the record describes nothing; a
         * delivered signal means it still describes a live process under this
         * transport. */
        if (judged.boot == QSocRemoteIdentityMatch::Differs) {
            m_conn->jobs()->forget(jobId);
        } else if (judged.signalled) {
            m_conn->jobs()->rebind(jobId, m_conn->generation());
        }
        return judged.text + stderrTail(result);
    }
    return QStringLiteral("Error: unknown action '%1'").arg(action);
}
