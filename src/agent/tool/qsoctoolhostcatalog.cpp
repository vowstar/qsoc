// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolhostcatalog.h"

#include "agent/remote/qsochostprofile.h"

namespace {

QString jsonString(const json &args, const char *key)
{
    if (!args.contains(key) || !args[key].is_string()) {
        return {};
    }
    return QString::fromStdString(args[key].get<std::string>());
}

QString opKindLabel(QSocHostCatalogOp::Kind kind)
{
    switch (kind) {
    case QSocHostCatalogOp::Kind::CapabilityAppend:
        return QStringLiteral("capability_append");
    case QSocHostCatalogOp::Kind::CapabilityRemove:
        return QStringLiteral("capability_remove");
    case QSocHostCatalogOp::Kind::CapabilityReplace:
        return QStringLiteral("capability_replace");
    case QSocHostCatalogOp::Kind::SetWorkspace:
        return QStringLiteral("set_workspace");
    case QSocHostCatalogOp::Kind::SetTarget:
        return QStringLiteral("set_target");
    }
    return {};
}

bool parseOpKind(const QString &raw, QSocHostCatalogOp::Kind *out)
{
    if (raw == QStringLiteral("capability_append")) {
        *out = QSocHostCatalogOp::Kind::CapabilityAppend;
        return true;
    }
    if (raw == QStringLiteral("capability_remove")) {
        *out = QSocHostCatalogOp::Kind::CapabilityRemove;
        return true;
    }
    if (raw == QStringLiteral("capability_replace")) {
        *out = QSocHostCatalogOp::Kind::CapabilityReplace;
        return true;
    }
    if (raw == QStringLiteral("set_workspace")) {
        *out = QSocHostCatalogOp::Kind::SetWorkspace;
        return true;
    }
    if (raw == QStringLiteral("set_target")) {
        *out = QSocHostCatalogOp::Kind::SetTarget;
        return true;
    }
    return false;
}

} // namespace

/* host_register */

QSocToolHostRegister::QSocToolHostRegister(QObject *parent, QSocHostCatalog *catalog)
    : QSocTool(parent)
    , catalog_(catalog)
{}

QString QSocToolHostRegister::getName() const
{
    return QStringLiteral("host_register");
}

QString QSocToolHostRegister::getDescription() const
{
    return QStringLiteral(
        "Register a new named host in the project catalog. The alias should match a "
        "Host block in ~/.ssh/config when possible so transport settings come from "
        "there; otherwise pass `target` as a fallback `[user@]host[:port]`. "
        "Workspace is required (ssh-config has no workspace concept). Capability is "
        "free-form text the parent agent uses to dispatch sub-agent tasks to the "
        "right host. No SSH connection is opened by this call. Rejects duplicate "
        "alias.");
}

json QSocToolHostRegister::getParametersSchema() const
{
    return json{
        {"type", "object"},
        {"properties",
         {{"alias",
           {{"type", "string"},
            {"description", "Catalog key; matches the ssh-config Host alias when applicable."}}},
          {"workspace",
           {{"type", "string"}, {"description", "Absolute remote path for agent operations."}}},
          {"capability",
           {{"type", "string"},
            {"description",
             "Free-form description of what this host can do (e.g. 'Vivado "
             "synthesis, JTAG programming')."}}},
          {"target",
           {{"type", "string"},
            {"description",
             "Optional [user@]host[:port] fallback when the alias is NOT in "
             "~/.ssh/config."}}}}},
        {"required", json::array({"alias", "workspace"})}};
}

QString QSocToolHostRegister::execute(const json &arguments)
{
    if (catalog_ == nullptr) {
        return QStringLiteral("Error: host catalog is not configured");
    }
    QSocHostProfile entry;
    entry.alias      = jsonString(arguments, "alias");
    entry.workspace  = jsonString(arguments, "workspace");
    entry.capability = jsonString(arguments, "capability");
    entry.target     = jsonString(arguments, "target");
    QString err;
    if (!catalog_->upsert(entry, false, &err)) {
        return QStringLiteral("Error: %1").arg(err);
    }
    return QStringLiteral("Registered host '%1' (workspace: %2%3)")
        .arg(
            entry.alias,
            entry.workspace,
            entry.target.isEmpty() ? QString() : QStringLiteral(", target: %1").arg(entry.target));
}

/* host_update */

QSocToolHostUpdate::QSocToolHostUpdate(QObject *parent, QSocHostCatalog *catalog)
    : QSocTool(parent)
    , catalog_(catalog)
{}

QString QSocToolHostUpdate::getName() const
{
    return QStringLiteral("host_update");
}

QString QSocToolHostUpdate::getDescription() const
{
    return QStringLiteral(
        "Mutate an existing catalog entry by applying an ordered list of ops. "
        "Use capability_append / capability_remove / capability_replace to grow "
        "or shrink the capability text. Use set_workspace / set_target to "
        "amend supplemental fields. set_target only updates the catalog "
        "fallback; it never edits ~/.ssh/config. Ops apply atomically: if any "
        "op fails the catalog is unchanged. When the alias only exists in user "
        "scope, the entry is first materialised into project scope.");
}

json QSocToolHostUpdate::getParametersSchema() const
{
    return json{
        {"type", "object"},
        {"properties",
         {{"alias", {{"type", "string"}, {"description", "Catalog alias to mutate."}}},
          {"opList",
           {{"type", "array"},
            {"description",
             "Ordered operations. Each item has fields op (one of "
             "capability_append, capability_remove, capability_replace, "
             "set_workspace, set_target) and value."},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"op",
                 {{"type", "string"},
                  {"enum",
                   json::array(
                       {"capability_append",
                        "capability_remove",
                        "capability_replace",
                        "set_workspace",
                        "set_target"})}}},
                {"value",
                 {{"type", "string"}, {"description", "Argument for the op (free-form string)."}}}}},
              {"required", json::array({"op", "value"})}}}}}}},
        {"required", json::array({"alias", "opList"})}};
}

QString QSocToolHostUpdate::execute(const json &arguments)
{
    if (catalog_ == nullptr) {
        return QStringLiteral("Error: host catalog is not configured");
    }
    const QString alias = jsonString(arguments, "alias");
    if (alias.isEmpty()) {
        return QStringLiteral("Error: alias is required");
    }
    if (!arguments.contains("opList") || !arguments["opList"].is_array()) {
        return QStringLiteral("Error: opList must be an array");
    }
    QList<QSocHostCatalogOp> ops;
    for (const auto &item : arguments["opList"]) {
        if (!item.is_object() || !item.contains("op") || !item.contains("value")) {
            return QStringLiteral("Error: each op needs op and value fields");
        }
        const QString           rawKind = jsonString(item, "op");
        const QString           value   = jsonString(item, "value");
        QSocHostCatalogOp::Kind kind;
        if (!parseOpKind(rawKind, &kind)) {
            return QStringLiteral("Error: unknown op '%1'").arg(rawKind);
        }
        ops.append({.kind = kind, .value = value});
    }
    QString err;
    if (!catalog_->applyOps(alias, ops, &err)) {
        return QStringLiteral("Error: %1").arg(err);
    }
    QStringList labels;
    labels.reserve(ops.size());
    for (const auto &operation : ops) {
        labels.append(opKindLabel(operation.kind));
    }
    return QStringLiteral("Updated host '%1' with ops: %2").arg(alias, labels.join(", "));
}

/* host_remove */

QSocToolHostRemove::QSocToolHostRemove(QObject *parent, QSocHostCatalog *catalog)
    : QSocTool(parent)
    , catalog_(catalog)
{}

QString QSocToolHostRemove::getName() const
{
    return QStringLiteral("host_remove");
}

QString QSocToolHostRemove::getDescription() const
{
    return QStringLiteral(
        "Remove a named host from the project catalog. Clears the active "
        "binding if it was pointing at the removed alias. Cannot remove "
        "user-scope entries; those live in `~/.config/qsoc/host.yml` and the "
        "user edits that file directly.");
}

json QSocToolHostRemove::getParametersSchema() const
{
    return json{
        {"type", "object"},
        {"properties",
         {{"alias", {{"type", "string"}, {"description", "Catalog alias to remove."}}}}},
        {"required", json::array({"alias"})}};
}

QString QSocToolHostRemove::execute(const json &arguments)
{
    if (catalog_ == nullptr) {
        return QStringLiteral("Error: host catalog is not configured");
    }
    const QString alias = jsonString(arguments, "alias");
    if (alias.isEmpty()) {
        return QStringLiteral("Error: alias is required");
    }
    QString err;
    if (!catalog_->remove(alias, &err)) {
        return QStringLiteral("Error: %1").arg(err);
    }
    return QStringLiteral("Removed host '%1'").arg(alias);
}
