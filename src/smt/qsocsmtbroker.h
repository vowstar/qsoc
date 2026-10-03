// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSMTBROKER_H
#define QSOCSMTBROKER_H

#include "smt/qsocmemorybudget.h"

#include <functional>
#include <memory>
#include <stop_token>
#include <QJsonObject>
#include <QObject>

class QSocSmtBroker : public QObject
{
    Q_OBJECT

public:
    using Solver  = std::function<QJsonObject(const QJsonObject &, std::stop_token)>;
    using Sampler = std::function<QSocMemoryBudget::Snapshot()>;
    using Reply   = std::function<void(const QJsonObject &)>;

    explicit QSocSmtBroker(
        QObject                 *parent       = nullptr,
        Solver                   solver       = {},
        int                      queueWaitMs  = 120000,
        QSocMemoryBudget::Policy memoryPolicy = {},
        Sampler                  sampler      = {});
    ~QSocSmtBroker() override;

    quint64     submit(quint64 owner, const QJsonObject &request, Reply reply);
    bool        cancel(quint64 task);
    void        removeOwner(quint64 owner);
    void        shutdown();
    int         activeCount() const;
    int         queuedCount() const;
    QJsonObject resourceStatus() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

#endif
