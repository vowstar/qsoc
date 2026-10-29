// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLTODO_H
#define QSOCTOOLTODO_H

#include "agent/qsoctodomodel.h"
#include "agent/qsoctool.h"

class QSocWorkspaceFs;

class QSocToolTodo : public QSocTool
{
public:
    QString getName() const override { return operation_; }
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return operation_ == "todo_list"; }

    QSocTodoModel *model() const;

protected:
    QSocToolTodo(QObject *parent, QSocWorkspaceFs *fs, const QString &operation);

private:
    QSocWorkspaceFs *fs_;
    QString          operation_;
};

class QSocToolTodoList : public QSocToolTodo
{
    Q_OBJECT
public:
    explicit QSocToolTodoList(QObject *parent = nullptr, QSocWorkspaceFs *fs = nullptr)
        : QSocToolTodo(parent, fs, QStringLiteral("todo_list"))
    {}
};

class QSocToolTodoAdd : public QSocToolTodo
{
    Q_OBJECT
public:
    explicit QSocToolTodoAdd(QObject *parent = nullptr, QSocWorkspaceFs *fs = nullptr)
        : QSocToolTodo(parent, fs, QStringLiteral("todo_add"))
    {}
};

class QSocToolTodoUpdate : public QSocToolTodo
{
    Q_OBJECT
public:
    explicit QSocToolTodoUpdate(QObject *parent = nullptr, QSocWorkspaceFs *fs = nullptr)
        : QSocToolTodo(parent, fs, QStringLiteral("todo_update"))
    {}
};

class QSocToolTodoDelete : public QSocToolTodo
{
    Q_OBJECT
public:
    explicit QSocToolTodoDelete(QObject *parent = nullptr, QSocWorkspaceFs *fs = nullptr)
        : QSocToolTodo(parent, fs, QStringLiteral("todo_delete"))
    {}
};

#endif
