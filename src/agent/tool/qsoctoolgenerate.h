// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLGENERATE_H
#define QSOCTOOLGENERATE_H

#include "agent/qsoctool.h"
#include "common/qsocgeneratemanager.h"

/**
 * @brief Tool to generate Verilog RTL from netlist
 */
class QSocToolGenerateVerilog : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolGenerateVerilog(
        QObject *parent = nullptr, QSocGenerateManager *generateManager = nullptr);
    ~QSocToolGenerateVerilog() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setGenerateManager(QSocGenerateManager *generateManager);

private:
    QSocGenerateManager *generateManager = nullptr;
};

/**
 * @brief Tool to render Jinja2 templates
 */
class QSocToolGenerateTemplate : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolGenerateTemplate(
        QObject *parent = nullptr, QSocGenerateManager *generateManager = nullptr);
    ~QSocToolGenerateTemplate() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setGenerateManager(QSocGenerateManager *generateManager);

private:
    QSocGenerateManager *generateManager = nullptr;
};

#endif // QSOCTOOLGENERATE_H
