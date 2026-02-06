// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLMODULE_H
#define QSOCTOOLMODULE_H

#include "agent/qsoctool.h"
#include "common/qsocmodulemanager.h"

/**
 * @brief Tool to list modules
 */
class QSocToolModuleList : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolModuleList(
        QObject *parent = nullptr, QSocModuleManager *moduleManager = nullptr);
    ~QSocToolModuleList() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setModuleManager(QSocModuleManager *moduleManager);

private:
    QSocModuleManager *moduleManager = nullptr;
};

/**
 * @brief Tool to show module details
 */
class QSocToolModuleShow : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolModuleShow(
        QObject *parent = nullptr, QSocModuleManager *moduleManager = nullptr);
    ~QSocToolModuleShow() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setModuleManager(QSocModuleManager *moduleManager);

private:
    QSocModuleManager *moduleManager = nullptr;
};

/**
 * @brief Tool to import Verilog modules
 */
class QSocToolModuleImport : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolModuleImport(
        QObject *parent = nullptr, QSocModuleManager *moduleManager = nullptr);
    ~QSocToolModuleImport() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setModuleManager(QSocModuleManager *moduleManager);

private:
    QSocModuleManager *moduleManager = nullptr;
};

/**
 * @brief Tool to add bus interface to a module using LLM matching
 */
class QSocToolModuleBusAdd : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolModuleBusAdd(
        QObject *parent = nullptr, QSocModuleManager *moduleManager = nullptr);
    ~QSocToolModuleBusAdd() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setModuleManager(QSocModuleManager *moduleManager);

private:
    QSocModuleManager *moduleManager = nullptr;
};

#endif // QSOCTOOLMODULE_H
