// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTREMOTE_H
#define QSOCAGENTREMOTE_H

#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsshsession.h"

#include <QList>
#include <QObject>
#include <QString>

class QSocSshSession;
class QSocSftpClient;
class QSocToolRegistry;
class QSocConfig;
class QSocMonitorTaskSource;
class QSocHostCatalog;
class QSocSshConfigParser;

/**
 * @brief Bundle of remote-session state shared by the agent and its tools.
 * @details Populated step by step by the helper functions below: SSH/SFTP
 *          connect first, then workspace preparation, then registry build.
 *          The struct owns no objects; the caller (typically the CLI worker)
 *          assumes ownership and tears down on `/local` or process exit.
 */
struct AgentRemoteState
{
    QSocSshSession         *session = nullptr;
    QSocSftpClient         *sftp    = nullptr;
    QList<QSocSshSession *> jumps; /* ProxyJump chain, outlives target */
    QSocToolRegistry       *registry = nullptr;
    /* Staging values seeded by prepareAgentRemoteWorkspace(). Tools never
     * bind to this member: buildAgentRemoteRegistry() takes the long-lived
     * context explicitly, so a short-lived state struct cannot leave the
     * tools holding a dangling pointer. */
    QSocRemotePathContext path;

    QString targetKey; /* "user@alias:port", stable lookup key. */
    QString workspace; /* Remote absolute workspace path. */
    QString display;   /* "<targetKey>:<workspace>", UI-safe label. */
};

/**
 * @brief Stable handle to whichever transport currently backs a workspace.
 * @details Tools bind to one of these once and ask it for the session or
 *          SFTP client on every call, so replacing the transport underneath
 *          them needs no re-wiring. That indirection is the whole point:
 *          `AgentRemoteState` already aggregates the same fields, but it is
 *          deliberately short-lived, and a tool holding one directly would
 *          keep reading a transport that has been torn down.
 *
 *          Owned by whoever owns the binding (the CLI worker, or the spawn
 *          tool's host cache) and outlives every registry built from it.
 *          Deliberately not a state machine: liveness lives on the session,
 *          and there is no backoff, scheduler, or failover here.
 */
class QSocRemoteConnection
{
public:
    /** @brief Take over the transport a successful connect produced. */
    void adopt(const AgentRemoteState &state);

    /** @brief Forget the transport without touching it; the caller frees. */
    void release();

    QSocSshSession        *session() const { return m_session; }
    QSocSftpClient        *sftp() const { return m_sftp; }
    QSocRemotePathContext *path() { return &m_path; }
    QString                target() const { return m_target; }
    QString                workspace() const { return m_workspace; }

    /** @brief Whether a transport is bound and still usable. */
    bool isUsable() const;

    /** @brief Why the workspace cannot be used, empty while it can. */
    QString unusableText() const;

private:
    QSocSshSession       *m_session = nullptr;
    QSocSftpClient       *m_sftp    = nullptr;
    QSocRemotePathContext m_path;
    QString               m_target;
    QString               m_workspace;
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
 * @brief Ensure the workspace directory exists and seed the path context.
 * @details Calls `mkdir -p` on the remote workspace via SFTP, then sets
 *          @p state.path root/cwd/writableDirs and computes @p state.display.
 *          The session and SFTP fields of @p state must already be open.
 * @param workspace Remote absolute path.
 * @param state In/out state; reads sftp, writes path/workspace/display.
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
 * @param state Connected session/sftp; must be populated.
 * @param pathCtx Path context the tools bind to. Must outlive the
 *                registry: pass the caller's long-lived context, never
 *                the address of a temporary (copy `state->path` into it
 *                first when the state struct is short-lived).
 * @param socConfig Used by the web tools; may be nullptr to skip search.
 * @return New registry. Never null.
 */
QSocToolRegistry *buildAgentRemoteRegistry(
    QObject               *parent,
    AgentRemoteState      *state,
    QSocRemoteConnection  *conn,
    QSocRemotePathContext *pathCtx,
    QSocConfig            *socConfig,
    QSocMonitorTaskSource *monitorSource = nullptr);

/**
 * @brief Ask a remote workspace whether it can still answer.
 * @details The session's cached liveness flag only reports a death some
 *          earlier call already ran into. A host that went quiet without
 *          closing the connection leaves that flag reading "fine", so any
 *          decision that matters has to spend one bounded round trip.
 * @param sftp Connected client; nullptr answers false.
 * @param root Directory to stat, normally the workspace root.
 * @param budgetMs Liveness budget, deliberately far below a transfer budget:
 *                 a caller is usually holding up a keystroke.
 * @param errorMessage Optional sink for a user-safe reason.
 * @return True when the host answered.
 */
bool remoteWorkspaceAnswers(
    QSocSftpClient *sftp, const QString &root, int budgetMs, QString *errorMessage = nullptr);

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
