// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMINPUT_H
#define QSOCPRCMINPUT_H

#include "common/qsocmmiogenerator.h"

#include <optional>
#include <yaml-cpp/yaml.h>
#include <QMap>
#include <QStringList>

struct QSocPrcmSource
{
    QString file;
    QString path;
    int     line   = 0;
    int     column = 0;
};

struct QSocPrcmDiagnostic
{
    QString               code;
    QString               message;
    QList<QSocPrcmSource> source;
};

struct QSocPrcmFeedback
{
    QString signal;
    QString sampleClock;
};

struct QSocPrcmSupply
{
    bool             alwaysOn = false;
    QString          request;
    QSocPrcmFeedback valid;
};

struct QSocPrcmMode
{
    quint64 code      = 0;
    bool    power     = false;
    bool    clock     = false;
    bool    reset     = true;
    bool    isolation = true;
};

struct QSocPrcmClockBinding
{
    QString controller;
    QString target;
    QString stage;
};

struct QSocPrcmResetBinding
{
    QString controller;
    QString source;
    QString target;
};

struct QSocPrcmHandshake
{
    QString          request;
    QSocPrcmFeedback completion;
};

struct QSocPrcmTransition
{
    QString from;
    QString to;
};

struct QSocPrcmServiceUse
{
    QString     domain;
    QString     service;
    QStringList mode;
};

struct QSocPrcmDomain
{
    QString                           supply;
    QSocPrcmClockBinding              clock;
    QSocPrcmResetBinding              reset;
    QSocPrcmHandshake                 quiesce;
    QSocPrcmHandshake                 isolation;
    QString                           resetMode;
    QMap<QString, QSocPrcmMode>       mode;
    QList<QSocPrcmTransition>         transition;
    QMap<QString, QString>            service;
    QMap<QString, QSocPrcmServiceUse> require;
};

struct QSocPrcmDomainPolicy
{
    QStringList allow;
    QString     target;
};

struct QSocPrcmChipMode
{
    quint64                             code = 0;
    QMap<QString, QSocPrcmDomainPolicy> domain;
};

struct QSocPrcmInput
{
    QString                         clockController;
    QString                         clockInput;
    QString                         resetController;
    QString                         resetSource;
    QString                         supply;
    QSocMmioBus                     bus          = QSocMmioBus::Apb4;
    quint32                         dataWidth    = 32;
    quint32                         addressWidth = 32;
    QMap<QString, QSocPrcmSupply>   supplyTable;
    QMap<QString, QSocPrcmDomain>   domain;
    QString                         chipResetMode;
    QMap<QString, QSocPrcmChipMode> chipMode;
    QMap<QString, QSocPrcmSource>   source;
};

struct QSocPrcmParseResult
{
    std::optional<QSocPrcmInput> input;
    QList<QSocPrcmDiagnostic>    diagnostic;
};

class QSocPrcmParser
{
public:
    /* Parse declarations. Resource binding and semantic checks are separate. */
    static QSocPrcmParseResult parse(const YAML::Node &netlist, const QString &file);
};

#endif // QSOCPRCMINPUT_H
