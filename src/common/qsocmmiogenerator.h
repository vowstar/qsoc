// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMMIOGENERATOR_H
#define QSOCMMIOGENERATOR_H

#include <QString>
#include <QStringList>

#include <yaml-cpp/yaml.h>

struct QSocModuleDefinition;

class QSocMmioGenerator
{
public:
    static bool        isMmio(const QSocModuleDefinition &definition);
    static YAML::Node  createDraftGenerator();
    static QStringList validate(const QSocModuleDefinition &definition);
    static bool        generateVerilog(
        const QSocModuleDefinition &definition, QString *verilog, QStringList *errors = nullptr);
};

#endif // QSOCMMIOGENERATOR_H
