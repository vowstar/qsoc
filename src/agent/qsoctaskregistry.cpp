// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctaskregistry.h"

#include <algorithm>
#include <QTimer>

namespace {

int statusRank(QSocTask::Status status)
{
    switch (status) {
    case QSocTask::Status::Running:
        return 0;
    case QSocTask::Status::Stuck:
        return 1;
    case QSocTask::Status::Pending:
        return 2;
    case QSocTask::Status::Idle:
        return 3;
    case QSocTask::Status::Completed:
        return 4;
    case QSocTask::Status::Aborted:
        return 5;
    case QSocTask::Status::Failed:
        return 6;
    }
    return 7;
}

} /* namespace */

QSocTaskRegistry::QSocTaskRegistry(QObject *parent)
    : QObject(parent)
{}

void QSocTaskRegistry::registerSource(QSocTaskSource *src)
{
    if (src == nullptr || sources_.contains(src))
        return;
    sources_.append(src);
    connect(src, &QSocTaskSource::tasksChanged, this, &QSocTaskRegistry::anySourceChanged);
    connect(src, &QSocTaskSource::taskEvidenceChanged, this, [this, src](const QString &id) {
        emit evidenceChanged(src->sourceTag(), id);
    });
    connect(src, &QObject::destroyed, this, [this, src]() {
        sources_.removeAll(src);
        emit anySourceChanged();
    });
}

QList<QSocTaskRegistry::TaggedRow> QSocTaskRegistry::listAll() const
{
    QList<TaggedRow> out;
    for (auto *src : sources_) {
        const QString tag = src->sourceTag();
        for (const auto &row : src->listTasks()) {
            out.append({tag, row, estimateFor(tag, row.id)});
        }
    }
    std::sort(out.begin(), out.end(), [](const TaggedRow &a, const TaggedRow &b) {
        const int ra = statusRank(a.row.status);
        const int rb = statusRank(b.row.status);
        if (ra != rb)
            return ra < rb;
        return a.row.startedAtMs > b.row.startedAtMs;
    });
    return out;
}

int QSocTaskRegistry::activeCount() const
{
    int total = 0;
    for (auto *src : sources_) {
        for (const auto &row : src->listTasks()) {
            if (!QSocTask::isTerminal(row.status))
                ++total;
        }
    }
    return total;
}

QString QSocTaskRegistry::tailFor(const QString &tag, const QString &id, int maxBytes) const
{
    for (auto *src : sources_) {
        if (src->sourceTag() == tag)
            return src->tailFor(id, maxBytes);
    }
    return QString();
}

bool QSocTaskRegistry::killTask(const QString &tag, const QString &id)
{
    for (auto *src : sources_) {
        if (src->sourceTag() == tag)
            return src->killTask(id);
    }
    return false;
}

void QSocTaskRegistry::setEstimate(
    const QString &tag, const QString &id, const QSocTask::Estimate &estimate)
{
    const auto key = qMakePair(tag, id);
    if (estimate.summary.isEmpty())
        estimates_.remove(key);
    else
        estimates_[key] = estimate;
    notifyEstimatesChanged();
}

void QSocTaskRegistry::clearEstimates()
{
    estimates_.clear();
    notifyEstimatesChanged();
}

void QSocTaskRegistry::notifyEstimatesChanged()
{
    if (estimateNotificationPending_)
        return;
    estimateNotificationPending_ = true;
    QTimer::singleShot(0, this, [this]() {
        estimateNotificationPending_ = false;
        emit estimatesChanged();
    });
}

QSocTask::Estimate QSocTaskRegistry::estimateFor(const QString &tag, const QString &id) const
{
    return estimates_.value(qMakePair(tag, id));
}
