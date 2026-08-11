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
    QList<QSocSshSession *> jumps;              /* ProxyJump chain, outlives target */
    QString                 targetKey;          /* "user@alias:port", stable lookup key. */
    QString                 endpointIdentity;   /* User plus final host-key identity. */
    QString                 workspace;          /* Remote absolute workspace path. */
    QString                 canonicalWorkspace; /* Host-resolved workspace identity. */
    QString                 workspaceTreeId;    /* Persistent random id stored in the root. */
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
     *          an incomplete bundle (no session, SFTP or workspace identity)
     *          without consuming it or changing anything, so a caller can ask
     *          before it frees the transport being replaced.
     *
     *          Path seeding: a first bind, or a bind to a different
     *          workspace, seeds root = cwd = workspace and writableDirs =
     *          {workspace}. A rebind to the SAME workspace keeps the working
     *          directory and the writable set, re-verifies the working
     *          directory against the host, and falls back to the root when
     *          the host does not confirm it. Whichever directory it lands on
     *          is published to the working-directory observer, so a rewind
     *          cannot leave the system prompt naming a directory the session
     *          has left. Believed file contents are forgotten either way: the
     *          read-before-overwrite guard keys off them, and nothing on the
     *          host has been observed since.
     * @return True when the transport was adopted.
     */
    bool adopt(AgentRemoteState &&state);

    /**
     * @brief Close and free the bound transport in dependency order.
     * @details SFTP, then the session, then the ProxyJump chain in reverse so
     *          children disconnect before their parents. Also clears the
     *          identity and the path context, so a later `adopt()` seeds a
     *          fresh working directory. The working-directory observer stays
     *          installed: it belongs to the binding, not to the transport.
     *          Leaves @ref generation alone: a stale
     *          holder must keep reading stale rather than matching a later
     *          rebind by accident. Safe to call with nothing bound.
     */
    void teardown();

    QSocSshSession        *session() const { return m_session; }
    QSocSftpClient        *sftp() const { return m_sftp; }
    QSocRemotePathContext *path() { return &m_path; }
    QString                target() const { return m_target; }
    QString                endpointIdentity() const { return m_endpointIdentity; }
    QString                workspace() const { return m_workspace; }
    QString                canonicalWorkspace() const { return m_canonicalWorkspace; }
    QString                workspaceTreeId() const { return m_workspaceTreeId; }

    /** @brief What @ref setWorkingDirectory decided. */
    enum class CwdChange : std::uint8_t {
        Changed,      /**< @ref path() now reports the new working directory. */
        Outside,      /**< The host resolves the request outside the root. */
        Unresolvable, /**< The host holds the name but cannot follow it. */
        Unknown,      /**< The host gave no usable answer. */
        Refused,      /**< Nothing is bound, so there is no host to ask. */
    };

    /**
     * @brief Move the working directory to where a request asks.
     * @details Every accepted change goes through
     *          @ref QSocRemotePathContext::setCwd, here and in @ref adopt
     *          alike, so the copies of it are written wherever it moves.
     *
     *          Lexical clamping alone cannot see a symlink: a directory whose
     *          name sits under the root while the host resolves it elsewhere
     *          passes a byte-prefix test, and the working directory then names
     *          a place the session is not. So the clamped request is
     *          canonicalized on the host and the canonical answer is what gets
     *          clamped, against a canonical root so both sides are in one
     *          spelling.
     *
     *          What is stored is the clamped lexical name, not the canonical
     *          one: the root is held lexically, and a canonical working
     *          directory under a root that is itself reached through a symlink
     *          would fail every later containment test in the workspace it is
     *          actually inside.
     *
     *          An answer that is not Ok leaves the working directory alone.
     *          Moving to a name the host cannot resolve would publish a
     *          directory nothing has confirmed, and every relative path the
     *          tools derive would be built on it.
     * @param errorMessage Optional sink for a user-safe reason.
     */
    CwdChange setWorkingDirectory(const QString &requested, QString *errorMessage = nullptr);

    /**
     * @brief Resolve a path and prove it remains inside a writable directory.
     * @details The returned canonical path is the one callers must operate on;
     *          checking a canonical name and then using the lexical one would
     *          reopen a symlink escape between history and ordinary tools.
     */
    bool resolveWritablePath(
        const QString &requested, QString *canonicalPath, QString *errorMessage = nullptr) const;

    /** @brief Resolve a writable directory entry without following its leaf. */
    bool resolveWritableEntry(
        const QString &requested, QString *entryPath, QString *errorMessage = nullptr) const;

    /** @brief Resolve and verify the working directory bound to this workspace. */
    bool resolveBoundCwd(QString *canonicalCwd, QString *errorMessage = nullptr) const;

    /**
     * @brief Observe every accepted working-directory change.
     * @details The working directory lives in the path context, but the agent
     *          config carries a copy of it into the system prompt and the hook
     *          envelope. This is how that copy is written, and it is installed
     *          on the context itself rather than here, so a reconnect that
     *          rewinds the directory publishes on the same path as an explicit
     *          request: there is one mutator, and it is the one that publishes.
     */
    void setWorkingDirectoryObserver(std::function<void(const QString &)> observer);

    /** @brief "<target>:<workspace>" UI label, empty while nothing is bound. */
    QString display() const;

    /**
     * @brief Which transport is bound now.
     * @details 0 if and only if nothing was ever bound. Increments by exactly
     *          one on every successful @ref adopt, never decreases, and
     *          survives @ref teardown.
     */
    Generation generation() const { return m_generation; }

    /** @brief Opaque identity of the currently bound transport. */
    QString transportLink() const { return m_transportLink; }

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
     *          reach the connect helpers. Fills @p out on success and spends
     *          only the absolute deadline supplied by reconnect().
     */
    using Rebuilder = std::function<bool(
        const QString    &target,
        const QString    &workspace,
        AgentRemoteState *out,
        QString          *errorMessage,
        QDeadlineTimer    deadline)>;

    void setRebuilder(Rebuilder rebuilder);

    /** @brief What `reconnect()` did. */
    enum class ReconnectOutcome : std::uint8_t {
        NotNeeded,   /**< The workspace is usable; nothing was touched. */
        Reconnected, /**< A fresh transport is bound. Remote state is unverified. */
        Refused,     /**< No target, workspace or rebuilder to work from. */
        Exhausted,   /**< Every attempt failed; the workspace stays unusable. */
        BudgetSpent, /**< This turn already spent its budget. Nothing was tried. */
        Aborted,     /**< The user asked to stop. Nothing further was tried. */
    };

    /**
     * @brief Hand the caller a full reconnect budget again.
     * @details Called when a new user request begins. A turn that reconnected
     *          once does not reconnect again, because a turn that keeps
     *          reconnecting is one where nothing on the host has been observed
     *          working between the attempts, and each of them holds the event
     *          loop for a full connect sequence. A new request is a new
     *          intent, so it gets the full budget.
     */
    void resetReconnectBudget();

    /** @brief Whether this turn has already spent its reconnect budget. */
    bool reconnectBudgetSpent() const { return m_reconnectsUsed >= kReconnectBudgetPerTurn; }

    /**
     * @brief Install the predicate `reconnect()` consults between attempts.
     * @details Each attempt is a full connect, so a user who asked to stop
     *          must not be made to sit through the next one. Consulted before
     *          every attempt including the first.
     */
    void setAbortProbe(std::function<bool()> probe);

    /**
     * @brief The installed abort probe, so a rebuilder can pass it down.
     * @details A connect attempt is a sequence of poll loops inside the
     *          session, and only the session can cut one of them short. Empty
     *          when none is installed.
     */
    std::function<bool()> abortProbe() const { return m_abortProbe; }

    /**
     * @brief Replace a transport that can no longer serve calls.
     * @details Covers both failure reasons. A stranded protocol keeps the same
     *          host, so nothing observable changed; a dead transport may mean
     *          the host rebooted and everything on it is now in question.
     *          Reconnecting is safe in both cases only because the caller is
     *          required to make the agent re-observe before acting: this
     *          restores the link, it does not resume the work.
     *
     *          Every attempt and bind stage shares one absolute clock, so a
     *          retry receives only the time the earlier attempt left.
     */
    /**
     * @param budget Optional caller-owned reconnect counter. A binding shared
     *               across sibling sub-agents must not share one budget: a
     *               counter on the connection would let one child's reconnect
     *               deny another's genuine drop, and a fresh child re-credit a
     *               spent one. When supplied, this counter is consulted and
     *               spent instead of the connection's. The main connection has
     *               no siblings and passes nullptr to use its own.
     */
    ReconnectOutcome reconnect(QString *errorMessage, int *budget = nullptr);

    /** @brief Reconnect while consuming an existing absolute deadline. */
    ReconnectOutcome reconnect(QString *errorMessage, int *budget, QDeadlineTimer deadline);

    /** @brief Attempts spent on the most recent reconnect(). */
    int lastReconnectAttempts() const { return m_lastAttempts; }

private:
    /** @brief How many times one reconnect() call tries before giving up. */
    static constexpr int kReconnectAttempts = 2;

    /** @brief One absolute budget shared by every attempt and bind stage. */
    static constexpr int kReconnectDeadlineMs = 30000;

    /**
     * @brief How many reconnect() calls one turn may spend.
     * @details Bounds the turn, not the call. Every tool call in a turn
     *          consults the workspace, so without this a turn pays a full
     *          connect sequence per call and the event loop is held for the
     *          sum of them.
     */
    static constexpr int kReconnectBudgetPerTurn = 1;

    /** @brief Whether a staging bundle carries everything a bind needs. */
    static bool isComplete(const AgentRemoteState &state);

    /**
     * @brief Free the transport, keeping identity and the path context.
     * @details What lets a reconnect free the old transport and still hand
     *          `adopt()` the previous workspace and working directory.
     */
    void closeTransport();

    /** @brief Ask the host whether @p dir is there. */
    bool confirmDirectory(const QString &dir, const QDeadlineTimer *deadline = nullptr) const;

    /** @brief Adopt while bounding the optional cwd verification. */
    bool adoptWithin(AgentRemoteState &&state, const QDeadlineTimer *deadline);

    /** @brief Verify the canonical root and its persistent tree marker. */
    bool verifyWorkspaceBinding(QString *canonicalRoot, QString *errorMessage) const;

    /** @brief Resolve one directory and prove it is inside the bound root. */
    bool resolveBoundDirectory(
        const QString &dir, QString *canonicalDir, QString *errorMessage) const;

    /** @brief Validate configured writable roots against their bound anchors. */
    bool canonicalWritableDirs(QStringList *dirs, QString *errorMessage) const;

    QSocSshSession                                        *m_session = nullptr;
    QSocSftpClient                                        *m_sftp    = nullptr;
    QList<QSocSshSession *>                                m_jumps;
    QSocRemotePathContext                                  m_path;
    QString                                                m_target;
    QString                                                m_endpointIdentity;
    QString                                                m_workspace;
    QString                                                m_canonicalWorkspace;
    QString                                                m_workspaceTreeId;
    QString                                                m_transportLink;
    QHash<QString, QString>                                m_writableAnchors;
    Rebuilder                                              m_rebuilder;
    std::function<bool(QSocSftpClient *, const QString &)> m_directoryProbe;
    std::function<bool()>                                  m_abortProbe;
    QSocRemoteJobLedger                                    m_jobs;
    Generation                                             m_generation           = 0;
    int                                                    m_lastAttempts         = 0;
    int                                                    m_reconnectsUsed       = 0;
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
 * @param abortProbe Optional stop predicate, installed on every session in the
 *                   chain so a poll inside any of them can be cut short.
 * @param deadline Absolute budget supplied to each hop and SFTP startup.
 * @return True on success, false on any connect or SFTP failure.
 */
bool connectAgentSshSession(
    const QString                 &target,
    QObject                       *parent,
    AgentRemoteState              *state,
    QString                       *errorMessage,
    QSocSshSession::SecretCallback secretCallback = {},
    std::function<bool()>          abortProbe     = {},
    QDeadlineTimer                 deadline       = QDeadlineTimer(30000));

/**
 * @brief Ensure and identify the workspace directory on the remote host.
 * @details Calls `mkdir -p` via SFTP, creates a random persistent tree marker
 *          when absent, and records both identities on @p state.
 *          The session and SFTP fields of @p state must already be open. The
 *          path context is seeded later, by
 *          `QSocRemoteConnection::adopt()`.
 * @param workspace Remote absolute path.
 * @param state In/out state; reads sftp, writes workspace identities.
 * @param errorMessage Optional sink for failure detail.
 * @param deadline Absolute budget shared by every workspace operation.
 * @return True on success.
 */
bool prepareAgentRemoteWorkspace(
    const QString    &workspace,
    AgentRemoteState *state,
    QString          *errorMessage,
    QDeadlineTimer    deadline = QDeadlineTimer(30000));

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
