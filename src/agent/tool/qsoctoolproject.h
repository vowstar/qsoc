// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLPROJECT_H
#define QSOCTOOLPROJECT_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

/**
 * @brief Tool to list projects
 */
class QSocToolProjectList : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolProjectList(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolProjectList() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;
};

/**
 * @brief Tool to show project details
 */
class QSocToolProjectShow : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolProjectShow(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolProjectShow() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;
};

/**
 * @brief Tool to create a project
 */
class QSocToolProjectCreate : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolProjectCreate(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolProjectCreate() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;
};

#endif // QSOCTOOLPROJECT_H
