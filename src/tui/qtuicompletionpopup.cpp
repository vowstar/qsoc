// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuicompletionpopup.h"

int QTuiCompletionPopup::lineCount() const
{
    if (!visible || items.isEmpty()) {
        return 0;
    }
    int visibleItems = qMin(static_cast<int>(items.size()), MAX_VISIBLE_ITEMS);
    return visibleItems + 2; /* + top and bottom borders */
}

void QTuiCompletionPopup::render(QTuiScreen &screen, int startY, int width)
{
    if (!visible || items.isEmpty()) {
        return;
    }

    int visibleItems = qMin(static_cast<int>(items.size()), MAX_VISIBLE_ITEMS);
    int boxWidth     = width;

    /* Compute required box width from longest visible item */
    int maxItemW = QTuiText::visualWidth(title) + 6;
    for (int idx = 0; idx < visibleItems; idx++) {
        int visIdx = viewStart + idx;
        if (visIdx >= items.size()) {
            break;
        }
        int itemW = QTuiText::visualWidth(items[visIdx]) + 6;
        maxItemW  = qMax(maxItemW, itemW);
    }
    boxWidth = qMin(boxWidth, qMax(maxItemW, 20));

    /* Top border with title */
    QString topLine = QStringLiteral("+- ") + title + QLatin1Char(' ');
    while (QTuiText::visualWidth(topLine) < boxWidth - 1) {
        topLine += QLatin1Char('-');
    }
    topLine += QLatin1Char('+');
    screen.putString(0, startY, topLine.left(width), false, true);

    /* Items */
    for (int row = 0; row < visibleItems; row++) {
        int visIdx = viewStart + row;
        if (visIdx >= items.size()) {
            break;
        }
        QString line = QStringLiteral("| ") + items[visIdx];
        while (QTuiText::visualWidth(line) < boxWidth - 1) {
            line += QLatin1Char(' ');
        }
        line += QLatin1Char('|');

        bool inv = (visIdx == highlight) && colorEnabled;
        screen.putString(0, startY + 1 + row, line.left(width), false, false, inv);
    }

    /* Bottom border with footer hint */
    QString hint    = QStringLiteral("Tab/Enter to accept  Esc to dismiss");
    QString botLine = QStringLiteral("+- ") + hint + QLatin1Char(' ');
    while (QTuiText::visualWidth(botLine) < boxWidth - 1) {
        botLine += QLatin1Char('-');
    }
    botLine += QLatin1Char('+');
    screen.putString(0, startY + 1 + visibleItems, botLine.left(width), false, true);
}

void QTuiCompletionPopup::setVisible(bool vis)
{
    visible = vis;
    if (!vis) {
        viewStart = 0;
        highlight = 0;
    }
}

void QTuiCompletionPopup::setItems(const QStringList &newItems)
{
    items = newItems;
    if (highlight >= items.size()) {
        highlight = items.isEmpty() ? 0 : static_cast<int>(items.size()) - 1;
    }
    adjustViewport();
}

void QTuiCompletionPopup::setHighlight(int index)
{
    if (items.isEmpty()) {
        highlight = 0;
        return;
    }
    highlight = qBound(0, index, static_cast<int>(items.size()) - 1);
    adjustViewport();
}

void QTuiCompletionPopup::moveHighlight(int delta)
{
    if (items.isEmpty()) {
        return;
    }
    int total = static_cast<int>(items.size());
    highlight = (highlight + delta + total) % total;
    adjustViewport();
}

void QTuiCompletionPopup::adjustViewport()
{
    int total = static_cast<int>(items.size());
    if (total <= MAX_VISIBLE_ITEMS) {
        viewStart = 0;
        return;
    }
    if (highlight < viewStart) {
        viewStart = highlight;
    } else if (highlight >= viewStart + MAX_VISIBLE_ITEMS) {
        viewStart = highlight - MAX_VISIBLE_ITEMS + 1;
    }
    viewStart = qBound(0, viewStart, total - MAX_VISIBLE_ITEMS);
}
