// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCGOAL_H
#define QSOCGOAL_H

#include <cstdint>
#include <optional>

#include <QDateTime>
#include <QObject>
#include <QString>

/**
 * @brief Lifecycle state of a project goal.
 * @details Mirrors the codex thread-goal state machine. Active is the
 *          working state; Paused stops auto-continuation until the user
 *          flips back; BudgetLimited is reached automatically when the
 *          token budget is exhausted and asks the LLM to wrap up;
 *          Complete is the terminal success state and clears the goal.
 */
enum class QSocGoalStatus : std::uint8_t {
    Active,
    Paused,
    BudgetLimited,
    Complete,
};

/**
 * @brief One project goal record.
 * @details Lives in `<project>/.qsoc/goal.yml` as a single entry. The
 *          @c id is a stable UUID minted on creation so the append-only
 *          event log can correlate entries across successive goals.
 *          The @c objective field is free-form text fed back into the
 *          LLM each auto-continuation turn.
 */
struct QSocGoal
{
    QString        id;
    QString        objective;
    QSocGoalStatus status      = QSocGoalStatus::Active;
    int            tokenBudget = 0; /**< 0 = no budget enforced */
    int            tokensUsed  = 0;
    qint64         secondsUsed = 0;
    QDateTime      createdAt;
    QDateTime      updatedAt;
};

/**
 * @brief Map a status enum to its on-disk string form.
 */
QString qSocGoalStatusToString(QSocGoalStatus status);

/**
 * @brief Inverse of @ref qSocGoalStatusToString; returns nullopt on
 *        unknown strings so the loader can skip malformed YAML.
 */
std::optional<QSocGoalStatus> qSocGoalStatusFromString(const QString &raw);

/**
 * @brief Loader, writer, and event-log appender for the project goal.
 * @details Single goal per project at any time. Writes go through
 *          @c QSaveFile (atomic temp+rename) for the YAML state and
 *          O_APPEND-style append for the JSONL event log. All
 *          mutations emit @ref goalChanged so the system prompt
 *          injector and status line stay in sync.
 *
 *          The event log is best-effort; a transient log failure
 *          logs a warning but does not abort the mutation, because
 *          losing the audit trail is preferable to losing the live
 *          goal state.
 */
class QSocGoalCatalog : public QObject
{
    Q_OBJECT

public:
    explicit QSocGoalCatalog(QObject *parent = nullptr);

    /**
     * @brief Load the per-project goal file. Missing files load as
     *        "no active goal"; malformed YAML is logged and treated
     *        the same.
     */
    void load(const QString &projectDir);

    /**
     * @brief Project-scope YAML path. Empty when no project dir set.
     */
    QString projectFilePath() const;

    /**
     * @brief Append-only JSONL event log path. Empty when no project
     *        dir set.
     */
    QString logFilePath() const;

    /**
     * @brief Snapshot of the current goal. Empty optional when none.
     */
    std::optional<QSocGoal> current() const;

    /**
     * @brief Create a new goal. Fails when one is already active.
     * @param objective Free-form text the LLM should pursue.
     * @param tokenBudget 0 = no cap; positive value triggers a
     *        BudgetLimited transition once @c tokensUsed reaches it.
     */
    bool create(const QString &objective, int tokenBudget, QString *errorMessage = nullptr);

    /**
     * @brief Discard any current goal (logged as @c discarded), then
     *        create a fresh one with the new objective.
     */
    bool replace(const QString &newObjective, int tokenBudget, QString *errorMessage = nullptr);

    /**
     * @brief Clear the active goal. No-op when none is active.
     */
    bool clear(QString *errorMessage = nullptr);

    /**
     * @brief Flip the goal's status. Active <-> Paused and -> Complete
     *        are user-driven; -> BudgetLimited is reserved for the
     *        accounting helper.
     */
    bool setStatus(QSocGoalStatus newStatus, QString *errorMessage = nullptr);

    /**
     * @brief Mutate the in-flight goal's objective (e.g. `/goal <new>`
     *        when one is already active and the user confirmed
     *        replacement). Logs an @c objective_updated event.
     */
    bool updateObjective(const QString &newObjective, QString *errorMessage = nullptr);

    /**
     * @brief Bump usage counters and trip BudgetLimited when the new
     *        @c tokensUsed crosses the configured budget.
     * @return True when accounting succeeded, even if the goal
     *         transitioned to BudgetLimited as a side effect.
     */
    bool accountUsage(int tokensDelta, qint64 secondsDelta, QString *errorMessage = nullptr);

    /**
     * @brief Append a free-form @c continued log event with the
     *        triggering reason (e.g. "auto"). The catalog itself does
     *        not auto-continue; callers (QSocAgent) drive it.
     */
    void noteContinuation(const QString &reason);

    /**
     * @brief Set/replace the token budget on the active goal.
     */
    bool setTokenBudget(int newBudget, QString *errorMessage = nullptr);

signals:
    /**
     * @brief Fired after every successful state mutation. Listeners
     *        regenerate the status chip and the next-turn prompt.
     */
    void goalChanged();

private:
    bool writeYaml(QString *errorMessage);
    void appendLog(const QString &event, const QString &extraJsonFields = QString());

    std::optional<QSocGoal> current_;
    QString                 projectDir_;
};

#endif // QSOCGOAL_H
