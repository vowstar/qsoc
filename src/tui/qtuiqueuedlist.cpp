// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiqueuedlist.h"

int QTuiQueuedList::lineCount() const
{
    return qMin(static_cast<int>(requests.size()), MAX_VISIBLE);
}

void QTuiQueuedList::render(QTuiScreen &screen, int startY, int width)
{
    int startIdx = qMax(0, static_cast<int>(requests.size()) - MAX_VISIBLE);
    int row      = 0;

    for (int idx = startIdx; idx < requests.size(); idx++) {
        QString line = "> " + requests[idx];
        screen.putString(0, startY + row, line.left(width), false, true); /* dim */
        row++;
    }
}

void QTuiQueuedList::addRequest(const QString &text)
{
    requests.append(text);
}

void QTuiQueuedList::removeRequest(const QString &text)
{
    requests.removeOne(text);
}

void QTuiQueuedList::clearAll()
{
    requests.clear();
}
