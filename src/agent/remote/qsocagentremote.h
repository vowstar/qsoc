// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTREMOTE_H
#define QSOCAGENTREMOTE_H

#include "agent/qsocfilehistory.h"
#include "agent/remote/qsocremotejobs.h"
#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsshsession.h"

#include <QList>
#include <QObject>
#include <QString>

#include <cstdint>

class QSocSshSession;
class QSocSftpClient;
class QSocToolRegistry;
class QSocConfig;
class QSocMonitorTaskSource;
class QSocHostCatalog;
class QSocSshConfigParser;

/**
 * @brief Short-lived staging bundle for one connect attempt.
 * @details Populated by `connectAgentSshSession()` then
 *          `prepareAgentRemoteWorkspace()`, and consumed exactly once by
 *          `QSocRemoteConnection::adopt()`, which drains it. It deliberately
 *          carries no path context and no registry: those live on the
 *          connection, which outlives every tool bound to it, so there is
 *          only ever one source of truth for the remote path. A staging
 *          bundle that was not adopted is freed with
 *          @ref discardAgentRemoteState.
 */
struct AgentRemoteState
{
    QSocSshSession         *session = nullptr;
    QSocSftpClient         *sftp    = nullptr;
    QList<QSocSshSession *> jumps;     /* ProxyJump chain, outlives target */
    QString                 targetKey; /* "user@alias:port", stable lookup key. */
    QString                 workspace; /* Remote absolute workspace path. */
};

/**
 * @brief Free a staging bundle that was never adopted.
 * @details SFTP, then the session, then the ProxyJump chain in reverse so
 *          children disconnect before their parents. Leaves @p state empty,
 *          so calling it twice is harmless.
 */
void discardAgentRemoteState(AgentRemoteState *state);

/**
 * @brief Sole owner of whichever transport currently backs a workspace.
 * @details Tools bind to one of these once and ask it for the session, the
 *          SFTP client and the path context on every call, so replacing the
 *          transport underneath them needs no re-wiring and cannot leave a
 *          second copy of the pointers behind to dangle.
 *
 *          Owned by whoever owns the binding (the CLI worker, or the spawn
 *          tool's host cache) and outlives every registry built from it.
 *          Pinned in place: every consumer holds a `QSocRemoteConnection *`,
 *          so a copy would double-free the transport and a move would dangle
 *          all of them at once.
 *
 *          Deliberately not a state machine: liveness lives on the session,
 *          and there is no backoff, scheduler, or failover here.
 */
class QSocRemoteConnection
{
public:
    /** @brief Counter identifying one bound transport. 0 means never bound. */
    using Generation = quint64;

    QSocRemoteConnection() = default;

    /** @brief Frees whatever is still bound. */
    ~QSocRemoteConnection();

    QSocRemoteConnection(const QSocRemoteConnection &)                = delete;
    QSocRemoteConnection &operator=(const QSocRemoteConnection &)     = delete;
    QSocRemoteConnection(QSocRemoteConnection &&)                     = delete;
    QSocRemoteConnection &operator=(QSocRemoteConnection &&) noexcept = delete;

    /**
     * @brief Take ownership of the transport a successful connect produced.
     * @details Drains @p state on success and bumps @ref generation. Refuses
     *          an incomplete bundle (no session, no SFTP or no workspace)
     *          without consuming it or changing anything, so a caller can ask
     *          before it frees the transport being replaced.
     *
     *          Path seeding: a first bind, or a bind to a different
     *          workspace, seeds root = cwd = workspace and writableDirs =
     *          {workspace}. A rebind to the SAME workspace keeps the working
     *          directory and the writable set, re-verifies the working
     *          directory against the host, and falls back to the root when
     *          the host does not confirm it. Believed file contents are
     *          forgotten either way: the read-before-overwrite guard keys off
     *          them, and nothing on the host has been observed since.
     * @return True when the transport was adopted.
     */
    bool adopt(AgentRemoteState &&state);

    /**
     * @brief Close and free the bound transport in dependency order.
     * @details SFTP, then the session, then the ProxyJump chain in reverse so
     *          children disconnect before their parents. Also clears the
     *          identity and the path context, so a later `adopt()` seeds a
     *          fresh working directory. Leaves @ref generation alone: a stale
     *          holder must keep reading stale rather than matching a later
     *          rebind by accident. Safe to call with nothing bound.
     */
    void teardown();

    QSocSshSession        *session() const { return m_session; }
    QSocSftpClient        *sftp() const { return m_sftp; }
    QSocRemotePathContext *path() { return &m_path; }
    QString                target() const { return m_target; }
    QString                workspace() const { return m_workspace; }

    /** @brief "<target>:<workspace>" UI label, empty while nothing is bound. */
    QString display() const;

    /**
     * @brief Which transport is bound now.
     * @details 0 if and only if nothing was ever bound. Increments by exactly
     *          one on every successful @ref adopt, never decreases, and
     *          survives @ref teardown.
     */
    Generation generation() const { return m_generation; }

    /** @brief Whether @p gen names the transport that is bound now. */
    bool isCurrent(Generation gen) const { return gen != 0 && gen == m_generation; }

    /**
     * @brief Override how a rebind confirms a working directory.
     * @details Production installs none and gets @ref remoteDirectoryExists;
     *          a test installs one to decide what the host answers.
     */
    void setDirectoryProbe(std::function<bool(QSocSftpClient *, const QString &)> probe);

    /** @brief Whether the last successful adopt kept the working directory. */
    bool lastReconnectKeptCwd() const { return m_lastReconnectKeptCwd; }

    /**
     * @brief Background jobs launched over this binding.
     * @details Lives here rather than in the path context because adopt()
     *          overwrites that wholesale, and a reconnect replaces a socket,
     *          not the processes on the far side. Cleared only when the
     *          (target, workspace) pair changes, since a job id means nothing
     *          outside the binding that produced it.
     */
    QSocRemoteJobLedger *jobs() { return &m_jobs; }

    /** @brief Whether a transport is bound and still usable. */
    bool isUsable() const;

    /** @brief Why the workspace cannot be used, empty while it can. */
    QString unusableText() const;

    /**
     * @brief Builds a fresh transport for an existing binding.
     * @details Supplied by whoever created the binding, because only they can
     *          reach the connect helpers. Fills @p out on success.
     */
    using Rebuilder = std::function<bool(
        const QString &target, const QString &workspace, AgentRemoteState *out, QString *errorMessage)>;

    void setRebuilder(Rebuilder rebuilder);

    /** @brief What `reconnect()` did. */
    enum class ReconnectOutcome : std::uint8_t {
        NotNeeded,   /**< The workspace is usable; nothing was touched. */
        Reconnected, /**< A fresh transport is bound. Remote state is unverified. */
        Refused,     /**< No target, workspace or rebuilder to work from. */
        Exhausted,   /**< Every attempt failed; the workspace stays unusable. */
    };

    /**
     * @brief Replace a transport that can no longer serve calls.
     * @details Covers both failure reasons. A stranded protocol keeps the same
     *          host, so nothing observable changed; a dead transport may mean
     *          the host rebooted and everything on it is now in question.
     *          Reconnecting is safe in both cases only because the caller is
     *          required to make the agent re-observe before acting: this
     *          restores the link, it does not resume the work.
     *
     *          The clock is the failed attempt's own connect timeout, so there
     *          is no sleep, no scheduler and no backoff state to own.
     */
    ReconnectOutcome reconnect(QString *errorMessage);

    /** @brief Attempts spent on the most recent reconnect(). */
    int lastReconnectAttempts() const { return m_lastAttempts; }

private:
    /** @brief How many times one reconnect() call tries before giving up. */
    static constexpr int kReconnectAttempts = 2;

    /** @brief Whether a staging bundle carries everything a bind needs. */
    static bool isComplete(const AgentRemoteState &state);

    /**
     * @brief Free the transport, keeping identity and the path context.
     * @details What lets a reconnect free the old transport and still hand
     *          `adopt()` the previous workspace and working directory.
     */
    void closeTransport();

    /** @brief Ask the host whether @p dir is there. */
    bool confirmDirectory(const QString &dir) const;

    QSocSshSession                                        *m_session = nullptr;
    QSocSftpClient                                        *m_sftp    = nullptr;
    QList<QSocSshSession *>                                m_jumps;
    QSocRemotePathContext                                  m_path;
    QString                                                m_target;
    QString                                                m_workspace;
    Rebuilder                                              m_rebuilder;
    std::function<bool(QSocSftpClient *, const QString &)> m_directoryProbe;
    QSocRemoteJobLedger                                    m_jobs;
    Generation                                             m_generation           = 0;
    int                                                    m_lastAttempts         = 0;
    bool                                                   m_lastReconnectKeptCwd = false;
};

/**
 * @brief Open an SSH session (with ProxyJump chain) and SFTP subsystem.
 * @details Parses `[user@]host[:port]` or a `~/.ssh/config` alias, resolves
 *          host config, builds a proxy-jump-aware connection, and opens
 *          the SFTP subsystem on the final session. On failure the helper
 *          tears down anything it allocated and leaves @p state untouched
 *          beyond what was already populated.
 * @param target Raw target string from the user.
 * @param parent QObject parent for newly created sessions.
 * @param state Output struct receiving session/sftp/jumps/targetKey.
 * @param errorMessage Optional sink for a UI-safe error string.
 * @param secretCallback Optional synchronous authentication prompt; not retained.
 * @return True on success, false on any connect or SFTP failure.
 */
bool connectAgentSshSession(
    const QString                 &target,
    QObject                       *parent,
    AgentRemoteState              *state,
    QString                       *errorMessage,
    QSocSshSession::SecretCallback secretCallback = {});

/**
 * @brief Ensure the workspace directory exists on the remote host.
 * @details Calls `mkdir -p` via SFTP and records the workspace on @p state.
 *          The session and SFTP fields of @p state must already be open. The
 *          path context is seeded later, by
 *          `QSocRemoteConnection::adopt()`.
 * @param workspace Remote absolute path.
 * @param state In/out state; reads sftp, writes workspace.
 * @param errorMessage Optional sink for failure detail.
 * @return True on success.
 */
bool prepareAgentRemoteWorkspace(
    const QString &workspace, AgentRemoteState *state, QString *errorMessage);

/**
 * @brief Build the remote-mode tool registry.
 * @details Registers same-named replacements for file, shell, and path
 *          tools that route through the SSH/SFTP backends, plus the
 *          control-plane tools (docs, web fetch, web search) that stay on
 *          the local side. Result is owned by @p parent.
 * @param parent QObject parent for the new registry and tools.
 * @param conn The binding's connection. Transport, identity and path context
 *             are all resolved through it on every tool call, so it must
 *             outlive the registry.
 * @param socConfig Used by the web tools; may be nullptr to skip search.
 * @param monitorSource Optional monitor task source; nullptr skips the
 *                      monitor tools.
 * @return New registry. Never null.
 */
QSocToolRegistry *buildAgentRemoteRegistry(
    QObject               *parent,
    QSocRemoteConnection  *conn,
    QSocConfig            *socConfig,
    QSocMonitorTaskSource *monitorSource = nullptr);

/**
 * @brief File-history accessor that follows a connection's transport.
 * @details Resolves `conn->sftp()` on every call, so a snapshot or a rewind
 *          taken after a reconnect reaches the transport that is bound now.
 *          A capture of the raw client would keep reading the one that was
 *          freed. Every operation fails closed while nothing is bound.
 * @param conn Connection to follow; must outlive the accessor.
 */
QSocFileHistory::LiveFileAccessor remoteLiveFileAccessor(QSocRemoteConnection *conn);

/**
 * @brief What one bounded stat of a remote path established.
 * @details Absent and Silent are separate answers on purpose: a host that
 *          says "no such directory" is alive and serving, while a host that
 *          says nothing has told us nothing about the path.
 */
enum class RemoteProbeResult : std::uint8_t {
    Present, /**< The host answered: the path is there. */
    Absent,  /**< The host answered: the path is not there. */
    Silent,  /**< No usable answer. Nothing may be assumed about the path. */
};

/**
 * @brief Stat one remote path under a liveness budget.
 * @details The session's cached liveness flag only reports a death some
 *          earlier call already ran into. A host that went quiet without
 *          closing the connection leaves that flag reading "fine", so any
 *          decision that matters has to spend one bounded round trip. The
 *          client's own operation budget is restored before returning.
 * @param sftp Connected client; nullptr answers Silent.
 * @param path Path to stat, normally the workspace root or the cwd.
 * @param budgetMs Liveness budget, deliberately far below a transfer budget:
 *                 a caller is usually holding up a keystroke.
 * @param errorMessage Optional sink for a user-safe reason; set on Silent.
 * @return What the host established about @p path.
 */
RemoteProbeResult probeRemotePath(
    QSocSftpClient *sftp, const QString &path, int budgetMs, QString *errorMessage = nullptr);

/**
 * @brief Can this host still serve calls?
 * @details An absent path still answers yes: the question is about the link,
 *          not about the path. Use before reusing or trusting a transport.
 * @return True when the host answered at all.
 */
bool remoteHostAnswers(
    QSocSftpClient *sftp, const QString &path, int budgetMs, QString *errorMessage = nullptr);

/**
 * @brief Is this directory there?
 * @details Silent and Absent are both no: a caller deciding whether to keep
 *          a working directory must not keep one it could not confirm.
 * @return True only when the host confirmed the directory.
 */
bool remoteDirectoryExists(
    QSocSftpClient *sftp, const QString &path, int budgetMs, QString *errorMessage = nullptr);

/**
 * @brief Why a file-history rewind must not touch this workspace.
 * @details A local tree needs no probe, so nothing bound is not a refusal:
 *          treating it as one refuses every rewind in local mode. Once a
 *          transport is bound the link is confirmed for real, because the
 *          cached liveness flag only reports a death some earlier call
 *          already ran into and a rewind is often the first call after a link
 *          goes quiet. On a workspace that cannot answer, "the file is
 *          absent" and "I could not ask" are the same answer, and the absent
 *          reading makes a restore skip removals without saying so.
 * @param conn Connection backing the workspace; nullptr means local.
 * @param budgetMs Liveness budget for the one bounded round trip.
 * @return Empty when the rewind may proceed, else a user-facing reason.
 */
QString remoteWorkspaceRewindRefusal(QSocRemoteConnection *conn, int budgetMs);

/**
 * @brief Outcome of an alias-or-target resolution against the host catalog
 *        and `~/.ssh/config`.
 * @details Tells the caller which connect string to hand to
 *          `connectAgentSshSession()` and, when known, what workspace to
 *          jump straight to (skipping the SFTP path picker).
 */
struct ResolvedHostTarget
{
    QString connectString; /**< Empty when not resolvable. */
    QString workspaceHint; /**< Non-empty when the catalog supplied a workspace. */
    QString capability;    /**< Non-empty when the catalog supplied a capability. */
    bool    fromCatalog   = false;
    bool    fromSshConfig = false;
};

/**
 * @brief Resolve an alias or raw target into a connect string + workspace.
 * @details Precedence:
 *          1. If @p arg matches a `Host` block in `~/.ssh/config`, return
 *             the alias as-is so the existing flow uses ssh-config for
 *             HostName/User/Port/IdentityFile/ProxyJump. Workspace and
 *             capability come from the catalog if it also has an entry
 *             under the same alias.
 *          2. Else if @p arg matches a catalog alias with a `target`
 *             fallback, return that target. Workspace and capability come
 *             from the catalog entry.
 *          3. Else if @p arg looks like a raw `[user@]host[:port]` string,
 *             return it as-is.
 *          4. Else return an empty `connectString` with an error message.
 * @param arg User-supplied alias or target.
 * @param catalog Host catalog (may be null to skip catalog lookup).
 * @param parser Parsed `~/.ssh/config` (may be null to skip ssh-config lookup).
 * @param out Resolved outcome; populated on success.
 * @param errorMessage Optional sink for failure detail.
 * @return True on success.
 */
bool resolveHostTarget(
    const QString             &arg,
    const QSocHostCatalog     *catalog,
    const QSocSshConfigParser *parser,
    ResolvedHostTarget        *out,
    QString                   *errorMessage = nullptr);

#endif // QSOCAGENTREMOTE_H
