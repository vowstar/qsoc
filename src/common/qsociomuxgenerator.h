// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCIOMUXGENERATOR_H
#define QSOCIOMUXGENERATOR_H

#include "common/qsocmmiogenerator.h"

#include <optional>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <yaml-cpp/yaml.h>

struct QSocModuleDefinition;

enum class QSocIomuxRole { InputValue, InputEnable, OutputValue, OutputEnable };

struct QSocIomuxEndpointPlan
{
    QString                link;
    std::optional<quint32> bit;
    bool                   invert = false;
    std::optional<quint8>  constant;

    bool operator==(const QSocIomuxEndpointPlan &) const = default;
};

struct QSocIomuxRoutePlan
{
    quint32               pin  = 0;
    quint32               slot = 0;
    QString               function;
    QString               signal;
    QSocIomuxEndpointPlan inputValue;
    QSocIomuxEndpointPlan inputEnable;
    QSocIomuxEndpointPlan outputValue;
    QSocIomuxEndpointPlan outputEnable;

    bool operator==(const QSocIomuxRoutePlan &) const = default;
};

struct QSocIomuxIntegrationPlan
{
    QString instance;
    QString clock;
    QString reset;
    QString control;
    QString padInputValue;
    QString padInputEnable;
    QString padOutputValue;
    QString padOutputEnable;

    bool operator==(const QSocIomuxIntegrationPlan &) const = default;
};

struct QSocIomuxPlan
{
    QString                   moduleName;
    quint32                   pinCount = 0;
    quint32                   hsSlots  = 0;
    QList<QSocIomuxRoutePlan> routes;
    QSocIomuxIntegrationPlan  integration;
    QSocMmioPlan              mmio;

    bool operator==(const QSocIomuxPlan &) const = default;
};

class QSocIomuxGenerator
{
public:
    static bool        isIomux(const QSocModuleDefinition &definition);
    static YAML::Node  createDraftGenerator();
    static QStringList validate(const QSocModuleDefinition &definition);
    static bool        buildPlan(
        const QSocModuleDefinition &definition, QSocIomuxPlan *plan, QStringList *errors = nullptr);
    static QString generateCoreVerilog(const QSocIomuxPlan &plan);
    static QString generateConnVerilog(const QSocIomuxPlan &plan);
    static QString generateReport(const QSocIomuxPlan &plan);
};

#endif // QSOCIOMUXGENERATOR_H
