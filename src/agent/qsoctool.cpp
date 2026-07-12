// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"

#include <utility>

#include <QScopeGuard>

/* QSocTool Implementation */

QSocTool::QSocTool(QObject *parent)
    : QObject(parent)
{}

QSocTool::~QSocTool() = default;

void QSocTool::abort() {}

json QSocTool::getDefinition() const
{
    return {
        {"type", "function"},
        {"function",
         {{"name", getName().toStdString()},
          {"description", getDescription().toStdString()},
          {"parameters", getParametersSchema()}}}};
}

/* QSocToolRegistry Implementation */

QSocToolRegistry::QSocToolRegistry(QObject *parent)
    : QObject(parent)
{}

QSocToolRegistry::~QSocToolRegistry() = default;

void QSocToolRegistry::registerTool(QSocTool *tool)
{
    if (tool == nullptr) {
        return;
    }

    const QString name = tool->getName();
    if (tools_.value(name).data() == tool) {
        return;
    }
    tools_[name] = tool;
    connect(tool, &QObject::destroyed, this, [this, name, tool]() {
        auto it = tools_.find(name);
        /* The current guard may already be null when destroyed is delivered. */
        if (it != tools_.end() && (it.value().isNull() || it.value().data() == tool)) {
            tools_.erase(it);
        }
    });
}

bool QSocToolRegistry::unregisterTool(QSocTool *tool)
{
    if (tool == nullptr) {
        return false;
    }

    auto it = tools_.find(tool->getName());
    if (it == tools_.end() || it.value().data() != tool) {
        return false;
    }
    tools_.erase(it);
    return true;
}

QSocTool *QSocToolRegistry::getTool(const QString &name) const
{
    return tools_.value(name).data();
}

bool QSocToolRegistry::hasTool(const QString &name) const
{
    return getTool(name) != nullptr;
}

json QSocToolRegistry::getToolDefinitions() const
{
    json       definitions = json::array();
    const auto tools       = tools_.values();
    for (const auto &tool : tools) {
        if (!tool.isNull()) {
            definitions.push_back(tool->getDefinition());
        }
    }
    return definitions;
}

QString QSocToolRegistry::executeTool(const QString &name, const json &arguments)
{
    QPointer<QSocTool> tool = getTool(name);
    if (tool.isNull()) {
        return QString("Error: Tool '%1' not found").arg(name);
    }

    ActiveCall call{tool};
    activeCalls_.insert(&call);
    QPointer<QSocToolRegistry> registry(this);
    const auto                 removeCall = qScopeGuard([registry, &call]() {
        if (!registry.isNull()) {
            registry->activeCalls_.remove(&call);
        }
    });
    return tool->execute(arguments);
}

int QSocToolRegistry::count() const
{
    int total = 0;
    for (const auto &tool : tools_) {
        if (!tool.isNull()) {
            ++total;
        }
    }
    return total;
}

QStringList QSocToolRegistry::toolNames() const
{
    QStringList names;
    for (auto it = tools_.constBegin(); it != tools_.constEnd(); ++it) {
        if (!it.value().isNull()) {
            names.append(it.key());
        }
    }
    return names;
}

void QSocToolRegistry::abortAll()
{
    QList<QPointer<QSocTool>> tools = tools_.values();
    for (const ActiveCall *call : std::as_const(activeCalls_)) {
        tools.append(call->tool);
    }

    QSet<QSocTool *> seen;
    for (const auto &tool : tools) {
        QSocTool *current = tool.data();
        if (current == nullptr || seen.contains(current)) {
            continue;
        }
        seen.insert(current);
        current->abort();
    }
}
