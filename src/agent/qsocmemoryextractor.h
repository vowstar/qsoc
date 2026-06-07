// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMEMORYEXTRACTOR_H
#define QSOCMEMORYEXTRACTOR_H

#include "agent/qsocagentconfig.h"
#include "agent/qsocmemorymanager.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <QList>
#include <QString>

class QSocAgent;
class QLLMService;

/**
 * @brief Background memory extraction via a constrained sub-agent.
 * @details After each turn the REPL hands the new conversation slice to a
 *          forked child agent restricted to memory_read / memory_write,
 *          which distills durable facts into topic files. The decision
 *          logic and prompt builders are pure statics (unit-testable with
 *          canned JSON, no LLM); extract() performs the actual spawn and
 *          drives the child on a nested event loop at REPL idle.
 *
 *          A cursor (index into the message array) tracks which messages
 *          have been processed. When the main agent already called
 *          memory_write in the slice, extraction is skipped and the cursor
 *          advances (no duplicate work).
 */
class QSocMemoryExtractor
{
public:
    /* Outcome of the pure pre-flight check over messages[cursor, end). */
    struct Decision
    {
        bool run            = false; /* Spawn the extractor child */
        bool alreadyWritten = false; /* Main agent saved memory in slice */
        int  newCount       = 0;     /* User/assistant messages in slice */
    };

    /* Pure: decide whether to extract the slice messages[cursor, end). */
    static Decision decide(const nlohmann::json &messages, int cursor, const QSocAgentConfig &cfg);

    /* System prompt for the extractor child (4 memory types + the
     * not-to-save rules + the two-step write convention). */
    static QString systemPrompt();

    /* User message: the delta transcript + a manifest of existing topics
     * so the child updates rather than duplicates. */
    static QString buildUserMessage(const QString &transcript, const QString &manifest, int newCount);

    /* Render existing topic headers into a compact manifest. */
    static QString buildManifest(const QList<QSocMemoryManager::MemoryHeader> &headers);

    QSocMemoryExtractor(QSocAgent *parent, QSocMemoryManager *memoryManager, QLLMService *llmService);

    /* Run extraction for the slice after @p cursor. Returns the new
     * cursor: end-of-messages when a child ran or the main agent already
     * saved; unchanged when skipped (too few new messages, disabled, or
     * off-cadence for @p turnNumber). Synchronous; safe at REPL idle.
     * onSpawn, when set, is invoked with the child just before it starts,
     * so the caller can show status and wire abort. */
    int extract(int cursor, int turnNumber, const std::function<void(QSocAgent *)> &onSpawn = {});

private:
    QSocAgent         *parent_;
    QSocMemoryManager *memoryManager_;
    QLLMService       *llmService_;
};

#endif // QSOCMEMORYEXTRACTOR_H
