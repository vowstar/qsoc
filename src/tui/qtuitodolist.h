// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUITODOLIST_H
#define QTUITODOLIST_H

#include "tui/qtuiwidget.h"

#include <QList>
#include <QString>

/**
 * @brief TODO list widget with animated checkboxes
 */
class QTuiTodoList : public QTuiWidget
{
public:
    struct TodoItem
    {
        int     id;
        QString title;
        QString priority;
        QString status; /* "done", "pending", "in_progress" */
    };

    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    void    setItems(const QList<TodoItem> &items);
    void    addItem(const TodoItem &item);
    void    updateStatus(int todoId, const QString &newStatus);
    void    setActive(int todoId);
    void    clearActive();
    QString getTitle(int todoId) const;
    void    tick();

    static constexpr int MAX_VISIBLE = 5;

private:
    QList<TodoItem> items;
    int             activeTodoId = -1;
    int             animFrame    = 0;
};

#endif // QTUITODOLIST_H
