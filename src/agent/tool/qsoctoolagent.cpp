// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolagent.h"

#include "agent/qsocagent.h"
#include "agent/qsocagentdefinition.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocsubagenttasksource.h"
#include "common/qllmservice.h"

#include <memory>
#include <utility>
#include <QEventLoop>
#include <QPointer>

QSocToolAgent::QSocToolAgent(
    QObject                     *parent,
    QLLMService                 *llmService,
    QSocToolRegistry            *parentRegistry,
    QSocAgentConfig              parentConfig,
    QSocAgentDefinitionRegistry *defRegistry,
    QSocSubAgentTaskSource      *taskSource)
    : QSocTool(parent)
    , llmService_(llmService)
    , parentRegistry_(parentRegistry)
    , parentConfig_(std::move(parentConfig))
    , defRegistry_(defRegistry)
    , taskSource_(taskSource)
{}

QString QSocToolAgent::getName() const
{
    return QStringLiteral("agent");
}

QString QSocToolAgent::getDescription() const
{
    QString desc = QStringLiteral(
        "Spawn a child sub-agent to handle a self-contained task. The child runs in a "
        "fresh conversation with a focused tool set and returns a single concise result. "
        "Use for exploration, summarization, focused multi-step work that should not "
        "pollute the main context. Set run_in_background=true to keep working while the "
        "child runs; the active child surfaces in the Ctrl+B task overlay.\n"
        "\nAvailable subagent_type values:\n");
    if (defRegistry_ != nullptr) {
        desc += defRegistry_->describeAvailable();
    }
    return desc;
}

json QSocToolAgent::getParametersSchema() const
{
    json enumValues = json::array();
    if (defRegistry_ != nullptr) {
        for (const QString &name : defRegistry_->availableNames()) {
            enumValues.push_back(name.toStdString());
        }
    }
    return json{
        {"type", "object"},
        {"properties",
         {{"subagent_type",
           {{"type", "string"},
            {"enum", enumValues},
            {"description", "Identifier of the sub-agent type to spawn."}}},
          {"description",
           {{"type", "string"},
            {"description", "Short 3-7 word label shown in the task overlay row."}}},
          {"prompt",
           {{"type", "string"},
            {"description",
             "Full instructions the child agent will receive as its first user message."}}},
          {"run_in_background",
           {{"type", "boolean"},
            {"default", false},
            {"description",
             "When true, return task_id immediately and run the child asynchronously."}}}}},
        {"required", json::array({"subagent_type", "description", "prompt"})}};
}

namespace {

QString jsonStringField(const json &args, const char *key)
{
    if (!args.contains(key) || !args[key].is_string()) {
        return {};
    }
    return QString::fromStdString(args[key].get<std::string>());
}

bool jsonBoolField(const json &args, const char *key, bool fallback)
{
    if (!args.contains(key) || !args[key].is_boolean()) {
        return fallback;
    }
    return args[key].get<bool>();
}

} // namespace

QString QSocToolAgent::execute(const json &arguments)
{
    if (defRegistry_ == nullptr || taskSource_ == nullptr) {
        return QStringLiteral(R"({"status":"error","error":"agent tool is not wired up"})");
    }

    const QString subagentType = jsonStringField(arguments, "subagent_type");
    const QString description  = jsonStringField(arguments, "description");
    const QString prompt       = jsonStringField(arguments, "prompt");
    const bool    background   = jsonBoolField(arguments, "run_in_background", false);

    if (subagentType.isEmpty() || prompt.isEmpty()) {
        return QStringLiteral(
            R"({"status":"error","error":"subagent_type and prompt are required"})");
    }

    const QSocAgentDefinition *def = defRegistry_->find(subagentType);
    if (def == nullptr) {
        return QString::fromUtf8(
            json{
                {"status", "error"},
                {"error", std::string("unknown subagent_type: ") + subagentType.toStdString()},
                {"available",
                 QString(defRegistry_->availableNames().join(QStringLiteral(", "))).toStdString()}}
                .dump()
                .c_str());
    }

    /* Single-in-flight policy: shared QLLMService cannot stream two
     * conversations at once. Block a second concurrent spawn. */
    if (taskSource_->hasActiveRun()) {
        return QStringLiteral(
            R"({"status":"error","error":"another sub-agent is currently running; )"
            R"(wait for it to finish or kill it from the task overlay"})");
    }

    /* Resolve parent registry + config dynamically from the live parent
     * agent when bound. This makes the spawn tool remote-mode correct:
     * after `/remote` swaps the parent's registry and sets remoteMode,
     * the child built here picks up the SSH-backed tool registry and
     * the remote config in the same step, instead of a stale local
     * snapshot captured at construction time. */
    QSocToolRegistry *effectiveRegistry = parentRegistry_;
    QSocAgentConfig   effectiveConfig   = parentConfig_;
    if (parentAgent_ != nullptr) {
        if (auto *liveReg = parentAgent_->getToolRegistry()) {
            effectiveRegistry = liveReg;
        }
        effectiveConfig = parentAgent_->getConfig();
    }

    if (llmService_ == nullptr || effectiveRegistry == nullptr) {
        return QStringLiteral(
            R"({"status":"error","error":"LLM service or tool registry not configured"})");
    }

    /* Build child config: clone parent (carries remote-mode fields,
     * project path, hooks) then apply definition overrides. */
    QSocAgentConfig childCfg      = effectiveConfig;
    childCfg.systemPromptOverride = def->promptBody;
    childCfg.toolsAllow           = def->toolsAllow;
    childCfg.isSubAgent           = true;
    childCfg.autoLoadMemory       = def->injectMemory;
    if (!def->injectSkills) {
        childCfg.skillListing.clear();
    }
    if (!def->model.isEmpty()) {
        childCfg.modelId = def->model;
    }

    auto *child = new QSocAgent(nullptr, llmService_, effectiveRegistry, childCfg);
    if (memoryManager_ != nullptr && def->injectMemory) {
        child->setMemoryManager(memoryManager_);
    }
    if (hookManager_ != nullptr) {
        child->setHookManager(hookManager_);
    }
    if (loopScheduler_ != nullptr) {
        child->setLoopScheduler(loopScheduler_);
    }

    const QString label  = description.isEmpty() ? subagentType : description;
    const QString taskId = taskSource_->registerRun(label, subagentType, child);

    /* Forward child token usage into the parent's running totals so
     * the parent's status pill / cost view reflects total cost in
     * real time. Routes only the DELTA on each emission to avoid
     * double counting. */
    if (parentAgent_ != nullptr) {
        auto *parent  = parentAgent_;
        auto  prevIn  = std::make_shared<qint64>(0);
        auto  prevOut = std::make_shared<qint64>(0);
        QObject::connect(
            child,
            &QSocAgent::tokenUsage,
            parent,
            [parent, prevIn, prevOut](qint64 inputTok, qint64 outputTok) {
                const qint64 dIn  = inputTok - *prevIn;
                const qint64 dOut = outputTok - *prevOut;
                *prevIn           = inputTok;
                *prevOut          = outputTok;
                parent->addExternalTokenUsage(dIn, dOut);
            });
    }

    /* Stream child progress into the overlay's transcript buffer. */
    QPointer<QSocSubAgentTaskSource> srcGuard(taskSource_);
    QObject::connect(
        child, &QSocAgent::contentChunk, taskSource_, [srcGuard, taskId](const QString &chunk) {
            if (!srcGuard.isNull()) {
                srcGuard->appendTranscript(taskId, chunk);
            }
        });
    QObject::connect(
        child,
        &QSocAgent::toolCalled,
        taskSource_,
        [srcGuard, taskId](const QString &name, const QString &args) {
            if (!srcGuard.isNull()) {
                srcGuard->appendTranscript(
                    taskId,
                    QStringLiteral("\n[tool] ") + name + QStringLiteral(" ") + args.left(200)
                        + QStringLiteral("\n"));
            }
        });
    QObject::connect(
        child,
        &QSocAgent::toolResult,
        taskSource_,
        [srcGuard, taskId](const QString &name, const QString &result) {
            if (!srcGuard.isNull()) {
                srcGuard->appendTranscript(
                    taskId,
                    QStringLiteral("[result ") + name + QStringLiteral("] ") + result.left(400)
                        + QStringLiteral("\n"));
            }
        });

    if (background) {
        QObject::connect(
            child,
            &QSocAgent::runComplete,
            taskSource_,
            [srcGuard, taskId](const QString &finalText) {
                if (!srcGuard.isNull()) {
                    srcGuard->markCompleted(taskId, finalText);
                }
            });
        QObject::connect(
            child, &QSocAgent::runError, taskSource_, [srcGuard, taskId](const QString &error) {
                if (!srcGuard.isNull()) {
                    srcGuard->markFailed(taskId, error);
                }
            });
        QObject::connect(
            child, &QSocAgent::runAborted, taskSource_, [srcGuard, taskId](const QString &) {
                if (!srcGuard.isNull()) {
                    srcGuard->markFailed(taskId, QStringLiteral("aborted"));
                }
            });
        child->runStream(prompt);
        return QString::fromUtf8(
            json{
                {"status", "async_launched"},
                {"task_id", taskId.toStdString()},
                {"subagent_type", subagentType.toStdString()},
                {"description", label.toStdString()}}
                .dump()
                .c_str());
    }

    /* Synchronous: child->run() nests a QEventLoop internally. The
     * parent is blocked here, so the shared QLLMService sees only
     * the child's stream; the single-in-flight invariant holds. */
    const QString result = child->run(prompt);
    taskSource_->markCompleted(taskId, result);

    return QString::fromUtf8(
        json{
            {"status", "ok"},
            {"task_id", taskId.toStdString()},
            {"subagent_type", subagentType.toStdString()},
            {"result", result.toStdString()}}
            .dump()
            .c_str());
}

void QSocToolAgent::abort()
{
    if (taskSource_ != nullptr) {
        taskSource_->abortAll();
    }
}
