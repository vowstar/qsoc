// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSSHCONFIGPARSER_H
#define QSOCSSHCONFIGPARSER_H

#include "agent/remote/qsocsshhostconfig.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

/**
 * @brief Deliberately limited parser for ~/.ssh/config.
 * @details Supports Host, HostName, User, Port, IdentityFile, IdentitiesOnly,
 *          UserKnownHostsFile, StrictHostKeyChecking, AddKeysToAgent, and
 *          Include. Explicitly does not support Match, ProxyCommand, port
 *          forwarding, certificates, or command-expanding tokens.
 *
 *          Security invariant: the parser treats IdentityFile as a path
 *          string only and never opens, reads, hashes, or otherwise inspects
 *          the private key file. Key contents are the exclusive domain of
 *          libssh2 or ssh-agent during authentication.
 */
class QSocSshConfigParser
{
public:
    /**
     * @brief Parse a config file and any safely-bounded Includes.
     * @param configPath Absolute path. Relative paths are resolved against
     *                   CWD by the caller.
     * @return True if the file parsed (even with ignored directives).
     */
    bool parse(const QString &configPath);

    /** @brief True when no blocks were parsed. */
    bool isEmpty() const { return m_blocks.isEmpty(); }

    /**
     * @brief Concrete host aliases suitable for a selection menu.
     * @details Excludes wildcard, negated, and Host * defaults. Order is
     *          stable: first appearance in file order wins.
     */
    QStringList listMenuHosts() const;

    /**
     * @brief Resolve effective settings for a host alias or raw host.
     * @details Matches alias against Host patterns in file order, applying
     *          first-value-wins for scalar options and accumulating
     *          IdentityFile entries. Unmatched hosts return a default
     *          QSocSshHostConfig with only alias/hostname populated.
     */
    QSocSshHostConfig resolve(const QString &alias) const;

    /**
     * @brief Informational notes about ignored or unsupported directives.
     * @details Never contains secrets, private-key contents, or raw paths
     *          for IdentityFile values. Safe to log.
     */
    QStringList notes() const { return m_notes; }

    /** @brief Maximum Include nesting depth (OpenSSH uses 16; QSoC limits to 4). */
    static constexpr int kMaxIncludeDepth = 4;

    /** @brief Maximum total included files (prevents pathological globs). */
    static constexpr int kMaxIncludedFiles = 32;

private:
    /** @brief One raw block = one or more Host patterns plus their options. */
    struct Block
    {
        QStringList                    patterns;
        QList<QPair<QString, QString>> options;
    };

    bool parseFile(
        const QString &absPath, int depth, const QString &parentDir, QSet<QString> &seenFiles);

    void handleInclude(
        const QString &value, int depth, const QString &parentDir, QSet<QString> &seenFiles);

    static bool    patternMatch(const QString &pattern, const QString &host);
    static bool    blockMatches(const QStringList &patterns, const QString &host);
    static QString expandTokens(
        const QString &value,
        const QString &alias,
        const QString &hostname,
        int            port,
        const QString &user);
    static QString     expandTildeOnly(const QString &path);
    static QStringList tokenizeLine(const QString &line);

    QList<Block> m_blocks;
    QStringList  m_notes;
};

#endif // QSOCSSHCONFIGPARSER_H
