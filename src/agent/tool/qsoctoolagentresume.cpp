// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolagentresume.h"

#include "agent/qsocsubagenttasksource.h"

QSocToolAgentResume::QSocToolAgentResume(QObject *parent, QSocSubAgentTaskSource *taskSource)
    : QSocTool(parent)
    , taskSource_(taskSource)
{}

QString QSocToolAgentResume::getName() const
{
    return QStringLiteral("agent_resume");
}

QString QSocToolAgentResume::getDescription() const
{
    return QStringLiteral(
        "Prepare a resume payload for a backgrounded sub-agent run that survived a "
        "process restart (or simply went past its in-memory eviction window). "
        "Returns the original subagent_type plus a synthesized resume_prompt that "
        "embeds the prior transcript tail. Follow up with the `agent` tool using "
        "those fields to actually re-spawn. Use this when the parent agent needs to "
        "pick up where a prior run left off.");
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
            {"task_id", taskId.toStdString()},
            {"original_subagent_type", meta.subagentType.toStdString()},
            {"original_label", meta.label.toStdString()},
            {"original_status", meta.status.toStdString()},
            {"isolation", meta.isolation.toStdString()},
            {"worktree", meta.worktreePath.toStdString()},
            {"resume_prompt", resumePrompt.toStdString()},
            {"hint",
             "Call the `agent` tool with subagent_type=original_subagent_type and "
             "prompt=resume_prompt to actually re-spawn."}}
            .dump()
            .c_str());
}
