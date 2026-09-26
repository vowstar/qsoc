// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"

#include <utility>

#include <QScopeGuard>
#include <QTimer>

/* QSocToolCallContext Implementation */

QSocToolCallContext::QSocToolCallContext(QObject *owner)
    : QSocToolCallContext(owner, nullptr)
{}

QSocToolCallContext::QSocToolCallContext(QObject *owner, QObject *fallbackScope)
    : owner_(owner)
    , scope_(owner != nullptr ? owner : fallbackScope)
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
    resultStatus_          = QSocToolResultStatus::Uncertain;
    emit cancellationRequested();
}

void QSocToolCallContext::completeDeferred(const QString &result)
{
    if (!deferred_ || completed_)
        return;
    completed_ = true;
    emit deferredCompleted(result);
}

/* QSocTool Implementation */

QSocTool::QSocTool(QObject *parent)
    : QObject(parent)
{}

QString QSocTool::statusLine(ResultStatus status)
{
    switch (status) {
    case ResultStatus::Ok:
        return QStringLiteral("status: ok\n");
    case ResultStatus::Failed:
        return QStringLiteral("status: failed\n");
    case ResultStatus::Uncertain:
        return QStringLiteral("status: uncertain\n");
    case ResultStatus::Dispatched:
        return QStringLiteral("status: dispatched\n");
    }
    return QStringLiteral("status: uncertain\n");
}

QSocTool::ResultStatus QSocTool::classifyResult(const QString &result)
{
    /* An explicit status must be the first line, so no amount of body text
     * can forge or hide one. */
    const QString first = result.left(result.indexOf(QLatin1Char('\n'))).trimmed();
    if (first == QStringLiteral("status: ok")) {
        return ResultStatus::Ok;
    }
    if (first == QStringLiteral("status: failed")) {
        return ResultStatus::Failed;
    }
    if (first == QStringLiteral("status: uncertain")) {
        return ResultStatus::Uncertain;
    }
    if (first == QStringLiteral("status: dispatched"))
        return ResultStatus::Dispatched;
    if (result.trimmed().startsWith(QStringLiteral("Error:"))) {
        return ResultStatus::Failed;
    }
    /* Tools that answer in JSON declare the same thing in a "status" member,
     * so read that rather than leaving their failures unstyled. Only a
     * top-level object counts, and only its own status member: this is the
     * tool's declaration, not a search of its payload. */
    const QString trimmed = result.trimmed();
    if (trimmed.startsWith(QLatin1Char('{'))) {
        const auto parsed = json::parse(trimmed.toStdString(), nullptr, /*allow_exceptions=*/false);
        if (parsed.is_object() && parsed.contains("status") && parsed["status"].is_string()) {
            const auto declared = QString::fromStdString(parsed["status"].get<std::string>());
            if (declared == QStringLiteral("error")) {
                return ResultStatus::Failed;
            }
            if (declared == QStringLiteral("uncertain")) {
                return ResultStatus::Uncertain;
            }
        }
    }
    return ResultStatus::Ok;
}

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
    ++registry->revision_;
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
            ++registry->revision_;
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
            it = tools_.erase(it);
            ++revision_;
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

void QSocToolCallContext::reportOutput(const QString &text)
{
    if (!text.isEmpty() && !isCancellationRequested() && !completed_)
        emit outputReady(text);
}

QString QSocToolRegistry::executeTool(
    const QString                            &name,
    const json                               &arguments,
    QObject                                  *owner,
    std::function<void(const QString &)>      output,
    std::function<void(QSocToolResultStatus)> outcome)
{
    QPointer<QSocTool> tool = getTool(name);
    if (tool.isNull()) {
        return QString("Error: Tool '%1' not found").arg(name);
    }

    ActiveCall call(tool, owner, this);
    if (output)
        connect(&call.context, &QSocToolCallContext::outputReady, &call.context, std::move(output));
    // cppcheck-suppress danglingLifetime
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
    const QString              result     = tool->execute(arguments);
    if (outcome)
        outcome(call.context.resultStatus_.value_or(QSocTool::classifyResult(result)));
    return result;
}

std::optional<QString> QSocToolRegistry::executeToolDeferred(
    const QString                            &name,
    const json                               &arguments,
    QObject                                  *owner,
    std::function<void(const QString &)>      completed,
    std::function<void(const QString &)>      output,
    std::function<void(QSocToolResultStatus)> outcome)
{
    const QPointer<QSocTool> tool = getTool(name);
    if (!tool)
        return QString("Error: Tool '%1' not found").arg(name);
    if (!tool->supportsDeferred())
        return executeTool(name, arguments, owner, std::move(output), std::move(outcome));
    auto *call = new ActiveCall(tool, owner, this);
    call->setParent(this);
    call->context.canDefer_ = true;
    if (output)
        connect(&call->context, &QSocToolCallContext::outputReady, call, std::move(output));
    activeCalls_.insert(call);
    tool->callContexts_.append(&call->context);
    const QPointer<QSocToolRegistry> registry(this);
    const QPointer<QObject>          receiver(owner);
    const QPointer<ActiveCall>       guardedCall(call);
    connect(
        &call->context,
        &QSocToolCallContext::deferredCompleted,
        call,
        [registry, receiver, call, outcome, completed = std::move(completed)](
            const QString &result) {
            const auto status = call->context.resultStatus_.value_or(
                QSocTool::classifyResult(result));
            if (registry)
                registry->activeCalls_.remove(call);
            if (receiver)
                QTimer::singleShot(0, receiver, [completed, outcome, status, result, receiver]() {
                    if (outcome)
                        outcome(status);
                    if (receiver)
                        completed(result);
                });
            call->deleteLater();
        });
    const auto cancel = [call]() {
        call->context.requestCancellation();
        call->context.completeDeferred(QStringLiteral("Error: Tool invocation cancelled"));
    };
    connect(tool, &QObject::destroyed, call, cancel);
    connect(this, &QObject::destroyed, call, cancel);
    if (owner)
        connect(owner, &QObject::destroyed, call, cancel);
    /* execute() may destroy its tool before returning. */
    // cppcheck-suppress nullPointerRedundantCheck
    const QString result = tool->execute(arguments);
    if (tool && guardedCall)
        tool->callContexts_.removeOne(&call->context);
    if (!guardedCall)
        return QStringLiteral("Error: Tool registry destroyed");
    if (call->context.deferred_)
        return std::nullopt;
    if (registry)
        registry->activeCalls_.remove(call);
    const auto status = call->context.resultStatus_.value_or(QSocTool::classifyResult(result));
    delete call;
    if (outcome)
        outcome(status);
    return result;
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
