// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLRESOURCES_H
#define QSOCTOOLRESOURCES_H

#include "agent/qsoctool.h"

#include <functional>
#include <QJsonObject>
#include <QStringList>

class QSocToolResources : public QSocTool
{
    Q_OBJECT
public:
    using Paths = std::function<QStringList()>;
    using Query = std::function<QJsonObject(const QStringList &)>;

    explicit QSocToolResources(QObject *parent, Paths paths = {}, Query query = {});
    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    Paths paths_;
    Query query_;
};

#endif /* QSOCTOOLRESOURCES_H */
