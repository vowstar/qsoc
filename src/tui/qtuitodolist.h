// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUITODOLIST_H
#define QTUITODOLIST_H

#include "tui/qtuiwidget.h"

#include <QElapsedTimer>
#include <QList>
#include <QMap>
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

    /* Remove all completed (done) TODOs */
    void clearDone();
    /* Remove a specific TODO by id */
    void removeItem(int todoId);
    /* Clear all TODOs */
    void clearAll();

    /* Show/hide the widget without clearing items (used by Ctrl+T hotkey) */
    void setVisible(bool vis) { visible = vis; }
    bool isVisible() const { return visible; }

    static constexpr int MAX_VISIBLE = 5;

private:
    QList<TodoItem>          items;
    int                      activeTodoId = -1;
    int                      animFrame    = 0;
    bool                     visible      = true;
    QMap<int, QElapsedTimer> completionTimers;    /* id → time since marked done */
    static constexpr int     DONE_TTL_MS = 30000; /* 30 seconds */
};

#endif // QTUITODOLIST_H
