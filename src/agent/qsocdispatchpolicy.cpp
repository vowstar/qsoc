// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocdispatchpolicy.h"

#include "agent/remote/qsochostprofile.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "common/qsocconfig.h"

#include <yaml-cpp/yaml.h>

#include <QDir>

namespace {

const QString kLocal = QStringLiteral("local");

QString scalarOf(const YAML::Node &node)
{
    return node.IsScalar() ? QString::fromStdString(node.as<std::string>()).trimmed() : QString();
}

std::optional<LLMModelConfig> modelEntry(const YAML::Node &models, const QString &key)
{
    if (key.isEmpty() || !models.IsDefined() || !models.IsMap()) {
        return std::nullopt;
    }
    const YAML::Node entry = models[key.toStdString()];
    if (!entry.IsDefined() || !entry.IsMap()) {
        return std::nullopt;
    }
    return QLLMService::parseModelEntry(key, entry);
}

bool atOrBelow(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root + QLatin1Char('/'));
}

bool reachable(
    const QString &alias, const QSocHostCatalog *catalog, const QSocSshConfigParser *sshConfig)
{
    if (alias == kLocal || (sshConfig != nullptr && sshConfig->resolve(alias).fromConfig)) {
        return true;
    }
    const QSocHostProfile *entry = catalog != nullptr ? catalog->find(alias) : nullptr;
    return entry != nullptr && !entry->target.isEmpty();
}

/* Why one `hosts` entry cannot be granted; empty when it can. */
QString hostProblem(
    const QString &alias, const YAML::Node &node, const YAML::Node &models, QSocDispatchHost *out)
{
    if (!node.IsNull() && !node.IsMap()) {
        return QStringLiteral("is not a mapping");
    }
    if (node.IsMap()) {
        for (const auto &field : node) {
            const QString name = QString::fromStdString(field.first.as<std::string>());
            if (name != QStringLiteral("workspace") && name != QStringLiteral("model")) {
                return QStringLiteral("has unknown field '%1'").arg(name);
            }
        }
    }
    out->alias = alias;
    if (node.IsMap() && node["workspace"]) {
        out->workspace = QDir::cleanPath(scalarOf(node["workspace"]));
        if (alias == kLocal) {
            return QStringLiteral("cannot set a workspace on this machine");
        }
        if (!out->workspace.startsWith(QLatin1Char('/')) || out->workspace == QStringLiteral("/")) {
            return QStringLiteral("workspace must be an absolute directory below /");
        }
    }
    if (node.IsMap() && node["model"]) {
        out->model = scalarOf(node["model"]);
        if (!modelEntry(models, out->model)) {
            return QStringLiteral("model '%1' is not in llm.models").arg(out->model);
        }
    }
    return {};
}

} // namespace

QSocDispatchPolicy QSocDispatchPolicy::load(
    const QSocConfig *config, const QSocHostCatalog *catalog, const QSocSshConfigParser *sshConfig)
{
    if (config == nullptr) {
        return {};
    }
    return fromNodes(
        config->getYamlNode(QStringLiteral("agent.dispatch")),
        config->getYamlNode(QStringLiteral("llm.models")),
        catalog,
        sshConfig);
}

QSocDispatchPolicy QSocDispatchPolicy::fromNodes(
    const YAML::Node          &dispatch,
    const YAML::Node          &models,
    const QSocHostCatalog     *catalog,
    const QSocSshConfigParser *sshConfig)
{
    QSocDispatchPolicy policy;
    if (!dispatch.IsDefined() || dispatch.IsNull()) {
        return policy;
    }
    if (!dispatch.IsMap()) {
        policy.hostsDeclared_ = true;
        policy.warnings_ << QStringLiteral("agent.dispatch is not a mapping; no host is granted");
        return policy;
    }
    for (const auto &item : dispatch) {
        const QString key = QString::fromStdString(item.first.as<std::string>());
        if (key != QStringLiteral("hosts") && key != QStringLiteral("models")) {
            policy.hostsDeclared_ = true;
            policy.warnings_ << QStringLiteral(
                                    "agent.dispatch.%1 is not a known key; no host is "
                                    "granted")
                                    .arg(key);
        }
    }
    const YAML::Node hosts = dispatch["hosts"];
    if (hosts.IsDefined()) {
        policy.hostsDeclared_ = true;
    }
    if (hosts.IsDefined() && hosts.IsMap()) {
        for (const auto &item : hosts) {
            const QString    alias = QString::fromStdString(item.first.as<std::string>()).trimmed();
            QSocDispatchHost grant;
            QString          problem = hostProblem(alias, item.second, models, &grant);
            if (problem.isEmpty() && !reachable(alias, catalog, sshConfig)) {
                problem = QStringLiteral(
                    "is neither in ~/.ssh/config nor a host catalog entry with a target");
            }
            if (!problem.isEmpty()) {
                const QString text
                    = QStringLiteral("agent.dispatch.hosts.%1 %2").arg(alias, problem);
                policy.refusedHosts_.insert(alias, text);
                policy.warnings_ << text;
                continue;
            }
            const QSocHostProfile *entry = catalog != nullptr ? catalog->find(alias) : nullptr;
            grant.capability = entry != nullptr ? entry->capability.trimmed() : QString();
            policy.hosts_.insert(alias, grant);
            if (!grant.model.isEmpty()) {
                policy.boundModels_.insert(grant.model, *modelEntry(models, grant.model));
            }
        }
    } else if (hosts.IsDefined() && !hosts.IsNull()) {
        policy.warnings_ << QStringLiteral(
            "agent.dispatch.hosts is not a mapping; no host is granted");
    }
    const YAML::Node granted = dispatch["models"];
    if (granted.IsDefined() && granted.IsSequence()) {
        for (const auto &item : granted) {
            const QString key   = scalarOf(item);
            const auto    entry = modelEntry(models, key);
            if (entry) {
                policy.models_.append(*entry);
                continue;
            }
            const QString text
                = QStringLiteral("agent.dispatch.models: '%1' is not in llm.models").arg(key);
            policy.refusedModels_.insert(key, text);
            policy.warnings_ << text;
        }
    } else if (granted.IsDefined() && !granted.IsNull()) {
        policy.warnings_ << QStringLiteral(
            "agent.dispatch.models is not a list; no other model is granted");
    }
    return policy;
}

QStringList QSocDispatchPolicy::hostChoices() const
{
    if (!hostsDeclared_) {
        return {};
    }
    QStringList choices{kLocal};
    for (const QString &alias : hosts_.keys()) {
        if (alias != kLocal) {
            choices << alias;
        }
    }
    return choices;
}

QStringList QSocDispatchPolicy::modelChoices() const
{
    QStringList keys;
    for (const auto &entry : models_) {
        keys << entry.id;
    }
    return keys;
}

QString QSocDispatchPolicy::promptSection() const
{
    if (!hostsDeclared_ && models_.isEmpty()) {
        return {};
    }
    QString text = QStringLiteral(
        "\n# Dispatch resources\n\n"
        "The user grants these to children of the `agent` tool. Omit `host` to run a child "
        "where you work now, and omit `model` to run it on your own model.\n");
    if (hostsDeclared_) {
        text += QStringLiteral("\n## Hosts\n\n");
        for (const QString &alias : hostChoices()) {
            const QSocDispatchHost grant = hosts_.value(alias);
            QString line = alias == kLocal ? QStringLiteral("this machine")
                                           : (grant.capability.isEmpty()
                                                  ? QStringLiteral("(no capability text)")
                                                  : grant.capability);
            if (!grant.model.isEmpty()) {
                line += QStringLiteral("; every child here runs on model %1").arg(grant.model);
            }
            text += QStringLiteral("- %1: %2\n").arg(alias, line);
        }
    }
    if (!models_.isEmpty()) {
        text += QStringLiteral("\n## Models\n\n");
        for (const auto &entry : models_) {
            text += QStringLiteral("- %1: %2, context %3 tokens\n")
                        .arg(entry.id, entry.name)
                        .arg(entry.contextTokens);
        }
    }
    return text;
}

QString QSocDispatchPolicy::describe() const
{
    QString text = QStringLiteral("Dispatch hosts:");
    if (!hostsDeclared_) {
        text += QStringLiteral(" not declared; host catalog entries are dispatchable\n");
    } else {
        text += QLatin1Char('\n');
        for (const QString &alias : hostChoices()) {
            const QSocDispatchHost grant = hosts_.value(alias);
            text += QStringLiteral("  %1").arg(alias);
            if (!grant.workspace.isEmpty()) {
                text += QStringLiteral("  workspace %1").arg(grant.workspace);
            }
            if (!grant.model.isEmpty()) {
                text += QStringLiteral("  model %1").arg(grant.model);
            }
            text += QLatin1Char('\n');
        }
    }
    const QStringList keys = modelChoices();
    text += keys.isEmpty() ? QStringLiteral("Dispatch models: none, children use the main model\n")
                           : QStringLiteral("Dispatch models: %1\n").arg(keys.join(", "));
    for (const QString &warning : warnings_) {
        text += QStringLiteral("Refused: %1\n").arg(warning);
    }
    return text;
}

std::optional<QSocChildPlan> QSocDispatchPolicy::resolveChild(
    const QSocChildRequest &request,
    const QSocMainState    &mainAgent,
    const QSocHostCatalog  *catalog,
    const QSocModelLookup  &lookup,
    QString                *error) const
{
    const auto refuse = [error](const QString &why) -> std::optional<QSocChildPlan> {
        *error = why;
        return std::nullopt;
    };
    const QString host  = request.host.trimmed();
    const bool    named = !host.isEmpty() && host != kLocal;
    const QString key   = !host.isEmpty() ? host : (mainAgent.remote ? mainAgent.alias : kLocal);
    if (refusedHosts_.contains(key)) {
        return refuse(refusedHosts_.value(key));
    }
    if (named && hostsDeclared_ && !hosts_.contains(host)) {
        return refuse(QStringLiteral("host '%1' is not in agent.dispatch.hosts; use one of: %2")
                          .arg(host, hostChoices().join(QStringLiteral(", "))));
    }
    if (named && !hostsDeclared_ && (catalog == nullptr || catalog->find(host) == nullptr)) {
        QStringList valid{kLocal};
        for (const auto &entry :
             catalog != nullptr ? catalog->allList() : QList<QSocHostProfile>()) {
            valid << entry.alias;
        }
        return refuse(QStringLiteral("unknown host '%1'; valid hosts: %2")
                          .arg(host, valid.join(QStringLiteral(", "))));
    }
    const QSocDispatchHost grant = hosts_.value(key);
    QSocChildPlan          plan;
    plan.host = host;
    if (named) {
        plan.workspace     = grant.workspace;
        plan.workspaceRoot = grant.workspace;
    }
    if (!request.workspace.isEmpty()) {
        const QString workspace = QDir::cleanPath(request.workspace);
        if (!named) {
            return refuse(QStringLiteral("workspace applies only to a named remote host"));
        }
        if (!workspace.startsWith(QLatin1Char('/')) || workspace == QStringLiteral("/")) {
            return refuse(QStringLiteral("workspace %1 must be an absolute directory below /")
                              .arg(request.workspace));
        }
        if (!grant.workspace.isEmpty() && !atOrBelow(workspace, grant.workspace)) {
            return refuse(QStringLiteral("workspace %1 is outside %2, the workspace granted on %3")
                              .arg(workspace, grant.workspace, host));
        }
        plan.workspace      = workspace;
        plan.workspaceNamed = true;
    }
    return resolveModel(plan, request, mainAgent, grant.model, lookup, error);
}

std::optional<QSocChildPlan> QSocDispatchPolicy::resolveModel(
    QSocChildPlan           plan,
    const QSocChildRequest &request,
    const QSocMainState    &mainAgent,
    const QString          &boundModel,
    const QSocModelLookup  &lookup,
    QString                *error) const
{
    const auto refuse = [error](const QString &why) -> std::optional<QSocChildPlan> {
        *error = why;
        return std::nullopt;
    };
    std::optional<LLMModelConfig> entry;
    if (!boundModel.isEmpty()) {
        if (!request.model.isEmpty() && request.model != boundModel) {
            return refuse(QStringLiteral("every child on this host runs on model %1; omit model")
                              .arg(boundModel));
        }
        entry = boundModels_.value(boundModel);
    } else if (!request.model.isEmpty() && request.model != mainAgent.modelId) {
        if (refusedModels_.contains(request.model)) {
            return refuse(refusedModels_.value(request.model));
        }
        for (const auto &granted : models_) {
            if (granted.id == request.model) {
                entry = granted;
            }
        }
        if (!entry) {
            const QStringList keys = modelChoices();
            return refuse(QStringLiteral("model '%1' is not in agent.dispatch.models; %2")
                              .arg(
                                  request.model,
                                  keys.isEmpty()
                                      ? QStringLiteral("omit model")
                                      : QStringLiteral("use one of: %1").arg(keys.join(", "))));
        }
    } else if (request.model.isEmpty() && !request.definitionModel.isEmpty()) {
        entry = lookup ? lookup(request.definitionModel) : std::nullopt;
        if (!entry) {
            return refuse(QStringLiteral("definition model '%1' is not in llm.models")
                              .arg(request.definitionModel));
        }
    }
    if (entry && entry->id == mainAgent.modelId) {
        entry.reset();
    }
    if (request.fork && entry) {
        return refuse(QStringLiteral(
                          "fork mode keeps the main agent's model %1, but the child would run "
                          "on %2; use a named subagent_type")
                          .arg(mainAgent.modelId, entry->id));
    }
    plan.effort = entry && entry->effortSet ? entry->effort : mainAgent.effort;
    plan.model  = entry;
    return plan;
}
