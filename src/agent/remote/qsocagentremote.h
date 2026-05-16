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
    QSocRemotePathContext   path;

    QString targetKey; /* "user@alias:port", stable lookup key. */
    QString workspace; /* Remote absolute workspace path. */
    QString display;   /* "<targetKey>:<workspace>", UI-safe label. */
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
 * @param state Connected session/sftp/path; must be populated.
 * @param socConfig Used by the web tools; may be nullptr to skip search.
 * @return New registry. Never null.
 */
QSocToolRegistry *buildAgentRemoteRegistry(
    QObject               *parent,
    AgentRemoteState      *state,
    QSocConfig            *socConfig,
    QSocMonitorTaskSource *monitorSource = nullptr);

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
