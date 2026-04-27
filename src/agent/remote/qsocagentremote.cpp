// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"

#include "agent/qsoctool.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "agent/remote/qsocsshhostconfig.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/tool/qsoctooldoc.h"
#include "agent/tool/qsoctoolweb.h"
#include "common/qsocconfig.h"

#include <QDir>
#include <QFileInfo>

#include <functional>

namespace {

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
    const QString &target, QObject *parent, AgentRemoteState *state, QString *errorMessage)
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
        auto                         *session = new QSocSshSession(parent);
        QSocSshSession::ConnectStatus status
            = (currentParent != nullptr) ? session->connectToVia(cfg, currentParent, errOut)
                                         : session->connectTo(cfg, errOut);
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

    state->path.setRoot(workspace);
    state->path.setCwd(workspace);
    state->path.setWritableDirs({workspace});

    state->workspace = workspace;
    state->display   = state->targetKey + QStringLiteral(":") + workspace;
    return true;
}

QSocToolRegistry *buildAgentRemoteRegistry(
    QObject *parent, AgentRemoteState *state, QSocConfig *socConfig)
{
    auto *registry = new QSocToolRegistry(parent);
    registry->registerTool(new QSocToolRemoteFileRead(parent, state->sftp, &state->path));
    registry->registerTool(new QSocToolRemoteFileList(parent, state->sftp, &state->path));
    registry->registerTool(new QSocToolRemoteFileWrite(parent, state->sftp, &state->path));
    registry->registerTool(new QSocToolRemoteFileEdit(parent, state->sftp, &state->path));
    registry->registerTool(new QSocToolRemoteShellBash(parent, state->session, &state->path));
    registry->registerTool(new QSocToolRemoteBashManage(parent, state->session, &state->path));
    registry->registerTool(new QSocToolRemotePath(parent, &state->path));
    /* Control-plane tools stay local even in remote mode. */
    registry->registerTool(new QSocToolDocQuery(parent));
    registry->registerTool(new QSocToolWebFetch(parent, socConfig));
    if (socConfig != nullptr && !socConfig->getValue("web.search_api_url").isEmpty()) {
        registry->registerTool(new QSocToolWebSearch(parent, socConfig));
    }
    state->registry = registry;
    return registry;
}
