// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuitodolist.h"

#include <algorithm>

int QTuiTodoList::lineCount() const
{
    if (!visible || items.isEmpty()) {
        return 0;
    }
    return qMin(static_cast<int>(items.size()), MAX_VISIBLE);
}

void QTuiTodoList::render(QTuiScreen &screen, int startY, int width)
{
    /* Build display list: active first, then in_progress, then pending, then done.
     * Within each group, most recent (highest index) first. */
    QList<int> displayOrder;

    /* Active item always first */
    for (int idx = items.size() - 1; idx >= 0; idx--) {
        if (items[idx].id == activeTodoId) {
            displayOrder.append(idx);
        }
    }
    /* In-progress items */
    for (int idx = items.size() - 1; idx >= 0; idx--) {
        if (items[idx].status == "in_progress" && items[idx].id != activeTodoId) {
            displayOrder.append(idx);
        }
    }
    /* Pending items */
    for (int idx = items.size() - 1; idx >= 0; idx--) {
        if (items[idx].status == "pending" && items[idx].id != activeTodoId) {
            displayOrder.append(idx);
        }
    }
    /* Done items (most recent first) */
    for (int idx = items.size() - 1; idx >= 0; idx--) {
        if (items[idx].status == "done") {
            displayOrder.append(idx);
        }
    }

    int row   = 0;
    int limit = qMin(static_cast<int>(displayOrder.size()), MAX_VISIBLE);

    for (int orderIdx = 0; orderIdx < limit; orderIdx++) {
        const TodoItem &item = items[displayOrder[orderIdx]];
        QString         checkbox;

        if (item.status == "done") {
            checkbox = "[x]";
        } else if (item.id == activeTodoId) {
            checkbox = (animFrame % 10 < 5) ? "[*]" : "[ ]";
        } else {
            checkbox = "[ ]";
        }

        QString line = QString("%1 %2 (%3)").arg(checkbox, item.title, item.priority);
        screen.putString(0, startY + row, line.left(width));
        row++;
    }
}

void QTuiTodoList::setItems(const QList<TodoItem> &newItems)
{
    items = newItems;
}

void QTuiTodoList::addItem(const TodoItem &item)
{
    for (auto &existing : items) {
        if (existing.id == item.id) {
            existing = item;
            return;
        }
    }
    items.append(item);
}

void QTuiTodoList::updateStatus(int todoId, const QString &newStatus)
{
    for (auto &item : items) {
        if (item.id == todoId) {
            item.status = newStatus;
            if (newStatus == "done") {
                QElapsedTimer timer;
                timer.start();
                completionTimers[todoId] = timer;
            } else {
                completionTimers.remove(todoId);
            }
            return;
        }
    }
}

void QTuiTodoList::setActive(int todoId)
{
    activeTodoId = todoId;
}

void QTuiTodoList::clearActive()
{
    activeTodoId = -1;
}

QString QTuiTodoList::getTitle(int todoId) const
{
    for (const auto &item : items) {
        if (item.id == todoId) {
            return item.title;
        }
    }
    return {};
}

void QTuiTodoList::clearDone()
{
    items.erase(
        std::remove_if(
            items.begin(), items.end(), [](const TodoItem &item) { return item.status == "done"; }),
        items.end());
}

void QTuiTodoList::clearAll()
{
    items.clear();
    activeTodoId = -1;
}

void QTuiTodoList::removeItem(int todoId)
{
    for (int idx = 0; idx < items.size(); idx++) {
        if (items[idx].id == todoId) {
            items.removeAt(idx);
            return;
        }
    }
}

void QTuiTodoList::tick()
{
    animFrame++;

    /* Expire done items after TTL */
    QList<int> expired;
    for (auto iter = completionTimers.begin(); iter != completionTimers.end(); ++iter) {
        if (iter.value().elapsed() > DONE_TTL_MS) {
            expired.append(iter.key());
        }
    }
    for (int todoId : expired) {
        completionTimers.remove(todoId);
        removeItem(todoId);
    }
}
