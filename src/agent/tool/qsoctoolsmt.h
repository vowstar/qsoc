// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSMT_H
#define QSOCTOOLSMT_H

#include "agent/qsoctool.h"

#include <map>
#include <memory>
#include <QJsonObject>

class QSocToolSmt final : public QSocTool
{
    Q_OBJECT
public:
    explicit QSocToolSmt(QObject *parent = nullptr, QString executable = {});
    ~QSocToolSmt() override;
    static bool supported();
    QString     getName() const override;
    QString     getDescription() const override;
    json        getParametersSchema() const override;
    QString     execute(const json &arguments) override;
    bool        isReadOnly() const override { return true; }
    bool        supportsDeferred() const override { return true; }

private:
    struct Job;
    QString                                 finishImmediately(const QJsonObject &result);
    void                                    completeJob(quint64 id, QJsonObject result);
    QString                                 executable_;
    quint64                                 nextId_ = 0;
    std::map<quint64, std::unique_ptr<Job>> jobs_;
};

#endif
