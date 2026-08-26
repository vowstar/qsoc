// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOGENERATOR_H
#define QSOCMMIOGENERATOR_H

#include <optional>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <yaml-cpp/yaml.h>

struct QSocModuleDefinition;

enum class QSocMmioAccess { ReadWrite, ReadOnly };

struct QSocMmioFieldPlan
{
    QString                name;
    QString                description;
    quint32                lsb    = 0;
    quint32                width  = 1;
    QSocMmioAccess         access = QSocMmioAccess::ReadOnly;
    std::optional<quint64> resetValue;
    std::optional<quint64> constantValue;
    QString                inputPort;
    QString                outputPort;

    bool operator==(const QSocMmioFieldPlan &) const = default;
};

struct QSocMmioRegisterPlan
{
    QString                  name;
    QString                  description;
    quint64                  byteOffset = 0;
    QList<QSocMmioFieldPlan> fields;

    bool operator==(const QSocMmioRegisterPlan &) const = default;
};

struct QSocMmioPlan
{
    QString                     moduleName;
    quint32                     dataWidth    = 32;
    quint32                     addressWidth = 32;
    QList<QSocMmioRegisterPlan> registers;

    bool operator==(const QSocMmioPlan &) const = default;
};

class QSocMmioGenerator
{
public:
    static bool        isMmio(const QSocModuleDefinition &definition);
    static YAML::Node  createDraftGenerator();
    static QStringList validate(const QSocModuleDefinition &definition);
    static bool        buildPlan(
        const QSocModuleDefinition &definition, QSocMmioPlan *plan, QStringList *errors = nullptr);
    static bool generateVerilog(
        const QSocModuleDefinition &definition, QString *verilog, QStringList *errors = nullptr);
};

#endif // QSOCMMIOGENERATOR_H
