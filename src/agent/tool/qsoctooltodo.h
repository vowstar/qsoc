// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLTODO_H
#define QSOCTOOLTODO_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

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
 */
class QSocToolTodoList : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolTodoList(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolTodoList() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;

    QString             todoFilePath() const;
    QList<QSocTodoItem> loadTodos() const;
    QString             formatTodoList(const QList<QSocTodoItem> &todos) const;
};

/**
 * @brief Tool to add a new todo item
 */
class QSocToolTodoAdd : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolTodoAdd(QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolTodoAdd() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;

    QString             todoFilePath() const;
    QList<QSocTodoItem> loadTodos() const;
    bool                saveTodos(const QList<QSocTodoItem> &todos) const;
};

/**
 * @brief Tool to update a todo item's status
 */
class QSocToolTodoUpdate : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolTodoUpdate(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolTodoUpdate() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;

    QString             todoFilePath() const;
    QList<QSocTodoItem> loadTodos() const;
    bool                saveTodos(const QList<QSocTodoItem> &todos) const;
};

/**
 * @brief Tool to delete a todo item
 */
class QSocToolTodoDelete : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolTodoDelete(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolTodoDelete() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;

    QString             todoFilePath() const;
    QList<QSocTodoItem> loadTodos() const;
    bool                saveTodos(const QList<QSocTodoItem> &todos) const;
};

#endif // QSOCTOOLTODO_H
