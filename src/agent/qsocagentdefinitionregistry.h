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

private:
    QMap<QString, QSocAgentDefinition> defs_;
};

#endif /* QSOCAGENTDEFINITIONREGISTRY_H */
