// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTODOMODEL_H
#define QSOCTODOMODEL_H

#include "common/qsoctasksource.h"
#include <nlohmann/json.hpp>
#include <thread>

class QSocWorkspaceFs;

struct QSocTodoItem
{
    int        id = 0;
    QString    title;
    QString    description;
    QString    priority = "medium";
    QString    status   = "pending";
    QString    planId;
    QList<int> dependsOn;
};

class QSocTodoModel : public QSocTaskSource
{
    Q_OBJECT
public:
    explicit QSocTodoModel(QSocWorkspaceFs *fs);
    ~QSocTodoModel() override;
    QString execute(
        const QString        &operation,
        const nlohmann::json &arguments,
        const QString        &scope = {},
        bool                  child = false);
    bool                 refresh(QString *error, bool force = false);
    void                 setScope(const QString &scope, bool active = true);
    QList<QSocTodoItem>  items() const { return items_; }
    QString              sourceTag() const override { return QStringLiteral("todo"); }
    QList<QSocTask::Row> listTasks() const override;
    QString              tailFor(const QString &id, int maxBytes) const override;
    bool                 killTask(const QString &) override { return false; }
    static QString       dependencyError(const QList<QSocTodoItem> &items);
    QString              dependencySmt() const;

private:
    void                checkDependencies();
    QSocWorkspaceFs    *fs_;
    QList<QSocTodoItem> items_;
    QString             root_;
    QString             scope_;
    QString             error_;
    QString             checkedGraph_;
    QString             dependencyCheck_;
    std::jthread        proofThread_;
    bool                checking_   = false;
    bool                loaded_     = false;
    bool                editing_    = false;
    bool                refreshing_ = false;
    bool                active_     = false;

    QString             currentPlan(const QString &scope) const;
    QList<QSocTodoItem> visibleItems() const;
    QString             save(const QList<QSocTodoItem> &items, const QString &plan);
};

#endif
