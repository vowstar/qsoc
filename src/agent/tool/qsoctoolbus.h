// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLBUS_H
#define QSOCTOOLBUS_H

#include "agent/qsoctool.h"
#include "common/qsocbusmanager.h"

/**
 * @brief Tool to list buses
 */
class QSocToolBusList : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolBusList(QObject *parent = nullptr, QSocBusManager *busManager = nullptr);
    ~QSocToolBusList() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setBusManager(QSocBusManager *busManager);

private:
    QSocBusManager *busManager = nullptr;
};

/**
 * @brief Tool to show bus details
 */
class QSocToolBusShow : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolBusShow(QObject *parent = nullptr, QSocBusManager *busManager = nullptr);
    ~QSocToolBusShow() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setBusManager(QSocBusManager *busManager);

private:
    QSocBusManager *busManager = nullptr;
};

/**
 * @brief Tool to import bus definitions from CSV files
 */
class QSocToolBusImport : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolBusImport(QObject *parent = nullptr, QSocBusManager *busManager = nullptr);
    ~QSocToolBusImport() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setBusManager(QSocBusManager *busManager);

private:
    QSocBusManager *busManager = nullptr;
};

#endif // QSOCTOOLBUS_H
