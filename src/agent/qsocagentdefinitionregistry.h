// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTDEFINITIONREGISTRY_H
#define QSOCAGENTDEFINITIONREGISTRY_H

#include "agent/qsocagentdefinition.h"

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

/**
 * @brief Catalog of sub-agent definitions discoverable by the
 *        `agent` spawn tool.
 * @details MVP exposes only built-ins registered in C++.
 *          scanFromDisk(...) is reserved for a future path that
 *          loads markdown agent files from
 *          `~/.config/qsoc/agents/` (user scope) and
 *          `<projectPath>/.qsoc/agents/` (project scope) using the
 *          frontmatter style already used by skills.
 */
class QSocAgentDefinitionRegistry : public QObject
{
    Q_OBJECT

public:
    explicit QSocAgentDefinitionRegistry(QObject *parent = nullptr);

    /**
     * @brief Seed the registry with compiled-in sub-agent types.
     * @details MVP adds `general-purpose`. Safe to call multiple
     *          times: a re-registration replaces the prior entry.
     */
    void registerBuiltins();

    /**
     * @brief Add or replace a definition by name.
     */
    void registerDefinition(const QSocAgentDefinition &def);

    /**
     * @brief Look up a definition. Returns nullptr if absent.
     */
    const QSocAgentDefinition *find(const QString &name) const;

    /**
     * @brief Sorted list of registered names.
     */
    QStringList availableNames() const;

    /**
     * @brief Multi-line "name: description" enumeration suitable for
     *        embedding in the spawn tool's parameter description so
     *        the parent LLM can pick the right `subagent_type`.
     */
    QString describeAvailable() const;

    /**
     * @brief Number of registered definitions.
     */
    int count() const;

    /**
     * @brief Load markdown agent definitions from disk.
     * @details Scans `userDir` and `projectDir` for `*.md` files.
     *          Each file's YAML-style frontmatter populates a
     *          QSocAgentDefinition; the body after the closing
     *          `---` becomes promptBody. Project-scope entries
     *          override user-scope entries which override builtins
     *          (by name). Pass an empty path to skip a scope.
     *          Parse errors do not abort: the offending entry is
     *          inserted with `parseError` set so /agents can list it.
     *          Empty / unreadable directories produce a single info
     *          log line, not a warning.
     */
    void scanFromDisk(const QString &userDir, const QString &projectDir);

    /**
     * @brief Definitions whose load surfaced a parseError, with
     *        their source paths. Used by `/agents` to flag broken
     *        files without burying them in the main listing.
     */
    QList<QSocAgentDefinition> brokenDefinitions() const;

private:
    /**
     * @brief Parse a single markdown agent definition file.
     * @param path Absolute path to the *.md file.
     * @param scope "user" or "project".
     * @return Parsed definition; on failure `parseError` is non-empty
     *         and the rest of the fields may be partially populated.
     */
    QSocAgentDefinition parseAgentMarkdown(const QString &path, const QString &scope) const;

    /**
     * @brief Scan a single directory for *.md files and register them.
     * @details Empty / missing directories are silently skipped.
     */
    void scanDirectory(const QString &dirPath, const QString &scope);

    QMap<QString, QSocAgentDefinition> defs_;
    QList<QSocAgentDefinition>         broken_;
};

#endif /* QSOCAGENTDEFINITIONREGISTRY_H */
