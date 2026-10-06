// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCDISPATCHPOLICY_H
#define QSOCDISPATCHPOLICY_H

#include "common/qllmservice.h"

#include <functional>
#include <optional>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

class QSocConfig;
class QSocHostCatalog;
class QSocSshConfigParser;

namespace YAML {
class Node;
}

/** @brief One host of `agent.dispatch.hosts`. */
struct QSocDispatchHost
{
    QString alias;
    QString workspace;  /* a workspace the call names must stay under it */
    QString model;      /* every child on the host runs on this llm.models key */
    QString capability; /* catalog text, shown in the prompt */
};

/** @brief What one `agent` call asks for. Empty means omitted. */
struct QSocChildRequest
{
    QString host;            /* `local` is this machine */
    QString workspace;       /* only for a named remote host */
    QString model;           /* llm.models key */
    QString definitionModel; /* the definition's `model:` */
    bool    fork = false;
};

/** @brief The main agent at the moment of the call. */
struct QSocMainState
{
    QString modelId;
    QString effort;
    QString alias; /* the remote binding, empty when local */
    bool    remote = false;
};

/** @brief Where and on which model a child runs. */
struct QSocChildPlan
{
    QString                       host; /* empty: the main agent's binding; `local`: this machine */
    QString                       workspace;     /* empty: the host's catalog workspace */
    QString                       workspaceRoot; /* a named workspace must resolve at or below it */
    bool                          workspaceNamed = false;
    std::optional<LLMModelConfig> model; /* set when the child leaves the main model */
    QString                       effort;
};

/** @brief Look up an llm.models key in the live registry. */
using QSocModelLookup = std::function<std::optional<LLMModelConfig>(const QString &key)>;

/**
 * @brief The hosts and models the user lets sub-agents use.
 * @details A snapshot of `agent.dispatch`, read once and replaced only when
 *          the user reloads it. A malformed entry is kept as a refusal, so a
 *          typo never widens what a child may use.
 */
class QSocDispatchPolicy
{
public:
    /** @brief Read the policy from the merged config. */
    static QSocDispatchPolicy load(
        const QSocConfig          *config,
        const QSocHostCatalog     *catalog,
        const QSocSshConfigParser *sshConfig);

    /** @brief Read the policy from the `agent.dispatch` and `llm.models` nodes. */
    static QSocDispatchPolicy fromNodes(
        const YAML::Node          &dispatch,
        const YAML::Node          &models,
        const QSocHostCatalog     *catalog,
        const QSocSshConfigParser *sshConfig);

    /** @brief True when `agent.dispatch.hosts` decides which hosts are usable. */
    bool hostsDeclared() const { return hostsDeclared_; }

    /** @brief `local` plus the granted aliases; empty when hosts are not declared. */
    QStringList hostChoices() const;

    /** @brief The usable `agent.dispatch.models` keys. */
    QStringList modelChoices() const;

    /** @brief One line per entry that was refused at load. */
    QStringList warnings() const { return warnings_; }

    /** @brief The `# Dispatch resources` prompt section; empty when nothing is declared. */
    QString promptSection() const;

    /** @brief A plain listing for the `/dispatch` command. */
    QString describe() const;

    /**
     * @brief Decide where and on which model a child runs.
     * @details @p catalog answers for named hosts when `hosts` is not
     *          declared, and @p lookup resolves a definition's model.
     *          Returns nothing and sets @p error when the call is refused.
     */
    std::optional<QSocChildPlan> resolveChild(
        const QSocChildRequest &request,
        const QSocMainState    &mainAgent,
        const QSocHostCatalog  *catalog,
        const QSocModelLookup  &lookup,
        QString                *error) const;

private:
    std::optional<QSocChildPlan> resolveModel(
        QSocChildPlan           plan,
        const QSocChildRequest &request,
        const QSocMainState    &mainAgent,
        const QString          &boundModel,
        const QSocModelLookup  &lookup,
        QString                *error) const;

    bool                            hostsDeclared_ = false;
    QMap<QString, QSocDispatchHost> hosts_;
    QMap<QString, QString>          refusedHosts_;
    QList<LLMModelConfig>           models_;
    QMap<QString, LLMModelConfig>   boundModels_;
    QMap<QString, QString>          refusedModels_;
    QStringList                     warnings_;
};

#endif /* QSOCDISPATCHPOLICY_H */
