// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCONTEXTRESTORE_H
#define QSOCCONTEXTRESTORE_H

#include <cstdint>
#include <functional>
#include <optional>

#include <nlohmann/json.hpp>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

using json = nlohmann::json;

/**
 * @brief Bounded set of context "supplies" re-injected after a compaction.
 * @details When the agent compacts its conversation it swaps the message
 *          history for a short summary plus the most recent messages. The
 *          summary carries the narrative, but the concrete files, active
 *          skill rules, and any running background agents fall out of
 *          context. This struct holds the supplies to put back: the most
 *          recently read files (small ones re-inlined, large ones as a
 *          path-only pointer), the recently invoked skills, and the
 *          currently running sub-agents. The builder produces it; the
 *          agent appends `toMessages()` after the summary so the model
 *          sees them next turn; the CLI renders the display lines.
 */
struct QSocContextRestore
{
    enum class Mode : std::uint8_t { Read, Referenced };

    struct FileItem
    {
        QString displayPath;    /* path as shown and re-read */
        int     lines = 0;      /* line count for the "(N lines)" suffix */
        QString attachmentText; /* message body (content for Read, pointer for Referenced) */
        Mode    mode = Mode::Referenced;
    };

    struct SkillItem
    {
        QString name;
        QString attachmentText; /* "## <name>\n<body>" (body truncated) */
    };

    struct AgentItem
    {
        QString id;
        QString label;
        QString attachmentText; /* one-line status block */
    };

    QList<FileItem>  files;
    QList<SkillItem> skills;
    QList<AgentItem> agents;

    bool isEmpty() const { return files.isEmpty() && skills.isEmpty() && agents.isEmpty(); }

    /** @brief Display paths of the Read (content re-inlined) files. */
    QStringList readPaths() const;
    /** @brief Display paths of the Referenced (path-only) files. */
    QStringList referencedPaths() const;
    /** @brief Names of the restored skills. */
    QStringList skillNames() const;
    /** @brief Labels of the restored running agents. */
    QStringList agentLabels() const;
};

/**
 * @brief Pure builder for the post-compaction context restore.
 * @details Side-effect free: all I/O (file reads, skill-body reads, token
 *          estimation) is injected through `Inputs`, so the same builder
 *          serves local and remote sessions and is fully unit-testable.
 *          Budgets and counts come from `Inputs` (CLI passes the agent
 *          config values); the constants below are the defaults.
 */
class QSocContextRestoreBuilder
{
public:
    static constexpr int kMaxFilesToRestore = 5;
    static constexpr int kFileTokenBudget   = 50000;
    static constexpr int kMaxTokensPerFile  = 5000;
    static constexpr int kMaxTokensPerSkill = 5000;
    static constexpr int kSkillsTokenBudget = 25000;

    /** @brief Lightweight running-agent row (decoupled from the task source). */
    struct AgentRow
    {
        QString id;
        QString label;
        QString summary;
    };

    struct Inputs
    {
        bool enabled = true;

        /* File candidates, most-recent first; excluded paths are skipped. */
        QStringList   candidatePaths;
        QSet<QString> excludedPaths;
        /* Returns file content, or nullopt when unreadable (skipped). */
        std::function<std::optional<QString>(const QString &)> readFile;

        /* Skill names, most-recent first; body reader returns nullopt when
         * the skill body cannot be read (skipped). */
        QStringList                                            skillNames;
        std::function<std::optional<QString>(const QString &)> readSkill;

        /* Currently running background agents. */
        QList<AgentRow> agents;

        /* Token estimator (bind to QSocAgent::estimateTokens). */
        std::function<int(const QString &)> estimateTokens;

        /* Budgets / counts (default to the constants above). */
        int maxFiles          = kMaxFilesToRestore;
        int fileBudget        = kFileTokenBudget;
        int maxTokensPerFile  = kMaxTokensPerFile;
        int maxTokensPerSkill = kMaxTokensPerSkill;
        int skillsBudget      = kSkillsTokenBudget;
    };

    /** @brief Build the restore payload from injected inputs. */
    static QSocContextRestore build(const Inputs &inputs);

    /**
     * @brief Convert a restore payload into messages to append after the
     *        post-compaction summary (one user message per file, one for
     *        all skills, one for all running agents).
     */
    static json toMessages(const QSocContextRestore &restore);

    /**
     * @brief Truncate @p text to at most @p maxTokens using the injected
     *        estimator (truncate-then-remeasure). Appends a marker when cut.
     */
    static QString truncateToTokens(
        const QString &text, int maxTokens, const std::function<int(const QString &)> &estimate);
};

#endif // QSOCCONTEXTRESTORE_H
