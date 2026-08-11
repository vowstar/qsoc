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
#include <QScopeGuard>
#include <QSet>
#include <QUuid>

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

QString remoteChildPath(const QString &parent, const QString &child)
{
    return parent == QStringLiteral("/") ? parent + child : parent + QLatin1Char('/') + child;
}

QString workspaceMetadataDir(const QString &root)
{
    return remoteChildPath(root, QStringLiteral(".qsoc-agent"));
}

QString workspaceTreeMarker(const QString &metadataDir)
{
    return remoteChildPath(metadataDir, QStringLiteral("tree-id"));
}

bool parseWorkspaceTreeId(const QByteArray &bytes, QString *treeId)
{
    const QString value  = QString::fromUtf8(bytes);
    const QUuid   parsed = QUuid::fromString(value);
    if (parsed.isNull() || parsed.toString(QUuid::WithoutBraces) != value) {
        return false;
    }
    if (treeId != nullptr) {
        *treeId = value;
    }
    return true;
}

bool resolveWorkspaceMetadataDir(
    QSocSftpClient *sftp,
    const QString  &canonicalRoot,
    bool            create,
    QString        *metadataDir,
    QString        *errorMessage)
{
    if (metadataDir != nullptr) {
        metadataDir->clear();
    }
    if (sftp == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("remote SFTP client is not connected");
        }
        return false;
    }

    const QString expected = workspaceMetadataDir(canonicalRoot);
    QString       err;
    switch (sftp->linkPresence(expected, &err)) {
    case QSocSftpClient::Presence::Present:
        break;
    case QSocSftpClient::Presence::Absent:
        if (!create) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("remote workspace metadata is missing");
            }
            return false;
        }
        if (!sftp->mkdirP(expected, &err)) {
            if (errorMessage != nullptr) {
                *errorMessage = err.isEmpty()
                                    ? QStringLiteral("remote workspace metadata cannot be created")
                                    : err;
            }
            return false;
        }
        break;
    case QSocSftpClient::Presence::Unknown:
        if (errorMessage != nullptr) {
            *errorMessage = err.isEmpty()
                                ? QStringLiteral("remote workspace metadata cannot be inspected")
                                : err;
        }
        return false;
    }

    QString resolved;
    if (sftp->canonicalize(expected, &resolved, &err) != QSocSftpClient::Canonical::Ok) {
        if (errorMessage != nullptr) {
            *errorMessage = err.isEmpty()
                                ? QStringLiteral("remote workspace metadata cannot be resolved")
                                : err;
        }
        return false;
    }
    if (resolved != expected) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("remote workspace metadata changed identity");
        }
        return false;
    }
    if (metadataDir != nullptr) {
        *metadataDir = resolved;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool readWorkspaceTreeId(
    QSocSftpClient *sftp, const QString &marker, QString *treeId, QString *errorMessage)
{
    QString canonicalMarker;
    QString err;
    if (sftp == nullptr
        || sftp->canonicalize(marker, &canonicalMarker, &err) != QSocSftpClient::Canonical::Ok) {
        if (errorMessage != nullptr) {
            *errorMessage = err.isEmpty()
                                ? QStringLiteral("remote workspace tree marker cannot be resolved")
                                : err;
        }
        return false;
    }
    if (canonicalMarker != marker) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("remote workspace tree marker changed identity");
        }
        return false;
    }
    const QByteArray bytes = sftp->readFile(marker, 128, &err);
    QString          value;
    if (!err.isEmpty() || !parseWorkspaceTreeId(bytes, &value)) {
        if (errorMessage != nullptr) {
            *errorMessage = err.isEmpty()
                                ? QStringLiteral("remote workspace tree marker is invalid")
                                : err;
        }
        return false;
    }
    if (treeId != nullptr) {
        *treeId = value;
    }
    return true;
}

} // namespace

bool connectAgentSshSession(
    const QString                 &target,
    QObject                       *parent,
    AgentRemoteState              *state,
    QString                       *errorMessage,
    QSocSshSession::SecretCallback secretCallback,
    std::function<bool()>          abortProbe,
    QDeadlineTimer                 deadline)
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
        /* Every session in the chain, not just the outermost: a hop's own
         * handshake and auth are polls of their own, and a stop during one of
         * them is the same stop. */
        session->setAbortProbe(abortProbe);
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

    state->session          = newSession;
    state->sftp             = newSftp;
    state->jumps            = localJumps;
    state->endpointIdentity = user + QLatin1Char(':') + newSession->hostKeyIdentity();
    if (state->endpointIdentity.endsWith(QLatin1Char(':'))) {
        discardAgentRemoteState(state);
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("SSH host did not provide a stable identity");
        }
        return false;
    }
    return true;
}

bool prepareAgentRemoteWorkspace(
    const QString    &workspace,
    AgentRemoteState *state,
    QString          *errorMessage,
    QDeadlineTimer    deadline)
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

    if (deadline.hasExpired()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("workspace preparation did not finish in time");
        }
        return false;
    }
    return state->sftp->runWithin(deadline, [&] {
        QString err;
        if (!state->sftp->mkdirP(workspace, &err)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("workspace mkdir failed: %1").arg(err);
            }
            return false;
        }

        QString canonicalWorkspace;
        switch (state->sftp->canonicalize(workspace, &canonicalWorkspace, &err)) {
        case QSocSftpClient::Canonical::Ok:
            break;
        case QSocSftpClient::Canonical::Unresolvable:
        case QSocSftpClient::Canonical::Unknown:
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("workspace resolve failed: %1").arg(err);
            }
            return false;
        }

        QString metadataDir;
        if (!resolveWorkspaceMetadataDir(state->sftp, canonicalWorkspace, true, &metadataDir, &err)) {
            if (errorMessage != nullptr) {
                *errorMessage
                    = QStringLiteral("workspace metadata verification failed: %1").arg(err);
            }
            return false;
        }

        const QString marker = workspaceTreeMarker(metadataDir);
        switch (state->sftp->linkPresence(marker, &err)) {
        case QSocSftpClient::Presence::Present:
            break;
        case QSocSftpClient::Presence::Absent: {
            const QString fresh   = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto    created = state->sftp->createFileIfAbsent(marker, fresh.toUtf8(), &err);
            if (created == QSocSftpClient::CreateOutcome::Failed
                || created == QSocSftpClient::CreateOutcome::Unknown) {
                if (errorMessage != nullptr) {
                    *errorMessage
                        = QStringLiteral("workspace identity creation failed: %1").arg(err);
                }
                return false;
            }
            break;
        }
        case QSocSftpClient::Presence::Unknown:
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("workspace identity lookup failed: %1").arg(err);
            }
            return false;
        }

        QString workspaceTreeId;
        if (!readWorkspaceTreeId(state->sftp, marker, &workspaceTreeId, &err)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("workspace identity read failed: %1").arg(err);
            }
            return false;
        }

        state->workspace          = workspace;
        state->canonicalWorkspace = canonicalWorkspace;
        state->workspaceTreeId    = workspaceTreeId;
        return true;
    });
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
    state->targetKey.clear();
    state->endpointIdentity.clear();
    state->workspace.clear();
    state->canonicalWorkspace.clear();
    state->workspaceTreeId.clear();
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
    registry->registerTool(new QSocToolRemotePath(parent, conn, pathCtx));
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

constexpr int kReconnectProbeMs = 3000;

} // namespace

QSocRemoteConnection::~QSocRemoteConnection()
{
    teardown();
}

bool QSocRemoteConnection::isComplete(const AgentRemoteState &state)
{
    return state.session != nullptr && state.sftp != nullptr && !state.endpointIdentity.isEmpty()
           && !state.workspace.isEmpty() && !state.canonicalWorkspace.isEmpty()
           && !state.workspaceTreeId.isEmpty();
}

bool QSocRemoteConnection::confirmDirectory(const QString &dir, const QDeadlineTimer *deadline) const
{
    if (deadline != nullptr && deadline->hasExpired()) {
        return false;
    }
    if (m_directoryProbe) {
        return m_directoryProbe(m_sftp, dir);
    }
    QString canonical;
    QString err;
    if (deadline == nullptr) {
        return resolveBoundDirectory(dir, &canonical, &err);
    }
    return m_sftp != nullptr && m_sftp->runWithin(*deadline, [&] {
        return resolveBoundDirectory(dir, &canonical, &err);
    });
}

void QSocRemoteConnection::setDirectoryProbe(
    std::function<bool(QSocSftpClient *, const QString &)> probe)
{
    m_directoryProbe = std::move(probe);
}

bool QSocRemoteConnection::adopt(AgentRemoteState &&state)
{
    return adoptWithin(std::move(state), nullptr);
}

bool QSocRemoteConnection::adoptWithin(AgentRemoteState &&state, const QDeadlineTimer *deadline)
{
    if (!isComplete(state)) {
        return false;
    }
    /* One predicate for "this is the same binding", used for the working
     * directory and for the job ledger alike. The host belongs in it as much
     * as the path does: the same directory name on a different host is a
     * different directory, and a job id there names a different process. */
    const bool        rebindsSameBinding = !m_workspace.isEmpty() && m_workspace == state.workspace
                                           && m_target == state.targetKey
                                           && m_endpointIdentity == state.endpointIdentity
                                           && m_canonicalWorkspace == state.canonicalWorkspace
                                           && m_workspaceTreeId == state.workspaceTreeId;
    const QString     previousCwd        = rebindsSameBinding ? m_path.cwd() : QString();
    const QStringList previousWritable = rebindsSameBinding ? m_path.writableDirs() : QStringList();
    const auto previousAnchors = rebindsSameBinding ? m_writableAnchors : QHash<QString, QString>();
    if (!rebindsSameBinding) {
        m_jobs.clear();
    }

    m_session            = state.session;
    m_sftp               = state.sftp;
    m_jumps              = state.jumps;
    m_target             = state.targetKey;
    m_endpointIdentity   = state.endpointIdentity;
    m_workspace          = state.workspace;
    m_canonicalWorkspace = state.canonicalWorkspace;
    m_workspaceTreeId    = state.workspaceTreeId;

    state.session = nullptr;
    state.sftp    = nullptr;
    state.jumps.clear();
    state.targetKey.clear();
    state.endpointIdentity.clear();
    state.workspace.clear();
    state.canonicalWorkspace.clear();
    state.workspaceTreeId.clear();

    /* A fresh context, so every believed file content goes with the transport
     * that was observed producing it. The read-before-overwrite guard keys off
     * this, and it is what forces a re-read before any edit after a
     * reconnect. */
    m_path.reset();
    m_path.setRoot(m_workspace);
    m_lastReconnectKeptCwd = false;
    if (rebindsSameBinding) {
        m_path.setWritableDirs(previousWritable);
        m_writableAnchors = previousAnchors;
        /* The working directory is a path, not a handle, so it survives when
         * it still exists. Verifying beats assuming: the host may have
         * rebooted out from under it. */
        if (!previousCwd.isEmpty() && confirmDirectory(previousCwd, deadline)) {
            m_path.setCwd(previousCwd);
            m_lastReconnectKeptCwd = true;
        } else {
            m_path.setCwd(m_workspace);
        }
    } else {
        m_path.setCwd(m_workspace);
        m_path.setWritableDirs({m_workspace});
        m_writableAnchors.clear();
        m_writableAnchors.insert(m_path.root(), m_canonicalWorkspace);
    }
    ++m_generation;
    m_transportLink = QUuid::createUuid().toString(QUuid::WithoutBraces);
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
    m_path.reset();
    m_target.clear();
    m_endpointIdentity.clear();
    m_workspace.clear();
    m_canonicalWorkspace.clear();
    m_workspaceTreeId.clear();
    m_transportLink.clear();
    m_writableAnchors.clear();
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

void QSocRemoteConnection::setWorkingDirectoryObserver(std::function<void(const QString &)> observer)
{
    m_path.setCwdObserver(std::move(observer));
}

void QSocRemoteConnection::setAbortProbe(std::function<bool()> probe)
{
    m_abortProbe = std::move(probe);
}

void QSocRemoteConnection::resetReconnectBudget()
{
    m_reconnectsUsed = 0;
}

bool QSocRemoteConnection::canonicalWritableDirs(QStringList *dirs, QString *errorMessage) const
{
    if (dirs != nullptr) {
        dirs->clear();
    }
    if (m_sftp == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("remote SFTP client is not connected");
        }
        return false;
    }
    if (!verifyWorkspaceBinding(nullptr, errorMessage)) {
        return false;
    }
    for (const QString &lexical : m_path.writableDirs()) {
        const QString anchor = m_writableAnchors.value(lexical);
        if (anchor.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("remote writable directory has no bound identity: %1")
                                    .arg(lexical);
            }
            return false;
        }
        QString resolved;
        QString err;
        if (m_sftp->canonicalize(lexical, &resolved, &err) != QSocSftpClient::Canonical::Ok) {
            if (errorMessage != nullptr) {
                *errorMessage = err;
            }
            return false;
        }
        if (resolved != anchor) {
            if (errorMessage != nullptr) {
                *errorMessage
                    = QStringLiteral("remote writable directory changed identity: %1").arg(lexical);
            }
            return false;
        }
        if (dirs != nullptr) {
            dirs->append(anchor);
        }
    }
    if (dirs == nullptr || !dirs->isEmpty()) {
        return true;
    }
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("no verified remote writable directory is bound");
    }
    return false;
}

bool QSocRemoteConnection::verifyWorkspaceBinding(QString *canonicalRoot, QString *errorMessage) const
{
    const auto refuse = [canonicalRoot, errorMessage](const QString &reason) {
        if (canonicalRoot != nullptr) {
            canonicalRoot->clear();
        }
        if (errorMessage != nullptr) {
            *errorMessage = reason;
        }
        return false;
    };
    if (m_sftp == nullptr || m_path.root().isEmpty() || m_canonicalWorkspace.isEmpty()
        || m_workspaceTreeId.isEmpty()) {
        return refuse(QStringLiteral("no verified remote workspace is bound"));
    }
    QString currentRoot;
    QString err;
    if (m_sftp->canonicalize(m_path.root(), &currentRoot, &err) != QSocSftpClient::Canonical::Ok) {
        return refuse(
            err.isEmpty() ? QStringLiteral("the remote workspace cannot be resolved") : err);
    }
    if (currentRoot != m_canonicalWorkspace) {
        return refuse(
            QStringLiteral("the remote workspace changed identity: %1").arg(m_path.root()));
    }
    QString metadataDir;
    if (!resolveWorkspaceMetadataDir(m_sftp, m_canonicalWorkspace, false, &metadataDir, &err)) {
        return refuse(err);
    }
    QString currentTreeId;
    if (!readWorkspaceTreeId(m_sftp, workspaceTreeMarker(metadataDir), &currentTreeId, &err)) {
        return refuse(err);
    }
    if (currentTreeId != m_workspaceTreeId) {
        return refuse(
            QStringLiteral("the remote workspace changed identity: %1").arg(m_path.root()));
    }
    if (canonicalRoot != nullptr) {
        *canonicalRoot = currentRoot;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool QSocRemoteConnection::resolveBoundDirectory(
    const QString &dir, QString *canonicalDir, QString *errorMessage) const
{
    const auto refuse = [canonicalDir, errorMessage](const QString &reason) {
        if (canonicalDir != nullptr) {
            canonicalDir->clear();
        }
        if (errorMessage != nullptr) {
            *errorMessage = reason;
        }
        return false;
    };
    QString canonicalRoot;
    QString err;
    if (!verifyWorkspaceBinding(&canonicalRoot, &err)) {
        return refuse(err);
    }
    if (!remoteDirectoryExists(m_sftp, dir, kReconnectProbeMs, &err)) {
        return refuse(err.isEmpty() ? QStringLiteral("the remote directory does not exist") : err);
    }
    QString resolved;
    if (m_sftp->canonicalize(dir, &resolved, &err) != QSocSftpClient::Canonical::Ok) {
        return refuse(
            err.isEmpty() ? QStringLiteral("the remote directory cannot be resolved") : err);
    }
    if (!QSocRemotePathContext::isWithinAny(resolved, {canonicalRoot})) {
        return refuse(
            QStringLiteral("the host resolves %1 to %2, outside the workspace").arg(dir, resolved));
    }
    if (canonicalDir != nullptr) {
        *canonicalDir = resolved;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool QSocRemoteConnection::resolveBoundCwd(QString *canonicalCwd, QString *errorMessage) const
{
    return resolveBoundDirectory(m_path.cwd(), canonicalCwd, errorMessage);
}

bool QSocRemoteConnection::resolveWritablePath(
    const QString &requested, QString *canonicalPath, QString *errorMessage) const
{
    const auto refuse = [canonicalPath, errorMessage](const QString &reason) {
        if (canonicalPath != nullptr) {
            canonicalPath->clear();
        }
        if (errorMessage != nullptr) {
            *errorMessage = reason;
        }
        return false;
    };
    if (m_sftp == nullptr) {
        return refuse(QStringLiteral("remote SFTP client is not connected"));
    }

    const QString lexical = m_path.normalize(requested);
    QString       resolved;
    QString       err;
    if (m_sftp->canonicalize(lexical, &resolved, &err) != QSocSftpClient::Canonical::Ok) {
        return refuse(
            err.isEmpty() ? QStringLiteral("the host cannot resolve %1").arg(lexical) : err);
    }

    QStringList writable;
    if (!canonicalWritableDirs(&writable, &err)) {
        return refuse(err);
    }
    if (!QSocRemotePathContext::isWithinAny(resolved, writable)) {
        return refuse(
            QStringLiteral("remote path is outside writable directories: %1").arg(resolved));
    }
    if (canonicalPath != nullptr) {
        *canonicalPath = resolved;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool QSocRemoteConnection::resolveWritableEntry(
    const QString &requested, QString *entryPath, QString *errorMessage) const
{
    const auto refuse = [entryPath, errorMessage](const QString &reason) {
        if (entryPath != nullptr) {
            entryPath->clear();
        }
        if (errorMessage != nullptr) {
            *errorMessage = reason;
        }
        return false;
    };
    if (m_sftp == nullptr) {
        return refuse(QStringLiteral("remote SFTP client is not connected"));
    }
    const QString   lexical = m_path.normalize(requested);
    const qsizetype slash   = lexical.lastIndexOf(QLatin1Char('/'));
    if (slash < 0 || lexical.mid(slash + 1).isEmpty()) {
        return refuse(QStringLiteral("remote file path has no leaf name"));
    }
    const QString parent = slash == 0 ? QStringLiteral("/") : lexical.left(slash);
    const QString leaf   = lexical.mid(slash + 1);
    QString       canonicalParent;
    QString       err;
    if (m_sftp->canonicalize(parent, &canonicalParent, &err) != QSocSftpClient::Canonical::Ok) {
        return refuse(
            err.isEmpty() ? QStringLiteral("the host cannot resolve %1").arg(parent) : err);
    }
    QStringList writable;
    if (!canonicalWritableDirs(&writable, &err)) {
        return refuse(err);
    }
    const QString entry = QDir(canonicalParent).filePath(leaf);
    if (!QSocRemotePathContext::isWithinAny(entry, writable)) {
        return refuse(QStringLiteral("remote path is outside writable directories: %1").arg(entry));
    }
    if (entryPath != nullptr) {
        *entryPath = entry;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

QSocRemoteConnection::CwdChange QSocRemoteConnection::setWorkingDirectory(
    const QString &requested, QString *errorMessage)
{
    const auto refuse = [errorMessage](CwdChange outcome, const QString &why) {
        if (errorMessage != nullptr) {
            *errorMessage = why;
        }
        return outcome;
    };
    if (m_sftp == nullptr) {
        return refuse(CwdChange::Refused, QStringLiteral("no remote workspace is bound"));
    }

    const QString lexical = m_path.resolveCwdRequest(requested);
    QString       err;
    QString       canonicalDir;
    switch (probeRemotePath(m_sftp, lexical, kReconnectProbeMs, &err)) {
    case RemoteProbeResult::Present:
        break;
    case RemoteProbeResult::Absent:
        return refuse(
            CwdChange::Unresolvable,
            QStringLiteral("the remote directory does not exist: %1").arg(lexical));
    case RemoteProbeResult::Silent:
        return refuse(CwdChange::Unknown, err);
    }
    switch (m_sftp->canonicalize(lexical, &canonicalDir, &err)) {
    case QSocSftpClient::Canonical::Ok:
        break;
    case QSocSftpClient::Canonical::Unresolvable:
        return refuse(
            CwdChange::Unresolvable,
            QStringLiteral("the host holds %1 but cannot resolve it").arg(lexical));
    case QSocSftpClient::Canonical::Unknown:
        return refuse(CwdChange::Unknown, err);
    }

    QString canonicalRoot;
    if (!verifyWorkspaceBinding(&canonicalRoot, &err)) {
        return refuse(CwdChange::Unknown, err);
    }

    if (!QSocRemotePathContext::isWithinAny(canonicalDir, {canonicalRoot})) {
        return refuse(
            CwdChange::Outside,
            QStringLiteral("the host resolves %1 to %2, outside the workspace")
                .arg(lexical, canonicalDir));
    }

    m_path.setCwd(lexical);
    return CwdChange::Changed;
}

QSocRemoteConnection::ReconnectOutcome QSocRemoteConnection::reconnect(
    QString *errorMessage, int *budget)
{
    m_lastAttempts = 0;
    if (m_session != nullptr && m_session->isConnected()) {
        return ReconnectOutcome::NotNeeded;
    }
    if (!m_rebuilder || m_target.isEmpty() || m_workspace.isEmpty()) {
        return ReconnectOutcome::Refused;
    }
    /* The budget belongs to the caller when supplied: a binding shared across
     * sibling sub-agents cannot share one counter without one child denying or
     * re-crediting another. The connection owns the counter only for the main
     * link, which has no siblings. Asked before the first attempt, not after
     * it: the budget exists to stop the second and later tool calls of one turn
     * from each paying a full connect sequence on the event loop. */
    int &used = (budget != nullptr) ? *budget : m_reconnectsUsed;
    if (used >= kReconnectBudgetPerTurn) {
        return ReconnectOutcome::BudgetSpent;
    }
    ++used;

    const QString target    = m_target;
    const QString workspace = m_workspace;

    for (int attempt = 1; attempt <= kReconnectAttempts; ++attempt) {
        if (m_abortProbe && m_abortProbe()) {
            return ReconnectOutcome::Aborted;
        }
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
    const auto resolve = [conn](const QString &path) {
        QString entry;
        QString err;
        if (conn == nullptr || !conn->resolveWritableEntry(path, &entry, &err)) {
            return QString();
        }
        return entry;
    };
    accessor.exists = [conn, mapPresence, resolve](const QString &path) {
        QSocSftpClient *sftp  = (conn != nullptr) ? conn->sftp() : nullptr;
        const QString   entry = resolve(path);
        if (sftp == nullptr || entry.isEmpty()) {
            return QSocFileHistory::FileState::Unknown;
        }
        return mapPresence(sftp->linkPresence(entry));
    };
    accessor.read = [conn, mapPresence, resolve](const QString &path) {
        QSocSftpClient *sftp  = (conn != nullptr) ? conn->sftp() : nullptr;
        const QString   entry = resolve(path);
        if (sftp == nullptr || entry.isEmpty()) {
            return QSocFileHistory::LiveRead::unknown();
        }
        QString resolved;
        QString err;
        switch (sftp->realPath(entry, &resolved, &err)) {
        case QSocSftpClient::Presence::Present:
            if (resolved != entry) {
                return QSocFileHistory::LiveRead::unknown();
            }
            break;
        case QSocSftpClient::Presence::Absent:
            return mapPresence(sftp->linkPresence(entry)) == QSocFileHistory::FileState::Absent
                       ? QSocFileHistory::LiveRead::absent()
                       : QSocFileHistory::LiveRead::unknown();
        case QSocSftpClient::Presence::Unknown:
            return QSocFileHistory::LiveRead::unknown();
        }
        const QByteArray bytes = sftp->readFile(entry, 0, &err);
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
        switch (mapPresence(sftp->linkPresence(entry))) {
        case QSocFileHistory::FileState::Absent:
            return QSocFileHistory::LiveRead::absent();
        case QSocFileHistory::FileState::Present:
        case QSocFileHistory::FileState::Unknown:
            break;
        }
        return QSocFileHistory::LiveRead::unknown();
    };
    accessor.write = [conn, resolve](const QString &path, const QString &content) {
        QSocSftpClient *sftp  = (conn != nullptr) ? conn->sftp() : nullptr;
        const QString   entry = resolve(path);
        if (sftp == nullptr || entry.isEmpty()) {
            return false;
        }
        QString err;
        return sftp->writeFile(entry, content.toUtf8(), &err);
    };
    accessor.remove = [conn, resolve](const QString &path) {
        QSocSftpClient *sftp  = (conn != nullptr) ? conn->sftp() : nullptr;
        const QString   entry = resolve(path);
        if (sftp == nullptr || entry.isEmpty()) {
            return false;
        }
        QString err;
        return sftp->removeFile(entry, &err);
    };
    accessor.coversPath = [resolve](const QString &path) { return !resolve(path).isEmpty(); };
    accessor.tree       = [conn]() {
        if (conn == nullptr || conn->target().isEmpty() || conn->workspace().isEmpty()) {
            return QString();
        }
        if (conn->canonicalWorkspace().isEmpty() || conn->workspaceTreeId().isEmpty()) {
            return QString();
        }
        if (conn->endpointIdentity().isEmpty()) {
            return QString();
        }
        return QStringLiteral("ssh:") + conn->endpointIdentity() + QChar(0x1F)
               + conn->canonicalWorkspace() + QChar(0x1F) + conn->workspaceTreeId();
    };
    accessor.generation = [conn]() { return (conn != nullptr) ? conn->transportLink() : QString(); };
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
