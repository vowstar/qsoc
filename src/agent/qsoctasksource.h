// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTASKSOURCE_H
#define QSOCTASKSOURCE_H

#include <QList>
#include <QObject>
#include <QString>

/**
 * @brief Polymorphic background-task model for the task overlay.
 * @details Each subsystem (loop scheduler, background bash, future
 *          sub-agents) implements QSocTaskSource and registers itself
 *          with QSocTaskRegistry. The overlay does not know about
 *          concrete types; it asks sources for rows, tail content, and
 *          kill action. Adding a fourth task type is a one-file change
 *          (new source class) plus one registration line.
 */
namespace QSocTask {

enum class Kind {
    Loop,           /* QSocLoopScheduler job */
    BackgroundBash, /* QSocToolShellBash::activeProcesses entry */
    SubAgent,       /* future: in-process or remote sub-agent */
};

enum class Status {
    Running,   /* actively executing */
    Pending,   /* scheduled, waiting for next fire */
    Idle,      /* alive but not currently doing work */
    Stuck,     /* watchdog flagged */
    Completed, /* finished cleanly (transient, removed soon) */
    Failed,    /* finished with error (transient) */
};

/**
 * @brief Compact row for list rendering.
 * @details `id` must be stable for the lifetime of the task; the overlay
 *          uses `(sourceTag, id)` as the selection key.
 */
struct Row
{
    QString id;      /* stable; e.g. 8-hex for loop, "#N" for bash */
    QString label;   /* short title for the cell */
    QString summary; /* one-liner detail string */
    Kind    kind;
    Status  status;
    qint64  startedAtMs; /* 0 = never started */
    bool    canKill;
};

} /* namespace QSocTask */

/**
 * @brief Abstract source of background tasks.
 * @details Implementations adapt a concrete subsystem (scheduler, bash
 *          tool) to the polymorphic task model. They must emit
 *          tasksChanged() whenever listTasks() output would differ in
 *          shape (add / remove / status change). Internal mutations
 *          that don't affect the surface (e.g. recurring lastFiredAt
 *          bumps) should NOT emit.
 */
class QSocTaskSource : public QObject
{
    Q_OBJECT

public:
    explicit QSocTaskSource(QObject *parent = nullptr)
        : QObject(parent)
    {}
    ~QSocTaskSource() override = default;

    /** @brief Stable short tag identifying this source ("loop"/"bg"/"agent"). */
    virtual QString sourceTag() const = 0;

    /** @brief Snapshot of all tasks visible from this source. */
    virtual QList<QSocTask::Row> listTasks() const = 0;

    /**
     * @brief Tail content for the detail view; truncated to maxBytes.
     * @details Sources free to choose a domain-appropriate body:
     *          bash returns last N bytes of stdout; loop returns
     *          cron + prompt + last-fire metadata.
     *          Returns empty string when the task no longer exists.
     */
    virtual QString tailFor(const QString &id, int maxBytes) const = 0;

    /**
     * @brief Best-effort terminate; returns true on success.
     * @details The overlay does not gate by canKill — it always calls
     *          this when the user presses x. Sources should handle the
     *          "already gone" case gracefully (return false).
     */
    virtual bool killTask(const QString &id) = 0;

signals:
    /** @brief Shape of listTasks() may have changed; consumers should refresh. */
    void tasksChanged();
};

#endif /* QSOCTASKSOURCE_H */
