// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolagentresume.h"

#include "agent/qsocagent.h"
#include "agent/qsocagentmailbox.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/tool/qsoctoolagent.h"

QSocToolAgentResume::QSocToolAgentResume(
    QObject *parent, QSocSubAgentTaskSource *taskSource, QSocToolAgent *spawner)
    : QSocTool(parent)
    , taskSource_(taskSource)
    , spawner_(spawner)
{}

QString QSocToolAgentResume::getName() const
{
    return QStringLiteral("agent_resume");
}

QString QSocToolAgentResume::getDescription() const
{
    return QStringLiteral(
        "Continue an earlier sub-agent run. A live child receives new_instructions "
        "as a follow-up (resume: live or queued). A finished child with a stored "
        "history is rebuilt from it and runs in the background (resume: history); "
        "its result arrives as a task notification. Otherwise returns the original "
        "subagent_type plus a synthesized resume_prompt that embeds the prior "
        "transcript tail (resume: prompt_only); pass those to the `agent` tool.");
}

json QSocToolAgentResume::getParametersSchema() const
{
    return json{
        {"type", "object"},
        {"properties",
         {{"task_id",
           {{"type", "string"},
            {"description",
             "task_id from a prior `agent` call (run_in_background=true) or from "
             "/agents-history."}}},
          {"new_instructions",
           {{"type", "string"},
            {"description",
             "Optional new instructions appended to the resume payload. When "
             "empty, the resume_prompt asks the child to continue the task it "
             "was working on."}}},
          {"max_tail_bytes",
           {{"type", "integer"},
            {"default", 4000},
            {"description", "Cap on transcript tail bytes embedded in the resume_prompt."}}}}},
        {"required", json::array({"task_id"})}};
}

QString QSocToolAgentResume::execute(const json &arguments)
{
    if (taskSource_ == nullptr) {
        return QStringLiteral(R"({"status":"error","error":"task source not configured"})");
    }
    if (!arguments.contains("task_id") || !arguments["task_id"].is_string()) {
        return QStringLiteral(R"({"status":"error","error":"task_id is required"})");
    }
    const QString taskId = QString::fromStdString(arguments["task_id"].get<std::string>());
    const QString newInstructions
        = (arguments.contains("new_instructions") && arguments["new_instructions"].is_string())
              ? QString::fromStdString(arguments["new_instructions"].get<std::string>())
              : QString();
    int maxTailBytes = 4000;
    if (arguments.contains("max_tail_bytes") && arguments["max_tail_bytes"].is_number_integer()) {
        maxTailBytes = arguments["max_tail_bytes"].get<int>();
        if (maxTailBytes < 0) {
            maxTailBytes = 0;
        }
    }

    const QPointer<QSocToolCallContext> context(currentCallContext());
    auto         *caller = context ? qobject_cast<QSocAgent *>(context->executionScope()) : nullptr;
    auto         *mailbox = taskSource_->mailbox();
    const QString sender  = caller != nullptr && mailbox != nullptr ? mailbox->idFor(caller)
                                                                    : QString();
    if (spawner_ != nullptr && !sender.isEmpty()) {
        const json resumed = spawner_->resumeRun(taskId, newInstructions, sender);
        if (resumed.value("resume", std::string()) != "unavailable") {
            return QString::fromStdString(resumed.dump());
        }
    }

    QSocSubAgentTaskSource::HistoricalRun meta;
    if (!taskSource_->findHistoricalRun(taskId, &meta)) {
        return QString::fromUtf8(
            json{
                {"status", "error"},
                {"error",
                 std::string("no metadata sidecar found for task_id ") + taskId.toStdString()}}
                .dump()
                .c_str());
    }

    const QString tail = taskSource_->tailFor(taskId, maxTailBytes);

    /* Synthesize the resume prompt. The child receives an explicit
     * "this is a resumed run" framing so the LLM understands the
     * embedded tail is its OWN earlier work, not the parent's. */
    QString resumePrompt = QStringLiteral(
        "You are RESUMING an earlier %1 sub-agent run (task %2: %3).\n"
        "The transcript below is from your own prior session; "
        "continue from where you left off.\n"
        "\n"
        "=== Prior transcript (tail) ===\n"
        "%4\n"
        "=== End prior transcript ===\n");
    resumePrompt
        = resumePrompt
              .arg(meta.subagentType.isEmpty() ? QStringLiteral("(unknown)") : meta.subagentType)
              .arg(taskId)
              .arg(meta.label.isEmpty() ? QStringLiteral("(no label)") : meta.label)
              .arg(tail.isEmpty() ? QStringLiteral("(transcript empty)") : tail);
    if (!newInstructions.isEmpty()) {
        resumePrompt += QStringLiteral("\nNew instructions:\n") + newInstructions
                        + QLatin1Char('\n');
    } else {
        resumePrompt += QStringLiteral(
            "\nResume the task using the prior context. If the original goal "
            "appears already complete in the transcript, summarize the outcome "
            "instead of re-running it.\n");
    }

    return QString::fromUtf8(
        json{
            {"status", "ok"},
            {"resume", "prompt_only"},
            {"task_id", taskId.toStdString()},
            {"original_subagent_type", meta.subagentType.toStdString()},
            {"original_label", meta.label.toStdString()},
            {"original_status", meta.status.toStdString()},
            {"isolation", meta.isolation.toStdString()},
            {"worktree", meta.worktreePath.toStdString()},
            {"host", meta.host.toStdString()},
            {"workspace", meta.workspace.toStdString()},
            {"model", meta.model.toStdString()},
            {"resume_prompt", resumePrompt.toStdString()},
            {"hint",
             "Call the `agent` tool with subagent_type=original_subagent_type and "
             "prompt=resume_prompt to actually re-spawn. Pass host, workspace and model "
             "too when they are set, so the run continues where it ran."}}
            .dump()
            .c_str());
}

#include "moc_qsoctoolagentresume.cpp"
