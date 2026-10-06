// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLTODO_H
#define QSOCTOOLTODO_H

#include "agent/qsoctool.h"

class QSocWorkspaceFs;

#include <QList>

/**
 * @brief Structure representing a single todo item
 */
struct QSocTodoItem
{
    int     id = 0;
    QString title;
    QString description;
    QString priority = "medium";  /* high, medium, low */
    QString status   = "pending"; /* pending, in_progress, done */
};

/**
 * @brief Tool to list all todo items
 * @details The todo tools keep `.qsoc/todos.md` (and the `.qsoc/todos.hwm`
 *          id mark) in the workspace that @p fs names: the local project, or
 *          the remote workspace in remote mode.
 */
class QSocToolTodoList : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolTodoList(QObject *parent = nullptr, QSocWorkspaceFs *fs = nullptr);
    ~QSocToolTodoList() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    QSocWorkspaceFs *fs = nullptr;

    QString formatTodoList(const QList<QSocTodoItem> &todos) const;
};

/**
 * @brief Tool to add a new todo item
 */
class QSocToolTodoAdd : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolTodoAdd(QObject *parent = nullptr, QSocWorkspaceFs *fs = nullptr);
    ~QSocToolTodoAdd() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocWorkspaceFs *fs = nullptr;
};

/**
 * @brief Tool to update a todo item's status
 */
class QSocToolTodoUpdate : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolTodoUpdate(QObject *parent = nullptr, QSocWorkspaceFs *fs = nullptr);
    ~QSocToolTodoUpdate() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocWorkspaceFs *fs = nullptr;
};

/**
 * @brief Tool to delete a todo item
 */
class QSocToolTodoDelete : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolTodoDelete(QObject *parent = nullptr, QSocWorkspaceFs *fs = nullptr);
    ~QSocToolTodoDelete() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocWorkspaceFs *fs = nullptr;
};

#endif // QSOCTOOLTODO_H
