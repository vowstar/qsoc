// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"

#include <utility>

#include <QScopeGuard>

/* QSocToolCallContext Implementation */

QSocToolCallContext::QSocToolCallContext(QObject *owner)
    : owner_(owner)
{}

bool QSocToolCallContext::isCancellationRequested() const
{
    return cancellationRequested_;
}

void QSocToolCallContext::requestCancellation()
{
    if (cancellationRequested_) {
        return;
    }
    cancellationRequested_ = true;
    emit cancellationRequested();
}

/* QSocTool Implementation */

QSocTool::QSocTool(QObject *parent)
    : QObject(parent)
{}

QSocTool::~QSocTool() = default;

void QSocTool::abort() {}

QSocToolCallContext *QSocTool::currentCallContext() const
{
    return callContexts_.isEmpty() ? nullptr : callContexts_.constLast().data();
}

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

    const QPointer<QSocToolRegistry> registry(this);
    const QPointer<QSocTool>         candidate(tool);
    QPointer<QSocTool>               previousTool;
    QString                          name;
    {
        const auto previousTools = tools_;
        name                     = candidate->getName();
        if (registry.isNull() || candidate.isNull()) {
            return;
        }
        previousTool = previousTools.value(name);
    }
    if (registry->tools_.value(name).data() != previousTool.data()) {
        return;
    }
    if (registry->tools_.value(name).data() == candidate.data()) {
        return;
    }
    registry->tools_[name] = candidate;
    connect(candidate, &QObject::destroyed, registry, [registry, candidate, name]() {
        if (registry.isNull()) {
            return;
        }
        auto it = registry->tools_.find(name);
        /* The current guard may already be null when destroyed is delivered. */
        if (it != registry->tools_.end()
            && (it.value().isNull()
                || (!candidate.isNull() && it.value().data() == candidate.data()))) {
            registry->tools_.erase(it);
        }
    });
}

bool QSocToolRegistry::unregisterTool(QSocTool *tool)
{
    if (tool == nullptr) {
        return false;
    }

    bool removed = false;
    for (auto it = tools_.begin(); it != tools_.end();) {
        if (it.value().data() == tool) {
            it      = tools_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    return removed;
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

QString QSocToolRegistry::executeTool(const QString &name, const json &arguments, QObject *owner)
{
    QPointer<QSocTool> tool = getTool(name);
    if (tool.isNull()) {
        return QString("Error: Tool '%1' not found").arg(name);
    }

    ActiveCall call(tool, owner);
    activeCalls_.insert(&call);
    tool->callContexts_.append(&call.context);
    QPointer<QSocToolRegistry> registry(this);
    const auto                 removeCall = qScopeGuard([registry, tool, &call]() {
        if (!tool.isNull()) {
            tool->callContexts_.removeOne(&call.context);
        }
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
    QList<QPointer<QSocTool>>            tools = tools_.values();
    QList<QPointer<QSocToolCallContext>> contexts;
    for (ActiveCall *call : std::as_const(activeCalls_)) {
        tools.append(call->tool);
        contexts.append(&call->context);
    }

    for (const auto &context : contexts) {
        if (!context.isNull()) {
            context->requestCancellation();
        }
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

void QSocToolRegistry::abortCalls(QObject *owner)
{
    if (owner == nullptr) {
        return;
    }

    QList<QPointer<QSocToolCallContext>> contexts;
    for (ActiveCall *call : std::as_const(activeCalls_)) {
        if (call->context.owner_.data() == owner) {
            contexts.append(&call->context);
        }
    }
    for (const auto &context : contexts) {
        if (!context.isNull()) {
            context->requestCancellation();
        }
    }
}
