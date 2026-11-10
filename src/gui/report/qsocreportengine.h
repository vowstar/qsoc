// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREPORTENGINE_H
#define QSOCREPORTENGINE_H

#include <memory>
#include <QJsonObject>
#include <QString>

namespace QSocReportLayout {
class Engine
{
public:
    Engine();
    ~Engine();
    QJsonObject layout(const QJsonObject &graph, QString &error);

private:
    class Runtime;
    std::unique_ptr<Runtime> runtime;
};
} // namespace QSocReportLayout

#endif
