// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuicompletionpopup.h"

/* Shared menu palette — matches QTuiMenu's exec() overlay colours. */
static constexpr QTuiBgColor MENU_BG_NORMAL    = 237;
static constexpr QTuiBgColor MENU_BG_HIGHLIGHT = 239;

int QTuiCompletionPopup::lineCount() const
{
    if (!visible || items.isEmpty()) {
        return 0;
    }
    int visibleItems = qMin(static_cast<int>(items.size()), MAX_VISIBLE_ITEMS);
    return visibleItems + 2; /* title + items + footer */
}

void QTuiCompletionPopup::render(QTuiScreen &screen, int startY, int width)
{
    if (!visible || items.isEmpty()) {
        return;
    }

    int visibleItems = qMin(static_cast<int>(items.size()), MAX_VISIBLE_ITEMS);

    /* Compute box width from longest visible item (plus its hint, if any) */
    int maxItemW = QTuiText::visualWidth(title) + 4;
    for (int idx = 0; idx < visibleItems; idx++) {
        int visIdx = viewStart + idx;
        if (visIdx >= items.size()) {
            break;
        }
        int itemW = QTuiText::visualWidth(items[visIdx]) + 4;
        if (visIdx < hints.size() && !hints[visIdx].isEmpty()) {
            itemW += QTuiText::visualWidth(hints[visIdx]) + 1;
        }
        maxItemW = qMax(maxItemW, itemW);
    }
    int boxWidth = qMin(width, qMax(maxItemW, 20));

    /* Title row */
    QString titleLine = QStringLiteral("  ") + title;
    while (QTuiText::visualWidth(titleLine) < boxWidth) {
        titleLine += QLatin1Char(' ');
    }
    screen.putString(
        0, startY, titleLine.left(width), true, false, false, QTuiFgColor::Yellow, MENU_BG_NORMAL);

    /* Items */
    for (int row = 0; row < visibleItems; row++) {
        int visIdx = viewStart + row;
        if (visIdx >= items.size()) {
            break;
        }
        QString line = QStringLiteral("  ") + items[visIdx];

        bool        isHL  = (visIdx == highlight) && colorEnabled;
        QTuiFgColor fgCol = isHL ? QTuiFgColor::Yellow : QTuiFgColor::Default;
        QTuiBgColor bgCol = isHL ? MENU_BG_HIGHLIGHT : MENU_BG_NORMAL;

        const bool hasHint = visIdx < hints.size() && !hints[visIdx].isEmpty();
        if (hasHint) {
            line += QLatin1Char(' ');
        }
        while (QTuiText::visualWidth(line) + (hasHint ? QTuiText::visualWidth(hints[visIdx]) : 0)
               < boxWidth) {
            line += QLatin1Char(' ');
        }
        screen.putString(0, startY + 1 + row, line.left(width), isHL, false, false, fgCol, bgCol);

        if (hasHint) {
            const int hintX = QTuiText::visualWidth(line);
            if (hintX < width) {
                screen.putString(
                    hintX,
                    startY + 1 + row,
                    hints[visIdx].left(width - hintX),
                    false,
                    false,
                    false,
                    QTuiFgColor::Gray,
                    bgCol);
            }
        }
    }

    /* Footer */
    QString hint = QStringLiteral("  Tab/Enter  Esc to dismiss");
    while (QTuiText::visualWidth(hint) < boxWidth) {
        hint += QLatin1Char(' ');
    }
    screen.putString(
        0,
        startY + 1 + visibleItems,
        hint.left(width),
        false,
        true,
        false,
        QTuiFgColor::Gray,
        MENU_BG_NORMAL);
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
    /* Hints are item-aligned; reset whenever the item list changes so a
     * stale hint cannot leak across two unrelated popup invocations. */
    hints.clear();
    if (highlight >= items.size()) {
        highlight = items.isEmpty() ? 0 : static_cast<int>(items.size()) - 1;
    }
    adjustViewport();
}

void QTuiCompletionPopup::setHints(const QStringList &newHints)
{
    hints = newHints;
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
