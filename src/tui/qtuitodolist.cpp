// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuitodolist.h"

#include <algorithm>

int QTuiTodoList::lineCount() const
{
    if (visible && !planRows_.isEmpty())
        return 2;
    if (!visible || items.isEmpty()) {
        return 0;
    }
    return qMin(static_cast<int>(items.size()), MAX_VISIBLE);
}

void QTuiTodoList::render(QTuiScreen &screen, int startY, int width)
{
    if (!planRows_.isEmpty()) {
        int     total = 0, done = 0;
        QString current, problem;
        for (const auto &row : planRows_) {
            if (row.id == "error") {
                problem = row.summary;
                continue;
            }
            ++total;
            if (row.status == QSocTask::Status::Completed)
                ++done;
            if (current.isEmpty() && row.status == QSocTask::Status::Running)
                current = row.label + QStringLiteral(" | ") + row.summary;
        }
        const QString summary = total
                                    ? QStringLiteral("Plan: %1/%2 items done").arg(done).arg(total)
                                    : QStringLiteral("Plan needs attention");
        screen.putString(
            0,
            startY,
            QTuiText::truncate(summary, width),
            false,
            false,
            false,
            problem.isEmpty() ? QTuiFgColor::Green : QTuiFgColor::Yellow);
        const QString detail
            = !problem.isEmpty() ? problem
              : current.isEmpty()
                  ? (done == total
                         ? QStringLiteral("All listed items done | Ctrl+B details")
                         : QStringLiteral(
                               "Ctrl+B tasks | Ready and waiting items remain in the plan"))
                  : current;
        screen.putString(
            0,
            startY + 1,
            QTuiText::truncate(detail, width),
            false,
            true,
            false,
            problem.isEmpty() ? QTuiFgColor::Default : QTuiFgColor::Red);
        return;
    }
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

void QTuiTodoList::setPlanRows(const QList<QSocTask::Row> &rows)
{
    planRows_.clear();
    bool planned = false;
    for (const auto &row : rows) {
        if (row.kind != QSocTask::Kind::Todo)
            continue;
        planRows_.append(row);
        planned |= !row.planId.isEmpty() || row.id == "error";
    }
    if (!planned)
        planRows_.clear();
}

void QTuiTodoList::setItems(const QList<TodoItem> &items)
{
    this->items = items;
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
    planRows_.clear();
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
