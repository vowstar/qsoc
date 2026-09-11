// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTASKFORECAST_H
#define QSOCTASKFORECAST_H

#include "agent/qsoctaskregistry.h"

#include <nlohmann/json.hpp>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QTimer>

class QSocAgent;
class QLLMService;

class QSocTaskForecast : public QObject
{
    Q_OBJECT
public:
    explicit QSocTaskForecast(
        QSocTaskRegistry *registry, QSocAgent *agent, QObject *parent = nullptr);
    ~QSocTaskForecast() override { invalidate(); }
    void        setEnabled(bool enabled);
    void        refresh();
    static bool parseEstimate(const nlohmann::json &value, QSocTask::Estimate *estimate);

private:
    struct Snapshot
    {
        QString        tag;
        QString        id;
        qint64         startedAt = 0;
        quint64        revision  = 0;
        nlohmann::json evidence;
    };
    void schedule();
    void dispatch();
    void finish(bool success);
    void invalidate();
    bool bindingMatches() const;

    QPointer<QSocTaskRegistry> registry_;
    QPointer<QSocAgent>        agent_;
    QPointer<QLLMService>      llm_;
    QTimer                     debounce_;
    QMap<QString, Snapshot>    latest_;
    QMap<QString, Snapshot>    pending_;
    QMap<QString, Snapshot>    sent_;
    QSet<QString>              dirty_;
    QString                    response_;
    QString                    binding_;
    quint64                    revision_ = 0;
    bool                       enabled_  = true;
};

#endif
