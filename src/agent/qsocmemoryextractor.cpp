// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocmemoryextractor.h"

#include "agent/qsocagent.h"

#include <QEventLoop>
#include <QStringList>

using json = nlohmann::json;

QSocMemoryExtractor::Decision QSocMemoryExtractor::decide(
    const json &messages, int cursor, const QSocAgentConfig &cfg)
{
    Decision decision;
    if (!messages.is_array()) {
        return decision;
    }

    const int total = static_cast<int>(messages.size());
    const int start = cursor < 0 ? 0 : cursor;
    if (start >= total) {
        return decision;
    }

    for (int idx = start; idx < total; idx++) {
        const auto       &msg  = messages[idx];
        const std::string role = msg.value("role", std::string());
        if (role == "user" || role == "assistant") {
            decision.newCount++;
        }
        if (role == "assistant" && msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto &call : msg["tool_calls"]) {
                if (call.contains("function")
                    && call["function"].value("name", std::string()) == "memory_write") {
                    decision.alreadyWritten = true;
                }
            }
        }
    }

    if (decision.alreadyWritten) {
        /* The main agent already saved memory in this slice: caller skips
         * the fork and advances the cursor. */
        return decision;
    }
    decision.run = decision.newCount >= cfg.memoryExtractMinNewMessages;
    return decision;
}

QString QSocMemoryExtractor::systemPrompt()
{
    return QStringLiteral(
        "You are the memory-extraction sub-agent for QSoC. Read the recent "
        "conversation slice and update persistent memory so future sessions "
        "start better informed. You may only use memory_read and memory_write.\n\n"
        "Use at most a few turns: read any existing topics you might update, "
        "then write. Do not investigate the codebase or verify anything "
        "further; rely only on what the slice states.\n\n"
        "## Memory types\n"
        "- user: who the user is (role, expertise, durable preferences).\n"
        "- feedback: how to work (corrections and confirmed approaches). State "
        "the rule, then a 'Why:' line and a 'How to apply:' line.\n"
        "- project: ongoing goals, decisions, constraints not derivable from "
        "code. Convert relative dates to absolute.\n"
        "- reference: pointers to external resources (URLs, dashboards, "
        "tickets).\n\n"
        "## Do NOT save\n"
        "- Anything derivable from code, git history, or AGENTS.md.\n"
        "- Debugging recipes or one-off fixes.\n"
        "- Ephemeral or in-progress task state from this conversation.\n"
        "- Secrets, tokens, credentials, internal hostnames, or private paths.\n\n"
        "## How to write\n"
        "Call memory_write with name (kebab-case), type, description (one "
        "line), scope ('user' for global, 'project' for this project), and "
        "content (Markdown). The MEMORY.md index updates automatically. Prefer "
        "updating an existing topic over a near-duplicate: memory_read it "
        "first, merge, then memory_write the same name. If the slice holds "
        "nothing durable, do nothing.");
}

QString QSocMemoryExtractor::buildManifest(const QList<QSocMemoryManager::MemoryHeader> &headers)
{
    if (headers.isEmpty()) {
        return QStringLiteral("(none yet)");
    }

    QString out;
    for (const auto &header : headers) {
        const QString type = header.type.isEmpty() ? QStringLiteral("note") : header.type;
        const QString desc = header.description.isEmpty() ? header.name : header.description;
        out += QStringLiteral("- [%1/%2] %3: %4\n").arg(type, header.scope, header.name, desc);
    }
    return out;
}

QString QSocMemoryExtractor::buildUserMessage(
    const QString &transcript, const QString &manifest, int newCount)
{
    /* Single multi-arg call: substitutes all placeholders at once so a
     * literal %N inside manifest/transcript is not re-substituted. */
    return QStringLiteral(
               "Existing memory topics:\n%1\n\n"
               "Recent conversation (last %2 messages) to extract from:\n%3")
        .arg(manifest, QString::number(newCount), transcript);
}

QSocMemoryExtractor::QSocMemoryExtractor(
    QSocAgent *parent, QSocMemoryManager *memoryManager, QLLMService *llmService)
    : parent_(parent)
    , memoryManager_(memoryManager)
    , llmService_(llmService)
{}

int QSocMemoryExtractor::extract(
    int cursor, int turnNumber, const std::function<void(QSocAgent *)> &onSpawn)
{
    if (!parent_ || !memoryManager_ || !llmService_) {
        return cursor;
    }

    const QSocAgentConfig cfg = parent_->getConfig();
    if (!cfg.memoryExtractEnabled) {
        return cursor;
    }
    if (cfg.memoryExtractEveryTurns > 1 && (turnNumber % cfg.memoryExtractEveryTurns) != 0) {
        return cursor;
    }

    const json messages = parent_->getMessages();
    const int  total    = messages.is_array() ? static_cast<int>(messages.size()) : 0;
    /* Heal a stale cursor left past the end by an in-session compaction
     * that shrank `messages`; otherwise extraction would never run again. */
    const int      start    = qBound(0, cursor, total);
    const Decision decision = decide(messages, start, cfg);

    if (decision.alreadyWritten) {
        return total; /* Main agent saved; advance past the slice. */
    }
    if (!decision.run) {
        return start; /* Nothing worth extracting yet; keep the (clamped) cursor. */
    }

    const QString transcript = parent_->formatMessagesForSummary(start, total);
    const QString manifest   = buildManifest(memoryManager_->scanHeaders("all"));
    const QString userMsg    = buildUserMessage(transcript, manifest, decision.newCount);

    /* Constrained child: memory tools only, short turn cap, no memory /
     * project / skill injection (keeps it focused and cache-cheap). */
    QSocAgentConfig childCfg = cfg;
    childCfg.isSubAgent      = true;
    childCfg.toolsAllow      = QStringList{"memory_read", "memory_write"};
    childCfg.toolsDeny.clear();
    childCfg.maxTurnsOverride    = 5;
    childCfg.autoLoadMemory      = false;
    childCfg.memoryRecallEnabled = false;
    childCfg.injectProjectMd     = false;
    childCfg.skillListing.clear();
    childCfg.criticalReminder.clear();
    childCfg.planMode = false;
    /* Memory tools only touch local files, so the child is never remote.
     * Model and reasoning effort are inherited from the user's config; an
     * explicit memory_extract_model may override. qsoc imposes no cost bias
     * and never silently swaps in a model the user did not choose. */
    childCfg.remoteMode           = false;
    childCfg.hooks                = QSocHookConfig();
    childCfg.systemPromptOverride = systemPrompt();
    if (!cfg.memoryExtractModel.isEmpty()) {
        childCfg.modelId = cfg.memoryExtractModel;
    }

    QLLMService *childLlm = llmService_->clone(nullptr);
    if (!cfg.memoryExtractModel.isEmpty()) {
        childLlm->setCurrentModel(cfg.memoryExtractModel);
    }
    auto *child = new QSocAgent(nullptr, childLlm, parent_->getToolRegistry(), childCfg);
    childLlm->setParent(child); /* tie LLM lifetime to the child */
    child->setMemoryManager(memoryManager_);

    if (onSpawn) {
        onSpawn(child);
    }

    /* Drive the child on a nested loop; quit on any terminal signal. The
     * delete happens only after exec() returns, never inside a handler. */
    QEventLoop loop;
    bool       terminal = false;
    const auto stop     = [&loop, &terminal]() {
        if (terminal) {
            return;
        }
        terminal = true;
        loop.quit();
    };
    QObject::connect(child, &QSocAgent::runComplete, &loop, [stop](const QString &) { stop(); });
    QObject::connect(child, &QSocAgent::runError, &loop, [stop](const QString &) { stop(); });
    QObject::connect(child, &QSocAgent::runAborted, &loop, [stop](const QString &) { stop(); });

    child->runStream(userMsg);
    if (!terminal) {
        loop.exec();
    }

    delete child;

    return total;
}
