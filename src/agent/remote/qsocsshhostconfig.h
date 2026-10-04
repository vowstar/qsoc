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
    /**
     * @brief StrictHostKeyChecking policy for a host key not yet known.
     * @details A changed key is refused under every policy.
     */
    enum class StrictHostKey {
        Yes,       /**< Refuse an unknown key. */
        Ask,       /**< Confirm an unknown key and save it; refuse with nobody to ask. */
        AcceptNew, /**< Accept an unknown key and save it. */
        No,        /**< Accept an unknown key without saving it, with a warning. */
    };

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

    /**
     * Known-hosts file list, whitespace separated (empty means
     * ~/.ssh/known_hosts). New keys are saved to the first one.
     */
    QString userKnownHostsFile;

    /** Strict host key checking policy; OpenSSH's default is ask. */
    StrictHostKey strictHostKey = StrictHostKey::Ask;

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
