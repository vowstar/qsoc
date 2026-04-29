// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTASKREGISTRY_H
#define QSOCTASKREGISTRY_H

#include "agent/qsoctasksource.h"

#include <QList>
#include <QObject>
#include <QString>

/**
 * @brief Aggregates multiple QSocTaskSource instances for the task overlay.
 * @details The overlay holds one registry; the registry holds N sources.
 *          listAll() unions every source's rows and tags them with the
 *          source's sourceTag() so the overlay can route tailFor /
 *          killTask back to the right source. Rows are sorted by status
 *          (Running first) then startedAtMs descending.
 *
 *          Sources do not own each other; the registry does not own
 *          sources either — both are owned by QSocCliWorker (or a
 *          future task-router class) which controls lifetime.
 */
class QSocTaskRegistry : public QObject
{
    Q_OBJECT

public:
    /** @brief A row plus the source tag it came from. */
    struct TaggedRow
    {
        QString       sourceTag;
        QSocTask::Row row;
    };

    explicit QSocTaskRegistry(QObject *parent = nullptr);
    ~QSocTaskRegistry() override = default;

    /**
     * @brief Add a source. The registry connects tasksChanged ->
     *        anySourceChanged for fanout. Caller retains ownership.
     */
    void registerSource(QSocTaskSource *src);

    /**
     * @brief Snapshot of all rows from all sources, sorted.
     * @details Sort order: Running > Stuck > Pending > Idle > others;
     *          within a status bucket, newer (larger startedAtMs) first.
     */
    QList<TaggedRow> listAll() const;

    /** @brief Total active rows across sources. */
    int activeCount() const;

    /**
     * @brief Tail content for (tag, id); empty if tag not registered or
     *        task missing.
     */
    QString tailFor(const QString &tag, const QString &id, int maxBytes) const;

    /** @brief Kill the (tag, id) task. False if tag unknown or kill failed. */
    bool killTask(const QString &tag, const QString &id);

signals:
    /** @brief Any underlying source emitted tasksChanged. */
    void anySourceChanged();

private:
    QList<QSocTaskSource *> sources_;
};

#endif /* QSOCTASKREGISTRY_H */
