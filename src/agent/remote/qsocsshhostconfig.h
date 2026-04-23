// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSSHHOSTCONFIG_H
#define QSOCSSHHOSTCONFIG_H

#include <QString>
#include <QStringList>

/**
 * @brief Resolved SSH host settings derived from a limited .ssh/config subset.
 * @details Holds a path-only view of an identity file; QSoC code never reads
 *          the private key contents. libssh2 or ssh-agent may use the key
 *          internally for authentication.
 */
struct QSocSshHostConfig
{
    /** @brief Strict host key checking mode. */
    enum class StrictHostKey { Yes, AcceptNew, No };

    /** Original alias or raw target as passed in. */
    QString alias;

    /** Resolved hostname after HostName/%h expansion. */
    QString hostname;

    /** Resolved TCP port. Default 22. */
    int port = 22;

    /** Resolved user. Empty means "use local username". */
    QString user;

    /**
     * Identity file paths (never contents). Accumulates when multiple
     * matching blocks contribute entries, unless IdentitiesOnly prunes.
     */
    QStringList identityFiles;

    /** If true, use only the identity files listed here. */
    bool identitiesOnly = false;

    /** Known-hosts file path (empty means default ~/.ssh/known_hosts). */
    QString userKnownHostsFile;

    /** Strict host key checking policy. */
    StrictHostKey strictHostKey = StrictHostKey::Yes;

    /** Parsed value of the AddKeysToAgent directive. */
    bool addKeysToAgent = false;

    /**
     * Ordered list of ProxyJump hop aliases. Empty means a direct connect.
     * Each entry is a raw alias; the caller resolves it through the same
     * QSocSshConfigParser to get hostname/port/user for the hop.
     */
    QStringList proxyJump;

    /** True when this host resolution came from an explicit config match. */
    bool fromConfig = false;
};

#endif // QSOCSSHHOSTCONFIG_H
