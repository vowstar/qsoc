// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsftpclient.h"

#include "agent/remote/qsocsshsession.h"

#include <QFileInfo>
#include <QUuid>

namespace {

/* Slice length for one poll. */
constexpr int kWaitMs = 200;

/* Separate, smaller budget for close / unlink once an operation has spent
 * its own, so cleanup stays bounded instead of being skipped. */
constexpr int kCleanupMs = 2000;

/* Ceiling on one libssh2_sftp_write call. The library builds chunks for the
 * whole count before sending any and only advances its acked offset on a
 * matched STATUS, so an uncapped count makes the in-flight (and, after an
 * abandon, leakable) set scale with the file and makes the acked figure
 * meaningless. 8 outgoing chunks of MAX_SFTP_OUTGOING_SIZE. */
constexpr qint64 kWriteWindowBytes = 8 * 30000;

/* Same dir as the final path; dot-prefix hides it in most ls output. The
 * suffix is random, not a timestamp: two writers inside the same
 * millisecond would otherwise pick the same name, and the file is opened
 * with EXCL so a collision fails loudly instead of truncating. */
QString makeTempPath(const QString &finalPath)
{
    const QFileInfo info(finalPath);
    return info.absolutePath() + QStringLiteral("/.qsoc-write-")
           + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral("-") + info.fileName();
}

/* Sibling of the target holding its previous content while the replacement
 * is renamed in. A leftover one means a publish was interrupted, and it is
 * deliberately left visible so the content can be recovered. */
QString makeBackupPath(const QString &finalPath)
{
    return finalPath + QStringLiteral(".qsoc-bak-") + QUuid::createUuid().toString(QUuid::Id128);
}

} // namespace

QSocSftpClient::OpScope::OpScope(QSocSftpClient *client, int budgetMs)
    : m_client(client)
    , m_owner(!client->m_opActive)
{
    if (m_owner) {
        m_client->m_opActive             = true;
        m_client->m_lastFailureUncertain = false;
        m_client->m_deadline             = QDeadlineTimer(budgetMs);
    }
}

QSocSftpClient::OpScope::~OpScope()
{
    if (m_owner) {
        m_client->m_opActive = false;
        /* No operation may hand control back while the subsystem is
         * stranded: every handle it opened is closed by now, so this is the
         * last point where the subsystem can still be released. */
        (void) m_client->rebuildSubsystem();
    }
}

QSocSftpClient::QSocSftpClient(QSocSshSession &session)
    : m_session(session)
{}

QSocSftpClient::~QSocSftpClient()
{
    close();
}

void QSocSftpClient::setError(const QString &msg, QString *sink)
{
    m_lastError = msg;
    if (sink != nullptr) {
        *sink = msg;
    }
}

void QSocSftpClient::setOperationTimeoutMs(int timeoutMs)
{
    m_opBudgetMs = timeoutMs;
}

bool QSocSftpClient::noteTransport(int rc)
{
    if (!m_session.notePossibleTransportError(rc)) {
        return false;
    }
    m_lastFailureUncertain = true;
    return true;
}

bool QSocSftpClient::wait()
{
    auto giveUp = [this] {
        /* A request is outstanding and no reply came: whatever it asked for
         * may still have happened on the server, and libssh2 is left
         * mid-request on state the whole subsystem shares. */
        m_lastFailureUncertain = true;
        m_stranded             = true;
        return false;
    };

    if (m_deadline.hasExpired()) {
        return giveUp();
    }
    const qint64 remaining = m_deadline.remainingTime();
    const int  slice = remaining < 0 ? kWaitMs
                                     : qMax(1, static_cast<int>(qMin<qint64>(remaining, kWaitMs)));
    const auto outcome
        = QSocSshSession::waitSocket(m_session.socketFd(), m_session.rawSession(), slice);
    if (outcome == QSocSshSession::WaitOutcome::Fatal) {
        m_session.markTransportDead();
        m_lastFailureUncertain = true;
        return false;
    }
    if (m_deadline.hasExpired()) {
        return giveUp();
    }
    return true;
}

bool QSocSftpClient::rebuildSubsystem()
{
    if (!m_stranded) {
        return m_session.isConnected();
    }
    m_stranded = false;
    /* An unusable session cannot answer, so touching the socket here would
     * only spend a caller's budget proving what is already known. The
     * subsystem's channel goes away with the session. */
    if (m_session.rawSession() == nullptr || !m_session.isConnected()) {
        m_sftp = nullptr;
        return false;
    }
    if (m_sftp == nullptr) {
        return true;
    }
    LIBSSH2_SFTP *sftp = m_sftp;
    /* Dropped either way: on success libssh2 freed it, and on failure it
     * still owns state we must never re-enter. Only sftp_shutdown frees the
     * struct, so an incomplete one leaks it along with any open handle. */
    m_sftp = nullptr;
    const QDeadlineTimer deadline(kCleanupMs);
    int                  rc = 0;
    while ((rc = libssh2_sftp_shutdown(sftp)) == LIBSSH2_ERROR_EAGAIN) {
        if (deadline.hasExpired()) {
            /* The channel could not be released, which is what a transport
             * left mid-packet looks like from here: no later caller can put
             * it back in sync. */
            m_session.markAbandonedExchange();
            return false;
        }
        const qint64 remaining = deadline.remainingTime();
        const int    slice     = qMax(1, static_cast<int>(qMin<qint64>(remaining, kWaitMs)));
        const auto   outcome
            = QSocSshSession::waitSocket(m_session.socketFd(), m_session.rawSession(), slice);
        if (outcome == QSocSshSession::WaitOutcome::Fatal) {
            m_session.markTransportDead();
            return false;
        }
    }
    if (rc != 0) {
        return !noteTransport(rc);
    }
    return true;
}

void QSocSftpClient::setPublishObserver(std::function<void(PublishStage)> observer)
{
    m_publishObserver = std::move(observer);
}

void QSocSftpClient::setDataPhaseObserver(std::function<void()> observer)
{
    m_dataPhaseObserver = std::move(observer);
}

/* Error text for a wait that gave up, naming the actual reason so the
 * agent does not read a dead host as a slow one. */
QString QSocSftpClient::waitFailureText(const QString &timeoutText) const
{
    const QString unusable = m_session.unusableText();
    return unusable.isEmpty() ? timeoutText : unusable;
}

bool QSocSftpClient::drainClose(LIBSSH2_SFTP_HANDLE *handle)
{
    if (handle == nullptr) {
        return true;
    }
    /* Cleanup runs after the operation budget is gone, so it takes a fresh
     * small one of its own rather than inheriting an expired clock. Its
     * final code still matters: an unfinished close leaves a server-side
     * handle open and libssh2 mid-request. */
    m_deadline = QDeadlineTimer(kCleanupMs);
    int rc     = 0;
    while ((rc = libssh2_sftp_close(handle)) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            return false;
        }
    }
    if (rc != 0) {
        noteTransport(rc);
        return false;
    }
    return true;
}

bool QSocSftpClient::closeDir(LIBSSH2_SFTP_HANDLE *handle)
{
    if (handle == nullptr) {
        return true;
    }
    m_deadline = QDeadlineTimer(kCleanupMs);
    int rc     = 0;
    while ((rc = libssh2_sftp_closedir(handle)) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            return false;
        }
    }
    if (rc != 0) {
        noteTransport(rc);
        return false;
    }
    return true;
}

bool QSocSftpClient::drainUnlink(const QByteArray &path)
{
    /* The budget comes first: releasing a stranded subsystem and opening the
     * replacement are both requests, and both would otherwise inherit an
     * expired clock and strand themselves on the spot. */
    m_deadline = QDeadlineTimer(kCleanupMs);
    if (!rebuildSubsystem()) {
        return false;
    }
    if (!open(nullptr)) {
        return false;
    }
    int rc = 0;
    while ((rc = libssh2_sftp_unlink(m_sftp, path.constData())) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            return false;
        }
    }
    if (rc != 0) {
        /* A server-reported refusal (an already absent path) says nothing
         * about the link, so it must not escalate. */
        noteTransport(rc);
        return false;
    }
    return true;
}

QSocSftpClient::StepOutcome QSocSftpClient::renameStep(const QByteArray &from, const QByteArray &to)
{
    int rc = 0;
    while ((rc = libssh2_sftp_rename_ex(
                m_sftp,
                from.constData(),
                static_cast<unsigned int>(from.size()),
                to.constData(),
                static_cast<unsigned int>(to.size()),
                0 /* no OVERWRITE: never clobber an existing destination */))
           == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            /* No reply arrived. The server may or may not have done it. The
             * single per-subsystem rename request-id slot is now spoken for,
             * so release the subsystem before the caller writes its message:
             * whether the session survived is part of what it reports. */
            (void) rebuildSubsystem();
            return StepOutcome::Unknown;
        }
    }
    if (rc == 0) {
        return StepOutcome::Ok;
    }
    if (noteTransport(rc)) {
        return StepOutcome::Unknown;
    }
    return StepOutcome::Failed;
}

bool QSocSftpClient::open(QString *errorMessage)
{
    /* Usability before the cached handle: reusing an SFTP channel on a
     * session that died, or on one stranded mid-request, hands every later
     * call a subsystem that cannot answer correctly. */
    if (m_session.rawSession() != nullptr && !m_session.isConnected()) {
        m_sftp = nullptr;
        setError(m_session.unusableText(), errorMessage);
        return false;
    }
    if (m_stranded && !rebuildSubsystem()) {
        setError(m_session.unusableText(), errorMessage);
        return false;
    }
    if (m_sftp != nullptr) {
        return true;
    }
    LIBSSH2_SESSION *session = m_session.rawSession();
    if (session == nullptr) {
        setError(QStringLiteral("SSH session is not connected"), errorMessage);
        return false;
    }
    OpScope scope(this, m_opBudgetMs);
    while ((m_sftp = libssh2_sftp_init(session)) == nullptr) {
        const int err = libssh2_session_last_errno(session);
        if (err != LIBSSH2_ERROR_EAGAIN) {
            noteTransport(err);
            setError(waitFailureText(QStringLiteral("Failed to open SFTP subsystem")), errorMessage);
            return false;
        }
        if (!wait()) {
            setError(
                waitFailureText(QStringLiteral("Timed out opening SFTP subsystem")), errorMessage);
            return false;
        }
    }
    return true;
}

void QSocSftpClient::close()
{
    if (m_sftp == nullptr) {
        m_stranded = false;
        return;
    }
    /* The subsystem's channel belongs to the session. Once that is gone the
     * handle only looks valid, so it may be dropped but never used. */
    if (m_session.rawSession() == nullptr) {
        m_sftp     = nullptr;
        m_stranded = false;
        return;
    }
    m_deadline = QDeadlineTimer(kCleanupMs);
    int rc     = 0;
    while ((rc = libssh2_sftp_shutdown(m_sftp)) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            break;
        }
    }
    m_sftp = nullptr;
    if (rc != 0) {
        noteTransport(rc);
    }
    if (m_stranded) {
        /* The channel was never released, so the session is out of sync with
         * the peer and only its own teardown can settle that. */
        m_stranded = false;
        m_session.markAbandonedExchange();
    }
}

QByteArray QSocSftpClient::readFile(const QString &path, qint64 maxBytes, QString *errorMessage)
{
    OpScope scope(this, m_opBudgetMs);
    if (!open(errorMessage)) {
        return {};
    }
    const QByteArray pathBytes = path.toUtf8();

    LIBSSH2_SFTP_HANDLE *handle = nullptr;
    while ((handle = libssh2_sftp_open(m_sftp, pathBytes.constData(), LIBSSH2_FXF_READ, 0))
           == nullptr) {
        const int err = libssh2_session_last_errno(m_session.rawSession());
        if (err != LIBSSH2_ERROR_EAGAIN) {
            noteTransport(err);
            setError(
                waitFailureText(QStringLiteral("SFTP open for read failed: %1").arg(path)),
                errorMessage);
            return {};
        }
        if (!wait()) {
            setError(
                waitFailureText(QStringLiteral("Timed out opening %1 for read").arg(path)),
                errorMessage);
            return {};
        }
    }

    QByteArray out;
    char       buffer[16384];
    bool       observed = false;
    while (true) {
        if (!observed) {
            observed = true;
            if (m_dataPhaseObserver) {
                m_dataPhaseObserver();
            }
        }
        const ssize_t nread = libssh2_sftp_read(handle, buffer, sizeof(buffer));
        if (nread > 0) {
            out.append(buffer, static_cast<int>(nread));
            if (maxBytes > 0 && out.size() >= maxBytes) {
                out.truncate(static_cast<int>(maxBytes));
                break;
            }
            continue;
        }
        if (nread == 0) {
            break;
        }
        if (nread == LIBSSH2_ERROR_EAGAIN) {
            if (!wait()) {
                /* The handle goes first: the subsystem cannot be released
                 * from under an open one. */
                (void) drainClose(handle);
                (void) rebuildSubsystem();
                setError(
                    waitFailureText(QStringLiteral("Timed out reading %1").arg(path)), errorMessage);
                return {};
            }
            continue;
        }
        noteTransport(static_cast<int>(nread));
        setError(waitFailureText(QStringLiteral("SFTP read error on %1").arg(path)), errorMessage);
        (void) drainClose(handle);
        return {};
    }
    (void) drainClose(handle);
    return out;
}

bool QSocSftpClient::writeFile(const QString &path, const QByteArray &content, QString *errorMessage)
{
    OpScope scope(this, m_opBudgetMs);
    if (!open(errorMessage)) {
        return false;
    }

    const QFileInfo finalInfo(path);
    if (!mkdirP(finalInfo.absolutePath(), errorMessage)) {
        return false;
    }

    const QString       tempPath      = makeTempPath(path);
    const QByteArray    tempPathBytes = tempPath.toUtf8();
    const unsigned long mode          = 0644;
    const int           flags         = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL;

    LIBSSH2_SFTP_HANDLE *handle = nullptr;
    while ((handle = libssh2_sftp_open(m_sftp, tempPathBytes.constData(), flags, mode)) == nullptr) {
        const int err = libssh2_session_last_errno(m_session.rawSession());
        if (err != LIBSSH2_ERROR_EAGAIN) {
            noteTransport(err);
            setError(
                waitFailureText(QStringLiteral("SFTP open for write failed: %1").arg(tempPath)),
                errorMessage);
            return false;
        }
        if (!wait()) {
            setError(
                waitFailureText(QStringLiteral("Timed out opening %1 for write").arg(tempPath)),
                errorMessage);
            return false;
        }
    }

    qint64 offset    = 0;
    m_lastBytesAcked = 0;
    bool observed    = false;
    while (offset < content.size()) {
        if (!observed) {
            observed = true;
            if (m_dataPhaseObserver) {
                m_dataPhaseObserver();
            }
        }
        /* The window is recomputed from an unchanged offset, so an EAGAIN
         * retry repeats the same argument set, which libssh2 requires. */
        const qint64  window = qMin<qint64>(content.size() - offset, kWriteWindowBytes);
        const ssize_t written
            = libssh2_sftp_write(handle, content.constData() + offset, static_cast<size_t>(window));
        if (written > 0) {
            offset += written;
            m_lastBytesAcked = offset;
            continue;
        }
        if (written == LIBSSH2_ERROR_EAGAIN) {
            if (!wait()) {
                (void) drainClose(handle);
                setError(
                    QStringLiteral("%1; %2 of %3 bytes acknowledged, the remote may hold more")
                        .arg(waitFailureText(QStringLiteral("Timed out writing %1").arg(tempPath)))
                        .arg(offset)
                        .arg(content.size()),
                    errorMessage);
                /* Releases the stranded subsystem first and stays out of the
                 * way when it cannot be released. */
                (void) drainUnlink(tempPathBytes);
                return false;
            }
            continue;
        }
        noteTransport(static_cast<int>(written));
        setError(
            waitFailureText(QStringLiteral("SFTP write error on %1").arg(tempPath)), errorMessage);
        (void) drainClose(handle);
        (void) drainUnlink(tempPathBytes);
        return false;
    }

    /* Close is part of the write: it flushes buffered bytes. Its final
     * return code decides whether the temp holds the whole content, so an
     * error here must abort the publish. Renaming an unflushed temp over a
     * good file is how a truncated write becomes data loss. */
    int closeRc = 0;
    while ((closeRc = libssh2_sftp_close(handle)) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            setError(
                waitFailureText(QStringLiteral("Timed out closing %1").arg(tempPath)), errorMessage);
            (void) drainUnlink(tempPathBytes);
            return false;
        }
    }
    if (closeRc != 0) {
        noteTransport(closeRc);
        setError(
            waitFailureText(QStringLiteral("Failed to flush %1; write abandoned").arg(tempPath)),
            errorMessage);
        (void) drainUnlink(tempPathBytes);
        return false;
    }

    /* Publish. The target is never unlinked: it is renamed aside first, so
     * at every instant either the old or the new content is reachable under
     * a name. Every rename here refuses to overwrite, so a concurrent writer
     * loses its rename instead of silently replacing our copy.
     *
     * The rule after an Unknown outcome is to issue no further requests: we
     * cannot know what the server did, and any cleanup we send could be the
     * thing that destroys the last surviving copy. */
    auto observe = [this](PublishStage stage) {
        if (m_publishObserver) {
            m_publishObserver(stage);
        }
    };

    const QByteArray finalBytes = path.toUtf8();
    QString          backupPath;
    QByteArray       backupBytes;
    switch (presence(path, nullptr)) {
    case Presence::Present: {
        backupPath              = makeBackupPath(path);
        backupBytes             = backupPath.toUtf8();
        const StepOutcome aside = renameStep(finalBytes, backupBytes);
        if (aside == StepOutcome::Unknown) {
            setError(
                QStringLiteral(
                    "%1: cannot tell whether %2 was moved to %3; both names left as "
                    "they are, and %4 was not published")
                    .arg(
                        waitFailureText(QStringLiteral("No answer from the remote host")),
                        path,
                        backupPath,
                        tempPath),
                errorMessage);
            return false;
        }
        if (aside == StepOutcome::Failed) {
            setError(
                QStringLiteral("Could not move %1 aside; existing content left untouched").arg(path),
                errorMessage);
            (void) drainUnlink(tempPathBytes);
            return false;
        }
        break;
    }
    case Presence::Absent:
        break;
    case Presence::Unknown:
        setError(
            waitFailureText(
                QStringLiteral("Cannot tell whether %1 exists; refusing to publish").arg(path)),
            errorMessage);
        return false;
    }
    observe(PublishStage::AfterAside);

    observe(PublishStage::BeforePublish);
    const StepOutcome publish = renameStep(tempPathBytes, finalBytes);
    if (publish == StepOutcome::Unknown) {
        /* We do not know whether the new content landed. Restoring the
         * backup could overwrite a successful write and deleting anything
         * could destroy the only copy, so both files stay where they are. */
        setError(
            QStringLiteral("%1: publish outcome unknown; %2 and %3 both left in place")
                .arg(
                    waitFailureText(QStringLiteral("No answer from the remote host")),
                    tempPath,
                    backupPath.isEmpty() ? path : backupPath),
            errorMessage);
        return false;
    }
    if (publish == StepOutcome::Failed) {
        if (backupBytes.isEmpty()) {
            setError(
                QStringLiteral("SFTP rename failed for %1; nothing was changed").arg(path),
                errorMessage);
            (void) drainUnlink(tempPathBytes);
            return false;
        }
        /* A definite refusal means the target slot is still empty, so
         * putting the original back is safe to attempt. Whether it worked
         * has to be reported, not assumed. */
        const StepOutcome restore = renameStep(backupBytes, finalBytes);
        if (restore == StepOutcome::Ok) {
            setError(
                QStringLiteral("SFTP rename failed for %1; previous content restored").arg(path),
                errorMessage);
            (void) drainUnlink(tempPathBytes);
            return false;
        }
        if (restore == StepOutcome::Unknown) {
            setError(
                QStringLiteral(
                    "SFTP rename failed for %1 and the restore of %2 could not be "
                    "confirmed; recover the content from %2 or %3 by hand")
                    .arg(path, backupPath, tempPath),
                errorMessage);
            return false;
        }
        setError(
            QStringLiteral(
                "SFTP rename failed for %1 and the previous content could not be "
                "restored; it is still at %2")
                .arg(path, backupPath),
            errorMessage);
        (void) drainUnlink(tempPathBytes);
        return false;
    }
    if (!backupBytes.isEmpty()) {
        /* The replacement is in place; the saved copy is now redundant. A
         * failure to remove it leaves a harmless sibling, not a lost file. */
        (void) drainUnlink(backupBytes);
    }
    return true;
}

bool QSocSftpClient::mkdirP(const QString &path, QString *errorMessage)
{
    OpScope scope(this, m_opBudgetMs);
    if (!open(errorMessage)) {
        return false;
    }
    if (path.isEmpty() || path == QStringLiteral("/")) {
        return true;
    }

    /* Walk ancestors, mkdir each rung. */
    QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString     cumulative;
    for (const QString &seg : parts) {
        cumulative += QLatin1Char('/');
        cumulative += seg;
        const QByteArray bytes = cumulative.toUtf8();
        /* EEXIST is fine; success is also fine. */
        int rc = 0;
        while ((rc = libssh2_sftp_mkdir(m_sftp, bytes.constData(), 0755)) == LIBSSH2_ERROR_EAGAIN) {
            if (!wait()) {
                setError(
                    waitFailureText(QStringLiteral("Timed out during mkdir %1").arg(cumulative)),
                    errorMessage);
                return false;
            }
        }
        if (rc != 0) {
            if (noteTransport(rc)) {
                setError(m_session.unusableText(), errorMessage);
                return false;
            }
            const unsigned long sftpErr = libssh2_sftp_last_error(m_sftp);
            if (sftpErr != LIBSSH2_FX_FILE_ALREADY_EXISTS
                && sftpErr != LIBSSH2_FX_FAILURE /* many servers map EEXIST to FAILURE */) {
                setError(
                    QStringLiteral("mkdir failed at %1 (sftp err %2)").arg(cumulative).arg(sftpErr),
                    errorMessage);
                return false;
            }
        }
    }
    return true;
}

bool QSocSftpClient::rename(const QString &oldPath, const QString &newPath, QString *errorMessage)
{
    OpScope scope(this, m_opBudgetMs);
    if (!open(errorMessage)) {
        return false;
    }
    if (renameStep(oldPath.toUtf8(), newPath.toUtf8()) == StepOutcome::Ok) {
        return true;
    }
    setError(
        waitFailureText(QStringLiteral("SFTP rename failed: %1 -> %2").arg(oldPath, newPath)),
        errorMessage);
    return false;
}

bool QSocSftpClient::removeFile(const QString &path, QString *errorMessage)
{
    OpScope scope(this, m_opBudgetMs);
    if (!open(errorMessage)) {
        return false;
    }
    /* An already-absent file is success: rewind-to-absent is idempotent.
     * An unanswered stat is not: reporting the delete as done would leave
     * the caller believing a file it never removed is gone. */
    QString        presenceErr;
    const Presence before = presence(path, &presenceErr);
    if (before == Presence::Unknown) {
        setError(presenceErr, errorMessage);
        return false;
    }
    if (before == Presence::Absent) {
        return true;
    }
    const QByteArray pathBytes = path.toUtf8();
    int              urc       = 0;
    while ((urc = libssh2_sftp_unlink(m_sftp, pathBytes.constData())) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            setError(waitFailureText(QStringLiteral("Timed out deleting %1").arg(path)), errorMessage);
            return false;
        }
    }
    if (urc != 0) {
        noteTransport(urc);
        setError(waitFailureText(QStringLiteral("SFTP unlink failed: %1").arg(path)), errorMessage);
        return false;
    }
    return true;
}

QList<QSocSftpClient::Entry> QSocSftpClient::listDir(
    const QString &path, int limit, QString *errorMessage)
{
    OpScope      scope(this, m_opBudgetMs);
    QList<Entry> entries;
    if (!open(errorMessage)) {
        return entries;
    }
    const QByteArray     pathBytes = path.toUtf8();
    LIBSSH2_SFTP_HANDLE *handle    = nullptr;
    while ((handle = libssh2_sftp_opendir(m_sftp, pathBytes.constData())) == nullptr) {
        const int err = libssh2_session_last_errno(m_session.rawSession());
        if (err != LIBSSH2_ERROR_EAGAIN) {
            noteTransport(err);
            setError(
                waitFailureText(QStringLiteral("SFTP opendir failed: %1").arg(path)), errorMessage);
            return entries;
        }
        if (!wait()) {
            setError(
                waitFailureText(QStringLiteral("Timed out opening directory %1").arg(path)),
                errorMessage);
            return entries;
        }
    }

    char                    name[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    while (true) {
        const int rc = libssh2_sftp_readdir(handle, name, sizeof(name), &attrs);
        if (rc > 0) {
            const QString entryName = QString::fromUtf8(name, rc);
            if (entryName == QStringLiteral(".") || entryName == QStringLiteral("..")) {
                continue;
            }
            Entry entry;
            entry.name = entryName;
            if ((attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0) {
                entry.size = static_cast<qint64>(attrs.filesize);
            }
            if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0) {
                entry.isDirectory = (attrs.permissions & LIBSSH2_SFTP_S_IFDIR) != 0;
                entry.isSymlink   = (attrs.permissions & LIBSSH2_SFTP_S_IFLNK) != 0;
            }
            entries.push_back(entry);
            if (limit > 0 && entries.size() >= limit) {
                break;
            }
            continue;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (!wait()) {
                (void) closeDir(handle);
                setError(
                    waitFailureText(QStringLiteral("Timed out listing %1").arg(path)), errorMessage);
                return {};
            }
            continue;
        }
        if (rc == 0) {
            /* End of directory. */
            break;
        }
        /* Anything else truncates the listing. Returning what we have would
         * read as "the rest of the files are not there". */
        noteTransport(rc);
        (void) closeDir(handle);
        setError(waitFailureText(QStringLiteral("SFTP readdir failed on %1").arg(path)), errorMessage);
        return {};
    }
    (void) closeDir(handle);
    return entries;
}

QSocSftpClient::Presence QSocSftpClient::presence(const QString &path, QString *errorMessage)
{
    OpScope scope(this, m_opBudgetMs);
    if (!open(errorMessage)) {
        return Presence::Unknown;
    }
    const QByteArray        pathBytes = path.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int                     rc = 0;
    while ((rc = libssh2_sftp_stat(m_sftp, pathBytes.constData(), &attrs)) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            setError(waitFailureText(QStringLiteral("Timed out statting %1").arg(path)), errorMessage);
            return Presence::Unknown;
        }
    }
    if (rc == 0) {
        return Presence::Present;
    }
    /* Only a protocol-level reply carries a meaningful SFTP status code; on
     * a transport error the last status is stale. */
    if (rc == LIBSSH2_ERROR_SFTP_PROTOCOL) {
        const unsigned long sftpErr = libssh2_sftp_last_error(m_sftp);
        if (sftpErr == LIBSSH2_FX_NO_SUCH_FILE || sftpErr == LIBSSH2_FX_NO_SUCH_PATH) {
            return Presence::Absent;
        }
    }
    noteTransport(rc);
    setError(waitFailureText(QStringLiteral("SFTP stat failed on %1").arg(path)), errorMessage);
    return Presence::Unknown;
}
