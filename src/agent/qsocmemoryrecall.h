// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMEMORYRECALL_H
#define QSOCMEMORYRECALL_H

#include "agent/qsocmemorymanager.h"

#include <functional>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief Selective memory recall: rank topic-file headers against the
 *        current query and assemble a budget-capped reminder block.
 * @details Stateless apart from immutable config, so it is trivially
 *          unit-testable with a stubbed reader and canned selector JSON
 *          (no LLM, no Qt event loop). The agent owns the LLM round trip;
 *          this class only builds the selector prompt, parses its reply,
 *          and assembles the injected block.
 *
 *          Each turn the agent recomputes recall against the live query
 *          and injects the result as a non-persisted reminder message, so
 *          the static system-prompt prefix stays byte-stable and the
 *          provider prompt cache is preserved.
 */
class QSocMemoryRecall
{
public:
    struct Config
    {
        int maxFiles   = 5;     /* Max topic files selected per turn */
        int perFileCap = 4096;  /* Max bytes injected from one file */
        int turnBudget = 61440; /* Max cumulative bytes injected per turn */
    };

    explicit QSocMemoryRecall(Config config);

    /* Whether a query is worth recalling for: needs >=2 whitespace
     * tokens (a single word lacks context to rank against). */
    static bool queryIsRecallable(const QString &query);

    /* System-prompt text for the selector LLM call. */
    static QString selectorSystemPrompt();

    /* User-message text for the selector LLM call: query + a manifest of
     * candidate headers. Caller sends [system, user] and reads JSON
     * {"selected_memories":[name,...]} back. */
    QString buildSelectorPrompt(
        const QList<QSocMemoryManager::MemoryHeader> &headers, const QString &query) const;

    /* Parse the selector reply content into a list of selected names.
     * Tolerates code-fenced JSON, surrounding prose, or a bare array. */
    static QStringList parseSelection(const QString &responseContent);

    /* Strip leading YAML frontmatter from raw topic-file content. */
    static QString stripFrontmatter(const QString &content);

    /* Assemble the recall reminder block from the selected headers,
     * reading each file via @p reader. Applies per-file cap, per-turn
     * budget, and maxFiles. Returns empty when nothing surfaces. */
    QString assembleBlock(
        const QList<QSocMemoryManager::MemoryHeader>                            &selected,
        const std::function<QString(const QString &scope, const QString &name)> &reader) const;

    const Config &config() const { return config_; }

private:
    Config config_;

    /* Human-readable freshness phrase for an age in days. */
    static QString freshnessPhrase(int ageDays);
};

#endif // QSOCMEMORYRECALL_H
