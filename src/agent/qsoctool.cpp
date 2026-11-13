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
    , registry_(qobject_cast<QSocToolRegistry *>(fallbackScope))
{}

QMetaObject::Connection QSocToolCallContext::retainCancellation(
    QObject *receiver, std::function<void()> callback) const
{
    if (!registry_ || !receiver || !callback)
        return {};
    return connect(
        registry_,
        &QSocToolRegistry::callsAborted,
        receiver,
        [owner = owner_, callback = std::move(callback)](QObject *cancelledOwner, bool all) {
            if (all || (owner && owner == cancelledOwner))
                callback();
        });
}

bool QSocToolCallContext::isCancellationRequested() const
{
    return cancellationRequested_ || stopSource_.stop_requested();
}

void QSocToolCallContext::setRequestContext(const QString &effort, std::stop_token stop)
{
    reasoningEffort_ = effort;
    parentToken_     = stop;
    parentStop_.emplace(stop, [this, source = stopSource_]() mutable {
        /* Stop callbacks can release the context, so queue its notification first. */
        QMetaObject::invokeMethod(this, [this] { requestCancellation(); }, Qt::QueuedConnection);
        source.request_stop();
    });
}

void QSocToolCallContext::requestCancellation()
{
    if (cancellationRequested_) {
        return;
    }
    const QPointer<QSocToolCallContext> guard(this);
    auto                                source = stopSource_;
    cancellationRequested_                     = true;
    resultStatus_                              = QSocToolResultStatus::Uncertain;
    source.request_stop();
    if (guard)
        emit guard->cancellationRequested();
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
    return QSocToolResult::classify(result);
}

QString QSocTool::statusName(ResultStatus status)
{
    return QSocToolResult::name(status);
}

QSocTool::~QSocTool() = default;

void QSocTool::abort() {}

QSocToolCallContext *QSocTool::currentCallContext() const
{
    return callContexts_.isEmpty() ? nullptr : callContexts_.constLast().data();
}

QSocImageAttach::Options QSocTool::imageAttachmentOptions() const
{
    QSocImageAttach::Options options;
    if (const auto *context = currentCallContext()) {
        options.stop            = context->stopToken();
        options.reasoningEffort = context->reasoningEffort();
    }
    return options;
}

QString QSocTool::executeChildTool(QSocTool *tool, const json &arguments) const
{
    const QPointer<QSocTool> child(tool);
    if (!child)
        return QStringLiteral("Error: child tool is unavailable");
    child->callContexts_.append(currentCallContext());
    const auto restore = qScopeGuard([child] {
        if (child)
            child->callContexts_.removeLast();
    });
    return child->execute(arguments);
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

namespace {

/* A trailing underscore names a whole family. */
constexpr const char *kWorkspaceBound[] = {
    "grep",         "show_figure", "read_file",  "read_document", "read_waveform", "list_files",
    "write_file",   "edit_file",   "bash",       "bash_manage",   "path_context",  "monitor",
    "monitor_stop", "lsp",         "todo_",      "skill_",        "project_",      "module_",
    "bus_",         "generate_",   "schematic_",
};

} // namespace

QString QSocToolRegistry::workspaceBoundFamily(const QString &name)
{
    for (const char *entry : kWorkspaceBound) {
        const QLatin1String bound(entry);
        if (name == bound)
            return name;
        if (bound.endsWith(QLatin1Char('_')) && name.startsWith(bound))
            return QString(bound) + QLatin1Char('*');
    }
    return {};
}

bool QSocToolRegistry::isWorkspaceBound(const QString &name)
{
    return !workspaceBoundFamily(name).isEmpty();
}

void QSocToolRegistry::setFallback(QSocToolRegistry *base)
{
    if (base == this || base == fallback_.data())
        return;
    /* Keep revision() moving forward when the base changes. */
    revision_ += 1 + (fallback_ ? fallback_->revision() : 0);
    fallback_ = base;
}

bool QSocToolRegistry::reachesBase(const QString &name) const
{
    return fallback_ && !isWorkspaceBound(name);
}

bool QSocToolRegistry::isBaseTool(const QString &name) const
{
    return tools_.value(name).isNull() && reachesBase(name) && fallback_->getTool(name) != nullptr;
}

quint64 QSocToolRegistry::revision() const
{
    return revision_ + (fallback_ ? fallback_->revision() : 0);
}

QMap<QString, QPointer<QSocTool>> QSocToolRegistry::visibleTools() const
{
    QMap<QString, QPointer<QSocTool>> visible;
    if (fallback_) {
        const auto base = fallback_->visibleTools();
        for (auto it = base.constBegin(); it != base.constEnd(); ++it) {
            if (reachesBase(it.key()))
                visible.insert(it.key(), it.value());
        }
    }
    for (auto it = tools_.constBegin(); it != tools_.constEnd(); ++it) {
        if (!it.value().isNull())
            visible.insert(it.key(), it.value());
    }
    return visible;
}

bool QSocToolRegistry::ownsTool(const QString &name, const QSocTool *tool) const
{
    return tools_.value(name).data() == tool;
}

QSocTool *QSocToolRegistry::getTool(const QString &name) const
{
    if (QSocTool *own = tools_.value(name).data())
        return own;
    if (reachesBase(name))
        return fallback_->getTool(name);
    return nullptr;
}

bool QSocToolRegistry::hasTool(const QString &name) const
{
    return getTool(name) != nullptr;
}

json QSocToolRegistry::getToolDefinitions() const
{
    json       definitions = json::array();
    const auto tools       = visibleTools();
    for (const auto &tool : tools) {
        if (!tool.isNull())
            definitions.push_back(tool->getDefinition());
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
    std::function<void(QSocToolResultStatus)> outcome,
    const QString                            &reasoningEffort,
    std::stop_token                           stop)
{
    QPointer<QSocTool> tool = getTool(name);
    if (tool.isNull()) {
        return QString("Error: Tool '%1' not found").arg(name);
    }

    ActiveCall call(tool, owner, this, !ownsTool(name, tool));
    call.context.setRequestContext(reasoningEffort, stop);
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
    std::function<void(QSocToolResultStatus)> outcome,
    const QString                            &reasoningEffort,
    std::stop_token                           stop)
{
    const QPointer<QSocTool> tool = getTool(name);
    if (!tool)
        return QString("Error: Tool '%1' not found").arg(name);
    if (!tool->supportsDeferred())
        return executeTool(
            name, arguments, owner, std::move(output), std::move(outcome), reasoningEffort, stop);
    auto *call = new ActiveCall(tool, owner, this, !ownsTool(name, tool));
    call->context.setRequestContext(reasoningEffort, stop);
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
    return static_cast<int>(visibleTools().size());
}

QStringList QSocToolRegistry::toolNames() const
{
    return visibleTools().keys();
}

void QSocToolRegistry::abortAll()
{
    const QPointer<QSocToolRegistry>     guard(this);
    QList<QPointer<QSocTool>>            tools = tools_.values();
    QList<QPointer<QSocToolCallContext>> contexts;
    for (ActiveCall *call : std::as_const(activeCalls_)) {
        if (!call->borrowed)
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
    if (guard)
        emit guard->callsAborted(nullptr, true);
}

void QSocToolRegistry::abortCalls(QObject *owner)
{
    if (owner == nullptr) {
        return;
    }

    const QPointer<QSocToolRegistry>     guard(this);
    const QPointer<QSocToolRegistry>     base = fallback_;
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
    if (base)
        base->abortCalls(owner);
    if (guard)
        emit guard->callsAborted(owner, false);
}

#include "moc_qsoctool.cpp"
