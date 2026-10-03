// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#ifndef QSOCAGENTTASKMODEL_H
#define QSOCAGENTTASKMODEL_H
#include "common/qsoctaskregistry.h"
#include <functional>
#include <QJsonObject>

// Presentation model: rows are snapshots received over the socket; actions
// are RPC callbacks. No task executor or agent is hosted in the frontend.
class QSocAgentTaskModel : public QSocTaskRegistry
{
public:
    using Request = std::function<QJsonObject(const QString &, const QJsonObject &)>;
    explicit QSocAgentTaskModel(Request request)
        : request_(std::move(request))
    {}
    void               refresh();
    QList<TaggedRow>   listAll() const override { return rows_; }
    int                activeCount() const override;
    QString            tailFor(const QString &tag, const QString &id, int maxBytes) const override;
    bool               killTask(const QString &tag, const QString &id) override;
    QSocTask::Estimate estimateFor(const QString &tag, const QString &id) const override;

private:
    Request          request_;
    QList<TaggedRow> rows_;
    bool             refreshing_ = false;
};
#endif
