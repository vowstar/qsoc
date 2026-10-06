// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsoctoolremote.h"

#include "agent/qsocagent.h"
#include "agent/qsocfilehistory.h"
#include "agent/qsocshellcommand.h"
#include "agent/qsoctaskeventqueue.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotejobs.h"
#include "agent/remote/qsocremotejobwatcher.h"
#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/tool/qsoctoolfilecore.h"
#include "agent/tool/qsoctoolpath.h"
#include "common/qllmservice.h"
#include "common/qsocboundedcapture.h"
#include "common/qsocshellpath.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace {

/** @brief One resolved path argument, or the refusal to hand back. */
struct ResolvedPath
{
    QString path;
    QString error;
};

enum class RemoteJobPathUse : std::uint8_t {
    Create,
    Inspect,
};

QString remoteChildPath(const QString &parent, const QString &child)
{
    return parent == QStringLiteral("/") ? parent + child : parent + QLatin1Char('/') + child;
}

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
    conn->learnHome(raw);
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

namespace Core = QSocToolFileCore;

const QString kWriteScope = QStringLiteral("the writable directories path_context lists");

/* The refusal for @p path when it is not a regular file, or empty. */
QString regularFileRefusal(QSocSftpClient *sftp, const QString &path)
{
    QSocSftpClient::LinkStat stat;
    QString                  err;
    switch (sftp->linkStat(path, &stat, &err)) {
    case QSocSftpClient::Presence::Absent:
        return Core::fileNotFound(path);
    case QSocSftpClient::Presence::Unknown:
        return QStringLiteral("Error: %1").arg(err);
    case QSocSftpClient::Presence::Present:
        break;
    }
    return stat.regular ? QString() : Core::notAFile(path);
}

/* One SFTP directory listing in the shared entry form. */
bool listRemoteDir(
    QSocSftpClient *sftp, const QString &dir, QList<Core::ListEntry> *entries, QString *error)
{
    const QList<QSocSftpClient::Entry> found = sftp->listDir(dir, 0, error);
    if (found.isEmpty() && !error->isEmpty()) {
        return false;
    }
    for (const QSocSftpClient::Entry &entry : found) {
        entries->append(
            {.name        = entry.name,
             .isDirectory = entry.isDirectory,
             .isSymlink   = entry.isSymlink,
             .hidden      = entry.name.startsWith(QLatin1Char('.'))});
    }
    return true;
}

/* `<workspace>/.qsoc-agent/jobs`, verified to resolve to itself; created
 * first for RemoteJobPathUse::Create. */
ResolvedPath verifyJobsRoot(QSocRemoteConnection *conn, RemoteJobPathUse use)
{
    if (conn == nullptr || conn->sftp() == nullptr) {
        return {{}, QStringLiteral("remote job storage is not connected")};
    }

    const QString metadata = remoteChildPath(conn->path()->root(), QStringLiteral(".qsoc-agent"));
    const QString requestedJobsRoot = remoteChildPath(metadata, QStringLiteral("jobs"));
    const QString canonicalMetadata
        = remoteChildPath(conn->canonicalWorkspace(), QStringLiteral(".qsoc-agent"));
    const QString expectedJobsRoot = remoteChildPath(canonicalMetadata, QStringLiteral("jobs"));

    QString jobsRoot;
    QString err;
    if (!conn->resolveWritablePath(requestedJobsRoot, &jobsRoot, &err)) {
        return {{}, err};
    }
    if (jobsRoot != expectedJobsRoot) {
        return {{}, QStringLiteral("remote job storage changed identity")};
    }
    if (use == RemoteJobPathUse::Inspect) {
        return {jobsRoot, {}};
    }
    if (!conn->sftp()->mkdirP(jobsRoot, &err)) {
        return {{}, err.isEmpty() ? QStringLiteral("remote job storage cannot be created") : err};
    }
    QString verifiedJobsRoot;
    if (!conn->resolveWritablePath(requestedJobsRoot, &verifiedJobsRoot, &err)) {
        return {{}, err};
    }
    if (verifiedJobsRoot != expectedJobsRoot) {
        return {{}, QStringLiteral("remote job storage changed identity")};
    }
    return {verifiedJobsRoot, {}};
}

ResolvedPath resolveRemoteJobPath(QSocRemoteConnection *conn, const QString &jobId)
{
    const ResolvedPath root = verifyJobsRoot(conn, RemoteJobPathUse::Inspect);
    if (!root.error.isEmpty()) {
        return root;
    }
    const QString expectedJobDir = remoteChildPath(root.path, jobId);
    QString       jobDir;
    QString       err;
    if (!conn->resolveWritablePath(expectedJobDir, &jobDir, &err)) {
        return {{}, err};
    }
    if (jobDir != expectedJobDir) {
        return {{}, QStringLiteral("remote job directory changed identity")};
    }

    switch (conn->sftp()->linkPresence(jobDir, &err)) {
    case QSocSftpClient::Presence::Absent:
        return {jobDir, {}};
    case QSocSftpClient::Presence::Unknown:
        return {{}, err.isEmpty() ? QStringLiteral("remote job directory cannot be inspected") : err};
    case QSocSftpClient::Presence::Present:
        break;
    }

    static const QStringList stateFiles{
        QStringLiteral("command"),
        QStringLiteral("start_time"),
        QStringLiteral("boot_id"),
        QStringLiteral("pid"),
        QStringLiteral("pid_start"),
        QStringLiteral("exit_code"),
        QStringLiteral("output.log"),
    };
    for (const QString &name : stateFiles) {
        const QString expected = remoteChildPath(jobDir, name);
        QString       resolved;
        if (!conn->resolveWritablePath(expected, &resolved, &err)) {
            return {{}, err};
        }
        if (resolved != expected) {
            return {{}, QStringLiteral("remote job state changed identity: %1").arg(name)};
        }
    }
    return {jobDir, {}};
}

/* Refusal for an exec tool on a host the probe found no shell on. Registry
 * shaping keeps these tools away from such a host; this covers a reconnect
 * that probed a different answer than the bind did. */
QString noExecutorRefusal(QSocRemoteConnection *conn)
{
    const QSocMachine &host = conn->host();
    const QString      why  = host.shellError.isEmpty() ? host.summary() : host.shellError;
    if (host.kind == QSocMachine::Kind::Unknown) {
        return QStringLiteral(
                   "Error: the shell of this host is unknown (%1); file tools still work, and "
                   "a reconnect with /ssh probes again")
            .arg(why);
    }
    return QStringLiteral("Error: this host has no usable shell to run commands (%1)").arg(why);
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

/* What a `!` line prints: its output, then how it ended, one item a line. */
QString shellEscapeText(const QSocSshExec::Result &result)
{
    QString    output;
    const auto append = [&output](const QString &text) {
        output += text;
        if (!output.isEmpty() && !output.endsWith(QLatin1Char('\n'))) {
            output += QLatin1Char('\n');
        }
    };
    append(QSocShellPath::decodeConsoleOutput(result.stdoutBytes));
    append(QSocShellPath::decodeConsoleOutput(result.stderrBytes));
    if (result.timedOut) {
        append(QStringLiteral("(timed out)"));
    } else if (result.aborted) {
        append(QStringLiteral("(aborted)"));
    } else if (result.exitCode != 0) {
        append(QStringLiteral("(exit code: %1)").arg(result.exitCode));
    }
    if (!result.errorText.isEmpty()) {
        append(QStringLiteral("(%1)").arg(result.errorText));
    }
    return output;
}

/* A job no live agent started belongs to the user. */
QString jobOwner(const QSocToolCallContext *context)
{
    const auto *agent = context == nullptr ? nullptr
                                           : qobject_cast<QSocAgent *>(context->executionScope());
    return agent != nullptr ? agent->agentIdentity() : QSocTaskEvent::userOwner();
}

} // namespace

namespace {

/* Place @p start's job under the binding's jobs root. The root is checked over
 * SFTP once per transport; the launch script creates the job directory and
 * refuses when the root stopped being a plain directory since. */
void reserveRemoteJob(QSocRemoteConnection *conn, QSocRemoteJobStart *start)
{
    if (conn->path()->root().isEmpty()) {
        start->failure = QStringLiteral("Error: workspace root is not configured");
        return;
    }
    QString root = conn->verifiedJobsRoot();
    if (root.isEmpty()) {
        const ResolvedPath verified = verifyJobsRoot(conn, RemoteJobPathUse::Create);
        if (!verified.error.isEmpty()) {
            start->failure = QStringLiteral("Error: %1").arg(verified.error);
            return;
        }
        root = verified.path;
        conn->setVerifiedJobsRoot(root);
    }
    start->failure.clear();
    start->jobDir = remoteChildPath(root, start->record.jobId);
}

/* The launch found the jobs root replaced, or gone, and started nothing. */
bool jobsRootRefused(const QSocSshExec::Result &result)
{
    return result.exitCode == 3 && result.stdoutBytes.contains("token: jobs_root_moved");
}

/* Run @p launch for @p start; a cached root the host refused is verified
 * again and the launch retried once. */
QSocSshExec::Result launchRemoteJob(
    QSocRemoteConnection                                         *conn,
    QSocRemoteJobStart                                           *start,
    const std::function<QSocSshExec::Result(const QString &dir)> &launch)
{
    const bool cached = !conn->verifiedJobsRoot().isEmpty();
    auto       result = launch(start->jobDir);
    if (!cached || !jobsRootRefused(result)) {
        return result;
    }
    conn->setVerifiedJobsRoot({});
    reserveRemoteJob(conn, start);
    return start->failure.isEmpty() ? launch(start->jobDir) : result;
}

void recordRemoteJob(
    QSocRemoteConnection            *conn,
    QSocRemoteJobStart              *start,
    const QSocRemoteJobLaunchReport &report,
    const QString                   &command)
{
    start->record.commandLine  = command;
    start->record.generation   = conn->generation();
    start->record.bootIdentity = report.bootIdentity;
    start->record.pidStart     = report.pidStart;
    start->record.pid          = report.pid;
    start->record.launchedMs   = QDateTime::currentMSecsSinceEpoch();
    /* Recorded here or the id is unusable: bash_manage identifies a job by
     * what the ledger holds, and the watcher polls exactly those. */
    start->noted = conn->jobs()->note(start->record);
    conn->watcher()->kick();
}

} // namespace

QSocRemoteJobStart startRemoteJob(
    QSocRemoteConnection *conn,
    const QString        &cwd,
    const QString        &command,
    bool                  monitor,
    const QString        &ownerId,
    QSocSshExec         **running,
    qint64                maxOutputBytes)
{
    QSocRemoteJobStart start;
    start.record.jobId          = newRemoteJobId();
    start.record.monitor        = monitor;
    start.record.ownerId        = ownerId;
    start.record.maxOutputBytes = maxOutputBytes;
    reserveRemoteJob(conn, &start);
    if (!start.failure.isEmpty()) {
        return start;
    }
    const auto result = launchRemoteJob(conn, &start, [&](const QString &jobDir) {
        const QSocRemoteExec launch = remoteScriptExec(
            conn->host(),
            jobLaunchScript(
                machineShellPath(conn->host(), jobDir),
                machineShellPath(conn->host(), cwd),
                start.record.jobId,
                command,
                conn->host().shell,
                monitor),
            false);
        QSocSshExec exec(*conn->session());
        if (running != nullptr) {
            *running = &exec;
        }
        auto ran = exec.run(launch.command, 10000, launch.input);
        if (running != nullptr) {
            *running = nullptr;
        }
        return ran;
    });

    if (remoteRunStatus(result) == QSocTool::ResultStatus::Uncertain) {
        start.failure = composeJobUncertain(
            start.record.jobId,
            QStringLiteral(
                "the launch did not complete over this link, so the job may or may not be "
                "running"),
            QStringLiteral("check bash_manage(action=status) before launching the same work again"));
        return start;
    }
    if (result.exitCode != 0 || !result.errorText.isEmpty()) {
        start.failure = QStringLiteral("Error: job launch failed (exit %1) %2")
                            .arg(result.exitCode)
                            .arg(result.errorText);
        return start;
    }
    const auto report = parseJobLaunchOutput(QString::fromUtf8(result.stdoutBytes));
    if (report.pid <= 0) {
        start.failure = composeJobUncertain(
            start.record.jobId,
            QStringLiteral("the host started the job but reported no pid for it"),
            QStringLiteral("read bash_manage(action=output); this job cannot be signalled"));
        return start;
    }
    recordRemoteJob(conn, &start, report, command);
    return start;
}

QString runBoundRemoteShellEscape(
    QSocRemoteConnection *conn, const QString &command, std::optional<int> *exitCode)
{
    if (conn == nullptr || !conn->isUsable()) {
        return sessionRefusal(conn) + QLatin1Char('\n');
    }

    constexpr int      kRemoteShellEscapeMs = 15 * 60 * 1000;
    constexpr qint64   kCaptureBytes        = QSocBoundedCapture::kDefaultLimit / 2;
    QSocSshExec        exec(*conn->session());
    const QSocMachine &host   = conn->host();
    const auto         report = [exitCode](const QSocSshExec::Result &result) {
        const bool exited = !result.transportDead && !result.timedOut && !result.aborted
                            && result.exitSignal.isEmpty() && result.exitCode >= 0;
        if (exitCode != nullptr && exited)
            *exitCode = result.exitCode;
        return shellEscapeText(result);
    };
    if (machineShellEscapeMode(host) == QSocShellEscapeMode::Passthrough) {
        return remoteShellEscapePassthroughNotice() + QLatin1Char('\n')
               + report(exec.run(command, kRemoteShellEscapeMs, {}, kCaptureBytes));
    }

    QString cwd;
    QString err;
    if (!conn->resolveBoundCwd(&cwd, &err)) {
        return QStringLiteral("Error: %1\n").arg(err);
    }
    const QSocRemoteExec request = remoteShellEscapeExec(host, cwd, command);
    return report(exec.run(request.command, kRemoteShellEscapeMs, request.input, kCaptureBytes))
           + QStringLiteral("(shell: %1)\n").arg(shellEscapeShellName(host));
}

/* read_file */

QSocToolRemoteFileRead::QSocToolRemoteFileRead(
    QObject *parent, QSocRemoteConnection *conn, QSocRemotePathContext *pathCtx, QLLMService *llm)
    : QSocTool(parent)
    , m_conn(conn)
    , m_pathCtx(pathCtx)
    , m_llm(llm)
{}

QString QSocToolRemoteFileRead::getName() const
{
    return QStringLiteral("read_file");
}

QString QSocToolRemoteFileRead::getDescription() const
{
    return Core::readDescription(m_llm != nullptr && m_llm->currentSupportsImage());
}

json QSocToolRemoteFileRead::getParametersSchema() const
{
    return Core::readSchema();
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
    if (const QString refusal = regularFileRefusal(m_conn->sftp(), remotePath); !refusal.isEmpty()) {
        return refusal;
    }

    Core::Reader reader(Core::readWindow(arguments));
    QString      err;
    if (!m_conn->sftp()->readStream(
            remotePath, [&reader](const QByteArray &chunk) { return reader.feed(chunk); }, &err)) {
        return QStringLiteral("Error: %1").arg(err);
    }
    reader.finish();

    /* Record a full read so the remote edit_file / write_file tools can
     * enforce read-before-edit and detect on-disk changes. Partial / offset
     * reads do not qualify: the agent has not seen the whole file. */
    if (const auto whole = reader.wholeFile(); m_pathCtx && whole) {
        m_pathCtx->readState().recordRead(remotePath, QString::fromUtf8(*whole));
    }
    return reader.result(remotePath, m_llm);
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
    return Core::writeDescription(kWriteScope);
}

json QSocToolRemoteFileWrite::getParametersSchema() const
{
    return Core::writeSchema();
}

QString QSocToolRemoteFileWrite::execute(const json &arguments)
{
    const Core::WriteCall call = Core::writeCall(arguments);
    if (!call.error.isEmpty()) {
        return call.error;
    }
    if (m_conn == nullptr) {
        return QStringLiteral("Error: remote SSH connection is not configured");
    }
    QString remotePath;
    QString resolveError;
    m_conn->learnHome(call.path);
    if (!m_conn->resolveWritablePath(call.path, &remotePath, &resolveError)) {
        return QStringLiteral("Error: %1").arg(resolveError);
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
            return Core::notReadYet(remotePath, QStringLiteral("overwriting"));
        }
        QString          readErr;
        const QByteArray current = m_conn->sftp()->readFile(remotePath, 0, &readErr);
        if (current.isNull() && !readErr.isEmpty()) {
            /* A transport error must not be misread as a concurrent change. */
            return QStringLiteral("Error: %1").arg(readErr);
        }
        beforeContent = QString::fromUtf8(current);
        if (m_pathCtx->readState().changedSinceRead(remotePath, beforeContent)) {
            return Core::changedSinceRead(remotePath, QStringLiteral("overwriting"));
        }
    }

    /* Checkpoint the pre-write state so rewind can restore the remote file. */
    if (m_fileHistory != nullptr && m_fileHistory->isPathInScope(remotePath)
        && !m_fileHistory->trackEdit(remotePath, existedBefore, beforeContent)) {
        return QStringLiteral("Error: Cannot save file history before writing: %1").arg(remotePath);
    }

    const QByteArray bytes = call.content.toUtf8();
    QString          err;
    if (!m_conn->sftp()->writeFile(remotePath, bytes, &err)) {
        return sftpWriteError(m_conn->sftp(), err);
    }
    /* The written content is now the agent's known state. */
    m_pathCtx->readState().recordRead(remotePath, call.content);
    return Core::wroteText(bytes.size(), remotePath);
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
    return Core::listDescription();
}

json QSocToolRemoteFileList::getParametersSchema() const
{
    return Core::listSchema();
}

QString QSocToolRemoteFileList::execute(const json &arguments)
{
    const Core::ListCall call     = Core::listCall(arguments);
    const ResolvedPath   resolved = remoteResolve(m_pathCtx, m_conn, call.directory);
    if (!resolved.error.isEmpty()) {
        return resolved.error;
    }
    QSocSftpClient *sftp = m_conn->sftp();
    QString         err;
    switch (sftp->presence(resolved.path, &err)) {
    case QSocSftpClient::Presence::Absent:
        return Core::directoryNotFound(resolved.path);
    case QSocSftpClient::Presence::Unknown:
        return QStringLiteral("Error: %1").arg(err);
    case QSocSftpClient::Presence::Present:
        break;
    }
    return Core::listFiles(
        resolved.path,
        call,
        m_pathCtx->pathCase() == Qt::CaseSensitive,
        [sftp](const QString &dir, QList<Core::ListEntry> *entries, QString *error) {
            return listRemoteDir(sftp, dir, entries, error);
        });
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
    return Core::editDescription(kWriteScope);
}

json QSocToolRemoteFileEdit::getParametersSchema() const
{
    return Core::editSchema();
}

QString QSocToolRemoteFileEdit::execute(const json &arguments)
{
    Core::EditCall call = Core::editCall(arguments);
    if (!call.error.isEmpty()) {
        return call.error;
    }
    if (m_conn == nullptr) {
        return QStringLiteral("Error: remote SSH connection is not configured");
    }
    QString remotePath;
    QString resolveError;
    m_conn->learnHome(call.path);
    if (!m_conn->resolveWritablePath(call.path, &remotePath, &resolveError)) {
        return QStringLiteral("Error: %1").arg(resolveError);
    }
    call.path = remotePath;
    if (const QString refusal = regularFileRefusal(m_conn->sftp(), remotePath); !refusal.isEmpty()) {
        return refusal;
    }
    QString          err;
    const QByteArray bytes = m_conn->sftp()->readFile(remotePath, 0, &err);
    if (bytes.isNull() && !err.isEmpty()) {
        return QStringLiteral("Error: %1").arg(err);
    }
    const QString content = QString::fromUtf8(bytes);

    /* Read-before-edit + stale-on-disk guard: the agent must have read this
     * exact remote file first, and it must not have changed since, so an
     * edit never blindly clobbers unseen content or a concurrent change. */
    if (!m_pathCtx->readState().wasRead(remotePath)) {
        return Core::notReadYet(remotePath, QStringLiteral("editing"));
    }
    if (m_pathCtx->readState().changedSinceRead(remotePath, content)) {
        return Core::changedSinceRead(remotePath, QStringLiteral("editing"));
    }

    const Core::EditOutcome edit = Core::applyEdit(content, call);
    if (!edit.error.isEmpty()) {
        return edit.error;
    }
    /* Checkpoint the pre-edit content so rewind can restore the remote file. */
    if (m_fileHistory != nullptr && m_fileHistory->isPathInScope(remotePath)
        && !m_fileHistory->trackEdit(remotePath, true, content)) {
        return QStringLiteral("Error: Cannot save file history before editing: %1").arg(remotePath);
    }
    if (!m_conn->sftp()->writeFile(remotePath, edit.content.toUtf8(), &err)) {
        return sftpWriteError(m_conn->sftp(), err);
    }
    /* The agent now knows the post-edit content. */
    m_pathCtx->readState().recordRead(remotePath, edit.content);
    return Core::editedText(remotePath, edit.count);
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
    QString text = QStringLiteral("Execute a shell command on the remote workspace via SSH.");
    if (m_conn != nullptr && m_conn->host().shell.kind == QSocShellExecutor::Kind::Sh) {
        text += QStringLiteral(
            " Only POSIX sh is available on this host: write POSIX sh, not bash syntax.");
    }
    if (m_conn != nullptr && m_conn->host().shell.kind == QSocShellExecutor::Kind::GitBash) {
        text += QLatin1Char(' ') + gitBashGuidance();
    }
    return text;
}

json QSocToolRemoteShellBash::getParametersSchema() const
{
    return QSocShellCommand::bashSchema();
}

QString QSocToolRemoteShellBash::execute(const json &arguments)
{
    m_stopRequested = false;
    if (m_conn == nullptr || !m_conn->isUsable()) {
        return sessionRefusal(m_conn);
    }
    if (!arguments.contains("command") || !arguments["command"].is_string()) {
        return QStringLiteral("Error: command is required");
    }
    if (!machineOffersExecTools(m_conn->host())) {
        return noExecutorRefusal(m_conn);
    }
    const QString cmd       = QString::fromStdString(arguments["command"].get<std::string>());
    const int     timeoutMs = QSocShellCommand::timeoutArgument(arguments);
    qint64        maxOutput = kDefaultJobOutputBytes;
    if (arguments.contains("max_output") && arguments["max_output"].is_number_integer()
        && arguments["max_output"].get<long long>() > 0) {
        maxOutput = arguments["max_output"].get<long long>();
    }

    QString cwd;
    QString resolveError;
    if (arguments.contains("working_directory") && arguments["working_directory"].is_string()
        && !arguments["working_directory"].get<std::string>().empty()) {
        const ResolvedPath dir = remoteResolve(
            m_pathCtx,
            m_conn,
            QString::fromStdString(arguments["working_directory"].get<std::string>()));
        if (!dir.error.isEmpty()) {
            return dir.error;
        }
        cwd = dir.path;
    } else if (!m_conn->resolveBoundCwd(&cwd, &resolveError)) {
        return QStringLiteral("Error: %1").arg(resolveError);
    }

    /* Background mode: spawn a detached job under
     * `<workspace>/.qsoc-agent/jobs/<id>/`, return job_id immediately. The
     * wrapper writes `pid`, `boot_id`, `pid_start`, `output.log` and
     * `exit_code`; bash_manage reads them back later and checks the recorded
     * identity before it signals anything. */
    const bool background = arguments.contains("background") && arguments["background"].is_boolean()
                            && arguments["background"].get<bool>();
    if (background) {
        const QSocRemoteJobStart start = startRemoteJob(
            m_conn, cwd, cmd, false, jobOwner(currentCallContext()), &m_running, maxOutput);
        if (!start.failure.isEmpty()) {
            return start.failure;
        }
        QString launched = composeJobLaunchResult(start.record, start.jobDir);
        if (!start.noted) {
            launched += jobLedgerFullNote();
        }
        return launched;
    }

    QSocRemoteJobStart start;
    start.record.jobId = newRemoteJobId();
    reserveRemoteJob(m_conn, &start);
    if (m_stopRequested) {
        return QSocShellCommand::abortedText();
    }
    /* Without job storage (a read-only workspace) the command still runs; it
     * just cannot outlive the call. */
    if (!start.failure.isEmpty()) {
        return runAttached(cmd, cwd, timeoutMs);
    }
    start.record.ownerId        = jobOwner(currentCallContext());
    start.record.maxOutputBytes = maxOutput;
    return runAsJob(&start, cmd, cwd, timeoutMs);
}

QString QSocToolRemoteShellBash::runAttached(const QString &cmd, const QString &cwd, int timeoutMs)
{
    const QSocRemoteExec request = remoteCommandExec(m_conn->host(), cwd, cmd);
    QSocSshExec          exec(*m_conn->session());
    m_running = &exec;
    const auto result
        = exec.run(request.command, timeoutMs, request.input, QSocBoundedCapture::kDefaultLimit / 2);
    m_running = nullptr;
    QSocShellCommand::Outcome outcome;
    outcome.status        = remoteRunStatus(result);
    outcome.exitCode      = result.exitCode;
    outcome.exitSignal    = result.exitSignal;
    outcome.timedOut      = result.timedOut;
    outcome.aborted       = result.aborted;
    outcome.transportDead = result.transportDead;
    outcome.error         = result.errorText;
    outcome.output        = QString::fromUtf8(result.stdoutBytes + result.stderrBytes);
    return QSocShellCommand::resultText(outcome);
}

QString QSocToolRemoteShellBash::runAsJob(
    QSocRemoteJobStart *start, const QString &cmd, const QString &cwd, int timeoutMs)
{
    const QSocMachine &host     = m_conn->host();
    const QString      shellCwd = machineShellPath(host, cwd);
    /* One round trip: launch the job, then wait for it on the host. A cwd that
     * does not exist fails here, before anything is launched. */
    const auto result = launchRemoteJob(m_conn, start, [&](const QString &jobDir) {
        const QString shellJobDir = machineShellPath(host, jobDir);
        const QString script
            = QStringLiteral("cd -- %1 || exit 1\n").arg(remoteShellQuote(shellCwd))
              + jobLaunchScript(shellJobDir, shellCwd, start->record.jobId, cmd, host.shell)
              + jobWaitScript(shellJobDir, timeoutMs, QSocShellCommand::kLastOutputLines, true);
        const QSocRemoteExec request = remoteScriptExec(host, script, false);
        QSocSshExec          exec(*m_conn->session());
        m_running = &exec;
        auto ran  = exec.run(
            request.command,
            timeoutMs + kJobWaitGraceMs,
            request.input,
            QSocBoundedCapture::kDefaultLimit);
        m_running = nullptr;
        return ran;
    });
    if (!start->failure.isEmpty()) {
        return runAttached(cmd, cwd, timeoutMs);
    }
    const QString shellDir = machineShellPath(host, start->jobDir);

    const QSocRemoteJobWait         wait   = parseJobWait(result.stdoutBytes);
    const QSocRemoteJobLaunchReport report = parseJobLaunchOutput(wait.head);
    if (wait.answered && wait.ended) {
        QSocShellCommand::Outcome outcome;
        outcome.status   = wait.exitCode == 0 ? ResultStatus::Ok : ResultStatus::Failed;
        outcome.exitCode = wait.exitCode;
        outcome.output   = QString::fromUtf8(wait.output);
        return QSocShellCommand::resultText(outcome);
    }
    if (report.pid <= 0) {
        /* Nothing was launched that could still run, or nothing can tell. */
        QSocShellCommand::Outcome outcome;
        outcome.status   = remoteRunStatus(result) == ResultStatus::Ok ? ResultStatus::Failed
                                                                       : remoteRunStatus(result);
        outcome.exitCode = result.exitCode;
        outcome.timedOut = result.timedOut;
        outcome.aborted  = result.aborted;
        outcome.transportDead = result.transportDead;
        outcome.error         = result.errorText;
        outcome.output        = QString::fromUtf8(result.stderrBytes);
        return QSocShellCommand::resultText(outcome);
    }
    recordRemoteJob(m_conn, start, report, cmd);
    const QSocShellCommand::Handle handle{
        QStringLiteral("job_id"),
        QStringLiteral("Job ID"),
        start->record.jobId,
        shellDir + QStringLiteral("/output.log"),
        QString::fromUtf8(wait.output)};
    const QString unwatched = start->noted ? QString() : QLatin1Char('\n') + jobLedgerFullNote();
    if (result.aborted || m_stopRequested) {
        if (stopJob(start->record)) {
            if (QSocToolCallContext *context = currentCallContext()) {
                context->setResultStatus(ResultStatus::Uncertain);
            }
            return QSocShellCommand::abortedText();
        }
        return QSocShellCommand::stillRunningAfterAbortText(handle) + unwatched;
    }
    if (wait.answered || result.timedOut) {
        return QSocShellCommand::timedOutText(timeoutMs, handle) + unwatched;
    }
    return composeJobUncertain(
        start->record.jobId,
        QStringLiteral("the link failed while the command ran, so it may still be running"),
        QStringLiteral("check bash_manage(action=status)"));
}

bool QSocToolRemoteShellBash::stopJob(const QSocRemoteJobRecord &record)
{
    const QSocRemoteExec request
        = remoteScriptExec(m_conn->host(), jobSignalScript(record, QStringLiteral("-KILL")), false);
    QSocSshExec exec(*m_conn->session());
    const auto  result = exec.run(request.command, kSignalExecMs, request.input);
    if (parseJobToken(QString::fromUtf8(result.stdoutBytes)) != QSocRemoteJobToken::Signalled) {
        return false;
    }
    /* Stopped by the call that started it: nobody else is told. */
    m_conn->watcher()->forget(record.jobId);
    m_conn->jobs()->forget(record.jobId);
    return true;
}

void QSocToolRemoteShellBash::abort()
{
    m_stopRequested = true;
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
        "Manage remote workspace paths. "
        "Actions: 'list' (show root, working dir, and writable dirs), 'set_working' (change "
        "working dir), 'add' (make an existing remote directory writable), 'remove' (forget a "
        "directory), 'clear' (forget every added directory).");
}

json QSocToolRemotePath::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", {"list", "set_working", "add", "remove", "clear"}},
            {"description", "Action to perform"}}},
          {"path",
           {{"type", "string"},
            {"description", "Directory path (required for set_working, add, remove)"}}}}},
        {"required", json::array({"action"})}};
}

QString QSocToolRemotePath::listing() const
{
    QString out
        = QStringLiteral("Project: %1\nWorking: %2\n").arg(m_pathCtx->root(), m_pathCtx->cwd());
    const QStringList added = m_conn != nullptr ? m_conn->addedWritableDirs() : QStringList();
    if (!added.isEmpty()) {
        out += QStringLiteral("Recent:\n");
        for (const QString &dir : added) {
            out += QStringLiteral("  - ") + dir + QLatin1Char('\n');
        }
    }
    out += QStringLiteral("Writable:\n");
    for (const QString &dir : m_pathCtx->writableDirs()) {
        const QString anchor = m_conn != nullptr ? m_conn->writableAnchor(dir) : QString();
        const bool    holds  = m_conn != nullptr && m_conn->writableDirHolds(dir);
        out += QSocPathContext::describeWritableRoot(dir, anchor, holds);
    }
    return out.trimmed();
}

QString QSocToolRemotePath::execute(const json &arguments)
{
    if (m_pathCtx == nullptr) {
        return QStringLiteral("Error: remote path context is not configured");
    }
    QString action = arguments.contains("action") && arguments["action"].is_string()
                         ? QString::fromStdString(arguments["action"].get<std::string>())
                         : QStringLiteral("list");
    /* Earlier spellings of list and set_working. */
    if (action == QStringLiteral("show")) {
        action = QStringLiteral("list");
    } else if (action == QStringLiteral("cwd")) {
        action = QStringLiteral("set_working");
    }

    if (action == QStringLiteral("list")) {
        return listing();
    }
    if (m_conn == nullptr) {
        return QStringLiteral("Error: no remote workspace is bound");
    }
    if (action == QStringLiteral("clear")) {
        m_conn->clearWritableDirectories();
        return QStringLiteral("User directories cleared.");
    }

    if (!arguments.contains("path") || !arguments["path"].is_string()) {
        return QStringLiteral("Error: path is required for action '%1'").arg(action);
    }
    const QString path = QString::fromStdString(arguments["path"].get<std::string>());

    if (action == QStringLiteral("set_working")) {
        QString why;
        /* Every refusal reads the same to the model, and they must: what it
         * has to know is that the move did not happen and where it still is.
         * Which of them it was lives in @p why. */
        if (m_conn->setWorkingDirectory(path, &why) != QSocRemoteConnection::CwdChange::Changed) {
            return QStringLiteral("Error: %1. The working directory is unchanged and still %2.")
                .arg(why, m_pathCtx->cwd());
        }
        return QStringLiteral("Working directory set to: %1").arg(m_pathCtx->cwd());
    }

    if (action == QStringLiteral("add")) {
        QString added;
        QString why;
        if (!m_conn->addWritableDirectory(path, &added, &why)) {
            return QStringLiteral("Error: %1").arg(why);
        }
        return QStringLiteral("Added to path context: %1").arg(added);
    }

    if (action == QStringLiteral("remove")) {
        m_conn->removeWritableDirectory(path);
        return QStringLiteral("Removed from path context: %1").arg(path);
    }

    return QStringLiteral("Error: Unknown action '%1'").arg(action);
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
               "Manage a remote command by job_id from a bash timeout or background response. "
               "Actions: status, wait, output, kill (SIGKILL), terminate (SIGTERM, then SIGKILL "
               "after 5 seconds). A signal is sent only when the host still reports the boot "
               "identity and the process start time recorded at launch; otherwise the answer is "
               "uncertain and nothing is signalled.")
           + (m_conn != nullptr && machineOffersExecTools(m_conn->host()) && !m_conn->host().hasProc
                  ? QStringLiteral(
                        " This host has no /proc, so a job's identity may be unverifiable and "
                        "signals refused.")
                  : QString());
}

json QSocToolRemoteBashManage::getParametersSchema() const
{
    return QSocShellCommand::bashManageSchema("job_id", "string");
}

QSocSshExec::Result QSocToolRemoteBashManage::runScript(const QString &script, int timeoutMs)
{
    const QSocRemoteExec request = remoteScriptExec(m_conn->host(), script, false);
    QSocSshExec          exec(*m_conn->session());
    m_running = &exec;
    auto result
        = exec.run(request.command, timeoutMs, request.input, QSocBoundedCapture::kDefaultLimit);
    m_running = nullptr;
    return result;
}

void QSocToolRemoteBashManage::abort()
{
    if (m_running != nullptr) {
        m_running->requestAbort();
    }
}

namespace {

/* A job query that never reached the remote host says nothing about the job.
 * Reporting the empty stdout would read as "no output yet" or, for terminate,
 * as a kill that happened. */
QString queryFailure(const QSocSshExec::Result &result)
{
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
}

/* Host stderr is evidence, so it follows the verdict rather than replacing it. */
QString stderrTail(const QSocSshExec::Result &result)
{
    return result.stderrBytes.isEmpty()
               ? QString()
               : QStringLiteral("stderr:\n") + QString::fromUtf8(result.stderrBytes);
}

const QStringList kManageActions{
    QStringLiteral("status"),
    QStringLiteral("wait"),
    QStringLiteral("output"),
    QStringLiteral("kill"),
    QStringLiteral("terminate"),
};

} // namespace

QString QSocToolRemoteBashManage::execute(const json &arguments)
{
    if (m_conn == nullptr || !m_conn->isUsable()) {
        return sessionRefusal(m_conn);
    }
    if (!machineOffersExecTools(m_conn->host())) {
        return noExecutorRefusal(m_conn);
    }
    if (m_conn->path()->root().isEmpty()) {
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
    if (!kManageActions.contains(action)) {
        return QStringLiteral("Error: unknown action '%1'").arg(action);
    }
    if (action == QStringLiteral("kill") || action == QStringLiteral("terminate")) {
        const QString unrecorded = refuseUnrecordedJob(jobId, *m_conn->jobs());
        if (!unrecorded.isEmpty()) {
            return unrecorded;
        }
        return action == QStringLiteral("kill") ? sendSignal(jobId, QStringLiteral("-KILL"))
                                                : terminate(jobId);
    }
    const ResolvedPath jobPath = resolveRemoteJobPath(m_conn, jobId);
    if (!jobPath.error.isEmpty()) {
        return QStringLiteral("Error: %1").arg(jobPath.error);
    }
    const QString dir = machineShellPath(m_conn->host(), jobPath.path);
    if (action == QStringLiteral("status")) {
        return status(jobId, dir);
    }
    if (action == QStringLiteral("wait")) {
        return waitFor(jobId, dir, QSocShellCommand::timeoutArgument(arguments));
    }
    return output(jobId, dir, QSocShellCommand::maxLinesArgument(arguments));
}

QString QSocToolRemoteBashManage::status(const QString &jobId, const QString &dir)
{
    /* What this session recorded for the id. An id it never handed out yields
     * a default record, which the scripts report as unverifiable. */
    const QSocRemoteJobRecord record  = m_conn->jobs()->record(jobId);
    const auto                result  = runScript(jobStatusScript(dir, record), kQueryMs);
    const QString             failure = queryFailure(result);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QString observed = QString::fromUtf8(result.stdoutBytes);
    const auto    token    = parseJobToken(observed);
    /* An exit code the host reported for a job it could identify is the one
     * thing that settles a record, and only a settled record can be evicted
     * to make room for the next launch. */
    const QString exitCode = parseJobStatusExitCode(observed);
    if (token == QSocRemoteJobToken::Absent && !exitCode.isEmpty()) {
        m_conn->jobs()->markSettled(jobId, exitCode.toInt());
    }
    QString       verdict = token == QSocRemoteJobToken::Absent
                                ? QSocTool::statusLine(QSocTool::ResultStatus::Ok)
                                      + QStringLiteral("job_id: %1\n").arg(jobId)
                                : composeJobRefusal(jobId, token);
    const QString reason  = m_conn->watcher()->killReason(jobId);
    if (!reason.isEmpty()) {
        verdict += QStringLiteral("killed_by_watchdog: %1\n").arg(reason);
    }
    return verdict + jobScriptEvidence(observed) + stderrTail(result);
}

QString QSocToolRemoteBashManage::output(const QString &jobId, const QString &dir, int maxLines)
{
    const QSocRemoteJobRecord record = m_conn->jobs()->record(jobId);
    const auto                result = runScript(jobOutputScript(dir, maxLines, record), kOutputMs);
    const QString             failure = queryFailure(result);
    if (!failure.isEmpty()) {
        return failure;
    }
    /* A log that happens to open with "Error:" must not make the call read as
     * failed, so the verdict always leads. */
    const QString observed = QString::fromUtf8(result.stdoutBytes);
    const auto    token    = parseJobToken(observed);
    const QString verdict  = token == QSocRemoteJobToken::Absent
                                 ? QSocTool::statusLine(QSocTool::ResultStatus::Ok)
                                       + QStringLiteral("job_id: %1\n").arg(jobId)
                                 : composeJobRefusal(jobId, token);
    return verdict + jobScriptEvidence(observed);
}

QString QSocToolRemoteBashManage::waitFor(const QString &jobId, const QString &dir, int timeoutMs)
{
    const auto result = runScript(
        jobWaitScript(dir, timeoutMs, QSocShellCommand::kLastOutputLines, false),
        timeoutMs + kWaitGraceMs);
    const QSocRemoteJobWait wait = parseJobWait(result.stdoutBytes);
    if (result.aborted && !wait.answered) {
        return QSocTool::statusLine(QSocTool::ResultStatus::Uncertain)
               + QStringLiteral("job_id: %1\nWait aborted; the job is still running.\n").arg(jobId);
    }
    if (!wait.answered) {
        const QString failure = queryFailure(result);
        return failure.isEmpty() ? QSocTool::statusLine(QSocTool::ResultStatus::Uncertain)
                                       + QStringLiteral("error: the host printed no job state\n")
                                 : failure;
    }
    QString text = QSocTool::statusLine(QSocTool::ResultStatus::Ok)
                   + QStringLiteral("job_id: %1\n").arg(jobId);
    if (wait.ended) {
        /* Its end is read here, so nobody is told about it again. */
        m_conn->jobs()->markSettled(jobId, wait.exitCode);
        m_conn->watcher()->forget(jobId);
        text += QStringLiteral("exit_code: %1\n").arg(wait.exitCode);
    } else {
        text += QStringLiteral("running: yes\nwaited_ms: %1\nLast output:\n").arg(timeoutMs);
    }
    return text + QString::fromUtf8(wait.output);
}

QString QSocToolRemoteBashManage::sendSignal(
    const QString &jobId, const QString &signalArg, bool *signalled)
{
    const QSocRemoteJobRecord record     = m_conn->jobs()->record(jobId);
    const QString             signalName = QStringLiteral("SIG") + signalArg.mid(1);
    const auto                result     = runScript(jobSignalScript(record, signalArg), kSignalMs);
    const QString             failure    = queryFailure(result);
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
        m_conn->jobs()->markStopped(jobId);
    }
    if (signalled != nullptr) {
        *signalled = judged.signalled;
    }
    return judged.text + stderrTail(result);
}

QString QSocToolRemoteBashManage::terminate(const QString &jobId)
{
    bool          signalled = false;
    const QString sent      = sendSignal(jobId, QStringLiteral("-TERM"), &signalled);
    if (!signalled) {
        return sent;
    }
    const ResolvedPath jobPath = resolveRemoteJobPath(m_conn, jobId);
    if (!jobPath.error.isEmpty()) {
        return sent;
    }
    const QString dir    = machineShellPath(m_conn->host(), jobPath.path);
    const auto    result = runScript(
        jobWaitScript(dir, QSocShellCommand::kTerminateGraceMs, 1, false),
        QSocShellCommand::kTerminateGraceMs + kWaitGraceMs);
    const QSocRemoteJobWait wait = parseJobWait(result.stdoutBytes);
    if (wait.ended) {
        m_conn->jobs()->markSettled(jobId, wait.exitCode);
        m_conn->watcher()->forget(jobId);
        return sent + QStringLiteral("exited: yes\nexit_code: %1\n").arg(wait.exitCode);
    }
    if (result.aborted) {
        return sent + QStringLiteral("Terminate requested; wait aborted; the job may still run.\n");
    }
    return sent
           + QStringLiteral("still running after %1 ms; escalating\n")
                 .arg(QSocShellCommand::kTerminateGraceMs)
           + sendSignal(jobId, QStringLiteral("-KILL"));
}

#include "moc_qsoctoolremote.cpp"
