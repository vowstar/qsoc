// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSFTPCLIENT_H
#define QSOCSFTPCLIENT_H

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstdint>
#include <functional>
#include <QByteArray>
#include <QDeadlineTimer>
#include <QString>
#include <QStringList>

class QSocSshSession;

/**
 * @brief SFTP helper riding on top of an established SSH session.
 * @details Opens the SFTP subsystem on first use, exposes simple file and
 *          directory operations, and reports errors as user-safe strings
 *          (no secrets, no private-key contents). Writes go through a
 *          temporary path + atomic rename where the server supports it.
 *
 *          Invariant: an abandoned SFTP request invalidates the SFTP
 *          subsystem, because libssh2 keeps read, write and request-id state
 *          per `LIBSSH2_SFTP` and a later call would re-enter it
 *          mid-request. It condemns the SSH session only when that
 *          subsystem cannot be released.
 */
class QSocSftpClient
{
public:
    /**
     * @brief Publish milestones for `writeFile`, in order.
     * @details Exposed only so tests can cut the link at an exact point;
     *          production code never installs an observer.
     */
    enum class PublishStage : std::uint8_t {
        AfterAside,    /**< The old content is parked under its backup name. */
        BeforePublish, /**< About to rename the staged content into place. */
    };

    /** @brief Install a test observer for the publish sequence. */
    void setPublishObserver(std::function<void(PublishStage)> observer);

    /**
     * @brief Install a test observer for the bulk-transfer phase.
     * @details Fired once per operation, immediately before the first
     *          `libssh2_sftp_read` or `libssh2_sftp_write` of that
     *          operation, which is the only point where a test can strand
     *          the subsystem in its data phase. Production code never
     *          installs an observer.
     */
    void setDataPhaseObserver(std::function<void()> observer);

    /** @brief Directory entry returned by `listDir`. */
    struct Entry
    {
        QString name;
        qint64  size        = 0;
        bool    isDirectory = false;
        bool    isSymlink   = false;
    };

    explicit QSocSftpClient(QSocSshSession &session);
    ~QSocSftpClient();

    QSocSftpClient(const QSocSftpClient &)            = delete;
    QSocSftpClient &operator=(const QSocSftpClient &) = delete;

    /** @brief Open the SFTP subsystem. Returns false on failure. */
    bool open(QString *errorMessage = nullptr);

    /** @brief Tear down SFTP subsystem; safe to call repeatedly. */
    void close();

    bool isOpen() const { return m_sftp != nullptr; }

    /**
     * @brief Read a remote file.
     * @param maxBytes Hard cap on returned content; 0 means unlimited.
     */
    QByteArray readFile(const QString &path, qint64 maxBytes = 0, QString *errorMessage = nullptr);

    /**
     * @brief Replace a remote file's content without ever unlinking it.
     * @details Writes a temp file, renames any existing target aside, then
     *          renames the temp into place and drops the saved copy. The
     *          target is never removed before its replacement is on disk,
     *          so a link that dies mid-publish leaves either the old
     *          content or a recoverable `.qsoc-bak-*` beside it. When the
     *          final rename's outcome is unknown, nothing is restored and
     *          nothing is deleted: guessing either way can destroy the one
     *          surviving copy.
     */
    bool writeFile(const QString &path, const QByteArray &content, QString *errorMessage = nullptr);

    /** @brief Recursive mkdir. Equivalent to `mkdir -p`. */
    bool mkdirP(const QString &path, QString *errorMessage = nullptr);

    /** @brief Rename (or move) a remote path. */
    bool rename(const QString &oldPath, const QString &newPath, QString *errorMessage = nullptr);

    /** @brief Delete a remote file. Succeeds if the file is already absent. */
    bool removeFile(const QString &path, QString *errorMessage = nullptr);

    /**
     * @brief List a directory; limit caps returned entries, 0 means unlimited.
     * @details Fails closed: any transport error returns an empty list with
     *          @p errorMessage set, never a silently truncated one.
     */
    QList<Entry> listDir(const QString &path, int limit = 0, QString *errorMessage = nullptr);

    /** @brief Result of a remote stat. */
    enum class Presence {
        Absent,  /**< The server answered: no such file or path. */
        Present, /**< The server answered: it is there. */
        Unknown, /**< No usable answer. Callers must not assume either way. */
    };

    /**
     * @brief Check whether a remote path exists (file or directory).
     * @details Unknown is a distinct outcome on purpose. A boolean would
     *          make a dead link indistinguishable from an absent file, and
     *          callers that skip their read-before-overwrite guard on
     *          "absent" would then clobber a file they never read.
     */
    Presence presence(const QString &path, QString *errorMessage = nullptr);

    /** @brief Most recent error, user-safe for logs. */
    QString lastError() const { return m_lastError; }

    /**
     * @brief Whether the last failure left the remote state undetermined.
     * @details True when a request went out and no answer came back, so the
     *          server may or may not have carried it out. Callers must
     *          report this as uncertain rather than as a clean failure: a
     *          write reported as failed invites a retry that double-applies.
     *
     *          One of three deliberately separate signals, on three
     *          different subjects. Do not merge them:
     *          - this one: did *this request* take effect? (unknown)
     *          - `QSocSshExec::Result::transportDead`: was the link gone
     *            during *this call*?
     *          - `QSocSshSession::Unusable`: may *any later call* use this
     *            session at all?
     *          A dead link makes all three true at once, which is what
     *          makes them look redundant; a plain timeout on a healthy
     *          link sets only the first.
     */
    bool lastFailureUncertain() const { return m_lastFailureUncertain; }

    /**
     * @brief Bytes the server acknowledged during the most recent write.
     * @details `libssh2_sftp_write` reports acked bytes only, while it may
     *          already have put more on the wire, so this is a lower bound
     *          on what the remote holds and never an upper one.
     */
    qint64 lastBytesAcked() const { return m_lastBytesAcked; }

    /**
     * @brief Budget for one operation, measured from its entry. Default 30000.
     * @details Bounds every EAGAIN retry loop, including the ones a dead
     *          transport would otherwise never break out of.
     */
    void setOperationTimeoutMs(int timeoutMs);
    int  operationTimeoutMs() const { return m_opBudgetMs; }

private:
    /** @brief Outcome of one publish step. */
    enum class StepOutcome {
        Ok,      /**< The server confirmed it. */
        Failed,  /**< The server refused it; nothing changed. */
        Unknown, /**< No answer came back; it may or may not have happened. */
    };

    /**
     * @brief RAII owner of the operation deadline.
     * @details The outermost scope owns the budget; a nested one (mkdirP or
     *          presence called from inside writeFile) is inert and consumes
     *          the remaining time. Without this, every inner helper reset
     *          the clock and the "absolute" deadline was not absolute.
     */
    class OpScope
    {
    public:
        OpScope(QSocSftpClient *client, int budgetMs);
        ~OpScope();
        OpScope(const OpScope &)            = delete;
        OpScope &operator=(const OpScope &) = delete;

    private:
        QSocSftpClient *m_client;
        bool            m_owner;
    };

    /**
     * @brief Wait during a request exchange, bounded by the deadline.
     * @details Giving up here strands libssh2 mid-request: every SFTP request
     *          gets exactly one reply, and the state that reply belongs to
     *          lives in the subsystem, so whichever call came next would
     *          re-enter it. The subsystem is therefore marked stranded and
     *          the outcome uncertain; whether the SSH session survives is
     *          decided by `rebuildSubsystem`.
     * @return False when the wait gave up.
     */
    bool wait();

    /**
     * @brief Release a stranded subsystem, or condemn the session.
     * @details Bounded `libssh2_sftp_shutdown`. Completing it proves the SSH
     *          transport is in sync, so only the subsystem is dropped and the
     *          next call opens a fresh one. Failing to complete it is the
     *          detection for a transport left mid-packet, which no later
     *          caller can recover from, so the session is condemned. Does no
     *          I/O at all once the session is already unusable.
     * @return True when the SSH session may still be used.
     */
    bool rebuildSubsystem();

    /** @brief Name why a wait gave up: a dead transport, or just slow. */
    QString waitFailureText(const QString &timeoutText) const;
    void    setError(const QString &msg, QString *sink);
    /**
     * @brief Rename with a tri-state outcome, refusing to overwrite.
     * @details The `libssh2_sftp_rename` convenience macro asks for
     *          OVERWRITE, which would let the publish sequence clobber a
     *          concurrent writer's backup or target. This spells the flags
     *          out as none.
     */
    StepOutcome renameStep(const QByteArray &from, const QByteArray &to);
    /** @brief Poison the session on a transport code, flagging uncertainty. */
    bool noteTransport(int rc);
    /* Drive a close / unlink to completion on the non-blocking session.
     * Best-effort: an EAGAIN is retried until the socket is ready, but the
     * cleanup budget stops the loop so a cleanup path cannot hang. Used
     * everywhere instead of bare libssh2 calls so a half-issued
     * (EAGAIN-ignored) op never leaks a handle or a temp file. Each reports
     * whether it finished, because an unfinished one strands the subsystem. */
    bool drainClose(LIBSSH2_SFTP_HANDLE *handle);
    bool closeDir(LIBSSH2_SFTP_HANDLE *handle);
    /* Releases a stranded subsystem first and refuses to run when that
     * fails: `unlink_request_id` is one slot per subsystem, so an unlink
     * issued while stranded skips its own send and reports the abandoned
     * request's reply as its own, deleting a path nobody asked about. */
    bool drainUnlink(const QByteArray &path);

    QSocSshSession &m_session;
    LIBSSH2_SFTP   *m_sftp       = nullptr;
    int             m_opBudgetMs = 30000;
    QDeadlineTimer  m_deadline;
    bool            m_opActive             = false;
    bool            m_lastFailureUncertain = false;
    bool            m_stranded             = false;
    qint64          m_lastBytesAcked       = 0;
    QString         m_lastError;

    std::function<void(PublishStage)> m_publishObserver;
    std::function<void()>             m_dataPhaseObserver;
};

#endif // QSOCSFTPCLIENT_H
