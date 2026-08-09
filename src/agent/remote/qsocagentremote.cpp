// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"

#include "agent/qsoctool.h"
#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/tool/qsoctooldoc.h"
#include "agent/tool/qsoctoolmonitor.h"
#include "agent/tool/qsoctoolweb.h"
#include "common/qsocconfig.h"

#include <QDir>
#include <QFileInfo>

#include <functional>
#include <type_traits>
#include <utility>

namespace {

/* One home for the remote path. A `path` member here would let a caller bind
 * tools to a staging bundle again, and the compiler is the only thing that
 * catches it before the tools outlive it. */
template<typename T, typename = void>
struct HasPathMember : std::false_type
{};
template<typename T>
struct HasPathMember<T, std::void_t<decltype(std::declval<T &>().path)>> : std::true_type
{};
static_assert(!HasPathMember<AgentRemoteState>::value, "AgentRemoteState must carry no path");

/* Every consumer holds a QSocRemoteConnection*, so a copy would double-free
 * the transport and a move would dangle all of them at once. */
static_assert(!std::is_copy_constructible_v<QSocRemoteConnection>);
static_assert(!std::is_copy_assignable_v<QSocRemoteConnection>);
static_assert(!std::is_move_constructible_v<QSocRemoteConnection>);
static_assert(!std::is_move_assignable_v<QSocRemoteConnection>);

struct ParsedTarget
{
    QString user;
    QString hostname;
    int     port = 22;
    QString rawAlias; /* hostname before ssh_config resolution */
    bool    explicitUser = false;
    bool    explicitPort = false;
};

bool parseTargetString(const QString &target, ParsedTarget *out, QString *errorMessage)
{
    QString rest = target.trimmed();
    QString user;
    QString hostname;
    int     port = 22;

    const qsizetype atIndex = rest.indexOf(QLatin1Char('@'));
    if (atIndex >= 0) {
        user = rest.left(atIndex);
        rest = rest.mid(atIndex + 1);
    }

    QString hostPortPart = rest;
    if (hostPortPart.contains(QLatin1Char('/'))) {
        hostPortPart = hostPortPart.section(QLatin1Char('/'), 0, 0);
        if (hostPortPart.endsWith(QLatin1Char(':'))) {
            hostPortPart.chop(1);
        }
    }

    bool            explicitPort = false;
    const qsizetype colonIndex   = hostPortPart.lastIndexOf(QLatin1Char(':'));
    if (colonIndex >= 0) {
        hostname             = hostPortPart.left(colonIndex);
        bool      ok         = false;
        const int parsedPort = hostPortPart.mid(colonIndex + 1).toInt(&ok);
        if (ok && parsedPort > 0 && parsedPort < 65536) {
            port         = parsedPort;
            explicitPort = true;
        } else {
            hostname = hostPortPart;
        }
    } else {
        hostname = hostPortPart;
    }

    if (hostname.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("invalid SSH target");
        }
        return false;
    }

    out->user         = user;
    out->hostname     = hostname;
    out->port         = port;
    out->rawAlias     = hostname;
    out->explicitUser = !user.isEmpty();
    out->explicitPort = explicitPort;
    return true;
}

QString defaultOsUser()
{
#ifdef Q_OS_WIN
    return qEnvironmentVariable("USERNAME");
#else
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) {
        user = qEnvironmentVariable("LOGNAME");
    }
    return user;
#endif
}

} // namespace

bool connectAgentSshSession(
    const QString                 &target,
    QObject                       *parent,
    AgentRemoteState              *state,
    QString                       *errorMessage,
    QSocSshSession::SecretCallback secretCallback)
{
    if (state == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("internal error: null state");
        }
        return false;
    }

    ParsedTarget parsed;
    if (!parseTargetString(target, &parsed, errorMessage)) {
        return false;
    }

    /* Pull ~/.ssh/config (with Include chains) so aliases like r9pro
     * resolve to the real HostName/Port/User/IdentityFile. */
    QSocSshConfigParser configParser;
    {
        const QString cfg = QDir::homePath() + QStringLiteral("/.ssh/config");
        if (QFileInfo::exists(cfg)) {
            configParser.parse(cfg);
        }
    }
    const QSocSshHostConfig resolvedCfg = configParser.resolve(parsed.rawAlias);
    QString                 user        = parsed.user;
    QString                 hostname    = parsed.hostname;
    int                     port        = parsed.port;

    if (resolvedCfg.fromConfig) {
        if (!resolvedCfg.hostname.isEmpty()) {
            hostname = resolvedCfg.hostname;
        }
        if (!parsed.explicitPort && resolvedCfg.port > 0) {
            port = resolvedCfg.port;
        }
        if (!parsed.explicitUser && !resolvedCfg.user.isEmpty()) {
            user = resolvedCfg.user;
        }
    }
    if (user.isEmpty()) {
        user = defaultOsUser();
    }
    if (user.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "could not determine a default username; pass user@host explicitly");
        }
        return false;
    }

    /* Stable binding key: tracks the alias the user typed, so lookups
     * don't break if the config later switches HostName for that alias. */
    state->targetKey = QStringLiteral("%1@%2:%3").arg(user, parsed.rawAlias).arg(port);

    QSocSshHostConfig host;
    host.alias         = state->targetKey;
    host.hostname      = hostname;
    host.port          = port;
    host.user          = user;
    host.identityFiles = resolvedCfg.identityFiles;
    /* IdentitiesOnly=yes without any configured IdentityFile would
     * starve auth of keys because our parser does not synthesize the
     * default id_* list. Flip to no so the session's default key
     * enumeration kicks in, matching first-connect UX. */
    host.identitiesOnly = resolvedCfg.identitiesOnly && !host.identityFiles.isEmpty();
    host.proxyJump      = resolvedCfg.proxyJump;
    /* Default to accept-new so first-time connects and ProxyJump hops
     * work without a pre-populated known_hosts. Mismatches still abort. */
    host.strictHostKey = QSocSshHostConfig::StrictHostKey::AcceptNew;

    const QString osDefaultUser = defaultOsUser();

    /* Resolve a hop alias into a QSocSshHostConfig, filling in sensible
     * defaults when the alias is not in the config file. */
    auto hopConfig = [&](const QString &hopAlias) -> QSocSshHostConfig {
        QSocSshHostConfig cfg = configParser.resolve(hopAlias);
        if (!cfg.fromConfig) {
            cfg.hostname = hopAlias;
            cfg.port     = 22;
        }
        if (cfg.user.isEmpty()) {
            cfg.user = osDefaultUser;
        }
        cfg.alias          = hopAlias;
        cfg.strictHostKey  = QSocSshHostConfig::StrictHostKey::AcceptNew;
        cfg.identitiesOnly = cfg.identitiesOnly && !cfg.identityFiles.isEmpty();
        return cfg;
    };

    QList<QSocSshSession *> localJumps;
    std::function<QSocSshSession *(const QSocSshHostConfig &, QSocSshSession *, QString *)>
        connectChain;
    connectChain = [&](const QSocSshHostConfig &cfg,
                       QSocSshSession          *parentSession,
                       QString                 *errOut) -> QSocSshSession                 *{
        QSocSshSession *currentParent = parentSession;
        for (const QString &hopAlias : cfg.proxyJump) {
            const QSocSshHostConfig hopCfg = hopConfig(hopAlias);
            QString                 hopErr;
            QSocSshSession         *hopSession = connectChain(hopCfg, currentParent, &hopErr);
            if (hopSession == nullptr) {
                if (errOut != nullptr) {
                    *errOut = QStringLiteral("ProxyJump via %1 failed: %2").arg(hopAlias, hopErr);
                }
                return nullptr;
            }
            localJumps.append(hopSession);
            currentParent = hopSession;
        }
        auto *session = new QSocSshSession(parent);
        /* ProxyJump children skip prompting: their auth bytes ride
         * through the parent channel and the agent socket path is
         * not reachable; the interactive route only makes sense at
         * the outermost session. */
        if (secretCallback && currentParent == nullptr) {
            session->setSecretCallback(secretCallback);
        }
        QSocSshSession::ConnectStatus status
            = (currentParent != nullptr) ? session->connectToVia(cfg, currentParent, errOut)
                                         : session->connectTo(cfg, errOut);
        session->setSecretCallback({});
        if (status != QSocSshSession::ConnectStatus::Ok) {
            delete session;
            return nullptr;
        }
        return session;
    };

    QString err;
    auto   *newSession = connectChain(host, nullptr, &err);
    if (newSession == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("SSH connect failed: %1").arg(err);
        }
        for (auto it = localJumps.rbegin(); it != localJumps.rend(); ++it) {
            (*it)->disconnectFromHost();
            delete *it;
        }
        return false;
    }

    auto *newSftp = new QSocSftpClient(*newSession);
    if (!newSftp->open(&err)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("SFTP open failed: %1").arg(err);
        }
        delete newSftp;
        newSession->disconnectFromHost();
        delete newSession;
        for (auto it = localJumps.rbegin(); it != localJumps.rend(); ++it) {
            (*it)->disconnectFromHost();
            delete *it;
        }
        return false;
    }

    state->session = newSession;
    state->sftp    = newSftp;
    state->jumps   = localJumps;
    return true;
}

bool prepareAgentRemoteWorkspace(
    const QString &workspace, AgentRemoteState *state, QString *errorMessage)
{
    if (state == nullptr || state->sftp == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("internal error: SFTP not open");
        }
        return false;
    }
    if (workspace.isEmpty() || !workspace.startsWith(QLatin1Char('/'))) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("workspace must be an absolute remote path");
        }
        return false;
    }

    QString err;
    if (!state->sftp->mkdirP(workspace, &err)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("workspace mkdir failed: %1").arg(err);
        }
        return false;
    }

    state->workspace = workspace;
    return true;
}

void discardAgentRemoteState(AgentRemoteState *state)
{
    if (state == nullptr) {
        return;
    }
    if (state->sftp != nullptr) {
        state->sftp->close();
        delete state->sftp;
        state->sftp = nullptr;
    }
    if (state->session != nullptr) {
        state->session->disconnectFromHost();
        delete state->session;
        state->session = nullptr;
    }
    for (auto it = state->jumps.rbegin(); it != state->jumps.rend(); ++it) {
        (*it)->disconnectFromHost();
        delete *it;
    }
    state->jumps.clear();
}

QSocToolRegistry *buildAgentRemoteRegistry(
    QObject               *parent,
    QSocRemoteConnection  *conn,
    QSocConfig            *socConfig,
    QSocMonitorTaskSource *monitorSource)
{
    /* Stable for the connection's lifetime: adopt() rewrites the context in
     * place, so the address the tools bind to never changes. */
    QSocRemotePathContext *pathCtx  = conn->path();
    auto                  *registry = new QSocToolRegistry(parent);
    registry->registerTool(new QSocToolRemoteFileRead(parent, conn, pathCtx));
    registry->registerTool(new QSocToolRemoteFileList(parent, conn, pathCtx));
    registry->registerTool(new QSocToolRemoteFileWrite(parent, conn, pathCtx));
    registry->registerTool(new QSocToolRemoteFileEdit(parent, conn, pathCtx));
    registry->registerTool(new QSocToolRemoteShellBash(parent, conn, pathCtx));
    registry->registerTool(new QSocToolRemoteBashManage(parent, conn, pathCtx));
    registry->registerTool(new QSocToolRemotePath(parent, pathCtx));
    if (monitorSource != nullptr) {
        QSocMonitorTaskSource::RemoteSpec remote;
        remote.targetKey = conn->target();
        remote.workspace = conn->workspace();
        registry->registerTool(new QSocToolMonitor(parent, monitorSource, remote));
        registry->registerTool(new QSocToolMonitorStop(parent, monitorSource));
    }
    /* Control-plane tools stay local even in remote mode. */
    registry->registerTool(new QSocToolDocQuery(parent));
    /* Remote-mode web_fetch has no QLLMService handle (the model lives on the
     * client). Image inlining therefore falls back to alt-text; users who want
     * vision should fetch locally. */
    registry->registerTool(new QSocToolWebFetch(parent, socConfig, /*llm=*/nullptr));
    if (socConfig != nullptr && !socConfig->getValue("web.search_api_url").isEmpty()) {
        registry->registerTool(new QSocToolWebSearch(parent, socConfig));
    }
    return registry;
}

namespace {

/* Liveness budget for re-verifying the working directory after a reconnect. */
constexpr int kReconnectProbeMs = 3000;

} // namespace

QSocRemoteConnection::~QSocRemoteConnection()
{
    teardown();
}

bool QSocRemoteConnection::isComplete(const AgentRemoteState &state)
{
    return state.session != nullptr && state.sftp != nullptr && !state.workspace.isEmpty();
}

bool QSocRemoteConnection::confirmDirectory(const QString &dir) const
{
    if (m_directoryProbe) {
        return m_directoryProbe(m_sftp, dir);
    }
    return remoteDirectoryExists(m_sftp, dir, kReconnectProbeMs);
}

void QSocRemoteConnection::setDirectoryProbe(
    std::function<bool(QSocSftpClient *, const QString &)> probe)
{
    m_directoryProbe = std::move(probe);
}

bool QSocRemoteConnection::adopt(AgentRemoteState &&state)
{
    if (!isComplete(state)) {
        return false;
    }
    /* One predicate for "this is the same binding", used for the working
     * directory and for the job ledger alike. The host belongs in it as much
     * as the path does: the same directory name on a different host is a
     * different directory, and a job id there names a different process. */
    const bool        rebindsSameBinding = !m_workspace.isEmpty() && m_workspace == state.workspace
                                           && m_target == state.targetKey;
    const QString     previousCwd        = rebindsSameBinding ? m_path.cwd() : QString();
    const QStringList previousWritable = rebindsSameBinding ? m_path.writableDirs() : QStringList();
    if (!rebindsSameBinding) {
        m_jobs.clear();
    }

    m_session   = state.session;
    m_sftp      = state.sftp;
    m_jumps     = state.jumps;
    m_target    = state.targetKey;
    m_workspace = state.workspace;

    state.session = nullptr;
    state.sftp    = nullptr;
    state.jumps.clear();
    state.targetKey.clear();
    state.workspace.clear();

    /* A fresh context, so every believed file content goes with the transport
     * that was observed producing it. The read-before-overwrite guard keys off
     * this, and it is what forces a re-read before any edit after a
     * reconnect. */
    m_path = QSocRemotePathContext{};
    m_path.setRoot(m_workspace);
    m_lastReconnectKeptCwd = false;
    if (rebindsSameBinding) {
        m_path.setWritableDirs(previousWritable);
        /* The working directory is a path, not a handle, so it survives when
         * it still exists. Verifying beats assuming: the host may have
         * rebooted out from under it. */
        if (!previousCwd.isEmpty() && confirmDirectory(previousCwd)) {
            m_path.setCwd(previousCwd);
            m_lastReconnectKeptCwd = true;
        } else {
            m_path.setCwd(m_workspace);
        }
    } else {
        m_path.setCwd(m_workspace);
        m_path.setWritableDirs({m_workspace});
    }
    ++m_generation;
    return true;
}

void QSocRemoteConnection::closeTransport()
{
    if (m_sftp != nullptr) {
        m_sftp->close();
        delete m_sftp;
        m_sftp = nullptr;
    }
    if (m_session != nullptr) {
        m_session->disconnectFromHost();
        delete m_session;
        m_session = nullptr;
    }
    for (auto it = m_jumps.rbegin(); it != m_jumps.rend(); ++it) {
        (*it)->disconnectFromHost();
        delete *it;
    }
    m_jumps.clear();
}

void QSocRemoteConnection::teardown()
{
    closeTransport();
    m_path = QSocRemotePathContext{};
    m_target.clear();
    m_workspace.clear();
    m_lastReconnectKeptCwd = false;
}

QString QSocRemoteConnection::display() const
{
    if (m_target.isEmpty() || m_workspace.isEmpty()) {
        return {};
    }
    return m_target + QStringLiteral(":") + m_workspace;
}

bool QSocRemoteConnection::isUsable() const
{
    return m_session != nullptr && m_session->isConnected();
}

QString QSocRemoteConnection::unusableText() const
{
    if (m_session == nullptr) {
        return QStringLiteral("no remote workspace is bound");
    }
    return m_session->unusableText();
}

void QSocRemoteConnection::setRebuilder(Rebuilder rebuilder)
{
    m_rebuilder = std::move(rebuilder);
}

QSocRemoteConnection::ReconnectOutcome QSocRemoteConnection::reconnect(QString *errorMessage)
{
    m_lastAttempts = 0;
    if (m_session != nullptr && m_session->isConnected()) {
        return ReconnectOutcome::NotNeeded;
    }
    if (!m_rebuilder || m_target.isEmpty() || m_workspace.isEmpty()) {
        return ReconnectOutcome::Refused;
    }

    const QString target    = m_target;
    const QString workspace = m_workspace;

    for (int attempt = 1; attempt <= kReconnectAttempts; ++attempt) {
        m_lastAttempts = attempt;
        AgentRemoteState fresh;
        if (!m_rebuilder(target, workspace, &fresh, errorMessage)) {
            /* A failed attempt leaves the caller exactly as it was, so the
             * previous transport is still ours to free below. */
            continue;
        }
        if (!isComplete(fresh)) {
            /* Asked before anything is freed, because adopt() would refuse
             * this and the previous transport would already be gone. */
            discardAgentRemoteState(&fresh);
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("reconnect produced an incomplete transport");
            }
            continue;
        }
        /* Only once a replacement exists: without this the previous session,
         * its SFTP channel and its whole ProxyJump chain are never freed. The
         * identity and the working directory stay on this object, which is
         * what lets adopt() below recognise the rebind and keep the cwd.
         * Background job stamps are deliberately kept: those jobs may still be
         * running, and the agent is told to verify them either way. */
        closeTransport();
        adopt(std::move(fresh));
        return ReconnectOutcome::Reconnected;
    }
    return ReconnectOutcome::Exhausted;
}

QSocFileHistory::LiveFileAccessor remoteLiveFileAccessor(QSocRemoteConnection *conn)
{
    QSocFileHistory::LiveFileAccessor accessor;
    /* The two enums list their states in different orders on purpose, so
     * every mapping between them is spelled out. */
    const auto mapPresence = [](QSocSftpClient::Presence presence) {
        switch (presence) {
        case QSocSftpClient::Presence::Absent:
            return QSocFileHistory::FileState::Absent;
        case QSocSftpClient::Presence::Present:
            return QSocFileHistory::FileState::Present;
        case QSocSftpClient::Presence::Unknown:
            break;
        }
        return QSocFileHistory::FileState::Unknown;
    };
    accessor.exists = [conn, mapPresence](const QString &path) {
        QSocSftpClient *sftp = (conn != nullptr) ? conn->sftp() : nullptr;
        if (sftp == nullptr) {
            return QSocFileHistory::FileState::Unknown;
        }
        return mapPresence(sftp->presence(path));
    };
    accessor.read = [conn, mapPresence](const QString &path) {
        QSocSftpClient *sftp = (conn != nullptr) ? conn->sftp() : nullptr;
        if (sftp == nullptr) {
            return QSocFileHistory::LiveRead::unknown();
        }
        QString          err;
        const QByteArray bytes = sftp->readFile(path, 0, &err);
        /* readFile sets err only on failure, so an empty err means the server
         * answered and a null QByteArray is an empty file, not a failure. */
        if (err.isEmpty()) {
            return QSocFileHistory::LiveRead::present(QString::fromUtf8(bytes));
        }
        /* One extra round trip, and only on the failure path, to separate
         * "there is nothing to read" from "I could not read it". A file that
         * is there and unreadable is unknown content; lastError() is sticky
         * across calls and lastFailureUncertain() answers a different
         * question, so neither can decide this. */
        switch (mapPresence(sftp->presence(path))) {
        case QSocFileHistory::FileState::Absent:
            return QSocFileHistory::LiveRead::absent();
        case QSocFileHistory::FileState::Present:
        case QSocFileHistory::FileState::Unknown:
            break;
        }
        return QSocFileHistory::LiveRead::unknown();
    };
    accessor.write = [conn](const QString &path, const QString &content) {
        QSocSftpClient *sftp = (conn != nullptr) ? conn->sftp() : nullptr;
        if (sftp == nullptr) {
            return false;
        }
        QString err;
        return sftp->writeFile(path, content.toUtf8(), &err);
    };
    accessor.remove = [conn](const QString &path) {
        QSocSftpClient *sftp = (conn != nullptr) ? conn->sftp() : nullptr;
        if (sftp == nullptr) {
            return false;
        }
        QString err;
        return sftp->removeFile(path, &err);
    };
    accessor.generation = [conn]() -> quint64 { return (conn != nullptr) ? conn->generation() : 0; };
    return accessor;
}

RemoteProbeResult probeRemotePath(
    QSocSftpClient *sftp, const QString &path, int budgetMs, QString *errorMessage)
{
    if (sftp == nullptr || path.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("no remote workspace is bound");
        }
        return RemoteProbeResult::Silent;
    }
    const int saved = sftp->operationTimeoutMs();
    sftp->setOperationTimeoutMs(budgetMs);
    QString    probeErr;
    const auto presence = sftp->presence(path, &probeErr);
    sftp->setOperationTimeoutMs(saved);
    switch (presence) {
    case QSocSftpClient::Presence::Present:
        return RemoteProbeResult::Present;
    case QSocSftpClient::Presence::Absent:
        return RemoteProbeResult::Absent;
    case QSocSftpClient::Presence::Unknown:
        break;
    }
    if (errorMessage != nullptr) {
        *errorMessage = probeErr;
    }
    return RemoteProbeResult::Silent;
}

bool remoteHostAnswers(QSocSftpClient *sftp, const QString &path, int budgetMs, QString *errorMessage)
{
    return probeRemotePath(sftp, path, budgetMs, errorMessage) != RemoteProbeResult::Silent;
}

bool remoteDirectoryExists(
    QSocSftpClient *sftp, const QString &path, int budgetMs, QString *errorMessage)
{
    const auto result = probeRemotePath(sftp, path, budgetMs, errorMessage);
    if (result == RemoteProbeResult::Absent && errorMessage != nullptr) {
        *errorMessage = QStringLiteral("%1 does not exist on the remote host").arg(path);
    }
    return result == RemoteProbeResult::Present;
}

QString remoteWorkspaceRewindRefusal(QSocRemoteConnection *conn, int budgetMs)
{
    /* No transport was ever bound, or /local unbound it: the working tree is
     * this machine's disk and needs no probe. */
    if (conn == nullptr || conn->session() == nullptr) {
        return {};
    }
    if (!conn->isUsable()) {
        /* A session that was never connected records no failure reason, and an
         * empty string here would read as "go ahead". */
        const QString why = conn->unusableText();
        return why.isEmpty() ? QStringLiteral("the remote workspace is not connected") : why;
    }
    if (conn->sftp() == nullptr) {
        return QStringLiteral("no remote workspace is bound");
    }
    QString probeErr;
    if (!remoteHostAnswers(conn->sftp(), conn->path()->root(), budgetMs, &probeErr)) {
        return probeErr.isEmpty() ? QStringLiteral("the remote host did not answer") : probeErr;
    }
    return {};
}

bool resolveHostTarget(
    const QString             &arg,
    const QSocHostCatalog     *catalog,
    const QSocSshConfigParser *parser,
    ResolvedHostTarget        *out,
    QString                   *errorMessage)
{
    if (out == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("internal error: null out");
        }
        return false;
    }
    *out                  = ResolvedHostTarget{};
    const QString trimmed = arg.trimmed();
    if (trimmed.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("empty alias or target");
        }
        return false;
    }
    out->connectString = trimmed;

    if (parser != nullptr) {
        const auto resolved = parser->resolve(trimmed);
        if (resolved.fromConfig) {
            out->fromSshConfig = true;
            if (catalog != nullptr) {
                const auto *entry = catalog->find(trimmed);
                if (entry != nullptr) {
                    out->workspaceHint = entry->workspace;
                    out->capability    = entry->capability;
                    out->fromCatalog   = true;
                }
            }
            return true;
        }
    }

    if (catalog != nullptr) {
        const auto *entry = catalog->find(trimmed);
        if (entry != nullptr) {
            out->fromCatalog   = true;
            out->workspaceHint = entry->workspace;
            out->capability    = entry->capability;
            if (!entry->target.isEmpty()) {
                out->connectString = entry->target;
                return true;
            }
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral(
                                    "alias %1 is in the catalog but has no target "
                                    "and is not in ~/.ssh/config; set target via "
                                    "host_update or define Host %1 in ~/.ssh/config")
                                    .arg(trimmed);
            }
            return false;
        }
    }

    /* Pass through: connectAgentSshSession() understands raw
     * `[user@]host[:port]` strings and will report a parse error if
     * the input is malformed. */
    return true;
}
