// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsoctoolremote.h"

#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace {

QString remoteResolve(QSocRemotePathContext *ctx, const QString &raw, bool *ok = nullptr)
{
    if (ctx == nullptr) {
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return ctx->normalize(raw);
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

} // namespace

/* read_file */

QSocToolRemoteFileRead::QSocToolRemoteFileRead(
    QObject *parent, QSocSftpClient *sftp, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_sftp(sftp)
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
    const QString raw        = QString::fromStdString(arguments["file_path"].get<std::string>());
    bool          ok         = false;
    const QString remotePath = remoteResolve(m_pathCtx, raw, &ok);
    if (!ok) {
        return QStringLiteral("Error: remote path context is not configured");
    }
    if (m_sftp == nullptr) {
        return QStringLiteral("Error: remote SFTP client is not connected");
    }

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
    QByteArray bytes = m_sftp->readFile(remotePath, 0, &err);
    if (bytes.isNull() && !err.isEmpty()) {
        return QStringLiteral("Error: %1").arg(err);
    }

    const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    QString           snippet;
    int               lineNum = 0;
    int               emitted = 0;
    for (const QString &line : lines) {
        if (lineNum >= offset) {
            snippet += line + QLatin1Char('\n');
            ++emitted;
            if (emitted >= maxLines) {
                break;
            }
        }
        ++lineNum;
    }
    if (snippet.isEmpty()) {
        return QStringLiteral("File is empty or offset beyond file length: %1").arg(remotePath);
    }
    return snippet;
}

/* write_file */

QSocToolRemoteFileWrite::QSocToolRemoteFileWrite(
    QObject *parent, QSocSftpClient *sftp, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_sftp(sftp)
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
        "created if missing. Writes are restricted to configured writable directories.");
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
    const QString raw        = QString::fromStdString(arguments["file_path"].get<std::string>());
    bool          ok         = false;
    const QString remotePath = remoteResolve(m_pathCtx, raw, &ok);
    if (!ok) {
        return QStringLiteral("Error: remote path context is not configured");
    }
    if (m_sftp == nullptr) {
        return QStringLiteral("Error: remote SFTP client is not connected");
    }
    if (!m_pathCtx->isWritable(remotePath)) {
        return QStringLiteral("Error: remote path is outside writable directories: %1")
            .arg(remotePath);
    }
    const QString content = QString::fromStdString(arguments["content"].get<std::string>());
    QString       err;
    if (!m_sftp->writeFile(remotePath, content.toUtf8(), &err)) {
        return QStringLiteral("Error: %1").arg(err);
    }
    return QStringLiteral("Wrote %1 (%2 bytes) on remote")
        .arg(remotePath)
        .arg(content.toUtf8().size());
}

/* list_files */

QSocToolRemoteFileList::QSocToolRemoteFileList(
    QObject *parent, QSocSftpClient *sftp, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_sftp(sftp)
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
    const QString raw = QString::fromStdString(arguments["directory_path"].get<std::string>());
    bool          ok  = false;
    const QString remotePath = remoteResolve(m_pathCtx, raw, &ok);
    if (!ok) {
        return QStringLiteral("Error: remote path context is not configured");
    }
    if (m_sftp == nullptr) {
        return QStringLiteral("Error: remote SFTP client is not connected");
    }
    int limit = 200;
    if (arguments.contains("limit") && arguments["limit"].is_number_integer()) {
        limit = arguments["limit"].get<int>();
        if (limit <= 0) {
            limit = 200;
        }
    }
    QString    err;
    const auto entries = m_sftp->listDir(remotePath, limit, &err);
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
    QObject *parent, QSocSftpClient *sftp, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_sftp(sftp)
    , m_pathCtx(pathCtx)
{}

QString QSocToolRemoteFileEdit::getName() const
{
    return QStringLiteral("edit_file");
}

QString QSocToolRemoteFileEdit::getDescription() const
{
    return QStringLiteral(
        "Edit a remote file by replacing a unique substring. Fails if the old "
        "string is missing or appears more than once.");
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
    bool          ok         = false;
    const QString remotePath = remoteResolve(m_pathCtx, raw, &ok);
    if (!ok) {
        return QStringLiteral("Error: remote path context is not configured");
    }
    if (m_sftp == nullptr) {
        return QStringLiteral("Error: remote SFTP client is not connected");
    }
    if (!m_pathCtx->isWritable(remotePath)) {
        return QStringLiteral("Error: remote path is outside writable directories: %1")
            .arg(remotePath);
    }
    QString          err;
    const QByteArray bytes = m_sftp->readFile(remotePath, 0, &err);
    if (bytes.isNull() && !err.isEmpty()) {
        return QStringLiteral("Error: %1").arg(err);
    }
    QString   content = QString::fromUtf8(bytes);
    const int first   = content.indexOf(oldString);
    if (first < 0) {
        return QStringLiteral("Error: old_string not found in %1").arg(remotePath);
    }
    const int second = content.indexOf(oldString, first + oldString.size());
    if (second >= 0) {
        return QStringLiteral("Error: old_string is not unique in %1 (add more surrounding context)")
            .arg(remotePath);
    }
    content.replace(first, oldString.size(), newString);
    if (!m_sftp->writeFile(remotePath, content.toUtf8(), &err)) {
        return QStringLiteral("Error: %1").arg(err);
    }
    return QStringLiteral("Edited %1 on remote").arg(remotePath);
}

/* bash (shell) */

QSocToolRemoteShellBash::QSocToolRemoteShellBash(
    QObject *parent, QSocSshSession *session, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_session(session)
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
    if (m_session == nullptr || !m_session->isConnected()) {
        return QStringLiteral("Error: SSH session is not connected");
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
     * wrapper writes `pid`, `output.log`, and `exit_code`; bash_manage reads
     * them back later. */
    const bool background = arguments.contains("background") && arguments["background"].is_boolean()
                            && arguments["background"].get<bool>();
    if (background) {
        if (m_pathCtx == nullptr || m_pathCtx->root().isEmpty()) {
            return QStringLiteral("Error: workspace root is not configured");
        }
        const QString jobsRoot = m_pathCtx->root() + QStringLiteral("/.qsoc-agent/jobs");
        const QString jobId    = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString jobDir   = jobsRoot + QLatin1Char('/') + jobId;
        const QString wrapper
            = QStringLiteral(
                  "set -e; "
                  "mkdir -p %1; "
                  "cd %2; "
                  "printf %s %3 > %1/command; "
                  "date +%s > %1/start_time; "
                  "nohup /bin/bash -lc %3 > %1/output.log 2>&1 & "
                  "echo $! > %1/pid; "
                  "wait $! ; printf %d $? > %1/exit_code")
                  .arg(jobDir)
                  .arg(cwd)
                  .arg(
                      QStringLiteral("'")
                      + QString(cmd).replace(QLatin1Char('\''), QStringLiteral("'\\''"))
                      + QStringLiteral("'"));
        /* Fire-and-forget the wrapper: disown the background pipeline so the
         * SSH exec channel closes immediately after the shell writes the pid. */
        const QString launch
            = QStringLiteral("mkdir -p %1 && (%2) >/dev/null 2>&1 & disown").arg(jobDir, wrapper);

        QSocSshExec exec(*m_session);
        m_running         = &exec;
        const auto result = exec.run(launch, 10000);
        m_running         = nullptr;
        if (result.exitCode != 0 || !result.errorText.isEmpty()) {
            return QStringLiteral("Error: job launch failed (exit %1) %2")
                .arg(result.exitCode)
                .arg(result.errorText);
        }
        return QStringLiteral("job_id: %1\njob_dir: %2\n").arg(jobId, jobDir);
    }

    const QString wrapped = buildBashCommand(cwd, cmd);

    QSocSshExec exec(*m_session);
    m_running         = &exec;
    const auto result = exec.run(wrapped, timeoutMs);
    m_running         = nullptr;

    QString out;
    out += QStringLiteral("exit_code: %1\n").arg(result.exitCode);
    if (result.timedOut) {
        out += QStringLiteral("timed_out: true\n");
    }
    if (result.aborted) {
        out += QStringLiteral("aborted: true\n");
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
    if (!result.errorText.isEmpty()) {
        out += QStringLiteral("error: ") + result.errorText + QLatin1Char('\n');
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

QSocToolRemotePath::QSocToolRemotePath(QObject *parent, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
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
        const QString requested = QString::fromStdString(arguments["path"].get<std::string>());
        const QString resolved  = m_pathCtx->resolveCwdRequest(requested);
        m_pathCtx->setCwd(resolved);
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
    QObject *parent, QSocSshSession *session, QSocRemotePathContext *pathCtx)
    : QSocTool(parent)
    , m_session(session)
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
        "Actions: status, output, terminate (SIGTERM), kill (SIGKILL).");
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
    if (m_session == nullptr || !m_session->isConnected()) {
        return QStringLiteral("Error: SSH session is not connected");
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

    auto runShell = [&](const QString &shell, int timeoutMs) {
        QSocSshExec exec(*m_session);
        return exec.run(shell, timeoutMs);
    };

    if (action == QStringLiteral("status")) {
        const QString script
            = QStringLiteral(
                  "cd %1 2>/dev/null || { echo 'no_job'; exit 0; }; "
                  "pid=$(cat pid 2>/dev/null); "
                  "start=$(cat start_time 2>/dev/null); "
                  "exit_code=$(cat exit_code 2>/dev/null); "
                  "bytes=$(stat -c%s output.log 2>/dev/null || echo 0); "
                  "running=no; "
                  "[ -n \"$pid\" ] && kill -0 \"$pid\" 2>/dev/null && running=yes; "
                  "printf "
                  "'pid=%s\\nstart_time=%s\\nrunning=%s\\nexit_code=%s\\noutput_bytes=%s\\n' "
                  "\"$pid\" \"$start\" \"$running\" \"$exit_code\" \"$bytes\"")
                  .arg(jobDir);
        const auto result = runShell(script, 5000);
        return QString::fromUtf8(result.stdoutBytes)
               + (result.stderrBytes.isEmpty()
                      ? QString()
                      : QStringLiteral("stderr:\n") + QString::fromUtf8(result.stderrBytes));
    }
    if (action == QStringLiteral("output")) {
        int maxLines = 200;
        if (arguments.contains("max_lines") && arguments["max_lines"].is_number_integer()) {
            maxLines = arguments["max_lines"].get<int>();
            if (maxLines <= 0) {
                maxLines = 200;
            }
        }
        const QString script
            = QStringLiteral("tail -n %1 %2/output.log 2>&1 || true").arg(maxLines).arg(jobDir);
        const auto result = runShell(script, 10000);
        return QString::fromUtf8(result.stdoutBytes);
    }
    if (action == QStringLiteral("terminate") || action == QStringLiteral("kill")) {
        const QString signal = action == QStringLiteral("kill") ? QStringLiteral("-KILL")
                                                                : QStringLiteral("-TERM");
        const QString script = QStringLiteral(
                                   "pid=$(cat %1/pid 2>/dev/null); "
                                   "if [ -n \"$pid\" ]; then kill %2 \"$pid\" 2>&1; fi; "
                                   "echo done")
                                   .arg(jobDir, signal);
        const auto    result = runShell(script, 5000);
        return QString::fromUtf8(result.stdoutBytes)
               + (result.stderrBytes.isEmpty()
                      ? QString()
                      : QStringLiteral("stderr:\n") + QString::fromUtf8(result.stderrBytes));
    }
    return QStringLiteral("Error: unknown action '%1'").arg(action);
}
